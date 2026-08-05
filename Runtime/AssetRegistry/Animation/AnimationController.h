#pragma once

#include "AssetRegistry/FileId.h"
#include "Containers/Vector.h"
#include "Core/YamlSerializable.h"
#include "Engine/Object.h"
#include "Memory/LockFreeHeapAllocator.h"

#include <cstdint>
#include <limits>
#include <string>

namespace Sailor
{
	using AnimationControllerNodeId = uint64_t;
	static constexpr AnimationControllerNodeId InvalidAnimationControllerNodeId = 0;

	enum class EAnimationParameterType : uint8_t
	{
		Float,
		Int,
		Bool,
		Trigger,
		Invalid = 0xff
	};

	enum class EAnimationConditionOperation : uint8_t
	{
		Equal,
		NotEqual,
		Less,
		LessOrEqual,
		Greater,
		GreaterOrEqual,
		IsSet,
		Invalid = 0xff
	};

	struct AnimationParameterDefinition
	{
		AnimationControllerNodeId m_id = InvalidAnimationControllerNodeId;
		std::string m_name;
		EAnimationParameterType m_type = EAnimationParameterType::Float;
		float m_defaultFloat = 0.0f;
		int32_t m_defaultInt = 0;
		bool m_defaultBool = false;
	};

	struct AnimationStateDefinition
	{
		AnimationControllerNodeId m_id = InvalidAnimationControllerNodeId;
		std::string m_name;
		std::string m_clipSlot;
		float m_speed = 1.0f;
		bool m_bLoop = true;
		float m_editorX = 0.0f;
		float m_editorY = 0.0f;
	};

	struct AnimationTransitionCondition
	{
		AnimationControllerNodeId m_parameterId = InvalidAnimationControllerNodeId;
		EAnimationConditionOperation m_operation = EAnimationConditionOperation::Equal;
		float m_floatValue = 0.0f;
		int32_t m_intValue = 0;
		bool m_boolValue = false;

		uint32_t m_parameterIndex = (std::numeric_limits<uint32_t>::max)();
	};

	struct AnimationTransitionDefinition
	{
		AnimationControllerNodeId m_id = InvalidAnimationControllerNodeId;
		AnimationControllerNodeId m_fromStateId = InvalidAnimationControllerNodeId;
		AnimationControllerNodeId m_toStateId = InvalidAnimationControllerNodeId;
		int32_t m_priority = 0;
		float m_duration = 0.2f;
		bool m_bHasExitTime = false;
		float m_exitTime = 0.0f;
		TVector<AnimationTransitionCondition> m_conditions;

		uint32_t m_fromStateIndex = (std::numeric_limits<uint32_t>::max)();
		uint32_t m_toStateIndex = (std::numeric_limits<uint32_t>::max)();
	};

	struct AnimationSetEntry
	{
		std::string m_slot;
		FileId m_animation;
	};

	class AnimationControllerAsset final : public IYamlSerializable
	{
	public:
		SAILOR_API YAML::Node Serialize() const override;
		SAILOR_API void Deserialize(const YAML::Node& inData) override;

		TVector<AnimationParameterDefinition>& GetParameters() { return m_parameters; }
		const TVector<AnimationParameterDefinition>& GetParameters() const { return m_parameters; }
		TVector<AnimationStateDefinition>& GetStates() { return m_states; }
		const TVector<AnimationStateDefinition>& GetStates() const { return m_states; }
		TVector<AnimationTransitionDefinition>& GetTransitions() { return m_transitions; }
		const TVector<AnimationTransitionDefinition>& GetTransitions() const { return m_transitions; }

		AnimationControllerNodeId GetDefaultStateId() const { return m_defaultStateId; }
		void SetDefaultStateId(AnimationControllerNodeId stateId) { m_defaultStateId = stateId; }

	private:
		uint32_t m_version = 1;
		AnimationControllerNodeId m_defaultStateId = InvalidAnimationControllerNodeId;
		TVector<AnimationParameterDefinition> m_parameters;
		TVector<AnimationStateDefinition> m_states;
		TVector<AnimationTransitionDefinition> m_transitions;
	};

	class AnimationSetAsset final : public IYamlSerializable
	{
	public:
		SAILOR_API YAML::Node Serialize() const override;
		SAILOR_API void Deserialize(const YAML::Node& inData) override;

		TVector<AnimationSetEntry>& GetEntries() { return m_entries; }
		const TVector<AnimationSetEntry>& GetEntries() const { return m_entries; }

	private:
		uint32_t m_version = 1;
		TVector<AnimationSetEntry> m_entries;
	};

	class AnimationController final : public Object
	{
	public:
		SAILOR_API explicit AnimationController(FileId uid) : Object(uid) {}

		SAILOR_API bool Initialize(
			const AnimationControllerAsset& asset,
			TVector<std::string>* outErrors = nullptr);
		SAILOR_API bool Validate(TVector<std::string>* outErrors = nullptr) const;

		SAILOR_API int32_t FindStateIndex(AnimationControllerNodeId stateId) const;
		SAILOR_API int32_t FindParameterIndex(AnimationControllerNodeId parameterId) const;
		SAILOR_API int32_t FindParameterIndex(const std::string& name) const;

		const TVector<AnimationParameterDefinition>& GetParameters() const { return m_parameters; }
		const TVector<AnimationStateDefinition>& GetStates() const { return m_states; }
		const TVector<AnimationTransitionDefinition>& GetTransitions() const { return m_transitions; }
		uint32_t GetDefaultStateIndex() const { return m_defaultStateIndex; }
		uint64_t GetRevision() const { return m_revision; }

	private:
		TVector<AnimationParameterDefinition> m_parameters;
		TVector<AnimationStateDefinition> m_states;
		TVector<AnimationTransitionDefinition> m_transitions;
		uint32_t m_defaultStateIndex = 0;
		uint64_t m_revision = 0;
	};

	using AnimationControllerPtr = TObjectPtr<AnimationController>;

	class AnimationSet final : public Object
	{
	public:
		SAILOR_API explicit AnimationSet(FileId uid) : Object(uid) {}

		SAILOR_API bool Initialize(
			const AnimationSetAsset& asset,
			TVector<std::string>* outErrors = nullptr);
		SAILOR_API const FileId* FindAnimation(const std::string& slot) const;
		const TVector<AnimationSetEntry>& GetEntries() const { return m_entries; }
		uint64_t GetRevision() const { return m_revision; }

	private:
		TVector<AnimationSetEntry> m_entries;
		uint64_t m_revision = 0;
	};

	using AnimationSetPtr = TObjectPtr<AnimationSet>;

	struct AnimationParameterValue
	{
		float m_floatValue = 0.0f;
		int32_t m_intValue = 0;
		bool m_boolValue = false;
	};

	class AnimationControllerInstance final
	{
	public:
		SAILOR_API bool SetController(const AnimationControllerPtr& controller);
		SAILOR_API void Reset();
		SAILOR_API void Tick(float deltaTime, float activeClipDuration);

		SAILOR_API bool SetFloat(const std::string& name, float value);
		SAILOR_API bool SetInt(const std::string& name, int32_t value);
		SAILOR_API bool SetBool(const std::string& name, bool value);
		SAILOR_API bool SetTrigger(const std::string& name);
		SAILOR_API bool ResetTrigger(const std::string& name);

		bool IsValid() const { return m_controller && !m_controller->GetStates().IsEmpty(); }
		bool IsTransitioning() const { return m_destinationStateIndex != InvalidIndex; }
		uint32_t GetActiveStateIndex() const { return m_activeStateIndex; }
		uint32_t GetDestinationStateIndex() const { return m_destinationStateIndex; }
		float GetActiveStateTime() const { return m_activeStateTime; }
		float GetDestinationStateTime() const { return m_destinationStateTime; }
		SAILOR_API float GetTransitionAlpha() const;
		const AnimationControllerPtr& GetController() const { return m_controller; }

		static constexpr uint32_t InvalidIndex = (std::numeric_limits<uint32_t>::max)();

	private:
		bool SetParameter(const std::string& name, EAnimationParameterType type, const AnimationParameterValue& value);
		bool AreConditionsMet(const AnimationTransitionDefinition& transition) const;
		void ConsumeTriggers(const AnimationTransitionDefinition& transition);
		void TryBeginTransition(float activeClipDuration);
		void CompleteTransition();
		bool SynchronizeController();

		AnimationControllerPtr m_controller;
		TVector<AnimationParameterValue> m_parameterValues;
		TVector<AnimationControllerNodeId> m_parameterIds;
		TVector<EAnimationParameterType> m_parameterTypes;
		uint32_t m_activeStateIndex = InvalidIndex;
		uint32_t m_destinationStateIndex = InvalidIndex;
		AnimationControllerNodeId m_activeStateId = InvalidAnimationControllerNodeId;
		AnimationControllerNodeId m_destinationStateId = InvalidAnimationControllerNodeId;
		uint64_t m_controllerRevision = 0;
		float m_activeStateTime = 0.0f;
		float m_destinationStateTime = 0.0f;
		float m_transitionElapsed = 0.0f;
		float m_transitionDuration = 0.0f;
	};
}
