#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/GlobalIllumination/GIProbesImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
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
#include <fstream>
#include <functional>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace Sailor;

namespace Sailor::Tests
{
	void RequireRejectedComputeSubmission(const std::function<bool()>& submit);
}

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

	class ReloadTaskProbe final : public IAssetInfoHandlerListener
	{
	public:
		ReloadTaskProbe(AssetInfo& info) : m_handler(*info.GetHandler()), m_fileId(info.GetFileId())
		{
			m_handler.Subscribe(this);
		}
		~ReloadTaskProbe()
		{
			m_handler.Unsubscribe(this);
		}

		void OnImportAsset(AssetInfoPtr) override {}
		void OnUpdateAssetInfo(AssetInfoPtr info, bool bExpired) override
		{
			if (!bExpired || info->GetFileId() != m_fileId)
			{
				return;
			}
			++m_notifications;
			m_bWaitedForPrevious = m_previousFinished.load();
			auto worker = Tasks::CreateTask("Reload test: worker", []() {});
			auto render = worker->Then([]() {}, "Reload test: render", EThreadType::Render);
			auto rhi = render->Then([]() {}, "Reload test: RHI", EThreadType::RHI);
			m_completion = rhi->Then([this]()
				{
					m_afterEntered = true;
					m_releaseAfter.wait();
					m_completed = true;
				}, "Reload test: final worker", EThreadType::Worker);
			worker->Run();
		}

		void Update()
		{
			std::latch entered(1), release(1);
			auto previous = Tasks::CreateTask("Reload test: previous render reader", [&]()
				{
					entered.count_down();
					release.wait();
					m_previousFinished = true;
				}, EThreadType::Render);
			previous->Run();
			entered.wait();
			std::jthread unblock([&]()
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					release.count_down();
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
					while (!m_afterEntered.load() && std::chrono::steady_clock::now() < deadline)
					{
						std::this_thread::yield();
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					m_releaseAfter.count_down();
				});
			const bool bUpdated = App::UpdateAsset(m_fileId.ToString().c_str());
			const bool bCompletedOnReturn = m_completed.load();
			unblock.join();
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(
				{ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
			Require(bUpdated && m_notifications == 1, "targeted update must notify the changed asset once");
			Require(m_bWaitedForPrevious && bCompletedOnReturn,
				"targeted update must fence previous readers and finish its cross-queue publication before returning");
		}

		uint32_t m_notifications = 0;

	private:
		IAssetInfoHandler& m_handler;
		FileId m_fileId;
		Tasks::ITaskPtr m_completion;
		std::atomic<bool> m_previousFinished{ false }, m_afterEntered{ false }, m_completed{ false };
		std::latch m_releaseAfter{ 1 };
		bool m_bWaitedForPrevious = false;
	};

	void TestTargetedAssetCapture(const std::filesystem::path& workspace)
	{
		GIWorld world;
		world.WaitReady();
		auto* registry = App::GetSubmodule<AssetRegistry>();
		auto* textures = App::GetSubmodule<TextureImporter>();
		const auto material = world.m_material;
		auto* materialInfo = registry->GetAssetInfoPtr(material->GetFileId());
		const auto materialPath = std::filesystem::path(materialInfo->GetAssetFilepath());
		Require(std::filesystem::canonical(materialPath).generic_string().starts_with(
			std::filesystem::canonical(workspace / "Content").generic_string() + "/"),
			"reload fixture may only change its temporary workspace material");
		const auto oldScene = GlobalIlluminationECSTestAccess::CapturedScene(world.GI());
		const auto oldEmission = oldScene->m_materials[0]->m_parameters.m_emissiveFactor;

		const auto imagePath = workspace / "Content/Reload.tga";
		auto writeTexture = [&](uint8_t red, uint8_t blue)
		{
			std::array<uint8_t, 21> bytes{};
			bytes[2] = 2;
			bytes[12] = bytes[14] = 1;
			bytes[16] = 24;
			bytes[18] = blue;
			bytes[20] = red;
			std::ofstream output(imagePath, std::ios::binary);
			output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			Require(static_cast<bool>(output), "reload texture must be written");
		};
		writeTexture(255, 0);
		TextureAssetInfo textureInfo;
		auto textureMetadata = textureInfo.Serialize();
		const auto textureId = FileId::CreateNewFileId();
		textureMetadata["fileId"] = textureId;
		textureMetadata["filename"] = "Reload.tga";
		textureMetadata["bShouldKeepCpuBuffers"] = true;
		textureMetadata["bShouldGenerateMips"] = false;
		{
			std::ofstream metadata(imagePath.string() + ".asset");
			metadata << textureMetadata;
		}
		Require(registry->GetOrLoadFile(imagePath.string()) == textureId, "reload texture must register");
		TexturePtr texture;
		Require(textures->LoadTexture_Immediate(textureId, texture) && texture->HasCpuData(),
			"reload fixture needs a fully loaded CPU/GPU texture");
		const auto textureSlot = textures->GetTextureIndex(textureId);

		auto document = YAML::LoadFile(materialPath.string());
		auto vectors = document["uniformsVec4"].as<TMap<std::string, glm::vec4>>();
		auto scalars = document["uniformsFloat"].as<TMap<std::string, float>>();
		auto samplers = document["samplers"].as<TMap<std::string, FileId>>();
		vectors["material.emissiveFactor"] = glm::vec4(7, 3, 1, 0);
		scalars["material.alphaCutoff"] = 0.375f;
		samplers["baseColorSampler"] = textureId;
		document["uniformsVec4"] = vectors;
		document["uniformsFloat"] = scalars;
		document["samplers"] = samplers;
		{
			std::ofstream output(materialPath);
			output << document;
		}
		{
			ReloadTaskProbe publication(*materialInfo);
			publication.Update();
			const auto unchangedRevision = material->GetContentRevision();
			Require(App::UpdateAsset(material->GetFileId().ToString().c_str()) &&
				publication.m_notifications == 1 && material->GetContentRevision() == unchangedRevision,
				"an unchanged targeted update must not reload or revise the material");
		}
		Require(App::GetSubmodule<MaterialImporter>()->GetLoadedMaterial(material->GetFileId()) == material,
			"targeted reload must preserve material identity");
		const auto captured = Raytracing::PathTracer::CaptureMaterials({ material });
		const auto findTexture = [](const Raytracing::PathTracer::MaterialSnapshots& snapshots)
		{
			for (const auto& sampler : snapshots[0]->m_samplers)
			{
				if (sampler.m_first == "baseColorSampler") return sampler.m_second.m_texture;
			}
			return TSharedPtr<const Raytracing::PathTracer::TextureSnapshot>{};
		};
		const auto redTexture = findTexture(captured);
		Require(captured[0]->m_parameters.m_emissiveFactor == glm::vec3(7, 3, 1) &&
			captured[0]->m_parameters.m_alphaCutoff == 0.375f && redTexture && redTexture->m_data && redTexture->m_data->Num() == 4 &&
			(*redTexture->m_data)[0] == 255 && (*redTexture->m_data)[2] == 0,
			"GI must capture the completed emission, alpha and sampler update together");
		Require(oldScene->m_materials[0]->m_parameters.m_emissiveFactor == oldEmission,
			"in-flight GI must retain its old material values across reload");

		writeTexture(0, 255);
		{
			ReloadTaskProbe publication(*registry->GetAssetInfoPtr(textureId));
			publication.Update();
		}
		const auto updated = Raytracing::PathTracer::CaptureMaterials({ material });
		const auto blueTexture = findTexture(updated);
		Require(textures->GetLoadedTexture(textureId) == texture && textures->GetTextureIndex(textureId) == textureSlot &&
			blueTexture && blueTexture->m_data && (*blueTexture->m_data)[0] == 0 && (*blueTexture->m_data)[2] == 255 &&
			(*redTexture->m_data)[0] == 255 && (*redTexture->m_data)[2] == 0 &&
			updated[0]->m_contentRevision > captured[0]->m_contentRevision,
			"texture reload must finish dependent materials, preserve object/slot identity and retain old CPU pixels");
		Require(!App::UpdateAsset(nullptr) && !App::UpdateAsset("") &&
			!App::UpdateAsset(FileId::CreateNewFileId().ToString().c_str()),
			"invalid targeted updates must remain rejected");
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
		auto checkDistance = [](const GIProbesPreparedScenePtr& scene, float expected)
		{
			GIProbeBakeRaySample sample;
			std::string diagnostic;
			Require(scene->m_sampler->SampleVisibility(glm::vec3(0), glm::vec3(0, 0, -1), 20, 1, sample, diagnostic), diagnostic);
			Require(sample.m_bHit && std::abs(sample.m_distance - expected) < 1e-4f,
				"each prepared transport must trace its own static transforms");
		};
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
		Require(prepared->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0 &&
			prepared->m_sampler->GetLastScenePreparationStats().m_reusedBlasCount == 2 &&
			captured->m_instances[0].m_modelGeometry &&
			captured->m_instances[0].m_modelGeometry == captured->m_instances[1].m_modelGeometry,
			"the initial capture must share immutable model geometry and reuse its BLAS for both objects");
		checkDistance(prepared, 2);
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
		Require(recaptured->m_instances[0].m_modelGeometry == captured->m_instances[0].m_modelGeometry &&
			recaptured->m_materials[0] == captured->m_materials[0],
			"light-only owner capture must retain frozen model geometry and material snapshots");
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
			moved->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0 &&
			moved->m_sampler->GetLastScenePreparationStats().m_reusedBlasCount == 2,
			"a Static transform change must rebuild transport while reusing immutable model BLAS");
		checkDistance(moved, 4);
		checkDistance(prepared, 2);
		world.Step();
		world.WaitReady();
		world.m_material->SetUniform("material.baseColorFactor", glm::vec4(1, 1, 1, 0.5f));
		world.Step(0.6f);
		world.Step();
		const auto alpha = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(alpha && alpha->m_geometryHash != moved->m_geometryHash &&
			alpha->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0 &&
			alpha->m_sampler->GetLastScenePreparationStats().m_reusedBlasCount == 2,
			"material alpha edits must not use the light-only transport shortcut");
		const auto alphaCapture = GlobalIlluminationECSTestAccess::CapturedScene(world.GI());
		Require(alphaCapture->m_materials[0] != captured->m_materials[0] &&
			alphaCapture->m_materials[0]->m_parameters.m_baseColorFactor.a == 0.5f,
			"transport rebuilt for alpha must contain the new material snapshot");
		world.Step();
		world.WaitReady();
		Require(world.GI().RebuildRuntimeGIProbesScene(diagnostic), diagnostic);
		world.Step();
		const auto explicitRebuild = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(explicitRebuild && explicitRebuild != alpha && explicitRebuild->m_sampler != alpha->m_sampler &&
			GlobalIlluminationECSTestAccess::CapturedScene(world.GI()) != alphaCapture &&
			explicitRebuild->m_geometryHash == alpha->m_geometryHash &&
			explicitRebuild->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0 &&
			explicitRebuild->m_sampler->GetLastScenePreparationStats().m_reusedBlasCount == 2,
			"explicit rebuild must recapture and prepare transport without duplicating immutable model BLAS");
		checkDistance(explicitRebuild, 4);
	}

	void TestEmissionPreparation()
	{
		GIWorld world;
		world.m_material->SetUniform("material.emissiveFactor", glm::vec4(0));
		world.WaitReady();
		const auto original = GlobalIlluminationECSTestAccess::PreparedScene(world.GI());
		const auto captured = GlobalIlluminationECSTestAccess::CapturedScene(world.GI());
		world.m_material->SetUniform("material.emissiveFactor", glm::vec4(2, 4, 8, 0));
		world.Step(0.6f);
		world.Step();
		const auto updated = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		const auto recaptured = GlobalIlluminationECSTestAccess::CapturedScene(world.GI());
		Require(updated && updated->m_geometryHash == original->m_geometryHash &&
			updated->m_lightingHash != original->m_lightingHash &&
			updated->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0 &&
			updated->m_sampler->GetLastScenePreparationStats().m_decodedTextureCount == 0,
			"emission-only updates must change GI lighting without rebuilding geometry or decoding textures");
		Require(captured->m_instances[0].m_modelGeometry &&
			recaptured->m_instances[0].m_modelGeometry == captured->m_instances[0].m_modelGeometry &&
			recaptured->m_materials[0] != captured->m_materials[0] &&
			recaptured->m_materials[0]->m_parameters.m_emissiveFactor == glm::vec3(2, 4, 8) &&
			captured->m_materials[0]->m_parameters.m_emissiveFactor == glm::vec3(0),
			"emission capture must retain geometry without mutating previous material values");
		world.Step();
		world.WaitReady();
	}

	void TestEmissionDuringPreparation()
	{
		GIWorld world;
		world.m_material->SetUniform("material.emissiveFactor", glm::vec4(0));
		world.Step();
		const auto prepared = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(prepared && prepared->m_sampler, "prepare the real initial scene before changing emission");
		world.m_material->SetUniform("material.emissiveFactor", glm::vec4(3, 2, 1, 0));
		world.Step();
		Require(GlobalIlluminationECSTestAccess::PreparedScene(world.GI()) == prepared,
			"emission changes must not discard usable geometry before its first publication");
		world.WaitReady();
		world.Step(0.6f);
		world.Step();
		const auto latest = GlobalIlluminationECSTestAccess::WaitPreparation(world.GI());
		Require(latest && latest->m_lightingHash != prepared->m_lightingHash &&
			latest->m_sampler->GetLastScenePreparationStats().m_builtBlasCount == 0,
			"after first publication the next emission generation must catch up without rebuilding BLAS");
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
				uint32_t cameraCount = 1;
				float stateWeight = 1.0f;
				auto renderMode = ESceneViewRenderMode::Lit;
				bool giEnabled = true, rejectNextUpload = false;
				auto lights = TSharedPtr<TVector<RHILightShaderData>>::Make();
				lights->Add(RHILightShaderData{});
				(*lights)[0].m_intensity = glm::vec3(3, 5, 7);
				auto bones = TSharedPtr<TVector<glm::mat4>>::Make();
				bones->Add(glm::mat4(2.0f));
				uint64_t lightingRevision = 1, animationRevision = 1;
				auto checkBuffers = [&](const RHISceneViewSnapshot& snapshot, RHISemaphorePtr wait = {})
				{
					const auto& gi = *snapshot.m_globalIllumination;
					RHIGlobalIlluminationGpuLayout layout;
					TVector<RHIGlobalIlluminationGpuCoefficients> coefficients;
					TVector<RHIGlobalIlluminationGpuState> states;
					std::string diagnostic;
					Require(BuildGlobalIlluminationGpuLayout(*gi.m_layout, layout, diagnostic), diagnostic);
					Require(BuildGlobalIlluminationGpuCoefficients(gi, coefficients, diagnostic), diagnostic);
					Require(BuildGlobalIlluminationGpuStates(gi, states, diagnostic), diagnostic);
					const auto debug = snapshot.m_renderMode == ESceneViewRenderMode::GlobalIlluminationOnly ?
						EGlobalIlluminationDebugVisualization::IndirectOnly : EGlobalIlluminationDebugVisualization::Lit;
					const auto header = BuildGlobalIlluminationGpuHeader(&gi, debug,
						snapshot.m_globalIlluminationMode, snapshot.m_bGlobalIlluminationEnabled);
					const glm::mat4 identity(1.0f);
					const size_t numBones = snapshot.m_cpuBoneMatrices ? snapshot.m_cpuBoneMatrices->Num() : 0;
					const size_t numLights = snapshot.m_cpuLightsData ? snapshot.m_cpuLightsData->Num() : 0;
					struct Buffer
					{
						const char* name;
						const void* data;
						size_t size;
					};
					const std::array<Buffer, 11> expected{ {
						{ "globalIlluminationBvh", layout.m_nodes.GetData(), layout.m_nodes.Num() * sizeof(RHIGlobalIlluminationGpuBvhNode) },
						{ "globalIlluminationBricks", layout.m_bricks.GetData(), layout.m_bricks.Num() * sizeof(RHIGlobalIlluminationGpuBrick) },
						{ "globalIlluminationProbes", layout.m_probes.GetData(), layout.m_probes.Num() * sizeof(RHIGlobalIlluminationGpuProbe) },
						{ "globalIlluminationCoefficients", coefficients.GetData(), coefficients.Num() * sizeof(RHIGlobalIlluminationGpuCoefficients) },
						{ "globalIlluminationStates", states.GetData(), states.Num() * sizeof(RHIGlobalIlluminationGpuState) },
						{ "globalIlluminationHeader", &header, sizeof(header) },
						{ "light", numLights ? snapshot.m_cpuLightsData->GetData() : nullptr, numLights * sizeof(RHILightShaderData) },
						{ "bones", numBones ? snapshot.m_cpuBoneMatrices->GetData() : &identity, (std::max)(size_t{ 1 }, numBones) * sizeof(glm::mat4) },
						{ "lightsMatrices", snapshot.m_shadowMatrices.GetData(), snapshot.m_shadowMatrices.Num() * sizeof(glm::mat4) },
						{ "shadowIndices", snapshot.m_shadowIndices.GetData(), snapshot.m_shadowIndices.Num() * sizeof(uint32_t) },
						{ "shadowAtlasTiles", snapshot.m_shadowAtlasTiles.GetData(), snapshot.m_shadowAtlasTiles.Num() * sizeof(uint32_t) }
					} };
					std::array<RHIBufferPtr, 11> readbacks;
					auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(command, true);
					command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
					for (size_t i = 0; i < expected.size(); ++i)
					{
						const auto& buffer = expected[i];
						auto bindings = std::string_view(buffer.name) == "bones" ? snapshot.m_boneMatrices : snapshot.m_rhiLightsData;
						auto binding = bindings->GetOrAddShaderBinding(buffer.name);
						Require(binding && binding->m_vulkan.m_valueBinding, "framegraph must publish each shared and per-view buffer");
						if (buffer.size == 0) continue;
						readbacks[i] = driver->CreateBuffer(buffer.size, EBufferUsageBit::BufferTransferDst_Bit,
							EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
						command->m_vulkan.m_commandBuffer->CopyBuffer(*binding->m_vulkan.m_valueBinding->Get(),
							*readbacks[i]->m_vulkan.m_buffer->Get(), buffer.size);
					}
					command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
					commands->EndCommandList(command);
					auto fence = RHIFencePtr::Make();
					Require(driver->SubmitCommandList(command, fence, {}, wait), "shared resource readback submission must succeed");
					fence->Wait(5000000000ull);
					Require(fence->IsFinished(), "shared resource readback must complete");
					for (size_t i = 0; i < expected.size(); ++i)
					{
						Require(expected[i].size == 0 || std::memcmp(readbacks[i]->GetPointer(), expected[i].data, expected[i].size) == 0,
							std::string("GPU bytes must match the retained publication: ") + expected[i].name);
					}
					fence->ClearDependencies();
				};
				auto upload = [&](uint32_t flight, const GIProbesDataPtr& payload, uint64_t revision,
					uint64_t expectedBytes, bool expectSharedUpload = true)
				{
					flights[flight]->BeginSubmission(++submission, flight);
					auto view = RHISceneViewPtr::Make();
					auto gi = RHIGlobalIlluminationSnapshotPtr::Make();
					gi->m_generation = revision;
					gi->m_lightingHash = payload->m_lightingHash;
					gi->m_layout = payload;
					gi->m_qualityBudget = 1;
					RHIGlobalIlluminationState state;
					state.m_data = payload;
					state.m_effectiveWeight = stateWeight;
					gi->m_states.Add(state);
					view->m_snapshots.Resize(cameraCount);
					for (uint32_t camera = 0; camera < cameraCount; ++camera)
					{
						auto& snapshot = view->m_snapshots[camera];
						snapshot.m_submissionContext = flights[flight];
						snapshot.m_cameraIndex = camera;
						snapshot.m_camera = TUniquePtr<CameraData>::Make();
						snapshot.m_globalIlluminationMode = EGlobalIlluminationMode::Runtime;
						snapshot.m_bGlobalIlluminationEnabled = giEnabled;
						snapshot.m_globalIllumination = gi;
						snapshot.m_renderMode = renderMode;
						snapshot.m_cpuLightsData = lights;
						snapshot.m_lightingRevision = lightingRevision;
						snapshot.m_cpuBoneMatrices = bones;
						snapshot.m_animationRevision = animationRevision;
						snapshot.m_shadowMatrices.Add(glm::mat4(float(camera + 1)));
						snapshot.m_shadowIndices.Add(camera + 1);
						snapshot.m_shadowAtlasTiles.Add(camera * 4);
					}
					TVector<RHICommandListPtr> transfers, graphics;
					RHISemaphorePtr input, chain;
					if (rejectNextUpload)
					{
						auto before = driver->CreateCommandList(false, ECommandListQueue::Graphics);
						commands->BeginCommandList(before, true);
						commands->EndCommandList(before);
						input = driver->CreateWaitSemaphore();
						Require(driver->SubmitCommandList(before, RHIFencePtr::Make(), input), "incoming upload dependency must submit");
						const auto oldStats = graph->GetGlobalIlluminationRenderStats();
						Tests::RequireRejectedComputeSubmission([&]() { return graph->Process(view, transfers, graphics, input, chain); });
						Require(chain == input && transfers.IsEmpty() && graphics.IsEmpty(),
							"rejected shared upload must preserve the incoming semaphore and stop before per-view work");
						Require(graph->GetGlobalIlluminationRenderStats().m_activeRevision == oldStats.m_activeRevision,
							"rejected shared upload must not publish its GI statistics");
						flights[flight]->InvalidateSubmissionResources();
						flights[flight]->BeginSubmission(++submission, flight);
						rejectNextUpload = false;
					}
					Require(graph->Process(view, transfers, graphics, input, chain), "the actual GI framegraph must process");
					Require((chain != input) == expectSharedUpload,
						"only changed shared payloads may add a submission, independently of camera count");
					Require(transfers.Num() == cameraCount && graphics.Num() == cameraCount,
						"every camera must retain its own transfer and graphics commands");
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
					auto& first = view->m_snapshots[0];
					for (auto& snapshot : view->m_snapshots)
					{
						Require(snapshot.m_boneMatrices == first.m_boneMatrices, "cameras must share one bones binding");
						for (const char* name : { "light", "globalIlluminationHeader", "globalIlluminationBvh",
							"globalIlluminationBricks", "globalIlluminationProbes", "globalIlluminationCoefficients", "globalIlluminationStates" })
						{
							Require(snapshot.m_rhiLightsData->GetOrAddShaderBinding(name)->m_vulkan.m_valueBinding ==
								first.m_rhiLightsData->GetOrAddShaderBinding(name)->m_vulkan.m_valueBinding,
								"camera bindings must reference the same shared GPU allocation");
						}
						if (snapshot.m_cameraIndex != 0)
						{
							Require(snapshot.m_frameBindings != first.m_frameBindings &&
								snapshot.m_rhiLightsData->GetOrAddShaderBinding("lightsMatrices")->m_vulkan.m_valueBinding !=
								first.m_rhiLightsData->GetOrAddShaderBinding("lightsMatrices")->m_vulkan.m_valueBinding,
								"camera frame and shadow allocations must remain independent");
						}
						checkBuffers(snapshot, chain);
						chain.Clear();
					}
					const auto stats = graph->GetGlobalIlluminationRenderStats();
					Require(stats.m_bActive == giEnabled && stats.m_flightSlot == flight, "GI activity and flight must match the submission");
					Require(stats.m_uploadedGpuBytes == expectedBytes && stats.m_copiedCpuBytes == expectedBytes,
						"GI uploaded " + std::to_string(stats.m_uploadedGpuBytes) + " bytes; expected " +
						std::to_string(expectedBytes) + " for changed payload ranges");
					return view;
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
				cameraCount = 3;
				auto otherFlight = upload(1, refined, 2, layoutBytes + lightingBytes);
				checkBuffers(olderFlight->m_snapshots[0]);
				upload(0, refined, 2, lightingBytes);
				auto stable = upload(0, refined, 2, 0, false);
				cameraCount = 1;
				auto fewerCameras = upload(0, refined, 2, 0, false);
				Require(stable->m_snapshots[0].m_rhiLightsData == fewerCameras->m_snapshots[0].m_rhiLightsData,
					"unchanged frames must reuse existing camera bindings");
				cameraCount = 4;
				upload(0, refined, 2, 0, false);
				auto changed = GIProbesDataPtr::Make(*refined);
				changed->m_probes[0].m_validity = changed->m_probes[0].m_validity < 0.5f ? 0.75f : 0.25f;
				Require(ComputeGIProbesTransportHash(*changed, changed->m_transportHash), "transport edit must hash");
				upload(0, changed, 3, layoutBytes + lightingBytes);
				checkBuffers(otherFlight->m_snapshots[0]);
				stateWeight = 0.5f;
				upload(0, changed, 3, sizeof(RHIGlobalIlluminationGpuState));
				renderMode = ESceneViewRenderMode::GlobalIlluminationOnly;
				upload(0, changed, 3, sizeof(RHIGlobalIlluminationGpuHeader));
				giEnabled = false;
				upload(0, changed, 3, sizeof(RHIGlobalIlluminationGpuHeader));
				giEnabled = true;
				upload(0, changed, 3, sizeof(RHIGlobalIlluminationGpuHeader));

				(*lights)[0].m_intensity = glm::vec3(11, 13, 17);
				++lightingRevision;
				upload(0, changed, 3, 0);
				(*bones)[0] = glm::mat4(3.0f);
				++animationRevision;
				upload(0, changed, 3, 0);
				lights = TSharedPtr<TVector<RHILightShaderData>>::Make(*lights);
				(*lights)[0].m_intensity = glm::vec3(19, 23, 29);
				bones = TSharedPtr<TVector<glm::mat4>>::Make(*bones);
				(*bones)[0] = glm::mat4(4.0f);
				upload(0, changed, 3, 0);
				lights->Resize(17);
				bones->Resize(17);
				for (size_t i = 0; i < 17; ++i)
				{
					(*lights)[i].m_intensity = glm::vec3(float(i + 2));
					(*bones)[i] = glm::mat4(float(i + 3));
				}
				++lightingRevision;
				++animationRevision;
				upload(0, changed, 3, 0);
				upload(0, changed, 3, 0, false);
				lights->Clear();
				bones.Clear();
				++lightingRevision;
				++animationRevision;
				upload(0, changed, 3, 0);
				upload(0, changed, 3, 0, false);
				lights->Add(RHILightShaderData{});
				(*lights)[0].m_intensity = glm::vec3(31, 37, 41);
				bones = TSharedPtr<TVector<glm::mat4>>::Make();
				bones->Add(glm::mat4(5.0f));
				++lightingRevision;
				++animationRevision;
				rejectNextUpload = true;
				upload(0, changed, 4, layoutBytes + lightingBytes);
				upload(0, changed, 4, 0, false);

				const auto activeStats = graph->GetGlobalIlluminationRenderStats();
				auto otherGraph = RHIFrameGraphPtr::Make();
				auto emptyView = RHISceneViewPtr::Make();
				TVector<RHICommandListPtr> emptyTransfers, emptyGraphics;
				RHISemaphorePtr emptyChain;
				Require(otherGraph->Process(emptyView, emptyTransfers, emptyGraphics, {}, emptyChain),
					"a second graph must process an empty view");
				const auto retainedStats = graph->GetGlobalIlluminationRenderStats();
				Require(retainedStats.m_bActive && retainedStats.m_activeRevision == activeStats.m_activeRevision &&
					retainedStats.m_uploadedGpuBytes == activeStats.m_uploadedGpuBytes,
					"processing a second graph must not overwrite the first graph's GI statistics");
				Require(!otherGraph->GetGlobalIlluminationRenderStats().m_bActive,
					"an empty graph must not inherit another graph's active GI statistics");
				graph->Clear();
				Require(!graph->GetGlobalIlluminationRenderStats().m_bActive &&
					graph->GetGlobalIlluminationRenderStats().m_gpuAllocatedBytes == 0,
					"clearing a graph must reset its own GI statistics");
				std::cout << "FrameGraph shared uploads: 1-4 cameras, light/bone revisions, capacity growth, GI states/header, native rejection/retry and graph-owned statistics passed\n";
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
		run("Emission preparation", [&]() { TestEmissionPreparation(); });
		run("Emission during preparation", [&]() { TestEmissionDuringPreparation(); });
		run("Cloud-only changes", [&]() { TestCloudsDoNotInvalidateGI(); });
		if (data)
		{
			run("GI layout uploads", [&]() { TestGpuLayoutUploads(data, publishedBytes); });
			run("Importer retry", [&]() { TestImporterRetry(workspace, *data, false); });
			run("Importer retry after GC", [&]() { TestImporterRetry(workspace, *data, true); });
		}
		run("Targeted asset capture", [&]() { TestTargetedAssetCapture(workspace); });
		Require(failures.empty(), failures);
		std::cout << "GI restart, preparation recovery and importer retry tests passed\n";
	}
}
