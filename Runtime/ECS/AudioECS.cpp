#include "ECS/AudioECS.h"

#include "AssetRegistry/Audio/AudioImporter.h"
#include "Audio/AudioSystem.h"
#include "Components/AudioListenerComponent.h"
#include "Components/AudioSourceComponent.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Math/Math.h"

#include <algorithm>
#include <cmath>
#include <new>

using namespace Sailor;

namespace
{
	constexpr float VelocityEpsilon = 0.000001f;

	glm::vec3 NormalizeOr(const glm::vec3& value, const glm::vec3& fallback)
	{
		const float lengthSquared = glm::dot(value, value);
		return std::isfinite(lengthSquared) && lengthSquared > VelocityEpsilon
			? value / std::sqrt(lengthSquared)
			: fallback;
	}

	AudioTransformState MakeTransformState(
		const TransformComponent& transform,
		const glm::vec3& velocity)
	{
		AudioTransformState state{};
		state.m_position = glm::vec3(transform.GetWorldPosition());
		state.m_forward = NormalizeOr(
			transform.GetForwardVector(),
			glm::vec3(0.0f, 0.0f, -1.0f));
		state.m_up = NormalizeOr(
			glm::vec3(transform.GetCachedWorldMatrix() * Math::vec4_Up),
			glm::vec3(0.0f, 1.0f, 0.0f));
		state.m_velocity = velocity;
		return state;
	}

	glm::vec3 CalculateVelocity(
		const glm::vec3& position,
		const glm::vec3& previousPosition,
		bool bHasPreviousPosition,
		float deltaTime)
	{
		if (!bHasPreviousPosition || !std::isfinite(deltaTime) || deltaTime <= VelocityEpsilon)
		{
			return {};
		}

		const glm::vec3 velocity = (position - previousPosition) / deltaTime;
		return Math::AllFinite(glm::vec4(velocity, 0.0f)) ? velocity : glm::vec3{};
	}
}

AudioSystem* AudioECS::GetAudioSystem() const
{
	return App::GetSubmodule<AudioSystem>();
}

void AudioECS::DestroyVoice(AudioSourceData& data)
{
	if (data.m_voiceId != InvalidAudioVoiceId)
	{
		if (AudioSystem* audioSystem = GetAudioSystem())
		{
			audioSystem->DestroyVoice(data.m_voiceId);
		}
	}

	data.m_voiceId = InvalidAudioVoiceId;
	data.m_voiceClip.Clear();
}

void AudioECS::OnComponentUnregistered(size_t, AudioSourceData& component)
{
	DestroyVoice(component);
}

size_t AudioECS::RegisterListener()
{
	size_t index = ECS::InvalidIndex;
	if (m_freeListeners.IsEmpty())
	{
		m_listeners.AddDefault(1);
		index = m_listeners.Num() - 1;
	}
	else
	{
		index = *m_freeListeners.Last();
		m_freeListeners.PopBack();
	}

	AudioListenerData& data = m_listeners[index];
	data.m_bIsActive = true;
	data.m_bIsDirty = true;
	return index;
}

void AudioECS::UnregisterListener(size_t index)
{
	if (index == ECS::InvalidIndex ||
		index >= m_listeners.Num() ||
		!m_listeners[index].m_bIsActive)
	{
		return;
	}

	AudioListenerData& data = m_listeners[index];
	if (data.m_listenerId != InvalidAudioListenerId)
	{
		if (AudioSystem* audioSystem = GetAudioSystem())
		{
			audioSystem->UnregisterListener(data.m_listenerId);
		}
	}

	data.~AudioListenerData();
	new (&data) AudioListenerData();
	m_freeListeners.PushBack(index);
}

AudioListenerData& AudioECS::GetListenerData(size_t index)
{
	check(index < m_listeners.Num() && m_listeners[index].m_bIsActive);
	return m_listeners[index];
}

const AudioListenerData& AudioECS::GetListenerData(size_t index) const
{
	check(index < m_listeners.Num() && m_listeners[index].m_bIsActive);
	return m_listeners[index];
}

void AudioECS::TickSource(
	AudioSystem& audioSystem,
	AudioSourceData& data,
	float deltaTime)
{
	GameObjectPtr owner = data.m_owner.StaticCast<GameObject>();
	AudioSourceComponent* component = data.m_component;
	if (!component || !component->IsValid())
	{
		DestroyVoice(data);
		return;
	}

	const AudioClipPtr clip = data.m_clip;
	const uint64_t clipRevision = clip && clip->IsReady()
		? clip->GetRevision()
		: 0;
	const bool bVoiceOutdated =
		data.m_voiceId != InvalidAudioVoiceId &&
		(data.m_voiceClip != clip ||
		 audioSystem.GetVoiceClipRevision(data.m_voiceId) != clipRevision);
	if (bVoiceOutdated || !clip || !clip->IsReady())
	{
		DestroyVoice(data);
	}

	if (data.m_voiceId == InvalidAudioVoiceId &&
		clip && clip->IsReady() &&
		(data.m_attemptedClip != clip ||
		 data.m_attemptedClipRevision != clipRevision ||
		 data.IsDirty()))
	{
		data.m_attemptedClip = clip;
		data.m_attemptedClipRevision = clipRevision;
		if (audioSystem.CreateVoice(clip, data.m_voiceId))
		{
			data.m_voiceClip = clip;
			data.m_lastTransformFrame = 0;
			data.m_bHasLastPosition = false;
			data.m_appliedPlayRequest = 0;
		}
	}

	if (data.m_voiceId == InvalidAudioVoiceId)
	{
		data.m_bIsDirty = false;
		return;
	}

	if (data.IsDirty())
	{
		audioSystem.SetVoiceSettings(data.m_voiceId, data.m_settings);
		data.m_bIsDirty = false;
	}

	const TransformComponent& transform = owner->GetTransformComponent();
	const glm::vec3 position = glm::vec3(transform.GetWorldPosition());
	const bool bTransformChanged =
		!data.m_bHasLastPosition ||
		transform.GetFrameLastChange() > data.m_lastTransformFrame;
	if (bTransformChanged || glm::dot(data.m_lastVelocity, data.m_lastVelocity) > VelocityEpsilon)
	{
		const glm::vec3 velocity = bTransformChanged
			? CalculateVelocity(position, data.m_lastPosition, data.m_bHasLastPosition, deltaTime)
			: glm::vec3{};
		audioSystem.SetVoiceTransform(
			data.m_voiceId,
			MakeTransformState(transform, velocity));
		data.m_lastPosition = position;
		data.m_lastVelocity = velocity;
		data.m_lastTransformFrame = transform.GetFrameLastChange();
		data.m_bHasLastPosition = true;
	}

	if (!data.m_bPlaybackRequested)
	{
		if (audioSystem.IsVoicePlaying(data.m_voiceId))
		{
			audioSystem.StopVoice(data.m_voiceId);
		}
		data.m_appliedPlayRequest = data.m_playRequest;
	}
	else if (data.m_appliedPlayRequest != data.m_playRequest)
	{
		if (audioSystem.PlayVoice(data.m_voiceId, true))
		{
			data.m_appliedPlayRequest = data.m_playRequest;
		}
	}
}

void AudioECS::TickListener(
	AudioSystem& audioSystem,
	AudioListenerData& data,
	float deltaTime)
{
	GameObjectPtr owner = data.m_owner.StaticCast<GameObject>();
	AudioListenerComponent* component = data.m_component;
	if (!component || !component->IsValid())
	{
		if (data.m_listenerId != InvalidAudioListenerId)
		{
			audioSystem.UnregisterListener(data.m_listenerId);
			data.m_listenerId = InvalidAudioListenerId;
		}
		return;
	}

	const TransformComponent& transform = owner->GetTransformComponent();
	const glm::vec3 position = glm::vec3(transform.GetWorldPosition());
	const bool bTransformChanged =
		!data.m_bHasLastPosition ||
		transform.GetFrameLastChange() > data.m_lastTransformFrame;
	if (!data.m_bIsDirty &&
		!bTransformChanged &&
		glm::dot(data.m_lastVelocity, data.m_lastVelocity) <= VelocityEpsilon)
	{
		return;
	}

	const glm::vec3 velocity = bTransformChanged
		? CalculateVelocity(position, data.m_lastPosition, data.m_bHasLastPosition, deltaTime)
		: glm::vec3{};
	AudioListenerState state{};
	state.m_transform = MakeTransformState(transform, velocity);
	state.m_priority = data.m_priority;
	state.m_bEnabled = data.m_bEnabled;
	if (data.m_listenerId == InvalidAudioListenerId)
	{
		data.m_listenerId = audioSystem.RegisterListener(state);
	}
	else
	{
		audioSystem.UpdateListener(data.m_listenerId, state);
	}

	data.m_lastPosition = position;
	data.m_lastVelocity = velocity;
	data.m_lastTransformFrame = transform.GetFrameLastChange();
	data.m_bHasLastPosition = true;
	data.m_bIsDirty = false;
}

Tasks::ITaskPtr AudioECS::Tick(float deltaTime)
{
	AudioSystem* audioSystem = GetAudioSystem();
	if (!audioSystem || !audioSystem->IsInitialized())
	{
		return nullptr;
	}

	for (AudioSourceData& data : m_components)
	{
		if (data.m_bIsActive)
		{
			TickSource(*audioSystem, data, deltaTime);
		}
	}

	for (AudioListenerData& data : m_listeners)
	{
		if (data.m_bIsActive)
		{
			TickListener(*audioSystem, data, deltaTime);
		}
	}

	audioSystem->Update();

	return nullptr;
}

void AudioECS::EndPlay()
{
	if (AudioSystem* audioSystem = GetAudioSystem())
	{
		for (AudioSourceData& data : m_components)
		{
			if (data.m_bIsActive && data.m_voiceId != InvalidAudioVoiceId)
			{
				audioSystem->DestroyVoice(data.m_voiceId);
				data.m_voiceId = InvalidAudioVoiceId;
			}
		}

		for (AudioListenerData& data : m_listeners)
		{
			if (data.m_bIsActive && data.m_listenerId != InvalidAudioListenerId)
			{
				audioSystem->UnregisterListener(data.m_listenerId);
				data.m_listenerId = InvalidAudioListenerId;
			}
		}
	}

	m_listeners.Clear();
	m_freeListeners.Clear();
	ECS::TSystem<AudioECS, AudioSourceData>::EndPlay();
}
