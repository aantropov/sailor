#include "Components/Tests/ShadowDrawCompletionTestComponent.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "GraphicsDriver/Vulkan/VulkanBuffer.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include "RHI/VertexDescription.h"
#include <algorithm>
#include <array>
#include <cmath>
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
	const auto HostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
	using Samplers = TextureImporter::TextureSamplersSnapshot;

	class ShadowProbe final : public ShadowPrepassNode
	{
	public:
		using ShadowPrepassNode::m_pBlurHorizontalShader;
		using ShadowPrepassNode::m_pBlurVerticalShader;
		using ShadowPrepassNode::m_pBlurHorizontalMaterial;
		using ShadowPrepassNode::m_pBlurVerticalMaterial;
		using ShadowPrepassNode::m_pBlurShaderBindings;
		ShadowProbe() { SetString("VirtualizeInstancePayloads", "false"); }
		auto Resources(const RHISceneViewSnapshot& scene)
		{
			return scene.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(this, scene.m_cameraIndex, 0u);
		}
		TextureBindingCacheEntry* Entry(uint32_t index)
		{
			const TSet<uint32_t> requested{ 0u, index };
			TextureBindingCacheEntry* result = nullptr;
			m_textureBindingCache.Find(TextureBindingCacheKey(requested), result);
			return result;
		}
		void SetBlurMaterials(RHIMaterialPtr horizontal, RHIMaterialPtr vertical)
		{
			m_pBlurHorizontalMaterial = horizontal;
			m_pBlurVerticalMaterial = vertical;
		}
	};

	class ShadowWorld final : public World
	{
	public:
		ShadowWorld() : World("Shadow completion fixture", 0, CreateEcs()) { BeginPlayEcs(); }
		~ShadowWorld() override { Clear(); }
	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> result;
			result.Add(TUniquePtr<TransformECS>::Make());
			result.Add(TUniquePtr<LightingECS>::Make());
			return result;
		}
	};

	class ShadowGraph final : public RHIFrameGraph
	{
	public:
		ShadowGraph()
		{
			auto& driver = Renderer::GetDriver();
			VertexP3N3UV2C4 vertices[4]{};
			for (uint32_t i = 0u; i < 4u; ++i)
			{
				vertices[i].m_texcoord = glm::vec2(i % 2u, i / 2u);
				vertices[i].m_position = glm::vec3(vertices[i].m_texcoord * 2.0f - 1.0f, 0.5f);
			}
			const uint32_t indices[] = { 0u, 1u, 2u, 2u, 1u, 3u };
			m_postEffectPlane = RHIMeshPtr::Make();
			m_postEffectPlane->m_vertexDescription = driver->GetOrAddVertexDescription<VertexP3N3UV2C4>();
			m_postEffectPlane->m_vertexBuffer = driver->CreateBuffer(sizeof(vertices), EBufferUsageBit::VertexBuffer_Bit, HostMemory);
			m_postEffectPlane->m_indexBuffer = driver->CreateBuffer(sizeof(indices), EBufferUsageBit::IndexBuffer_Bit, HostMemory);
			std::memcpy(m_postEffectPlane->m_vertexBuffer->GetPointer(), vertices, sizeof(vertices));
			std::memcpy(m_postEffectPlane->m_indexBuffer->GetPointer(), indices, sizeof(indices));
			m_postEffectPlane->m_bounds = Math::AABB(glm::vec3(0), glm::vec3(1));
		}
	};

	struct RestoreView
	{
		RHITexturePtr m_texture;
		VulkanImageViewPtr m_original;
		explicit RestoreView(RHITexturePtr texture) : m_texture(texture), m_original(texture->m_vulkan.m_imageView) {}
		void Poison() { m_texture->m_vulkan.m_imageView = VulkanImageViewPtr::Make(VulkanApi::GetInstance()->GetMainDevice(), m_texture->m_vulkan.m_image); }
		void Restore() { m_texture->m_vulkan.m_imageView = m_original; }
		~RestoreView() { Restore(); }
	};

	CameraData Camera()
	{
		CameraData result;
		result.SetAspect(16.0f / 9.0f);
		result.SetFov(60.0f);
		result.SetZNear(0.1f);
		result.SetZFar(80.0f);
		return result;
	}

	RHISpatialSceneVersionPtr CreateCaster(RHIMeshPtr mesh, uint32_t sampler, uint32_t meshCount = 1u)
	{
		RHISceneViewProxy proxy;
		proxy.m_staticMeshEcs = 1u;
		proxy.m_mobility = EMobilityType::Static;
		proxy.m_worldMatrix = glm::mat4(1.0f);
		proxy.m_worldAabb = Math::AABB(glm::vec3(0), glm::vec3(3));
		proxy.m_bCastShadows = true;
		proxy.m_shadowCaster = RHIShadowCasterProxyPtr::Make();
		proxy.m_shadowCaster->m_staticMeshEcs = 1u;
		proxy.m_shadowCaster->m_worldAabb = proxy.m_worldAabb;
		RHIShadowMeshProxy shadow;
		shadow.m_mesh = mesh;
		shadow.m_renderQueueTag = "Masked"_h.GetHash();
		shadow.m_baseColorSampler = sampler;
#if defined(__APPLE__)
		shadow.m_materialTextureSamplers = { 0u, sampler };
#endif
		for (uint32_t i = 0u; i < meshCount; ++i) proxy.m_shadowCaster->m_meshes.Add(shadow);
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

	RHIShaderBindingSetPtr FrameBindings()
	{
		auto& driver = Renderer::GetDriver();
		auto result = driver->CreateShaderBindings();
		UboFrameData frame{};
		frame.m_view = frame.m_projection = frame.m_invProjection = glm::mat4(1.0f);
		frame.m_viewportSize = glm::ivec2(32);
		for (uint32_t i = 0u; i < 2u; ++i)
		{
			auto buffer = driver->CreateBuffer(sizeof(frame), EBufferUsageBit::UniformBuffer_Bit, HostMemory);
			std::memcpy(buffer->GetPointer(), &frame, sizeof(frame));
			driver->AddBufferToShaderBindings(result, buffer, i ? "previousFrame" : "frame", i);
		}
		return result;
	}

	void InitializeSnapshot(RHISceneViewSnapshot& snapshot, RHISpatialSceneVersionPtr scene)
	{
		snapshot.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		snapshot.m_submissionContext->BeginSubmission(80u, 0u);
		snapshot.m_frameBindings = FrameBindings();
		snapshot.m_camera = TUniquePtr<CameraData>::Make(Camera());
		snapshot.m_cameraTransform = Math::Transform(glm::vec4(0, 2, 12, 1));
		snapshot.m_sceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make();
		if (scene) snapshot.m_sceneVersions->Add(scene->m_sceneVersion);
	}

	void AddPass(RHISceneViewSnapshot& snapshot, RHISpatialSceneVersionPtr scene, uint32_t matrixIndex)
	{
		RHIUpdateShadowMapCommand pass;
		pass.m_lighMatrixIndex = matrixIndex;
		pass.m_shadowType = EShadowType::PCF;
		pass.m_lightMatrix = glm::mat4(1.0f);
		pass.m_shadowMap = Renderer::GetDriver()->CreateRenderTarget(glm::ivec2(8), 1u, EFormat::R16_UNORM);
		pass.m_payloadCompletionToken = RHISubmissionCompletionTokenPtr::Make();
		RHISceneViewSnapshot visible;
		InitializeSnapshot(visible, scene);
		visible.ForEachSceneProxy(EMobilityType::Static, [&](const RHIVisibleSceneProxy& proxy)
		{
			RHIVisibleShadowCaster caster;
			caster.m_handle = proxy.m_handle;
			caster.m_record = proxy.m_record;
			caster.m_resource = proxy.m_resource;
			pass.m_meshList.Add(caster);
		});
		if (!snapshot.m_sceneVersions->Contains(scene->m_sceneVersion)) snapshot.m_sceneVersions->Add(scene->m_sceneVersion);
		snapshot.m_shadowMapsToUpdate.Add(std::move(pass));
	}

	void Prepare(ShadowProbe& node, RHIFrameGraphPtr graph, const RHISceneViewSnapshot& snapshot)
	{
		// Render coordinates; actual preparation/finalization uses RHI/Worker.
		auto task = node.Prepare(graph, snapshot);
		if (task) { task->Run(); task->Wait(); }
	}

	void AddEmptyEvsmPass(RHISceneViewSnapshot& snapshot, glm::vec2 radius = glm::vec2(1.0f))
	{
		RHIUpdateShadowMapCommand pass;
		pass.m_shadowType = EShadowType::EVSM;
		pass.m_lightMatrix = glm::mat4(1.0f);
		pass.m_blurRadius = radius;
		pass.m_payloadCompletionToken = RHISubmissionCompletionTokenPtr::Make();
		pass.m_shadowMap = Renderer::GetDriver()->CreateRenderTarget(glm::ivec2(32), 1u, EFormat::R32G32B32A32_SFLOAT);
		snapshot.m_shadowMapsToUpdate.Add(std::move(pass));
	}

	bool Tokens(const RHISceneViewSnapshot& snapshot, const TVector<bool>& expected)
	{
		if (snapshot.m_shadowMapsToUpdate.Num() != expected.Num()) return false;
		for (size_t i = 0u; i < expected.Num(); ++i)
		{
			const auto& token = snapshot.m_shadowMapsToUpdate[i].m_payloadCompletionToken;
			if (!token || token->IsPending() || token->IsSuccessful() != expected[i]) return false;
		}
		return true;
	}

	std::string Record(ShadowProbe& node, RHIFrameGraphPtr graph, const RHISceneViewSnapshot& snapshot,
		uint32_t expectedDraws, const TVector<bool>& tokens, RHIBufferPtr readback = {}, uint32_t expectedCandidates = 0u)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		auto graphics = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		for (auto cmd : { upload, graphics })
		{
			commands->BeginCommandList(cmd, true);
			cmd->m_vulkan.m_commandBuffer->AddDependency(snapshot.m_submissionContext);
			cmd->m_vulkan.m_commandBuffer->AddDependency(graph);
		}
		commands->MemoryBarrier(graphics, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
			static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit) | static_cast<EAccessFlags>(EAccessBit::UniformRead_Bit) |
			static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
		node.Process(graph, upload, graphics, snapshot);
		const auto stats = node.GetDrawCallStats();
		const uint32_t candidates = expectedCandidates ? expectedCandidates : expectedDraws;
		bool recorded = stats.m_numBatches == expectedDraws && stats.m_numInstances == candidates && Tokens(snapshot, tokens);
		const bool emptyCasters = std::all_of(snapshot.m_shadowMapsToUpdate.begin(), snapshot.m_shadowMapsToUpdate.end(),
			[](const auto& pass) { return pass.m_meshList.IsEmpty() && pass.m_internalCommandsList.IsEmpty(); });
		if (emptyCasters)
		{
			const auto commandStats = graphics->GetRecordedDrawCallStats();
			recorded &= commandStats.m_numBatches == expectedDraws && commandStats.m_numInstances == candidates;
		}
		if (recorded && readback)
		{
			auto target = snapshot.m_shadowMapsToUpdate[0].m_shadowMap;
			commands->ImageMemoryBarrier(graphics, target, EImageLayout::TransferSrcOptimal);
			commands->CopyImageToBuffer(graphics, target, readback);
			commands->MemoryBarrier(graphics, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
		}
		commands->EndCommandList(upload);
		commands->EndCommandList(graphics);
		if (!recorded)
		{
			upload->m_vulkan.m_commandBuffer->Reset();
			graphics->m_vulkan.m_commandBuffer->Reset();
			return std::format("recording mismatch: batches={}, candidates={}, expected={}/{}, tokens={}; discarded before submission",
				stats.m_numBatches, stats.m_numInstances, expectedDraws, candidates, Tokens(snapshot, tokens));
		}
		auto ready = driver->CreateWaitSemaphore();
		auto uploadFence = RHIFencePtr::Make();
		auto fence = RHIFencePtr::Make();
		if (!driver->SubmitCommandList(upload, uploadFence, ready) || !driver->SubmitCommandList(graphics, fence, nullptr, ready)) return "shadow submission failed";
		fence->Wait(5000000000ull);
		uploadFence->Wait(5000000000ull);
		if (!fence->IsFinished() || !uploadFence->IsFinished()) return "shadow fence exceeded five seconds; pending dependencies retained";
		upload->m_vulkan.m_commandBuffer->Reset();
		graphics->m_vulkan.m_commandBuffer->Reset();
		return {};
	}

	bool CompleteEntry(const TextureBindingCacheEntry& entry, const Samplers& source)
	{
		if (!entry.m_textureBindings || !entry.m_textureRemapBuffer || entry.m_textureSetSize != 2u || entry.m_sourceSlotRevisions.Num() != 2u ||
			entry.m_sourceDescriptorRevision != source.m_descriptorRevision) return false;
		auto native = entry.m_textureBindings->m_vulkan.m_descriptorSet;
		if (!native || !native->IsCompiled()) return false;
		auto buffer = entry.m_textureRemapBuffer;
		const auto* remap = static_cast<const uint32_t*>(buffer->GetPointer());
		if (!remap) return false;
		const auto range = *buffer->m_vulkan.m_buffer->Get();
		bool nativeRemap = false;
		for (const auto& descriptor : native->m_descriptors)
		{
			if (descriptor->GetBinding() != 0u) continue;
			VkWriteDescriptorSet write{};
			descriptor->Apply(write);
			if (!write.pBufferInfo || write.pBufferInfo->offset > range.m_buffer->m_size) return false;
			const auto bytes = write.pBufferInfo->range == VK_WHOLE_SIZE ? range.m_buffer->m_size - write.pBufferInfo->offset : write.pBufferInfo->range;
			nativeRemap = write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && write.descriptorCount == 1u &&
				write.pBufferInfo->buffer == static_cast<VkBuffer>(*range.m_buffer) && write.pBufferInfo->offset == range.m_offset && bytes == range.m_size;
		}
		if (!nativeRemap) return false;
		for (uint32_t i = 0u; i < TextureImporter::MaxTexturesInScene; ++i)
			if (remap[i] != (i == source.m_slots[1].m_index ? 1u : 0u)) return false;
		for (uint32_t i = 0u; i < 2u; ++i)
		{
			auto texture = i ? source.m_slots[i].m_texture : Renderer::GetDriver()->GetDefaultTexture();
			if (entry.m_sourceSlotRevisions[i] != source.m_slots[i].m_contentRevision || !native->ReferencesImageView(1u, i, texture->m_vulkan.m_imageView)) return false;
		}
		return true;
	}

	std::string FreshRequest(ShadowProbe& node, const RHISceneViewSnapshot& snapshot, uint32_t index)
	{
		auto* entry = node.Entry(index);
		RHIShaderBindingPtr sampler;
		if (!entry || !entry->m_textureBindings->GetShaderBindings().TryGet("textureSamplers", sampler)) return "no private sampler binding to republish";
		auto set = entry->m_textureBindings;
		auto previous = set->m_vulkan.m_descriptorSet;
		const auto revision = set->GetDescriptorRevision();
		const auto compatibility = set->GetCompatibilityHashCode();
		const auto layout = sampler->GetLayout();
		const auto textures = sampler->GetTextureBindings();
		auto result = Renderer::GetDriver()->AddSamplerToShaderBindings(set, layout.m_name, textures, layout.m_binding, true, layout.m_arrayCount);
		if (result != sampler || set->m_vulkan.m_descriptorSet == previous || !set->m_vulkan.m_descriptorSet->IsCompiled() ||
			set->GetDescriptorRevision() <= revision || set->GetCompatibilityHashCode() != compatibility ||
			result->GetLayout().m_arrayCount != layout.m_arrayCount) return "valid native republish did not create a fresh same-layout request";
		uint32_t groups = 0u;
		for (const auto& view : node.Resources(snapshot)->m_activeShadowViews)
			for (const auto& group : view->m_packet.GetGroups())
				if (group.m_batch.m_textureBindings == set)
				{
					++groups;
					if (VulkanApi::IsCompatible(group.m_batch.m_material->m_vulkan.m_pipelines[0]->m_layout, set->m_vulkan.m_descriptorSet, 2u)) return "private dense set unexpectedly bypasses projection";
				}
		return groups ? std::string{} : "fresh sampler request is not used by an actual packet";
	}
}

struct Sailor::ShadowDrawCompletionState
{
	enum class Phase { Loading, Warm, Reload, Validate };
	Phase m_phase = Phase::Loading;
	std::filesystem::path m_ownedDirectory;
	AssetInfoPtr m_info;
	TexturePtr m_texture;
	Tasks::TaskPtr<TexturePtr> m_load;
	uint32_t m_index = 0u;
	Samplers m_sourceA, m_sourceB;
	std::array<ShaderSetPtr, 7> m_shaders{};
	TRefPtr<ShadowGraph> m_graph;
	std::array<TRefPtr<ShadowProbe>, 2> m_nodes;
	std::array<TextureBindingCacheEntry, 2> m_warm;
	std::array<VulkanDescriptorSetPtr, 2> m_warmNative;
	RHISpatialSceneVersionPtr m_caster, m_neighbor;
};

namespace
{
	std::string Warm(ShadowDrawCompletionState& state)
	{
		state.m_graph = TRefPtr<ShadowGraph>::Make();
		state.m_caster = CreateCaster(state.m_graph->GetFullscreenNdcQuad(), state.m_index);
		state.m_neighbor = CreateCaster(state.m_graph->GetFullscreenNdcQuad(), 0u);
		for (uint32_t i = 0u; i < 2u; ++i)
		{
			state.m_nodes[i] = TRefPtr<ShadowProbe>::Make();
			RHISceneViewSnapshot snapshot;
			InitializeSnapshot(snapshot, state.m_caster);
			AddPass(snapshot, state.m_caster, 0u);
			Prepare(*state.m_nodes[i], state.m_graph, snapshot);
			if (!Tokens(snapshot, { true }) || state.m_nodes[i]->Resources(snapshot)->m_activeShadowViews[0]->m_packet.GetNumDrawInstances() != 1u) return "warm Prepare did not produce one successful real caster";
#if defined(__APPLE__)
			auto* entry = state.m_nodes[i]->Entry(state.m_index);
			if (!entry || !CompleteEntry(*entry, state.m_sourceA)) return "warm TexA native references/remap are incomplete";
			state.m_warm[i] = *entry;
			state.m_warmNative[i] = entry->m_textureBindings->m_vulkan.m_descriptorSet;
#endif
		}
		return {};
	}

	bool HasPublishedBuffer(RHIShaderBindingSetPtr set, const char* name, uint32_t index, EShaderBindingType type, size_t bytes)
	{
		RHIShaderBindingPtr binding;
		if (!set || !set->m_vulkan.m_descriptorSet || !set->m_vulkan.m_descriptorSet->IsCompiled() ||
			!set->GetShaderBindings().TryGet(name, binding) || !binding || !binding->m_vulkan.m_valueBinding ||
			binding->GetLayout().m_binding != index || binding->GetLayout().m_type != type) return false;
		const auto range = *binding->m_vulkan.m_valueBinding->Get();
		if (!range.m_buffer || static_cast<VkBuffer>(*range.m_buffer) == VK_NULL_HANDLE || range.m_size < bytes) return false;
		const size_t offset = type == EShaderBindingType::StorageBuffer && !binding->m_vulkan.m_bBindSsboWithOffset ? 0u : range.m_offset;
		for (const auto& descriptor : set->m_vulkan.m_descriptorSet->m_descriptors)
		{
			if (descriptor->GetBinding() != index) continue;
			VkWriteDescriptorSet write{};
			descriptor->Apply(write);
			if (!write.pBufferInfo || write.pBufferInfo->offset > range.m_buffer->m_size) return false;
			const auto nativeBytes = write.pBufferInfo->range == VK_WHOLE_SIZE ?
				range.m_buffer->m_size - write.pBufferInfo->offset : write.pBufferInfo->range;
			return write.descriptorType == static_cast<VkDescriptorType>(type) && write.descriptorCount == 1u &&
				write.pBufferInfo->buffer == static_cast<VkBuffer>(*range.m_buffer) && write.pBufferInfo->offset == offset &&
				nativeBytes >= range.m_offset - offset + bytes;
		}
		return false;
	}

	bool CompleteBlurTuple(const ShadowProbe& node)
	{
		return node.m_pBlurHorizontalMaterial && node.m_pBlurVerticalMaterial &&
			node.m_pBlurHorizontalMaterial->IsReady() && node.m_pBlurVerticalMaterial->IsReady() &&
			node.m_pBlurHorizontalMaterial->GetBindings() == node.m_pBlurShaderBindings &&
			node.m_pBlurVerticalMaterial->GetBindings() == node.m_pBlurShaderBindings &&
			HasPublishedBuffer(node.m_pBlurShaderBindings, "data", 0u, EShaderBindingType::UniformBuffer, 3u * sizeof(glm::vec4));
	}

	std::string ValidateBlurRadius(ShadowDrawCompletionState& state)
	{
		auto node = TRefPtr<ShadowProbe>::Make();
		auto readback = Renderer::GetDriver()->CreateBuffer(1024u * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
		struct Case { EShadowType m_type; glm::vec2 m_radius; uint32_t m_draws; };
		const Case cases[] = {
			{ EShadowType::EVSM, { 0, 0 }, 0 },
			{ EShadowType::PCF, { 2, 2 }, 0 },
			{ EShadowType::EVSM, { 0.1f, 0.1f }, 0 },
			{ EShadowType::EVSM, { 1, 2 }, 2 },
			{ EShadowType::EVSM, { 0, 1 }, 2 },
			{ EShadowType::EVSM, { 1, 0 }, 2 },
			{ EShadowType::EVSM, { 0.11f, 0.1f }, 2 },
			{ EShadowType::EVSM, { 0, 0 }, 0 }
		};
		for (const auto& test : cases)
		{
			RHISceneViewSnapshot snapshot;
			InitializeSnapshot(snapshot, {});
			AddEmptyEvsmPass(snapshot, test.m_radius);
			snapshot.m_shadowMapsToUpdate[0].m_shadowType = test.m_type;
			const auto previousTemplate = node->m_pBlurShaderBindings;
			Prepare(*node, state.m_graph, snapshot);
			if (auto error = Record(*node, state.m_graph, snapshot, test.m_draws, { true }, readback); !error.empty())
				return std::format("blur radius ({}, {}), type {}: {}", test.m_radius.x, test.m_radius.y, static_cast<int>(test.m_type), error);
			if (test.m_draws)
			{
				if (!CompleteBlurTuple(*node) || !node->Resources(snapshot)->m_blurShaderBindings)
					return "positive-radius EVSM did not publish its blur resources";
			}
			else if (node->Resources(snapshot)->m_blurShaderBindings || node->m_pBlurShaderBindings != previousTemplate ||
				(!previousTemplate && (node->m_pBlurHorizontalShader || node->m_pBlurVerticalShader ||
					node->m_pBlurHorizontalMaterial || node->m_pBlurVerticalMaterial)))
				return "an unblurred submission initialized blur-only resources";
			const glm::vec4 expected = test.m_type == EShadowType::EVSM ? glm::vec4(1, 1, -1, 1) : glm::vec4(0);
			const auto* pixels = static_cast<const glm::vec4*>(readback->GetPointer());
			for (uint32_t p = 0u; p < 1024u; ++p)
				for (uint32_t c = 0u; c < 4u; ++c)
					if (!std::isfinite(pixels[p][c]) || std::abs(pixels[p][c] - expected[c]) > 0.00001f)
						return std::format("blur radius ({}, {}), pixel {} channel {}: expected {}, got {}",
							test.m_radius.x, test.m_radius.y, p, c, expected[c], pixels[p][c]);
		}

		auto idle = TRefPtr<ShadowProbe>::Make();
		RHISceneViewSnapshot empty;
		InitializeSnapshot(empty, {});
		if (auto error = Record(*idle, state.m_graph, empty, 0u, {}); !error.empty()) return error;
		if (idle->m_pBlurHorizontalShader || idle->m_pBlurVerticalShader || idle->m_pBlurShaderBindings ||
			idle->Resources(empty)->m_blurShaderBindings) return "an empty submission initialized blur resources";

		RHISceneViewSnapshot atlas;
		InitializeSnapshot(atlas, {});
		AddEmptyEvsmPass(atlas, glm::vec2(0));
		auto& tile = atlas.m_shadowMapsToUpdate[0];
		tile.m_shadowType = EShadowType::PCF;
		Prepare(*node, state.m_graph, atlas);
		if (auto error = Record(*node, state.m_graph, atlas, 0u, { true }); !error.empty()) return error;
		tile.m_shadowType = EShadowType::EVSM;
		tile.m_renderArea = glm::ivec4(8, 8, 8, 8);
		Prepare(*node, state.m_graph, atlas);
		if (auto error = Record(*node, state.m_graph, atlas, 0u, { true }, readback); !error.empty()) return error;
		const auto* pixels = static_cast<const glm::vec4*>(readback->GetPointer());
		for (uint32_t y = 0u; y < 32u; ++y)
			for (uint32_t x = 0u; x < 32u; ++x)
			{
				const glm::vec4 expected = x >= 8u && x < 16u && y >= 8u && y < 16u ? glm::vec4(1, 1, -1, 1) : glm::vec4(0);
				for (uint32_t c = 0u; c < 4u; ++c)
					if (pixels[y * 32u + x][c] != expected[c]) return "unblurred EVSM tile clear changed the wrong atlas pixels";
			}
		return {};
	}

	std::string ValidateColdPublication(ShadowDrawCompletionState& state)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto gate = TRefPtr<ShadowProbe>::Make();
		gate->m_pBlurHorizontalShader = state.m_shaders[5];
		gate->m_pBlurVerticalShader = state.m_shaders[6];
		RHISceneViewSnapshot empty;
		InitializeSnapshot(empty, {});
		AddEmptyEvsmPass(empty);
		Prepare(*gate, state.m_graph, empty);
		auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		auto graphics = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		for (auto cmd : { upload, graphics })
		{
			commands->BeginCommandList(cmd, true);
			cmd->m_vulkan.m_commandBuffer->AddDependency(empty.m_submissionContext);
			cmd->m_vulkan.m_commandBuffer->AddDependency(state.m_graph);
		}
		gate->Process(state.m_graph, upload, graphics, empty);
		commands->EndCommandList(upload);
		commands->EndCommandList(graphics);
		const bool complete = CompleteBlurTuple(*gate);
		auto firstFlight = gate->Resources(empty)->m_blurShaderBindings;
		const bool flightReady = HasPublishedBuffer(firstFlight, "data", 0u, EShaderBindingType::UniformBuffer, 3u * sizeof(glm::vec4));
		if (!complete || !flightReady)
		{
			upload->m_vulkan.m_commandBuffer->Reset();
			graphics->m_vulkan.m_commandBuffer->Reset();
			return std::format("preloaded shader latch: complete tuple={}, flight UBO={}; discarded before submission; incompatible-reflection cases not entered", complete, flightReady);
		}
		auto ready = driver->CreateWaitSemaphore();
		auto uploadFence = RHIFencePtr::Make();
		auto fence = RHIFencePtr::Make();
		if (!driver->SubmitCommandList(upload, uploadFence, ready) || !driver->SubmitCommandList(graphics, fence, nullptr, ready)) return "cold tuple submission failed";
		fence->Wait(5000000000ull);
		uploadFence->Wait(5000000000ull);
		if (!fence->IsFinished() || !uploadFence->IsFinished()) return "cold tuple fence exceeded five seconds; dependencies retained";
		upload->m_vulkan.m_commandBuffer->Reset();
		graphics->m_vulkan.m_commandBuffer->Reset();
		auto firstTemplate = gate->m_pBlurShaderBindings;
		auto firstHorizontal = gate->m_pBlurHorizontalMaterial;
		auto firstVertical = gate->m_pBlurVerticalMaterial;
		auto templateNative = firstTemplate->m_vulkan.m_descriptorSet;
		const auto templateRevision = firstTemplate->GetDescriptorRevision();
		const auto templateHash = firstTemplate->GetCompatibilityHashCode();
		auto firstNative = firstFlight->m_vulkan.m_descriptorSet;
		const auto firstRevision = firstFlight->GetDescriptorRevision();
		empty.m_shadowMapsToUpdate.Clear();
		if (auto error = Record(*gate, state.m_graph, empty, 0u, {}); !error.empty()) return error;
		if (!CompleteBlurTuple(*gate) || gate->m_pBlurShaderBindings != firstTemplate || gate->m_pBlurHorizontalMaterial != firstHorizontal ||
			gate->m_pBlurVerticalMaterial != firstVertical || gate->Resources(empty)->m_blurShaderBindings != firstFlight ||
			firstTemplate->m_vulkan.m_descriptorSet != templateNative || firstTemplate->GetDescriptorRevision() != templateRevision ||
			firstTemplate->GetCompatibilityHashCode() != templateHash ||
			firstFlight->m_vulkan.m_descriptorSet != firstNative || firstFlight->GetDescriptorRevision() != firstRevision) return "committed cold tuple or flight UBO was rebuilt on reuse";

		auto incompatible = driver->CreateShaderBindings();
		if (!driver->FillShadersLayout(incompatible, { state.m_shaders[4]->GetDebugVertexShaderRHI(), state.m_shaders[4]->GetDebugFragmentShaderRHI() }, 1u)) return "ShadowCaster reflection is unavailable";
		const auto& reflected = incompatible->GetLayoutBindings();
		if (reflected.FindIf([](const ShaderLayoutBinding& binding)
			{ return binding.m_binding == 0u && binding.m_name == "data" && binding.m_type == EShaderBindingType::StorageBuffer; }) == static_cast<size_t>(-1)) return "real ShadowCaster data binding is not the incompatible StorageBuffer prerequisite";
		for (uint32_t flightOnly = 0u; flightOnly < 2u; ++flightOnly)
		{
			auto node = TRefPtr<ShadowProbe>::Make();
			node->m_pBlurHorizontalShader = state.m_shaders[5];
			node->m_pBlurVerticalShader = state.m_shaders[6];
			RHISceneViewSnapshot previousFlight;
			InitializeSnapshot(previousFlight, {});
			if (flightOnly)
			{
				AddEmptyEvsmPass(previousFlight);
				Prepare(*node, state.m_graph, previousFlight);
				if (auto error = Record(*node, state.m_graph, previousFlight, 2u, { true }); !error.empty()) return error;
				if (!CompleteBlurTuple(*node)) return "submission rejection requires a complete retained node tuple";
			}
			auto oldTemplate = node->m_pBlurShaderBindings;
			auto oldHorizontal = node->m_pBlurHorizontalMaterial;
			auto oldVertical = node->m_pBlurVerticalMaterial;
			auto oldNative = oldTemplate ? oldTemplate->m_vulkan.m_descriptorSet : VulkanDescriptorSetPtr{};
			const auto oldRevision = oldTemplate ? oldTemplate->GetDescriptorRevision() : 0u;
			const auto oldHash = oldTemplate ? oldTemplate->GetCompatibilityHashCode() : 0u;
			auto oldFlight = flightOnly ? node->Resources(previousFlight)->m_blurShaderBindings : RHIShaderBindingSetPtr{};
			if (flightOnly && !HasPublishedBuffer(oldFlight, "data", 0u, EShaderBindingType::UniformBuffer, 3u * sizeof(glm::vec4))) return "submission rejection requires a complete previous flight UBO";
			auto oldFlightNative = oldFlight ? oldFlight->m_vulkan.m_descriptorSet : VulkanDescriptorSetPtr{};
			const auto oldFlightRevision = oldFlight ? oldFlight->GetDescriptorRevision() : 0u;
			RHISceneViewSnapshot snapshot;
			InitializeSnapshot(snapshot, state.m_neighbor);
			RHIUpdateShadowMapCommand evsm;
			evsm.m_shadowType = EShadowType::EVSM;
			evsm.m_lightMatrix = glm::mat4(1.0f);
			evsm.m_blurRadius = glm::vec2(1.0f);
			evsm.m_payloadCompletionToken = RHISubmissionCompletionTokenPtr::Make();
			evsm.m_shadowMap = driver->CreateRenderTarget(glm::ivec2(32), 1u, EFormat::R32G32B32A32_SFLOAT);
			snapshot.m_shadowMapsToUpdate.Add(std::move(evsm));
			AddPass(snapshot, state.m_neighbor, 1u);
			auto resources = node->Resources(snapshot);
			if (resources->m_blurShaderBindings) return "fresh private flight already has blur bindings";
			auto target = snapshot.m_shadowMapsToUpdate[0].m_shadowMap;
			auto readback = driver->CreateBuffer(1024u * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
			node->m_pBlurVerticalShader = state.m_shaders[4];
			for (uint32_t retry = 0u; retry < 2u; ++retry)
			{
				if (retry) node->m_pBlurVerticalShader = state.m_shaders[6];
				Prepare(*node, state.m_graph, snapshot);
				if (!Tokens(snapshot, { true, true }) || resources->m_activeShadowViews[0]->m_packet.GetNumDrawInstances() != 0u ||
					resources->m_activeShadowViews[1]->m_packet.GetNumDrawInstances() != 1u ||
					resources->m_activeShadowViews[1]->m_packet.GetGroups().Num() != 1u) return "cold rejection needs one independent real PCF caster and an empty EVSM packet";
				if (auto error = Record(*node, state.m_graph, snapshot, retry ? 3u : 1u, { retry != 0u, true }, readback); !error.empty())
					return std::format("{} UBO {}: {}", flightOnly ? "flight" : "template", retry ? "retry" : "rejection", error);
				const auto* pixels = static_cast<const glm::vec4*>(readback->GetPointer());
				const glm::vec4 expected(1, 1, -1, 1);
				for (uint32_t p = 0u; p < 1024u; ++p)
					for (uint32_t c = 0u; c < 4u; ++c)
						if (!std::isfinite(pixels[p][c]) || std::abs(pixels[p][c] - expected[c]) > 0.00001f)
							return std::format("cold {} UBO retry {} pixel {} channel {}: expected {}, got {}; pixel=({}, {}, {}, {})",
								flightOnly ? "flight" : "template", retry, p, c, expected[c], pixels[p][c],
								pixels[p].x, pixels[p].y, pixels[p].z, pixels[p].w);
				if (snapshot.m_shadowMapsToUpdate[0].m_shadowMap != target) return "UBO retry changed the requested target";
				if (flightOnly && (node->Resources(previousFlight)->m_blurShaderBindings != oldFlight ||
					oldFlight->m_vulkan.m_descriptorSet != oldFlightNative || oldFlight->GetDescriptorRevision() != oldFlightRevision)) return "new flight preparation changed the retained previous flight UBO";
				if (!retry || flightOnly)
				{
					if (node->m_pBlurShaderBindings != oldTemplate || node->m_pBlurHorizontalMaterial != oldHorizontal || node->m_pBlurVerticalMaterial != oldVertical ||
						(oldTemplate && (oldTemplate->m_vulkan.m_descriptorSet != oldNative || oldTemplate->GetDescriptorRevision() != oldRevision || oldTemplate->GetCompatibilityHashCode() != oldHash))) return "UBO rejection changed the retained node tuple";
				}
				if (!retry)
				{
					if (resources->m_blurShaderBindings || !Tokens(snapshot, { false, true })) return "failed candidate was published or its failed token was lost before retry";
				}
				else if (!CompleteBlurTuple(*node) || resources->m_blurShaderBindings == oldFlight ||
					!HasPublishedBuffer(resources->m_blurShaderBindings, "data", 0u, EShaderBindingType::UniformBuffer, 3u * sizeof(glm::vec4))) return "corrected same-owner UBO retry did not publish complete native data bindings";
			}
		}

		// Positive cold/growth/reuse coverage only: no deterministic SSBO failure seam.
		auto node = TRefPtr<ShadowProbe>::Make();
		node->m_pBlurHorizontalShader = state.m_shaders[5];
		node->m_pBlurVerticalShader = state.m_shaders[6];
		RHISceneViewSnapshot snapshot;
		InitializeSnapshot(snapshot, state.m_neighbor);
		AddPass(snapshot, state.m_neighbor, 0u);
		auto resources = node->Resources(snapshot);
		Prepare(*node, state.m_graph, snapshot);
		auto view = resources->m_activeShadowViews[0];
		if (view->m_perInstanceData || view->m_sizePerInstanceData || view->m_sizeInstanceIndices) return "cold SSBO view was already populated";
		RHIShaderBindingSetPtr previousSet;
		VulkanDescriptorSetPtr previousNative;
		RHIShaderBindingSetPtr coldSet;
		for (uint32_t phase = 0u; phase < 3u; ++phase)
		{
			const uint32_t count = phase ? 3u : 1u;
			if (phase == 1u)
			{
				auto larger = CreateCaster(state.m_graph->GetFullscreenNdcQuad(), 0u, 3u);
				snapshot.m_shadowMapsToUpdate.Clear(false);
				AddPass(snapshot, larger, 0u);
			}
			Prepare(*node, state.m_graph, snapshot);
			if (!Tokens(snapshot, { true }) || resources->m_activeShadowViews[0] != view || view->m_packet.GetNumStorageInstances() != count ||
				view->m_packet.GetNumDrawInstances() != count || view->m_packet.GetGroups().Num() != 1u) return "SSBO growth did not use one real same-view packed run";
			if (auto error = Record(*node, state.m_graph, snapshot, 1u, { true }, {}, count); !error.empty()) return "SSBO cold/growth/reuse: " + error;
			const size_t dataBytes = sizeof(ShadowPrepassNode::PerInstanceData) * count;
			const size_t indexBytes = sizeof(uint32_t) * count;
			auto set = view->m_perInstanceData;
			if (!view->m_bUploadedThisSubmission || view->m_sizePerInstanceData != dataBytes || view->m_sizeInstanceIndices != indexBytes ||
				!HasPublishedBuffer(set, "data", 0u, EShaderBindingType::StorageBuffer, dataBytes) ||
				!HasPublishedBuffer(set, "indices", 1u, EShaderBindingType::StorageBuffer, indexBytes)) return "SSBO pair/capacity/native publication is incomplete";
			if (phase == 1u && (set == previousSet || set->m_vulkan.m_descriptorSet == previousNative)) return "larger packet did not publish a fresh SSBO pair";
			if (phase == 2u && (set != previousSet || set->m_vulkan.m_descriptorSet != previousNative)) return "same-size packet unnecessarily replaced its SSBO pair";
			if (!phase) coldSet = set;
			previousSet = set;
			previousNative = set->m_vulkan.m_descriptorSet;
		}
		if (!HasPublishedBuffer(coldSet, "data", 0u, EShaderBindingType::StorageBuffer, sizeof(ShadowPrepassNode::PerInstanceData)) ||
			!HasPublishedBuffer(coldSet, "indices", 1u, EShaderBindingType::StorageBuffer, sizeof(uint32_t))) return "retained original SSBO pair lost its native resources after growth";
		return {};
	}

	[[maybe_unused]] std::string ValidateLighting(ShadowDrawCompletionState& state)
	{
		ShadowWorld world;
		auto owner = world.Instantiate("Private directional light");
		owner->GetTransformComponent().SetRotation(glm::quat(glm::radians(glm::vec3(-35, 20, 0))));
		world.GetECS<TransformECS>()->Tick(0.0f);
		world.GetECS<TransformECS>()->PostTick();
		auto* lighting = world.GetECS<LightingECS>();
		auto& light = lighting->GetComponentData(lighting->RegisterComponent());
		light.SetOwner(owner);
		light.m_type = ELightType::Directional;
		light.m_shadowType = EShadowType::PCF;
		light.MarkDirty();
		lighting->Tick(0.0f);
		auto scene = RHISceneViewPtr::Make();
		scene->m_world = &world;
		scene->m_cameras.Add(Camera());
		scene->m_cameraTransforms.Add(Math::Transform(glm::vec4(0, 2, 12, 1)));
		auto context = RHIRenderSubmissionContextPtr::Make();
		context->BeginSubmission(80u, 0u);
		scene->SetSubmissionContext(context);
		auto submission = scene->GetOrCreateSubmissionCompletionToken();
		scene->AddSceneVersion(state.m_caster);
		if (scene->m_bHasCustomDepthShadowCasters) return "Lighting retry cannot use forced custom-depth refresh";
		auto& node = *state.m_nodes[0];
		const auto fill = [&]()
		{
			scene->m_rhiLightsDataPerCamera.Clear(false);
			lighting->FillLightingData(scene);
			scene->PrepareSnapshots();
			scene->m_snapshots[0].m_frameBindings = FrameBindings();
		};
		fill();
		auto& snapshot = scene->m_snapshots[0];
		RHIShaderBindingSetPtr failedRequest;
		VulkanDescriptorSetPtr failedNative;
		uint64_t failedRevision = 0u;
		size_t failedHash = 0u;
		TSet<uint32_t> failedPayloads;
		for (uint32_t phase = 0u; phase < 3u; ++phase)
		{
			if (snapshot.m_shadowMapsToUpdate.IsEmpty()) return std::format("Lighting phase {} emitted no updates", phase);
			Prepare(node, state.m_graph, snapshot);
			TVector<bool> before, after;
			uint32_t candidates = 0u;
			const auto resources = node.Resources(snapshot);
			for (uint32_t passIndex = 0u; passIndex < resources->m_activeShadowViews.Num(); ++passIndex)
			{
				const auto& view = resources->m_activeShadowViews[passIndex];
				const uint32_t count = view->m_packet.GetNumDrawInstances();
				if (count > 1u) return "fixture expected at most one caster per cascade";
				candidates += count;
				if (phase == 1u && count != 0u) failedPayloads.Insert(snapshot.m_shadowMapsToUpdate[passIndex].m_lighMatrixIndex);
				before.Add(true);
				after.Add(phase != 1u || count == 0u);
			}
			if (!candidates || !Tokens(snapshot, before)) return "Lighting Prepare did not produce successful nonempty caster packets";
			RestoreView restore(state.m_sourceB.m_slots[1].m_texture);
			if (phase == 1u)
			{
				if (auto error = FreshRequest(node, snapshot, state.m_index); !error.empty()) return error;
				restore.Poison();
			}
			auto request = node.Entry(state.m_index)->m_textureBindings;
			const auto revision = request->GetDescriptorRevision();
			const auto hash = request->GetCompatibilityHashCode();
			if (phase == 1u) { failedRequest = request; failedNative = request->m_vulkan.m_descriptorSet; failedRevision = revision; failedHash = hash; }
			if (phase == 2u && (request != failedRequest || request->m_vulkan.m_descriptorSet != failedNative || revision != failedRevision || hash != failedHash)) return "C rebuilt the failed request instead of retrying it";
			if (auto error = Record(node, state.m_graph, snapshot, phase == 1u ? 0u : candidates, after); !error.empty()) return std::format("Lighting phase {}: {}", phase, error);
			restore.Restore();
			if (request->GetDescriptorRevision() != revision || request->GetCompatibilityHashCode() != hash) return "view repair changed the failed descriptor request";
			if (phase == 0u) scene->m_cameraTransforms[0].m_position.x += 4.0f;
			fill(); // B's failed tokens were checked by Record before Lighting may reset them.
			if (scene->GetOrCreateSubmissionCompletionToken() != submission || !submission->IsPending()) return "Lighting changed the pending submission identity";
			if (phase == 1u)
			{
				for (const auto& retry : snapshot.m_shadowMapsToUpdate)
					if (!failedPayloads.Remove(retry.m_lighMatrixIndex)) return "identical C retried an unaffected or duplicate payload";
				if (!failedPayloads.IsEmpty()) return "identical C did not retry every failed payload";
			}
		}
		return snapshot.m_shadowMapsToUpdate.IsEmpty() ? std::string{} : "unchanged D did not reuse successful C";
	}

	[[maybe_unused]] std::string ValidateDependencies(ShadowDrawCompletionState& state)
	{
		auto& node = *state.m_nodes[1];
		RHISceneViewSnapshot snapshot;
		InitializeSnapshot(snapshot, state.m_caster);
		AddPass(snapshot, state.m_caster, 0u);
		AddPass(snapshot, state.m_neighbor, 1u);
		AddPass(snapshot, state.m_neighbor, 2u);
		snapshot.m_shadowMapsToUpdate[1].m_internalCommandsList.Add(0u);
		RestoreView restore(state.m_sourceB.m_slots[1].m_texture);
		restore.Poison();
		Prepare(node, state.m_graph, snapshot);
		auto* entry = node.Entry(state.m_index);
		const auto& warm = state.m_warm[1];
		if (!entry || entry->m_textureBindings != warm.m_textureBindings || entry->m_textureRemapBuffer != warm.m_textureRemapBuffer ||
			entry->m_textureBindings->m_vulkan.m_descriptorSet != state.m_warmNative[1] ||
			!CompleteEntry(*entry, state.m_sourceA) || !Tokens(snapshot, { false, true, true })) return "Prepare did not retain exact full TexA fallback with false/true/true tokens";
		auto resources = node.Resources(snapshot);
		for (uint32_t i = 0u; i < 3u; ++i)
		{
			const auto& view = resources->m_activeShadowViews[i];
			if (view->m_packet.GetNumDrawInstances() != 1u || view->m_packet.GetGroups().Num() != 1u ||
				(i && view == resources->m_activeShadowViews[i - 1u])) return "dependency view identities or one-caster packets are incorrect";
		}
		if (resources->m_activeShadowViews[0]->m_packet.GetGroups()[0].m_batch.m_textureBindings != warm.m_textureBindings) return "D packet does not use retained native TexA";
		if (auto error = Record(node, state.m_graph, snapshot, 4u, { false, false, true }); !error.empty()) return "full fallback dependency: " + error;
		restore.Restore();
		Prepare(node, state.m_graph, snapshot);
		if (!Tokens(snapshot, { true, true, true }) || !CompleteEntry(*node.Entry(state.m_index), state.m_sourceB)) return "fallback retry failed to prepare current TexB";
		if (auto error = Record(node, state.m_graph, snapshot, 4u, { true, true, true }); !error.empty()) return "fallback retry: " + error;
		Prepare(node, state.m_graph, snapshot);
		if (auto error = FreshRequest(node, snapshot, state.m_index); !error.empty()) return error;
		if (!Tokens(snapshot, { true, true, true })) return "late dependency rejection did not start from successful Prepare";
		restore.Poison();
		if (auto error = Record(node, state.m_graph, snapshot, 2u, { false, false, true }); !error.empty()) return "late dependency rejection: " + error;
		restore.Restore();
		Prepare(node, state.m_graph, snapshot);
		return Record(node, state.m_graph, snapshot, 4u, { true, true, true });
	}

	std::string ValidateBlurPublication(RHIFrameGraphPtr graph, RHIMaterialPtr horizontal, RHIMaterialPtr vertical)
	{
		auto& driver = Renderer::GetDriver();
		auto node = TRefPtr<ShadowProbe>::Make();
		RHISceneViewSnapshot snapshot;
		InitializeSnapshot(snapshot, {});
		AddEmptyEvsmPass(snapshot);
		Prepare(*node, graph, snapshot);
		if (auto error = Record(*node, graph, snapshot, 2u, { true }); !error.empty()) return "blur publication initialization: " + error;
		node->SetBlurMaterials(horizontal, vertical);
		auto resources = node->Resources(snapshot);
		auto bindings = resources->m_blurShaderBindings;
		auto extra = driver->AddBufferToShaderBindings(bindings, "unusedProducer", sizeof(glm::vec4), 31u, EShaderBindingType::UniformBuffer);
		if (!extra || !extra->m_vulkan.m_valueBinding) return "unused producer buffer could not be published";
		for (auto material : { horizontal, vertical })
		{
			const auto& layouts = material->m_vulkan.m_pipelines[0]->m_layout->m_descriptionSetLayouts;
			if (layouts.Num() != 2u || layouts[1]->m_descriptorSetLayoutBindings.Num() != 1u ||
				layouts[1]->m_descriptorSetLayoutBindings[0].binding != 1u) return "normal blur projection must omit unused producer binding 31";
		}
		auto& request = snapshot.m_shadowMapsToUpdate[0];
		auto readback = driver->CreateBuffer(1024u * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
		auto checkPixels = [&](uint32_t count, glm::vec4 expected, const char* phase) -> std::string
		{
			const auto* pixels = static_cast<const glm::vec4*>(readback->GetPointer());
			for (uint32_t p = 0u; p < count; ++p)
				for (uint32_t c = 0u; c < 4u; ++c)
					if (!std::isfinite(pixels[p][c]) || std::abs(pixels[p][c] - expected[c]) > 0.00001f)
						return std::format("blur publication {} pixel {} channel {}: expected {}, got {}", phase, p, c, expected[c], pixels[p][c]);
			return {};
		};
		Prepare(*node, graph, snapshot);
		if (!Tokens(snapshot, { true }) || resources->m_activeShadowViews[0]->m_packet.GetNumDrawInstances()) return "blur publication A has no complete empty packet";
		if (auto error = Record(*node, graph, snapshot, 2u, { true }, readback); !error.empty()) return "blur publication A: " + error;
		if (auto error = checkPixels(1024u, glm::vec4(1.25f, 1.5f, -1, 1), "A"); !error.empty()) return error;
		RHIShaderBindingPtr samplerA;
		if (!bindings->GetShaderBindings().TryGet("colorSampler", samplerA) || !samplerA->GetTextureBinding()) return "blur publication A has no sampler";
		auto textureA = samplerA->GetTextureBinding();
		auto viewA = textureA->m_vulkan.m_imageView;
		auto nativeA = bindings->m_vulkan.m_descriptorSet;
		const auto revisionA = bindings->GetDescriptorRevision();
		const auto hashA = bindings->GetCompatibilityHashCode();
		if (textureA == request.m_shadowMap || textureA->GetExtent() != glm::ivec2(32) || !nativeA ||
			!nativeA->IsCompiled() || !nativeA->ReferencesImageView(1u, 0u, viewA)) return "A does not retain its real horizontal temporary image";
		for (auto material : { horizontal, vertical })
			if (VulkanApi::IsCompatible(material->m_vulkan.m_pipelines[0]->m_layout, nativeA, 1u)) return "unused producer binding did not force blur projection";

		struct RestoreBuffer
		{
			RHIShaderBindingPtr m_binding;
			decltype(m_binding->m_vulkan.m_valueBinding) m_value;
			void Restore() { m_binding->m_vulkan.m_valueBinding = m_value; }
			~RestoreBuffer() { Restore(); }
		} restore{ extra, extra->m_vulkan.m_valueBinding };
		// A's sampled 32x32 temporary cannot alias the pool's new 16x16 H output.
		auto targetB = driver->CreateRenderTarget(glm::ivec2(16), 1u, EFormat::R32G32B32A32_SFLOAT);
		request.m_shadowMap = targetB;
		Prepare(*node, graph, snapshot);
		if (!Tokens(snapshot, { true }) || resources->m_activeShadowViews[0]->m_packet.GetNumDrawInstances()) return "blur publication B did not start Prepare-complete";
		auto unavailable = restore.m_value->Get();
		auto originalBuffer = unavailable.m_ptr.m_buffer;
		unavailable.m_ptr.m_buffer = VulkanBufferPtr::Make(VulkanApi::GetInstance()->GetMainDevice(),
			originalBuffer->m_size, originalBuffer->m_usage, originalBuffer->m_sharingMode);
		if (static_cast<VkBuffer>(*unavailable.m_ptr.m_buffer) != VK_NULL_HANDLE) return "producer failure buffer unexpectedly has a native handle";
		extra->m_vulkan.m_valueBinding = decltype(restore.m_value)::Make(unavailable, TWeakPtr<RHIShaderBinding::VulkanBufferAllocator>{});
		// Record rejects old-owner stale draws before submission, even though projecting
		// A for these normal materials legitimately excludes the unavailable binding 31.
		const auto rejected = Record(*node, graph, snapshot, 0u, { false }, readback);
		RHIShaderBindingPtr retained;
		const bool unchanged = resources->m_blurShaderBindings == bindings && bindings->GetShaderBindings().TryGet("colorSampler", retained) &&
			retained == samplerA && retained->GetTextureBinding() == textureA && textureA->m_vulkan.m_imageView == viewA &&
			bindings->m_vulkan.m_descriptorSet == nativeA && bindings->GetDescriptorRevision() == revisionA &&
			bindings->GetCompatibilityHashCode() == hashA && nativeA->IsCompiled() && nativeA->ReferencesImageView(1u, 0u, viewA);
		if (!rejected.empty()) return std::format("blur producer refusal: {}; A publication unchanged={}", rejected, unchanged);
		if (!unchanged) return "rejected blur producer changed A sampler/native/revision/hash";
		if (auto error = checkPixels(256u, glm::vec4(1, 1, -1, 1), "B rejected"); !error.empty()) return error;
		if (!Tokens(snapshot, { false })) return "rejected B payload became successful before retry";
		restore.Restore();
		Prepare(*node, graph, snapshot);
		if (request.m_shadowMap != targetB || !Tokens(snapshot, { true }) ||
			resources->m_activeShadowViews[0]->m_packet.GetNumDrawInstances()) return "same-target blur retry did not prepare successfully";
		if (auto error = Record(*node, graph, snapshot, 2u, { true }, readback); !error.empty()) return "blur publication retry: " + error;
		if (auto error = checkPixels(256u, glm::vec4(1.25f, 1.5f, -1, 1), "B retry"); !error.empty()) return error;
		RHIShaderBindingPtr samplerC;
		if (!bindings->GetShaderBindings().TryGet("colorSampler", samplerC) || samplerC != samplerA ||
			!samplerC->GetTextureBinding() || samplerC->GetTextureBinding() == textureA || samplerC->GetTextureBinding() == targetB ||
			samplerC->GetTextureBinding()->GetExtent() != glm::ivec2(16) ||
			bindings->m_vulkan.m_descriptorSet == nativeA || !bindings->m_vulkan.m_descriptorSet->IsCompiled() ||
			bindings->GetDescriptorRevision() <= revisionA ||
			!bindings->m_vulkan.m_descriptorSet->ReferencesImageView(1u, 0u, samplerC->GetTextureBinding()->m_vulkan.m_imageView) ||
			!nativeA->IsCompiled() || !nativeA->ReferencesImageView(1u, 0u, viewA)) return "same-target retry did not publish B while retaining native A's original image";
		return {};
	}

	std::string ValidateBlur(ShadowDrawCompletionState& state)
	{
		auto& driver = Renderer::GetDriver();
		auto& node = *state.m_nodes[0];
		RHISceneViewSnapshot snapshot;
		InitializeSnapshot(snapshot, {});
		AddEmptyEvsmPass(snapshot);
		Prepare(node, state.m_graph, snapshot);
		// Initialize actual Process-owned blur shaders/data, before swapping only its materials.
		if (auto error = Record(node, state.m_graph, snapshot, 2u, { true }); !error.empty()) return "blur initialization: " + error;
		std::array<RHIMaterialPtr, 4> materials;
		const RenderState renderState(false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0u, false);
		for (uint32_t i = 0u; i < 4u; ++i)
		{
			materials[i] = driver->CreateMaterial(state.m_graph->GetFullscreenNdcQuad()->m_vertexDescription,
				EPrimitiveTopology::TriangleList, renderState, state.m_shaders[i]);
			if (!materials[i]) return "blur fixture material creation failed";
			const auto& layouts = materials[i]->m_vulkan.m_pipelines[0]->m_layout->m_descriptionSetLayouts;
			if (layouts.Num() != 2u || layouts[1]->m_descriptorSetLayoutBindings.Num() != 1u ||
				layouts[1]->m_descriptorSetLayoutBindings[0].binding != 1u ||
				layouts[0]->m_descriptorSetLayoutBindings.Num() != (i >= 2u ? 1u : 0u) ||
				(i >= 2u && layouts[0]->m_descriptorSetLayoutBindings[0].binding != 7u)) return "compiled H/V interfaces do not isolate read-only frame binding 7";
		}
		auto readback = driver->CreateBuffer(32u * 32u * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
		for (uint32_t failVertical = 0u; failVertical < 2u; ++failVertical)
		{
			const glm::vec4 zero(0);
			auto texture = driver->CreateTexture(&zero, sizeof(zero), glm::ivec3(1), 1u, ETextureType::Texture2D,
				EFormat::R32G32B32A32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp);
			snapshot.m_frameBindings = driver->CreateShaderBindings();
			auto extra = driver->CreateBuffer(16u, EBufferUsageBit::UniformBuffer_Bit, HostMemory);
			if (!driver->AddBufferToShaderBindings(snapshot.m_frameBindings, extra, "unused", 0u) ||
				!driver->AddSamplerToShaderBindings(snapshot.m_frameBindings, "failureInput", texture, 7u)) return "read-only blur frame setup failed";
			node.SetBlurMaterials(materials[failVertical ? 0u : 2u], materials[failVertical ? 3u : 1u]);
			if (VulkanApi::IsCompatible(materials[failVertical ? 3u : 2u]->m_vulkan.m_pipelines[0]->m_layout,
				snapshot.m_frameBindings->m_vulkan.m_descriptorSet, 0u)) return "fresh blur frame must require projection";
			RestoreView restore(texture);
			for (uint32_t repaired = 0u; repaired < 2u; ++repaired)
			{
				Prepare(node, state.m_graph, snapshot);
				if (!Tokens(snapshot, { true }) || node.Resources(snapshot)->m_activeShadowViews[0]->m_packet.GetNumDrawInstances()) return "empty EVSM packet was not legitimately Prepare-complete";
				if (!repaired) restore.Poison();
				const uint32_t draws = repaired ? 2u : failVertical;
				if (auto error = Record(node, state.m_graph, snapshot, draws, { repaired != 0u }, readback); !error.empty()) return std::format("{} blur {}: {}", failVertical ? "vertical" : "horizontal", repaired ? "retry" : "rejection", error);
				const glm::vec4 expected = repaired ? glm::vec4(1.25f, 1.5f, -1, 1) : glm::vec4(1, 1, -1, 1);
				const auto* pixels = static_cast<const glm::vec4*>(readback->GetPointer());
				for (uint32_t p = 0u; p < 1024u; ++p)
					for (uint32_t c = 0u; c < 4u; ++c)
						if (!std::isfinite(pixels[p][c]) || std::abs(pixels[p][c] - expected[c]) > 0.00001f)
							return std::format("blur mode {} retry {} pixel {} channel {}: expected {}, got {}", failVertical, repaired, p, c, expected[c], pixels[p][c]);
				restore.Restore();
			}
		}
		return ValidateBlurPublication(state.m_graph, materials[0], materials[1]);
	}

	std::string WriteTexture(const std::filesystem::path& filename, uint8_t red)
	{
		std::array<uint8_t, 34> tga{};
		tga[2] = 2u; tga[12] = 2u; tga[14] = 2u; tga[16] = 32u; tga[17] = 0x28u;
		for (uint32_t p = 0u; p < 4u; ++p)
		{
			tga[18u + p * 4u] = 40u; tga[19u + p * 4u] = 110u;
			tga[20u + p * 4u] = red; tga[21u + p * 4u] = 255u;
		}
		std::ofstream output(filename, std::ios::binary);
		output.write(reinterpret_cast<const char*>(tga.data()), static_cast<std::streamsize>(tga.size()));
		output.close();
		return output ? std::string{} : "private TGA write failed";
	}

	std::string CreateAssets(ShadowDrawCompletionState& state, const std::string& runId)
	{
		auto* registry = App::GetSubmodule<AssetRegistry>();
		std::filesystem::path filename;
		if (!registry->ResolveWorkspaceContentPathForWrite("Tests/ShadowDrawCompletion-" + runId + "/requested.tga", filename)) return "no writable private content path";
		std::error_code error;
		if (!std::filesystem::create_directory(filename.parent_path(), error) || error) return "private content leaf was not created exclusively";
		state.m_ownedDirectory = filename.parent_path();
		if (auto message = WriteTexture(filename, 80u); !message.empty()) return message;
		state.m_info = registry->GetAssetInfoPtr(registry->GetOrLoadFile(filename.string()));
		if (!state.m_info) return "normal registry import did not create texture metadata";
		const char* paths[] = { "Tests/Shaders/ShadowDrawCompletion.shader", "Shaders/ShadowCaster.shader", "Shaders/Blur.shader" };
		AssetInfoPtr shaderInfos[3];
		for (uint32_t i = 0u; i < 3u; ++i)
			if (!(shaderInfos[i] = registry->GetAssetInfoPtr(paths[i]))) return "shadow fixture shader metadata is missing";
		const std::array<TVector<std::string>, 7> defines{ TVector<std::string>{"HORIZONTAL"}, {}, {"HORIZONTAL", "READ_FAILURE_INPUT"},
			{"READ_FAILURE_INPUT"}, {"MASKED"}, {"HORIZONTAL", "EVSM"}, {"VERTICAL", "EVSM"} };
		for (uint32_t i = 0u; i < state.m_shaders.size(); ++i)
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfos[i < 4u ? 0u : i == 4u ? 1u : 2u]->GetFileId(), state.m_shaders[i], defines[i]);
		state.m_load = App::GetSubmodule<TextureImporter>()->LoadTexture(state.m_info->GetFileId(), state.m_texture);
		return {};
	}
}

ShadowDrawCompletionTestComponent::~ShadowDrawCompletionTestComponent() = default;

void ShadowDrawCompletionTestComponent::Tick(float)
{
	if (IsFinished()) return;
	if (!m_state)
	{
		m_state = TSharedPtr<ShadowDrawCompletionState>::Make();
		if (auto error = CreateAssets(*m_state, GetTestRunId()); !error.empty())
		{
			std::error_code cleanup;
			if (!m_state->m_ownedDirectory.empty()) std::filesystem::remove_all(m_state->m_ownedDirectory, cleanup);
			MarkFailed(error);
			return;
		}
	}
	if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 90000)
	{
		MarkFailed("shadow completion timed out; unfinished jobs retain private sources at " + m_state->m_ownedDirectory.string());
		return;
	}
	auto& state = *m_state;
	auto* importer = App::GetSubmodule<TextureImporter>();
	using Phase = ShadowDrawCompletionState::Phase;
	if (state.m_phase == Phase::Loading)
	{
		for (const auto& shader : state.m_shaders) if (!shader || !shader->IsReady()) return;
		if (!state.m_load || !state.m_load->IsFinished()) return;
		if (!state.m_texture) { MarkFailed("private texture load failed; source retained at " + state.m_ownedDirectory.string()); return; }
		if (!state.m_texture->IsReady()) return;
		state.m_index = static_cast<uint32_t>(importer->GetTextureIndex(state.m_info->GetFileId()));
		if (!state.m_index || state.m_index >= TextureImporter::MaxTexturesInScene) { MarkFailed("private texture has no unique sampler slot"); return; }
		state.m_sourceA = importer->GetTextureSamplersSnapshot({ 0u, state.m_index });
		state.m_phase = Phase::Warm;
		m_validation = Tasks::CreateTaskWithResult<std::string>("Warm independent shadow nodes", [hold = m_state]() { return Warm(*hold); }, EThreadType::Render);
		m_validation->Run();
		return;
	}
	if (state.m_phase == Phase::Reload)
	{
		const auto current = importer->GetTextureSamplersSnapshot({ 0u, state.m_index });
		if (!current.m_slots[1].m_texture || !current.m_slots[1].m_texture->IsReady() ||
			current.m_slots[1].m_texture == state.m_sourceA.m_slots[1].m_texture ||
			current.m_slots[1].m_contentRevision == state.m_sourceA.m_slots[1].m_contentRevision) return;
		state.m_sourceB = current;
		state.m_phase = Phase::Validate;
		m_validation = Tasks::CreateTaskWithResult<std::string>("Validate shadow draw completion", [hold = m_state]()
		{
			if (auto error = ValidateBlurRadius(*hold); !error.empty()) return error;
			if (auto error = ValidateColdPublication(*hold); !error.empty()) return error;
#if defined(__APPLE__)
			if (auto error = ValidateLighting(*hold); !error.empty()) return error;
			if (auto error = ValidateDependencies(*hold); !error.empty()) return error;
#endif
			return ValidateBlur(*hold);
		}, EThreadType::Render);
		m_validation->Run();
		return;
	}
	if (!m_validation || !m_validation->IsFinished()) return;
	std::string error = m_validation->GetResult();
	if (error.empty() && state.m_phase == Phase::Warm)
	{
		error = WriteTexture(state.m_ownedDirectory / "requested.tga", 210u);
		if (error.empty())
		{
			state.m_phase = Phase::Reload;
			importer->OnUpdateAssetInfo(state.m_info, true);
			return;
		}
	}
	std::error_code cleanup;
	std::filesystem::remove_all(state.m_ownedDirectory, cleanup);
	if (cleanup) error += "; private source cleanup failed: " + cleanup.message();
	if (!error.empty()) { MarkFailed(error); return; }
#if defined(__APPLE__)
	AddJournalEvent("ShadowDrawCompletionEvidence", "Real Lighting A/B/C/D: late rejected recording retracts payload success, identical C retries, D reuses; real TexA-to-TexB reload produced Prepare-false drawable fallback D/O/N: 4 runs/candidates and false/true/true -> false/false/true; late dependency rejection: 2 draws, corrected retry: 4");
#else
	AddJournalEvent("ShadowDrawCompletionEvidence", "Dense-cache Lighting/dependency failures are macOS-only and were not executed with this platform's global sampler set");
#endif
	AddJournalEvent("ShadowBlurEvidence", "Actual empty EVSM Prepare/Process: horizontal reject 0, vertical reject 1, each restored retry 2 blur draws; all 1024 float pixels checked per submission with five-second fence bounds");
	AddJournalEvent("ShadowBlurRadiusEvidence", "Command recorder: zero/threshold EVSM and PCF record zero blur draws and create no blur resources; positive EVSM records two. Cold/empty/warm submissions, single-lobe/fractional radii, all 1024 reverse-Z clear pixels and isolated 8x8 atlas tile verified");
	AddJournalEvent("ShadowBlurPublicationEvidence", "Own unused buffer 31 caused actual H sampler producer refusal: A32 retained sampler/view/native/revision/hash, B16 recorded zero with failed token and all 256 clear pixels; exact range restoration retried B with two draws and all 256 blurred pixels, retaining native A; no V-only producer-failure coverage");
	AddJournalEvent("ShadowColdPublicationEvidence", "Preloaded-shader owner latch completed/reused template plus H/V and flight UBO; real reflected Storage-vs-Uniform rejected template/fresh-flight candidates, each retained node publication and allowed one independent PCF caster draw; same-owner retries accepted two normal blur draws plus PCF and all 1024 EVSM pixels; real SSBO 1/3/3 storage+candidates grew then reused one view, retaining the original pair; positive SSBO coverage is not failure rollback proof");
	AddJournalEvent("ShadowDrawCompletionScope", "Recorded candidates, existing completion tokens and blur pixels; not CSM atlas/matrix atomic publication, visual quality, performance or Windows GPU coverage");
	MarkPassed();
}
