#include "Editor/GlobalIlluminationBakeController.h"

#include "AssetRegistry/AssetRegistry.h"
#include "GlobalIllumination/GIProbesBaker.h"
#include "GlobalIllumination/GIProbesBinary.h"
#include "GlobalIllumination/GIProbesScene.h"
#include "AssetRegistry/GlobalIllumination/GIProbesImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabAssetInfo.h"
#include "Core/LogMacros.h"
#include "Engine/World.h"
#include "Math/Math.h"
#include "Tasks/Scheduler.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>

using namespace Sailor;

namespace
{
	struct ProbeBakeScene final
	{
		GIProbesSceneSnapshotPtr m_snapshot{};
		GIProbesDataPtr m_layoutSource{};
		std::filesystem::path m_outputPath{};
		glm::vec3 m_volumeMin{};
		glm::vec3 m_volumeMax{};
	};

	void LogBakeWarning(const std::string& diagnostic)
	{
		if (!diagnostic.empty())
		{
			SAILOR_LOG("[Warning] GI bake: %s", diagnostic.c_str());
		}
	}

	void UpdateStatus(
		const TSharedPtr<GlobalIlluminationBakeController::SharedState>& state,
		const std::function<void(EditorGIProbesBakeStatus&)>& update)
	{
		state->m_lock.Lock();
		update(state->m_status);
		state->m_lock.Unlock();
	}

	void Fail(
		const TSharedPtr<GlobalIlluminationBakeController::SharedState>& state,
		std::string diagnostic)
	{
		UpdateStatus(
			state,
			[&diagnostic](EditorGIProbesBakeStatus& status)
			{
				status.m_state = EEditorGIProbesBakeState::Failed;
				status.m_stage = "Failed";
				status.m_diagnostic = std::move(diagnostic);
			});
	}

	void Cancelled(
		const TSharedPtr<GlobalIlluminationBakeController::SharedState>& state,
		std::string diagnostic)
	{
		UpdateStatus(
			state,
			[&diagnostic](EditorGIProbesBakeStatus& status)
			{
				status.m_state = EEditorGIProbesBakeState::Cancelled;
				status.m_stage = "Cancelled";
				status.m_diagnostic = std::move(diagnostic);
			});
	}

	bool IsProbesPath(const std::string& value)
	{
		std::string extension =
			std::filesystem::path(value).extension().string();
		std::transform(
			extension.begin(),
			extension.end(),
			extension.begin(),
			[](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
		return extension == ".probes";
	}

	bool IsEditorOnlyPrefab(const YAML::Node& prefab)
	{
		const YAML::Node components = prefab["components"];
		if (!components || !components.IsSequence())
		{
			return false;
		}
		for (const YAML::Node& component : components)
		{
			const YAML::Node typeName = component["typename"];
			if (typeName &&
				typeName.IsScalar() &&
				typeName.Scalar() == "Sailor::EditorComponent")
			{
				return true;
			}
		}
		return false;
	}

	YAML::Node MakeProbeBakeComparableWorldDocument(
		const YAML::Node& source)
	{
		YAML::Node result = YAML::Clone(source);
		const YAML::Node prefabs = result["prefabs"];
		if (!prefabs || !prefabs.IsSequence())
		{
			return result;
		}

		YAML::Node filteredPrefabs(YAML::NodeType::Sequence);
		for (const YAML::Node& prefab : prefabs)
		{
			if (!IsEditorOnlyPrefab(prefab))
			{
				filteredPrefabs.push_back(YAML::Clone(prefab));
			}
		}
		result["prefabs"] = std::move(filteredPrefabs);
		return result;
	}

	bool IsSavedWorldSnapshot(
		World* world,
		WorldPrefabAssetInfoPtr worldAssetInfo,
		std::string& outDiagnostic)
	{
		auto* worldImporter = App::GetSubmodule<WorldPrefabImporter>();
		if (!world || !worldAssetInfo || !worldImporter)
		{
			outDiagnostic =
				"the current world cannot be matched to a saved .world asset";
			return false;
		}

		std::string savedText;
		if (!AssetRegistry::ReadTextFile(
				worldAssetInfo->GetAssetFilepath(),
				savedText))
		{
			outDiagnostic = "the selected .world asset cannot be read";
			return false;
		}

		YAML::Node savedDocument;
		std::string yamlDiagnostic;
		if (!External::TryLoadYaml(
				savedText,
				savedDocument,
				yamlDiagnostic))
		{
			outDiagnostic = "the selected .world asset is invalid: " +
				yamlDiagnostic;
			return false;
		}

		WorldPrefabPtr savedWorld = worldImporter->Create();
		if (!savedWorld ||
			!External::GuardYamlExceptions(
				[&savedWorld, &savedDocument]()
				{
					savedWorld->Deserialize(savedDocument);
				},
				yamlDiagnostic) ||
			!savedWorld->IsReady())
		{
			if (yamlDiagnostic.empty() && savedWorld)
			{
				yamlDiagnostic = savedWorld->GetLoadDiagnostic();
			}
			outDiagnostic =
				"the selected .world asset cannot be normalized: " +
				(yamlDiagnostic.empty()
					? std::string("unknown world format error")
					: yamlDiagnostic);
			return false;
		}

		WorldPrefabPtr currentWorld = WorldPrefab::FromWorld(world);
		if (!currentWorld || !currentWorld->IsReady())
		{
			outDiagnostic =
				"the current world cannot be serialized for GI probe baking";
			if (currentWorld && !currentWorld->GetLoadDiagnostic().empty())
			{
				outDiagnostic += ": " + currentWorld->GetLoadDiagnostic();
			}
			return false;
		}

		if (!AreWorldDocumentsEquivalentForProbeBake(
				savedDocument,
				currentWorld->Serialize(),
				yamlDiagnostic))
		{
			outDiagnostic =
				yamlDiagnostic.empty()
					? "the current level has unsaved changes or does not match the selected .world asset; save it before baking"
					: yamlDiagnostic;
			return false;
		}
		return true;
	}

	bool GatherScene(
		World* world,
		const EditorGIProbesBakeRequest& request,
		ProbeBakeScene& scene,
		std::string& outDiagnostic)
	{
		auto* assetRegistry = App::GetSubmodule<AssetRegistry>();
		if (!world || !assetRegistry)
		{
			outDiagnostic = "a loaded Editor world and AssetRegistry are required";
			return false;
		}
		WorldPrefabAssetInfoPtr worldAssetInfo = request.m_worldAsset
			? assetRegistry->GetAssetInfoPtr<WorldPrefabAssetInfoPtr>(
				request.m_worldAsset)
			: nullptr;
		if (!worldAssetInfo)
		{
			outDiagnostic =
				"GI probe baking requires the current world to be saved as a registered .world asset";
			return false;
		}
		if (!IsSavedWorldSnapshot(world, worldAssetInfo, outDiagnostic))
		{
			return false;
		}
		if (request.m_stateName.empty())
		{
			outDiagnostic = "GI probe state name cannot be empty";
			return false;
		}
		if (!Math::AllFinite(request.m_fallbackEnvironment))
		{
			outDiagnostic =
				"the GI probe fallback environment must contain finite values";
			return false;
		}
		if (!IsProbesPath(request.m_outputVirtualPath) ||
			!assetRegistry->ResolveWorkspaceContentPathForWrite(
				request.m_outputVirtualPath,
				scene.m_outputPath))
		{
			outDiagnostic =
				"the bake output must be a safe workspace-relative .probes path";
			return false;
		}
		std::error_code fileError;
		if (!request.m_bOverwrite &&
			std::filesystem::exists(scene.m_outputPath, fileError) &&
			!fileError)
		{
			outDiagnostic = "the target .probes asset already exists";
			return false;
		}

		if (request.m_layoutSource)
		{
			auto* importer = App::GetSubmodule<GIProbesImporter>();
			GIProbesAssetPtr layoutAsset;
			if (!importer ||
				!importer->LoadGIProbes_Immediate(
					request.m_layoutSource,
					layoutAsset) ||
				!layoutAsset ||
				!layoutAsset->IsReady())
			{
				outDiagnostic =
					"the selected layout source is not a readable .probes asset";
				return false;
			}
			scene.m_layoutSource = layoutAsset->GetSnapshot().m_data;
			if (!scene.m_layoutSource)
			{
				outDiagnostic =
					"the selected layout source has no valid probe data";
				return false;
			}
		}

		GIProbesSceneCaptureRequest captureRequest;
		captureRequest.m_settings = request.m_settings;
		captureRequest.m_fallbackEnvironment = request.m_fallbackEnvironment;
		captureRequest.m_sourceIdentity = request.m_worldAsset.ToString();
		scene.m_snapshot = GIProbesSceneSnapshotPtr::Make();
		if (!CaptureGIProbesScene(
				world,
				captureRequest,
				*scene.m_snapshot,
				outDiagnostic,
				[](const std::string& warning)
				{
					LogBakeWarning(warning);
				}))
		{
			return false;
		}

		if (scene.m_layoutSource)
		{
			scene.m_volumeMin = scene.m_layoutSource->m_volumeMin;
			scene.m_volumeMax = scene.m_layoutSource->m_volumeMax;
		}
		else
		{
			const float padding = (std::max)(
				request.m_settings.m_minProbeSpacing,
				0.5f);
			scene.m_volumeMin =
				scene.m_snapshot->m_worldBounds.m_min - glm::vec3(padding);
			scene.m_volumeMax =
				scene.m_snapshot->m_worldBounds.m_max + glm::vec3(padding);
		}
		return true;
	}
}

bool Sailor::AreWorldDocumentsEquivalentForProbeBake(
	const YAML::Node& savedDocument,
	const YAML::Node& currentDocument,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
	std::string normalizedSavedWorld;
	std::string normalizedCurrentWorld;
	std::string yamlDiagnostic;
	if (!External::TryDumpYaml(
			MakeProbeBakeComparableWorldDocument(savedDocument),
			normalizedSavedWorld,
			yamlDiagnostic) ||
		!External::TryDumpYaml(
			MakeProbeBakeComparableWorldDocument(currentDocument),
			normalizedCurrentWorld,
			yamlDiagnostic))
	{
		outDiagnostic = "the current and saved worlds cannot be compared: " +
			yamlDiagnostic;
		return false;
	}
	if (normalizedSavedWorld != normalizedCurrentWorld)
	{
		outDiagnostic =
			"the current level has unsaved changes or does not match the selected .world asset; save it before baking";
		return false;
	}
	return true;
}

GlobalIlluminationBakeController::GlobalIlluminationBakeController() :
	m_state(TSharedPtr<SharedState>::Make())
{}

GlobalIlluminationBakeController::~GlobalIlluminationBakeController()
{
	std::string diagnostic;
	Cancel(diagnostic);
	Wait();
}

bool GlobalIlluminationBakeController::Start(
	World* world,
	const EditorGIProbesBakeRequest& request,
	std::string& outDiagnostic)
{
	if (GetStatus().IsRunning())
	{
		outDiagnostic = "a GI probe bake is already running";
		return false;
	}
	if (request.m_threadCount == 0u ||
		request.m_threadCount > GIProbesMaxBakeThreadCount)
	{
		outDiagnostic =
			"a GI probe bake requires a thread count between 1 and " +
			std::to_string(GIProbesMaxBakeThreadCount);
		return false;
	}

	auto state = TSharedPtr<SharedState>::Make();
	state->m_status.m_state = EEditorGIProbesBakeState::Preparing;
	state->m_status.m_stage = "Preparing immutable scene snapshot";
	state->m_status.m_outputVirtualPath = request.m_outputVirtualPath;
	m_state = state;
	m_task.Clear();

	auto scene = TSharedPtr<ProbeBakeScene>::Make();
	if (!GatherScene(world, request, *scene, outDiagnostic))
	{
		Fail(state, outDiagnostic);
		return false;
	}

	auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
	if (!scheduler)
	{
		outDiagnostic = "the task scheduler is unavailable";
		Fail(state, outDiagnostic);
		return false;
	}

	m_task = Tasks::CreateTask(
		"Bake adaptive irradiance GI probes",
		[state, scene, request]()
		{
			const auto started = std::chrono::steady_clock::now();
			try
			{
				if (state->m_cancel.load(std::memory_order_acquire))
				{
					Cancelled(state, "GI probe bake was cancelled before sampling");
					return;
				}
				UpdateStatus(
					state,
					[](EditorGIProbesBakeStatus& status)
					{
						status.m_state = EEditorGIProbesBakeState::Baking;
						status.m_stage = "Preparing CPU path tracer";
					});

				GIProbesBakeSettings effectiveSettings = request.m_settings;
				if (scene->m_layoutSource)
				{
					effectiveSettings.m_maxSubdivisionLevel =
						scene->m_layoutSource->m_bakeSettings.m_maxSubdivisionLevel;
					effectiveSettings.m_minProbeSpacing =
						scene->m_layoutSource->m_bakeSettings.m_minProbeSpacing;
					effectiveSettings.m_normalBias =
						scene->m_layoutSource->m_bakeSettings.m_normalBias;
					effectiveSettings.m_viewBias =
						scene->m_layoutSource->m_bakeSettings.m_viewBias;
					effectiveSettings.m_maxRayDistance =
						scene->m_layoutSource->m_bakeSettings.m_maxRayDistance;
				}
				GIProbesPreparedScene preparedScene;
				auto reportPreparation =
					[state, started](
						const Raytracing::PathTracer::ScenePreparationProgress&
							progress) -> bool
					{
						if (state->m_cancel.load(std::memory_order_acquire))
						{
							return false;
						}
						const bool bPreparingGeometry = progress.m_stage ==
							Raytracing::PathTracer::EScenePreparationStage::Geometry;
						const float stageFraction = progress.m_total > 0u ?
							static_cast<float>(progress.m_completed) /
								static_cast<float>(progress.m_total) : 1.0f;
						const float progressBase = bPreparingGeometry ? 0.0f : 0.06f;
						const float progressRange = bPreparingGeometry ? 0.06f : 0.04f;
						const std::string stage =
							(bPreparingGeometry ?
								"Preparing bake geometry (" :
								"Preparing bake materials (") +
							std::to_string(progress.m_completed) + "/" +
							std::to_string(progress.m_total) + ")";
						UpdateStatus(
							state,
							[&stage, started, progressBase, progressRange,
								stageFraction](EditorGIProbesBakeStatus& status)
							{
								status.m_state = EEditorGIProbesBakeState::Baking;
								status.m_progress = progressBase +
									progressRange * stageFraction;
								status.m_stage = stage;
								status.m_elapsedSeconds =
									std::chrono::duration<float>(
										std::chrono::steady_clock::now() - started)
										.count();
							});
						return !state->m_cancel.load(std::memory_order_acquire);
					};
				std::string preparationDiagnostic;
				if (!PrepareGIProbesScene(
						*scene->m_snapshot,
						effectiveSettings,
						&state->m_cancel,
						preparedScene,
						preparationDiagnostic,
						reportPreparation,
						[](const std::string& warning)
						{
							LogBakeWarning(warning);
						}))
				{
					if (state->m_cancel.load(std::memory_order_acquire))
					{
						Cancelled(state, std::move(preparationDiagnostic));
					}
					else
					{
						Fail(state, std::move(preparationDiagnostic));
					}
					return;
				}
				effectiveSettings = preparedScene.m_effectiveSettings;

				GIProbesBakeRequest bakeRequest;
				bakeRequest.m_stateName = request.m_stateName;
				bakeRequest.m_volumeMin = scene->m_volumeMin;
				bakeRequest.m_volumeMax = scene->m_volumeMax;
				bakeRequest.m_settings = effectiveSettings;
				bakeRequest.m_sceneGeometryBounds =
					scene->m_snapshot->m_geometryBounds;
				bakeRequest.m_sourceWorldHash =
					scene->m_snapshot->m_sourceWorldHash;
				bakeRequest.m_threadCount = request.m_threadCount;
				bakeRequest.m_layoutSource = scene->m_layoutSource.GetRawPtr();
				bakeRequest.m_cancel = &state->m_cancel;
				bakeRequest.m_progress =
					[state, started](const GIProbesBakeProgress& progress)
					{
						UpdateStatus(
							state,
							[&progress, started](EditorGIProbesBakeStatus& status)
							{
								status.m_state = EEditorGIProbesBakeState::Baking;
								status.m_progress = 0.1f +
									0.9f * progress.m_fraction;
								status.m_completedProbes = progress.m_completedProbes;
								status.m_totalProbes = progress.m_totalProbes;
								status.m_stage = progress.m_stage;
								status.m_elapsedSeconds =
									std::chrono::duration<float>(
										std::chrono::steady_clock::now() - started).count();
							});
					};

				GIProbesBakeResult result = GIProbesBaker::Bake(
					bakeRequest,
					*preparedScene.m_sampler);
				if (!result.IsSuccess())
				{
					if (result.m_status == EGIProbesBakeStatus::Cancelled ||
						state->m_cancel.load(std::memory_order_acquire))
					{
						Cancelled(state, std::move(result.m_diagnostic));
					}
					else
					{
						Fail(state, std::move(result.m_diagnostic));
					}
					return;
				}

				state->m_lock.Lock();
				if (state->m_cancel.load(std::memory_order_acquire))
				{
					state->m_status.m_state =
						EEditorGIProbesBakeState::Cancelled;
					state->m_status.m_stage = "Cancelled";
					state->m_status.m_diagnostic =
						"GI probe bake was cancelled before its atomic commit";
					state->m_lock.Unlock();
					return;
				}
				state->m_status.m_state = EEditorGIProbesBakeState::Saving;
				state->m_status.m_stage = "Saving one baked state atomically";
				state->m_lock.Unlock();
				std::string saveDiagnostic;
				if (!GIProbesBinary::SaveAtomic(
						scene->m_outputPath,
						*result.m_data,
						saveDiagnostic,
						request.m_bOverwrite))
				{
					Fail(state, std::move(saveDiagnostic));
					return;
				}

				UpdateStatus(
					state,
					[&result, &saveDiagnostic, started](
						EditorGIProbesBakeStatus& status)
					{
						status.m_state = EEditorGIProbesBakeState::Succeeded;
						status.m_progress = 1.0f;
						status.m_stage = "Completed";
						status.m_brickCount = static_cast<uint32_t>(
							result.m_data->m_bricks.Num());
						status.m_probeCount = static_cast<uint32_t>(
							result.m_data->m_probes.Num());
						status.m_layoutHash = result.m_data->m_layoutHash;
						status.m_transportHash = result.m_data->m_transportHash;
						status.m_lightingHash = result.m_data->m_lightingHash;
						status.m_elapsedSeconds =
							std::chrono::duration<float>(
								std::chrono::steady_clock::now() - started).count();
						status.m_diagnostic = std::move(saveDiagnostic);
					});
			}
			catch (const std::exception& exception)
			{
				Fail(
					state,
					std::string("GI probe bake failed: ") +
						exception.what());
			}
			catch (...)
			{
				Fail(state, "GI probe bake failed: unknown error");
			}
		},
		EThreadType::Background);
	scheduler->Run(m_task);
	outDiagnostic = "started background GI probe bake";
	return true;
}

bool GlobalIlluminationBakeController::Cancel(std::string& outDiagnostic)
{
	m_state->m_lock.Lock();
	const EEditorGIProbesBakeState state = m_state->m_status.m_state;
	if (state == EEditorGIProbesBakeState::Saving)
	{
		m_state->m_lock.Unlock();
		outDiagnostic =
			"the GI probe bake is already committing its output atomically";
		return false;
	}
	if (state != EEditorGIProbesBakeState::Preparing &&
		state != EEditorGIProbesBakeState::Baking)
	{
		m_state->m_lock.Unlock();
		outDiagnostic = "no GI probe bake is running";
		return false;
	}
	m_state->m_cancel.store(true, std::memory_order_release);
	m_state->m_lock.Unlock();
	outDiagnostic = "requested GI probe bake cancellation";
	return true;
}

EditorGIProbesBakeStatus
GlobalIlluminationBakeController::GetStatus() const
{
	if (!m_state)
	{
		return {};
	}
	m_state->m_lock.Lock();
	EditorGIProbesBakeStatus status = m_state->m_status;
	m_state->m_lock.Unlock();
	return status;
}

void GlobalIlluminationBakeController::Wait()
{
	if (m_task && !m_task->IsFinished())
	{
		m_task->Wait();
	}
}
