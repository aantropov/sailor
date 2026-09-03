#include "EditorEngineProtocolInternal.h"

#include "Editor/GlobalIlluminationBakeController.h"
#include "Editor/GlobalIlluminationEditorState.h"
#include "GlobalIllumination/GISettings.h"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Sailor.h"
#include "Settings/GraphicsSettings.h"

#include <cmath>
#include <string>

namespace Sailor::Protocol::EditorEngineProtocolCommands
{
	using sailor::editor::v1::ProtocolRequest;
	using sailor::editor::v1::ProtocolResponse;

	static bool TryParseFileId(const std::string& value, bool bRequired, Sailor::FileId& outFileId)
	{
		outFileId = {};
		if (value.empty())
		{
			return !bRequired;
		}
		outFileId = Sailor::FileId(value);
		return static_cast<bool>(outFileId);
	}

	static sailor::editor::v1::GIProbesBakeState ToProtocolBakeState(Sailor::EEditorGIProbesBakeState state)
	{
		switch (state)
		{
		case Sailor::EEditorGIProbesBakeState::Idle:
			return sailor::editor::v1::GI_PROBES_BAKE_STATE_IDLE;
		case Sailor::EEditorGIProbesBakeState::Preparing:
			return sailor::editor::v1::GI_PROBES_BAKE_STATE_PREPARING;
		case Sailor::EEditorGIProbesBakeState::Baking:
			return sailor::editor::v1::GI_PROBES_BAKE_STATE_BAKING;
		case Sailor::EEditorGIProbesBakeState::Saving:
			return sailor::editor::v1::GI_PROBES_BAKE_STATE_SAVING;
		case Sailor::EEditorGIProbesBakeState::Succeeded:
			return sailor::editor::v1::GI_PROBES_BAKE_STATE_SUCCEEDED;
		case Sailor::EEditorGIProbesBakeState::Failed:
			return sailor::editor::v1::GI_PROBES_BAKE_STATE_FAILED;
		case Sailor::EEditorGIProbesBakeState::Cancelled:
			return sailor::editor::v1::GI_PROBES_BAKE_STATE_CANCELLED;
		default:
			return sailor::editor::v1::GI_PROBES_BAKE_STATE_UNSPECIFIED;
		}
	}

	static bool TryGetGlobalIlluminationProbeMode(sailor::editor::v1::GlobalIlluminationProbeMode protocolMode,
		Sailor::EGlobalIlluminationProbeMode& outMode)
	{
		switch (protocolMode)
		{
		case sailor::editor::v1::GLOBAL_ILLUMINATION_PROBE_MODE_BLEND:
			outMode = Sailor::EGlobalIlluminationProbeMode::Blend;
			return true;
		case sailor::editor::v1::GLOBAL_ILLUMINATION_PROBE_MODE_ADDITIVE:
			outMode = Sailor::EGlobalIlluminationProbeMode::Additive;
			return true;
		case sailor::editor::v1::GLOBAL_ILLUMINATION_PROBE_MODE_UNSPECIFIED:
		default:
			return false;
		}
	}

	static bool TryGetGlobalIlluminationMode(sailor::editor::v1::GlobalIlluminationMode protocolMode,
		Sailor::EGlobalIlluminationMode& outMode)
	{
		switch (protocolMode)
		{
		case sailor::editor::v1::GLOBAL_ILLUMINATION_MODE_NO_GI:
			outMode = Sailor::EGlobalIlluminationMode::NoGI;
			return true;
		case sailor::editor::v1::GLOBAL_ILLUMINATION_MODE_RUNTIME:
			outMode = Sailor::EGlobalIlluminationMode::Runtime;
			return true;
		case sailor::editor::v1::GLOBAL_ILLUMINATION_MODE_BAKED:
			outMode = Sailor::EGlobalIlluminationMode::Baked;
			return true;
		case sailor::editor::v1::GLOBAL_ILLUMINATION_MODE_UNSPECIFIED:
		default:
			return false;
		}
	}

	static sailor::editor::v1::GlobalIlluminationMode ToProtocolGlobalIlluminationMode(
		Sailor::EGlobalIlluminationMode mode)
	{
		switch (mode)
		{
		case Sailor::EGlobalIlluminationMode::NoGI:
			return sailor::editor::v1::GLOBAL_ILLUMINATION_MODE_NO_GI;
		case Sailor::EGlobalIlluminationMode::Runtime:
			return sailor::editor::v1::GLOBAL_ILLUMINATION_MODE_RUNTIME;
		case Sailor::EGlobalIlluminationMode::Baked:
		default:
			return sailor::editor::v1::GLOBAL_ILLUMINATION_MODE_BAKED;
		}
	}

	static sailor::editor::v1::GlobalIlluminationProbeMode ToProtocolGlobalIlluminationProbeMode(
		Sailor::EGlobalIlluminationProbeMode mode)
	{
		return mode == Sailor::EGlobalIlluminationProbeMode::Additive
				   ? sailor::editor::v1::GLOBAL_ILLUMINATION_PROBE_MODE_ADDITIVE
				   : sailor::editor::v1::GLOBAL_ILLUMINATION_PROBE_MODE_BLEND;
	}

	static sailor::editor::v1::RuntimeGIProbesLifecycle ToProtocolRuntimeGIProbesLifecycle(
		Sailor::ERuntimeGIProbesLifecycle lifecycle)
	{
		switch (lifecycle)
		{
		case Sailor::ERuntimeGIProbesLifecycle::Disabled:
			return sailor::editor::v1::RUNTIME_GI_PROBES_LIFECYCLE_DISABLED;
		case Sailor::ERuntimeGIProbesLifecycle::PreparingScene:
			return sailor::editor::v1::RUNTIME_GI_PROBES_LIFECYCLE_PREPARING_SCENE;
		case Sailor::ERuntimeGIProbesLifecycle::Tracing:
			return sailor::editor::v1::RUNTIME_GI_PROBES_LIFECYCLE_TRACING;
		case Sailor::ERuntimeGIProbesLifecycle::Ready:
			return sailor::editor::v1::RUNTIME_GI_PROBES_LIFECYCLE_READY;
		case Sailor::ERuntimeGIProbesLifecycle::Paused:
			return sailor::editor::v1::RUNTIME_GI_PROBES_LIFECYCLE_PAUSED;
		case Sailor::ERuntimeGIProbesLifecycle::Throttled:
			return sailor::editor::v1::RUNTIME_GI_PROBES_LIFECYCLE_THROTTLED;
		case Sailor::ERuntimeGIProbesLifecycle::Failed:
			return sailor::editor::v1::RUNTIME_GI_PROBES_LIFECYCLE_FAILED;
		default:
			return sailor::editor::v1::RUNTIME_GI_PROBES_LIFECYCLE_UNSPECIFIED;
		}
	}

	static sailor::editor::v1::RuntimeGIProbesPreviewBudget ToProtocolRuntimeGIProbesPreviewBudget(
		Sailor::Settings::ERuntimeGIProbesEditorBudget budget)
	{
		return budget == Sailor::Settings::ERuntimeGIProbesEditorBudget::Balanced
				   ? sailor::editor::v1::RUNTIME_GI_PROBES_PREVIEW_BUDGET_BALANCED
				   : sailor::editor::v1::RUNTIME_GI_PROBES_PREVIEW_BUDGET_ECO;
	}

	static bool TryGetRuntimeGIProbesPreviewBudget(sailor::editor::v1::RuntimeGIProbesPreviewBudget protocolBudget,
		Sailor::Settings::ERuntimeGIProbesEditorBudget& outBudget)
	{
		switch (protocolBudget)
		{
		case sailor::editor::v1::RUNTIME_GI_PROBES_PREVIEW_BUDGET_ECO:
			outBudget = Sailor::Settings::ERuntimeGIProbesEditorBudget::Eco;
			return true;
		case sailor::editor::v1::RUNTIME_GI_PROBES_PREVIEW_BUDGET_BALANCED:
			outBudget = Sailor::Settings::ERuntimeGIProbesEditorBudget::Balanced;
			return true;
		case sailor::editor::v1::RUNTIME_GI_PROBES_PREVIEW_BUDGET_UNSPECIFIED:
		default:
			return false;
		}
	}

	static void SetProtocolRuntimeGIProbesSettings(sailor::editor::v1::RuntimeGIProbesSettings& destination,
		const Sailor::RuntimeGIProbesSettings& source)
	{
		destination.set_version(source.m_version);
		destination.set_include_sky(source.m_bIncludeSky);
		destination.set_include_emissive(source.m_bIncludeEmissive);
		destination.set_include_direct_lighting(source.m_bIncludeDirectLighting);
		destination.set_bounce_count(source.m_bounceCount);
		destination.set_min_probe_spacing(source.m_minProbeSpacing);
		destination.set_normal_bias(source.m_normalBias);
		destination.set_view_bias(source.m_viewBias);
		destination.set_max_ray_distance(source.m_maxRayDistance);
	}

	static sailor::editor::v1::GlobalIlluminationProbeResidency ToProtocolGlobalIlluminationProbeResidency(
		Sailor::EGlobalIlluminationProbeResidency residency)
	{
		switch (residency)
		{
		case Sailor::EGlobalIlluminationProbeResidency::Unloaded:
			return sailor::editor::v1::GLOBAL_ILLUMINATION_PROBE_RESIDENCY_UNLOADED;
		case Sailor::EGlobalIlluminationProbeResidency::Loading:
			return sailor::editor::v1::GLOBAL_ILLUMINATION_PROBE_RESIDENCY_LOADING;
		case Sailor::EGlobalIlluminationProbeResidency::Resident:
			return sailor::editor::v1::GLOBAL_ILLUMINATION_PROBE_RESIDENCY_RESIDENT;
		case Sailor::EGlobalIlluminationProbeResidency::Failed:
			return sailor::editor::v1::GLOBAL_ILLUMINATION_PROBE_RESIDENCY_FAILED;
		default:
			return sailor::editor::v1::GLOBAL_ILLUMINATION_PROBE_RESIDENCY_UNSPECIFIED;
		}
	}

	bool DispatchGICommand(const ProtocolRequest& request, ProtocolResponse& response)
	{
		switch (request.command_case())
		{
		case ProtocolRequest::kStartGiProbesBake:
		{
			const auto& bake = request.start_gi_probes_bake();
			Sailor::EditorGIProbesBakeRequest nativeRequest;
			if (!TryParseFileId(bake.world_file_id(), true, nativeRequest.m_worldAsset) ||
				!TryParseFileId(bake.layout_source_file_id(), false, nativeRequest.m_layoutSource) ||
				bake.output_virtual_path().empty() || bake.state_name().empty() || !bake.has_settings())
			{
				SetError(response, "The GI probe bake request is invalid.");
				break;
			}

			const auto& settings = bake.settings();
			nativeRequest.m_outputVirtualPath = bake.output_virtual_path();
			nativeRequest.m_stateName = bake.state_name();
			nativeRequest.m_settings.m_raysPerProbe = settings.rays_per_probe();
			nativeRequest.m_settings.m_bounceCount = settings.bounce_count();
			nativeRequest.m_settings.m_randomSeed = settings.random_seed();
			nativeRequest.m_settings.m_maxSubdivisionLevel = settings.max_subdivision_level();
			nativeRequest.m_settings.m_minProbeSpacing = settings.min_probe_spacing();
			nativeRequest.m_settings.m_normalBias = settings.normal_bias();
			nativeRequest.m_settings.m_viewBias = settings.view_bias();
			nativeRequest.m_settings.m_maxRayDistance = settings.max_ray_distance();
			nativeRequest.m_settings.m_bIncludeSky = settings.include_sky();
			nativeRequest.m_settings.m_bIncludeEmissive = settings.include_emissive();
			nativeRequest.m_settings.m_bIncludeDirectLighting = settings.include_direct_lighting();
			nativeRequest.m_bOverwrite = bake.overwrite();
			nativeRequest.m_threadCount = bake.has_thread_count() ? bake.thread_count() : 1u;

			if (bake.has_fallback_environment() && !IsFiniteVector4(bake.fallback_environment()))
			{
				SetError(response, "The GI probe bake vectors must be finite.");
				break;
			}
			if (bake.has_fallback_environment())
			{
				nativeRequest.m_fallbackEnvironment = {
					bake.fallback_environment().x(), bake.fallback_environment().y(), bake.fallback_environment().z()};
			}

			std::string diagnostic;
			if (!Sailor::App::StartEditorGIProbesBake(nativeRequest, diagnostic))
			{
				SetError(response, diagnostic.empty() ? "Failed to start the GI probe bake." : diagnostic);
				break;
			}
			SetBoolResult(response, true);
			break;
		}

		case ProtocolRequest::kGetGiProbesBakeStatus:
		{
			Sailor::EditorGIProbesBakeStatus status;
			if (!Sailor::App::GetEditorGIProbesBakeStatus(status))
			{
				SetError(response, "The GI probe bake controller is unavailable.");
				break;
			}
			SetSuccess(response);
			auto* result = response.mutable_gi_probes_bake_status_result();
			result->set_state(ToProtocolBakeState(status.m_state));
			result->set_progress(status.m_progress);
			result->set_completed_probes(status.m_completedProbes);
			result->set_total_probes(status.m_totalProbes);
			result->set_brick_count(status.m_brickCount);
			result->set_probe_count(status.m_probeCount);
			result->set_elapsed_seconds(status.m_elapsedSeconds);
			result->set_layout_hash(status.m_layoutHash);
			result->set_transport_hash(status.m_transportHash);
			result->set_lighting_hash(status.m_lightingHash);
			result->set_stage(status.m_stage);
			result->set_output_virtual_path(status.m_outputVirtualPath);
			result->set_diagnostic(status.m_diagnostic);
			break;
		}

		case ProtocolRequest::kCancelGiProbesBake:
		{
			std::string diagnostic;
			if (!Sailor::App::CancelEditorGIProbesBake(diagnostic))
			{
				SetError(response, diagnostic.empty() ? "Failed to cancel the GI probe bake." : diagnostic);
				break;
			}
			SetBoolResult(response, true);
			break;
		}

		case ProtocolRequest::kSetGiSettings:
		{
			Sailor::GISettings nativeSettings;
			const auto& protocolSettings = request.set_gi_settings();
			bool bValid = true;
			std::string diagnostic;
			if (protocolSettings.has_mode() &&
				!TryGetGlobalIlluminationMode(protocolSettings.mode(), nativeSettings.m_mode))
			{
				bValid = false;
				diagnostic = "The Global Illumination mode is invalid.";
			}
			if (bValid && !protocolSettings.has_runtime_probes())
			{
				bValid = false;
				diagnostic = "Runtime GI probe settings are required.";
			}
			if (bValid)
			{
				const auto& runtime = protocolSettings.runtime_probes();
				nativeSettings.m_runtimeProbes.m_version = runtime.version();
				nativeSettings.m_runtimeProbes.m_bIncludeSky = runtime.include_sky();
				nativeSettings.m_runtimeProbes.m_bIncludeEmissive = runtime.include_emissive();
				nativeSettings.m_runtimeProbes.m_bIncludeDirectLighting = runtime.include_direct_lighting();
				nativeSettings.m_runtimeProbes.m_bounceCount = runtime.bounce_count();
				nativeSettings.m_runtimeProbes.m_minProbeSpacing = runtime.min_probe_spacing();
				nativeSettings.m_runtimeProbes.m_normalBias = runtime.normal_bias();
				nativeSettings.m_runtimeProbes.m_viewBias = runtime.view_bias();
				nativeSettings.m_runtimeProbes.m_maxRayDistance = runtime.max_ray_distance();
				if (!nativeSettings.m_runtimeProbes.Validate(diagnostic))
				{
					bValid = false;
				}
			}
			for (const auto& probe : protocolSettings.probes())
			{
				if (!bValid)
				{
					break;
				}
				Sailor::GlobalIlluminationProbeBinding binding;
				if (probe.name().empty() || !TryParseFileId(probe.asset_file_id(), true, binding.m_asset) ||
					!TryGetGlobalIlluminationProbeMode(probe.mode(), binding.m_mode) ||
					!std::isfinite(probe.initial_weight()) || probe.initial_weight() < 0.0f)
				{
					bValid = false;
					diagnostic = "A Global Illumination ECS probe binding is invalid.";
					break;
				}
				binding.m_initialWeight = probe.initial_weight();
				binding.m_bPreload = probe.preload();
				if (!nativeSettings.m_probes.Insert(probe.name(), std::move(binding)))
				{
					bValid = false;
					diagnostic = "Global Illumination ECS probe binding names must be unique.";
					break;
				}
			}
			if (!bValid || !Sailor::App::SetEditorGISettings(std::move(nativeSettings), diagnostic))
			{
				SetError(
					response, diagnostic.empty() ? "Failed to update Global Illumination ECS settings." : diagnostic);
				break;
			}
			SetBoolResult(response, true);
			break;
		}

		case ProtocolRequest::kGetGlobalIlluminationState:
		{
			Sailor::EditorGlobalIlluminationState state;
			if (!Sailor::App::GetEditorGlobalIlluminationState(state))
			{
				SetError(response, "Global Illumination ECS is unavailable.");
				break;
			}
			SetSuccess(response);
			auto* result = response.mutable_global_illumination_state_result();
			result->set_max_probe_states_per_snapshot(state.m_maxProbeStatesPerSnapshot);
			result->set_diagnostic(state.m_diagnostic);
			result->set_composition_count(state.m_compositionCount);
			result->set_rejected_composition_count(state.m_rejectedCompositionCount);
			result->set_mode(ToProtocolGlobalIlluminationMode(state.m_mode));
			result->set_enabled(state.m_bEnabled);
			SetProtocolRuntimeGIProbesSettings(*result->mutable_runtime_probes(), state.m_runtimeSettings);
			auto* runtime = result->mutable_runtime_state();
			runtime->set_lifecycle(ToProtocolRuntimeGIProbesLifecycle(state.m_runtimeStatus.m_lifecycle));
			runtime->set_enabled(state.m_runtimeStatus.m_bEnabled);
			runtime->set_paused(state.m_runtimeStatus.m_bPaused);
			runtime->set_preview_enabled(state.m_bRuntimePreviewEnabled);
			runtime->set_preview_budget(ToProtocolRuntimeGIProbesPreviewBudget(state.m_runtimeEditorBudget));
			runtime->set_scene_generation(state.m_runtimeStatus.m_sceneGeneration);
			runtime->set_lighting_generation(state.m_runtimeStatus.m_lightingGeneration);
			runtime->set_published_revision(state.m_runtimeStatus.m_publishedRevision);
			runtime->set_capacity(state.m_runtimeStatus.m_capacity);
			runtime->set_active_probe_count(state.m_runtimeStatus.m_activeProbeCount);
			runtime->set_ready_probe_count(state.m_runtimeStatus.m_readyProbeCount);
			runtime->set_worker_count(state.m_runtimeStatus.m_workerCount);
			runtime->set_published_bytes(state.m_runtimeStatus.m_publishedBytes);
			runtime->set_coverage(state.m_runtimeStatus.m_coverage);
			runtime->set_refinement(state.m_runtimeStatus.m_refinement);
			runtime->set_diagnostic(state.m_runtimeStatus.m_diagnostic);
			for (const Sailor::GlobalIlluminationProbeState& probe : state.m_probes)
			{
				auto* protocolProbe = result->add_probes();
				protocolProbe->set_name(probe.m_name);
				protocolProbe->set_asset_file_id(probe.m_asset.ToString());
				protocolProbe->set_mode(ToProtocolGlobalIlluminationProbeMode(probe.m_mode));
				protocolProbe->set_weight(probe.m_weight);
				protocolProbe->set_residency(ToProtocolGlobalIlluminationProbeResidency(probe.m_residency));
				protocolProbe->set_asset_revision(probe.m_assetRevision);
				protocolProbe->set_diagnostic(probe.m_diagnostic);
			}
			break;
		}

		case ProtocolRequest::kSetRuntimeGiProbesPreview:
		{
			std::string diagnostic;
			if (!Sailor::App::SetEditorRuntimeGIProbesPreviewEnabled(
					request.set_runtime_gi_probes_preview().enabled(), diagnostic))
			{
				SetError(response, diagnostic.empty() ? "Failed to update Runtime GI probe preview." : diagnostic);
				break;
			}
			SetBoolResult(response, true);
			break;
		}

		case ProtocolRequest::kSetRuntimeGiProbesPaused:
		{
			std::string diagnostic;
			if (!Sailor::App::SetEditorRuntimeGIProbesPaused(
					request.set_runtime_gi_probes_paused().paused(), diagnostic))
			{
				SetError(response, diagnostic.empty() ? "Failed to update Runtime GI probe pause state." : diagnostic);
				break;
			}
			SetBoolResult(response, true);
			break;
		}

		case ProtocolRequest::kSetRuntimeGiProbesPreviewBudget:
		{
			Sailor::Settings::ERuntimeGIProbesEditorBudget budget{};
			if (!TryGetRuntimeGIProbesPreviewBudget(request.set_runtime_gi_probes_preview_budget().budget(), budget))
			{
				SetError(response, "Runtime GI preview budget is unsupported.");
				break;
			}
			std::string diagnostic;
			if (!Sailor::App::SetEditorRuntimeGIProbesBudget(budget, diagnostic))
			{
				SetError(response, diagnostic.empty() ? "Failed to update the Runtime GI preview budget." : diagnostic);
				break;
			}
			SetBoolResult(response, true);
			break;
		}

		case ProtocolRequest::kRestartRuntimeGiProbes:
		{
			std::string diagnostic;
			if (!Sailor::App::RestartEditorRuntimeGIProbes(diagnostic))
			{
				SetError(response, diagnostic.empty() ? "Failed to restart Runtime GI probes." : diagnostic);
				break;
			}
			SetBoolResult(response, true);
			break;
		}

		case ProtocolRequest::kRebuildRuntimeGiProbesScene:
		{
			std::string diagnostic;
			if (!Sailor::App::RebuildEditorRuntimeGIProbesScene(diagnostic))
			{
				SetError(response, diagnostic.empty() ? "Failed to rebuild the Runtime GI probe scene." : diagnostic);
				break;
			}
			SetBoolResult(response, true);
			break;
		}
		default:
			return false;
		}
		return true;
	}
}
