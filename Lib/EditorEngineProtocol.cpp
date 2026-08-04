#include "EditorEngineProtocolInternal.h"
#include "EditorEngineProtocolLifecycle.h"

#include "Memory/UniquePtr.hpp"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Sailor.h"
#include "Tasks/Scheduler.h"
#include "Tasks/Tasks.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <yaml-cpp/yaml.h>

#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

#if defined(GetMessage)
#undef GetMessage
#endif

bool Sailor::Protocol::DispatchEditorEngineProtocolOperationOnEditorThread(
	void*,
	const EditorEngineProtocolDependencies::FEditorEngineProtocolOperation operation,
	void* operationContext)
{
	if (!operation)
	{
		return false;
	}

	auto* scheduler = Sailor::App::GetSubmodule<Sailor::Tasks::Scheduler>();
	if (!scheduler)
	{
		return false;
	}

	if (scheduler->IsEditorThread())
	{
		operation(operationContext);
		return true;
	}

	auto task = Sailor::Tasks::CreateTask(
		"Editor protocol operation",
		[operation, operationContext]()
		{
			operation(operationContext);
		},
		Sailor::EThreadType::Editor);
	scheduler->Run(task);
	task->Wait();
	return task->IsFinished();
}

namespace
{
	using Sailor::Protocol::EEditorEngineTransportStatus;
	using Sailor::Protocol::EditorEngineProtocolMaxPayloadSize;
	using Sailor::Protocol::EditorEngineProtocolLatestVersion;
	using Sailor::Protocol::EditorEngineProtocolStrictInstanceIdsVersion;
	using Sailor::Protocol::EditorEngineProtocolVersion;
	using sailor::editor::v1::ProtocolRequest;
	using sailor::editor::v1::ProtocolResponse;
	using sailor::editor::v1::Vector4;
	using sailor::editor::v1::ViewportEvent;
	using sailor::editor::v1::ViewportTransformOperation;
	using sailor::editor::v1::ViewportTransformSpace;

	constexpr uint32_t c_maxBatchItems = 65536u;

	Sailor::Protocol::TEditorEngineProtocolLifecycleGate&
		GetEditorEngineProtocolLifecycleGate()
	{
		// Host-control exports may still be entered while the managed app is
		// coordinating process termination. Keep the synchronization object
		// alive until process reclamation; sessions are reset explicitly.
		static auto* const gate =
			new Sailor::Protocol::TEditorEngineProtocolLifecycleGate();
		return *gate;
	}

	void StartEngine(void*)
	{
		Sailor::App::Start();
	}

	void StopEngine(
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		if (dependencies.m_stop)
		{
			dependencies.m_stop(dependencies.m_context);
			return;
		}
		Sailor::App::Stop();
	}

	void ShutdownEngine(
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		if (dependencies.m_shutdown)
		{
			dependencies.m_shutdown(dependencies.m_context);
			return;
		}
		Sailor::App::Shutdown();
	}

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

	void SetVector4Result(
		ProtocolResponse& response,
		float x,
		float y,
		float z,
		float w)
	{
		SetSuccess(response);
		auto* value = response.mutable_vector4_result()->mutable_value();
		value->set_x(x);
		value->set_y(y);
		value->set_z(z);
		value->set_w(w);
	}

	void SetInstanceIdResult(
		ProtocolResponse& response,
		bool bSucceeded,
		const char* instanceId)
	{
		SetSuccess(response);
		auto* result = response.mutable_instance_id_result();
		result->set_succeeded(bSucceeded);
		if (instanceId)
		{
			result->set_instance_id(instanceId);
		}
	}

	bool IsFiniteVector4(const Vector4& value)
	{
		return std::isfinite(value.x()) &&
			std::isfinite(value.y()) &&
			std::isfinite(value.z()) &&
			std::isfinite(value.w());
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

		if (kind == "assetDrop")
		{
			const YAML::Node fileIdNode = event["fileId"];
			const YAML::Node normalizedXNode = event["normalizedX"];
			const YAML::Node normalizedYNode = event["normalizedY"];
			if (!fileIdNode.IsScalar() ||
				!normalizedXNode.IsScalar() ||
				!normalizedYNode.IsScalar())
			{
				outError =
					"The native viewport asset drop event is missing scalar fields.";
				return false;
			}

			const float normalizedX = normalizedXNode.as<float>();
			const float normalizedY = normalizedYNode.as<float>();
			if (!std::isfinite(normalizedX) ||
				!std::isfinite(normalizedY) ||
				normalizedX < 0.0f ||
				normalizedX > 1.0f ||
				normalizedY < 0.0f ||
				normalizedY > 1.0f)
			{
				outError =
					"The native viewport asset drop coordinates are invalid.";
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
				outError =
					"The native viewport tool shortcut event is missing keyCode.";
				return false;
			}

			const uint32_t keyCode = keyCodeNode.as<uint32_t>();
			if (keyCode != 'Q' &&
				keyCode != 'W' &&
				keyCode != 'E' &&
				keyCode != 'R' &&
				keyCode != 'T')
			{
				outError =
					"The native viewport tool shortcut key is unsupported.";
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

	void DispatchCreateModelInstance(
		const sailor::editor::v1::CreateModelInstanceRequest& request,
		ProtocolResponse& response)
	{
		if (request.model_file_id().empty() ||
			request.name().empty() ||
			(request.apply_world_position() &&
				(!request.has_world_position() ||
					!std::isfinite(request.world_position().x()) ||
					!std::isfinite(request.world_position().y()) ||
					!std::isfinite(request.world_position().z()))))
		{
			SetError(response, "The model instance request is invalid.");
			return;
		}

		TInteropString instanceId;
		const auto& worldPosition = request.world_position();
		const bool bSucceeded = Sailor::App::CreateEditorModelInstance(
			request.model_file_id().c_str(),
			request.name().c_str(),
			request.parent_instance_id().c_str(),
			request.create_hierarchy(),
			request.apply_world_position(),
			worldPosition.x(),
			worldPosition.y(),
			worldPosition.z(),
			request.preferred_instance_id().c_str(),
			instanceId.GetOutput());
		SetInstanceIdResult(response, bSucceeded, instanceId.GetValue());
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

	void DispatchTraceViewportRay(
		const sailor::editor::v1::ViewportRayRequest& request,
		ProtocolResponse& response)
	{
		if (request.viewport_id() != 1u ||
			!std::isfinite(request.normalized_x()) ||
			!std::isfinite(request.normalized_y()) ||
			request.normalized_x() < 0.0f ||
			request.normalized_x() > 1.0f ||
			request.normalized_y() < 0.0f ||
			request.normalized_y() > 1.0f)
		{
			SetError(response, "The viewport ray request is invalid.");
			return;
		}

		float worldX = 0.0f;
		float worldY = 0.0f;
		float worldZ = 0.0f;
		if (!Sailor::App::TraceViewportRay(
				request.viewport_id(),
				request.normalized_x(),
				request.normalized_y(),
				worldX,
				worldY,
				worldZ))
		{
			SetError(response, "Failed to trace the viewport ray.");
			return;
		}

		SetVector4Result(response, worldX, worldY, worldZ, 1.0f);
	}

	void DispatchInstantiatePrefabInstance(
		const sailor::editor::v1::InstantiatePrefabInstanceRequest& request,
		ProtocolResponse& response)
	{
		if (request.apply_world_position() &&
			(!request.has_world_position() ||
				!IsFiniteVector4(request.world_position())))
		{
			SetError(response, "The prefab world position is invalid.");
			return;
		}

		TInteropString instanceId;
		const Vector4& worldPosition = request.world_position();
		const bool bSucceeded = Sailor::App::InstantiateEditorPrefabInstance(
			request.file_id().c_str(),
			request.parent_instance_id().c_str(),
			request.apply_world_position(),
			worldPosition.x(),
			worldPosition.y(),
			worldPosition.z(),
			instanceId.GetOutput());
		SetInstanceIdResult(response, bSucceeded, instanceId.GetValue());
	}

	void DispatchGetViewportToolState(
		const sailor::editor::v1::ViewportIdRequest& request,
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
		result->set_operation(
			static_cast<ViewportTransformOperation>(operation));
		result->set_space(static_cast<ViewportTransformSpace>(space));
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

	void DispatchAnimatorParameter(
		const sailor::editor::v1::AnimatorParameterRequest& request,
		ProtocolResponse& response)
	{
		uint32_t valueKind = 0;
		float floatValue = 0.0f;
		int32_t intValue = 0;
		bool boolValue = false;
		switch (request.value_case())
		{
		case sailor::editor::v1::AnimatorParameterRequest::kFloatValue:
			valueKind = 1;
			floatValue = request.float_value();
			break;
		case sailor::editor::v1::AnimatorParameterRequest::kIntValue:
			valueKind = 2;
			intValue = request.int_value();
			break;
		case sailor::editor::v1::AnimatorParameterRequest::kBoolValue:
			valueKind = 3;
			boolValue = request.bool_value();
			break;
		case sailor::editor::v1::AnimatorParameterRequest::kTrigger:
			valueKind = 4;
			break;
		case sailor::editor::v1::AnimatorParameterRequest::kResetTrigger:
			valueKind = 5;
			break;
		case sailor::editor::v1::AnimatorParameterRequest::VALUE_NOT_SET:
			SetError(response, "Animator parameter value is not set.");
			return;
		}

		SetBoolResult(
			response,
			Sailor::App::SetEditorAnimatorParameter(
				request.instance_id().c_str(),
				request.name().c_str(),
				valueKind,
				floatValue,
				intValue,
				boolValue));
	}

	void DispatchAnimatorState(
		const sailor::editor::v1::InstanceIdRequest& request,
		ProtocolResponse& response)
	{
		bool bHasController = false;
		uint64_t controllerRevision = 0;
		uint64_t activeStateId = 0;
		float activeStateTime = 0.0f;
		bool bTransitioning = false;
		uint64_t destinationStateId = 0;
		float destinationStateTime = 0.0f;
		float transitionAlpha = 0.0f;
		TInteropString activeStateName;
		TInteropString destinationStateName;
		if (!Sailor::App::GetEditorAnimatorState(
				request.instance_id().c_str(),
				bHasController,
				controllerRevision,
				activeStateId,
				activeStateName.GetOutput(),
				activeStateTime,
				bTransitioning,
				destinationStateId,
				destinationStateName.GetOutput(),
				destinationStateTime,
				transitionAlpha))
		{
			SetError(response, "Animator component was not found.");
			return;
		}

		SetSuccess(response);
		auto* result = response.mutable_animator_state_result();
		result->set_has_controller(bHasController);
		result->set_controller_revision(controllerRevision);
		result->set_active_state_id(activeStateId);
		result->set_active_state_name(activeStateName.GetValue() ? activeStateName.GetValue() : "");
		result->set_active_state_time(activeStateTime);
		result->set_transitioning(bTransitioning);
		result->set_destination_state_id(destinationStateId);
		result->set_destination_state_name(
			destinationStateName.GetValue() ? destinationStateName.GetValue() : "");
		result->set_destination_state_time(destinationStateTime);
		result->set_transition_alpha(transitionAlpha);
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
			SetEmptyResult(response);
			break;

		case ProtocolRequest::kStop:
			StopEngine(dependencies);
			SetEmptyResult(response);
			break;

		case ProtocolRequest::kShutdown:
			ShutdownEngine(dependencies);
			SetEmptyResult(response);
			break;

		case ProtocolRequest::kRequestAssetReload:
			SetBoolResult(response, Sailor::App::RequestAssetReload());
			break;

		case ProtocolRequest::kUpdateAsset:
			SetBoolResult(
				response,
				Sailor::App::UpdateAsset(request.update_asset().file_id().c_str()));
			break;

		case ProtocolRequest::kCreateModelInstance:
			DispatchCreateModelInstance(
				request.create_model_instance(),
				response);
			break;

		case ProtocolRequest::kSetAnimatorParameter:
			DispatchAnimatorParameter(request.set_animator_parameter(), response);
			break;

		case ProtocolRequest::kGetAnimatorState:
			DispatchAnimatorState(request.get_animator_state(), response);
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
					instantiate.parent_instance_id().c_str(),
					instantiate.strict_instance_ids()));
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

		case ProtocolRequest::kIsEngineMainThreadReady:
			SetBoolResult(
				response,
				Sailor::App::IsEngineMainThreadReady());
			break;

		case ProtocolRequest::kTraceViewportRay:
			DispatchTraceViewportRay(
				request.trace_viewport_ray(),
				response);
			break;

		case ProtocolRequest::kInstantiatePrefabInstance:
			DispatchInstantiatePrefabInstance(
				request.instantiate_prefab_instance(),
				response);
			break;

		case ProtocolRequest::kFocusEditorCamera:
		{
			const auto& focus = request.focus_editor_camera();
			SetBoolResult(
				response,
				focus.viewport_id() != 0 &&
					Sailor::App::FocusEditorCamera(
						focus.instance_id().c_str()));
			break;
		}

		case ProtocolRequest::kSetPrefabLink:
		{
			const auto& link = request.set_prefab_link();
			SetBoolResult(
				response,
				Sailor::App::SetEditorPrefabLink(
					link.instance_id().c_str(),
					link.file_id().c_str()));
			break;
		}

		case ProtocolRequest::kBreakPrefabLink:
			SetBoolResult(
				response,
				Sailor::App::BreakEditorPrefabLink(
					request.break_prefab_link()
						.instance_id()
						.c_str()));
			break;

		case ProtocolRequest::kSetViewportToolState:
		{
			const auto& state = request.set_viewport_tool_state();
			SetBoolResult(
				response,
				state.viewport_id() != 0 &&
					Sailor::App::SetEditorViewportToolState(
						static_cast<uint32_t>(state.operation()),
						static_cast<uint32_t>(state.space())));
			break;
		}

		case ProtocolRequest::kGetViewportToolState:
			DispatchGetViewportToolState(
				request.get_viewport_tool_state(),
				response);
			break;

		case ProtocolRequest::COMMAND_NOT_SET:
		default:
			SetError(response, "Protocol command is not set or is unknown.");
			break;
		}
	}

	struct TEditorProtocolDispatchContext final
	{
		const ProtocolRequest* m_request = nullptr;
		ProtocolResponse* m_response = nullptr;
		const Sailor::Protocol::EditorEngineProtocolDependencies* m_dependencies = nullptr;
		std::exception_ptr m_exception{};
		bool m_bExecuted = false;
	};

	void ExecuteDispatchedEditorProtocolRequest(void* context) noexcept
	{
		auto& dispatchContext =
			*static_cast<TEditorProtocolDispatchContext*>(context);
		try
		{
			DispatchRequest(
				*dispatchContext.m_request,
				*dispatchContext.m_response,
				*dispatchContext.m_dependencies);
		}
		catch (...)
		{
			dispatchContext.m_exception = std::current_exception();
		}
		dispatchContext.m_bExecuted = true;
	}

	void DispatchRequestOnEditorThread(
		const ProtocolRequest& request,
		ProtocolResponse& response,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		if (!dependencies.m_dispatchEditorOperation)
		{
			DispatchRequest(request, response, dependencies);
			return;
		}

		TEditorProtocolDispatchContext context{
			&request,
			&response,
			&dependencies,
			{},
			false
		};
		const bool bDispatched = dependencies.m_dispatchEditorOperation(
			dependencies.m_editorDispatchContext,
			ExecuteDispatchedEditorProtocolRequest,
			&context);
		if (!bDispatched || !context.m_bExecuted)
		{
			throw std::runtime_error(
				"Failed to execute the Engine protocol operation on the Editor worker.");
		}
		if (context.m_exception)
		{
			std::rethrow_exception(context.m_exception);
		}
	}

	enum class EProtocolLifecycleCompletion : uint8_t
	{
		None,
		Initialization,
		Operation,
		Shutdown
	};

	class TProtocolLifecycleCompletion final
	{
	public:
		TProtocolLifecycleCompletion(
			Sailor::Protocol::TEditorEngineProtocolLifecycleGate& gate,
			const EProtocolLifecycleCompletion completion)
			: m_gate(gate)
			, m_completion(completion)
		{
		}

		TProtocolLifecycleCompletion(const TProtocolLifecycleCompletion&) = delete;
		TProtocolLifecycleCompletion& operator=(
			const TProtocolLifecycleCompletion&) = delete;

		~TProtocolLifecycleCompletion()
		{
			switch (m_completion)
			{
			case EProtocolLifecycleCompletion::Initialization:
				m_gate.CompleteInitialization(m_bSucceeded);
				break;

			case EProtocolLifecycleCompletion::Operation:
				m_gate.ReleaseOperation();
				break;

			case EProtocolLifecycleCompletion::Shutdown:
				m_gate.CompleteShutdown();
				break;

			case EProtocolLifecycleCompletion::None:
			default:
				break;
			}
		}

		void MarkSucceeded()
		{
			m_bSucceeded = true;
		}

	private:
		Sailor::Protocol::TEditorEngineProtocolLifecycleGate& m_gate;
		EProtocolLifecycleCompletion m_completion =
			EProtocolLifecycleCompletion::None;
		bool m_bSucceeded = false;
	};

	void DispatchRequestWithLifecycleAdmission(
		const ProtocolRequest& request,
		ProtocolResponse& response,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		auto& gate = dependencies.m_lifecycleGate
			? *dependencies.m_lifecycleGate
			: GetEditorEngineProtocolLifecycleGate();
		std::string admissionError;

		switch (request.command_case())
		{
		case ProtocolRequest::kInitialize:
		{
			if (!dependencies.m_bAllowInitialize)
			{
				SetError(
					response,
					"Engine initialization is available only during local host bootstrap.");
				return;
			}
			if (!gate.TryBeginInitialization(admissionError))
			{
				SetError(response, admissionError);
				return;
			}

			TProtocolLifecycleCompletion completion(
				gate,
				EProtocolLifecycleCompletion::Initialization);
			DispatchRequest(request, response, dependencies);
			completion.MarkSucceeded();
			return;
		}

		case ProtocolRequest::kStart:
		{
			const auto startRoutine = dependencies.m_start
				? dependencies.m_start
				: StartEngine;
			if (!gate.TryBeginStartAsync(
					dependencies.m_context,
					startRoutine,
					admissionError))
			{
				SetError(response, admissionError);
				return;
			}

			SetEmptyResult(response);
			return;
		}

		case ProtocolRequest::kStop:
			if (gate.NoteStopRequested())
			{
				DispatchRequest(request, response, dependencies);
				gate.WaitForStartDrainAndJoin();
			}
			else
			{
				SetEmptyResult(response);
			}
			return;

		case ProtocolRequest::kShutdown:
		{
			if (!gate.TryBeginShutdown(admissionError))
			{
				SetError(response, admissionError);
				return;
			}

			const TProtocolLifecycleCompletion completion(
				gate,
				EProtocolLifecycleCompletion::Shutdown);
			// Initialization owns partially built App state. Once it drains,
			// Stop can safely release a blocking Start before the remaining
			// regular operation leases are joined.
			gate.WaitForInitializationDrain();
			StopEngine(dependencies);
			gate.WaitForShutdownDrain();
			gate.WaitForStartDrainAndJoin();
			DispatchRequest(request, response, dependencies);
			return;
		}

		case ProtocolRequest::kIsEngineRunning:
			SetBoolResult(response, gate.IsStartActive());
			return;

		default:
		{
			const bool bAllowWhenIdle =
				request.command_case() == ProtocolRequest::kGetExitCode ||
				request.command_case() ==
					ProtocolRequest::kIsEngineMainThreadReady;
			if (!gate.TryAcquireOperation(
					admissionError,
					bAllowWhenIdle))
			{
				SetError(response, admissionError);
				return;
			}

			const TProtocolLifecycleCompletion completion(
				gate,
				EProtocolLifecycleCompletion::Operation);
			if (bAllowWhenIdle)
			{
				// These lifecycle probes are valid before App initialization
				// and after shutdown, when the Scheduler (and therefore the
				// dedicated Editor worker) does not exist.
				DispatchRequest(request, response, dependencies);
			}
			else
			{
				DispatchRequestOnEditorThread(
					request,
					response,
					dependencies);
			}
			return;
		}
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
	const bool bUsesStrictInstanceIdsVersion =
		request.protocol_version() ==
			EditorEngineProtocolStrictInstanceIdsVersion;
	response.set_protocol_version(
		bUsesStrictInstanceIdsVersion
			? EditorEngineProtocolStrictInstanceIdsVersion
			: request.protocol_version() == EditorEngineProtocolVersion
				? EditorEngineProtocolVersion
				: EditorEngineProtocolLatestVersion);
	response.set_request_id(request.request_id());
	response.set_supports_strict_instance_ids(true);

	const bool bRequestsStrictInstanceIds =
		request.command_case() ==
			ProtocolRequest::kInstantiatePrefabFromYaml &&
		request.instantiate_prefab_from_yaml().strict_instance_ids();
	if (request.protocol_version() != EditorEngineProtocolVersion &&
		!bUsesStrictInstanceIdsVersion)
	{
		SetError(
			response,
			"Unsupported protocol version " +
			std::to_string(request.protocol_version()) +
			"; expected " +
			std::to_string(EditorEngineProtocolVersion) +
			" or " +
			std::to_string(
				EditorEngineProtocolStrictInstanceIdsVersion) +
			".");
	}
	else if (bRequestsStrictInstanceIds &&
		!bUsesStrictInstanceIdsVersion)
	{
		SetError(
			response,
			"Strict instance-id restoration requires protocol version " +
			std::to_string(
				EditorEngineProtocolStrictInstanceIdsVersion) +
			".");
	}
	else if (bUsesStrictInstanceIdsVersion &&
		!bRequestsStrictInstanceIds)
	{
		SetError(
			response,
			"Protocol version " +
			std::to_string(
				EditorEngineProtocolStrictInstanceIdsVersion) +
			" is reserved for strict instance-id restoration.");
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
			DispatchRequestWithLifecycleAdmission(
				request,
				response,
				dependencies);
		}
	}

	return static_cast<int32_t>(
		SerializeResponse(response, responseData, responseSize));
}

void Sailor::Protocol::FreeEditorEngineProtocolBuffer(uint8_t* buffer) noexcept
{
	delete[] buffer;
}

void Sailor::Protocol::WaitForEditorEngineProtocolStartDrain()
{
	GetEditorEngineProtocolLifecycleGate().WaitForStartDrainAndJoin();
}

void Sailor::Protocol::ResetEditorEngineProtocolLifecycle()
{
	GetEditorEngineProtocolLifecycleGate().Reset();
}
