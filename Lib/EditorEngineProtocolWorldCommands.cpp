#include "EditorEngineProtocolInternal.h"

#include "Memory/UniquePtr.hpp"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Sailor.h"

#include <yaml-cpp/yaml.h>

#include <cmath>

namespace Sailor::Protocol::EditorEngineProtocolCommands
{
	using sailor::editor::v1::ProtocolRequest;
	using sailor::editor::v1::ProtocolResponse;
	using sailor::editor::v1::Vector4;

	static void SetUInt64Result(ProtocolResponse& response, uint64_t value)
	{
		SetSuccess(response);
		response.mutable_uint64_result()->set_value(value);
	}

	static void SetInstanceIdResult(ProtocolResponse& response, bool bSucceeded, const char* instanceId)
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
		return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z()) &&
			   std::isfinite(value.w());
	}

	static void DispatchSerializeCurrentWorld(ProtocolResponse& response)
	{
		char* value = nullptr;
		const uint32_t length = Sailor::App::SerializeCurrentWorld(&value);
		Sailor::TUniquePtr<char[]> ownedValue(value);
		SetStringResult(response, value, length);
	}

	static void DispatchSerializeEngineTypes(ProtocolResponse& response)
	{
		char* value = nullptr;
		const uint32_t length = Sailor::App::SerializeEngineTypes(&value);
		Sailor::TUniquePtr<char[]> ownedValue(value);
		SetStringResult(response, value, length);
	}

	static void DispatchSerializeEditorTypes(ProtocolResponse& response)
	{
		char* value = nullptr;
		const uint32_t length = Sailor::App::SerializeEditorTypes(&value);
		Sailor::TUniquePtr<char[]> ownedValue(value);
		SetStringResult(response, value, length);
	}

	static void DispatchSerializeWorkspaceCacheIdentity(ProtocolResponse& response)
	{
		char* value = nullptr;
		const uint32_t length = Sailor::App::SerializeWorkspaceCacheIdentity(&value);
		Sailor::TUniquePtr<char[]> ownedValue(value);
		SetStringResult(response, value, length);
	}

	static void DispatchCreateGameObject(const sailor::editor::v1::CreateGameObjectRequest& request,
		ProtocolResponse& response)
	{
		char* instanceId = nullptr;
		const bool bSucceeded = Sailor::App::CreateEditorGameObject(
			request.parent_instance_id().c_str(), request.preferred_instance_id().c_str(), &instanceId);
		Sailor::TUniquePtr<char[]> ownedInstanceId(instanceId);

		SetSuccess(response);
		auto* result = response.mutable_instance_id_result();
		result->set_succeeded(bSucceeded);
		if (instanceId)
		{
			result->set_instance_id(instanceId);
		}
	}

	static void DispatchCreateModelInstance(const sailor::editor::v1::CreateModelInstanceRequest& request,
		ProtocolResponse& response)
	{
		if (request.model_file_id().empty() || request.name().empty() ||
			(request.apply_world_position() &&
				(!request.has_world_position() || !std::isfinite(request.world_position().x()) ||
					!std::isfinite(request.world_position().y()) || !std::isfinite(request.world_position().z()))))
		{
			SetError(response, "The model instance request is invalid.");
			return;
		}

		char* instanceId = nullptr;
		const auto& worldPosition = request.world_position();
		const bool bSucceeded = Sailor::App::CreateEditorModelInstance(request.model_file_id().c_str(),
			request.name().c_str(),
			request.parent_instance_id().c_str(),
			request.create_hierarchy(),
			request.apply_world_position(),
			worldPosition.x(),
			worldPosition.y(),
			worldPosition.z(),
			request.preferred_instance_id().c_str(),
			&instanceId);
		Sailor::TUniquePtr<char[]> ownedInstanceId(instanceId);
		SetInstanceIdResult(response, bSucceeded, instanceId);
	}

	static void DispatchAddComponent(const sailor::editor::v1::AddComponentRequest& request, ProtocolResponse& response)
	{
		char* instanceId = nullptr;
		const bool bSucceeded = Sailor::App::AddEditorComponent(request.instance_id().c_str(),
			request.component_type_name().c_str(),
			request.preferred_instance_id().c_str(),
			&instanceId);
		Sailor::TUniquePtr<char[]> ownedInstanceId(instanceId);

		SetSuccess(response);
		auto* result = response.mutable_instance_id_result();
		result->set_succeeded(bSucceeded);
		if (instanceId)
		{
			result->set_instance_id(instanceId);
		}
	}

	static void DispatchInstantiatePrefabInstance(const sailor::editor::v1::InstantiatePrefabInstanceRequest& request,
		ProtocolResponse& response)
	{
		if (request.apply_world_position() &&
			(!request.has_world_position() || !IsFiniteVector4(request.world_position())))
		{
			SetError(response, "The prefab world position is invalid.");
			return;
		}

		char* instanceId = nullptr;
		const Vector4& worldPosition = request.world_position();
		const bool bSucceeded = Sailor::App::InstantiateEditorPrefabInstance(request.file_id().c_str(),
			request.parent_instance_id().c_str(),
			request.apply_world_position(),
			worldPosition.x(),
			worldPosition.y(),
			worldPosition.z(),
			&instanceId);
		Sailor::TUniquePtr<char[]> ownedInstanceId(instanceId);
		SetInstanceIdResult(response, bSucceeded, instanceId);
	}

	static void DispatchSelection(const sailor::editor::v1::SelectionRequest& request, ProtocolResponse& response)
	{
		YAML::Node selection(YAML::NodeType::Sequence);
		for (const auto& instanceId : request.instance_ids())
		{
			selection.push_back(instanceId);
		}

		const std::string serializedSelection = YAML::Dump(selection);
		SetBoolResult(response, Sailor::App::SetEditorSelection(serializedSelection.c_str()));
	}

	static void DispatchAnimatorParameter(const sailor::editor::v1::AnimatorParameterRequest& request,
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

		SetBoolResult(response,
			Sailor::App::SetEditorAnimatorParameter(
				request.instance_id().c_str(), request.name().c_str(), valueKind, floatValue, intValue, boolValue));
	}

	static void DispatchAnimatorState(const sailor::editor::v1::InstanceIdRequest& request, ProtocolResponse& response)
	{
		bool bHasController = false;
		uint64_t controllerRevision = 0;
		uint64_t activeStateId = 0;
		float activeStateTime = 0.0f;
		bool bTransitioning = false;
		uint64_t destinationStateId = 0;
		float destinationStateTime = 0.0f;
		float transitionAlpha = 0.0f;
		char* activeStateName = nullptr;
		char* destinationStateName = nullptr;
		const bool bFound = Sailor::App::GetEditorAnimatorState(request.instance_id().c_str(),
			bHasController,
			controllerRevision,
			activeStateId,
			&activeStateName,
			activeStateTime,
			bTransitioning,
			destinationStateId,
			&destinationStateName,
			destinationStateTime,
			transitionAlpha);
		Sailor::TUniquePtr<char[]> ownedActiveStateName(activeStateName);
		Sailor::TUniquePtr<char[]> ownedDestinationStateName(destinationStateName);
		if (!bFound)
		{
			SetError(response, "Animator component was not found.");
			return;
		}

		SetSuccess(response);
		auto* result = response.mutable_animator_state_result();
		result->set_has_controller(bHasController);
		result->set_controller_revision(controllerRevision);
		result->set_active_state_id(activeStateId);
		result->set_active_state_name(activeStateName ? activeStateName : "");
		result->set_active_state_time(activeStateTime);
		result->set_transitioning(bTransitioning);
		result->set_destination_state_id(destinationStateId);
		result->set_destination_state_name(destinationStateName ? destinationStateName : "");
		result->set_destination_state_time(destinationStateTime);
		result->set_transition_alpha(transitionAlpha);
	}

	bool DispatchWorldCommand(const ProtocolRequest& request, ProtocolResponse& response)
	{
		switch (request.command_case())
		{
		case ProtocolRequest::kCreateModelInstance:
			DispatchCreateModelInstance(request.create_model_instance(), response);
			break;

		case ProtocolRequest::kSetAnimatorParameter:
			DispatchAnimatorParameter(request.set_animator_parameter(), response);
			break;

		case ProtocolRequest::kGetAnimatorState:
			DispatchAnimatorState(request.get_animator_state(), response);
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
			SetBoolResult(response, Sailor::App::LoadEditorWorld(request.load_editor_world().file_id().c_str()));
			break;

		case ProtocolRequest::kCreateEditorWorld:
			SetBoolResult(response, Sailor::App::CreateEditorWorld());
			break;

		case ProtocolRequest::kSetEditorSimulation:
			SetBoolResult(response, Sailor::App::SetEditorSimulationEnabled(request.set_editor_simulation().enabled()));
			break;

		case ProtocolRequest::kGetEditorSimulationState:
			SetBoolResult(response, Sailor::App::IsEditorSimulationEnabled());
			break;

		case ProtocolRequest::kGetEditorManagedMutationRevision:
		{
			const auto& mutation = request.get_editor_managed_mutation_revision();
			SetUInt64Result(response,
				Sailor::App::GetEditorManagedMutationRevision(mutation.kind(), mutation.instance_id().c_str()));
			break;
		}

		case ProtocolRequest::kUpdateObject:
		{
			const auto& update = request.update_object();
			SetBoolResult(
				response, Sailor::App::UpdateEditorObject(update.instance_id().c_str(), update.yaml_changes().c_str()));
			break;
		}

		case ProtocolRequest::kReparentObject:
		{
			const auto& reparent = request.reparent_object();
			SetBoolResult(response,
				Sailor::App::ReparentEditorObject(reparent.instance_id().c_str(),
					reparent.parent_instance_id().c_str(),
					reparent.keep_world_transform()));
			break;
		}

		case ProtocolRequest::kCreateGameObject:
			DispatchCreateGameObject(request.create_game_object(), response);
			break;

		case ProtocolRequest::kDestroyObject:
			SetBoolResult(response, Sailor::App::DestroyEditorObject(request.destroy_object().instance_id().c_str()));
			break;

		case ProtocolRequest::kResetComponentToDefaults:
			SetBoolResult(response,
				Sailor::App::ResetEditorComponentToDefaults(
					request.reset_component_to_defaults().instance_id().c_str()));
			break;

		case ProtocolRequest::kAddComponent:
			DispatchAddComponent(request.add_component(), response);
			break;

		case ProtocolRequest::kRemoveComponent:
			SetBoolResult(
				response, Sailor::App::RemoveEditorComponent(request.remove_component().instance_id().c_str()));
			break;

		case ProtocolRequest::kInstantiatePrefab:
		{
			const auto& instantiate = request.instantiate_prefab();
			SetBoolResult(response,
				Sailor::App::InstantiateEditorPrefab(
					instantiate.file_id().c_str(), instantiate.parent_instance_id().c_str()));
			break;
		}

		case ProtocolRequest::kInstantiatePrefabFromYaml:
		{
			const auto& instantiate = request.instantiate_prefab_from_yaml();
			char* instanceId = nullptr;
			const bool bSucceeded = Sailor::App::InstantiateEditorPrefabFromYaml(instantiate.prefab_yaml().c_str(),
				instantiate.parent_instance_id().c_str(),
				instantiate.strict_instance_ids(),
				&instanceId);
			Sailor::TUniquePtr<char[]> ownedInstanceId(instanceId);
			SetInstanceIdResult(response, bSucceeded, instanceId);
			break;
		}

		case ProtocolRequest::kSetEditorSelection:
			DispatchSelection(request.set_editor_selection(), response);
			break;

		case ProtocolRequest::kSerializeEngineTypes:
			DispatchSerializeEngineTypes(response);
			break;

		case ProtocolRequest::kInstantiatePrefabInstance:
			DispatchInstantiatePrefabInstance(request.instantiate_prefab_instance(), response);
			break;

		case ProtocolRequest::kSetPrefabLink:
		{
			const auto& link = request.set_prefab_link();
			SetBoolResult(
				response, Sailor::App::SetEditorPrefabLink(link.instance_id().c_str(), link.file_id().c_str()));
			break;
		}

		case ProtocolRequest::kBreakPrefabLink:
			SetBoolResult(
				response, Sailor::App::BreakEditorPrefabLink(request.break_prefab_link().instance_id().c_str()));
			break;

		default:
			return false;
		}

		return true;
	}
}
