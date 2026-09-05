#include "EditorEngineProtocolInternal.h"

#include "Memory/UniquePtr.hpp"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Sailor.h"
#include "Settings/GraphicsSettings.h"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <string>
#include <vector>

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

	static bool TryReadVector4(const YAML::Node& node, Vector4& outVector, const char* fieldName, std::string& outError)
	{
		if (!node || !node.IsSequence() || node.size() != 4)
		{
			outError = std::string("Viewport event field '") + fieldName + "' must contain exactly four numbers.";
			return false;
		}

		float values[4]{};
		for (size_t i = 0; i < 4; ++i)
		{
			values[i] = node[i].as<float>();
			if (!std::isfinite(values[i]))
			{
				outError = std::string("Viewport event field '") + fieldName + "' contains a non-finite value.";
				return false;
			}
		}

		outVector.set_x(values[0]);
		outVector.set_y(values[1]);
		outVector.set_z(values[2]);
		outVector.set_w(values[3]);
		return true;
	}

	static bool TryParseTransformOperation(const std::string& value, ViewportTransformOperation& outOperation)
	{
		using namespace sailor::editor::v1;
		if (value == "Select")
		{
			outOperation = VIEWPORT_TRANSFORM_OPERATION_SELECT;
			return true;
		}
		if (value == "Translate")
		{
			outOperation = VIEWPORT_TRANSFORM_OPERATION_TRANSLATE;
			return true;
		}
		if (value == "Rotate")
		{
			outOperation = VIEWPORT_TRANSFORM_OPERATION_ROTATE;
			return true;
		}
		if (value == "Scale")
		{
			outOperation = VIEWPORT_TRANSFORM_OPERATION_SCALE;
			return true;
		}

		return false;
	}

	static bool TryParseTransformSpace(const std::string& value, ViewportTransformSpace& outSpace)
	{
		using namespace sailor::editor::v1;
		if (value == "World")
		{
			outSpace = VIEWPORT_TRANSFORM_SPACE_WORLD;
			return true;
		}
		if (value == "Local")
		{
			outSpace = VIEWPORT_TRANSFORM_SPACE_LOCAL;
			return true;
		}

		return false;
	}

	static bool TryConvertViewportEventUnchecked(const char* serializedEvent,
		ViewportEvent& outEvent,
		std::string& outError)
	{
		if (!serializedEvent || serializedEvent[0] == '\0')
		{
			outError = "The native viewport event is empty.";
			return false;
		}

		const YAML::Node event = YAML::Load(serializedEvent);
		if (!event || !event.IsMap())
		{
			outError = "The native viewport event must be a YAML mapping.";
			return false;
		}

		const YAML::Node kindNode = event["kind"];
		const YAML::Node revisionNode = event["revision"];
		const YAML::Node managedMutationRevisionNode = event["managedMutationRevision"];
		if (!kindNode.IsScalar() || !revisionNode.IsScalar() || !managedMutationRevisionNode.IsScalar())
		{
			outError = "The native viewport event is missing its envelope fields.";
			return false;
		}

		const std::string kind = kindNode.as<std::string>();
		outEvent.set_revision(revisionNode.as<uint64_t>());
		outEvent.set_managed_mutation_revision(managedMutationRevisionNode.as<uint64_t>());

		if (kind == "selection")
		{
			const YAML::Node selectedInstanceIdNode = event["selectedInstanceId"];
			if (!selectedInstanceIdNode.IsScalar())
			{
				outError = "The native viewport selection event is missing selectedInstanceId.";
				return false;
			}

			outEvent.mutable_selection()->set_selected_instance_id(selectedInstanceIdNode.as<std::string>());
			return true;
		}

		if (kind == "assetDrop")
		{
			const YAML::Node fileIdNode = event["fileId"];
			const YAML::Node normalizedXNode = event["normalizedX"];
			const YAML::Node normalizedYNode = event["normalizedY"];
			if (!fileIdNode.IsScalar() || !normalizedXNode.IsScalar() || !normalizedYNode.IsScalar())
			{
				outError = "The native viewport asset drop event is missing scalar fields.";
				return false;
			}

			const float normalizedX = normalizedXNode.as<float>();
			const float normalizedY = normalizedYNode.as<float>();
			if (!std::isfinite(normalizedX) || !std::isfinite(normalizedY) || normalizedX < 0.0f ||
				normalizedX > 1.0f || normalizedY < 0.0f || normalizedY > 1.0f)
			{
				outError = "The native viewport asset drop coordinates are invalid.";
				return false;
			}

			auto* assetDrop = outEvent.mutable_asset_drop();
			assetDrop->set_file_id(fileIdNode.as<std::string>());
			assetDrop->set_normalized_x(normalizedX);
			assetDrop->set_normalized_y(normalizedY);
			return true;
		}

		if (kind == "toolShortcut")
		{
			const YAML::Node keyCodeNode = event["keyCode"];
			if (!keyCodeNode.IsScalar())
			{
				outError = "The native viewport tool shortcut event is missing keyCode.";
				return false;
			}

			const uint32_t keyCode = keyCodeNode.as<uint32_t>();
			if (keyCode != 'Q' && keyCode != 'W' && keyCode != 'E' && keyCode != 'R' && keyCode != 'T')
			{
				outError = "The native viewport tool shortcut key is unsupported.";
				return false;
			}

			outEvent.mutable_tool_shortcut()->set_key_code(keyCode);
			return true;
		}

		if (kind != "transform")
		{
			outError = "Unsupported native viewport event kind '" + kind + "'.";
			return false;
		}

		const YAML::Node instanceIdNode = event["instanceId"];
		const YAML::Node operationNode = event["operation"];
		const YAML::Node spaceNode = event["space"];
		if (!instanceIdNode.IsScalar() || !operationNode.IsScalar() || !spaceNode.IsScalar())
		{
			outError = "The native viewport transform event is missing scalar fields.";
			return false;
		}

		auto* transform = outEvent.mutable_transform();
		transform->set_instance_id(instanceIdNode.as<std::string>());

		ViewportTransformOperation operation{};
		const std::string operationValue = operationNode.as<std::string>();
		if (!TryParseTransformOperation(operationValue, operation))
		{
			outError = "Unsupported viewport transform operation '" + operationValue + "'.";
			return false;
		}
		transform->set_operation(operation);

		ViewportTransformSpace space{};
		const std::string spaceValue = spaceNode.as<std::string>();
		if (!TryParseTransformSpace(spaceValue, space))
		{
			outError = "Unsupported viewport transform space '" + spaceValue + "'.";
			return false;
		}
		transform->set_space(space);

		return TryReadVector4(
				   event["beforePosition"], *transform->mutable_before_position(), "beforePosition", outError) &&
			   TryReadVector4(
				   event["beforeRotation"], *transform->mutable_before_rotation(), "beforeRotation", outError) &&
			   TryReadVector4(event["beforeScale"], *transform->mutable_before_scale(), "beforeScale", outError) &&
			   TryReadVector4(
				   event["afterPosition"], *transform->mutable_after_position(), "afterPosition", outError) &&
			   TryReadVector4(
				   event["afterRotation"], *transform->mutable_after_rotation(), "afterRotation", outError) &&
			   TryReadVector4(event["afterScale"], *transform->mutable_after_scale(), "afterScale", outError);
	}

	static bool TryConvertViewportEvent(const char* serializedEvent, ViewportEvent& outEvent, std::string& outError)
	{
		try
		{
			return TryConvertViewportEventUnchecked(serializedEvent, outEvent, outError);
		}
		catch (const YAML::Exception& exception)
		{
			outEvent.Clear();
			outError = "Failed to parse the native viewport event: " + std::string(exception.what());
			return false;
		}
	}

	static void DispatchViewportEvents(const sailor::editor::v1::CountRequest& request,
		ProtocolResponse& response,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		const uint32_t requestedCount = request.max_count();
		if (!ValidateBatchCount(requestedCount, response))
		{
			return;
		}

		auto events =
			requestedCount > 0 ? Sailor::TUniquePtr<char*[]>::Make(requestedCount) : Sailor::TUniquePtr<char*[]>{};
		std::vector<Sailor::TUniquePtr<char[]>> ownedEvents;
		ownedEvents.reserve(requestedCount);
		const uint32_t numEvents =
			dependencies.m_pullEditorViewportEvents
				? dependencies.m_pullEditorViewportEvents(dependencies.m_context, events.GetRawPtr(), requestedCount)
				: Sailor::App::PullEditorViewportEvents(events.GetRawPtr(), requestedCount);
		for (uint32_t i = 0; i < requestedCount; ++i)
		{
			ownedEvents.emplace_back(events[i]);
		}
		if (numEvents > requestedCount)
		{
			SetError(response, "The native viewport event source exceeded the requested batch capacity.");
			return;
		}

		SetSuccess(response);
		auto* result = response.mutable_viewport_event_batch_result();
		for (uint32_t i = 0; i < numEvents; ++i)
		{
			std::string error;
			ViewportEvent event;
			if (!TryConvertViewportEvent(events[i], event, error))
			{
				continue;
			}

			result->add_events()->CopyFrom(event);
		}
	}

	static void DispatchRemoteViewportDiagnostics(const sailor::editor::v1::ViewportIdRequest& request,
		ProtocolResponse& response)
	{
		char* value = nullptr;
		const uint32_t length = Sailor::App::GetEditorRemoteViewportDiagnostics(request.viewport_id(), &value);
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
			Sailor::App::SetEditorRenderTargetSize(size.width(), size.height());
			SetEmptyResult(response);
			break;
		}

		case ProtocolRequest::kUpsertRemoteViewport:
		{
			const auto& viewport = request.upsert_remote_viewport();
			SetBoolResult(response,
				Sailor::App::UpsertEditorRemoteViewport(viewport.viewport_id(),
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
				response, Sailor::App::DestroyEditorRemoteViewport(request.destroy_remote_viewport().viewport_id()));
			break;

		case ProtocolRequest::kGetRemoteViewportState:
			SetUInt32Result(
				response, Sailor::App::GetEditorRemoteViewportState(request.get_remote_viewport_state().viewport_id()));
			break;

		case ProtocolRequest::kGetRemoteViewportDiagnostics:
			DispatchRemoteViewportDiagnostics(request.get_remote_viewport_diagnostics(), response);
			break;

		case ProtocolRequest::kRetryRemoteViewport:
			SetBoolResult(
				response, Sailor::App::RetryEditorRemoteViewport(request.retry_remote_viewport().viewport_id()));
			break;

		case ProtocolRequest::kSetRemoteViewportMacHostHandle:
		{
			const auto& host = request.set_remote_viewport_mac_host_handle();
			SetBoolResult(response,
				Sailor::App::SetEditorRemoteViewportMacHostHandle(
					host.viewport_id(), host.host_handle_kind(), host.host_handle_value()));
			break;
		}

		case ProtocolRequest::kSendRemoteViewportInput:
		{
			const auto& input = request.send_remote_viewport_input();
			SetBoolResult(response,
				Sailor::App::SendEditorRemoteViewportInput(input.viewport_id(),
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
					input.captured()));
			break;
		}

		case ProtocolRequest::kPullEditorViewportEvents:
			DispatchViewportEvents(request.pull_editor_viewport_events(), response, dependencies);
			break;

		case ProtocolRequest::kRenderPathTracedImage:
		{
			const auto& render = request.render_path_traced_image();
			SetBoolResult(response,
				Sailor::App::RenderPathTracedImage(render.output_path().c_str(),
					render.instance_id().c_str(),
					render.height(),
					render.samples_per_pixel(),
					render.max_bounces()));
			break;
		}

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
