#include "EditorEngineProtocolInternal.h"

#include "Memory/UniquePtr.hpp"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Sailor.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <yaml-cpp/yaml.h>

#include <cmath>
#include <limits>
#include <new>
#include <string>

namespace
{
	using Sailor::Protocol::EEditorEngineTransportStatus;
	using Sailor::Protocol::EditorEngineProtocolMaxPayloadSize;
	using Sailor::Protocol::EditorEngineProtocolVersion;
	using sailor::editor::v1::ProtocolRequest;
	using sailor::editor::v1::ProtocolResponse;
	using sailor::editor::v1::Vector4;
	using sailor::editor::v1::ViewportEvent;
	using sailor::editor::v1::ViewportTransformOperation;
	using sailor::editor::v1::ViewportTransformSpace;

	constexpr uint32_t c_maxBatchItems = 65536u;

	class TInteropString final
	{
	public:
		TInteropString() = default;
		TInteropString(const TInteropString&) = delete;
		TInteropString& operator=(const TInteropString&) = delete;

		~TInteropString()
		{
			delete[] m_value;
		}

		char** GetOutput()
		{
			return &m_value;
		}

		const char* GetValue() const
		{
			return m_value;
		}

	private:
		char* m_value = nullptr;
	};

	class TInteropStringArray final
	{
	public:
		explicit TInteropStringArray(uint32_t capacity)
			: m_values(capacity > 0 ? Sailor::TUniquePtr<char*[]>::Make(capacity) : Sailor::TUniquePtr<char*[]>{})
			, m_capacity(capacity)
		{
			for (uint32_t i = 0; i < m_capacity; ++i)
			{
				m_values[i] = nullptr;
			}
		}

		TInteropStringArray(const TInteropStringArray&) = delete;
		TInteropStringArray& operator=(const TInteropStringArray&) = delete;

		~TInteropStringArray()
		{
			for (uint32_t i = 0; i < m_capacity; ++i)
			{
				delete[] m_values[i];
			}
		}

		char** GetValues()
		{
			return m_values.GetRawPtr();
		}

		const char* operator[](uint32_t index) const
		{
			return m_values[index];
		}

	private:
		Sailor::TUniquePtr<char*[]> m_values;
		uint32_t m_capacity = 0;
	};

	void SetError(ProtocolResponse& response, const std::string& error)
	{
		response.set_success(false);
		response.set_error(error);
		response.clear_result();
	}

	void SetSuccess(ProtocolResponse& response)
	{
		response.set_success(true);
		response.clear_error();
	}

	void SetEmptyResult(ProtocolResponse& response)
	{
		SetSuccess(response);
		response.mutable_empty_result();
	}

	void SetBoolResult(ProtocolResponse& response, bool value)
	{
		SetSuccess(response);
		response.mutable_bool_result()->set_value(value);
	}

	void SetUInt32Result(ProtocolResponse& response, uint32_t value)
	{
		SetSuccess(response);
		response.mutable_uint32_result()->set_value(value);
	}

	void SetUInt64Result(ProtocolResponse& response, uint64_t value)
	{
		SetSuccess(response);
		response.mutable_uint64_result()->set_value(value);
	}

	void SetStringResult(
		ProtocolResponse& response,
		const char* value,
		uint32_t length)
	{
		SetSuccess(response);
		auto* result = response.mutable_string_result();
		result->set_has_value(value != nullptr);
		if (value)
		{
			result->set_value(value, length);
		}
	}

	bool ValidateBatchCount(uint32_t count, ProtocolResponse& response)
	{
		if (count <= c_maxBatchItems)
		{
			return true;
		}

		SetError(response, "Requested batch count exceeds the native protocol limit.");
		return false;
	}

	bool TryFindEmbeddedNull(
		const google::protobuf::Message& message,
		std::string& outFieldName)
	{
		const google::protobuf::Descriptor* descriptor = message.GetDescriptor();
		const google::protobuf::Reflection* reflection = message.GetReflection();
		if (!descriptor || !reflection)
		{
			return false;
		}

		for (int fieldIndex = 0; fieldIndex < descriptor->field_count(); ++fieldIndex)
		{
			const google::protobuf::FieldDescriptor* field =
				descriptor->field(fieldIndex);
			if (field->is_repeated())
			{
				const int fieldSize = reflection->FieldSize(message, field);
				for (int valueIndex = 0; valueIndex < fieldSize; ++valueIndex)
				{
					if (field->cpp_type() ==
						google::protobuf::FieldDescriptor::CPPTYPE_STRING)
					{
						std::string scratch;
						const std::string& value =
							reflection->GetRepeatedStringReference(
								message,
								field,
								valueIndex,
								&scratch);
						if (value.find('\0') != std::string::npos)
						{
							outFieldName = field->full_name();
							return true;
						}
					}
					else if (field->cpp_type() ==
						google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
						TryFindEmbeddedNull(
							reflection->GetRepeatedMessage(
								message,
								field,
								valueIndex),
							outFieldName))
					{
						return true;
					}
				}
				continue;
			}

			if (field->cpp_type() ==
				google::protobuf::FieldDescriptor::CPPTYPE_STRING)
			{
				std::string scratch;
				const std::string& value = reflection->GetStringReference(
					message,
					field,
					&scratch);
				if (value.find('\0') != std::string::npos)
				{
					outFieldName = field->full_name();
					return true;
				}
			}
			else if (field->cpp_type() ==
				google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
				reflection->HasField(message, field) &&
				TryFindEmbeddedNull(
					reflection->GetMessage(message, field),
					outFieldName))
			{
				return true;
			}
		}

		return false;
	}

	bool TryReadVector4(
		const YAML::Node& node,
		Vector4& outVector,
		const char* fieldName,
		std::string& outError)
	{
		if (!node || !node.IsSequence() || node.size() != 4)
		{
			outError = std::string("Viewport event field '") + fieldName +
				"' must contain exactly four numbers.";
			return false;
		}

		float values[4]{};
		for (size_t i = 0; i < 4; ++i)
		{
			values[i] = node[i].as<float>();
			if (!std::isfinite(values[i]))
			{
				outError = std::string("Viewport event field '") + fieldName +
					"' contains a non-finite value.";
				return false;
			}
		}

		outVector.set_x(values[0]);
		outVector.set_y(values[1]);
		outVector.set_z(values[2]);
		outVector.set_w(values[3]);
		return true;
	}

	bool TryParseTransformOperation(
		const std::string& value,
		ViewportTransformOperation& outOperation)
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

	bool TryParseTransformSpace(
		const std::string& value,
		ViewportTransformSpace& outSpace)
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

	bool TryConvertViewportEventUnchecked(
		const char* serializedEvent,
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
		if (!kindNode.IsScalar() ||
			!revisionNode.IsScalar() ||
			!managedMutationRevisionNode.IsScalar())
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

			outEvent.mutable_selection()->set_selected_instance_id(
				selectedInstanceIdNode.as<std::string>());
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
		if (!instanceIdNode.IsScalar() ||
			!operationNode.IsScalar() ||
			!spaceNode.IsScalar())
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

		return
			TryReadVector4(event["beforePosition"], *transform->mutable_before_position(), "beforePosition", outError) &&
			TryReadVector4(event["beforeRotation"], *transform->mutable_before_rotation(), "beforeRotation", outError) &&
			TryReadVector4(event["beforeScale"], *transform->mutable_before_scale(), "beforeScale", outError) &&
			TryReadVector4(event["afterPosition"], *transform->mutable_after_position(), "afterPosition", outError) &&
			TryReadVector4(event["afterRotation"], *transform->mutable_after_rotation(), "afterRotation", outError) &&
			TryReadVector4(event["afterScale"], *transform->mutable_after_scale(), "afterScale", outError);
	}

	bool TryConvertViewportEvent(
		const char* serializedEvent,
		ViewportEvent& outEvent,
		std::string& outError)
	{
		try
		{
			return TryConvertViewportEventUnchecked(
				serializedEvent,
				outEvent,
				outError);
		}
		catch (const YAML::Exception& exception)
		{
			outEvent.Clear();
			outError =
				"Failed to parse the native viewport event: " +
				std::string(exception.what());
			return false;
		}
	}

	void DispatchInitialize(
		const sailor::editor::v1::InitializeRequest& request,
		ProtocolResponse& response)
	{
		const int numArguments = request.arguments_size();
		Sailor::TUniquePtr<const char*[]> arguments{};
		if (numArguments > 0)
		{
			arguments = Sailor::TUniquePtr<const char*[]>::Make(
				static_cast<size_t>(numArguments));
			for (int i = 0; i < numArguments; ++i)
			{
				arguments[i] = request.arguments(i).c_str();
			}
		}

		Sailor::App::Initialize(arguments.GetRawPtr(), numArguments);
		SetEmptyResult(response);
	}

	void DispatchMessages(
		const sailor::editor::v1::CountRequest& request,
		ProtocolResponse& response)
	{
		const uint32_t requestedCount = request.max_count();
		if (!ValidateBatchCount(requestedCount, response))
		{
			return;
		}

		TInteropStringArray messages(requestedCount);
		const uint32_t numMessages = Sailor::App::PullEditorMessages(
			messages.GetValues(),
			requestedCount);
		if (numMessages > requestedCount)
		{
			SetError(
				response,
				"The native message source exceeded the requested batch capacity.");
			return;
		}

		SetSuccess(response);
		auto* result = response.mutable_string_list_result();
		for (uint32_t i = 0; i < numMessages; ++i)
		{
			result->add_values(messages[i] ? messages[i] : "");
		}
	}

	void DispatchViewportEvents(
		const sailor::editor::v1::CountRequest& request,
		ProtocolResponse& response,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		const uint32_t requestedCount = request.max_count();
		if (!ValidateBatchCount(requestedCount, response))
		{
			return;
		}

		TInteropStringArray events(requestedCount);
		const uint32_t numEvents = dependencies.m_pullEditorViewportEvents
			? dependencies.m_pullEditorViewportEvents(
				dependencies.m_context,
				events.GetValues(),
				requestedCount)
			: Sailor::App::PullEditorViewportEvents(
				events.GetValues(),
				requestedCount);
		if (numEvents > requestedCount)
		{
			SetError(
				response,
				"The native viewport event source exceeded the requested batch capacity.");
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

	void DispatchSerializeCurrentWorld(ProtocolResponse& response)
	{
		TInteropString value;
		const uint32_t length = Sailor::App::SerializeCurrentWorld(value.GetOutput());
		SetStringResult(response, value.GetValue(), length);
	}

	void DispatchSerializeEngineTypes(ProtocolResponse& response)
	{
		TInteropString value;
		const uint32_t length = Sailor::App::SerializeEngineTypes(value.GetOutput());
		SetStringResult(response, value.GetValue(), length);
	}

	void DispatchSerializeEditorTypes(ProtocolResponse& response)
	{
		TInteropString value;
		const uint32_t length = Sailor::App::SerializeEditorTypes(value.GetOutput());
		SetStringResult(response, value.GetValue(), length);
	}

	void DispatchSerializeWorkspaceCacheIdentity(ProtocolResponse& response)
	{
		TInteropString value;
		const uint32_t length = Sailor::App::SerializeWorkspaceCacheIdentity(value.GetOutput());
		SetStringResult(response, value.GetValue(), length);
	}

	void DispatchRemoteViewportDiagnostics(
		const sailor::editor::v1::ViewportIdRequest& request,
		ProtocolResponse& response)
	{
		TInteropString value;
		const uint32_t length = Sailor::App::GetEditorRemoteViewportDiagnostics(
			request.viewport_id(),
			value.GetOutput());
		SetStringResult(response, value.GetValue(), length);
	}

	void DispatchCreateGameObject(
		const sailor::editor::v1::CreateGameObjectRequest& request,
		ProtocolResponse& response)
	{
		TInteropString instanceId;
		const bool bSucceeded = Sailor::App::CreateEditorGameObject(
			request.parent_instance_id().c_str(),
			request.preferred_instance_id().c_str(),
			instanceId.GetOutput());

		SetSuccess(response);
		auto* result = response.mutable_instance_id_result();
		result->set_succeeded(bSucceeded);
		if (instanceId.GetValue())
		{
			result->set_instance_id(instanceId.GetValue());
		}
	}

	void DispatchAddComponent(
		const sailor::editor::v1::AddComponentRequest& request,
		ProtocolResponse& response)
	{
		TInteropString instanceId;
		const bool bSucceeded = Sailor::App::AddEditorComponent(
			request.instance_id().c_str(),
			request.component_type_name().c_str(),
			request.preferred_instance_id().c_str(),
			instanceId.GetOutput());

		SetSuccess(response);
		auto* result = response.mutable_instance_id_result();
		result->set_succeeded(bSucceeded);
		if (instanceId.GetValue())
		{
			result->set_instance_id(instanceId.GetValue());
		}
	}

	void DispatchSelection(
		const sailor::editor::v1::SelectionRequest& request,
		ProtocolResponse& response)
	{
		YAML::Node selection(YAML::NodeType::Sequence);
		for (const auto& instanceId : request.instance_ids())
		{
			selection.push_back(instanceId);
		}

		const std::string serializedSelection = YAML::Dump(selection);
		SetBoolResult(
			response,
			Sailor::App::SetEditorSelection(serializedSelection.c_str()));
	}

	void DispatchRequest(
		const ProtocolRequest& request,
		ProtocolResponse& response,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		switch (request.command_case())
		{
		case ProtocolRequest::kInitialize:
			DispatchInitialize(request.initialize(), response);
			break;

		case ProtocolRequest::kStart:
			Sailor::App::Start();
			SetEmptyResult(response);
			break;

		case ProtocolRequest::kStop:
			Sailor::App::Stop();
			SetEmptyResult(response);
			break;

		case ProtocolRequest::kShutdown:
			Sailor::App::Shutdown();
			SetEmptyResult(response);
			break;

		case ProtocolRequest::kRequestAssetReload:
			SetBoolResult(response, Sailor::App::RequestAssetReload());
			break;

		case ProtocolRequest::kGetAssetReloadState:
		{
			uint64_t requestGeneration = 0;
			uint64_t completedGeneration = 0;
			uint64_t successfulGeneration = 0;
			const bool bAvailable = Sailor::App::GetAssetReloadState(
				requestGeneration,
				completedGeneration,
				successfulGeneration);
			SetSuccess(response);
			auto* result = response.mutable_asset_reload_state_result();
			result->set_available(bAvailable);
			result->set_request_generation(requestGeneration);
			result->set_completed_generation(completedGeneration);
			result->set_successful_generation(successfulGeneration);
			break;
		}

		case ProtocolRequest::kGetExitCode:
			SetSuccess(response);
			response.mutable_int32_result()->set_value(Sailor::App::GetExitCode());
			break;

		case ProtocolRequest::kGetMessages:
			DispatchMessages(request.get_messages(), response);
			break;

		case ProtocolRequest::kSerializeCurrentWorld:
			DispatchSerializeCurrentWorld(response);
			break;

		case ProtocolRequest::kSerializeEditorTypes:
			DispatchSerializeEditorTypes(response);
			break;

		case ProtocolRequest::kSerializeWorkspaceCacheIdentity:
			DispatchSerializeWorkspaceCacheIdentity(response);
			break;

		case ProtocolRequest::kLoadEditorWorld:
			SetBoolResult(
				response,
				Sailor::App::LoadEditorWorld(
					request.load_editor_world().file_id().c_str()));
			break;

		case ProtocolRequest::kCreateEditorWorld:
			SetBoolResult(response, Sailor::App::CreateEditorWorld());
			break;

		case ProtocolRequest::kSetViewport:
		{
			const auto& viewport = request.set_viewport();
			Sailor::App::SetEditorViewport(
				viewport.window_pos_x(),
				viewport.window_pos_y(),
				viewport.width(),
				viewport.height());
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
			SetBoolResult(
				response,
				Sailor::App::UpsertEditorRemoteViewport(
					viewport.viewport_id(),
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
				response,
				Sailor::App::DestroyEditorRemoteViewport(
					request.destroy_remote_viewport().viewport_id()));
			break;

		case ProtocolRequest::kGetRemoteViewportState:
			SetUInt32Result(
				response,
				Sailor::App::GetEditorRemoteViewportState(
					request.get_remote_viewport_state().viewport_id()));
			break;

		case ProtocolRequest::kGetRemoteViewportDiagnostics:
			DispatchRemoteViewportDiagnostics(
				request.get_remote_viewport_diagnostics(),
				response);
			break;

		case ProtocolRequest::kRetryRemoteViewport:
			SetBoolResult(
				response,
				Sailor::App::RetryEditorRemoteViewport(
					request.retry_remote_viewport().viewport_id()));
			break;

		case ProtocolRequest::kSetRemoteViewportMacHostHandle:
		{
			const auto& host = request.set_remote_viewport_mac_host_handle();
			SetBoolResult(
				response,
				Sailor::App::SetEditorRemoteViewportMacHostHandle(
					host.viewport_id(),
					host.host_handle_kind(),
					host.host_handle_value()));
			break;
		}

		case ProtocolRequest::kSendRemoteViewportInput:
		{
			const auto& input = request.send_remote_viewport_input();
			SetBoolResult(
				response,
				Sailor::App::SendEditorRemoteViewportInput(
					input.viewport_id(),
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
			DispatchViewportEvents(
				request.pull_editor_viewport_events(),
				response,
				dependencies);
			break;

		case ProtocolRequest::kGetEditorManagedMutationRevision:
		{
			const auto& mutation = request.get_editor_managed_mutation_revision();
			SetUInt64Result(
				response,
				Sailor::App::GetEditorManagedMutationRevision(
					mutation.kind(),
					mutation.instance_id().c_str()));
			break;
		}

		case ProtocolRequest::kUpdateObject:
		{
			const auto& update = request.update_object();
			SetBoolResult(
				response,
				Sailor::App::UpdateEditorObject(
					update.instance_id().c_str(),
					update.yaml_changes().c_str()));
			break;
		}

		case ProtocolRequest::kReparentObject:
		{
			const auto& reparent = request.reparent_object();
			SetBoolResult(
				response,
				Sailor::App::ReparentEditorObject(
					reparent.instance_id().c_str(),
					reparent.parent_instance_id().c_str(),
					reparent.keep_world_transform()));
			break;
		}

		case ProtocolRequest::kCreateGameObject:
			DispatchCreateGameObject(request.create_game_object(), response);
			break;

		case ProtocolRequest::kDestroyObject:
			SetBoolResult(
				response,
				Sailor::App::DestroyEditorObject(
					request.destroy_object().instance_id().c_str()));
			break;

		case ProtocolRequest::kResetComponentToDefaults:
			SetBoolResult(
				response,
				Sailor::App::ResetEditorComponentToDefaults(
					request.reset_component_to_defaults().instance_id().c_str()));
			break;

		case ProtocolRequest::kAddComponent:
			DispatchAddComponent(request.add_component(), response);
			break;

		case ProtocolRequest::kRemoveComponent:
			SetBoolResult(
				response,
				Sailor::App::RemoveEditorComponent(
					request.remove_component().instance_id().c_str()));
			break;

		case ProtocolRequest::kInstantiatePrefab:
		{
			const auto& instantiate = request.instantiate_prefab();
			SetBoolResult(
				response,
				Sailor::App::InstantiateEditorPrefab(
					instantiate.file_id().c_str(),
					instantiate.parent_instance_id().c_str()));
			break;
		}

		case ProtocolRequest::kInstantiatePrefabFromYaml:
		{
			const auto& instantiate = request.instantiate_prefab_from_yaml();
			SetBoolResult(
				response,
				Sailor::App::InstantiateEditorPrefabFromYaml(
					instantiate.prefab_yaml().c_str(),
					instantiate.parent_instance_id().c_str()));
			break;
		}

		case ProtocolRequest::kSetEditorSelection:
			DispatchSelection(request.set_editor_selection(), response);
			break;

		case ProtocolRequest::kShowMainWindow:
			Sailor::App::ShowMainWindow(request.show_main_window().show());
			SetEmptyResult(response);
			break;

		case ProtocolRequest::kRenderPathTracedImage:
		{
			const auto& render = request.render_path_traced_image();
			SetBoolResult(
				response,
				Sailor::App::RenderPathTracedImage(
					render.output_path().c_str(),
					render.instance_id().c_str(),
					render.height(),
					render.samples_per_pixel(),
					render.max_bounces()));
			break;
		}

		case ProtocolRequest::kSerializeEngineTypes:
			DispatchSerializeEngineTypes(response);
			break;

		case ProtocolRequest::COMMAND_NOT_SET:
		default:
			SetError(response, "Protocol command is not set or is unknown.");
			break;
		}
	}

	EEditorEngineTransportStatus SerializeResponse(
		const ProtocolResponse& response,
		uint8_t** responseData,
		uint32_t* responseSize)
	{
		const size_t serializedSize = response.ByteSizeLong();
		if (serializedSize > EditorEngineProtocolMaxPayloadSize)
		{
			return EEditorEngineTransportStatus::PayloadTooLarge;
		}
		if (serializedSize == 0 ||
			serializedSize > static_cast<size_t>((std::numeric_limits<int>::max)()))
		{
			return EEditorEngineTransportStatus::SerializeFailed;
		}

		Sailor::TUniquePtr<uint8_t[]> serializedResponse(
			new (std::nothrow) uint8_t[serializedSize]);
		if (!serializedResponse)
		{
			return EEditorEngineTransportStatus::AllocationFailed;
		}

		if (!response.SerializeToArray(
				serializedResponse.GetRawPtr(),
				static_cast<int>(serializedSize)))
		{
			return EEditorEngineTransportStatus::SerializeFailed;
		}

		*responseSize = static_cast<uint32_t>(serializedSize);
		*responseData = serializedResponse.Release();
		return EEditorEngineTransportStatus::Ok;
	}
}

int32_t Sailor::Protocol::InvokeEditorEngineProtocol(
	const uint8_t* requestData,
	uint32_t requestSize,
	uint8_t** responseData,
	uint32_t* responseSize)
{
	return InvokeEditorEngineProtocol(
		requestData,
		requestSize,
		responseData,
		responseSize,
		EditorEngineProtocolDependencies{});
}

int32_t Sailor::Protocol::InvokeEditorEngineProtocol(
	const uint8_t* requestData,
	uint32_t requestSize,
	uint8_t** responseData,
	uint32_t* responseSize,
	const EditorEngineProtocolDependencies& dependencies)
{
	if (responseData)
	{
		*responseData = nullptr;
	}
	if (responseSize)
	{
		*responseSize = 0;
	}

	if (!requestData ||
		requestSize == 0 ||
		!responseData ||
		!responseSize)
	{
		return static_cast<int32_t>(
			EEditorEngineTransportStatus::InvalidArguments);
	}

	if (requestSize > EditorEngineProtocolMaxPayloadSize)
	{
		return static_cast<int32_t>(
			EEditorEngineTransportStatus::PayloadTooLarge);
	}

	ProtocolRequest request;
	if (!request.ParseFromArray(requestData, static_cast<int>(requestSize)))
	{
		return static_cast<int32_t>(
			EEditorEngineTransportStatus::ParseFailed);
	}

	ProtocolResponse response;
	response.set_protocol_version(EditorEngineProtocolVersion);
	response.set_request_id(request.request_id());

	if (request.protocol_version() != EditorEngineProtocolVersion)
	{
		SetError(
			response,
			"Unsupported protocol version " +
			std::to_string(request.protocol_version()) +
			"; expected " +
			std::to_string(EditorEngineProtocolVersion) +
			".");
	}
	else if (request.request_id() == 0)
	{
		SetError(response, "Protocol request_id must be non-zero.");
	}
	else
	{
		std::string embeddedNullField;
		if (TryFindEmbeddedNull(request, embeddedNullField))
		{
			SetError(
				response,
				"Protocol string field '" +
				embeddedNullField +
				"' contains an embedded NUL byte.");
		}
		else
		{
			DispatchRequest(request, response, dependencies);
		}
	}

	return static_cast<int32_t>(
		SerializeResponse(response, responseData, responseSize));
}

void Sailor::Protocol::FreeEditorEngineProtocolBuffer(uint8_t* buffer) noexcept
{
	delete[] buffer;
}
