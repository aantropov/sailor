#include "EditorEngineProtocolInternal.h"

#include "Memory/UniquePtr.hpp"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Sailor.h"

#include <string>
#include <vector>

namespace Sailor::Protocol::EditorEngineProtocolCommands
{
	using sailor::editor::v1::ProtocolRequest;
	using sailor::editor::v1::ProtocolResponse;

	constexpr uint32_t c_maxBatchItems = 65536u;

	void StopEngine(const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		if (dependencies.m_stop)
		{
			dependencies.m_stop(dependencies.m_context);
			return;
		}
		Sailor::App::Stop();
	}

	static void ShutdownEngine(const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		if (dependencies.m_shutdown)
		{
			dependencies.m_shutdown(dependencies.m_context);
			return;
		}
		Sailor::App::Shutdown();
	}

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

	void SetStringResult(ProtocolResponse& response, const char* value, uint32_t length)
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

	bool TryFindEmbeddedNull(const google::protobuf::Message& message, std::string& outFieldName)
	{
		const google::protobuf::Descriptor* descriptor = message.GetDescriptor();
		const google::protobuf::Reflection* reflection = message.GetReflection();
		if (!descriptor || !reflection)
		{
			return false;
		}

		for (int fieldIndex = 0; fieldIndex < descriptor->field_count(); ++fieldIndex)
		{
			const google::protobuf::FieldDescriptor* field = descriptor->field(fieldIndex);
			if (field->is_repeated())
			{
				const int fieldSize = reflection->FieldSize(message, field);
				for (int valueIndex = 0; valueIndex < fieldSize; ++valueIndex)
				{
					if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING)
					{
						std::string scratch;
						const std::string& value =
							reflection->GetRepeatedStringReference(message, field, valueIndex, &scratch);
						if (value.find('\0') != std::string::npos)
						{
							outFieldName = field->full_name();
							return true;
						}
					}
					else if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
							 TryFindEmbeddedNull(
								 reflection->GetRepeatedMessage(message, field, valueIndex), outFieldName))
					{
						return true;
					}
				}
				continue;
			}

			if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING)
			{
				std::string scratch;
				const std::string& value = reflection->GetStringReference(message, field, &scratch);
				if (value.find('\0') != std::string::npos)
				{
					outFieldName = field->full_name();
					return true;
				}
			}
			else if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
					 reflection->HasField(message, field) &&
					 TryFindEmbeddedNull(reflection->GetMessage(message, field), outFieldName))
			{
				return true;
			}
		}

		return false;
	}

	static void DispatchInitialize(const sailor::editor::v1::InitializeRequest& request, ProtocolResponse& response)
	{
		const int numArguments = request.arguments_size();
		Sailor::TUniquePtr<const char*[]> arguments{};
		if (numArguments > 0)
		{
			arguments = Sailor::TUniquePtr<const char*[]>::Make(static_cast<size_t>(numArguments));
			for (int i = 0; i < numArguments; ++i)
			{
				arguments[i] = request.arguments(i).c_str();
			}
		}

		Sailor::App::Initialize(arguments.GetRawPtr(), numArguments);
		SetEmptyResult(response);
	}

	static void DispatchMessages(const sailor::editor::v1::CountRequest& request, ProtocolResponse& response)
	{
		const uint32_t requestedCount = request.max_count();
		if (!ValidateBatchCount(requestedCount, response))
		{
			return;
		}

		auto messages =
			requestedCount > 0 ? Sailor::TUniquePtr<char*[]>::Make(requestedCount) : Sailor::TUniquePtr<char*[]>{};
		std::vector<Sailor::TUniquePtr<char[]>> ownedMessages;
		ownedMessages.reserve(requestedCount);
		const uint32_t numMessages = Sailor::App::PullEditorMessages(messages.GetRawPtr(), requestedCount);
		for (uint32_t i = 0; i < requestedCount; ++i)
		{
			ownedMessages.emplace_back(messages[i]);
		}
		if (numMessages > requestedCount)
		{
			SetError(response, "The native message source exceeded the requested batch capacity.");
			return;
		}

		SetSuccess(response);
		auto* result = response.mutable_string_list_result();
		for (uint32_t i = 0; i < numMessages; ++i)
		{
			result->add_values(messages[i] ? messages[i] : "");
		}
	}

	void DispatchRequest(const ProtocolRequest& request,
		ProtocolResponse& response,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		if (DispatchWorldCommand(request, response) || DispatchViewportCommand(request, response, dependencies) ||
			DispatchGICommand(request, response))
		{
			return;
		}

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
			SetBoolResult(response, Sailor::App::UpdateAsset(request.update_asset().file_id().c_str()));
			break;

		case ProtocolRequest::kGetAssetReloadState:
		{
			uint64_t requestGeneration = 0;
			uint64_t completedGeneration = 0;
			uint64_t successfulGeneration = 0;
			const bool bAvailable =
				Sailor::App::GetAssetReloadState(requestGeneration, completedGeneration, successfulGeneration);
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

		case ProtocolRequest::kPreviewAudioAsset:
			SetBoolResult(
				response, Sailor::App::PreviewEditorAudioAsset(request.preview_audio_asset().file_id().c_str()));
			break;

		case ProtocolRequest::kShowMainWindow:
			Sailor::App::ShowMainWindow(request.show_main_window().show());
			SetEmptyResult(response);
			break;

		case ProtocolRequest::kIsEngineMainThreadReady:
			SetBoolResult(response, Sailor::App::IsEngineMainThreadReady());
			break;

		case ProtocolRequest::COMMAND_NOT_SET:
		default:
			SetError(response, "Protocol command is not set or is unknown.");
			break;
		}
	}
}

void Sailor::Protocol::DispatchEditorEngineProtocolRequest(const sailor::editor::v1::ProtocolRequest& request,
	sailor::editor::v1::ProtocolResponse& response,
	const EditorEngineProtocolDependencies& dependencies)
{
	EditorEngineProtocolCommands::DispatchRequest(request, response, dependencies);
}
