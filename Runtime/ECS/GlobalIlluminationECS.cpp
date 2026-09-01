#include "ECS/GlobalIlluminationECS.h"

#include "AssetRegistry/AssetRegistry.h"
#include "Core/LogMacros.h"
#include "ECS/CameraECS.h"
#include "Engine/World.h"
#include "RHI/DebugContext.h"
#include "Sailor.h"
#include "Settings/GraphicsSettings.h"
#include "Tasks/Tasks.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;

namespace
{
	GIProbesBakeSettings ResolveRuntimeBakeSettings(
		const RuntimeGIProbesSettings& settings,
		const RuntimeGIProbesQualitySettings& quality) noexcept
	{
		GIProbesBakeSettings result;
		result.m_raysPerProbe = quality.m_targetSamplesPerProbe;
		result.m_bounceCount = settings.m_bounceCount;
		result.m_randomSeed = 0u;
		result.m_maxSubdivisionLevel = 0u;
		result.m_minProbeSpacing = settings.m_minProbeSpacing *
			quality.m_spacingMultiplier;
		result.m_normalBias = settings.m_normalBias;
		result.m_viewBias = settings.m_viewBias;
		result.m_maxRayDistance = settings.m_maxRayDistance;
		result.m_bIncludeSky = settings.m_bIncludeSky;
		result.m_bIncludeEmissive = settings.m_bIncludeEmissive;
		result.m_bIncludeDirectLighting =
			settings.m_bIncludeDirectLighting;
		return result;
	}

	bool AreRuntimeQualitySettingsEqual(
		const RuntimeGIProbesQualitySettings& lhs,
		const RuntimeGIProbesQualitySettings& rhs) noexcept
	{
		return lhs.m_version == rhs.m_version &&
			lhs.m_maxActiveProbes == rhs.m_maxActiveProbes &&
			lhs.m_clipmapCascadeCount == rhs.m_clipmapCascadeCount &&
			lhs.m_initialSamplesPerProbe == rhs.m_initialSamplesPerProbe &&
			lhs.m_targetSamplesPerProbe == rhs.m_targetSamplesPerProbe &&
			lhs.m_workerCount == rhs.m_workerCount &&
			lhs.m_maxDirtyUploadBytesPerFrame ==
				rhs.m_maxDirtyUploadBytesPerFrame &&
			lhs.m_spacingMultiplier == rhs.m_spacingMultiplier &&
			lhs.m_cpuDutyFraction == rhs.m_cpuDutyFraction &&
			lhs.m_cpuBudgetMilliseconds == rhs.m_cpuBudgetMilliseconds &&
			lhs.m_maxPublicationsPerSecond ==
				rhs.m_maxPublicationsPerSecond &&
			lhs.m_initialPublicationCoverage ==
				rhs.m_initialPublicationCoverage &&
			lhs.m_bEnabled == rhs.m_bEnabled;
	}
}

void GlobalIlluminationECS::BeginPlay()
{
	if (App::IsEditorMode())
	{
		const auto& editorSettings = App::GetEditorGraphicsSettings();
		m_bRuntimePreviewEnabled =
			editorSettings.m_bRuntimeGIProbesPreviewEnabled;
		m_runtimeEditorBudget = editorSettings.m_runtimeGIProbesBudget;
	}
	InitializeFromWorld();
}

Tasks::ITaskPtr GlobalIlluminationECS::Tick(float deltaTime)
{
	InitializeFromWorld();
	const bool bEnabled = IsEnabled();
	if (bEnabled != m_bObservedEnabled)
	{
		m_bObservedEnabled = bEnabled;
		m_bCompositionDirty = true;
	}
	const uint32_t qualityBudget = GetMaxProbeStatesPerSnapshot();
	if (qualityBudget != m_observedQualityBudget)
	{
		m_observedQualityBudget = qualityBudget;
		m_bCompositionDirty = true;
		const GlobalIlluminationSnapshotPtr snapshot = GetActiveSnapshot();
		if (snapshot && snapshot->m_states.Num() > qualityBudget)
		{
			ClearActiveSnapshot();
		}
	}
	if (!bEnabled || !UsesBakedGlobalIllumination(m_worldSettings.m_mode))
	{
		if (m_worldSettings.m_probeSource ==
			EGlobalIlluminationProbeSource::RuntimeExperimental)
		{
			StopRuntimeProvider(true);
		}
		return nullptr;
	}
	if (m_worldSettings.m_probeSource ==
		EGlobalIlluminationProbeSource::RuntimeExperimental)
	{
		TickRuntimeProvider(deltaTime);
	}
	else
	{
		TickBakedProvider();
	}
	DrawDebugVisualization();
	return nullptr;
}

void GlobalIlluminationECS::EndPlay()
{
	StopRuntimeProvider(true);
	if (GIProbesImporter* importer =
		App::GetSubmodule<GIProbesImporter>())
	{
		for (const auto& entry : m_bindings)
		{
			RuntimeBinding& binding = *entry.m_second;
			if (binding.m_bRuntimeRetained)
			{
				importer->ReleaseRuntimeGIProbes(binding.m_assetId);
				binding.m_bRuntimeRetained = false;
			}
		}
	}
	m_bindings.Clear();
	ClearActiveSnapshot();
	m_bInitialized = false;
	m_bCompositionDirty = true;
	m_observedQualityBudget = (std::numeric_limits<uint32_t>::max)();
	m_bObservedEnabled = true;
	ECS::TSystem<GlobalIlluminationECS, GlobalIlluminationECSData>::EndPlay();
}

uint32_t GlobalIlluminationECS::GetMaxProbeStatesPerSnapshot() const noexcept
{
	return App::GetActiveGraphicsSettings().m_maxGiProbeStatesPerSnapshot;
}

bool GlobalIlluminationECS::IsEnabled() const noexcept
{
	return App::GetActiveGraphicsSettings().m_bEnableGlobalIllumination;
}

bool GlobalIlluminationECS::ApplyWorldSettings(
	const GISettings& settings,
	std::string& outDiagnostic)
{
	if (!settings.Validate(outDiagnostic))
	{
		return false;
	}

	const EGlobalIlluminationProbeSource previousSource =
		m_worldSettings.m_probeSource;
	const bool bUseBakedAssets = settings.m_probeSource ==
		EGlobalIlluminationProbeSource::BakedAssets;
	TMap<std::string, RuntimeBinding> nextBindings;
	for (const auto& entry : settings.m_probes)
	{
		const std::string& name = entry.m_first;
		const GlobalIlluminationProbeBinding& source = *entry.m_second;
		RuntimeBinding binding;
		binding.m_assetId = source.m_asset;
		binding.m_mode = source.m_mode;
		binding.m_weight = source.m_initialWeight;
		binding.m_bPreload = source.m_bPreload;

		RuntimeBinding* existing = nullptr;
		const bool bNeedsResident = bUseBakedAssets && (source.m_bPreload ||
			source.m_initialWeight > 0.0f);
		if (bNeedsResident && m_bindings.Find(name, existing) &&
			existing->m_assetId == source.m_asset)
		{
			binding.m_asset = existing->m_asset;
			binding.m_loadTask = existing->m_loadTask;
			binding.m_bRuntimeRetained = existing->m_bRuntimeRetained;
			binding.m_residency = existing->m_residency;
			binding.m_observedRevision = existing->m_observedRevision;
			binding.m_diagnostic = existing->m_diagnostic;
		}
		nextBindings.Insert(name, std::move(binding));
	}

	if (GIProbesImporter* importer =
		App::GetSubmodule<GIProbesImporter>())
	{
		for (const auto& entry : m_bindings)
		{
			const RuntimeBinding& existing = *entry.m_second;
			RuntimeBinding* replacement = nullptr;
			const bool bTransferred = nextBindings.Find(
				entry.m_first,
				replacement) &&
				replacement &&
				replacement->m_assetId == existing.m_assetId &&
				replacement->m_bRuntimeRetained;
			if (existing.m_bRuntimeRetained && !bTransferred)
			{
				importer->ReleaseRuntimeGIProbes(existing.m_assetId);
			}
		}
	}

	m_bindings = std::move(nextBindings);
	m_worldSettings = settings;
	m_bInitialized = true;
	m_bCompositionDirty = true;
	if (settings.m_probeSource ==
		EGlobalIlluminationProbeSource::RuntimeExperimental)
	{
		m_bRuntimeSceneRebuildRequested = true;
		m_bRuntimePreparationFailed = false;
		m_runtimePreparationRetrySeconds = 0.0f;
		m_runtimePreparationDiagnostic.clear();
		if (previousSource != settings.m_probeSource)
		{
			ClearActiveSnapshot();
			m_runtimePublishedRevision = 0u;
		}
	}
	else if (previousSource != settings.m_probeSource)
	{
		StopRuntimeProvider(true);
	}
	for (const auto& entry : m_bindings)
	{
		RuntimeBinding& binding = *entry.m_second;
		if (bUseBakedAssets && IsEnabled() &&
			UsesBakedGlobalIllumination(settings.m_mode) &&
			(binding.m_bPreload || binding.m_weight > 0.0f) &&
			!binding.m_asset)
		{
			std::string loadDiagnostic;
			StartLoad(entry.m_first, binding, loadDiagnostic);
		}
	}
	outDiagnostic = "updated Global Illumination ECS world bindings";
	return true;
}

bool GlobalIlluminationECS::SetProbeWeight(
	const std::string& name,
	float weight,
	std::string& outDiagnostic)
{
	TMap<std::string, float> weights;
	weights.Insert(name, weight);
	return SetProbeWeights(weights, outDiagnostic);
}

bool GlobalIlluminationECS::SetProbeWeights(
	const TMap<std::string, float>& weights,
	std::string& outDiagnostic)
{
	InitializeFromWorld();
	outDiagnostic.clear();
	for (const auto& entry : weights)
	{
		if (!m_bindings.ContainsKey(entry.m_first))
		{
			outDiagnostic = "unknown global-illumination probe state '" +
				entry.m_first + "'";
			return false;
		}
		if (!std::isfinite(*entry.m_second) || *entry.m_second < 0.0f)
		{
			outDiagnostic = "global-illumination probe state '" +
				entry.m_first + "' requires a finite non-negative weight";
			return false;
		}
	}

	const uint32_t budget = GetMaxProbeStatesPerSnapshot();
	const uint32_t requestedCount = CountPositiveWeights(&weights);
	if (requestedCount > budget)
	{
		outDiagnostic = "requested " + std::to_string(requestedCount) +
			" global-illumination probe states, but the active quality budget is " +
			std::to_string(budget) +
			"; weights and the last complete snapshot were preserved";
		m_rejectedCompositionCount.fetch_add(1u, std::memory_order_release);
		SetDiagnostic(outDiagnostic);
		return false;
	}

	for (const auto& entry : weights)
	{
		RuntimeBinding& binding = m_bindings[entry.m_first];
		if (binding.m_weight == *entry.m_second)
		{
			continue;
		}
		binding.m_weight = *entry.m_second;
		m_bCompositionDirty = true;
		if (m_worldSettings.m_probeSource ==
			EGlobalIlluminationProbeSource::BakedAssets &&
			binding.m_weight > 0.0f && !binding.m_asset)
		{
			std::string loadDiagnostic;
			StartLoad(entry.m_first, binding, loadDiagnostic);
		}
	}
	outDiagnostic = "accepted global-illumination probe weights";
	return true;
}

bool GlobalIlluminationECS::SetProbeMode(
	const std::string& name,
	EGlobalIlluminationProbeMode mode,
	std::string& outDiagnostic)
{
	InitializeFromWorld();
	outDiagnostic.clear();
	if (!m_bindings.ContainsKey(name))
	{
		outDiagnostic = "unknown global-illumination probe state '" + name + "'";
		return false;
	}
	RuntimeBinding& binding = m_bindings[name];
	if (binding.m_mode != mode)
	{
		binding.m_mode = mode;
		m_bCompositionDirty = true;
	}
	outDiagnostic = "updated global-illumination probe mode";
	return true;
}

bool GlobalIlluminationECS::PreloadProbe(
	const std::string& name,
	std::string& outDiagnostic)
{
	InitializeFromWorld();
	if (m_worldSettings.m_probeSource !=
		EGlobalIlluminationProbeSource::BakedAssets)
	{
		outDiagnostic =
			"baked probe assets are inactive while Runtime Experimental is selected";
		return false;
	}
	if (!m_bindings.ContainsKey(name))
	{
		outDiagnostic = "unknown global-illumination probe state '" + name + "'";
		return false;
	}
	RuntimeBinding& binding = m_bindings[name];
	binding.m_bPreload = true;
	return StartLoad(name, binding, outDiagnostic);
}

bool GlobalIlluminationECS::UnloadProbe(
	const std::string& name,
	std::string& outDiagnostic)
{
	InitializeFromWorld();
	if (!m_bindings.ContainsKey(name))
	{
		outDiagnostic = "unknown global-illumination probe state '" + name + "'";
		return false;
	}
	RuntimeBinding& binding = m_bindings[name];
	if (binding.m_weight > 0.0f)
	{
		outDiagnostic = "cannot unload active global-illumination probe state '" +
			name + "'; set its weight to zero first";
		return false;
	}
	binding.m_bPreload = false;
	if (binding.m_bRuntimeRetained)
	{
		if (GIProbesImporter* importer =
			App::GetSubmodule<GIProbesImporter>())
		{
			importer->ReleaseRuntimeGIProbes(binding.m_assetId);
		}
		binding.m_bRuntimeRetained = false;
	}
	binding.m_loadTask.Clear();
	binding.m_asset.Clear();
	binding.m_residency = EGlobalIlluminationProbeResidency::Unloaded;
	binding.m_observedRevision = 0u;
	binding.m_diagnostic.clear();
	outDiagnostic = "unloaded global-illumination probe state '" + name + "'";
	return true;
}

GlobalIlluminationSnapshotPtr GlobalIlluminationECS::GetActiveSnapshot() const
{
	m_snapshotLock.Lock();
	GlobalIlluminationSnapshotPtr snapshot = m_activeSnapshot;
	m_snapshotLock.Unlock();
	return snapshot;
}

TVector<GlobalIlluminationProbeState> GlobalIlluminationECS::GetProbeStates() const
{
	struct SortedBinding final
	{
		std::string m_name{};
		const RuntimeBinding* m_binding = nullptr;
	};
	TVector<SortedBinding> sortedBindings;
	sortedBindings.Reserve(m_bindings.Num());
	for (const auto& entry : m_bindings)
	{
		sortedBindings.Add({ entry.m_first, entry.m_second });
	}
	std::sort(
		sortedBindings.begin(),
		sortedBindings.end(),
		[](const SortedBinding& lhs, const SortedBinding& rhs)
		{
			return lhs.m_name < rhs.m_name;
		});
	TVector<GlobalIlluminationProbeState> states;
	states.Reserve(sortedBindings.Num());
	for (const SortedBinding& sorted : sortedBindings)
	{
		const std::string& name = sorted.m_name;
		const RuntimeBinding& binding = *sorted.m_binding;
		GlobalIlluminationProbeState state;
		state.m_name = name;
		state.m_asset = binding.m_assetId;
		state.m_mode = binding.m_mode;
		state.m_weight = binding.m_weight;
		state.m_residency = binding.m_residency;
		state.m_assetRevision = binding.m_observedRevision;
		state.m_diagnostic = binding.m_diagnostic;
		states.Add(std::move(state));
	}
	return states;
}

std::string GlobalIlluminationECS::GetDiagnostic() const
{
	m_snapshotLock.Lock();
	std::string diagnostic = m_diagnostic;
	m_snapshotLock.Unlock();
	return diagnostic;
}

RuntimeGIProbesStatus GlobalIlluminationECS::GetRuntimeGIProbesStatus() const
{
	RuntimeGIProbesStatus status = m_runtimeProbes.GetStatus();
	if (m_runtimeScenePreparationTask &&
		!m_runtimeScenePreparationTask->IsFinished())
	{
		status.m_bEnabled = true;
		status.m_lifecycle = ERuntimeGIProbesLifecycle::PreparingScene;
		status.m_diagnostic = m_runtimePreparationDiagnostic.empty() ?
			"preparing an immutable runtime GI scene" :
			m_runtimePreparationDiagnostic;
	}
	else if (m_bRuntimePreparationFailed)
	{
		status.m_lifecycle = ERuntimeGIProbesLifecycle::Failed;
		status.m_diagnostic = m_runtimePreparationDiagnostic;
	}
	return status;
}

bool GlobalIlluminationECS::SetRuntimeGIProbesPreviewEnabled(
	bool bEnabled,
	std::string& outDiagnostic)
{
	if (bEnabled && m_worldSettings.m_probeSource !=
		EGlobalIlluminationProbeSource::RuntimeExperimental)
	{
		outDiagnostic =
			"select Runtime Experimental as the probe source before enabling preview";
		return false;
	}
	if (m_bRuntimePreviewEnabled == bEnabled)
	{
		outDiagnostic = bEnabled ?
			"runtime GI preview is already enabled" :
			"runtime GI preview is already disabled";
		return true;
	}
	m_bRuntimePreviewEnabled = bEnabled;
	if (bEnabled)
	{
		m_bRuntimeSceneRebuildRequested = true;
		m_bRuntimePreparationFailed = false;
		m_runtimePreparationRetrySeconds = 0.0f;
		outDiagnostic = "enabled experimental runtime GI preview";
	}
	else
	{
		StopRuntimeProvider(true);
		outDiagnostic = "disabled experimental runtime GI preview";
	}
	return true;
}

bool GlobalIlluminationECS::SetRuntimeGIProbesPaused(
	bool bPaused,
	std::string& outDiagnostic)
{
	const RuntimeGIProbesStatus status = m_runtimeProbes.GetStatus();
	if (!status.m_bEnabled)
	{
		outDiagnostic = "experimental runtime GI probes are not running";
		return false;
	}
	m_runtimeProbes.SetPaused(bPaused);
	outDiagnostic = bPaused ?
		"paused experimental runtime GI probes" :
		"resumed experimental runtime GI probes";
	return true;
}

bool GlobalIlluminationECS::SetRuntimeGIProbesEditorBudget(
	Settings::ERuntimeGIProbesEditorBudget budget,
	std::string& outDiagnostic)
{
	if (m_runtimeEditorBudget == budget)
	{
		outDiagnostic = "runtime GI preview budget is already active";
		return true;
	}
	m_runtimeEditorBudget = budget;
	if (!m_runtimePreparedScene || !m_runtimeProbes.GetStatus().m_bEnabled)
	{
		outDiagnostic = "updated runtime GI preview budget";
		return true;
	}
	glm::vec3 cameraPosition{};
	if (!TryGetRuntimeCameraPosition(cameraPosition))
	{
		outDiagnostic =
			"updated runtime GI preview budget; solver is waiting for an active camera";
		return true;
	}
	const bool bStarted = StartRuntimeSolver(cameraPosition, outDiagnostic);
	if (bStarted)
	{
		m_runtimeObservedQuality = ResolveRuntimeQualitySettings();
		m_bRuntimeObservedQualityValid = true;
	}
	return bStarted;
}

bool GlobalIlluminationECS::RestartRuntimeGIProbes(
	std::string& outDiagnostic)
{
	glm::vec3 cameraPosition{};
	if (!ShouldRunRuntimeProvider())
	{
		outDiagnostic =
			"experimental runtime GI probes are disabled by source, preview, or quality settings";
		return false;
	}
	if (!m_runtimePreparedScene)
	{
		m_bRuntimeSceneRebuildRequested = true;
		outDiagnostic =
			"runtime GI scene is not prepared; requested a scene rebuild";
		return true;
	}
	if (!TryGetRuntimeCameraPosition(cameraPosition))
	{
		outDiagnostic = "runtime GI probes require an active camera";
		return false;
	}
	return StartRuntimeSolver(cameraPosition, outDiagnostic);
}

bool GlobalIlluminationECS::RebuildRuntimeGIProbesScene(
	std::string& outDiagnostic)
{
	if (m_worldSettings.m_probeSource !=
		EGlobalIlluminationProbeSource::RuntimeExperimental)
	{
		outDiagnostic =
			"select Runtime Experimental before rebuilding its scene snapshot";
		return false;
	}
	if (m_runtimeScenePreparationCancel)
	{
		m_runtimeScenePreparationCancel->store(
			true,
			std::memory_order_release);
	}
	m_bRuntimeSceneRebuildRequested = true;
	m_bRuntimePreparationFailed = false;
	m_runtimePreparationRetrySeconds = 0.0f;
	m_runtimePreparationDiagnostic =
		"runtime GI scene rebuild was requested";
	outDiagnostic = m_runtimePreparationDiagnostic;
	return true;
}

void GlobalIlluminationECS::SetRuntimeGIProbesWorkAllowed(
	bool bAllowed) noexcept
{
	m_runtimeProbes.SetWorkAllowed(bAllowed);
}

void GlobalIlluminationECS::InitializeFromWorld()
{
	if (m_bInitialized || !GetWorld())
	{
		return;
	}
	m_bInitialized = true;
	for (const auto& entry : m_worldSettings.m_probes)
	{
		RuntimeBinding binding;
		binding.m_assetId = entry.m_second->m_asset;
		binding.m_mode = entry.m_second->m_mode;
		binding.m_weight = entry.m_second->m_initialWeight;
		binding.m_bPreload = entry.m_second->m_bPreload;
		m_bindings.Insert(entry.m_first, std::move(binding));
	}

	for (const auto& entry : m_bindings)
	{
		const std::string& name = entry.m_first;
		RuntimeBinding& binding = *entry.m_second;
		if (m_worldSettings.m_probeSource ==
				EGlobalIlluminationProbeSource::BakedAssets &&
			IsEnabled() && UsesBakedGlobalIllumination(m_worldSettings.m_mode) &&
			(binding.m_bPreload || binding.m_weight > 0.0f))
		{
			std::string loadDiagnostic;
			StartLoad(name, binding, loadDiagnostic);
		}
	}

	const uint32_t requestedCount = CountPositiveWeights();
	const uint32_t budget = GetMaxProbeStatesPerSnapshot();
	if (requestedCount > budget)
	{
		SetDiagnostic(
			"world requests " + std::to_string(requestedCount) +
			" active global-illumination probe states, but the quality budget is " +
			std::to_string(budget) + "; environment irradiance fallback remains active");
	}
}

void GlobalIlluminationECS::TickBakedProvider()
{
	if (m_runtimeProbes.GetStatus().m_bEnabled ||
		m_runtimeScenePreparationTask)
	{
		StopRuntimeProvider(true);
	}
	RefreshResidency();
	RecomposeIfNeeded();
}

void GlobalIlluminationECS::TickRuntimeProvider(float deltaTime)
{
	if (!ShouldRunRuntimeProvider())
	{
		StopRuntimeProvider(true);
		SetDiagnostic(App::IsEditorMode() && !m_bRuntimePreviewEnabled ?
			"experimental runtime GI preview is disabled in the editor" :
			"experimental runtime GI probes are disabled by the active quality profile");
		return;
	}

	glm::vec3 cameraPosition{};
	if (!TryGetRuntimeCameraPosition(cameraPosition))
	{
		SetDiagnostic(
			"experimental runtime GI probes are waiting for an active camera");
		return;
	}
	const RuntimeGIProbesQualitySettings quality =
		ResolveRuntimeQualitySettings();
	if (!m_bRuntimeObservedQualityValid)
	{
		m_runtimeObservedQuality = quality;
		m_bRuntimeObservedQualityValid = true;
	}
	else if (!AreRuntimeQualitySettingsEqual(
		m_runtimeObservedQuality,
		quality))
	{
		const bool bSpacingChanged =
			m_runtimeObservedQuality.m_spacingMultiplier !=
				quality.m_spacingMultiplier;
		m_runtimeObservedQuality = quality;
		if (bSpacingChanged)
		{
			m_bRuntimeSceneRebuildRequested = true;
			m_runtimePreparationRetrySeconds = 0.0f;
			m_runtimePreparationDiagnostic =
				"runtime GI probe spacing changed; preparing the next scene generation";
		}
		else if (m_runtimePreparedScene &&
			!m_runtimeScenePreparationTask)
		{
			std::string diagnostic;
			if (!StartRuntimeSolver(cameraPosition, diagnostic))
			{
				SetDiagnostic(diagnostic);
			}
		}
	}

	ConsumeRuntimeScenePreparation(cameraPosition);
	m_runtimePreparationRetrySeconds = (std::max)(
		0.0f,
		m_runtimePreparationRetrySeconds - (std::max)(deltaTime, 0.0f));
	if (m_bRuntimeSceneRebuildRequested &&
		!m_runtimeScenePreparationTask &&
		m_runtimePreparationRetrySeconds <= 0.0f)
	{
		std::string diagnostic;
		if (!BeginRuntimeScenePreparation(diagnostic))
		{
			m_bRuntimePreparationFailed = true;
			m_runtimePreparationDiagnostic = diagnostic;
			SetDiagnostic(diagnostic);
			m_runtimePreparationRetrySeconds = 0.5f;
		}
	}

	if (m_runtimePreparedScene &&
		!m_runtimeScenePreparationTask &&
		!m_bRuntimeSceneRebuildRequested)
	{
		m_runtimeRevisionPollSeconds += (std::max)(deltaTime, 0.0f);
		if (m_runtimeRevisionPollSeconds >= 0.5f)
		{
			m_runtimeRevisionPollSeconds = 0.0f;
			const RuntimeGIProbesQualitySettings quality =
				ResolveRuntimeQualitySettings();
			GIProbesSceneCaptureRequest request;
			request.m_settings = ResolveRuntimeBakeSettings(
				m_worldSettings.m_runtimeProbes,
				quality);
			request.m_sourceIdentity = GetWorld()->GetName();
			GIProbesSceneRevision revision;
			std::string observationDiagnostic;
			if (ObserveGIProbesSceneRevision(
					GetWorld(),
					request,
					revision,
					observationDiagnostic) &&
				revision != m_runtimePreparedScene->m_observedRevision)
			{
				m_bRuntimeSceneRebuildRequested = true;
				m_runtimePreparationDiagnostic =
					"runtime GI contributors changed; preparing the next scene generation";
			}
		}
	}

	if (m_runtimePreparedScene && m_bRuntimeAnchorValid &&
		!m_runtimeScenePreparationTask)
	{
		const float spacing = m_worldSettings.m_runtimeProbes.m_minProbeSpacing *
			ResolveRuntimeQualitySettings().m_spacingMultiplier;
		const glm::vec3 cameraDelta = cameraPosition - m_runtimeAnchorCamera;
		if (glm::dot(cameraDelta, cameraDelta) >
			spacing * spacing * 16.0f)
		{
			std::string diagnostic;
			if (!StartRuntimeSolver(cameraPosition, diagnostic))
			{
				SetDiagnostic(diagnostic);
			}
		}
	}

	m_runtimeProbes.Tick(deltaTime);
	PublishRuntimeSnapshotIfNeeded();
}

bool GlobalIlluminationECS::BeginRuntimeScenePreparation(
	std::string& outDiagnostic)
{
	if (!GetWorld())
	{
		outDiagnostic = "runtime GI scene capture requires an active world";
		return false;
	}
	if (m_runtimeScenePreparationTask &&
		!m_runtimeScenePreparationTask->IsFinished())
	{
		outDiagnostic = "runtime GI scene preparation is already in progress";
		return true;
	}

	const RuntimeGIProbesQualitySettings quality =
		ResolveRuntimeQualitySettings();
	const GIProbesBakeSettings bakeSettings = ResolveRuntimeBakeSettings(
		m_worldSettings.m_runtimeProbes,
		quality);
	GIProbesSceneCaptureRequest captureRequest;
	captureRequest.m_settings = bakeSettings;
	captureRequest.m_sourceIdentity = GetWorld()->GetName();
	GIProbesSceneSnapshot snapshot;
	TVector<std::string> warnings;
	if (!CaptureGIProbesScene(
			GetWorld(),
			captureRequest,
			snapshot,
			outDiagnostic,
			[&warnings](const std::string& warning)
			{
				warnings.Add(warning);
			}))
	{
		return false;
	}
	if (!warnings.IsEmpty())
	{
		outDiagnostic += "; " + std::to_string(warnings.Num()) +
			" contributor warnings were reported";
	}

	if (m_runtimeScenePreparationCancel)
	{
		m_runtimeScenePreparationCancel->store(
			true,
			std::memory_order_release);
	}
	m_runtimeScenePreparationCancel =
		TSharedPtr<std::atomic<bool>>::Make(false);
	const auto cancel = m_runtimeScenePreparationCancel;
	const auto immutableSnapshot =
		GIProbesSceneSnapshotPtr::Make(std::move(snapshot));
	const uint64_t requestId = ++m_runtimeScenePreparationRequestId;
	m_runtimeScenePreparationTask =
		Tasks::CreateTask<RuntimeScenePreparationResult>(
			"GlobalIlluminationECS:Prepare Runtime GI Scene",
			[immutableSnapshot, bakeSettings, cancel, requestId]()
			{
				RuntimeScenePreparationResult result;
				result.m_requestId = requestId;
				GIProbesPreparedScene preparedScene;
				TVector<std::string> preparationWarnings;
				if (!PrepareGIProbesScene(
						*immutableSnapshot,
						bakeSettings,
						cancel.GetRawPtr(),
						preparedScene,
						result.m_diagnostic,
						{},
						[&preparationWarnings](const std::string& warning)
						{
							preparationWarnings.Add(warning);
						}))
				{
					return result;
				}
				result.m_scene = GIProbesPreparedScenePtr::Make(
					std::move(preparedScene));
				if (!preparationWarnings.IsEmpty())
				{
					result.m_diagnostic += "; " +
						std::to_string(preparationWarnings.Num()) +
						" scene preparation warnings were reported";
				}
				return result;
			},
			EThreadType::Background);
	m_runtimeScenePreparationTask->Run();
	m_bRuntimeSceneRebuildRequested = false;
	m_bRuntimePreparationFailed = false;
	m_runtimePreparationDiagnostic =
		"preparing an immutable runtime GI scene";
	outDiagnostic = m_runtimePreparationDiagnostic;
	return true;
}

void GlobalIlluminationECS::ConsumeRuntimeScenePreparation(
	const glm::vec3& cameraPosition)
{
	if (!m_runtimeScenePreparationTask ||
		!m_runtimeScenePreparationTask->IsFinished())
	{
		return;
	}
	RuntimeScenePreparationResult result =
		m_runtimeScenePreparationTask->GetResult();
	m_runtimeScenePreparationTask.Clear();
	m_runtimeScenePreparationCancel.Clear();
	if (result.m_requestId != m_runtimeScenePreparationRequestId)
	{
		return;
	}
	if (!result.m_scene)
	{
		m_bRuntimePreparationFailed = true;
		m_runtimePreparationDiagnostic = result.m_diagnostic.empty() ?
			"runtime GI scene preparation failed" :
			std::move(result.m_diagnostic);
		SetDiagnostic(m_runtimePreparationDiagnostic);
		return;
	}
	const RuntimeGIProbesQualitySettings quality =
		ResolveRuntimeQualitySettings();
	GIProbesSceneCaptureRequest observationRequest;
	observationRequest.m_settings = ResolveRuntimeBakeSettings(
		m_worldSettings.m_runtimeProbes,
		quality);
	observationRequest.m_sourceIdentity = GetWorld() ?
		GetWorld()->GetName() : std::string();
	GIProbesSceneRevision currentRevision;
	std::string observationDiagnostic;
	if (!ObserveGIProbesSceneRevision(
			GetWorld(),
			observationRequest,
			currentRevision,
			observationDiagnostic) ||
		currentRevision != result.m_scene->m_observedRevision)
	{
		m_bRuntimeSceneRebuildRequested = true;
		m_runtimePreparationRetrySeconds = 0.1f;
		m_runtimePreparationDiagnostic = observationDiagnostic.empty() ?
			"GI contributors changed during scene preparation; retrying" :
			std::move(observationDiagnostic);
		SetDiagnostic(m_runtimePreparationDiagnostic);
		return;
	}

	m_runtimePreparedScene = std::move(result.m_scene);
	m_bRuntimePreparationFailed = false;
	m_runtimePreparationDiagnostic = std::move(result.m_diagnostic);
	std::string startDiagnostic;
	if (!StartRuntimeSolver(cameraPosition, startDiagnostic))
	{
		m_bRuntimePreparationFailed = true;
		m_runtimePreparationDiagnostic = std::move(startDiagnostic);
		SetDiagnostic(m_runtimePreparationDiagnostic);
	}
}

bool GlobalIlluminationECS::StartRuntimeSolver(
	const glm::vec3& cameraPosition,
	std::string& outDiagnostic)
{
	if (!m_runtimePreparedScene || !m_runtimePreparedScene->m_sampler)
	{
		outDiagnostic = "runtime GI scene has no prepared ray sampler";
		return false;
	}
	RuntimeGIProbesStartRequest request;
	request.m_worldSettings = m_worldSettings.m_runtimeProbes;
	request.m_qualitySettings = ResolveRuntimeQualitySettings();
	request.m_sampler = m_runtimePreparedScene->m_sampler;
	request.m_cameraPosition = cameraPosition;
	request.m_geometryGeneration =
		m_runtimePreparedScene->m_geometryHash;
	request.m_lightingGeneration =
		m_runtimePreparedScene->m_lightingHash;
	request.m_randomSeed = static_cast<uint32_t>(
		m_runtimePreparedScene->m_geometryHash ^
		(m_runtimePreparedScene->m_geometryHash >> 32u));
	if (!m_runtimeProbes.Start(request, outDiagnostic))
	{
		return false;
	}
	m_runtimeAnchorCamera = cameraPosition;
	m_bRuntimeAnchorValid = true;
	SetDiagnostic(outDiagnostic);
	return true;
}

void GlobalIlluminationECS::PublishRuntimeSnapshotIfNeeded()
{
	const RuntimeGIProbesStatus status = m_runtimeProbes.GetStatus();
	if (status.m_publishedRevision == 0u ||
		status.m_publishedRevision == m_runtimePublishedRevision)
	{
		return;
	}
	GIProbesDataPtr data = m_runtimeProbes.GetPublishedData();
	if (!data)
	{
		return;
	}

	GlobalIlluminationSnapshotPtr snapshot =
		GlobalIlluminationSnapshotPtr::Make();
	snapshot->m_generation = ++m_generation;
	snapshot->m_lightingHash = data->m_lightingHash;
	snapshot->m_layout = data;
	RHI::RHIGlobalIlluminationState state;
	state.m_name = "Runtime Experimental";
	state.m_data = data;
	state.m_effectiveWeight = 1.0f;
	state.m_mode = EGlobalIlluminationProbeMode::Blend;
	snapshot->m_states.Add(std::move(state));
	snapshot->m_qualityBudget = 1u;
	m_snapshotLock.Lock();
	m_activeSnapshot = std::move(snapshot);
	m_diagnostic = status.m_diagnostic;
	m_snapshotLock.Unlock();
	m_runtimePublishedRevision = status.m_publishedRevision;
	m_compositionCount.fetch_add(1u, std::memory_order_release);
}

void GlobalIlluminationECS::StopRuntimeProvider(bool bClearSnapshot)
{
	if (m_runtimeScenePreparationCancel)
	{
		m_runtimeScenePreparationCancel->store(
			true,
			std::memory_order_release);
	}
	m_runtimeScenePreparationTask.Clear();
	m_runtimeScenePreparationCancel.Clear();
	m_runtimePreparedScene.Clear();
	m_runtimeProbes.Disable();
	m_runtimePublishedRevision = 0u;
	m_bRuntimeAnchorValid = false;
	m_bRuntimeSceneRebuildRequested = true;
	m_runtimeRevisionPollSeconds = 0.0f;
	m_runtimePreparationRetrySeconds = 0.0f;
	if (bClearSnapshot)
	{
		ClearActiveSnapshot();
	}
}

bool GlobalIlluminationECS::ShouldRunRuntimeProvider() const noexcept
{
	const auto& quality =
		App::GetActiveGraphicsSettings().m_runtimeGIProbes;
	return m_worldSettings.m_probeSource ==
			EGlobalIlluminationProbeSource::RuntimeExperimental &&
		(App::IsEditorMode() ? m_bRuntimePreviewEnabled : quality.m_bEnabled) &&
		GetMaxProbeStatesPerSnapshot() > 0u;
}

RuntimeGIProbesQualitySettings
GlobalIlluminationECS::ResolveRuntimeQualitySettings() const noexcept
{
	RuntimeGIProbesQualitySettings quality =
		App::GetActiveGraphicsSettings().m_runtimeGIProbes;
	if (!App::IsEditorMode())
	{
		return quality;
	}
	quality.m_bEnabled = true;
	if (m_runtimeEditorBudget ==
		Settings::ERuntimeGIProbesEditorBudget::Eco)
	{
		quality.m_maxActiveProbes = (std::min)(
			quality.m_maxActiveProbes,
			2048u);
		quality.m_workerCount = 1u;
		quality.m_cpuDutyFraction = (std::min)(
			quality.m_cpuDutyFraction,
			0.1f);
		quality.m_cpuBudgetMilliseconds = (std::min)(
			quality.m_cpuBudgetMilliseconds,
			2.0f);
		quality.m_maxPublicationsPerSecond = (std::min)(
			quality.m_maxPublicationsPerSecond,
			1.0f);
	}
	return quality;
}

bool GlobalIlluminationECS::TryGetRuntimeCameraPosition(
	glm::vec3& outPosition) const
{
	CameraECS* camera = GetWorld() ? GetWorld()->GetECS<CameraECS>() : nullptr;
	if (!camera)
	{
		return false;
	}
	Math::Transform transform;
	CameraData cameraData;
	if (!camera->TryGetActiveCamera(transform, cameraData))
	{
		return false;
	}
	outPosition = transform.m_position;
	return std::isfinite(outPosition.x) &&
		std::isfinite(outPosition.y) &&
		std::isfinite(outPosition.z);
}

bool GlobalIlluminationECS::StartLoad(
	const std::string& name,
	RuntimeBinding& binding,
	std::string& outDiagnostic)
{
	if (binding.m_asset && binding.m_asset->IsReady())
	{
		binding.m_residency = EGlobalIlluminationProbeResidency::Resident;
		outDiagnostic = "global-illumination probe state is already resident";
		return true;
	}
	if (binding.m_loadTask && !binding.m_loadTask->IsFinished())
	{
		binding.m_residency = EGlobalIlluminationProbeResidency::Loading;
		outDiagnostic = "global-illumination probe state is already loading";
		return true;
	}
	GIProbesImporter* importer = App::GetSubmodule<GIProbesImporter>();
	if (!importer || !binding.m_assetId)
	{
		binding.m_residency = EGlobalIlluminationProbeResidency::Failed;
		binding.m_diagnostic = "no GI probe importer or asset reference is available";
		outDiagnostic = binding.m_diagnostic;
		return false;
	}
	if (!binding.m_bRuntimeRetained)
	{
		importer->RetainRuntimeGIProbes(binding.m_assetId);
		binding.m_bRuntimeRetained = true;
	}
	binding.m_loadTask = importer->LoadGIProbes(
		binding.m_assetId,
		binding.m_asset);
	if (!binding.m_loadTask)
	{
		binding.m_residency = EGlobalIlluminationProbeResidency::Failed;
		binding.m_diagnostic = "cannot start loading .probes asset for state '" +
			name + "'";
		outDiagnostic = binding.m_diagnostic;
		return false;
	}
	binding.m_residency = EGlobalIlluminationProbeResidency::Loading;
	binding.m_diagnostic.clear();
	outDiagnostic = "started loading global-illumination probe state '" + name + "'";
	return true;
}

void GlobalIlluminationECS::RefreshResidency()
{
	for (const auto& entry : m_bindings)
	{
		RuntimeBinding& binding = *entry.m_second;
		if (!binding.m_asset)
		{
			continue;
		}
		if (binding.m_asset->IsReady())
		{
			binding.m_residency = EGlobalIlluminationProbeResidency::Resident;
			const uint64_t revision = binding.m_asset->GetRevision();
			if (revision != binding.m_observedRevision)
			{
				binding.m_observedRevision = revision;
				binding.m_diagnostic = binding.m_asset->GetSnapshot().m_diagnostic;
				m_bCompositionDirty = true;
			}
		}
		else if (binding.m_loadTask && binding.m_loadTask->IsFinished())
		{
			binding.m_residency = EGlobalIlluminationProbeResidency::Failed;
			binding.m_diagnostic = binding.m_asset->GetSnapshot().m_diagnostic;
		}
		else
		{
			binding.m_residency = EGlobalIlluminationProbeResidency::Loading;
		}
	}
}

void GlobalIlluminationECS::RecomposeIfNeeded()
{
	if (!m_bCompositionDirty)
	{
		return;
	}
	const uint32_t budget = GetMaxProbeStatesPerSnapshot();
	if (budget == 0u)
	{
		ClearActiveSnapshot();
		m_bCompositionDirty = false;
		SetDiagnostic("Global Illumination ECS is disabled by the active quality preset");
		return;
	}

	struct SortedBinding final
	{
		std::string m_name{};
		RuntimeBinding* m_binding = nullptr;
	};
	TVector<SortedBinding> sortedBindings;
	sortedBindings.Reserve(m_bindings.Num());
	for (const auto& entry : m_bindings)
	{
		sortedBindings.Add({ entry.m_first, entry.m_second });
	}
	std::sort(
		sortedBindings.begin(),
		sortedBindings.end(),
		[](const SortedBinding& lhs, const SortedBinding& rhs)
		{
			return lhs.m_name < rhs.m_name;
		});
	TVector<GIProbesCompositionInput> inputs;
	inputs.Reserve(sortedBindings.Num());
	for (const SortedBinding& sorted : sortedBindings)
	{
		const std::string& name = sorted.m_name;
		RuntimeBinding& binding = *sorted.m_binding;
		if (binding.m_weight <= 0.0f)
		{
			continue;
		}
		if (!binding.m_asset)
		{
			std::string loadDiagnostic;
			StartLoad(name, binding, loadDiagnostic);
			return;
		}
		if (!binding.m_asset->IsReady())
		{
			if (binding.m_residency == EGlobalIlluminationProbeResidency::Failed)
			{
				SetDiagnostic(
					"cannot compose Global Illumination ECS snapshot because state '" +
					name + "' failed: " + binding.m_diagnostic);
			}
			return;
		}
		GIProbesCompositionInput input;
		input.m_name = name;
		input.m_data = binding.m_asset->GetSnapshot().m_data;
		input.m_mode = binding.m_mode;
		input.m_weight = binding.m_weight;
		input.m_asset = binding.m_assetId;
		inputs.Add(std::move(input));
	}

	GIProbesCompositionPlan plan = GIProbesComposer::BuildPlan(
		inputs,
		budget,
		EGIProbesCompositionValidation::TrustedAssetMetadata);
	if (plan.m_status == EGIProbesCompositionStatus::Disabled)
	{
		ClearActiveSnapshot();
		m_bCompositionDirty = false;
		SetDiagnostic("no active baked probe states; using environment irradiance fallback");
		return;
	}
	if (!plan.IsSuccess())
	{
		m_rejectedCompositionCount.fetch_add(1u, std::memory_order_release);
		m_bCompositionDirty = false;
		const GlobalIlluminationSnapshotPtr activeSnapshot = GetActiveSnapshot();
		if (activeSnapshot && activeSnapshot->m_states.Num() <= budget)
		{
			SetDiagnostic(
				"Global Illumination ECS preserved the last complete snapshot: " +
				plan.m_diagnostic);
		}
		else
		{
			ClearActiveSnapshot();
			SetDiagnostic(
				"Global Illumination ECS is using environment irradiance fallback: " +
				plan.m_diagnostic);
		}
		return;
	}

	GlobalIlluminationSnapshotPtr snapshot =
		GlobalIlluminationSnapshotPtr::Make();
	snapshot->m_generation = ++m_generation;
	snapshot->m_lightingHash = plan.m_lightingHash;
	snapshot->m_layout = plan.m_layout;
	snapshot->m_states.Reserve(plan.m_states.Num());
	for (size_t stateIndex = 0u;
		stateIndex < plan.m_states.Num();
		++stateIndex)
	{
		RHI::RHIGlobalIlluminationState state;
		state.m_name = std::move(plan.m_names[stateIndex]);
		state.m_asset = plan.m_assets[stateIndex];
		state.m_data = std::move(plan.m_states[stateIndex]);
		state.m_effectiveWeight = plan.m_effectiveWeights[stateIndex];
		state.m_mode = plan.m_modes[stateIndex];
		snapshot->m_states.Add(std::move(state));
	}
	snapshot->m_qualityBudget = budget;
	m_snapshotLock.Lock();
	m_activeSnapshot = std::move(snapshot);
	m_diagnostic = plan.m_diagnostic;
	m_snapshotLock.Unlock();
	m_compositionCount.fetch_add(1u, std::memory_order_release);
	m_bCompositionDirty = false;
}

void GlobalIlluminationECS::DrawDebugVisualization() const
{
	if (!GetWorld())
	{
		return;
	}
	const RHI::ESceneViewRenderMode mode = App::GetEditorRenderMode();
	if (mode != RHI::ESceneViewRenderMode::GlobalIlluminationProbes &&
		mode != RHI::ESceneViewRenderMode::GlobalIlluminationBricks &&
		mode != RHI::ESceneViewRenderMode::GlobalIlluminationValidity &&
		mode != RHI::ESceneViewRenderMode::GlobalIlluminationVisibility)
	{
		return;
	}

	const GlobalIlluminationSnapshotPtr snapshot = GetActiveSnapshot();
	if (!snapshot || !snapshot->m_layout)
	{
		return;
	}
	RHI::DebugContext* debugContext =
		GetWorld()->GetDebugContext().GetRawPtr();
	if (!debugContext)
	{
		return;
	}

	const GIProbesData& layout = *snapshot->m_layout;
	if (mode == RHI::ESceneViewRenderMode::GlobalIlluminationBricks)
	{
		constexpr size_t MaxDebugBricks = 2048u;
		const size_t stride = (std::max)(
			static_cast<size_t>(1u),
			(layout.m_bricks.Num() + MaxDebugBricks - 1u) / MaxDebugBricks);
		for (size_t brickIndex = 0u;
			brickIndex < layout.m_bricks.Num();
			brickIndex += stride)
		{
			const GIProbeBrick& brick = layout.m_bricks[brickIndex];
			const float level = layout.m_bakeSettings.m_maxSubdivisionLevel > 0u ?
				static_cast<float>(brick.m_subdivisionLevel) /
				static_cast<float>(layout.m_bakeSettings.m_maxSubdivisionLevel) : 0.0f;
			const glm::vec4 color = glm::mix(
				glm::vec4(0.1f, 0.45f, 1.0f, 1.0f),
				glm::vec4(1.0f, 0.25f, 0.05f, 1.0f),
				level);
			Math::AABB bounds;
			bounds.m_min = brick.m_min;
			bounds.m_max = brick.m_max;
			debugContext->DrawAABB(bounds, color);
		}
		return;
	}

	constexpr size_t MaxDebugProbes = 4096u;
	const size_t stride = (std::max)(
		static_cast<size_t>(1u),
		(layout.m_probes.Num() + MaxDebugProbes - 1u) / MaxDebugProbes);
	const float markerSize = glm::clamp(
		layout.m_bakeSettings.m_minProbeSpacing * 0.08f,
		0.02f,
		0.25f);
	static constexpr glm::vec3 VisibilityDirections[] = {
		glm::vec3(1.0f, 0.0f, 0.0f),
		glm::vec3(-1.0f, 0.0f, 0.0f),
		glm::vec3(0.0f, 1.0f, 0.0f),
		glm::vec3(0.0f, -1.0f, 0.0f),
		glm::vec3(0.0f, 0.0f, 1.0f),
		glm::vec3(0.0f, 0.0f, -1.0f)
	};
	for (size_t probeIndex = 0u;
		probeIndex < layout.m_probes.Num();
		probeIndex += stride)
	{
		const GIProbe& probe = layout.m_probes[probeIndex];
		const bool bValid = (probe.m_flags & static_cast<uint32_t>(
			EGIProbeFlag::Valid)) != 0u;
		const bool bRelocated = (probe.m_flags & static_cast<uint32_t>(
			EGIProbeFlag::Relocated)) != 0u;
		glm::vec4 color = bValid ?
			glm::vec4(0.15f, 1.0f, 0.25f, 1.0f) :
			glm::vec4(1.0f, 0.1f, 0.05f, 1.0f);
		if (mode == RHI::ESceneViewRenderMode::GlobalIlluminationValidity)
		{
			color = glm::mix(
				glm::vec4(1.0f, 0.05f, 0.02f, 1.0f),
				glm::vec4(0.05f, 1.0f, 0.15f, 1.0f),
				glm::clamp(probe.m_validity, 0.0f, 1.0f));
		}

		if (mode == RHI::ESceneViewRenderMode::GlobalIlluminationVisibility)
		{
			for (uint32_t directionIndex = 0u;
				directionIndex < GIProbeVisibilityDirectionCount;
				++directionIndex)
			{
				const glm::vec2 moments = probe.m_visibility[directionIndex];
				const float length = glm::clamp(
					moments.x,
					markerSize,
					layout.m_bakeSettings.m_minProbeSpacing * 2.0f);
				const float variance = (std::max)(
					moments.y - moments.x * moments.x,
					0.0f);
				const float uncertainty = glm::clamp(
					variance / (length * length + 0.0001f),
					0.0f,
					1.0f);
				debugContext->DrawLine(
					probe.m_position,
					probe.m_position + VisibilityDirections[directionIndex] * length,
					glm::mix(
						glm::vec4(0.05f, 0.9f, 1.0f, 1.0f),
						glm::vec4(1.0f, 0.2f, 0.05f, 1.0f),
						uncertainty));
			}
			continue;
		}

		debugContext->DrawLine(
			probe.m_position - glm::vec3(markerSize, 0.0f, 0.0f),
			probe.m_position + glm::vec3(markerSize, 0.0f, 0.0f),
			color);
		debugContext->DrawLine(
			probe.m_position - glm::vec3(0.0f, markerSize, 0.0f),
			probe.m_position + glm::vec3(0.0f, markerSize, 0.0f),
			color);
		debugContext->DrawLine(
			probe.m_position - glm::vec3(0.0f, 0.0f, markerSize),
			probe.m_position + glm::vec3(0.0f, 0.0f, markerSize),
			color);
		if (bRelocated)
		{
			debugContext->DrawArrow(
				probe.m_position - probe.m_relocationOffset,
				probe.m_position,
				glm::vec4(1.0f, 0.65f, 0.05f, 1.0f));
		}
	}
}

uint32_t GlobalIlluminationECS::CountPositiveWeights(
	const TMap<std::string, float>* overrides) const
{
	uint32_t count = 0u;
	for (const auto& entry : m_bindings)
	{
		float weight = entry.m_second->m_weight;
		const float* overrideWeight = nullptr;
		if (overrides && overrides->Find(entry.m_first, overrideWeight))
		{
			weight = *overrideWeight;
		}
		count += weight > 0.0f ? 1u : 0u;
	}
	return count;
}

void GlobalIlluminationECS::SetDiagnostic(std::string diagnostic)
{
	m_snapshotLock.Lock();
	m_diagnostic = std::move(diagnostic);
	m_snapshotLock.Unlock();
}

void GlobalIlluminationECS::ClearActiveSnapshot()
{
	m_snapshotLock.Lock();
	m_activeSnapshot.Clear();
	m_snapshotLock.Unlock();
}
