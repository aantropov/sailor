#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#include <miniaudio.h>

#include "Audio/AudioSystem.h"

#include "AssetRegistry/Audio/AudioImporter.h"
#include "Containers/Map.h"
#include "Containers/Vector.h"
#include "Core/LogMacros.h"
#include "Core/SpinLock.h"
#include "Tasks/Scheduler.h"
#include "Tasks/Tasks.h"

#include <algorithm>
#include <atomic>
#include <utility>

using namespace Sailor;

class AudioSystem::Impl
{
public:
	enum class ECommandType : uint8_t
	{
		CreateVoice,
		DestroyVoice,
		SetVoiceSettings,
		SetVoiceTransform,
		PlayVoice,
		StopVoice,
		RegisterListener,
		UnregisterListener,
		UpdateListener,
		Refresh
	};

	struct Command
	{
		ECommandType m_type = ECommandType::Refresh;
		AudioVoiceId m_voiceId = InvalidAudioVoiceId;
		AudioListenerId m_listenerId = InvalidAudioListenerId;
		AudioClipSnapshot m_clip{};
		AudioVoiceSettings m_settings{};
		AudioTransformState m_transform{};
		AudioListenerState m_listener{};
		bool m_bRestart = false;
	};

	struct Voice
	{
		ma_sound m_sound{};
		bool m_bInitialized = false;

		~Voice()
		{
			if (m_bInitialized)
			{
				ma_sound_uninit(&m_sound);
			}
		}
	};

	struct VoiceState
	{
		AudioTransformState m_transform{};
		uint64_t m_clipRevision = 0;
		bool m_bPlaying = false;
	};

	void InitializeBackend(bool bForceNullDevice)
	{
		check(App::GetSubmodule<Tasks::Scheduler>()->IsAudioThread());

		ma_engine_config config = ma_engine_config_init();
		config.listenerCount = 1;
		config.noDevice = bForceNullDevice ? MA_TRUE : MA_FALSE;
		if (bForceNullDevice)
		{
			config.channels = 2;
			config.sampleRate = 48000;
		}

		ma_result result = ma_engine_init(&config, &m_engine);
		if (result != MA_SUCCESS && !bForceNullDevice)
		{
			config = ma_engine_config_init();
			config.listenerCount = 1;
			config.noDevice = MA_TRUE;
			config.channels = 2;
			config.sampleRate = 48000;
			result = ma_engine_init(&config, &m_engine);
			m_bNullDevice.store(result == MA_SUCCESS, std::memory_order_release);
			if (result == MA_SUCCESS)
			{
				SAILOR_LOG("Audio device is unavailable; using the null audio backend.");
			}
		}
		else
		{
			m_bNullDevice.store(bForceNullDevice, std::memory_order_release);
		}

		m_bInitialized.store(result == MA_SUCCESS, std::memory_order_release);
		if (result == MA_SUCCESS && bForceNullDevice)
		{
			SAILOR_LOG("Audio backend initialized in null-device mode.");
		}
		if (result != MA_SUCCESS)
		{
			SAILOR_LOG_ERROR("Cannot initialize miniaudio: %s", ma_result_description(result));
		}
	}

	void ShutdownBackend()
	{
		check(App::GetSubmodule<Tasks::Scheduler>()->IsAudioThread());
		m_voices.Clear();
		m_listeners.Clear();
		if (m_bInitialized.exchange(false, std::memory_order_acq_rel))
		{
			ma_engine_uninit(&m_engine);
		}

		m_stateLock.Lock();
		m_voiceStates.Clear();
		m_listenerStates.Clear();
		m_activeListener = InvalidAudioListenerId;
		m_stateLock.Unlock();
	}

	void Enqueue(Command&& command)
	{
		Tasks::Scheduler* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		if (!scheduler)
		{
			return;
		}

		bool bScheduleDrain = false;
		m_pendingLock.Lock();
		if (command.m_type == ECommandType::Refresh && m_bRefreshPending)
		{
			m_pendingLock.Unlock();
			return;
		}
		m_bRefreshPending |= command.m_type == ECommandType::Refresh;
		m_pendingCommands.Emplace(std::move(command));
		if (!m_bDrainScheduled)
		{
			m_bDrainScheduled = true;
			bScheduleDrain = true;
		}
		m_pendingLock.Unlock();

		if (bScheduleDrain)
		{
			scheduler->Run(Tasks::CreateTask(
				"Process audio commands",
				[this]() { DrainCommands(); },
				EThreadType::Audio));
		}
	}

	void DrainCommands()
	{
		check(App::GetSubmodule<Tasks::Scheduler>()->IsAudioThread());
		for (;;)
		{
			TVector<Command> commands;
			m_pendingLock.Lock();
			if (m_pendingCommands.IsEmpty())
			{
				m_bDrainScheduled = false;
				m_pendingLock.Unlock();
				return;
			}
			commands = std::move(m_pendingCommands);
			m_pendingCommands = {};
			m_bRefreshPending = false;
			m_pendingLock.Unlock();

			for (const Command& command : commands)
			{
				ProcessCommand(command);
			}
			RefreshVoiceStates();
		}
	}

	void ProcessCommand(const Command& command)
	{
		switch (command.m_type)
		{
		case ECommandType::CreateVoice:
		{
			auto voice = TUniquePtr<Voice>::Make();
			const ma_uint32 flags = command.m_clip.m_bStream
				? MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_ASYNC
				: MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC;
			const ma_result result = ma_sound_init_from_file(
				&m_engine,
				command.m_clip.m_sourcePath.c_str(),
				flags,
				nullptr,
				nullptr,
				&voice->m_sound);
			if (result == MA_SUCCESS)
			{
				voice->m_bInitialized = true;
				m_voices.Insert(command.m_voiceId, std::move(voice));
			}
			else
			{
				SAILOR_LOG_ERROR(
					"Cannot create audio voice for '%s': %s",
					command.m_clip.m_sourcePath.c_str(),
					ma_result_description(result));
			}
			break;
		}
		case ECommandType::DestroyVoice:
			m_voices.Remove(command.m_voiceId);
			break;
		case ECommandType::SetVoiceSettings:
			if (Voice* voice = FindVoice(command.m_voiceId))
			{
				const AudioVoiceSettings& settings = command.m_settings;
				ma_sound_set_volume(&voice->m_sound, (std::max)(0.0f, settings.m_volume));
				ma_sound_set_pitch(&voice->m_sound, (std::max)(0.01f, settings.m_pitch));
				ma_sound_set_looping(&voice->m_sound, settings.m_bLoop ? MA_TRUE : MA_FALSE);
				ma_sound_set_spatialization_enabled(&voice->m_sound, settings.m_bSpatial ? MA_TRUE : MA_FALSE);
				ma_sound_set_min_distance(&voice->m_sound, (std::max)(0.01f, settings.m_minDistance));
				ma_sound_set_max_distance(&voice->m_sound, (std::max)(settings.m_maxDistance, settings.m_minDistance));
			}
			break;
		case ECommandType::SetVoiceTransform:
			if (Voice* voice = FindVoice(command.m_voiceId))
			{
				const AudioTransformState& transform = command.m_transform;
				ma_sound_set_position(&voice->m_sound, transform.m_position.x, transform.m_position.y, transform.m_position.z);
				ma_sound_set_direction(&voice->m_sound, transform.m_forward.x, transform.m_forward.y, transform.m_forward.z);
				ma_sound_set_velocity(&voice->m_sound, transform.m_velocity.x, transform.m_velocity.y, transform.m_velocity.z);
			}
			break;
		case ECommandType::PlayVoice:
			if (Voice* voice = FindVoice(command.m_voiceId))
			{
				if (command.m_bRestart)
				{
					ma_sound_seek_to_pcm_frame(&voice->m_sound, 0);
				}
				ma_sound_start(&voice->m_sound);
			}
			break;
		case ECommandType::StopVoice:
			if (Voice* voice = FindVoice(command.m_voiceId))
			{
				ma_sound_stop(&voice->m_sound);
			}
			break;
		case ECommandType::RegisterListener:
			m_listeners.Insert(command.m_listenerId, command.m_listener);
			ApplyActiveListener();
			break;
		case ECommandType::UnregisterListener:
			m_listeners.Remove(command.m_listenerId);
			ApplyActiveListener();
			break;
		case ECommandType::UpdateListener:
			if (auto it = m_listeners.Find(command.m_listenerId); it != m_listeners.end())
			{
				it.Value() = command.m_listener;
				ApplyActiveListener();
			}
			break;
		case ECommandType::Refresh:
			break;
		}
	}

	Voice* FindVoice(AudioVoiceId voiceId)
	{
		auto it = m_voices.Find(voiceId);
		return it != m_voices.end() ? it.Value().GetRawPtr() : nullptr;
	}

	void ApplyActiveListener()
	{
		AudioListenerId selectedId = InvalidAudioListenerId;
		const AudioListenerState* selectedState = nullptr;
		for (const auto& entry : m_listeners)
		{
			const AudioListenerState& state = *entry.m_second;
			if (!state.m_bEnabled)
			{
				continue;
			}

			if (selectedState == nullptr ||
				state.m_priority > selectedState->m_priority ||
				(state.m_priority == selectedState->m_priority && entry.m_first < selectedId))
			{
				selectedId = entry.m_first;
				selectedState = &state;
			}
		}

		ma_engine_listener_set_enabled(&m_engine, 0, selectedState ? MA_TRUE : MA_FALSE);
		if (selectedState)
		{
			const AudioTransformState& transform = selectedState->m_transform;
			ma_engine_listener_set_position(&m_engine, 0, transform.m_position.x, transform.m_position.y, transform.m_position.z);
			ma_engine_listener_set_direction(&m_engine, 0, transform.m_forward.x, transform.m_forward.y, transform.m_forward.z);
			ma_engine_listener_set_world_up(&m_engine, 0, transform.m_up.x, transform.m_up.y, transform.m_up.z);
			ma_engine_listener_set_velocity(&m_engine, 0, transform.m_velocity.x, transform.m_velocity.y, transform.m_velocity.z);
		}

		m_stateLock.Lock();
		m_activeListener = selectedId;
		m_stateLock.Unlock();
	}

	void RefreshVoiceStates()
	{
		m_stateLock.Lock();
		for (const auto& entry : m_voiceStates)
		{
			VoiceState& state = *entry.m_second;
			Voice* voice = FindVoice(entry.m_first);
			state.m_bPlaying = voice && ma_sound_is_playing(&voice->m_sound) == MA_TRUE;
		}
		m_stateLock.Unlock();
	}

	ma_engine m_engine{};
	TMap<AudioVoiceId, TUniquePtr<Voice>> m_voices{};
	TMap<AudioListenerId, AudioListenerState> m_listeners{};

	mutable SpinLock m_stateLock{};
	TMap<AudioVoiceId, VoiceState> m_voiceStates{};
	TMap<AudioListenerId, AudioListenerState> m_listenerStates{};
	AudioListenerId m_activeListener = InvalidAudioListenerId;

	SpinLock m_pendingLock{};
	TVector<Command> m_pendingCommands{};
	bool m_bDrainScheduled = false;
	bool m_bRefreshPending = false;

	std::atomic<AudioVoiceId> m_nextVoiceId{ 1 };
	std::atomic<AudioListenerId> m_nextListenerId{ 1 };
	std::atomic<bool> m_bInitialized{ false };
	std::atomic<bool> m_bNullDevice{ false };
};

AudioSystem::AudioSystem(bool bForceNullDevice) :
	m_pImpl(TUniquePtr<Impl>::Make())
{
	Tasks::Scheduler* scheduler = App::GetSubmodule<Tasks::Scheduler>();
	if (!scheduler)
	{
		SAILOR_LOG_ERROR("AudioSystem requires Tasks::Scheduler.");
		return;
	}

	auto initialize = Tasks::CreateTask(
		"Initialize audio backend",
		[this, bForceNullDevice]() { m_pImpl->InitializeBackend(bForceNullDevice); },
		EThreadType::Audio);
	scheduler->Run(initialize);
	initialize->Wait();
}

AudioSystem::~AudioSystem()
{
	Tasks::Scheduler* scheduler = App::GetSubmodule<Tasks::Scheduler>();
	if (!scheduler || !m_pImpl)
	{
		return;
	}

	Flush();
	auto shutdown = Tasks::CreateTask(
		"Shutdown audio backend",
		[this]() { m_pImpl->ShutdownBackend(); },
		EThreadType::Audio);
	scheduler->Run(shutdown);
	shutdown->Wait();
}

bool AudioSystem::IsInitialized() const
{
	return m_pImpl && m_pImpl->m_bInitialized.load(std::memory_order_acquire);
}

bool AudioSystem::IsUsingNullDevice() const
{
	return m_pImpl && m_pImpl->m_bNullDevice.load(std::memory_order_acquire);
}

bool AudioSystem::CreateVoice(const AudioClipPtr& clip, AudioVoiceId& outVoiceId)
{
	outVoiceId = InvalidAudioVoiceId;
	if (!IsInitialized() || !clip || !clip->IsReady())
	{
		return false;
	}

	const AudioClipSnapshot snapshot = clip->GetSnapshot();
	if (snapshot.m_sourcePath.empty())
	{
		return false;
	}

	AudioVoiceId voiceId = m_pImpl->m_nextVoiceId.fetch_add(1, std::memory_order_relaxed);
	if (voiceId == InvalidAudioVoiceId)
	{
		voiceId = m_pImpl->m_nextVoiceId.fetch_add(1, std::memory_order_relaxed);
	}

	Impl::VoiceState state{};
	state.m_clipRevision = snapshot.m_revision;
	m_pImpl->m_stateLock.Lock();
	const bool bInserted = m_pImpl->m_voiceStates.Insert(voiceId, state);
	m_pImpl->m_stateLock.Unlock();
	if (!bInserted)
	{
		return false;
	}

	Impl::Command command{};
	command.m_type = Impl::ECommandType::CreateVoice;
	command.m_voiceId = voiceId;
	command.m_clip = snapshot;
	m_pImpl->Enqueue(std::move(command));
	outVoiceId = voiceId;
	return true;
}

void AudioSystem::DestroyVoice(AudioVoiceId voiceId)
{
	if (!m_pImpl || voiceId == InvalidAudioVoiceId)
	{
		return;
	}

	m_pImpl->m_stateLock.Lock();
	const bool bRemoved = m_pImpl->m_voiceStates.Remove(voiceId);
	m_pImpl->m_stateLock.Unlock();
	if (bRemoved)
	{
		Impl::Command command{};
		command.m_type = Impl::ECommandType::DestroyVoice;
		command.m_voiceId = voiceId;
		m_pImpl->Enqueue(std::move(command));
	}
}

bool AudioSystem::SetVoiceSettings(AudioVoiceId voiceId, const AudioVoiceSettings& settings)
{
	if (!m_pImpl)
	{
		return false;
	}
	m_pImpl->m_stateLock.Lock();
	const bool bExists = m_pImpl->m_voiceStates.ContainsKey(voiceId);
	m_pImpl->m_stateLock.Unlock();
	if (!bExists)
	{
		return false;
	}

	Impl::Command command{};
	command.m_type = Impl::ECommandType::SetVoiceSettings;
	command.m_voiceId = voiceId;
	command.m_settings = settings;
	m_pImpl->Enqueue(std::move(command));
	return true;
}

bool AudioSystem::SetVoiceTransform(AudioVoiceId voiceId, const AudioTransformState& transform)
{
	if (!m_pImpl)
	{
		return false;
	}
	m_pImpl->m_stateLock.Lock();
	auto it = m_pImpl->m_voiceStates.Find(voiceId);
	if (it != m_pImpl->m_voiceStates.end())
	{
		it.Value().m_transform = transform;
	}
	const bool bExists = it != m_pImpl->m_voiceStates.end();
	m_pImpl->m_stateLock.Unlock();
	if (!bExists)
	{
		return false;
	}

	Impl::Command command{};
	command.m_type = Impl::ECommandType::SetVoiceTransform;
	command.m_voiceId = voiceId;
	command.m_transform = transform;
	m_pImpl->Enqueue(std::move(command));
	return true;
}

bool AudioSystem::PlayVoice(AudioVoiceId voiceId, bool bRestart)
{
	if (!m_pImpl)
	{
		return false;
	}
	m_pImpl->m_stateLock.Lock();
	auto it = m_pImpl->m_voiceStates.Find(voiceId);
	if (it != m_pImpl->m_voiceStates.end())
	{
		it.Value().m_bPlaying = true;
	}
	const bool bExists = it != m_pImpl->m_voiceStates.end();
	m_pImpl->m_stateLock.Unlock();
	if (!bExists)
	{
		return false;
	}

	Impl::Command command{};
	command.m_type = Impl::ECommandType::PlayVoice;
	command.m_voiceId = voiceId;
	command.m_bRestart = bRestart;
	m_pImpl->Enqueue(std::move(command));
	return true;
}

bool AudioSystem::StopVoice(AudioVoiceId voiceId)
{
	if (!m_pImpl)
	{
		return false;
	}
	m_pImpl->m_stateLock.Lock();
	auto it = m_pImpl->m_voiceStates.Find(voiceId);
	if (it != m_pImpl->m_voiceStates.end())
	{
		it.Value().m_bPlaying = false;
	}
	const bool bExists = it != m_pImpl->m_voiceStates.end();
	m_pImpl->m_stateLock.Unlock();
	if (!bExists)
	{
		return false;
	}

	Impl::Command command{};
	command.m_type = Impl::ECommandType::StopVoice;
	command.m_voiceId = voiceId;
	m_pImpl->Enqueue(std::move(command));
	return true;
}

bool AudioSystem::IsVoicePlaying(AudioVoiceId voiceId) const
{
	if (!m_pImpl)
	{
		return false;
	}
	m_pImpl->m_stateLock.Lock();
	const auto it = m_pImpl->m_voiceStates.Find(voiceId);
	const bool bPlaying = it != m_pImpl->m_voiceStates.end() && it.Value().m_bPlaying;
	m_pImpl->m_stateLock.Unlock();
	return bPlaying;
}

uint64_t AudioSystem::GetVoiceClipRevision(AudioVoiceId voiceId) const
{
	if (!m_pImpl)
	{
		return 0;
	}
	m_pImpl->m_stateLock.Lock();
	const auto it = m_pImpl->m_voiceStates.Find(voiceId);
	const uint64_t revision = it != m_pImpl->m_voiceStates.end() ? it.Value().m_clipRevision : 0;
	m_pImpl->m_stateLock.Unlock();
	return revision;
}

bool AudioSystem::GetVoiceTransform(AudioVoiceId voiceId, AudioTransformState& outTransform) const
{
	if (!m_pImpl)
	{
		return false;
	}
	m_pImpl->m_stateLock.Lock();
	const auto it = m_pImpl->m_voiceStates.Find(voiceId);
	const bool bExists = it != m_pImpl->m_voiceStates.end();
	if (bExists)
	{
		outTransform = it.Value().m_transform;
	}
	m_pImpl->m_stateLock.Unlock();
	return bExists;
}

size_t AudioSystem::GetNumVoices() const
{
	if (!m_pImpl)
	{
		return 0;
	}
	m_pImpl->m_stateLock.Lock();
	const size_t numVoices = m_pImpl->m_voiceStates.Num();
	m_pImpl->m_stateLock.Unlock();
	return numVoices;
}

AudioListenerId AudioSystem::RegisterListener(const AudioListenerState& state)
{
	if (!IsInitialized())
	{
		return InvalidAudioListenerId;
	}

	AudioListenerId listenerId = m_pImpl->m_nextListenerId.fetch_add(1, std::memory_order_relaxed);
	if (listenerId == InvalidAudioListenerId)
	{
		listenerId = m_pImpl->m_nextListenerId.fetch_add(1, std::memory_order_relaxed);
	}

	m_pImpl->m_stateLock.Lock();
	const bool bInserted = m_pImpl->m_listenerStates.Insert(listenerId, state);
	m_pImpl->m_stateLock.Unlock();
	if (!bInserted)
	{
		return InvalidAudioListenerId;
	}

	Impl::Command command{};
	command.m_type = Impl::ECommandType::RegisterListener;
	command.m_listenerId = listenerId;
	command.m_listener = state;
	m_pImpl->Enqueue(std::move(command));
	return listenerId;
}

void AudioSystem::UnregisterListener(AudioListenerId listenerId)
{
	if (!m_pImpl || listenerId == InvalidAudioListenerId)
	{
		return;
	}
	m_pImpl->m_stateLock.Lock();
	const bool bRemoved = m_pImpl->m_listenerStates.Remove(listenerId);
	m_pImpl->m_stateLock.Unlock();
	if (bRemoved)
	{
		Impl::Command command{};
		command.m_type = Impl::ECommandType::UnregisterListener;
		command.m_listenerId = listenerId;
		m_pImpl->Enqueue(std::move(command));
	}
}

bool AudioSystem::UpdateListener(AudioListenerId listenerId, const AudioListenerState& state)
{
	if (!m_pImpl)
	{
		return false;
	}
	m_pImpl->m_stateLock.Lock();
	auto it = m_pImpl->m_listenerStates.Find(listenerId);
	if (it != m_pImpl->m_listenerStates.end())
	{
		it.Value() = state;
	}
	const bool bExists = it != m_pImpl->m_listenerStates.end();
	m_pImpl->m_stateLock.Unlock();
	if (!bExists)
	{
		return false;
	}

	Impl::Command command{};
	command.m_type = Impl::ECommandType::UpdateListener;
	command.m_listenerId = listenerId;
	command.m_listener = state;
	m_pImpl->Enqueue(std::move(command));
	return true;
}

AudioListenerId AudioSystem::GetActiveListener() const
{
	if (!m_pImpl)
	{
		return InvalidAudioListenerId;
	}
	m_pImpl->m_stateLock.Lock();
	const AudioListenerId listenerId = m_pImpl->m_activeListener;
	m_pImpl->m_stateLock.Unlock();
	return listenerId;
}

bool AudioSystem::GetActiveListenerState(AudioListenerState& outState) const
{
	if (!m_pImpl)
	{
		return false;
	}
	m_pImpl->m_stateLock.Lock();
	const auto it = m_pImpl->m_listenerStates.Find(m_pImpl->m_activeListener);
	const bool bExists = it != m_pImpl->m_listenerStates.end();
	if (bExists)
	{
		outState = it.Value();
	}
	m_pImpl->m_stateLock.Unlock();
	return bExists;
}

void AudioSystem::Update()
{
	if (!IsInitialized())
	{
		return;
	}
	Impl::Command command{};
	command.m_type = Impl::ECommandType::Refresh;
	m_pImpl->Enqueue(std::move(command));
}

void AudioSystem::Flush()
{
	if (Tasks::Scheduler* scheduler = App::GetSubmodule<Tasks::Scheduler>())
	{
		if (!scheduler->IsAudioThread())
		{
			scheduler->WaitIdle(EThreadType::Audio);
		}
	}
}
