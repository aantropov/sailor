#pragma once

#include "Audio/AudioTypes.h"
#include "ECS/ECS.h"
#include "Engine/Types.h"

namespace Sailor
{
	class AudioSourceComponent;
	class AudioListenerComponent;
	class AudioSystem;

	class AudioSourceData final : public ECS::TComponent
	{
	public:
		const AudioClipPtr& GetClip() const { return m_clip; }
		const AudioVoiceSettings& GetSettings() const { return m_settings; }
		bool IsPlaybackRequested() const { return m_bPlaybackRequested; }

	private:
		AudioSourceComponent* m_component = nullptr;
		AudioClipPtr m_clip{};
		AudioClipPtr m_voiceClip{};
		AudioClipPtr m_attemptedClip{};
		AudioVoiceSettings m_settings{};
		AudioVoiceId m_voiceId = InvalidAudioVoiceId;
		uint64_t m_playRequest = 0;
		uint64_t m_appliedPlayRequest = 0;
		uint64_t m_attemptedClipRevision = 0;
		size_t m_lastTransformFrame = 0;
		glm::vec3 m_lastPosition{};
		glm::vec3 m_lastVelocity{};
		bool m_bPlaybackRequested = false;
		bool m_bHasLastPosition = false;

		friend class AudioECS;
		friend class AudioSourceComponent;
	};

	class AudioListenerData final
	{
	public:
		bool IsActive() const { return m_bIsActive; }

	private:
		AudioListenerComponent* m_component = nullptr;
		ObjectPtr m_owner{};
		AudioListenerId m_listenerId = InvalidAudioListenerId;
		int32_t m_priority = 0;
		size_t m_lastTransformFrame = 0;
		glm::vec3 m_lastPosition{};
		glm::vec3 m_lastVelocity{};
		bool m_bEnabled = true;
		bool m_bIsActive = false;
		bool m_bIsDirty = true;
		bool m_bHasLastPosition = false;

		friend class AudioECS;
		friend class AudioListenerComponent;
	};

	class SAILOR_API AudioECS final :
		public ECS::TSystem<AudioECS, AudioSourceData>
	{
	public:
		Tasks::ITaskPtr Tick(float deltaTime) override;
		void EndPlay() override;
		uint32_t GetOrder() const override { return 250; }

		size_t RegisterListener();
		void UnregisterListener(size_t index);
		AudioListenerData& GetListenerData(size_t index);
		const AudioListenerData& GetListenerData(size_t index) const;

	protected:
		void OnComponentUnregistered(
			size_t index,
			AudioSourceData& component) override;

	private:
		AudioSystem* GetAudioSystem() const;
		void DestroyVoice(AudioSourceData& data);
		void TickSource(AudioSystem& audioSystem, AudioSourceData& data, float deltaTime);
		void TickListener(AudioSystem& audioSystem, AudioListenerData& data, float deltaTime);

		TVector<AudioListenerData> m_listeners{};
		TList<size_t> m_freeListeners{};
	};

	template class ECS::TSystem<AudioECS, AudioSourceData>;
}
