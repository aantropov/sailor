#include "ECS/GlobalIlluminationECS.h"

#include "AssetRegistry/AssetRegistry.h"
#include "Core/LogMacros.h"
#include "Engine/World.h"
#include "RHI/DebugContext.h"
#include "Sailor.h"
#include "Settings/GraphicsSettings.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;

void GlobalIlluminationECS::BeginPlay()
{
	InitializeFromWorld();
}

Tasks::ITaskPtr GlobalIlluminationECS::Tick(float)
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
		return nullptr;
	}
	RefreshResidency();
	RecomposeIfNeeded();
	DrawDebugVisualization();
	return nullptr;
}

void GlobalIlluminationECS::EndPlay()
{
	if (ProbeVolumeImporter* importer =
		App::GetSubmodule<ProbeVolumeImporter>())
	{
		for (const auto& entry : m_bindings)
		{
			RuntimeBinding& binding = *entry.m_second;
			if (binding.m_bRuntimeRetained)
			{
				importer->ReleaseRuntimeProbeVolume(binding.m_assetId);
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
	const GlobalIlluminationWorldSettings& settings,
	std::string& outDiagnostic)
{
	if (!settings.Validate(outDiagnostic))
	{
		return false;
	}

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
		const bool bNeedsResident = source.m_bPreload ||
			source.m_initialWeight > 0.0f;
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

	if (ProbeVolumeImporter* importer =
		App::GetSubmodule<ProbeVolumeImporter>())
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
				importer->ReleaseRuntimeProbeVolume(existing.m_assetId);
			}
		}
	}

	m_bindings = std::move(nextBindings);
	m_worldSettings = settings;
	m_bInitialized = true;
	m_bCompositionDirty = true;
	for (const auto& entry : m_bindings)
	{
		RuntimeBinding& binding = *entry.m_second;
		if (IsEnabled() && UsesBakedGlobalIllumination(settings.m_mode) &&
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
		if (binding.m_weight > 0.0f && !binding.m_asset)
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
	if (mode != EGlobalIlluminationProbeMode::Blend &&
		mode != EGlobalIlluminationProbeMode::Additive)
	{
		outDiagnostic = "global-illumination probe mode must be Blend or Additive";
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
		if (ProbeVolumeImporter* importer =
			App::GetSubmodule<ProbeVolumeImporter>())
		{
			importer->ReleaseRuntimeProbeVolume(binding.m_assetId);
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
		if (IsEnabled() && UsesBakedGlobalIllumination(m_worldSettings.m_mode) &&
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
	ProbeVolumeImporter* importer = App::GetSubmodule<ProbeVolumeImporter>();
	if (!importer || !binding.m_assetId)
	{
		binding.m_residency = EGlobalIlluminationProbeResidency::Failed;
		binding.m_diagnostic = "no probe-volume importer or asset reference is available";
		outDiagnostic = binding.m_diagnostic;
		return false;
	}
	if (!binding.m_bRuntimeRetained)
	{
		importer->RetainRuntimeProbeVolume(binding.m_assetId);
		binding.m_bRuntimeRetained = true;
	}
	binding.m_loadTask = importer->LoadProbeVolume(
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
	TVector<ProbeVolumeCompositionInput> inputs;
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
		ProbeVolumeCompositionInput input;
		input.m_name = name;
		input.m_data = binding.m_asset->GetSnapshot().m_data;
		input.m_mode = binding.m_mode;
		input.m_weight = binding.m_weight;
		input.m_asset = binding.m_assetId;
		inputs.Add(std::move(input));
	}

	ProbeVolumeCompositionPlan plan = ProbeVolumeComposer::BuildPlan(
		inputs,
		budget,
		EProbeVolumeCompositionValidation::TrustedAssetMetadata);
	if (plan.m_status == EProbeVolumeCompositionStatus::Disabled)
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

	const ProbeVolumeData& layout = *snapshot->m_layout;
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
			const ProbeVolumeBrick& brick = layout.m_bricks[brickIndex];
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
		const ProbeVolumeSample& probe = layout.m_probes[probeIndex];
		const bool bValid = (probe.m_flags & static_cast<uint32_t>(
			EProbeVolumeSampleFlag::Valid)) != 0u;
		const bool bRelocated = (probe.m_flags & static_cast<uint32_t>(
			EProbeVolumeSampleFlag::Relocated)) != 0u;
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
				directionIndex < ProbeVolumeVisibilityDirectionCount;
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
