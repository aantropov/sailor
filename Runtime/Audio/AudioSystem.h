#pragma once

#include "Audio/AudioTypes.h"
#include "Core/Submodule.h"
#include "Engine/Types.h"
#include "Memory/UniquePtr.hpp"

namespace Sailor
{
	class SAILOR_API AudioSystem final : public TSubmodule<AudioSystem>
	{
	public:
		explicit AudioSystem(bool bForceNullDevice = false);
		~AudioSystem() override;

		bool IsInitialized() const;
		bool IsUsingNullDevice() const;

		bool CreateVoice(const AudioClipPtr& clip, AudioVoiceId& outVoiceId);
		void DestroyVoice(AudioVoiceId voiceId);
		bool SetVoiceSettings(AudioVoiceId voiceId, const AudioVoiceSettings& settings);
		bool SetVoiceTransform(AudioVoiceId voiceId, const AudioTransformState& transform);
		bool PlayVoice(AudioVoiceId voiceId, bool bRestart = false);
		bool StopVoice(AudioVoiceId voiceId);
		bool IsVoicePlaying(AudioVoiceId voiceId) const;
		uint64_t GetVoiceClipRevision(AudioVoiceId voiceId) const;
		bool GetVoiceTransform(AudioVoiceId voiceId, AudioTransformState& outTransform) const;
		size_t GetNumVoices() const;

		AudioListenerId RegisterListener(const AudioListenerState& state);
		void UnregisterListener(AudioListenerId listenerId);
		bool UpdateListener(AudioListenerId listenerId, const AudioListenerState& state);
		AudioListenerId GetActiveListener() const;
		bool GetActiveListenerState(AudioListenerState& outState) const;

		// Schedules a lightweight backend status refresh. AudioECS calls this once
		// per world update; repeated calls are coalesced by the audio command queue.
		void Update();
		void Flush();

	private:
		class Impl;
		TUniquePtr<Impl> m_pImpl;
	};
}
