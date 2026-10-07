#include "EditorEngineProtocolInternal.h"

#include "Memory/UniquePtr.hpp"
#include "Editor/EditorRuntimeBridge.h"
#include "Editor/EditorViewportEvent.h"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Sailor.h"
#include "Settings/GraphicsSettings.h"

#include <cmath>
#include <string>
#include <type_traits>

namespace Sailor::Protocol::EditorEngineProtocolCommands
{
	using sailor::editor::v1::EditorRenderMode;
	using sailor::editor::v1::ProtocolRequest;
	using sailor::editor::v1::ProtocolResponse;
	using sailor::editor::v1::Vector4;
	using sailor::editor::v1::ViewportEvent;
	using sailor::editor::v1::ViewportTransformOperation;
	using sailor::editor::v1::ViewportTransformSpace;

	static bool TryGetSceneViewRenderMode(EditorRenderMode protocolMode, Sailor::RHI::ESceneViewRenderMode& outMode)
	{
		switch (protocolMode)
		{
		case sailor::editor::v1::EDITOR_RENDER_MODE_LIT:
			outMode = Sailor::RHI::ESceneViewRenderMode::Lit;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_AMBIENT_OCCLUSION:
			outMode = Sailor::RHI::ESceneViewRenderMode::AmbientOcclusion;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_CASCADES:
			outMode = Sailor::RHI::ESceneViewRenderMode::Cascades;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_LIGHT_TILES:
			outMode = Sailor::RHI::ESceneViewRenderMode::LightTiles;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_ONLY:
			outMode = Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationOnly;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_PROBES:
			outMode = Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationProbes;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_BRICKS:
			outMode = Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationBricks;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_VALIDITY:
			outMode = Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationValidity;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_VISIBILITY:
			outMode = Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationVisibility;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_RESIDENCY:
			outMode = Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationResidency;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_ASSET_IDENTITY:
			outMode = Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationAssetIdentity;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_FALLBACK:
			outMode = Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationFallback;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_SUBDIVISIONS:
			outMode = Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationSubdivisions;
			return true;
		case sailor::editor::v1::EDITOR_RENDER_MODE_UNSPECIFIED:
		default:
			return false;
		}
	}

	EditorRenderMode ToProtocolRenderMode(Sailor::RHI::ESceneViewRenderMode mode)
	{
		switch (mode)
		{
		case Sailor::RHI::ESceneViewRenderMode::AmbientOcclusion:
			return sailor::editor::v1::EDITOR_RENDER_MODE_AMBIENT_OCCLUSION;
		case Sailor::RHI::ESceneViewRenderMode::Cascades:
			return sailor::editor::v1::EDITOR_RENDER_MODE_CASCADES;
		case Sailor::RHI::ESceneViewRenderMode::LightTiles:
			return sailor::editor::v1::EDITOR_RENDER_MODE_LIGHT_TILES;
		case Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationOnly:
			return sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_ONLY;
		case Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationProbes:
			return sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_PROBES;
		case Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationBricks:
			return sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_BRICKS;
		case Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationValidity:
			return sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_VALIDITY;
		case Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationVisibility:
			return sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_VISIBILITY;
		case Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationResidency:
			return sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_RESIDENCY;
		case Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationAssetIdentity:
			return sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_ASSET_IDENTITY;
		case Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationFallback:
			return sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_FALLBACK;
		case Sailor::RHI::ESceneViewRenderMode::GlobalIlluminationSubdivisions:
			return sailor::editor::v1::EDITOR_RENDER_MODE_GLOBAL_ILLUMINATION_SUBDIVISIONS;
		case Sailor::RHI::ESceneViewRenderMode::Lit:
		default:
			return sailor::editor::v1::EDITOR_RENDER_MODE_LIT;
		}
	}

	static void SetUInt32Result(ProtocolResponse& response, uint32_t value)
	{
		SetSuccess(response);
		response.mutable_uint32_result()->set_value(value);
	}

	static void SetVector4Result(ProtocolResponse& response, float x, float y, float z, float w)
	{
		SetSuccess(response);
		auto* value = response.mutable_vector4_result()->mutable_value();
		value->set_x(x);
		value->set_y(y);
		value->set_z(z);
		value->set_w(w);
	}

	static void WriteVector4(Vector4& output, const glm::vec4& value)
	{
		output.set_x(value.x);
		output.set_y(value.y);
		output.set_z(value.z);
		output.set_w(value.w);
	}

	static ViewportTransformOperation ToProtocolOperation(EditorViewport::ETransformOperation operation)
	{
		using namespace sailor::editor::v1;
		switch (operation)
		{
		case EditorViewport::ETransformOperation::Select: return VIEWPORT_TRANSFORM_OPERATION_SELECT;
		case EditorViewport::ETransformOperation::Translate: return VIEWPORT_TRANSFORM_OPERATION_TRANSLATE;
		case EditorViewport::ETransformOperation::Rotate: return VIEWPORT_TRANSFORM_OPERATION_ROTATE;
		case EditorViewport::ETransformOperation::Scale: return VIEWPORT_TRANSFORM_OPERATION_SCALE;
		}
		return VIEWPORT_TRANSFORM_OPERATION_UNSPECIFIED;
	}

	static ViewportTransformSpace ToProtocolSpace(EditorViewport::ETransformSpace space)
	{
		using namespace sailor::editor::v1;
		switch (space)
		{
		case EditorViewport::ETransformSpace::World: return VIEWPORT_TRANSFORM_SPACE_WORLD;
		case EditorViewport::ETransformSpace::Local: return VIEWPORT_TRANSFORM_SPACE_LOCAL;
		}
		return VIEWPORT_TRANSFORM_SPACE_UNSPECIFIED;
	}

	static void WriteViewportEvent(const EditorViewport::Event& event, ViewportEvent& output)
	{
		output.set_revision(event.m_revision);
		output.set_managed_mutation_revision(event.m_managedMutationRevision);
		std::visit([&output](const auto& value)
		{
			using T = std::decay_t<decltype(value)>;
			if constexpr (std::is_same_v<T, EditorViewport::SelectionEvent>)
			{
				auto* selection = output.mutable_selection();
				if (value.m_instanceId) selection->set_selected_instance_id(value.m_instanceId.ToString());
			}
			else if constexpr (std::is_same_v<T, EditorViewport::AssetDropEvent>)
			{
				auto* drop = output.mutable_asset_drop();
				drop->set_file_id(value.m_fileId);
				drop->set_normalized_x(value.m_position.x);
				drop->set_normalized_y(value.m_position.y);
			}
			else if constexpr (std::is_same_v<T, EditorViewport::ToolShortcutEvent>)
			{
				output.mutable_tool_shortcut()->set_key_code(value.m_keyCode);
			}
			else if constexpr (std::is_same_v<T, EditorViewport::TransformEvent>)
			{
				auto* transform = output.mutable_transform();
				transform->set_instance_id(value.m_instanceId.ToString());
				transform->set_operation(ToProtocolOperation(value.m_operation));
				transform->set_space(ToProtocolSpace(value.m_space));
				const auto& beforeRotation = value.m_before.GetRotation();
				const auto& afterRotation = value.m_after.GetRotation();
				WriteVector4(*transform->mutable_before_position(), value.m_before.m_position);
				WriteVector4(*transform->mutable_before_rotation(),
					{ beforeRotation.x, beforeRotation.y, beforeRotation.z, beforeRotation.w });
				WriteVector4(*transform->mutable_before_scale(), value.m_before.m_scale);
				WriteVector4(*transform->mutable_after_position(), value.m_after.m_position);
				WriteVector4(*transform->mutable_after_rotation(),
					{ afterRotation.x, afterRotation.y, afterRotation.z, afterRotation.w });
				WriteVector4(*transform->mutable_after_scale(), value.m_after.m_scale);
			}
		}, event.m_payload);
	}

	static void DispatchViewportEvents(const sailor::editor::v1::CountRequest& request,
		ProtocolResponse& response,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		const uint32_t requestedCount = request.max_count();
		if (!ValidateBatchCount(requestedCount, response)) return;

		const auto events = dependencies.m_pullEditorViewportEvents
			? dependencies.m_pullEditorViewportEvents(dependencies.m_context, requestedCount)
			: Sailor::App::PullEditorViewportEvents(requestedCount);
		SetSuccess(response);
		auto* result = response.mutable_viewport_event_batch_result();
		for (const auto& event : events) WriteViewportEvent(event, *result->add_events());
	}

	static void DispatchRemoteViewportDiagnostics(const sailor::editor::v1::ViewportIdRequest& request,
		ProtocolResponse& response)
	{
		char* value = nullptr;
		const uint32_t length = Sailor::EditorRuntime::GetEditorRemoteViewportDiagnostics(request.viewport_id(), &value);
		Sailor::TUniquePtr<char[]> ownedValue(value);
		SetStringResult(response, value, length);
	}

	static void DispatchTraceViewportRay(const sailor::editor::v1::ViewportRayRequest& request,
		ProtocolResponse& response)
	{
		if (request.viewport_id() != 1u || !std::isfinite(request.normalized_x()) ||
			!std::isfinite(request.normalized_y()) || request.normalized_x() < 0.0f || request.normalized_x() > 1.0f ||
			request.normalized_y() < 0.0f || request.normalized_y() > 1.0f)
		{
			SetError(response, "The viewport ray request is invalid.");
			return;
		}

		float worldX = 0.0f;
		float worldY = 0.0f;
		float worldZ = 0.0f;
		if (!Sailor::App::TraceViewportRay(
				request.viewport_id(), request.normalized_x(), request.normalized_y(), worldX, worldY, worldZ))
		{
			SetError(response, "Failed to trace the viewport ray.");
			return;
		}

		SetVector4Result(response, worldX, worldY, worldZ, 1.0f);
	}

	static void DispatchGetViewportToolState(const sailor::editor::v1::ViewportIdRequest& request,
		ProtocolResponse& response)
	{
		if (request.viewport_id() == 0)
		{
			SetError(response, "The viewport id is invalid.");
			return;
		}

		uint32_t operation = 0;
		uint32_t space = 0;
		if (!Sailor::App::GetEditorViewportToolState(operation, space))
		{
			SetError(response, "Failed to read the viewport tool state.");
			return;
		}

		SetSuccess(response);
		auto* result = response.mutable_viewport_tool_state_result();
		result->set_operation(static_cast<ViewportTransformOperation>(operation));
		result->set_space(static_cast<ViewportTransformSpace>(space));
	}

	bool DispatchViewportCommand(const ProtocolRequest& request,
		ProtocolResponse& response,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		switch (request.command_case())
		{
		case ProtocolRequest::kSetEditorStatsMode:
		{
			Sailor::Settings::ERenderStatsMode statsMode{};
			switch (request.set_editor_stats_mode().mode())
			{
			case sailor::editor::v1::EDITOR_STATS_MODE_NONE:
				statsMode = Sailor::Settings::ERenderStatsMode::None;
				break;
			case sailor::editor::v1::EDITOR_STATS_MODE_RENDER_STATS:
				statsMode = Sailor::Settings::ERenderStatsMode::RenderStats;
				break;
			case sailor::editor::v1::EDITOR_STATS_MODE_RENDER_STATS_AND_QUERIES:
				statsMode = Sailor::Settings::ERenderStatsMode::RenderStatsAndQueries;
				break;
			case sailor::editor::v1::EDITOR_STATS_MODE_UNSPECIFIED:
			default:
				SetError(response, "The Editor stats mode is invalid.");
				return true;
			}

			SetBoolResult(response, Sailor::App::SetRenderStatsMode(statsMode));
			break;
		}

		case ProtocolRequest::kSetEditorRenderMode:
		{
			Sailor::RHI::ESceneViewRenderMode renderMode{};
			if (!TryGetSceneViewRenderMode(request.set_editor_render_mode().mode(), renderMode))
			{
				SetError(response, "The Editor render mode is invalid.");
				break;
			}

			SetBoolResult(response, Sailor::App::SetEditorRenderMode(renderMode));
			break;
		}

		case ProtocolRequest::kGetEditorRenderMode:
			SetSuccess(response);
			response.mutable_editor_render_mode_result()->set_mode(
				ToProtocolRenderMode(Sailor::App::GetEditorRenderMode()));
			break;

		case ProtocolRequest::kSetViewport:
		{
			const auto& viewport = request.set_viewport();
			Sailor::App::SetEditorViewport(
				viewport.window_pos_x(), viewport.window_pos_y(), viewport.width(), viewport.height());
			SetEmptyResult(response);
			break;
		}

		case ProtocolRequest::kSetEditorRenderTargetSize:
		{
			const auto& size = request.set_editor_render_target_size();
			Sailor::EditorRuntime::SetEditorRenderTargetSize(size.width(), size.height());
			SetEmptyResult(response);
			break;
		}

		case ProtocolRequest::kUpsertRemoteViewport:
		{
			const auto& viewport = request.upsert_remote_viewport();
			SetBoolResult(response,
				Sailor::EditorRuntime::UpsertEditorRemoteViewport(viewport.viewport_id(),
					viewport.window_pos_x(),
					viewport.window_pos_y(),
					viewport.width(),
					viewport.height(),
					viewport.visible(),
					viewport.focused()));
			break;
		}

		case ProtocolRequest::kDestroyRemoteViewport:
			SetBoolResult(
				response, Sailor::EditorRuntime::DestroyEditorRemoteViewport(request.destroy_remote_viewport().viewport_id()));
			break;

		case ProtocolRequest::kGetRemoteViewportState:
			SetUInt32Result(
				response, Sailor::EditorRuntime::GetEditorRemoteViewportState(request.get_remote_viewport_state().viewport_id()));
			break;

		case ProtocolRequest::kGetRemoteViewportDiagnostics:
			DispatchRemoteViewportDiagnostics(request.get_remote_viewport_diagnostics(), response);
			break;

		case ProtocolRequest::kCaptureRemoteViewportFrameEvidence:
		{
			std::string diagnostic;
			if (Sailor::EditorRuntime::CaptureEditorRemoteViewportFrameEvidence(request.capture_remote_viewport_frame_evidence().viewport_id(), diagnostic))
			{
				SetStringResult(response, diagnostic.data(), static_cast<uint32_t>(diagnostic.size()));
			}
			else
			{
				SetError(response, diagnostic);
			}
			break;
		}

		case ProtocolRequest::kRetryRemoteViewport:
			SetBoolResult(
				response, Sailor::EditorRuntime::RetryEditorRemoteViewport(request.retry_remote_viewport().viewport_id()));
			break;

		case ProtocolRequest::kSetRemoteViewportMacHostHandle:
		{
			const auto& host = request.set_remote_viewport_mac_host_handle();
			SetBoolResult(response,
				Sailor::EditorRuntime::SetEditorRemoteViewportMacHostHandle(
					host.viewport_id(), host.host_handle_kind(), host.host_handle_value()));
			break;
		}

		case ProtocolRequest::kSendRemoteViewportInput:
		{
			const auto& input = request.send_remote_viewport_input();
			SetBoolResult(response,
				Sailor::EditorRuntime::SendEditorRemoteViewportInput(input.viewport_id(),
					input.kind(),
					input.pointer_x(),
					input.pointer_y(),
					input.wheel_delta_x(),
					input.wheel_delta_y(),
					input.key_code(),
					input.button(),
					input.modifiers(),
					input.pressed(),
					input.focused(),
					input.captured(),
					input.text()));
			break;
		}

		case ProtocolRequest::kPullEditorViewportEvents:
			DispatchViewportEvents(request.pull_editor_viewport_events(), response, dependencies);
			break;

		case ProtocolRequest::kRenderPathTracedImage:
			SetError(response, "Path-traced image export is not supported by the editor.");
			break;

		case ProtocolRequest::kTraceViewportRay:
			DispatchTraceViewportRay(request.trace_viewport_ray(), response);
			break;

		case ProtocolRequest::kFocusEditorCamera:
		{
			const auto& focus = request.focus_editor_camera();
			SetBoolResult(
				response, focus.viewport_id() != 0 && Sailor::App::FocusEditorCamera(focus.instance_id().c_str()));
			break;
		}

		case ProtocolRequest::kSetViewportToolState:
		{
			const auto& state = request.set_viewport_tool_state();
			SetBoolResult(response,
				state.viewport_id() != 0 &&
					Sailor::App::SetEditorViewportToolState(
						static_cast<uint32_t>(state.operation()), static_cast<uint32_t>(state.space())));
			break;
		}

		case ProtocolRequest::kGetViewportToolState:
			DispatchGetViewportToolState(request.get_viewport_tool_state(), response);
			break;

		default:
			return false;
		}

		return true;
	}
}
