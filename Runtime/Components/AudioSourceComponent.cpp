#include "Components/AudioSourceComponent.h"

#include "Audio/AudioSystem.h"
#include "Engine/GameObject.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;

void AudioSourceComponent::Initialize()
{
	AudioECS* ecs = GetWorld()->GetECS<AudioECS>();
	m_handle = ecs->RegisterComponent();
	AudioSourceData& data = GetData();
	data.m_component = this;
	data.SetOwner(GetOwner());
	data.MarkDirty();
}

void AudioSourceComponent::BeginPlay()
{
	if (m_bAutoPlay)
	{
		Play();
	}
}

void AudioSourceComponent::EndPlay()
{
	if (m_handle != ECS::InvalidIndex)
	{
		GetWorld()->GetECS<AudioECS>()->UnregisterComponent(m_handle);
		m_handle = ECS::InvalidIndex;
	}
}

AudioSourceData& AudioSourceComponent::GetData()
{
	return GetWorld()->GetECS<AudioECS>()->GetComponentData(m_handle);
}

const AudioSourceData& AudioSourceComponent::GetData() const
{
	return GetWorld()->GetECS<AudioECS>()->GetComponentData(m_handle);
}

void AudioSourceComponent::MarkSettingsDirty()
{
	GetData().MarkDirty();
}

const AudioClipPtr& AudioSourceComponent::GetClip() const
{
	return GetData().m_clip;
}

void AudioSourceComponent::SetClip(const AudioClipPtr& clip)
{
	AudioSourceData& data = GetData();
	if (data.m_clip != clip)
	{
		data.m_clip = clip;
		data.m_attemptedClip.Clear();
		data.m_attemptedClipRevision = 0;
		data.MarkDirty();
		if (data.m_bPlaybackRequested)
		{
			++data.m_playRequest;
		}
	}
}

bool AudioSourceComponent::GetLoop() const
{
	return GetData().m_settings.m_bLoop;
}

void AudioSourceComponent::SetLoop(bool value)
{
	AudioSourceData& data = GetData();
	if (data.m_settings.m_bLoop != value)
	{
		data.m_settings.m_bLoop = value;
		MarkSettingsDirty();
	}
}

bool AudioSourceComponent::GetSpatial() const
{
	return GetData().m_settings.m_bSpatial;
}

void AudioSourceComponent::SetSpatial(bool value)
{
	AudioSourceData& data = GetData();
	if (data.m_settings.m_bSpatial != value)
	{
		data.m_settings.m_bSpatial = value;
		MarkSettingsDirty();
	}
}

float AudioSourceComponent::GetVolume() const
{
	return GetData().m_settings.m_volume;
}

void AudioSourceComponent::SetVolume(float value)
{
	value = std::isfinite(value) ? (std::max)(0.0f, value) : 1.0f;
	AudioSourceData& data = GetData();
	if (data.m_settings.m_volume != value)
	{
		data.m_settings.m_volume = value;
		MarkSettingsDirty();
	}
}

float AudioSourceComponent::GetPitch() const
{
	return GetData().m_settings.m_pitch;
}

void AudioSourceComponent::SetPitch(float value)
{
	value = std::isfinite(value) ? (std::max)(0.01f, value) : 1.0f;
	AudioSourceData& data = GetData();
	if (data.m_settings.m_pitch != value)
	{
		data.m_settings.m_pitch = value;
		MarkSettingsDirty();
	}
}

float AudioSourceComponent::GetMinDistance() const
{
	return GetData().m_settings.m_minDistance;
}

void AudioSourceComponent::SetMinDistance(float value)
{
	value = std::isfinite(value) ? (std::max)(0.01f, value) : 1.0f;
	AudioSourceData& data = GetData();
	if (data.m_settings.m_minDistance != value)
	{
		data.m_settings.m_minDistance = value;
		data.m_settings.m_maxDistance = (std::max)(data.m_settings.m_maxDistance, value);
		MarkSettingsDirty();
	}
}

float AudioSourceComponent::GetMaxDistance() const
{
	return GetData().m_settings.m_maxDistance;
}

void AudioSourceComponent::SetMaxDistance(float value)
{
	value = std::isfinite(value) ? (std::max)(0.01f, value) : 1000.0f;
	AudioSourceData& data = GetData();
	value = (std::max)(value, data.m_settings.m_minDistance);
	if (data.m_settings.m_maxDistance != value)
	{
		data.m_settings.m_maxDistance = value;
		MarkSettingsDirty();
	}
}

void AudioSourceComponent::Play()
{
	AudioSourceData& data = GetData();
	data.m_bPlaybackRequested = true;
	++data.m_playRequest;
}

void AudioSourceComponent::Stop()
{
	AudioSourceData& data = GetData();
	data.m_bPlaybackRequested = false;
}

bool AudioSourceComponent::IsPlaying() const
{
	const AudioSourceData& data = GetData();
	const AudioSystem* audioSystem = App::GetSubmodule<AudioSystem>();
	return audioSystem && audioSystem->IsVoicePlaying(data.m_voiceId);
}
