#include "Components/AudioListenerComponent.h"

#include "Engine/GameObject.h"

using namespace Sailor;

void AudioListenerComponent::Initialize()
{
	AudioECS* ecs = GetWorld()->GetECS<AudioECS>();
	m_handle = ecs->RegisterListener();
	AudioListenerData& data = GetData();
	data.m_component = this;
	data.m_owner = GetOwner();
	data.m_bIsDirty = true;
}

void AudioListenerComponent::BeginPlay()
{
	GetData().m_bIsDirty = true;
}

void AudioListenerComponent::EndPlay()
{
	if (m_handle != ECS::InvalidIndex)
	{
		GetWorld()->GetECS<AudioECS>()->UnregisterListener(m_handle);
		m_handle = ECS::InvalidIndex;
	}
}

AudioListenerData& AudioListenerComponent::GetData()
{
	return GetWorld()->GetECS<AudioECS>()->GetListenerData(m_handle);
}

const AudioListenerData& AudioListenerComponent::GetData() const
{
	return GetWorld()->GetECS<AudioECS>()->GetListenerData(m_handle);
}

bool AudioListenerComponent::GetEnabled() const
{
	return GetData().m_bEnabled;
}

void AudioListenerComponent::SetEnabled(bool value)
{
	AudioListenerData& data = GetData();
	if (data.m_bEnabled != value)
	{
		data.m_bEnabled = value;
		data.m_bIsDirty = true;
	}
}

int32_t AudioListenerComponent::GetPriority() const
{
	return GetData().m_priority;
}

void AudioListenerComponent::SetPriority(int32_t value)
{
	AudioListenerData& data = GetData();
	if (data.m_priority != value)
	{
		data.m_priority = value;
		data.m_bIsDirty = true;
	}
}
