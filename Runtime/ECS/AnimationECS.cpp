#include "ECS/AnimationECS.h"
#include "Engine/GameObject.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "AssetRegistry/Animation/AnimationImporter.h"
#include "AssetRegistry/Animation/AnimationPose.h"
#include "Components/MeshRendererComponent.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "Math/Transform.h"

using namespace Sailor;
using namespace Sailor::Tasks;

namespace
{
	void MarkMeshSkeletonDirty(GameObjectPtr owner)
	{
		if (!owner)
		{
			return;
		}

		auto* meshEcs = owner->GetWorld()->GetECS<StaticMeshRendererECS>();
		if (!meshEcs)
		{
			return;
		}

		for (auto component : owner->GetComponents())
		{
			if (auto mesh = component.DynamicCast<MeshRendererComponent>())
			{
				if (meshEcs->IsComponentRegistered(mesh->GetComponentIndex()))
				{
					mesh->GetData().MarkDirty();
				}
			}
		}
	}
}

void AnimationECS::BeginPlay()
{
	auto& driver = RHI::Renderer::GetDriver();
	m_bonesBinding = driver->CreateShaderBindings();
	m_bonesBuffer = driver->AddSsboToShaderBindings(m_bonesBinding, "bones", sizeof(glm::mat4), BonesMaxNum, 0);
}

void AnimationECS::EndPlay()
{
	m_bonesBinding.Clear();
	m_bonesBuffer.Clear();
	m_nextBoneOffset = 0;
}

bool AnimationECS::TryAllocateBoneRange(uint32_t numBones, uint32_t& nextBoneOffset, uint32_t& outGpuOffset)
{
	outGpuOffset = AnimatorComponentData::InvalidGpuOffset;
	if (numBones == 0 || nextBoneOffset > BonesMaxNum || numBones > BonesMaxNum - nextBoneOffset)
	{
		return false;
	}

	outGpuOffset = nextBoneOffset;
	nextBoneOffset += numBones;
	return true;
}

void AnimationECS::InvalidateGpuLayout()
{
	m_nextBoneOffset = 0;

	for (auto& data : m_components)
	{
		data.m_gpuOffset = AnimatorComponentData::InvalidGpuOffset;
		MarkMeshSkeletonDirty(data.m_owner.StaticCast<GameObject>());
	}
}

void AnimationECS::SetAnimation(size_t componentIndex, const TObjectPtr<Animation>& animation)
{
	if (!IsComponentRegistered(componentIndex))
	{
		return;
	}

	auto& data = GetComponentData(componentIndex);
	const uint32_t previousBonesCount = data.GetBonesCount();
	data.GetAnimation() = animation;
	data.m_animationRevision = animation ? animation->m_revision : 0;
	const bool bControllerActive = data.m_controllerInstance.IsValid() &&
		data.m_animationSet && !data.m_controllerAnimations.IsEmpty();
	if (!bControllerActive)
	{
		data.SetBonesCount(animation ? animation->m_numBones : 0);
		data.m_skeletonSignature = animation ? animation->m_skeletonSignature : 0;
	}
	data.m_currentFrame = 0.0f;
	data.m_frameIndex = 0;
	data.m_lerp = 0.0f;
	data.MarkDirty();

	if (previousBonesCount != data.GetBonesCount())
	{
		InvalidateGpuLayout();
	}
	else
	{
		MarkMeshSkeletonDirty(data.m_owner.StaticCast<GameObject>());
	}
}

void AnimationECS::SetController(
	size_t componentIndex,
	const AnimationControllerPtr& controller)
{
	if (!IsComponentRegistered(componentIndex))
	{
		return;
	}
	GetComponentData(componentIndex).m_controller = controller;
	RefreshController(componentIndex, true);
}

void AnimationECS::SetAnimationSet(
	size_t componentIndex,
	const AnimationSetPtr& animationSet)
{
	if (!IsComponentRegistered(componentIndex))
	{
		return;
	}
	GetComponentData(componentIndex).m_animationSet = animationSet;
	RefreshController(componentIndex, false);
}

void AnimationECS::RefreshController(size_t componentIndex, bool bResetInstance)
{
	if (!IsComponentRegistered(componentIndex))
	{
		return;
	}

	auto& data = GetComponentData(componentIndex);
	const uint32_t previousBonesCount = data.GetBonesCount();
	if (bResetInstance)
	{
		data.m_controllerInstance.SetController(data.m_controller);
	}
	else if (data.m_controllerInstance.GetController() != data.m_controller)
	{
		data.m_controllerInstance.SetController(data.m_controller);
	}
	else if (data.m_controller)
	{
		data.m_controllerInstance.Tick(0.0f, 0.0f);
	}

	data.m_controllerRevision = data.m_controller ? data.m_controller->GetRevision() : 0;
	data.m_animationSetRevision = data.m_animationSet ? data.m_animationSet->GetRevision() : 0;
	data.m_animationRevision = data.m_animation ? data.m_animation->m_revision : 0;
	data.m_controllerAnimations.Clear();
	data.m_controllerAnimationRevisions.Clear();

	uint32_t controllerBonesCount = 0;
	uint64_t controllerSkeletonSignature = 0;
	if (data.m_controllerInstance.IsValid() && data.m_animationSet)
	{
		const auto& states = data.m_controller->GetStates();
		data.m_controllerAnimations.Resize(states.Num());
		data.m_controllerAnimationRevisions.Resize(states.Num());
		auto* animationImporter = App::GetSubmodule<AnimationImporter>();
		for (size_t stateIndex = 0; stateIndex < states.Num(); ++stateIndex)
		{
			const FileId* animationId = data.m_animationSet->FindAnimation(
				states[stateIndex].m_clipSlot);
			AnimationPtr animation;
			if (animationImporter && animationId && *animationId)
			{
				animationImporter->LoadAnimation_Immediate(*animationId, animation);
			}
			if (animation && animation->m_numBones > 0 &&
				animation->m_parentBoneIndices.Num() == animation->m_numBones &&
				animation->m_restPose.Num() == animation->m_numBones)
			{
				if (controllerBonesCount == 0)
				{
					controllerBonesCount = animation->m_numBones;
					controllerSkeletonSignature = animation->m_skeletonSignature;
				}
				else if (animation->m_numBones != controllerBonesCount ||
					animation->m_skeletonSignature != controllerSkeletonSignature)
				{
					SAILOR_LOG_ERROR(
						"Animation controller '%s' state '%s' uses an incompatible skeleton.",
						data.m_controller->GetFileId().ToString().c_str(),
						states[stateIndex].m_name.c_str());
					animation.Clear();
				}
			}
			data.m_controllerAnimations[stateIndex] = animation;
			data.m_controllerAnimationRevisions[stateIndex] = animation ? animation->m_revision : 0;
		}
	}

	data.SetBonesCount(controllerBonesCount > 0 ?
		controllerBonesCount : (data.m_animation ? data.m_animation->m_numBones : 0));
	data.m_skeletonSignature = controllerBonesCount > 0 ?
		controllerSkeletonSignature : (data.m_animation ? data.m_animation->m_skeletonSignature : 0);
	data.m_currentFrame = 0.0f;
	data.m_frameIndex = 0;
	data.m_lerp = 0.0f;
	data.MarkDirty();
	if (previousBonesCount != data.GetBonesCount())
	{
		InvalidateGpuLayout();
	}
	else
	{
		MarkMeshSkeletonDirty(data.m_owner.StaticCast<GameObject>());
	}
}

void AnimationECS::OnComponentUnregistered(size_t, AnimatorComponentData&)
{
	InvalidateGpuLayout();
}

Tasks::ITaskPtr AnimationECS::Tick(float deltaTime)
{
	for (size_t componentIndex = 0; componentIndex < m_components.Num(); ++componentIndex)
	{
		auto& data = m_components[componentIndex];
		bool bAnimationChanged = data.m_animation &&
			data.m_animationRevision != data.m_animation->m_revision;
		if (!bAnimationChanged &&
			data.m_controllerAnimationRevisions.Num() == data.m_controllerAnimations.Num())
		{
			for (size_t stateIndex = 0; stateIndex < data.m_controllerAnimations.Num(); ++stateIndex)
			{
				const auto& animation = data.m_controllerAnimations[stateIndex];
				if (animation && data.m_controllerAnimationRevisions[stateIndex] != animation->m_revision)
				{
					bAnimationChanged = true;
					break;
				}
			}
		}
		if ((data.m_controller && data.m_controllerRevision != data.m_controller->GetRevision()) ||
			(data.m_animationSet && data.m_animationSetRevision != data.m_animationSet->GetRevision()) ||
			bAnimationChanged)
		{
			RefreshController(componentIndex, false);
		}
	}

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto commands = renderer->GetDriverCommands();
	auto cmdList = GetWorld()->GetCommandList();
	commands->BeginDebugRegion(cmdList, "AnimationECS:Update", RHI::DebugContext::Color_CmdTransfer);

	for (const auto& data : m_components)
	{
		if (!data.m_bIsActive || data.m_gpuOffset == AnimatorComponentData::InvalidGpuOffset)
		{
			continue;
		}

		const uint32_t bonesCount = data.GetBonesCount();
		const bool bHasValidBoneRange = bonesCount > 0 &&
			data.m_gpuOffset <= BonesMaxNum && bonesCount <= BonesMaxNum - data.m_gpuOffset;
		if (!bHasValidBoneRange)
		{
			InvalidateGpuLayout();
			break;
		}
	}

	for (auto& data : m_components)
	{
		if (!data.m_bIsActive)
		{
			continue;
		}

		GameObjectPtr owner = data.m_owner.StaticCast<GameObject>();
		if (!owner || data.GetBonesCount() == 0)
		{
			continue;
		}

		bool bSampled = false;
		AnimationPtr skeletonAnimation;
		const bool bUseController = data.m_controllerInstance.IsValid() &&
			data.m_animationSet &&
			data.m_controllerAnimations.Num() == data.m_controller->GetStates().Num();
		if (bUseController)
		{
			uint32_t activeStateIndex = data.m_controllerInstance.GetActiveStateIndex();
			AnimationPtr activeAnimation = activeStateIndex < data.m_controllerAnimations.Num() ?
				data.m_controllerAnimations[activeStateIndex] : AnimationPtr{};
			const float activeDuration = activeAnimation ? activeAnimation->m_duration : 0.0f;
			const float controllerDelta = data.m_bIsPlaying ?
				deltaTime * (std::max)(data.m_playSpeed, 0.0f) : 0.0f;
			data.m_controllerInstance.Tick(controllerDelta, activeDuration);

			activeStateIndex = data.m_controllerInstance.GetActiveStateIndex();
			activeAnimation = activeStateIndex < data.m_controllerAnimations.Num() ?
				data.m_controllerAnimations[activeStateIndex] : AnimationPtr{};
			if (activeAnimation && activeAnimation->m_numBones == data.GetBonesCount())
			{
				skeletonAnimation = activeAnimation;
				bSampled = AnimationPose::Sample(
					activeAnimation,
					data.m_controllerInstance.GetActiveStateTime(),
					data.m_controller->GetStates()[activeStateIndex].m_bLoop,
					data.m_currentSkeleton,
					data.m_frameIndex,
					data.m_lerp);
			}

			if (bSampled && data.m_controllerInstance.IsTransitioning())
			{
				const uint32_t destinationStateIndex =
					data.m_controllerInstance.GetDestinationStateIndex();
				AnimationPtr destinationAnimation = destinationStateIndex < data.m_controllerAnimations.Num() ?
					data.m_controllerAnimations[destinationStateIndex] : AnimationPtr{};
				uint32_t blendFrameIndex = 0;
				float blendLerp = 0.0f;
				if (destinationAnimation &&
					destinationAnimation->m_numBones == data.GetBonesCount() &&
					AnimationPose::Sample(
						destinationAnimation,
						data.m_controllerInstance.GetDestinationStateTime(),
						data.m_controller->GetStates()[destinationStateIndex].m_bLoop,
						data.m_blendSkeleton,
						blendFrameIndex,
						blendLerp))
				{
					const float alpha = data.m_controllerInstance.GetTransitionAlpha();
					AnimationPose::BlendLocalPoses(
						data.m_currentSkeleton,
						data.m_blendSkeleton,
						alpha,
						data.m_currentSkeleton);
				}
			}
		}
		else if (data.GetAnimation() && data.GetAnimation()->m_numBones == data.GetBonesCount())
		{
			const auto& anim = *data.GetAnimation();
			skeletonAnimation = data.GetAnimation();
			const float lastFrame = static_cast<float>(anim.m_numFrames - 1);

			if (data.m_bIsPlaying)
			{
				float frameAdvance = deltaTime * anim.m_fps * data.m_playSpeed * (data.m_bForward ? 1.0f : -1.0f);
				data.m_currentFrame += frameAdvance;

				if (data.m_playMode == EAnimationPlayMode::Repeat)
				{
					if (lastFrame > 0.0f)
					{
						data.m_currentFrame = std::fmod(data.m_currentFrame, lastFrame);
						if (data.m_currentFrame < 0.0f)
						{
							data.m_currentFrame += lastFrame;
						}
					}
					else
					{
						data.m_currentFrame = 0.0f;
					}
				}
				else if (data.m_playMode == EAnimationPlayMode::Once)
				{
					if (data.m_currentFrame >= lastFrame)
					{
						data.m_currentFrame = lastFrame;
						data.m_bIsPlaying = false;
					}
					else if (data.m_currentFrame <= 0.0f && frameAdvance < 0.0f)
					{
						data.m_currentFrame = 0.0f;
						data.m_bIsPlaying = false;
					}
				}
				else if (data.m_playMode == EAnimationPlayMode::PingPong)
				{
					if (data.m_bForward && data.m_currentFrame >= lastFrame)
					{
						data.m_currentFrame = lastFrame;
						data.m_bForward = false;
					}
					else if (!data.m_bForward && data.m_currentFrame <= 0.0f)
					{
						data.m_currentFrame = 0.0f;
						data.m_bForward = true;
					}
				}
			}

			bSampled = AnimationPose::Sample(
				data.GetAnimation(),
				data.m_currentFrame / anim.m_fps,
				false,
				data.m_currentSkeleton,
				data.m_frameIndex,
				data.m_lerp);
		}

		if (!bSampled)
		{
			if (!skeletonAnimation && bUseController)
			{
				for (const auto& animation : data.m_controllerAnimations)
				{
					if (animation && animation->m_numBones == data.GetBonesCount())
					{
						skeletonAnimation = animation;
						break;
					}
				}
			}
			if (!skeletonAnimation && data.m_animation &&
				data.m_animation->m_numBones == data.GetBonesCount())
			{
				skeletonAnimation = data.m_animation;
			}
			if (data.m_currentSkeleton.Num() != data.GetBonesCount())
			{
				data.m_currentSkeleton.Resize(data.GetBonesCount());
			}
			if (skeletonAnimation &&
				skeletonAnimation->m_restPose.Num() == data.GetBonesCount())
			{
				data.m_currentSkeleton = skeletonAnimation->m_restPose;
			}
			else
			{
				for (auto& transform : data.m_currentSkeleton)
				{
					transform = Math::Transform{};
				}
			}
		}

		if (data.m_gpuOffset == AnimatorComponentData::InvalidGpuOffset)
		{
			if (!TryAllocateBoneRange(data.GetBonesCount(), m_nextBoneOffset, data.m_gpuOffset))
			{
				continue;
			}

			MarkMeshSkeletonDirty(data.m_owner.StaticCast<GameObject>());
		}

		ModelPtr model;
		if (auto mesh = owner->GetComponent<MeshRendererComponent>())
		{
			model = mesh->GetModel();
		}

		if (data.m_currentMatrices.Num() != data.m_currentSkeleton.Num())
		{
			data.m_currentMatrices.Resize(data.m_currentSkeleton.Num());
			data.m_globalMatrices.Resize(data.m_currentSkeleton.Num());
			data.m_composeState.Resize(data.m_currentSkeleton.Num());
		}
		TVector<int32_t> rootBoneIndices;
		const TVector<int32_t>* parentBoneIndices = skeletonAnimation ?
			&skeletonAnimation->m_parentBoneIndices : nullptr;
		if (!parentBoneIndices || parentBoneIndices->Num() != data.m_currentSkeleton.Num())
		{
			parentBoneIndices = &rootBoneIndices;
		}
		AnimationPose::ComposeLocalPose(
			data.m_currentSkeleton,
			*parentBoneIndices,
			data.m_globalMatrices,
			data.m_composeState);
		for (uint32_t i = 0; i < data.m_currentSkeleton.Num(); ++i)
		{
			const glm::mat4 bind = model && model->GetInverseBind().Num() > i ? model->GetInverseBind()[i] : glm::mat4(1.0f);
			data.m_currentMatrices[i] = data.m_globalMatrices[i] * bind;
		}

		commands->UpdateShaderBinding(cmdList, m_bonesBuffer,
			data.m_currentMatrices.GetData(),
			sizeof(glm::mat4) * data.m_currentMatrices.Num(),
			m_bonesBuffer->GetBufferOffset() + sizeof(glm::mat4) * data.m_gpuOffset);
	}

	commands->EndDebugRegion(cmdList);
	return nullptr;
}

void AnimationECS::FillAnimationData(RHI::RHISceneViewPtr& sceneView)
{
	sceneView->m_boneMatrices = m_bonesBinding;
}
