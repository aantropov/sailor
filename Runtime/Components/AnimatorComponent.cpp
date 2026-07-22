#include "Components/AnimatorComponent.h"
#include "Engine/GameObject.h"

using namespace Sailor;

void AnimatorComponent::Initialize()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<AnimationECS>();
	m_handle = ecs->RegisterComponent();
	GetData().SetOwner(GetOwner());
}

void AnimatorComponent::EndPlay()
{
	GetOwner()->GetWorld()->GetECS<AnimationECS>()->UnregisterComponent(m_handle);
	m_handle = ECS::InvalidIndex;
}

uint32_t AnimatorComponent::GetSkeletonOffset() const
{
	return GetData().m_gpuOffset;
}

AnimatorComponentData& AnimatorComponent::GetData()
{
	auto ecs = GetOwner()->GetWorld()->GetECS<AnimationECS>();
	return ecs->GetComponentData(m_handle);
}

SAILOR_API const AnimatorComponentData& Sailor::AnimatorComponent::GetData() const
{
	auto ecs = GetOwner()->GetWorld()->GetECS<AnimationECS>();
	return ecs->GetComponentData(m_handle);
}

void AnimatorComponent::SetAnimation(const AnimationPtr& animation)
{
	GetOwner()->GetWorld()->GetECS<AnimationECS>()->SetAnimation(m_handle, animation);
}

void AnimatorComponent::Play()
{
	GetData().m_bIsPlaying = true;
}

void AnimatorComponent::Stop()
{
	GetData().m_bIsPlaying = false;
}

void AnimatorComponent::SetPlaySpeed(float speed)
{
	GetData().m_playSpeed = speed;
}

void AnimatorComponent::SetPlayMode(EAnimationPlayMode mode)
{
	GetData().m_playMode = mode;
}
