#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/GlobalIllumination/GIProbesImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRendererComponent.h"
#include "ECS/GlobalIlluminationECS.h"
#include "ECS/LandscapeECS.h"
#include "ECS/LightingECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "GlobalIllumination/GIProbesBinary.h"
#include "Settings/GraphicsSettings.h"

#include <array>
#include <barrier>
#include <chrono>
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

	void TestRestart(GIProbesDataPtr& data)
	{
		GIWorld world;
		world.WaitReady();
		auto& gi = world.GI();
		const auto published = gi.GetActiveSnapshot();
		data = published->m_layout;
		const auto completed = gi.GetRuntimeGIProbesStatus();
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
		run("Restart", [&]() { TestRestart(data); });
		run("Material contributor revision", [&]() { TestContributorMaterialRevision(); });
		run("Changed-input recovery", [&]() { TestPreparationRecovery(false); });
		run("Explicit recovery", [&]() { TestPreparationRecovery(true); });
		run("Stale preparation recovery", [&]() { TestStalePreparationRecovery(); });
		if (data)
		{
			run("Importer retry", [&]() { TestImporterRetry(workspace, *data, false); });
			run("Importer retry after GC", [&]() { TestImporterRetry(workspace, *data, true); });
		}
		Require(failures.empty(), failures);
		std::cout << "GI restart, preparation recovery and importer retry tests passed\n";
	}
}
