#include "AnimationController.h"
#include "Math/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Sailor;

namespace
{
	void AddError(TVector<std::string>* errors, const std::string& error)
	{
		if (errors)
		{
			errors->Add(error);
		}
	}

	bool IsOperationValid(
		EAnimationParameterType type,
		EAnimationConditionOperation operation)
	{
		if (type == EAnimationParameterType::Invalid ||
			operation == EAnimationConditionOperation::Invalid)
		{
			return false;
		}
		if (type == EAnimationParameterType::Trigger)
		{
			return operation == EAnimationConditionOperation::IsSet;
		}
		if (type == EAnimationParameterType::Bool)
		{
			return operation == EAnimationConditionOperation::Equal ||
				operation == EAnimationConditionOperation::NotEqual;
		}
		return operation != EAnimationConditionOperation::IsSet;
	}
}

YAML::Node AnimationControllerAsset::Serialize() const
{
	YAML::Node result;
	result["version"] = m_version;
	result["defaultState"] = m_defaultStateId;

	for (const auto& parameter : m_parameters)
	{
		YAML::Node serialized;
		serialized["id"] = parameter.m_id;
		serialized["name"] = parameter.m_name;
		serialized["type"] = std::string(magic_enum::enum_name(parameter.m_type));
		switch (parameter.m_type)
		{
		case EAnimationParameterType::Float:
			serialized["default"] = parameter.m_defaultFloat;
			break;
		case EAnimationParameterType::Int:
			serialized["default"] = parameter.m_defaultInt;
			break;
		case EAnimationParameterType::Bool:
			serialized["default"] = parameter.m_defaultBool;
			break;
		case EAnimationParameterType::Trigger:
		case EAnimationParameterType::Invalid:
			break;
		}
		result["parameters"].push_back(serialized);
	}

	for (const auto& state : m_states)
	{
		YAML::Node serialized;
		serialized["id"] = state.m_id;
		serialized["name"] = state.m_name;
		serialized["clip"] = state.m_clipSlot;
		serialized["speed"] = state.m_speed;
		serialized["loop"] = state.m_bLoop;
		serialized["editor"]["x"] = state.m_editorX;
		serialized["editor"]["y"] = state.m_editorY;
		result["states"].push_back(serialized);
	}

	for (const auto& transition : m_transitions)
	{
		YAML::Node serialized;
		serialized["id"] = transition.m_id;
		serialized["from"] = transition.m_fromStateId;
		serialized["to"] = transition.m_toStateId;
		serialized["priority"] = transition.m_priority;
		serialized["duration"] = transition.m_duration;
		serialized["hasExitTime"] = transition.m_bHasExitTime;
		serialized["exitTime"] = transition.m_exitTime;
		serialized["conditions"] = YAML::Node(YAML::NodeType::Sequence);
		for (const auto& condition : transition.m_conditions)
		{
			YAML::Node serializedCondition;
			serializedCondition["parameter"] = condition.m_parameterId;
			serializedCondition["operation"] =
				std::string(magic_enum::enum_name(condition.m_operation));
			serializedCondition["floatValue"] = condition.m_floatValue;
			serializedCondition["intValue"] = condition.m_intValue;
			serializedCondition["boolValue"] = condition.m_boolValue;
			serialized["conditions"].push_back(serializedCondition);
		}
		result["transitions"].push_back(serialized);
	}

	return result;
}

void AnimationControllerAsset::Deserialize(const YAML::Node& inData)
{
	m_version = inData["version"].as<uint32_t>(1);
	m_defaultStateId = inData["defaultState"].as<AnimationControllerNodeId>(
		InvalidAnimationControllerNodeId);
	m_parameters.Clear();
	m_states.Clear();
	m_transitions.Clear();

	const YAML::Node parameters = inData["parameters"];
	if (parameters.IsSequence())
	{
		m_parameters.Reserve(parameters.size());
		for (const auto& serialized : parameters)
		{
			AnimationParameterDefinition parameter;
			parameter.m_id = serialized["id"].as<AnimationControllerNodeId>(
				InvalidAnimationControllerNodeId);
			parameter.m_name = serialized["name"].as<std::string>("");
			const YAML::Node parameterType = serialized["type"];
			parameter.m_type = parameterType && !parameterType.IsNull()
				? magic_enum::enum_cast<EAnimationParameterType>(
					parameterType.as<std::string>()).value_or(
						EAnimationParameterType::Invalid)
				: EAnimationParameterType::Float;
			switch (parameter.m_type)
			{
			case EAnimationParameterType::Float:
				parameter.m_defaultFloat = serialized["default"].as<float>(0.0f);
				break;
			case EAnimationParameterType::Int:
				parameter.m_defaultInt = serialized["default"].as<int32_t>(0);
				break;
			case EAnimationParameterType::Bool:
				parameter.m_defaultBool = serialized["default"].as<bool>(false);
				break;
			case EAnimationParameterType::Trigger:
			case EAnimationParameterType::Invalid:
				break;
			}
			m_parameters.Add(std::move(parameter));
		}
	}

	const YAML::Node states = inData["states"];
	if (states.IsSequence())
	{
		m_states.Reserve(states.size());
		for (const auto& serialized : states)
		{
			AnimationStateDefinition state;
			state.m_id = serialized["id"].as<AnimationControllerNodeId>(
				InvalidAnimationControllerNodeId);
			state.m_name = serialized["name"].as<std::string>("");
			state.m_clipSlot = serialized["clip"].as<std::string>("");
			state.m_speed = serialized["speed"].as<float>(1.0f);
			state.m_bLoop = serialized["loop"].as<bool>(true);
			const YAML::Node editor = serialized["editor"];
			state.m_editorX = editor["x"].as<float>(0.0f);
			state.m_editorY = editor["y"].as<float>(0.0f);
			m_states.Add(std::move(state));
		}
	}

	const YAML::Node transitions = inData["transitions"];
	if (transitions.IsSequence())
	{
		m_transitions.Reserve(transitions.size());
		for (const auto& serialized : transitions)
		{
			AnimationTransitionDefinition transition;
			transition.m_id = serialized["id"].as<AnimationControllerNodeId>(
				InvalidAnimationControllerNodeId);
			transition.m_fromStateId = serialized["from"].as<AnimationControllerNodeId>(
				InvalidAnimationControllerNodeId);
			transition.m_toStateId = serialized["to"].as<AnimationControllerNodeId>(
				InvalidAnimationControllerNodeId);
			transition.m_priority = serialized["priority"].as<int32_t>(0);
			transition.m_duration = serialized["duration"].as<float>(0.2f);
			transition.m_bHasExitTime = serialized["hasExitTime"].as<bool>(false);
			transition.m_exitTime = serialized["exitTime"].as<float>(0.0f);

			YAML::Node conditions;
			for (const auto& field : serialized)
			{
				if (field.first.as<std::string>("") == "conditions")
				{
					conditions = field.second;
					break;
				}
			}
			if (conditions.IsSequence())
			{
				transition.m_conditions.Reserve(conditions.size());
				for (const auto& serializedCondition : conditions)
				{
					AnimationTransitionCondition condition;
					condition.m_parameterId = serializedCondition["parameter"].as<AnimationControllerNodeId>(
						InvalidAnimationControllerNodeId);
					const YAML::Node conditionOperation =
						serializedCondition["operation"];
					condition.m_operation = conditionOperation &&
						!conditionOperation.IsNull()
						? magic_enum::enum_cast<EAnimationConditionOperation>(
							conditionOperation.as<std::string>()).value_or(
								EAnimationConditionOperation::Invalid)
						: EAnimationConditionOperation::Equal;
					condition.m_floatValue = serializedCondition["floatValue"].as<float>(0.0f);
					condition.m_intValue = serializedCondition["intValue"].as<int32_t>(0);
					condition.m_boolValue = serializedCondition["boolValue"].as<bool>(false);
					transition.m_conditions.Add(std::move(condition));
				}
			}
			m_transitions.Add(std::move(transition));
		}
	}
}

YAML::Node AnimationSetAsset::Serialize() const
{
	YAML::Node result;
	result["version"] = m_version;
	for (const auto& entry : m_entries)
	{
		YAML::Node serialized;
		serialized["slot"] = entry.m_slot;
		serialized["animation"] = entry.m_animation.Serialize();
		result["clips"].push_back(serialized);
	}
	return result;
}

void AnimationSetAsset::Deserialize(const YAML::Node& inData)
{
	m_version = inData["version"].as<uint32_t>(1);
	m_entries.Clear();
	const YAML::Node entries = inData["clips"];
	if (!entries.IsSequence())
	{
		return;
	}

	m_entries.Reserve(entries.size());
	for (const auto& serialized : entries)
	{
		AnimationSetEntry entry;
		entry.m_slot = serialized["slot"].as<std::string>("");
		if (serialized["animation"])
		{
			entry.m_animation = serialized["animation"].as<FileId>();
		}
		m_entries.Add(std::move(entry));
	}
}

bool AnimationController::Initialize(
	const AnimationControllerAsset& asset,
	TVector<std::string>* outErrors)
{
	TVector<AnimationParameterDefinition> previousParameters = m_parameters;
	TVector<AnimationStateDefinition> previousStates = m_states;
	TVector<AnimationTransitionDefinition> previousTransitions = m_transitions;
	const uint32_t previousDefaultStateIndex = m_defaultStateIndex;

	m_parameters = asset.GetParameters();
	m_states = asset.GetStates();
	m_transitions = asset.GetTransitions();
	m_defaultStateIndex = (std::numeric_limits<uint32_t>::max)();

	const int32_t defaultStateIndex = FindStateIndex(asset.GetDefaultStateId());
	if (defaultStateIndex >= 0)
	{
		m_defaultStateIndex = static_cast<uint32_t>(defaultStateIndex);
	}

	if (!Validate(outErrors))
	{
		m_parameters = std::move(previousParameters);
		m_states = std::move(previousStates);
		m_transitions = std::move(previousTransitions);
		m_defaultStateIndex = previousDefaultStateIndex;
		return false;
	}

	for (auto& transition : m_transitions)
	{
		transition.m_fromStateIndex = static_cast<uint32_t>(FindStateIndex(transition.m_fromStateId));
		transition.m_toStateIndex = static_cast<uint32_t>(FindStateIndex(transition.m_toStateId));
		for (auto& condition : transition.m_conditions)
		{
			condition.m_parameterIndex = static_cast<uint32_t>(FindParameterIndex(condition.m_parameterId));
		}
	}
	++m_revision;

	return true;
}

bool AnimationController::Validate(TVector<std::string>* outErrors) const
{
	if (outErrors)
	{
		outErrors->Clear();
	}
	bool bValid = true;

	if (m_states.IsEmpty())
	{
		AddError(outErrors, "The controller must contain at least one state.");
		bValid = false;
	}
	if (m_defaultStateIndex >= m_states.Num())
	{
		AddError(outErrors, "The controller default state does not exist.");
		bValid = false;
	}

	for (size_t i = 0; i < m_parameters.Num(); ++i)
	{
		const auto& parameter = m_parameters[i];
		if (parameter.m_id == InvalidAnimationControllerNodeId || parameter.m_name.empty())
		{
			AddError(outErrors, "Animation parameters require a stable id and name.");
			bValid = false;
		}
		if (parameter.m_type == EAnimationParameterType::Invalid)
		{
			AddError(outErrors, "Animation parameter type is unknown.");
			bValid = false;
		}
		if (parameter.m_type == EAnimationParameterType::Float &&
			!std::isfinite(parameter.m_defaultFloat))
		{
			AddError(outErrors, "Float parameter defaults must be finite.");
			bValid = false;
		}
		for (size_t j = 0; j < i; ++j)
		{
			if (m_parameters[j].m_id == parameter.m_id ||
				m_parameters[j].m_name == parameter.m_name)
			{
				AddError(outErrors, "Animation parameter ids and names must be unique.");
				bValid = false;
				break;
			}
		}
	}

	for (size_t i = 0; i < m_states.Num(); ++i)
	{
		const auto& state = m_states[i];
		if (state.m_id == InvalidAnimationControllerNodeId ||
			state.m_name.empty() || state.m_clipSlot.empty())
		{
			AddError(outErrors, "Animation states require a stable id, name, and clip slot.");
			bValid = false;
		}
		if (!std::isfinite(state.m_speed) || state.m_speed <= 0.0f ||
			!std::isfinite(state.m_editorX) || !std::isfinite(state.m_editorY))
		{
			AddError(outErrors, "Animation state speed and editor position must be finite, with speed greater than zero.");
			bValid = false;
		}
		for (size_t j = 0; j < i; ++j)
		{
			if (m_states[j].m_id == state.m_id || m_states[j].m_name == state.m_name)
			{
				AddError(outErrors, "Animation state ids and names must be unique.");
				bValid = false;
				break;
			}
		}
	}

	for (size_t i = 0; i < m_transitions.Num(); ++i)
	{
		const auto& transition = m_transitions[i];
		if (transition.m_id == InvalidAnimationControllerNodeId ||
			FindStateIndex(transition.m_fromStateId) < 0 ||
			FindStateIndex(transition.m_toStateId) < 0)
		{
			AddError(outErrors, "Animation transitions require a stable id and valid source and destination states.");
			bValid = false;
		}
		if (!std::isfinite(transition.m_duration) || transition.m_duration < 0.0f ||
			!std::isfinite(transition.m_exitTime) || transition.m_exitTime < 0.0f ||
			transition.m_exitTime > 1.0f)
		{
			AddError(outErrors, "Animation transition duration and normalized exit time are invalid.");
			bValid = false;
		}
		for (size_t j = 0; j < i; ++j)
		{
			if (m_transitions[j].m_id == transition.m_id)
			{
				AddError(outErrors, "Animation transition ids must be unique.");
				bValid = false;
				break;
			}
		}

		for (const auto& condition : transition.m_conditions)
		{
			const int32_t parameterIndex = FindParameterIndex(condition.m_parameterId);
			if (parameterIndex < 0)
			{
				AddError(outErrors, "Animation transition conditions must reference an existing parameter.");
				bValid = false;
				continue;
			}

			const auto& parameter = m_parameters[static_cast<size_t>(parameterIndex)];
			if (!IsOperationValid(parameter.m_type, condition.m_operation) ||
				(parameter.m_type == EAnimationParameterType::Float &&
				 !std::isfinite(condition.m_floatValue)))
			{
				AddError(outErrors, "Animation transition condition operation does not match its parameter type.");
				bValid = false;
			}
		}
	}

	return bValid;
}

int32_t AnimationController::FindStateIndex(AnimationControllerNodeId stateId) const
{
	for (size_t i = 0; i < m_states.Num(); ++i)
	{
		if (m_states[i].m_id == stateId)
		{
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

int32_t AnimationController::FindParameterIndex(AnimationControllerNodeId parameterId) const
{
	for (size_t i = 0; i < m_parameters.Num(); ++i)
	{
		if (m_parameters[i].m_id == parameterId)
		{
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

int32_t AnimationController::FindParameterIndex(const std::string& name) const
{
	for (size_t i = 0; i < m_parameters.Num(); ++i)
	{
		if (m_parameters[i].m_name == name)
		{
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

bool AnimationSet::Initialize(
	const AnimationSetAsset& asset,
	TVector<std::string>* outErrors)
{
	if (outErrors)
	{
		outErrors->Clear();
	}
	TVector<AnimationSetEntry> entries = asset.GetEntries();
	bool bValid = true;
	for (size_t i = 0; i < entries.Num(); ++i)
	{
		if (entries[i].m_slot.empty() || !entries[i].m_animation)
		{
			AddError(outErrors, "Animation set entries require a slot name and animation FileId.");
			bValid = false;
		}
		for (size_t j = 0; j < i; ++j)
		{
			if (entries[j].m_slot == entries[i].m_slot)
			{
				AddError(outErrors, "Animation set slot names must be unique.");
				bValid = false;
				break;
			}
		}
	}
	if (bValid)
	{
		m_entries = std::move(entries);
		++m_revision;
	}
	return bValid;
}

const FileId* AnimationSet::FindAnimation(const std::string& slot) const
{
	for (const auto& entry : m_entries)
	{
		if (entry.m_slot == slot)
		{
			return &entry.m_animation;
		}
	}
	return nullptr;
}

bool AnimationControllerInstance::SetController(const AnimationControllerPtr& controller)
{
	m_controller = controller;
	m_parameterValues.Clear();
	m_parameterIds.Clear();
	m_parameterTypes.Clear();
	m_activeStateIndex = InvalidIndex;
	m_destinationStateIndex = InvalidIndex;
	m_activeStateTime = 0.0f;
	m_destinationStateTime = 0.0f;
	m_transitionElapsed = 0.0f;
	m_transitionDuration = 0.0f;
	m_activeStateId = InvalidAnimationControllerNodeId;
	m_destinationStateId = InvalidAnimationControllerNodeId;
	m_controllerRevision = m_controller ? m_controller->GetRevision() : 0;

	if (!m_controller || m_controller->GetStates().IsEmpty())
	{
		return false;
	}

	m_parameterValues.Resize(m_controller->GetParameters().Num());
	m_parameterIds.Resize(m_controller->GetParameters().Num());
	m_parameterTypes.Resize(m_controller->GetParameters().Num());
	for (size_t i = 0; i < m_controller->GetParameters().Num(); ++i)
	{
		const auto& definition = m_controller->GetParameters()[i];
		auto& value = m_parameterValues[i];
		m_parameterIds[i] = definition.m_id;
		m_parameterTypes[i] = definition.m_type;
		value.m_floatValue = definition.m_defaultFloat;
		value.m_intValue = definition.m_defaultInt;
		value.m_boolValue = definition.m_type == EAnimationParameterType::Trigger ?
			false : definition.m_defaultBool;
	}
	m_activeStateIndex = m_controller->GetDefaultStateIndex();
	if (m_activeStateIndex < m_controller->GetStates().Num())
	{
		m_activeStateId = m_controller->GetStates()[m_activeStateIndex].m_id;
	}
	return m_activeStateIndex < m_controller->GetStates().Num();
}

void AnimationControllerInstance::Reset()
{
	SetController(m_controller);
}

void AnimationControllerInstance::Tick(float deltaTime, float activeClipDuration)
{
	if (!SynchronizeController() || !IsValid() || !std::isfinite(deltaTime) || deltaTime < 0.0f)
	{
		return;
	}

	const auto& states = m_controller->GetStates();
	if (IsTransitioning())
	{
		m_activeStateTime += deltaTime * states[m_activeStateIndex].m_speed;
		m_destinationStateTime += deltaTime * states[m_destinationStateIndex].m_speed;
		m_transitionElapsed += deltaTime;
		if (m_transitionDuration <= 0.0f || m_transitionElapsed >= m_transitionDuration)
		{
			CompleteTransition();
		}
		return;
	}

	m_activeStateTime += deltaTime * states[m_activeStateIndex].m_speed;
	TryBeginTransition(activeClipDuration);
}

bool AnimationControllerInstance::SetFloat(const std::string& name, float value)
{
	if (!std::isfinite(value))
	{
		return false;
	}
	AnimationParameterValue parameter;
	parameter.m_floatValue = value;
	return SetParameter(name, EAnimationParameterType::Float, parameter);
}

bool AnimationControllerInstance::SetInt(const std::string& name, int32_t value)
{
	AnimationParameterValue parameter;
	parameter.m_intValue = value;
	return SetParameter(name, EAnimationParameterType::Int, parameter);
}

bool AnimationControllerInstance::SetBool(const std::string& name, bool value)
{
	AnimationParameterValue parameter;
	parameter.m_boolValue = value;
	return SetParameter(name, EAnimationParameterType::Bool, parameter);
}

bool AnimationControllerInstance::SetTrigger(const std::string& name)
{
	AnimationParameterValue parameter;
	parameter.m_boolValue = true;
	return SetParameter(name, EAnimationParameterType::Trigger, parameter);
}

bool AnimationControllerInstance::ResetTrigger(const std::string& name)
{
	AnimationParameterValue parameter;
	parameter.m_boolValue = false;
	return SetParameter(name, EAnimationParameterType::Trigger, parameter);
}

float AnimationControllerInstance::GetTransitionAlpha() const
{
	if (!IsTransitioning())
	{
		return 0.0f;
	}
	if (m_transitionDuration <= 0.0f)
	{
		return 1.0f;
	}
	return std::clamp(m_transitionElapsed / m_transitionDuration, 0.0f, 1.0f);
}

bool AnimationControllerInstance::SetParameter(
	const std::string& name,
	EAnimationParameterType type,
	const AnimationParameterValue& value)
{
	if (!SynchronizeController())
	{
		return false;
	}
	const int32_t parameterIndex = m_controller->FindParameterIndex(name);
	if (parameterIndex < 0 ||
		m_controller->GetParameters()[static_cast<size_t>(parameterIndex)].m_type != type)
	{
		return false;
	}
	m_parameterValues[static_cast<size_t>(parameterIndex)] = value;
	return true;
}

bool AnimationControllerInstance::AreConditionsMet(
	const AnimationTransitionDefinition& transition) const
{
	for (const auto& condition : transition.m_conditions)
	{
		if (condition.m_parameterIndex >= m_parameterValues.Num())
		{
			return false;
		}
		const auto& definition = m_controller->GetParameters()[condition.m_parameterIndex];
		const auto& value = m_parameterValues[condition.m_parameterIndex];
		bool bConditionMet = false;

		switch (definition.m_type)
		{
		case EAnimationParameterType::Float:
		{
			const float epsilon = std::numeric_limits<float>::epsilon() * 4.0f;
			const bool bEqual = std::abs(value.m_floatValue - condition.m_floatValue) <= epsilon;
			switch (condition.m_operation)
			{
			case EAnimationConditionOperation::Equal: bConditionMet = bEqual; break;
			case EAnimationConditionOperation::NotEqual: bConditionMet = !bEqual; break;
			case EAnimationConditionOperation::Less: bConditionMet = value.m_floatValue < condition.m_floatValue; break;
			case EAnimationConditionOperation::LessOrEqual: bConditionMet = value.m_floatValue <= condition.m_floatValue; break;
			case EAnimationConditionOperation::Greater: bConditionMet = value.m_floatValue > condition.m_floatValue; break;
			case EAnimationConditionOperation::GreaterOrEqual: bConditionMet = value.m_floatValue >= condition.m_floatValue; break;
			case EAnimationConditionOperation::IsSet: break;
			case EAnimationConditionOperation::Invalid: break;
			}
			break;
		}
		case EAnimationParameterType::Int:
			switch (condition.m_operation)
			{
			case EAnimationConditionOperation::Equal: bConditionMet = value.m_intValue == condition.m_intValue; break;
			case EAnimationConditionOperation::NotEqual: bConditionMet = value.m_intValue != condition.m_intValue; break;
			case EAnimationConditionOperation::Less: bConditionMet = value.m_intValue < condition.m_intValue; break;
			case EAnimationConditionOperation::LessOrEqual: bConditionMet = value.m_intValue <= condition.m_intValue; break;
			case EAnimationConditionOperation::Greater: bConditionMet = value.m_intValue > condition.m_intValue; break;
			case EAnimationConditionOperation::GreaterOrEqual: bConditionMet = value.m_intValue >= condition.m_intValue; break;
			case EAnimationConditionOperation::IsSet: break;
			case EAnimationConditionOperation::Invalid: break;
			}
			break;
		case EAnimationParameterType::Bool:
			bConditionMet = condition.m_operation == EAnimationConditionOperation::Equal ?
				value.m_boolValue == condition.m_boolValue :
				value.m_boolValue != condition.m_boolValue;
			break;
		case EAnimationParameterType::Trigger:
			bConditionMet = value.m_boolValue;
			break;
		case EAnimationParameterType::Invalid:
			break;
		}

		if (!bConditionMet)
		{
			return false;
		}
	}
	return true;
}

void AnimationControllerInstance::ConsumeTriggers(
	const AnimationTransitionDefinition& transition)
{
	for (const auto& condition : transition.m_conditions)
	{
		if (condition.m_parameterIndex < m_parameterValues.Num() &&
			m_controller->GetParameters()[condition.m_parameterIndex].m_type ==
				EAnimationParameterType::Trigger)
		{
			m_parameterValues[condition.m_parameterIndex].m_boolValue = false;
		}
	}
}

void AnimationControllerInstance::TryBeginTransition(float activeClipDuration)
{
	const AnimationTransitionDefinition* selected = nullptr;
	for (const auto& transition : m_controller->GetTransitions())
	{
		if (transition.m_fromStateIndex != m_activeStateIndex)
		{
			continue;
		}

		const float normalizedTime = activeClipDuration > 0.0f ?
			m_activeStateTime / activeClipDuration : 0.0f;
		if ((transition.m_bHasExitTime && normalizedTime < transition.m_exitTime) ||
			!AreConditionsMet(transition))
		{
			continue;
		}

		if (!selected || transition.m_priority > selected->m_priority)
		{
			selected = &transition;
		}
	}

	if (!selected)
	{
		return;
	}

	m_destinationStateIndex = selected->m_toStateIndex;
	m_destinationStateId = selected->m_toStateId;
	m_destinationStateTime = 0.0f;
	m_transitionElapsed = 0.0f;
	m_transitionDuration = selected->m_duration;
	ConsumeTriggers(*selected);
	if (m_transitionDuration <= 0.0f)
	{
		CompleteTransition();
	}
}

void AnimationControllerInstance::CompleteTransition()
{
	if (!IsTransitioning())
	{
		return;
	}
	m_activeStateIndex = m_destinationStateIndex;
	m_activeStateId = m_destinationStateId;
	m_activeStateTime = m_destinationStateTime;
	m_destinationStateIndex = InvalidIndex;
	m_destinationStateId = InvalidAnimationControllerNodeId;
	m_destinationStateTime = 0.0f;
	m_transitionElapsed = 0.0f;
	m_transitionDuration = 0.0f;
}

bool AnimationControllerInstance::SynchronizeController()
{
	if (!m_controller)
	{
		return false;
	}
	if (m_controllerRevision == m_controller->GetRevision())
	{
		return true;
	}

	TVector<AnimationParameterValue> previousValues = m_parameterValues;
	TVector<AnimationControllerNodeId> previousIds = m_parameterIds;
	TVector<EAnimationParameterType> previousTypes = m_parameterTypes;
	m_parameterValues.Clear();
	m_parameterIds.Clear();
	m_parameterTypes.Clear();
	m_parameterValues.Resize(m_controller->GetParameters().Num());
	m_parameterIds.Resize(m_controller->GetParameters().Num());
	m_parameterTypes.Resize(m_controller->GetParameters().Num());
	for (size_t i = 0; i < m_controller->GetParameters().Num(); ++i)
	{
		const auto& definition = m_controller->GetParameters()[i];
		auto& value = m_parameterValues[i];
		value.m_floatValue = definition.m_defaultFloat;
		value.m_intValue = definition.m_defaultInt;
		value.m_boolValue = definition.m_type == EAnimationParameterType::Trigger ?
			false : definition.m_defaultBool;
		m_parameterIds[i] = definition.m_id;
		m_parameterTypes[i] = definition.m_type;
		for (size_t previousIndex = 0; previousIndex < previousIds.Num(); ++previousIndex)
		{
			if (previousIds[previousIndex] == definition.m_id &&
				previousIndex < previousTypes.Num() &&
				previousTypes[previousIndex] == definition.m_type)
			{
				value = previousValues[previousIndex];
				break;
			}
		}
	}

	const int32_t activeStateIndex = m_controller->FindStateIndex(m_activeStateId);
	const bool bActiveStateWasRemoved = activeStateIndex < 0;
	m_activeStateIndex = activeStateIndex >= 0 ?
		static_cast<uint32_t>(activeStateIndex) : m_controller->GetDefaultStateIndex();
	if (m_activeStateIndex >= m_controller->GetStates().Num())
	{
		return false;
	}
	m_activeStateId = m_controller->GetStates()[m_activeStateIndex].m_id;
	if (bActiveStateWasRemoved)
	{
		m_activeStateTime = 0.0f;
	}

	const int32_t destinationStateIndex = m_controller->FindStateIndex(m_destinationStateId);
	m_destinationStateIndex = !bActiveStateWasRemoved && destinationStateIndex >= 0 ?
		static_cast<uint32_t>(destinationStateIndex) : InvalidIndex;
	if (m_destinationStateIndex == InvalidIndex)
	{
		m_destinationStateId = InvalidAnimationControllerNodeId;
		m_destinationStateTime = 0.0f;
		m_transitionElapsed = 0.0f;
		m_transitionDuration = 0.0f;
	}
	m_controllerRevision = m_controller->GetRevision();
	return true;
}
