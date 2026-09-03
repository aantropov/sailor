#include "EditorEngineProtocolInternal.h"
#include "EditorEngineProtocolLifecycle.h"

#include "Memory/UniquePtr.hpp"
#include "Editor/GlobalIlluminationBakeController.h"
#include "Editor/GlobalIlluminationEditorState.h"
#include "GlobalIllumination/GISettings.h"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Sailor.h"
#include "Settings/GraphicsSettings.h"
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
	using Sailor::Protocol::EditorEngineProtocolVersion;
	using sailor::editor::v1::EditorRenderMode;
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

	bool TryGetSceneViewRenderMode(
		EditorRenderMode protocolMode,
		Sailor::RHI::ESceneViewRenderMode& outMode)
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

	EditorRenderMode ToProtocolRenderMode(
		Sailor::RHI::ESceneViewRenderMode mode)
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

	bool TryParseFileId(
		const std::string& value,
		bool bRequired,
		Sailor::FileId& outFileId)
	{
		outFileId = {};
		if (value.empty())
		{
			return !bRequired;
		}
		outFileId = Sailor::FileId(value);
		return static_cast<bool>(outFileId);
	}

	sailor::editor::v1::GIProbesBakeState ToProtocolBakeState(
		Sailor::EEditorGIProbesBakeState state)
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

	bool TryGetGlobalIlluminationProbeMode(
		sailor::editor::v1::GlobalIlluminationProbeMode protocolMode,
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

	bool TryGetGlobalIlluminationMode(
		sailor::editor::v1::GlobalIlluminationMode protocolMode,
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

	sailor::editor::v1::GlobalIlluminationMode
		ToProtocolGlobalIlluminationMode(
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

	sailor::editor::v1::GlobalIlluminationProbeMode
		ToProtocolGlobalIlluminationProbeMode(
			Sailor::EGlobalIlluminationProbeMode mode)
	{
		return mode == Sailor::EGlobalIlluminationProbeMode::Additive
			? sailor::editor::v1::GLOBAL_ILLUMINATION_PROBE_MODE_ADDITIVE
			: sailor::editor::v1::GLOBAL_ILLUMINATION_PROBE_MODE_BLEND;
	}

	sailor::editor::v1::RuntimeGIProbesLifecycle
		ToProtocolRuntimeGIProbesLifecycle(
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

	sailor::editor::v1::RuntimeGIProbesPreviewBudget
		ToProtocolRuntimeGIProbesPreviewBudget(
			Sailor::Settings::ERuntimeGIProbesEditorBudget budget)
	{
		return budget ==
			Sailor::Settings::ERuntimeGIProbesEditorBudget::Balanced
			? sailor::editor::v1::RUNTIME_GI_PROBES_PREVIEW_BUDGET_BALANCED
			: sailor::editor::v1::RUNTIME_GI_PROBES_PREVIEW_BUDGET_ECO;
	}

	bool TryGetRuntimeGIProbesPreviewBudget(
		sailor::editor::v1::RuntimeGIProbesPreviewBudget protocolBudget,
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

	void SetProtocolRuntimeGIProbesSettings(
		sailor::editor::v1::RuntimeGIProbesSettings& destination,
		const Sailor::RuntimeGIProbesSettings& source)
	{
		destination.set_version(source.m_version);
		destination.set_include_sky(source.m_bIncludeSky);
		destination.set_include_emissive(source.m_bIncludeEmissive);
		destination.set_include_direct_lighting(
			source.m_bIncludeDirectLighting);
		destination.set_bounce_count(source.m_bounceCount);
		destination.set_min_probe_spacing(source.m_minProbeSpacing);
		destination.set_normal_bias(source.m_normalBias);
		destination.set_view_bias(source.m_viewBias);
		destination.set_max_ray_distance(source.m_maxRayDistance);
	}

	sailor::editor::v1::GlobalIlluminationProbeResidency
		ToProtocolGlobalIlluminationProbeResidency(
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

		case ProtocolRequest::kSetEditorSimulation:
			SetBoolResult(
				response,
				Sailor::App::SetEditorSimulationEnabled(
					request.set_editor_simulation().enabled()));
			break;

		case ProtocolRequest::kGetEditorSimulationState:
			SetBoolResult(
				response,
				Sailor::App::IsEditorSimulationEnabled());
			break;

		case ProtocolRequest::kPreviewAudioAsset:
			SetBoolResult(
				response,
				Sailor::App::PreviewEditorAudioAsset(
					request.preview_audio_asset().file_id().c_str()));
			break;

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
				break;
			}

			if (response.error().empty())
			{
				SetBoolResult(
					response,
					Sailor::App::SetRenderStatsMode(statsMode));
			}
			break;
		}

		case ProtocolRequest::kSetEditorRenderMode:
		{
			Sailor::RHI::ESceneViewRenderMode renderMode{};
			if (!TryGetSceneViewRenderMode(
					request.set_editor_render_mode().mode(),
					renderMode))
			{
				SetError(response, "The Editor render mode is invalid.");
				break;
			}

			SetBoolResult(
				response,
				Sailor::App::SetEditorRenderMode(renderMode));
			break;
		}

		case ProtocolRequest::kGetEditorRenderMode:
			SetSuccess(response);
			response.mutable_editor_render_mode_result()->set_mode(
				ToProtocolRenderMode(Sailor::App::GetEditorRenderMode()));
			break;

		case ProtocolRequest::kStartGiProbesBake:
		{
			const auto& bake = request.start_gi_probes_bake();
			Sailor::EditorGIProbesBakeRequest nativeRequest;
			if (!TryParseFileId(
					bake.world_file_id(),
					true,
					nativeRequest.m_worldAsset) ||
				!TryParseFileId(
					bake.layout_source_file_id(),
					false,
					nativeRequest.m_layoutSource) ||
				bake.output_virtual_path().empty() ||
				bake.state_name().empty() ||
				!bake.has_settings())
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
			nativeRequest.m_settings.m_maxSubdivisionLevel =
				settings.max_subdivision_level();
			nativeRequest.m_settings.m_minProbeSpacing =
				settings.min_probe_spacing();
			nativeRequest.m_settings.m_normalBias = settings.normal_bias();
			nativeRequest.m_settings.m_viewBias = settings.view_bias();
			nativeRequest.m_settings.m_maxRayDistance =
				settings.max_ray_distance();
			nativeRequest.m_settings.m_bIncludeSky = settings.include_sky();
			nativeRequest.m_settings.m_bIncludeEmissive =
				settings.include_emissive();
			nativeRequest.m_settings.m_bIncludeDirectLighting =
				settings.include_direct_lighting();
			nativeRequest.m_bOverwrite = bake.overwrite();
			nativeRequest.m_threadCount = bake.has_thread_count() ?
				bake.thread_count() : 1u;

			if (bake.has_fallback_environment() &&
				!IsFiniteVector4(bake.fallback_environment()))
			{
				SetError(response, "The GI probe bake vectors must be finite.");
				break;
			}
			if (bake.has_fallback_environment())
			{
				nativeRequest.m_fallbackEnvironment = {
					bake.fallback_environment().x(),
					bake.fallback_environment().y(),
					bake.fallback_environment().z() };
			}

			std::string diagnostic;
			if (!Sailor::App::StartEditorGIProbesBake(
					nativeRequest,
					diagnostic))
			{
				SetError(
					response,
					diagnostic.empty()
						? "Failed to start the GI probe bake."
						: diagnostic);
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
				SetError(
					response,
					diagnostic.empty()
						? "Failed to cancel the GI probe bake."
						: diagnostic);
				break;
			}
			SetBoolResult(response, true);
			break;
		}

		case ProtocolRequest::kSetGiSettings:
		{
			Sailor::GISettings nativeSettings;
			const auto& protocolSettings =
				request.set_gi_settings();
			bool bValid = true;
			std::string diagnostic;
			if (protocolSettings.has_mode() &&
				!TryGetGlobalIlluminationMode(
					protocolSettings.mode(),
					nativeSettings.m_mode))
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
				nativeSettings.m_runtimeProbes.m_bIncludeSky =
					runtime.include_sky();
				nativeSettings.m_runtimeProbes.m_bIncludeEmissive =
					runtime.include_emissive();
				nativeSettings.m_runtimeProbes.m_bIncludeDirectLighting =
					runtime.include_direct_lighting();
				nativeSettings.m_runtimeProbes.m_bounceCount =
					runtime.bounce_count();
				nativeSettings.m_runtimeProbes.m_minProbeSpacing =
					runtime.min_probe_spacing();
				nativeSettings.m_runtimeProbes.m_normalBias =
					runtime.normal_bias();
				nativeSettings.m_runtimeProbes.m_viewBias =
					runtime.view_bias();
				nativeSettings.m_runtimeProbes.m_maxRayDistance =
					runtime.max_ray_distance();
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
				if (probe.name().empty() ||
					!TryParseFileId(
						probe.asset_file_id(),
						true,
						binding.m_asset) ||
					!TryGetGlobalIlluminationProbeMode(
						probe.mode(),
						binding.m_mode) ||
					!std::isfinite(probe.initial_weight()) ||
					probe.initial_weight() < 0.0f)
				{
					bValid = false;
					diagnostic =
						"A Global Illumination ECS probe binding is invalid.";
					break;
				}
				binding.m_initialWeight = probe.initial_weight();
				binding.m_bPreload = probe.preload();
				if (!nativeSettings.m_probes.Insert(
						probe.name(),
						std::move(binding)))
				{
					bValid = false;
					diagnostic =
						"Global Illumination ECS probe binding names must be unique.";
					break;
				}
			}
			if (!bValid ||
				!Sailor::App::SetEditorGISettings(
					std::move(nativeSettings),
					diagnostic))
			{
				SetError(
					response,
					diagnostic.empty()
						? "Failed to update Global Illumination ECS settings."
						: diagnostic);
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
			result->set_max_probe_states_per_snapshot(
				state.m_maxProbeStatesPerSnapshot);
			result->set_diagnostic(state.m_diagnostic);
			result->set_composition_count(state.m_compositionCount);
			result->set_rejected_composition_count(
				state.m_rejectedCompositionCount);
			result->set_mode(
				ToProtocolGlobalIlluminationMode(state.m_mode));
			result->set_enabled(state.m_bEnabled);
			SetProtocolRuntimeGIProbesSettings(
				*result->mutable_runtime_probes(),
				state.m_runtimeSettings);
			auto* runtime = result->mutable_runtime_state();
			runtime->set_lifecycle(ToProtocolRuntimeGIProbesLifecycle(
				state.m_runtimeStatus.m_lifecycle));
			runtime->set_enabled(state.m_runtimeStatus.m_bEnabled);
			runtime->set_paused(state.m_runtimeStatus.m_bPaused);
			runtime->set_preview_enabled(state.m_bRuntimePreviewEnabled);
			runtime->set_preview_budget(
				ToProtocolRuntimeGIProbesPreviewBudget(
					state.m_runtimeEditorBudget));
			runtime->set_scene_generation(
				state.m_runtimeStatus.m_sceneGeneration);
			runtime->set_lighting_generation(
				state.m_runtimeStatus.m_lightingGeneration);
			runtime->set_published_revision(
				state.m_runtimeStatus.m_publishedRevision);
			runtime->set_capacity(state.m_runtimeStatus.m_capacity);
			runtime->set_active_probe_count(
				state.m_runtimeStatus.m_activeProbeCount);
			runtime->set_ready_probe_count(
				state.m_runtimeStatus.m_readyProbeCount);
			runtime->set_worker_count(state.m_runtimeStatus.m_workerCount);
			runtime->set_published_bytes(
				state.m_runtimeStatus.m_publishedBytes);
			runtime->set_coverage(state.m_runtimeStatus.m_coverage);
			runtime->set_refinement(state.m_runtimeStatus.m_refinement);
			runtime->set_diagnostic(state.m_runtimeStatus.m_diagnostic);
			for (const Sailor::GlobalIlluminationProbeState& probe :
				state.m_probes)
			{
				auto* protocolProbe = result->add_probes();
				protocolProbe->set_name(probe.m_name);
				protocolProbe->set_asset_file_id(probe.m_asset.ToString());
				protocolProbe->set_mode(
					ToProtocolGlobalIlluminationProbeMode(probe.m_mode));
				protocolProbe->set_weight(probe.m_weight);
				protocolProbe->set_residency(
					ToProtocolGlobalIlluminationProbeResidency(
						probe.m_residency));
				protocolProbe->set_asset_revision(probe.m_assetRevision);
				protocolProbe->set_diagnostic(probe.m_diagnostic);
			}
			break;
		}

		case ProtocolRequest::kSetRuntimeGiProbesPreview:
		{
			std::string diagnostic;
			if (!Sailor::App::SetEditorRuntimeGIProbesPreviewEnabled(
					request.set_runtime_gi_probes_preview().enabled(),
					diagnostic))
			{
				SetError(response, diagnostic.empty()
					? "Failed to update Runtime GI probe preview."
					: diagnostic);
				break;
			}
			SetBoolResult(response, true);
			break;
		}

		case ProtocolRequest::kSetRuntimeGiProbesPaused:
		{
			std::string diagnostic;
			if (!Sailor::App::SetEditorRuntimeGIProbesPaused(
					request.set_runtime_gi_probes_paused().paused(),
					diagnostic))
			{
				SetError(response, diagnostic.empty()
					? "Failed to update Runtime GI probe pause state."
					: diagnostic);
				break;
			}
			SetBoolResult(response, true);
			break;
		}

		case ProtocolRequest::kSetRuntimeGiProbesPreviewBudget:
		{
			Sailor::Settings::ERuntimeGIProbesEditorBudget budget{};
			if (!TryGetRuntimeGIProbesPreviewBudget(
					request.set_runtime_gi_probes_preview_budget().budget(),
					budget))
			{
				SetError(response, "Runtime GI preview budget is unsupported.");
				break;
			}
			std::string diagnostic;
			if (!Sailor::App::SetEditorRuntimeGIProbesBudget(
					budget,
					diagnostic))
			{
				SetError(response, diagnostic.empty()
					? "Failed to update the Runtime GI preview budget."
					: diagnostic);
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
				SetError(response, diagnostic.empty()
					? "Failed to restart Runtime GI probes."
					: diagnostic);
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
				SetError(response, diagnostic.empty()
					? "Failed to rebuild the Runtime GI probe scene."
					: diagnostic);
				break;
			}
			SetBoolResult(response, true);
			break;
		}

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
			TInteropString instanceId;
			const bool bSucceeded =
				Sailor::App::InstantiateEditorPrefabFromYaml(
					instantiate.prefab_yaml().c_str(),
					instantiate.parent_instance_id().c_str(),
					instantiate.strict_instance_ids(),
					instanceId.GetOutput());
			SetInstanceIdResult(
				response,
				bSucceeded,
				instanceId.GetValue());
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
	response.set_protocol_version(EditorEngineProtocolVersion);
	response.set_request_id(request.request_id());
	response.set_supports_strict_instance_ids(true);

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
