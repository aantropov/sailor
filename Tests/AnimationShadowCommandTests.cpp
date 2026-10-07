#include "AssetRegistry/Animation/AnimationImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "Components/AnimatorComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRendererComponent.h"
#include "ECS/TransformECS.h"
#include "Engine/EngineLoop.h"
#include "Engine/GameObject.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "RHI/Buffer.h"
#include "RHI/Fence.h"
#include "RHI/RenderTarget.h"
#include "RHI/Shader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	class AnimatedQuad final : public Model
	{
	public:
		AnimatedQuad() : Model(FileId::Invalid)
		{
			auto& driver = Renderer::GetDriver();
			std::array<VertexP3N3T3B3UV2C4I4W4, 4> vertices{};
			for (uint32_t i = 0; i < vertices.size(); ++i)
			{
				vertices[i].m_position = glm::vec3(i % 2 ? 0.1f : -0.1f, i / 2 ? 0.1f : -0.1f, -0.3f);
				vertices[i].m_color = glm::vec4(1);
				vertices[i].m_boneWeights = glm::vec4(1, 0, 0, 0);
			}
			const std::array<uint32_t, 12> indices{ 0, 1, 2, 2, 1, 3, 2, 1, 0, 3, 1, 2 };
			const auto memory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
			auto mesh = RHIMeshPtr::Make();
			mesh->m_vertexDescription = driver->GetOrAddVertexDescription<VertexP3N3T3B3UV2C4I4W4>();
			mesh->m_vertexBuffer = driver->CreateBuffer(sizeof(vertices), EBufferUsageBit::VertexBuffer_Bit, memory);
			mesh->m_indexBuffer = driver->CreateBuffer(sizeof(indices), EBufferUsageBit::IndexBuffer_Bit, memory);
			std::memcpy(mesh->m_vertexBuffer->GetPointer(), vertices.data(), sizeof(vertices));
			std::memcpy(mesh->m_indexBuffer->GetPointer(), indices.data(), sizeof(indices));
			mesh->m_vertexOffset = mesh->m_firstIndex = mesh->m_materialIndex = 0;
			mesh->m_indexCount = static_cast<uint32_t>(indices.size());
			mesh->m_bounds = Math::AABB(glm::vec3(0.1f, 0, -0.3f), glm::vec3(0.3f, 0.2f, 0.1f));
			m_meshes.Add(mesh);
			m_renderInstances.Add(RenderInstance{ 0, -1, glm::mat4(1) });
			m_boundsAabb = mesh->m_bounds;
			m_boundsSphere = m_boundsAabb.ToSphere();
			m_inverseBind.Add(glm::mat4(1));
			Flush();
		}
	};

	struct ShadowObservation
	{
		uint32_t m_flight = 0;
		uint64_t m_animationRevision = 0;
		TSharedPtr<const TVector<glm::mat4>> m_bones;
		TSharedPtr<const TVector<RHISceneVersionPtr>> m_sceneVersions;
		TVector<glm::mat4> m_lightMatrices;
		TVector<uint32_t> m_updates;
		TVector<uint32_t> m_animatedCascades;
		TVector<RHISubmissionCompletionTokenPtr> m_payloads;
		RHITexturePtr m_map;
		RHIBufferPtr m_pixels;
		RHIFencePtr m_completion;
	};

	class AnimationShadowNode final : public ShadowPrepassNode
	{
	public:
		void Process(RHIFrameGraphPtr graph, RHICommandListPtr upload, RHICommandListPtr draw,
			const RHISceneViewSnapshot& scene) override
		{
			ShadowPrepassNode::Process(graph, upload, draw, scene);
			ShadowObservation observation;
			observation.m_flight = scene.m_submissionContext->GetFlightSlot();
			observation.m_completion = scene.m_submissionContext->GetOrCreateFrameCompletion();
			observation.m_animationRevision = scene.m_animationRevision;
			observation.m_bones = scene.m_cpuBoneMatrices;
			observation.m_sceneVersions = scene.m_sceneVersions;
			observation.m_lightMatrices = scene.m_shadowMatrices;
			for (const auto& pass : scene.m_shadowMapsToUpdate)
			{
				observation.m_updates.Add(pass.m_lighMatrixIndex);
				observation.m_payloads.Add(pass.m_payloadCompletionToken);
				if (!pass.m_meshList.IsEmpty())
				{
					for (const auto& caster : pass.m_meshList)
						Require(caster.GetMobility() == EMobilityType::Stationary && caster.GetSkeletonOffset() == 0,
							"the shadow cache must consume the published stationary animated caster");
					observation.m_animatedCascades.Add(pass.m_lighMatrixIndex);
				}
			}
			RHIShaderBindingPtr maps;
			Require(scene.m_rhiLightsData && scene.m_rhiLightsData->GetShaderBindings().TryGet("shadowMaps"_h, maps),
				"the real lighting pass must publish shadow-map bindings");
			observation.m_map = maps->GetTextureBinding(0);
			Require(observation.m_map && observation.m_map->GetFormat() == EFormat::R16_UNORM,
				"the first cascade must use the production PCF map");
			const auto extent = observation.m_map->GetExtent();
			observation.m_pixels = Renderer::GetDriver()->CreateBuffer(size_t(extent.x) * extent.y * sizeof(uint16_t),
				EBufferUsageBit::BufferTransferDst_Bit, EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
			auto commands = Renderer::GetDriverCommands();
			commands->ImageMemoryBarrier(draw, observation.m_map, EImageLayout::TransferSrcOptimal);
			commands->CopyImageToBuffer(draw, observation.m_map, observation.m_pixels);
			commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			m_observation = std::move(observation);
			++m_frames;
		}

		ShadowObservation m_observation;
		uint32_t m_frames = 0;
	};
}

namespace Sailor::Tests
{
	void RunAnimationShadowCommandTests()
	{
		auto* engine = App::GetSubmodule<EngineLoop>();
		auto* renderer = App::GetSubmodule<Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		Require(engine->GetWorlds().IsEmpty() && renderer->EnsureFrameGraph(),
			"the animation shadow fixture follows the completed world-switch test");
		auto graph = renderer->GetFrameGraph()->GetRHI();
		const auto originalNodes = graph->GetGraph();
		auto world = engine->CreateEmptyWorld("Animation shadow cache", EngineLoop::DefaultWorldMask);
		TObjectPtr<AnimatedQuad> model;
		MaterialPtr material;
		AnimationPtr clip;
		auto node = TRefPtr<AnimationShadowNode>::Make();
		const auto cleanup = [&]
		{
			renderer->WaitIdle();
			graph->GetGraph() = originalNodes;
			node->Clear();
			node.Clear();
			engine->ExitWorld(world.GetRawPtr());
			engine->ProcessPendingWorldExits();
			if (model) model.DestroyObject(world->GetAllocator());
			if (material) material.DestroyObject(world->GetAllocator());
			if (clip) clip.DestroyObject(world->GetAllocator());
		};
		try
		{
			for (auto owner : world->GetGameObjects())
				if (auto camera = owner->GetComponent<CameraComponent>())
				{
					camera->SetZNear(0.01f);
					camera->SetZFar(20.0f);
					camera->SetFov(60.0f);
				}
			auto sunOwner = world->Instantiate("Fixed shadow sun");
			sunOwner->GetTransformComponent().SetRotation(glm::quat(glm::radians(glm::vec3(-35, 20, 0))));
			auto sun = sunOwner->AddComponent<LightComponent>();
			sun->SetLightType(ELightType::Directional);
			sun->SetShadowType(EShadowType::PCF);
			ShaderSetPtr shader;
			const auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/Unlit.shader");
			Require(info && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(info->GetFileId(), shader),
				"the caster material shader must load");
			material = MaterialPtr::Make(world->GetAllocator(), FileId::Invalid);
			material->SetShader(shader);
			material->SetRenderState(RenderState(true, true, 0, false, ECullMode::Back,
				EBlendMode::None, EFillMode::Fill, "Opaque"_h.GetHash()));
			material->UpdateRHIResource();
			model = TObjectPtr<AnimatedQuad>::Make(world->GetAllocator());
			Require(model->IsReady() && material->IsReady(), "the caster must have ready native geometry and material");
			auto owner = world->Instantiate("Stationary animated caster");
			owner->SetMobilityType(EMobilityType::Stationary);
			auto mesh = owner->AddComponent<MeshRendererComponent>();
			mesh->SetModel(model);
			mesh->GetMaterials().Add(material);
			clip = AnimationPtr::Make(world->GetAllocator(), FileId::Invalid);
			clip->m_numBones = 1;
			clip->m_numFrames = 2;
			clip->m_fps = clip->m_duration = 1;
			clip->m_skeletonSignature = clip->m_revision = 1;
			clip->m_frames.Resize(2);
			clip->m_frames[1].m_position.x = 0.2f;
			clip->m_restPose.Add(clip->m_frames[0]);
			clip->m_parentBoneIndices.Add(-1);
			auto animator = owner->AddComponent<AnimatorComponent>();
			animator->SetAnimation(clip);
			animator->Stop();
			node->SetTag("ShadowPrepass"_h);
			graph->GetGraph().Clear();
			graph->GetGraph().Add(node);
			std::map<uint32_t, ShadowObservation> flights;
			std::vector<uint16_t> pausedPixels;
			std::vector<uint16_t> resumedPixels;
			ShadowObservation paused;
			FrameState previous;
			uint32_t reused = 0, invalidated = 0;
			for (uint32_t frame = 0; frame < 24; ++frame)
			{
				if (frame == 12) animator->Play();
				if (frame == 14) animator->Stop();
				const auto time = previous.GetTime() + (frame == 12 ? 500 : frame == 13 ? 0 : 16);
				FrameState current(world.GetRawPtr(), time, {}, { 32, 24 }, frame ? &previous : nullptr);
				engine->ProcessCpuFrame(current);
				const auto version = world->GetECS<StaticMeshRendererECS>()->GetRHIScene()->GetCurrentVersion();
				Require(version->m_stationaryHandles && version->m_stationaryHandles->Num() == 1,
					"the mesh ECS must publish one stationary instance before the renderer copies it");
				current.GetDrawImGuiTask()->Wait();
				const auto before = node->m_frames;
				Require(renderer->PushFrame(current), "the renderer must accept the animation frame");
				scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
				scheduler->ProcessTasksOnMainThread();
				Require(node->m_frames == before + 1, "the shadow node must execute exactly once per world frame");
				const auto observed = node->m_observation;
				Require(observed.m_completion && observed.m_completion->Wait(5000000000ull) == EFenceStatus::Finished,
					"the actual submitted shadow draw and readback must finish");
				for (const auto& payload : observed.m_payloads)
					Require(payload && payload->IsSuccessful(), "the production shadow pass must complete its payload");
				Require(observed.m_bones && observed.m_bones->Num() == 1 &&
					std::abs((*observed.m_bones)[0][3].x - (frame < 12 ? 0.0f : 0.1f)) < 1e-6f,
					"the real renderer must publish the current animator pose to the shadow graph");
				const auto* pixels = static_cast<const uint16_t*>(observed.m_pixels->GetPointer());
				const auto pixelCount = observed.m_pixels->GetSize() / sizeof(uint16_t);
				Require(std::any_of(pixels, pixels + pixelCount, [](uint16_t value) { return value != 0; }),
					"the animated mesh must rasterize real nonempty PCF depth");
				if (frame == 0)
				{
					paused = observed;
					pausedPixels.assign(pixels, pixels + pixelCount);
				}
				Require(observed.m_animationRevision == paused.m_animationRevision + (frame < 12 ? 0u : 1u) &&
					(*paused.m_bones)[0][3].x == 0.0f && observed.m_lightMatrices == paused.m_lightMatrices &&
					*observed.m_sceneVersions == *paused.m_sceneVersions,
					"only the animation pose may change; retained palette, caster scene and light matrices must stay stable");
				Require(std::equal(pausedPixels.begin(), pausedPixels.end(), pixels) == (frame < 12),
					"resuming animation must change the shadow pixels without moving the object or light");
				if (frame == 12) resumedPixels.assign(pixels, pixels + pixelCount);
				if (frame > 12)
					Require(std::equal(resumedPixels.begin(), resumedPixels.end(), pixels),
						"zero elapsed time and pausing again must preserve the resumed shadow pixels");
				auto found = flights.find(observed.m_flight);
				if (found == flights.end())
				{
					Require(frame < 12 && observed.m_updates.Num() == App::GetActiveGraphicsSettings().m_shadowCascadeCount &&
						!observed.m_animatedCascades.IsEmpty(), "each cold flight must publish actual animated cascades");
					flights.emplace(observed.m_flight, observed);
				}
				else
				{
					auto& cached = found->second;
					Require(observed.m_map == cached.m_map, "the same flight must reuse its shadow allocation");
					if (observed.m_animationRevision == cached.m_animationRevision)
					{
						Require(observed.m_bones == cached.m_bones && observed.m_updates.IsEmpty(),
							"a paused pose must reuse its completed shadow cache without recording another cascade");
						++reused;
					}
					else
					{
						Require(observed.m_updates == cached.m_animatedCascades,
							"a new animation revision must update exactly the cached animated cascades");
						cached = observed;
						++invalidated;
					}
				}
				previous = std::move(current);
			}
			Require(flights.size() >= 2 && reused >= 16 && invalidated == flights.size(),
				"both paused poses must reuse warm flights, with one invalidation per flight after resume");
			std::cout << "Animation shadow cache: real World/PushFrame, 24 frames, " << flights.size()
				<< " flights, paused reuse, one pose invalidation, retained snapshots and changed PCF pixels passed\n";
		}
		catch (...)
		{
			cleanup();
			throw;
		}
		cleanup();
	}
}
