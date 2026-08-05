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

void AnimatorComponent::SetController(const AnimationControllerPtr& controller)
{
	GetOwner()->GetWorld()->GetECS<AnimationECS>()->SetController(m_handle, controller);
}

void AnimatorComponent::SetAnimationSet(const AnimationSetPtr& animationSet)
{
	GetOwner()->GetWorld()->GetECS<AnimationECS>()->SetAnimationSet(m_handle, animationSet);
}

bool AnimatorComponent::SetFloat(const std::string& name, float value)
{
	return GetData().GetControllerInstance().SetFloat(name, value);
}

bool AnimatorComponent::SetInt(const std::string& name, int32_t value)
{
	return GetData().GetControllerInstance().SetInt(name, value);
}

bool AnimatorComponent::SetBool(const std::string& name, bool value)
{
	return GetData().GetControllerInstance().SetBool(name, value);
}

bool AnimatorComponent::SetTrigger(const std::string& name)
{
	return GetData().GetControllerInstance().SetTrigger(name);
}

bool AnimatorComponent::ResetTrigger(const std::string& name)
{
	return GetData().GetControllerInstance().ResetTrigger(name);
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
