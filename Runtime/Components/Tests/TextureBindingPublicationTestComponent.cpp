#include "Components/Tests/TextureBindingPublicationTestComponent.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "ECS/CameraECS.h"
#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "FrameGraph/DepthPrepassNode.h"
#include "FrameGraph/RenderSceneNode.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "GraphicsDriver/Vulkan/VulkanBuffer.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "RHI/Buffer.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "RHI/Mesh.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include "RHI/VertexDescription.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	constexpr uint32_t AbsentSlot = TextureImporter::MaxTexturesInScene - 1u;
	using Samplers = TextureImporter::TextureSamplersSnapshot;

	// Expose existing preparation output, without replacing a production Prepare or cache operation.
	template<typename TNode>
	class TextureNodeProbe final : public TNode
	{
	public:
		auto Resources(const RHISceneViewSnapshot& scene)
		{
			return scene.m_submissionContext->template GetOrAddFrameGraphResources<typename TNode::SubmissionResources>(
				this, scene.m_cameraIndex, 0u);
		}
		TextureBindingCache& Cache() { return this->m_textureBindingCache; }
	};
	using MainNode = TextureNodeProbe<RenderSceneNode>;
	using DepthNode = TextureNodeProbe<DepthPrepassNode>;
	using ShadowNode = TextureNodeProbe<ShadowPrepassNode>;

	struct PreparationCase
	{
		TRefPtr<MainNode> m_main;
		TRefPtr<DepthNode> m_depth;
		TRefPtr<ShadowNode> m_shadow;
		RHISpatialSceneVersionPtr m_scene;
		RHISceneViewSnapshot m_snapshot;
		std::array<TextureBindingCacheEntry, 3> m_warm;
	};

	class TexturePublicationWorld final : public World
	{
	public:
		TexturePublicationWorld() : World("Texture publication retry fixture", 0, CreateEcs()) { BeginPlayEcs(); }
		~TexturePublicationWorld() override { Clear(); }
	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<LightingECS>::Make());
			return systems;
		}
	};

	struct RestoreTextureView
	{
		RHITexturePtr m_texture;
		VulkanImageViewPtr m_view;
		~RestoreTextureView() { m_texture->m_vulkan.m_imageView = m_view; }
	};

	void Prepare(BaseFrameGraphNode& node, const RHISceneViewSnapshot& snapshot)
	{
		// The coordinator is Render; these real leaves run on Worker or RHI, never Render.
		auto task = node.Prepare(RHIFrameGraphPtr::Make(), snapshot);
		if (task) { task->Run(); task->Wait(); }
	}

	TextureBindingCacheEntry* FindEntry(TextureBindingCache& cache, const TSet<uint32_t>& requested)
	{
		TextureBindingCacheEntry* entry = nullptr;
		cache.Find(TextureBindingCacheKey(requested), entry);
		return entry;
	}

	bool SamePublication(const TextureBindingCacheEntry& a, const TextureBindingCacheEntry& b)
	{
		return a.m_textureBindings == b.m_textureBindings && a.m_textureRemapBuffer == b.m_textureRemapBuffer &&
			a.m_textureSetSize == b.m_textureSetSize && a.m_sourceDescriptorRevision == b.m_sourceDescriptorRevision &&
			a.m_sourceSlotRevisions == b.m_sourceSlotRevisions;
	}

	bool CompleteEntry(const TextureBindingCacheEntry& entry, const Samplers& source, std::string* diagnostic = nullptr)
	{
		const auto fail = [&](std::string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (!entry.m_textureBindings || !entry.m_textureRemapBuffer)
			return fail(std::format("missing owner: bindings={}, remap={}", bool(entry.m_textureBindings), bool(entry.m_textureRemapBuffer)));
		if (source.m_slots.Num() != 3u || entry.m_textureSetSize != 3u || entry.m_sourceSlotRevisions.Num() != 3u)
			return fail(std::format("counts: source slots={}, dense textures={}, cached slot revisions={}",
				source.m_slots.Num(), entry.m_textureSetSize, entry.m_sourceSlotRevisions.Num()));
		if (entry.m_sourceDescriptorRevision != source.m_descriptorRevision)
			return fail(std::format("source descriptor revision: cached={}, snapshot={}", entry.m_sourceDescriptorRevision, source.m_descriptorRevision));
		RHIShaderBindingPtr remapBinding, samplerBinding;
		if (!entry.m_textureBindings->GetShaderBindings().TryGet("textureSamplerRemap", remapBinding) || !remapBinding)
			return fail("missing textureSamplerRemap CPU binding");
		if (!entry.m_textureBindings->GetShaderBindings().TryGet("textureSamplers", samplerBinding) || !samplerBinding)
			return fail("missing textureSamplers CPU binding");
		if (samplerBinding->GetTextureBindings().Num() != 3u || samplerBinding->GetLayout().m_arrayCount != 3u)
			return fail(std::format("sampler counts: textures={}, layout={}", samplerBinding->GetTextureBindings().Num(), samplerBinding->GetLayout().m_arrayCount));
		auto native = entry.m_textureBindings->m_vulkan.m_descriptorSet;
		auto remapBuffer = entry.m_textureRemapBuffer;
		auto* remap = static_cast<const uint32_t*>(remapBuffer->GetPointer());
		if (!native || !native->IsCompiled() || !remap) return fail("native set not compiled or remap buffer not mapped");
		const auto range = *remapBuffer->m_vulkan.m_buffer;
		bool nativeRemap = false;
		for (const auto& descriptor : native->m_descriptors)
		{
			if (descriptor->GetBinding() != 0u) continue;
			VkWriteDescriptorSet write{};
			descriptor->Apply(write);
			VkDeviceSize effectiveRange = write.pBufferInfo ? write.pBufferInfo->range : 0u;
			if (write.pBufferInfo && effectiveRange == VK_WHOLE_SIZE && write.pBufferInfo->offset <= range.m_buffer->m_size)
				effectiveRange = range.m_buffer->m_size - write.pBufferInfo->offset;
			nativeRemap = write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && write.descriptorCount == 1u &&
				write.pBufferInfo && write.pBufferInfo->buffer == static_cast<VkBuffer>(*range.m_buffer) &&
				write.pBufferInfo->offset == range.m_offset && effectiveRange == range.m_size;
			if (!nativeRemap)
				return fail(std::format("native remap: type={}, count={}, has buffer info={}, same buffer={}, offset={}/{}, effective range={}/{}, encoded range={}",
					uint32_t(write.descriptorType), write.descriptorCount, write.pBufferInfo != nullptr,
					write.pBufferInfo && write.pBufferInfo->buffer == static_cast<VkBuffer>(*range.m_buffer),
					write.pBufferInfo ? write.pBufferInfo->offset : 0u, range.m_offset,
					effectiveRange, range.m_size, write.pBufferInfo ? write.pBufferInfo->range : 0u));
		}
		if (!nativeRemap) return fail("native set has no binding 0 remap descriptor");
		for (uint32_t i = 0u; i < 3u; ++i)
		{
			auto expected = i == 0u || !source.m_slots[i].m_texture ?
				Renderer::GetDriver()->GetDefaultTexture() : source.m_slots[i].m_texture;
			if (entry.m_sourceSlotRevisions[i] != source.m_slots[i].m_contentRevision ||
				remap[source.m_slots[i].m_index] != i || samplerBinding->GetTextureBinding(i) != expected ||
				!native->ReferencesImageView(1u, i, expected->m_vulkan.m_imageView))
				return fail(std::format("dense slot {} (global {}): revision={}/{}, remap={}/{}, same texture={}, native view={}",
					i, source.m_slots[i].m_index, entry.m_sourceSlotRevisions[i], source.m_slots[i].m_contentRevision,
					remap[source.m_slots[i].m_index], i, samplerBinding->GetTextureBinding(i) == expected,
					native->ReferencesImageView(1u, i, expected->m_vulkan.m_imageView)));
		}
		for (uint32_t i = 0u; i < TextureImporter::MaxTexturesInScene; ++i)
		{
			const uint32_t expected = i == source.m_slots[1].m_index ? 1u : i == AbsentSlot ? 2u : 0u;
			if (remap[i] != expected) return fail(std::format("remap[{}]={}, expected={}", i, remap[i], expected));
		}
		return true;
	}

	template<typename TInstance>
	bool PacketUses(const TPackedDrawPacket<TInstance>& packet, RHIShaderBindingSetPtr bindings, uint32_t expectedInstances)
	{
		if (packet.GetNumInstances() != expectedInstances || packet.GetGroups().IsEmpty()) return false;
		for (const auto& group : packet.GetGroups())
		{
			if (group.m_batch.m_textureBindings != bindings) return false;
#if defined(__APPLE__)
			if (group.m_batch.m_supportedMeshesPerBatch != 1024u / 3u) return false;
#endif
		}
		return true;
	}

	bool ShadowPacketsUse(ShadowNode& node, const RHISceneViewSnapshot& snapshot,
		RHIShaderBindingSetPtr bindings, uint32_t instancesPerCaster)
	{
		auto resources = node.Resources(snapshot);
		if (resources->m_activeShadowViews.Num() != snapshot.m_shadowMapsToUpdate.Num()) return false;
		uint32_t total = 0u;
		for (uint32_t i = 0u; i < resources->m_activeShadowViews.Num(); ++i)
		{
			const uint32_t expected = static_cast<uint32_t>(snapshot.m_shadowMapsToUpdate[i].m_meshList.Num()) * instancesPerCaster;
			const auto& packet = resources->m_activeShadowViews[i]->m_packet;
			if (expected ? !PacketUses(packet, bindings, expected) : packet.GetNumInstances() != 0u) return false;
			total += packet.GetNumInstances();
		}
		return total > 0u;
	}

	CameraData CreateCamera()
	{
		CameraData camera;
		camera.SetAspect(16.0f / 9.0f);
		camera.SetFov(60.0f);
		camera.SetZNear(0.1f);
		camera.SetZFar(80.0f);
		return camera;
	}

	RHISpatialSceneVersionPtr CreateScene(RHIMeshPtr mesh, RHIMaterialPtr material,
		const TSet<uint32_t>& requested, uint32_t textureIndex, bool ordinary, bool instanced)
	{
		RHISceneViewProxy proxy;
		proxy.m_staticMeshEcs = 1u;
		proxy.m_mobility = EMobilityType::Static;
		proxy.m_worldMatrix = glm::mat4(1.0f);
		proxy.m_worldAabb = Math::AABB(glm::vec3(0.0f), glm::vec3(3.0f));
		proxy.m_bCastShadows = true;
		proxy.m_shadowCaster = RHIShadowCasterProxyPtr::Make();
		proxy.m_shadowCaster->m_staticMeshEcs = 1u;
		proxy.m_shadowCaster->m_worldAabb = proxy.m_worldAabb;
		const size_t tag = material->GetRenderState().GetTag();
		if (ordinary)
		{
			proxy.m_meshes.Add(mesh);
			proxy.m_meshModelMatrices.Add(glm::mat4(1.0f));
			proxy.m_overrideMaterials.Add(material);
			proxy.m_renderQueueTags.Add(tag);
			proxy.m_baseColorFactors.Add(glm::vec4(1.0f));
			proxy.m_baseColorSamplers.Add(textureIndex);
			proxy.m_alphaCutoffs.Add(0.5f);
			RHIShadowMeshProxy shadow;
			shadow.m_mesh = mesh;
			shadow.m_renderQueueTag = tag;
			shadow.m_baseColorSampler = textureIndex;
#if defined(__APPLE__)
			proxy.m_materialTextureSamplers.Add(requested);
			shadow.m_materialTextureSamplers = requested;
#endif
			proxy.m_shadowCaster->m_meshes.Add(std::move(shadow));
		}
		if (instanced)
		{
			RHIInstancedMeshGroup group;
			group.m_instanceTransforms = { glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(1, 0, 0)) };
			group.m_meshes.Add(mesh);
			group.m_meshTransforms.Add(glm::mat4(1.0f));
			group.m_materials.Add(material);
			group.m_renderQueueTags.Add(tag);
			group.m_baseColorFactors.Add(glm::vec4(1.0f));
			group.m_baseColorSamplers.Add(textureIndex);
			group.m_alphaCutoffs.Add(0.5f);
			group.m_bCastShadows = true;
#if defined(__APPLE__)
			group.m_materialTextureSamplers.Add(requested);
#endif
			proxy.m_instancedGroups.Add(std::move(group));
		}
		auto topology = RHISceneProxyResourcePtr::Make(std::move(proxy));
		RHISceneInstanceRecord record;
		record.m_producerKey = 1u;
		record.m_mobility = EMobilityType::Static;
		record.m_worldBounds = topology->m_proxy.m_worldAabb;
		record.m_topology = topology;
		record.m_topologyRevision = topology->m_mainRevision;
		record.m_shadowRevision = topology->m_shadowRevision;
		record.m_renderFlags = 1u;
		auto result = RHISpatialSceneVersionPtr::Make();
		result->m_scene = RHIScenePtr::Make();
		const auto handle = result->m_scene->AddInstance(record);
		result->m_sceneVersion = result->m_scene->PublishVersion();
		result->m_staticOctree = TSharedPtr<RHISceneSpatialIndex>::Make(glm::ivec3(0), 128u, 2u);
		result->m_staticOctree->Update(glm::ivec3(0), glm::ivec3(3), handle);
		return result;
	}

	void CreateSnapshot(RHISceneViewSnapshot& snapshot, RHISpatialSceneVersionPtr scene)
	{
		snapshot.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		snapshot.m_submissionContext->BeginSubmission(79u, 0u);
		snapshot.m_camera = TUniquePtr<CameraData>::Make(CreateCamera());
		snapshot.m_cameraTransform = Math::Transform(glm::vec4(0, 2, 12, 1));
		snapshot.m_sceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make();
		snapshot.m_sceneVersions->Add(scene->m_sceneVersion);
		RHIUpdateShadowMapCommand shadow;
		shadow.m_shadowType = EShadowType::PCF;
		shadow.m_lightMatrix = glm::mat4(1.0f);
		shadow.m_payloadCompletionToken = RHISubmissionCompletionTokenPtr::Make();
		snapshot.ForEachSceneProxy(EMobilityType::Static, [&](const RHIVisibleSceneProxy& proxy)
		{
			snapshot.m_proxies.Add(proxy);
			RHIVisibleShadowCaster caster;
			caster.m_handle = proxy.m_handle;
			caster.m_record = proxy.m_record;
			caster.m_resource = proxy.m_resource;
			shadow.m_meshList.Add(caster);
		});
		snapshot.m_shadowMapsToUpdate.Add(std::move(shadow));
	}
}

struct Sailor::TextureBindingPublicationState
{
	enum class Phase { Loading, Warm, ReloadUnrelated, CheckUnrelated, ReloadRequested, Validate };
	Phase m_phase = Phase::Loading;
	std::filesystem::path m_ownedDirectory;
	std::array<AssetInfoPtr, 2> m_infos{};
	std::array<TexturePtr, 2> m_textures{};
	std::array<Tasks::TaskPtr<TexturePtr>, 2> m_loads{};
	std::array<uint32_t, 2> m_indices{};
	ShaderSetPtr m_shader;
	Samplers m_sourceA, m_sourceB;
	TextureImporter::TextureSamplerSlotSnapshot m_unrelatedA;
	TSet<uint32_t> m_requested;
	TVector<uint32_t> m_requestedIndices;
	std::array<PreparationCase, 4> m_cases;
	std::array<PreparationCase, 2> m_customCases;
};

namespace
{
	std::string WarmNodes(TextureBindingPublicationState& state)
	{
		auto& driver = Renderer::GetDriver();
		struct Vertex { glm::vec3 position; glm::vec2 uv; glm::vec4 color; };
		const Vertex vertices[] = { { {-1, -1, 0}, {0, 0}, glm::vec4(1) },
			{ {1, -1, 0}, {1, 0}, glm::vec4(1) }, { {0, 1, 0}, {0.5f, 1}, glm::vec4(1) } };
		const uint32_t indices[] = { 0u, 1u, 2u };
		auto mesh = RHIMeshPtr::Make();
		mesh->m_vertexDescription = RHIVertexDescriptionPtr::Make();
		mesh->m_vertexDescription->SetVertexStride(sizeof(Vertex));
		mesh->m_vertexDescription->AddAttribute(0u, 0u, EFormat::R32G32B32_SFLOAT, offsetof(Vertex, position));
		mesh->m_vertexDescription->AddAttribute(2u, 0u, EFormat::R32G32_SFLOAT, offsetof(Vertex, uv));
		mesh->m_vertexDescription->AddAttribute(3u, 0u, EFormat::R32G32B32A32_SFLOAT, offsetof(Vertex, color));
		const auto memory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
		mesh->m_vertexBuffer = driver->CreateBuffer(sizeof(vertices), EBufferUsageBit::VertexBuffer_Bit, memory);
		mesh->m_indexBuffer = driver->CreateBuffer(sizeof(indices), EBufferUsageBit::IndexBuffer_Bit, memory);
		if (!mesh->m_vertexBuffer || !mesh->m_indexBuffer || !mesh->m_vertexBuffer->GetPointer() || !mesh->m_indexBuffer->GetPointer())
			return "could not allocate the private host-visible mesh";
		std::memcpy(mesh->m_vertexBuffer->GetPointer(), vertices, sizeof(vertices));
		std::memcpy(mesh->m_indexBuffer->GetPointer(), indices, sizeof(indices));
		mesh->m_bounds = Math::AABB(glm::vec3(0), glm::vec3(1));
		const RenderState masked(true, true, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, "Masked"_h.GetHash());
		const RenderState custom(true, true, 0, true, ECullMode::None, EBlendMode::None, EFillMode::Fill, "Opaque"_h.GetHash());
		auto material = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, masked, state.m_shader);
		auto customMaterial = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, custom, state.m_shader);
		if (!material || !customMaterial || !material->GetVersion()->GetBindings() ||
			material->GetVersion()->GetBindings()->GetShaderBindings().Num() == 0u)
			return "fixture material must have real compiled graphics shaders and nonempty set 3 bindings";
		for (uint32_t i = 0u; i < state.m_cases.size(); ++i)
		{
			auto& test = state.m_cases[i];
			test.m_scene = CreateScene(mesh, material, state.m_requested, state.m_indices[0], i % 2u == 0u, i % 2u != 0u);
			CreateSnapshot(test.m_snapshot, test.m_scene);
			test.m_main = TRefPtr<MainNode>::Make();
			test.m_depth = TRefPtr<DepthNode>::Make();
			test.m_shadow = TRefPtr<ShadowNode>::Make();
			test.m_main->SetString("Tag", "Masked");
			test.m_depth->SetString("Tag", "Masked");
			if (i >= 2u)
			{
				test.m_main->SetString("VirtualizeInstancePayloads", "false");
				test.m_depth->SetString("VirtualizeInstancePayloads", "false");
				test.m_shadow->SetString("VirtualizeInstancePayloads", "false");
			}
			Prepare(*test.m_main, test.m_snapshot);
			Prepare(*test.m_depth, test.m_snapshot);
			Prepare(*test.m_shadow, test.m_snapshot);
			const uint32_t count = i % 2u ? 2u : 1u;
#if defined(__APPLE__)
			TextureBindingCache* caches[] = { &test.m_main->Cache(), &test.m_depth->Cache(), &test.m_shadow->Cache() };
			for (uint32_t n = 0u; n < 3u; ++n)
			{
				auto* entry = FindEntry(*caches[n], state.m_requested);
				if (!entry) return std::format("warm case {} node {} has no requested cache key: entries={}, visible proxies={}, main instances={}, depth instances={}",
					i, n, caches[n]->Num(), test.m_snapshot.m_proxies.Num(), test.m_main->Resources(test.m_snapshot)->m_packet.GetNumInstances(),
					test.m_depth->Resources(test.m_snapshot)->m_packet.GetNumInstances());
				std::string diagnostic;
				if (!CompleteEntry(*entry, state.m_sourceA, &diagnostic)) return std::format("warm case {} node {}: {}", i, n, diagnostic);
				test.m_warm[n] = *entry;
			}
			auto mainSet = test.m_warm[0].m_textureBindings;
			auto depthSet = test.m_warm[1].m_textureBindings;
			auto shadowSet = test.m_warm[2].m_textureBindings;
#else
			auto mainSet = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
			auto depthSet = mainSet;
			auto shadowSet = mainSet;
#endif
			if (!PacketUses(test.m_main->Resources(test.m_snapshot)->m_packet, mainSet, count) ||
				!PacketUses(test.m_depth->Resources(test.m_snapshot)->m_packet, depthSet, count) ||
				!ShadowPacketsUse(*test.m_shadow, test.m_snapshot, shadowSet, count) ||
				!test.m_snapshot.m_shadowMapsToUpdate[0].m_payloadCompletionToken->IsSuccessful())
				return std::format("warm case {} did not prepare actual main/depth/shadow instances", i);
		}
		for (uint32_t i = 0u; i < state.m_customCases.size(); ++i)
		{
			auto& test = state.m_customCases[i];
			test.m_scene = CreateScene(mesh, customMaterial, state.m_requested, state.m_indices[0], true, true);
			CreateSnapshot(test.m_snapshot, test.m_scene);
			test.m_depth = TRefPtr<DepthNode>::Make();
			test.m_depth->SetString("Tag", "Opaque");
			if (i) test.m_depth->SetString("VirtualizeInstancePayloads", "false");
			Prepare(*test.m_depth, test.m_snapshot);
#if defined(__APPLE__)
			auto* entry = FindEntry(test.m_depth->Cache(), state.m_requested);
			if (!entry) return "custom-depth warm cache has no requested key";
			std::string diagnostic;
			if (!CompleteEntry(*entry, state.m_sourceA, &diagnostic)) return "custom-depth warm: " + diagnostic;
			test.m_warm[1] = *entry;
			auto bindings = entry->m_textureBindings;
#else
			auto bindings = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
#endif
			if (!PacketUses(test.m_depth->Resources(test.m_snapshot)->m_customPacket, bindings, 3u))
				return "required custom depth did not prepare its ordinary and instanced stream";
		}
		return {};
	}

	std::string CheckUnrelatedReload(TextureBindingPublicationState& state)
	{
		const auto source = App::GetSubmodule<TextureImporter>()->GetTextureSamplersSnapshot(state.m_requestedIndices);
		if (source.m_descriptorRevision == state.m_sourceA.m_descriptorRevision) return "unrelated reload did not advance the real global descriptor revision";
		for (uint32_t i = 0u; i < 3u; ++i)
			if (source.m_slots[i].m_contentRevision != state.m_sourceA.m_slots[i].m_contentRevision ||
				source.m_slots[i].m_texture != state.m_sourceA.m_slots[i].m_texture) return "unrelated reload changed a requested slot";
		for (auto& test : state.m_cases)
		{
			Prepare(*test.m_main, test.m_snapshot);
			Prepare(*test.m_depth, test.m_snapshot);
			test.m_snapshot.m_shadowMapsToUpdate[0].m_payloadCompletionToken->Reset();
			Prepare(*test.m_shadow, test.m_snapshot);
			TextureBindingCache* caches[] = { &test.m_main->Cache(), &test.m_depth->Cache(), &test.m_shadow->Cache() };
			for (uint32_t n = 0u; n < 3u; ++n)
			{
				auto* entry = FindEntry(*caches[n], state.m_requested);
				auto expected = test.m_warm[n];
				expected.m_sourceDescriptorRevision = source.m_descriptorRevision;
				if (!entry || !SamePublication(*entry, expected) || !CompleteEntry(*entry, source)) return "unrelated reload rebuilt or corrupted warm A";
				test.m_warm[n] = *entry;
			}
			if (!test.m_snapshot.m_shadowMapsToUpdate[0].m_payloadCompletionToken->IsSuccessful()) return "unrelated reload/default substitution marked current shadows incomplete";
		}
		for (auto& test : state.m_customCases)
		{
			Prepare(*test.m_depth, test.m_snapshot);
			auto expected = test.m_warm[1];
			expected.m_sourceDescriptorRevision = source.m_descriptorRevision;
			auto* entry = FindEntry(test.m_depth->Cache(), state.m_requested);
			if (!entry || !SamePublication(*entry, expected)) return "unrelated reload replaced custom-depth A";
			test.m_warm[1] = *entry;
		}
		state.m_sourceA = source;
		return {};
	}

	std::string CheckShadowRetry(TextureBindingPublicationState& state, PreparationCase& test,
		uint32_t count, VulkanImageViewPtr unavailable)
	{
		TexturePublicationWorld world;
		auto owner = world.Instantiate("Private directional light");
		if (!owner) return "could not instantiate shadow retry owner";
		owner->GetTransformComponent().SetRotation(glm::quat(glm::radians(glm::vec3(-35, 20, 0))));
		auto* transforms = world.GetECS<TransformECS>();
		transforms->Tick(0.0f);
		transforms->PostTick();
		auto* lighting = world.GetECS<LightingECS>();
		auto& light = lighting->GetComponentData(lighting->RegisterComponent());
		light.SetOwner(owner);
		light.m_type = ELightType::Directional;
		light.m_shadowType = EShadowType::PCF;
		light.MarkDirty();
		lighting->Tick(0.0f);
		auto scene = RHISceneViewPtr::Make();
		scene->m_world = &world;
		scene->m_cameras.Add(CreateCamera());
		scene->m_cameraTransforms.Add(test.m_snapshot.m_cameraTransform);
		scene->SetSubmissionContext(test.m_snapshot.m_submissionContext);
		auto submission = scene->GetOrCreateSubmissionCompletionToken();
		scene->AddSceneVersion(test.m_scene);
		if (scene->m_bHasCustomDepthShadowCasters) return "shadow retry fixture must not force custom-depth refresh";
		auto texture = state.m_sourceB.m_slots[1].m_texture;
		RestoreTextureView restore{ texture, texture->m_vulkan.m_imageView };
		texture->m_vulkan.m_imageView = unavailable;
		const auto fill = [&]()
		{
			scene->m_rhiLightsDataPerCamera.Clear(false);
			lighting->FillLightingData(scene);
			scene->PrepareSnapshots();
		};
		// This World's first Fill is B. Only the node, not Lighting's CSM result, has warm A.
		fill();
		auto& snapshot = scene->m_snapshots[0];
		if (snapshot.m_shadowMapsToUpdate.IsEmpty()) return "first B Fill emitted no real CSM update";
		Prepare(*test.m_shadow, snapshot);
		auto* failed = FindEntry(test.m_shadow->Cache(), state.m_requested);
		if (!failed || !SamePublication(*failed, test.m_warm[2]) ||
			!ShadowPacketsUse(*test.m_shadow, snapshot, test.m_warm[2].m_textureBindings, count)) return "shadow B did not retain complete fallback A with real casters/instances";
		uint32_t failedPasses = 0u;
		for (const auto& pass : snapshot.m_shadowMapsToUpdate)
		{
			if (pass.m_meshList.IsEmpty()) continue;
			if (!pass.m_payloadCompletionToken || pass.m_payloadCompletionToken->IsPending() || pass.m_payloadCompletionToken->IsSuccessful())
				return "fallback A falsely acknowledged current shadow B";
			++failedPasses;
		}
		if (!failedPasses) return "shadow B had no actual failed caster payload";
		texture->m_vulkan.m_imageView = restore.m_view;
		fill(); // Check B above: Lighting may now reset and reuse its payload token.
		if (snapshot.m_shadowMapsToUpdate.Num() != failedPasses || scene->GetOrCreateSubmissionCompletionToken() != submission ||
			!submission->IsPending()) return "identical C Fill did not retry precisely the failed payloads on the same pending submission";
		Prepare(*test.m_shadow, snapshot);
		auto* current = FindEntry(test.m_shadow->Cache(), state.m_requested);
		if (!current || current->m_textureBindings == test.m_warm[2].m_textureBindings || !CompleteEntry(*current, state.m_sourceB) ||
			!ShadowPacketsUse(*test.m_shadow, snapshot, current->m_textureBindings, count)) return "same-B shadow retry did not prepare full C with actual instances";
		for (const auto& pass : snapshot.m_shadowMapsToUpdate)
			if (!pass.m_payloadCompletionToken || !pass.m_payloadCompletionToken->IsSuccessful()) return "C payload did not complete successfully";
		const auto stable = *current;
		fill();
		current = FindEntry(test.m_shadow->Cache(), state.m_requested);
		if (!snapshot.m_shadowMapsToUpdate.IsEmpty() || !current || !SamePublication(*current, stable) ||
			scene->GetOrCreateSubmissionCompletionToken() != submission) return "unchanged D failed to reuse successful C";
		return {};
	}

	std::string ValidateReload(TextureBindingPublicationState& state)
	{
		auto texture = state.m_sourceB.m_slots[1].m_texture;
		RestoreTextureView restore{ texture, texture->m_vulkan.m_imageView };
		auto unavailable = VulkanImageViewPtr::Make(texture->m_vulkan.m_image->GetDevice(), texture->m_vulkan.m_image);
		if (static_cast<VkImageView>(*unavailable) != VK_NULL_HANDLE) return "failure fixture view unexpectedly compiled";
		texture->m_vulkan.m_imageView = unavailable;
		// Safe old-78 gate: do not Process/submit a remap-only descriptor set.
		auto cold = TRefPtr<MainNode>::Make();
		cold->SetString("Tag", "Masked");
		Prepare(*cold, state.m_cases[0].m_snapshot);
		const bool emptyCache = cold->Cache().IsEmpty();
		const uint32_t coldInstances = cold->Resources(state.m_cases[0].m_snapshot)->m_packet.GetNumInstances();
		if (!emptyCache || coldInstances != 0u)
			return std::format("cold failed request published partial texture state: empty cache={}, draw instances={}", emptyCache, coldInstances);
		auto coldDepth = TRefPtr<DepthNode>::Make();
		coldDepth->SetString("Tag", "Masked");
		Prepare(*coldDepth, state.m_cases[0].m_snapshot);
		if (!coldDepth->Cache().IsEmpty() || coldDepth->Resources(state.m_cases[0].m_snapshot)->m_packet.GetNumInstances() != 0u)
			return "cold depth request retained partial bindings or draw instances";
		auto coldShadow = TRefPtr<ShadowNode>::Make();
		auto& coldSnapshot = state.m_cases[0].m_snapshot;
		auto coldToken = coldSnapshot.m_shadowMapsToUpdate[0].m_payloadCompletionToken;
		coldToken->Reset();
		Prepare(*coldShadow, coldSnapshot);
		auto coldShadowResources = coldShadow->Resources(coldSnapshot);
		if (!coldShadow->Cache().IsEmpty() || coldShadowResources->m_activeShadowViews.Num() != 1u ||
			coldShadowResources->m_activeShadowViews[0]->m_packet.GetNumInstances() != 0u ||
			coldToken->IsPending() || coldToken->IsSuccessful()) return "cold shadow request retained partial state or acknowledged its payload";
		for (uint32_t i = 0u; i < state.m_cases.size(); ++i)
		{
			auto& test = state.m_cases[i];
			const uint32_t count = i % 2u ? 2u : 1u;
			texture->m_vulkan.m_imageView = unavailable;
			Prepare(*test.m_main, test.m_snapshot);
			Prepare(*test.m_depth, test.m_snapshot);
			TextureBindingCache* caches[] = { &test.m_main->Cache(), &test.m_depth->Cache() };
			for (uint32_t n = 0u; n < 2u; ++n)
			{
				auto* entry = FindEntry(*caches[n], state.m_requested);
				if (!entry || !SamePublication(*entry, test.m_warm[n]) || !CompleteEntry(*entry, state.m_sourceA)) return "failed warm main/depth request changed A content or source keys";
			}
			if (!PacketUses(test.m_main->Resources(test.m_snapshot)->m_packet, test.m_warm[0].m_textureBindings, count) ||
				!PacketUses(test.m_depth->Resources(test.m_snapshot)->m_packet, test.m_warm[1].m_textureBindings, count)) return "main/depth fallback A lost actual instances or batch budget";
			texture->m_vulkan.m_imageView = restore.m_view;
			Prepare(*test.m_main, test.m_snapshot);
			Prepare(*test.m_depth, test.m_snapshot);
			std::array<TextureBindingCacheEntry, 2> current;
			for (uint32_t n = 0u; n < 2u; ++n)
			{
				auto* entry = FindEntry(*caches[n], state.m_requested);
				if (!entry || entry->m_textureBindings == test.m_warm[n].m_textureBindings || !CompleteEntry(*entry, state.m_sourceB)) return "same-B main/depth retry did not publish complete C";
				current[n] = *entry;
			}
			if (!PacketUses(test.m_main->Resources(test.m_snapshot)->m_packet, current[0].m_textureBindings, count) ||
				!PacketUses(test.m_depth->Resources(test.m_snapshot)->m_packet, current[1].m_textureBindings, count)) return "main/depth C omitted instances";
			Prepare(*test.m_main, test.m_snapshot);
			Prepare(*test.m_depth, test.m_snapshot);
			for (uint32_t n = 0u; n < 2u; ++n)
			{
				auto* entry = FindEntry(*caches[n], state.m_requested);
				if (!entry || !SamePublication(*entry, current[n])) return "unchanged main/depth C was republished";
			}
			if (auto error = CheckShadowRetry(state, test, count, unavailable); !error.empty()) return std::format("shadow case {}: {}", i, error);
		}
		for (auto& test : state.m_customCases)
		{
			texture->m_vulkan.m_imageView = unavailable;
			Prepare(*test.m_depth, test.m_snapshot);
			auto* entry = FindEntry(test.m_depth->Cache(), state.m_requested);
			if (!entry || !SamePublication(*entry, test.m_warm[1]) ||
				!PacketUses(test.m_depth->Resources(test.m_snapshot)->m_customPacket, entry->m_textureBindings, 3u)) return "custom-depth B lost fallback A or its ordinary/instanced packet";
			texture->m_vulkan.m_imageView = restore.m_view;
			Prepare(*test.m_depth, test.m_snapshot);
			entry = FindEntry(test.m_depth->Cache(), state.m_requested);
			if (!entry || entry->m_textureBindings == test.m_warm[1].m_textureBindings || !CompleteEntry(*entry, state.m_sourceB) ||
				!PacketUses(test.m_depth->Resources(test.m_snapshot)->m_customPacket, entry->m_textureBindings, 3u)) return "custom-depth same-B retry did not publish C";
			const auto stable = *entry;
			Prepare(*test.m_depth, test.m_snapshot);
			entry = FindEntry(test.m_depth->Cache(), state.m_requested);
			if (!entry || !SamePublication(*entry, stable)) return "custom-depth C was republished";
		}
		const auto unchanged = App::GetSubmodule<TextureImporter>()->GetTextureSamplersSnapshot(state.m_requestedIndices);
		if (unchanged.m_descriptorRevision != state.m_sourceB.m_descriptorRevision ||
			unchanged.m_slots[1].m_texture != texture || unchanged.m_slots[1].m_contentRevision != state.m_sourceB.m_slots[1].m_contentRevision)
			return "retry changed the real requested texture identity/revision";
		return {};
	}

	std::string CreateAssets(TextureBindingPublicationState& state, const std::string& runId)
	{
		auto* registry = App::GetSubmodule<AssetRegistry>();
		std::filesystem::path requestedPath;
		const std::string leaf = "Tests/TextureBindingPublication-" + runId;
		if (!registry->ResolveWorkspaceContentPathForWrite(leaf + "/requested.tga", requestedPath)) return "no writable content mount for private texture fixture";
		std::error_code error;
		if (!std::filesystem::create_directory(requestedPath.parent_path(), error) || error) return "private content leaf was not created exclusively";
		state.m_ownedDirectory = requestedPath.parent_path();
		for (uint32_t i = 0u; i < 2u; ++i)
		{
			const auto filename = state.m_ownedDirectory / (i ? "unrelated.tga" : "requested.tga");
			// Uncompressed top-left 2x2 BGRA; normal registry import creates its own sidecar and FileId.
			std::array<uint8_t, 34> tga{};
			tga[2] = 2u; tga[12] = 2u; tga[14] = 2u; tga[16] = 32u; tga[17] = 0x28u;
			for (uint32_t p = 0u; p < 4u; ++p)
			{
				tga[18u + p * 4u] = static_cast<uint8_t>(30u + p * 20u + i);
				tga[19u + p * 4u] = 110u; tga[20u + p * 4u] = 210u; tga[21u + p * 4u] = 255u;
			}
			std::ofstream output(filename, std::ios::binary);
			output.write(reinterpret_cast<const char*>(tga.data()), static_cast<std::streamsize>(tga.size()));
			output.close();
			if (!output) return "private TGA write failed";
			const FileId id = registry->GetOrLoadFile(filename.string());
			state.m_infos[i] = registry->GetAssetInfoPtr(id);
			if (!state.m_infos[i]) return "normal registry import did not create private texture metadata";
		}
		for (uint32_t i = 0u; i < 2u; ++i)
			state.m_loads[i] = App::GetSubmodule<TextureImporter>()->LoadTexture(state.m_infos[i]->GetFileId(), state.m_textures[i]);
		auto shaderInfo = registry->GetAssetInfoPtr("Tests/Shaders/TextureBindingPublication.shader");
		if (!shaderInfo) return "fixture shader metadata is missing";
		App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), state.m_shader);
		return {};
	}
}

TextureBindingPublicationTestComponent::~TextureBindingPublicationTestComponent() = default;

void TextureBindingPublicationTestComponent::Tick(float)
{
	if (IsFinished()) return;
	if (!m_state)
	{
		m_state = TSharedPtr<TextureBindingPublicationState>::Make();
		if (auto error = CreateAssets(*m_state, GetTestRunId()); !error.empty()) { MarkFailed(error); return; }
	}
	if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 90000)
	{
		// Tasks retain state. Do not remove sources while decode/reload or Prepare may still be using them.
		MarkFailed("texture publication timed out; private sources retained at " + m_state->m_ownedDirectory.string());
		return;
	}
	auto* importer = App::GetSubmodule<TextureImporter>();
	auto& state = *m_state;
	using Phase = TextureBindingPublicationState::Phase;
	if (state.m_phase == Phase::Loading)
	{
		if (!state.m_shader || !state.m_shader->IsReady()) return;
		for (uint32_t i = 0u; i < 2u; ++i)
		{
			if (!state.m_loads[i] || !state.m_loads[i]->IsFinished()) return;
			if (!state.m_textures[i] || !state.m_textures[i]->IsReady()) { MarkFailed("private texture load failed"); return; }
			state.m_indices[i] = static_cast<uint32_t>(importer->GetTextureIndex(state.m_infos[i]->GetFileId()));
		}
		if (!state.m_indices[0] || !state.m_indices[1] || state.m_indices[0] == state.m_indices[1] ||
			state.m_indices[0] >= AbsentSlot || state.m_indices[1] >= AbsentSlot) { MarkFailed("private textures need distinct valid sampler slots"); return; }
		state.m_requested = { 0u, state.m_indices[0], AbsentSlot };
		state.m_requestedIndices = { 0u, state.m_indices[0], AbsentSlot };
		state.m_sourceA = importer->GetTextureSamplersSnapshot(state.m_requestedIndices);
		state.m_unrelatedA = importer->GetTextureSamplersSnapshot({ state.m_indices[1] }).m_slots[0];
		if (!state.m_sourceA.m_slots[1].m_texture || state.m_sourceA.m_slots[2].m_texture || !state.m_unrelatedA.m_texture)
		{ MarkFailed("private sampler snapshot did not contain A plus an actually absent slot"); return; }
		state.m_phase = Phase::Warm;
		m_validation = Tasks::CreateTaskWithResult<std::string>("Warm texture publication nodes", [hold = m_state]() { return WarmNodes(*hold); }, EThreadType::Render);
		m_validation->Run();
		return;
	}
	if (state.m_phase == Phase::ReloadUnrelated || state.m_phase == Phase::ReloadRequested)
	{
		const bool unrelated = state.m_phase == Phase::ReloadUnrelated;
		const auto snapshot = importer->GetTextureSamplersSnapshot({ state.m_indices[unrelated ? 1u : 0u] });
		const auto& before = unrelated ? state.m_unrelatedA : state.m_sourceA.m_slots[1];
		if (snapshot.m_slots[0].m_texture == before.m_texture || snapshot.m_slots[0].m_contentRevision == before.m_contentRevision) return;
		if (unrelated)
		{
			state.m_phase = Phase::CheckUnrelated;
			m_validation = Tasks::CreateTaskWithResult<std::string>("Check unrelated sampler reload", [hold = m_state]() { return CheckUnrelatedReload(*hold); }, EThreadType::Render);
		}
		else
		{
			state.m_sourceB = importer->GetTextureSamplersSnapshot(state.m_requestedIndices);
			state.m_phase = Phase::Validate;
			m_validation = Tasks::CreateTaskWithResult<std::string>("Validate texture publication and shadow retry", [hold = m_state]() { return ValidateReload(*hold); }, EThreadType::Render);
		}
		m_validation->Run();
		return;
	}
	if (!m_validation || !m_validation->IsFinished()) return;
	const std::string error = m_validation->GetResult();
	if (!error.empty())
	{
		std::error_code cleanupError;
		std::filesystem::remove_all(state.m_ownedDirectory, cleanupError);
		MarkFailed(error + (cleanupError ? "; private source cleanup failed: " + cleanupError.message() : ""));
		return;
	}
#if defined(__APPLE__)
	if (state.m_phase == Phase::Warm || state.m_phase == Phase::CheckUnrelated)
	{
		const bool unrelated = state.m_phase == Phase::Warm;
		state.m_phase = unrelated ? Phase::ReloadUnrelated : Phase::ReloadRequested;
		// Main triggers the actual importer path; never wait for its RHI work from an RHI task.
		importer->OnUpdateAssetInfo(state.m_infos[unrelated ? 1u : 0u], true);
		return;
	}
	AddJournalEvent("TextureBindingPublicationEvidence",
		"Real ordinary/instanced main, depth and shadow Prepare: complete native A/remap, unrelated-slot reuse, absent-slot default, cold rejection, warm fallback budget, same-revision C and stable reuse in paged/nonpaged paths; isolated Lighting B token failures caused identical-input C retries and D reuse; required custom depth checked separately");
#else
	AddJournalEvent("TextureBindingPublicationEvidence", "Real ordinary/instanced Prepare uses the global texture set; Apple-only dense cache failure/retry cases were not executed on this platform");
#endif
	std::error_code cleanupError;
	std::filesystem::remove_all(state.m_ownedDirectory, cleanupError);
	if (cleanupError) { MarkFailed("private source cleanup failed: " + cleanupError.message()); return; }
	AddJournalEvent("TextureBindingPublicationScope", "Native references, mapped remap, actual packets and payload tokens only: no shadow pixel/GPU completion or forced allocation failure claim");
	MarkPassed();
}
