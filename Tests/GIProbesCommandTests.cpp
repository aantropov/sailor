#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/GlobalIllumination/GIProbesImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/SkyComponent.h"
#include "ECS/GlobalIlluminationECS.h"
#include "ECS/LandscapeECS.h"
#include "ECS/LightingECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "GlobalIllumination/GIProbesBinary.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "RHI/CommandList.h"
#include "Settings/GraphicsSettings.h"

#include <array>
#include <barrier>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace Sailor;

namespace Sailor
{
	class GlobalIlluminationECSTestAccess
	{
	public:
		static bool FailCurrentPreparation(GlobalIlluminationECS& system)
		{
			if (!system.m_runtimeScenePreparationTask)
			{
				return false;
			}
			system.m_runtimeScenePreparationTask->Wait();
			auto result = system.m_runtimeScenePreparationTask->GetResult();
			if (!result.m_scene)
			{
				return false;
			}
			result.m_scene.Clear();
			result.m_diagnostic = "injected preparation failure";
			system.m_runtimeScenePreparationTask =
				Tasks::TaskPtr<GlobalIlluminationECS::RuntimeScenePreparationResult>::Make(std::move(result));
			return true;
		}

		static uint64_t PreparationCount(const GlobalIlluminationECS& system)
		{
			return system.m_runtimeScenePreparationRequestId;
		}

		static GIProbesPreparedScenePtr WaitPreparation(GlobalIlluminationECS& system)
		{
			if (!system.m_runtimeScenePreparationTask)
			{
				return {};
			}
			system.m_runtimeScenePreparationTask->Wait();
			return system.m_runtimeScenePreparationTask->GetResult().m_scene;
		}

		static GIProbesPreparedScenePtr PreparedScene(const GlobalIlluminationECS& system)
		{
			return system.m_runtimePreparedScene;
		}

		static GIProbesSceneSnapshotPtr CapturedScene(const GlobalIlluminationECS& system)
		{
			return system.m_runtimeSceneSnapshot;
		}
	};
}

namespace
{
	void Require(bool value, const std::string& message)
	{
		if (!value)
		{
			throw std::runtime_error(message);
		}
	}

	class GIWorld final : public World
	{
	public:
		GIWorld() : World("GI lifecycle", 0, CreateEcs())
		{
			GetECS<StaticMeshRendererECS>()->BeginPlay();
			Instantiate("Camera")->AddComponent<CameraComponent>();
			auto* registry = App::GetSubmodule<AssetRegistry>();
			const auto info = registry->GetAssetInfoPtr<ModelAssetInfoPtr>("Quad.gltf");
			ModelPtr model;
			Require(info && App::GetSubmodule<ModelImporter>()->LoadModel_Immediate(info->GetFileId(), model),
				"GI lifecycle fixture must load its real model");
			auto object = Instantiate("Static quad");
			object->SetMobilityType(EMobilityType::Static);
			auto renderer = object->AddComponent<MeshRendererComponent>();
			renderer->GetData().SetModel(model);
			TVector<MaterialPtr> materials;
			auto loadMaterials = App::GetSubmodule<ModelImporter>()->LoadDefaultMaterials(info->GetFileId(), materials);
			loadMaterials->Wait();
			Require(loadMaterials->GetResult() && !materials.IsEmpty() && materials[0]->IsReady(),
				"GI fixture must use a fully loaded material");
			m_material = materials[0];
			renderer->GetMaterials() = { m_material };
			object->GetTransformComponent().SetPosition(glm::vec3(0, 0, -2));
			auto back = Instantiate("Back quad");
			back->SetMobilityType(EMobilityType::Static);
			back->GetTransformComponent().SetPosition(glm::vec3(0, 0, -4));
			auto backRenderer = back->AddComponent<MeshRendererComponent>();
			backRenderer->GetData().SetModel(model);
			backRenderer->GetMaterials() = { m_material };
			GISettings settings;
			settings.m_mode = EGlobalIlluminationMode::Runtime;
			settings.m_runtimeProbes.m_bounceCount = 1;
			settings.m_runtimeProbes.m_minProbeSpacing = 8;
			settings.m_runtimeProbes.m_maxRayDistance = 20;
			settings.m_runtimeProbes.m_bIncludeSky = false;
			std::string diagnostic;
			Require(GI().ApplyWorldSettings(settings, diagnostic), diagnostic);
			Require(GI().SetRuntimeGIProbesPreviewEnabled(true, diagnostic), diagnostic);
			Require(GI().SetRuntimeGIProbesEditorBudget(Settings::ERuntimeGIProbesEditorBudget::Balanced, diagnostic), diagnostic);
		}
		~GIWorld() override { Clear(); }
		GlobalIlluminationECS& GI() { return *GetECS<GlobalIlluminationECS>(); }

		void Step(float elapsed = 1.0f / 60.0f)
		{
			++m_currentFrame;
			GetECS<TransformECS>()->Tick(elapsed);
			if (auto task = GetECS<StaticMeshRendererECS>()->Tick(elapsed))
			{
				task->Wait();
			}
			GetECS<CameraECS>()->Tick(elapsed);
			GI().Tick(elapsed);
		}

		void WaitReady(uint64_t afterRevision = 0)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			do
			{
				Step();
				const auto status = GI().GetRuntimeGIProbesStatus();
				if (status.m_lifecycle == ERuntimeGIProbesLifecycle::Ready &&
					status.m_refinement == 1.0f && status.m_publishedRevision > afterRevision && GI().GetActiveSnapshot())
				{
					return;
				}
				std::this_thread::yield();
			} while (std::chrono::steady_clock::now() < deadline);
			throw std::runtime_error("GI did not converge: " + GI().GetRuntimeGIProbesStatus().m_diagnostic);
		}

		MaterialPtr m_material;

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<StaticMeshRendererECS>::Make());
			systems.Add(TUniquePtr<CameraECS>::Make());
			systems.Add(TUniquePtr<LightingECS>::Make());
			systems.Add(TUniquePtr<LandscapeECS>::Make());
			systems.Add(TUniquePtr<GlobalIlluminationECS>::Make());
			return systems;
		}
	};

	void TestRestart(GIProbesDataPtr& data, uint64_t& publishedBytes)
	{
		GIWorld world;
		world.WaitReady();
		auto& gi = world.GI();
		const auto published = gi.GetActiveSnapshot();
		data = published->m_layout;
		const auto completed = gi.GetRuntimeGIProbesStatus();
		publishedBytes = completed.m_publishedBytes;
		Require(completed.m_activeProbeCount == 8 && completed.m_readyProbeCount == 8,
			"the ECS fixture must fully refine its eight real probes");
		gi.SetRuntimeGIProbesWorkAllowed(false);
		std::string diagnostic;
		Require(gi.SetRuntimeGIProbesEditorBudget(Settings::ERuntimeGIProbesEditorBudget::Eco, diagnostic), diagnostic);
		Require(gi.GetRuntimeGIProbesStatus().m_readyProbeCount == completed.m_readyProbeCount,
			"ordinary budget changes must retain compatible completed probes");
		Require(gi.RestartRuntimeGIProbes(diagnostic), diagnostic);
		Require(gi.GetRuntimeGIProbesStatus().m_readyProbeCount == 0 &&
			gi.GetRuntimeGIProbesStatus().m_refinement == 0.0f,
			"explicit ECS restart must discard refined samples even with identical inputs");
		Require(gi.GetActiveSnapshot() == published, "restart must retain the last good publication while warming");
		gi.SetRuntimeGIProbesWorkAllowed(true);
		world.WaitReady(completed.m_publishedRevision);
		Require(gi.GetActiveSnapshot() != published &&
			gi.GetRuntimeGIProbesStatus().m_publishedRevision > completed.m_publishedRevision,
			"the restarted solver must trace and publish a new result");
	}

	void TestContributorMaterialRevision()
	{
		GIWorld world;
		std::string diagnostic;
		Require(world.GI().SetRuntimeGIProbesPreviewEnabled(false, diagnostic), diagnostic);
		auto* meshes = world.GetECS<StaticMeshRendererECS>();
		float emission = 1.0f;
		for (const auto mobility : { EMobilityType::Static, EMobilityType::Stationary, EMobilityType::Dynamic })
		{
			for (auto object : world.GetGameObjects())
			{
				if (object->GetComponent<MeshRendererComponent>())
				{
					object->SetMobilityType(mobility);
				}
			}
			world.Step();
			const auto revision = meshes->GetGlobalIlluminationContributorRevision();
			const auto scene = meshes->GetRHIScene()->GetCurrentVersion();
			world.m_material->SetUniform("material.emissiveFactor", glm::vec4(emission++, 0, 0, 0));
			world.Step();
			Require((meshes->GetGlobalIlluminationContributorRevision() != revision) ==
				(mobility != EMobilityType::Dynamic),
				"only Static and Stationary uniform edits must invalidate GI");
			Require(meshes->GetRHIScene()->GetCurrentVersion() == scene,
				"uniform edits must preserve the published RHI scene for every mobility");
			const auto updatedRevision = meshes->GetGlobalIlluminationContributorRevision();
			world.Step();
			Require(meshes->GetGlobalIlluminationContributorRevision() == updatedRevision,
				"an unchanged material must not invalidate GI again");
		}
	}

	void TestPreparationRecovery(bool explicitRebuild)
	{
		GIWorld world;
		world.Step();
		auto& gi = world.GI();
		Require(GlobalIlluminationECSTestAccess::FailCurrentPreparation(gi),
			"the real initial preparation must complete before injecting its failed outcome");
		world.Step();
		Require(gi.GetRuntimeGIProbesStatus().m_lifecycle == ERuntimeGIProbesLifecycle::Failed && !gi.GetActiveSnapshot(),
			"a failed first preparation must expose failure without publishing a scene");
		const auto attempts = GlobalIlluminationECSTestAccess::PreparationCount(gi);
		for (int i = 0; i < 20; ++i)
		{
			world.Step(0.5f);
		}
		Require(GlobalIlluminationECSTestAccess::PreparationCount(gi) == attempts,
			"unchanged failed inputs must not trigger repeated expensive preparation");
		if (explicitRebuild)
		{
			std::string diagnostic;
			Require(gi.RebuildRuntimeGIProbesScene(diagnostic), diagnostic);
		}
		else
		{
			const auto before = world.GetECS<StaticMeshRendererECS>()->GetGlobalIlluminationContributorRevision();
			const auto scene = world.GetECS<StaticMeshRendererECS>()->GetRHIScene()->GetCurrentVersion();
			world.m_material->SetUniform("material.emissiveFactor", glm::vec4(0.5f, 0.25f, 0.125f, 0));
			world.Step();
			Require(world.GetECS<StaticMeshRendererECS>()->GetGlobalIlluminationContributorRevision() != before,
				"the material edit must reach the real scene revision publisher");
			Require(world.GetECS<StaticMeshRendererECS>()->GetRHIScene()->GetCurrentVersion() == scene,
				"uniform-only GI invalidation must not republish the RHI scene");
		}
		world.WaitReady();
		Require(GlobalIlluminationECSTestAccess::PreparationCount(gi) == attempts + 1,
			"changed inputs or explicit rebuild must retry the failed first preparation once");
	}

	void TestStalePreparationRecovery()
	{
		GIWorld world;
		world.Step();
		auto& gi = world.GI();
		Require(GlobalIlluminationECSTestAccess::FailCurrentPreparation(gi),
			"the initial preparation must complete before injecting its failed outcome");
		const auto attempts = GlobalIlluminationECSTestAccess::PreparationCount(gi);
		world.m_material->SetUniform("material.emissiveFactor", glm::vec4(0.75f, 0.5f, 0.25f, 0));
		world.Step();
		Require(gi.GetRuntimeGIProbesStatus().m_lifecycle != ERuntimeGIProbesLifecycle::Failed,
			"a failed result with changed inputs must request retry instead of a permanent failure");
		world.WaitReady();
		Require(GlobalIlluminationECSTestAccess::PreparationCount(gi) == attempts + 1,
			"a stale first preparation must recover once the scene becomes stable");
	}

	void TestLightingPreparation()
	{
		GIWorld world;
		auto sky = world.Instantiate("Sky")->AddComponent<SkyComponent>();
		auto settings = world.GI().GetWorldSettings();
		settings.m_runtimeProbes.m_bIncludeSky = true;
		std::string diagnostic;
		Require(world.GI().ApplyWorldSettings(settings, diagnostic), diagnostic);
		world.Step();
		const auto prepared = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(prepared && prepared->m_sampler, "the real background task must prepare the initial sky scene");
		sky->SetSunAngle(25.0f);
		world.Step();
		Require(GlobalIlluminationECSTestAccess::PreparedScene(world.GI()) == prepared,
			"a newer light state must not discard usable prepared geometry or starve the first solve");
		world.WaitReady();
		const auto captured = GlobalIlluminationECSTestAccess::CapturedScene(world.GI());
		const auto firstPublication = world.GI().GetActiveSnapshot();
		Require(prepared->m_sampler->GetLastScenePreparationStats().m_builtBlasCount > 0,
			"the initial capture must build real frozen geometry");
		world.GI().SetRuntimeGIProbesWorkAllowed(false);
		world.Step(0.6f);
		world.Step();
		const auto relit = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		const auto recaptured = GlobalIlluminationECSTestAccess::CapturedScene(world.GI());
		Require(relit && relit != prepared && relit->m_geometryHash == prepared->m_geometryHash &&
			relit->m_lightingHash != prepared->m_lightingHash &&
			relit->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0 &&
			relit->m_sampler->GetLastScenePreparationStats().m_decodedTextureCount == 0,
			"sun-only refresh must retain prepared geometry and skip BLAS and texture decoding");
		Require(recaptured->m_instances[0].m_triangles == captured->m_instances[0].m_triangles &&
			recaptured->m_materials[0] == captured->m_materials[0],
			"light-only owner capture must retain frozen triangles and material snapshots");
		sky->SetSunAngle(35.0f);
		world.Step();
		Require(GlobalIlluminationECSTestAccess::PreparedScene(world.GI()) == relit,
			"continued sun movement must not discard the light-only result either");
		const auto attempts = GlobalIlluminationECSTestAccess::PreparationCount(world.GI());
		for (uint32_t frame = 0; frame < 8; ++frame)
		{
			sky->SetGiIndirectIntensity(1.0f + 0.1f * frame);
			world.Step(0.6f);
		}
		Require(GlobalIlluminationECSTestAccess::PreparationCount(world.GI()) == attempts &&
			world.GI().GetActiveSnapshot() == firstPublication,
			"moving light must coalesce while a throttled replacement has not published");
		world.GI().SetRuntimeGIProbesWorkAllowed(true);
		const auto beforeRefresh = world.GI().GetRuntimeGIProbesStatus().m_publishedRevision;
		world.WaitReady(beforeRefresh);
		world.Step(0.6f);
		world.Step();
		const auto caughtUp = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(caughtUp && caughtUp->m_lightingHash != relit->m_lightingHash &&
			caughtUp->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0,
			"after publishing, the next light-only preparation must catch up to the latest sky");
		world.Step();
		world.WaitReady();

		for (auto object : world.GetGameObjects())
		{
			if (object->GetComponent<MeshRendererComponent>())
			{
				object->GetTransformComponent().SetPosition(glm::vec3(0, 0, -6));
				break;
			}
		}
		world.Step(0.6f);
		world.Step();
		const auto moved = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(moved && moved->m_geometryHash != caughtUp->m_geometryHash &&
			moved->m_sampler->GetLastScenePreparationStats().m_builtBlasCount > 0,
			"a Static transform change must rebuild the frozen transport");
		world.Step();
		world.WaitReady();
		world.m_material->SetUniform("material.baseColorFactor", glm::vec4(1, 1, 1, 0.5f));
		world.Step(0.6f);
		world.Step();
		const auto alpha = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(alpha && alpha->m_geometryHash != moved->m_geometryHash &&
			alpha->m_sampler->GetLastScenePreparationStats().m_builtBlasCount > 0,
			"material alpha edits must not use the light-only transport shortcut");
		world.Step();
		world.WaitReady();
		Require(world.GI().RebuildRuntimeGIProbesScene(diagnostic), diagnostic);
		world.Step();
		const auto explicitRebuild = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(explicitRebuild && explicitRebuild->m_sampler->GetLastScenePreparationStats().m_builtBlasCount > 0,
			"an explicit rebuild must bypass retained geometry even with unchanged identities");
	}

	void TestCloudsDoNotInvalidateGI()
	{
		GIWorld world;
		auto sky = world.Instantiate("Sky")->AddComponent<SkyComponent>();
		GIProbesSceneCaptureRequest request;
		GIProbesSceneRevision before, after;
		std::string diagnostic;
		Require(ObserveGIProbesSceneRevision(&world, request, before, diagnostic), diagnostic);
		sky->SetCloudsDensity(0.8f);
		sky->SetCloudsCoverage(0.2f);
		sky->SetCloudsHorizonBlend(2.0f);
		sky->SetSunShaftsIntensity(0.9f);
		Require(ObserveGIProbesSceneRevision(&world, request, after, diagnostic), diagnostic);
		Require(before == after, "clouds, fog and shafts absent from CPU clear sky must not invalidate GI");
		sky->SetSunAngle(25.0f);
		Require(ObserveGIProbesSceneRevision(&world, request, after, diagnostic), diagnostic);
		Require(before.m_geometry == after.m_geometry && before.m_lighting != after.m_lighting,
			"a changed sun must invalidate lighting without invalidating geometry");
	}

	void TestGpuLayoutUploads(const GIProbesDataPtr& data, uint64_t publishedBytes)
	{
		auto task = Tasks::CreateTaskWithResult<std::string>("GI layout upload validation", [data, publishedBytes]()
		{
			try
			{
				using namespace RHI;
				auto& driver = Renderer::GetDriver();
				auto commands = Renderer::GetDriverCommands();
				auto graph = RHIFrameGraphPtr::Make();
				std::array<RHIRenderSubmissionContextPtr, 2> flights{
					RHIRenderSubmissionContextPtr::Make(), RHIRenderSubmissionContextPtr::Make() };
				uint64_t submission = 0;
				auto checkBuffers = [&](RHIShaderBindingSetPtr bindings, const GIProbesDataPtr& expected,
					RHISemaphorePtr wait = {})
				{
					RHIGlobalIlluminationGpuLayout layout;
					std::string diagnostic;
					Require(BuildGlobalIlluminationGpuLayout(*expected, layout, diagnostic), diagnostic);
					RHIGlobalIlluminationSnapshot snapshot;
					snapshot.m_layout = expected;
					snapshot.m_qualityBudget = 1;
					RHIGlobalIlluminationState state;
					state.m_data = expected;
					state.m_effectiveWeight = 1;
					snapshot.m_states.Add(state);
					TVector<RHIGlobalIlluminationGpuCoefficients> coefficients;
					Require(BuildGlobalIlluminationGpuCoefficients(snapshot, coefficients, diagnostic), diagnostic);
					const std::array<const char*, 4> names{ "globalIlluminationBvh", "globalIlluminationBricks",
						"globalIlluminationProbes", "globalIlluminationCoefficients" };
					const std::array<const void*, 4> values{ layout.m_nodes.GetData(), layout.m_bricks.GetData(),
						layout.m_probes.GetData(), coefficients.GetData() };
					const std::array<size_t, 4> sizes{ layout.m_nodes.Num() * sizeof(RHIGlobalIlluminationGpuBvhNode),
						layout.m_bricks.Num() * sizeof(RHIGlobalIlluminationGpuBrick),
						layout.m_probes.Num() * sizeof(RHIGlobalIlluminationGpuProbe),
						coefficients.Num() * sizeof(RHIGlobalIlluminationGpuCoefficients) };
					std::array<RHIBufferPtr, 4> readbacks;
					auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(command, true);
					command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
					for (size_t i = 0; i < names.size(); ++i)
					{
						auto binding = bindings->GetOrAddShaderBinding(names[i]);
						Require(binding && binding->m_vulkan.m_valueBinding, "framegraph must publish each GI buffer");
						readbacks[i] = driver->CreateBuffer(sizes[i], EBufferUsageBit::BufferTransferDst_Bit,
							EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
						command->m_vulkan.m_commandBuffer->CopyBuffer(*binding->m_vulkan.m_valueBinding->Get(),
							*readbacks[i]->m_vulkan.m_buffer->Get(), sizes[i]);
					}
					command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
					commands->EndCommandList(command);
					auto fence = RHIFencePtr::Make();
					Require(driver->SubmitCommandList(command, fence, {}, wait), "GI readback submission must succeed");
					fence->Wait(5000000000ull);
					Require(fence->IsFinished(), "GI readback must complete");
					for (size_t i = 0; i < names.size(); ++i)
					{
						Require(std::memcmp(readbacks[i]->GetPointer(), values[i], sizes[i]) == 0,
							std::string("GPU GI bytes must match the retained publication: ") + names[i]);
					}
					fence->ClearDependencies();
				};
				auto upload = [&](uint32_t flight, const GIProbesDataPtr& payload, uint64_t revision, uint64_t expectedBytes)
				{
					flights[flight]->BeginSubmission(++submission, flight);
					auto view = RHISceneViewPtr::Make();
					view->m_snapshots.Resize(1);
					auto& snapshot = view->m_snapshots[0];
					snapshot.m_submissionContext = flights[flight];
					snapshot.m_camera = TUniquePtr<CameraData>::Make();
					snapshot.m_globalIlluminationMode = EGlobalIlluminationMode::Runtime;
					snapshot.m_bGlobalIlluminationEnabled = true;
					snapshot.m_globalIllumination = RHIGlobalIlluminationSnapshotPtr::Make();
					auto& gi = *snapshot.m_globalIllumination;
					gi.m_generation = revision;
					gi.m_lightingHash = payload->m_lightingHash;
					gi.m_layout = payload;
					gi.m_qualityBudget = 1;
					RHIGlobalIlluminationState state;
					state.m_data = payload;
					state.m_effectiveWeight = 1;
					gi.m_states.Add(state);
					TVector<RHICommandListPtr> transfers, graphics;
					RHISemaphorePtr chain;
					Require(graph->Process(view, transfers, graphics, {}, chain), "the actual GI framegraph must process");
					for (size_t i = 0; i < transfers.Num(); ++i)
					{
						for (const auto& command : { transfers[i], graphics[i] })
						{
							auto next = driver->CreateWaitSemaphore();
							Require(driver->SubmitCommandList(command, RHIFencePtr::Make(), next, chain),
								"framegraph commands must submit in dependency order");
							chain = next;
						}
					}
					checkBuffers(snapshot.m_rhiLightsData, payload, chain);
					const auto stats = graph->GetGlobalIlluminationRenderStats();
					Require(stats.m_bActive, "the submitted GI payload must stay active");
					Require(stats.m_uploadedGpuBytes == expectedBytes && stats.m_copiedCpuBytes == expectedBytes,
						"GI uploaded " + std::to_string(stats.m_uploadedGpuBytes) + " bytes; expected " +
						std::to_string(expectedBytes) + " for changed payload ranges");
					return snapshot.m_rhiLightsData;
				};
				Require(data->m_bricks.Num() == 1 && data->m_probes.Num() == 8, "GI upload fixture must have eight probes");
				const uint64_t layoutBytes = sizeof(RHIGlobalIlluminationGpuBvhNode) + sizeof(RHIGlobalIlluminationGpuBrick) +
					8 * sizeof(RHIGlobalIlluminationGpuProbe);
				const uint64_t lightingBytes = 8 * sizeof(RHIGlobalIlluminationGpuCoefficients) +
					sizeof(RHIGlobalIlluminationGpuState) + sizeof(RHIGlobalIlluminationGpuHeader);
				Require(publishedBytes == layoutBytes + lightingBytes,
					"runtime publication telemetry must match the actual complete GPU upload");
				auto olderFlight = upload(0, data, 1, layoutBytes + lightingBytes);
				auto refined = GIProbesDataPtr::Make(*data);
				++refined->m_lightingHash;
				for (auto& probe : refined->m_probes)
				{
					probe.m_irradiance[0] += glm::vec3(0.25f);
				}
				auto otherFlight = upload(1, refined, 2, layoutBytes + lightingBytes);
				checkBuffers(olderFlight, data);
				upload(0, refined, 2, lightingBytes);
				upload(0, refined, 2, 0);
				auto changed = GIProbesDataPtr::Make(*refined);
				changed->m_probes[0].m_validity = changed->m_probes[0].m_validity < 0.5f ? 0.75f : 0.25f;
				Require(ComputeGIProbesTransportHash(*changed, changed->m_transportHash), "transport edit must hash");
				upload(0, changed, 3, layoutBytes + lightingBytes);
				checkBuffers(otherFlight, refined);
				std::cout << "GI GPU layout uploads passed: full=" << layoutBytes + lightingBytes
					<< ", SH-only=" << lightingBytes << ", unchanged=0; both flights retain exact buffer contents\n";
				return std::string{};
			}
			catch (const std::exception& error)
			{
				return std::string(error.what());
			}
		}, EThreadType::RHI);
		task->Run();
		task->Wait();
		Require(task->GetResult().empty(), task->GetResult());
	}

	void TestImporterRetry(const std::filesystem::path& workspace, const GIProbesData& data, bool collectFailed)
	{
		auto* importer = App::GetSubmodule<GIProbesImporter>();
		const char* name = collectFailed ? "RetryAfterGc.probes" : "Retry.probes";
		const auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<GIProbesAssetInfoPtr>(name);
		Require(static_cast<bool>(info), "the broken probe fixture must be registered");
		const auto uid = info->GetFileId();
		GIProbesAssetPtr failed;
		Require(!importer->LoadGIProbes_Immediate(uid, failed) && failed && !failed->IsReady(),
			"the initial incomplete probe file must fail real I/O validation");
		const auto failedRevision = failed->GetRevision();
		if (collectFailed)
		{
			importer->CollectGarbage();
		}
		std::string diagnostic;
		Require(GIProbesBinary::SaveAtomic(workspace / "Content" / name, data, diagnostic), diagnostic);
		std::array<GIProbesAssetPtr, 8> assets;
		std::array<Tasks::TaskPtr<GIProbesAssetPtr>, 8> tasks;
		std::barrier gate(9);
		std::array<std::jthread, 8> callers;
		for (size_t i = 0; i < callers.size(); ++i)
		{
			callers[i] = std::jthread([&, i]()
			{
				gate.arrive_and_wait();
				tasks[i] = importer->LoadGIProbes(uid, assets[i]);
			});
		}
		gate.arrive_and_wait();
		for (auto& caller : callers)
		{
			caller.join();
		}
		for (size_t i = 0; i < tasks.size(); ++i)
		{
			Require(static_cast<bool>(tasks[i]), "retry must return a load task");
			tasks[i]->Wait();
			Require(assets[i] == failed && assets[i]->IsReady(),
				"repairing the same file without hot reload must make the retained asset ready");
			Require(tasks[i] == tasks[0], "concurrent requests must share the same import attempt");
		}
		Require(failed->GetRevision() == failedRevision + 1, "eight callers must import the repaired file only once");
		importer->RetainRuntimeGIProbes(uid);
		importer->RetainRuntimeGIProbes(uid);
		importer->ReleaseRuntimeGIProbes(uid);
		importer->CollectGarbage();
		GIProbesAssetPtr cached;
		Require(importer->LoadGIProbes_Immediate(uid, cached) && cached == failed && cached->GetRevision() == failedRevision + 1,
			"ready probes must remain deduplicated while one runtime binding retains them");
		importer->ReleaseRuntimeGIProbes(uid);
		Require(importer->LoadGIProbes_Immediate(uid, cached) && cached != failed && cached->GetRevision() == 1,
			"releasing the final binding must allow eviction and a fresh import");
	}
}

namespace Sailor::Tests
{
	void RunGIProbesCommandTests(const std::filesystem::path& workspace)
	{
		std::string failures;
		auto run = [&](const char* name, auto test)
		{
			try
			{
				test();
			}
			catch (const std::exception& error)
			{
				failures += std::string(name) + ": " + error.what() + '\n';
				std::cerr << name << ": " << error.what() << '\n';
			}
		};
		GIProbesDataPtr data;
		uint64_t publishedBytes = 0;
		run("Restart", [&]() { TestRestart(data, publishedBytes); });
		run("Material contributor revision", [&]() { TestContributorMaterialRevision(); });
		run("Changed-input recovery", [&]() { TestPreparationRecovery(false); });
		run("Explicit recovery", [&]() { TestPreparationRecovery(true); });
		run("Stale preparation recovery", [&]() { TestStalePreparationRecovery(); });
		run("Lighting preparation", [&]() { TestLightingPreparation(); });
		run("Cloud-only changes", [&]() { TestCloudsDoNotInvalidateGI(); });
		if (data)
		{
			run("GI layout uploads", [&]() { TestGpuLayoutUploads(data, publishedBytes); });
			run("Importer retry", [&]() { TestImporterRetry(workspace, *data, false); });
			run("Importer retry after GC", [&]() { TestImporterRetry(workspace, *data, true); });
		}
		Require(failures.empty(), failures);
		std::cout << "GI restart, preparation recovery and importer retry tests passed\n";
	}
}
