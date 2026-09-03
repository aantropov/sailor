#include "EditorEngineProtocolInternal.h"
#include "EditorEngineProtocolLifecycle.h"

#include "Memory/UniquePtr.hpp"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Sailor.h"
#include "Tasks/Scheduler.h"
#include "Tasks/Tasks.h"

#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

#if defined(GetMessage)
#undef GetMessage
#endif

bool Sailor::Protocol::DispatchEditorEngineProtocolOperationOnEditorThread(void*,
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
		[operation, operationContext]() { operation(operationContext); },
		Sailor::EThreadType::Editor);
	scheduler->Run(task);
	task->Wait();
	return task->IsFinished();
}

namespace
{
	using namespace Sailor::Protocol::EditorEngineProtocolCommands;
	using sailor::editor::v1::ProtocolRequest;
	using sailor::editor::v1::ProtocolResponse;
	using Sailor::Protocol::EditorEngineProtocolMaxPayloadSize;
	using Sailor::Protocol::EditorEngineProtocolVersion;
	using Sailor::Protocol::EEditorEngineTransportStatus;

	Sailor::Protocol::TEditorEngineProtocolLifecycleGate& GetEditorEngineProtocolLifecycleGate()
	{
		// Host-control exports may still be entered while the managed app is
		// coordinating process termination. Keep the synchronization object
		// alive until process reclamation; sessions are reset explicitly.
		static auto* const gate = new Sailor::Protocol::TEditorEngineProtocolLifecycleGate();
		return *gate;
	}

	void StartEngine(void*)
	{
		Sailor::App::Start();
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
		auto& dispatchContext = *static_cast<TEditorProtocolDispatchContext*>(context);
		try
		{
			Sailor::Protocol::DispatchEditorEngineProtocolRequest(
				*dispatchContext.m_request, *dispatchContext.m_response, *dispatchContext.m_dependencies);
		}
		catch (...)
		{
			dispatchContext.m_exception = std::current_exception();
		}
		dispatchContext.m_bExecuted = true;
	}

	void DispatchRequestOnEditorThread(const ProtocolRequest& request,
		ProtocolResponse& response,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		if (!dependencies.m_dispatchEditorOperation)
		{
			DispatchRequest(request, response, dependencies);
			return;
		}

		TEditorProtocolDispatchContext context{&request, &response, &dependencies, {}, false};
		const bool bDispatched = dependencies.m_dispatchEditorOperation(
			dependencies.m_editorDispatchContext, ExecuteDispatchedEditorProtocolRequest, &context);
		if (!bDispatched || !context.m_bExecuted)
		{
			throw std::runtime_error("Failed to execute the Engine protocol operation on the Editor worker.");
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
		TProtocolLifecycleCompletion(Sailor::Protocol::TEditorEngineProtocolLifecycleGate& gate,
			const EProtocolLifecycleCompletion completion)
			: m_gate(gate), m_completion(completion)
		{
		}

		TProtocolLifecycleCompletion(const TProtocolLifecycleCompletion&) = delete;
		TProtocolLifecycleCompletion& operator=(const TProtocolLifecycleCompletion&) = delete;

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
		EProtocolLifecycleCompletion m_completion = EProtocolLifecycleCompletion::None;
		bool m_bSucceeded = false;
	};

	void DispatchRequestWithLifecycleAdmission(const ProtocolRequest& request,
		ProtocolResponse& response,
		const Sailor::Protocol::EditorEngineProtocolDependencies& dependencies)
	{
		auto& gate =
			dependencies.m_lifecycleGate ? *dependencies.m_lifecycleGate : GetEditorEngineProtocolLifecycleGate();
		std::string admissionError;

		switch (request.command_case())
		{
		case ProtocolRequest::kInitialize:
		{
			if (!dependencies.m_bAllowInitialize)
			{
				SetError(response, "Engine initialization is available only during local host bootstrap.");
				return;
			}
			if (!gate.TryBeginInitialization(admissionError))
			{
				SetError(response, admissionError);
				return;
			}

			TProtocolLifecycleCompletion completion(gate, EProtocolLifecycleCompletion::Initialization);
			DispatchRequest(request, response, dependencies);
			completion.MarkSucceeded();
			return;
		}

		case ProtocolRequest::kStart:
		{
			const auto startRoutine = dependencies.m_start ? dependencies.m_start : StartEngine;
			if (!gate.TryBeginStartAsync(dependencies.m_context, startRoutine, admissionError))
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

			const TProtocolLifecycleCompletion completion(gate, EProtocolLifecycleCompletion::Shutdown);
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
			const bool bAllowWhenIdle = request.command_case() == ProtocolRequest::kGetExitCode ||
										request.command_case() == ProtocolRequest::kIsEngineMainThreadReady;
			if (!gate.TryAcquireOperation(admissionError, bAllowWhenIdle))
			{
				SetError(response, admissionError);
				return;
			}

			const TProtocolLifecycleCompletion completion(gate, EProtocolLifecycleCompletion::Operation);
			if (bAllowWhenIdle)
			{
				// These lifecycle probes are valid before App initialization
				// and after shutdown, when the Scheduler (and therefore the
				// dedicated Editor worker) does not exist.
				DispatchRequest(request, response, dependencies);
			}
			else
			{
				DispatchRequestOnEditorThread(request, response, dependencies);
			}
			return;
		}
		}
	}

	EEditorEngineTransportStatus SerializeResponse(const ProtocolResponse& response,
		uint8_t** responseData,
		uint32_t* responseSize)
	{
		const size_t serializedSize = response.ByteSizeLong();
		if (serializedSize > EditorEngineProtocolMaxPayloadSize)
		{
			return EEditorEngineTransportStatus::PayloadTooLarge;
		}
		if (serializedSize == 0 || serializedSize > static_cast<size_t>((std::numeric_limits<int>::max)()))
		{
			return EEditorEngineTransportStatus::SerializeFailed;
		}

		Sailor::TUniquePtr<uint8_t[]> serializedResponse(new (std::nothrow) uint8_t[serializedSize]);
		if (!serializedResponse)
		{
			return EEditorEngineTransportStatus::AllocationFailed;
		}

		if (!response.SerializeToArray(serializedResponse.GetRawPtr(), static_cast<int>(serializedSize)))
		{
			return EEditorEngineTransportStatus::SerializeFailed;
		}

		*responseSize = static_cast<uint32_t>(serializedSize);
		*responseData = serializedResponse.Release();
		return EEditorEngineTransportStatus::Ok;
	}
}

int32_t Sailor::Protocol::InvokeEditorEngineProtocol(const uint8_t* requestData,
	uint32_t requestSize,
	uint8_t** responseData,
	uint32_t* responseSize)
{
	return InvokeEditorEngineProtocol(
		requestData, requestSize, responseData, responseSize, EditorEngineProtocolDependencies{});
}

int32_t Sailor::Protocol::InvokeEditorEngineProtocol(const uint8_t* requestData,
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

	if (!requestData || requestSize == 0 || !responseData || !responseSize)
	{
		return static_cast<int32_t>(EEditorEngineTransportStatus::InvalidArguments);
	}

	if (requestSize > EditorEngineProtocolMaxPayloadSize)
	{
		return static_cast<int32_t>(EEditorEngineTransportStatus::PayloadTooLarge);
	}

	ProtocolRequest request;
	if (!request.ParseFromArray(requestData, static_cast<int>(requestSize)))
	{
		return static_cast<int32_t>(EEditorEngineTransportStatus::ParseFailed);
	}

	ProtocolResponse response;
	response.set_protocol_version(EditorEngineProtocolVersion);
	response.set_request_id(request.request_id());
	response.set_supports_strict_instance_ids(true);

	if (request.protocol_version() != EditorEngineProtocolVersion)
	{
		SetError(response,
			"Unsupported protocol version " + std::to_string(request.protocol_version()) + "; expected " +
				std::to_string(EditorEngineProtocolVersion) + ".");
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
			SetError(response, "Protocol string field '" + embeddedNullField + "' contains an embedded NUL byte.");
		}
		else
		{
			DispatchRequestWithLifecycleAdmission(request, response, dependencies);
		}
	}

	return static_cast<int32_t>(SerializeResponse(response, responseData, responseSize));
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
