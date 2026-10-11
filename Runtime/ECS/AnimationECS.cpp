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
	TSharedPtr<const TVector<glm::mat4>> AcquireBoneSnapshot(
		TVector<TSharedPtr<TVector<glm::mat4>>>& pool,
		const TVector<glm::mat4>& source)
	{
		for (auto& candidate : pool)
		{
			if (candidate && !candidate.IsShared())
			{
				*candidate = source;
				return candidate;
			}
		}

		auto snapshot = TSharedPtr<TVector<glm::mat4>>::Make();
		*snapshot = source;
		pool.Add(snapshot);
		return snapshot;
	}

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

		meshEcs->MarkDirty(owner);
	}

	void AdvanceAnimationFrame(AnimatorComponentData& data, const Animation& animation, float deltaTime)
	{
		if (!data.m_bIsPlaying || animation.m_numFrames == 0) return;
		const float lastFrame = static_cast<float>(animation.m_numFrames - 1);
		const float advance = deltaTime * animation.m_fps * data.m_playSpeed * (data.m_bForward ? 1.0f : -1.0f);
		data.m_currentFrame += advance;
		if (data.m_playMode == EAnimationPlayMode::Repeat)
		{
			data.m_currentFrame = lastFrame > 0.0f ? std::fmod(data.m_currentFrame, lastFrame) : 0.0f;
			if (data.m_currentFrame < 0.0f) data.m_currentFrame += lastFrame;
		}
		else if (data.m_playMode == EAnimationPlayMode::Once)
		{
			if (data.m_currentFrame >= lastFrame)
			{
				data.m_currentFrame = lastFrame;
				data.m_bIsPlaying = false;
			}
			else if (data.m_currentFrame <= 0.0f && advance < 0.0f)
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
}

void AnimationECS::BeginPlay()
{
	auto& driver = RHI::Renderer::GetDriver();
	m_bonesBinding = driver->CreateShaderBindings();
	m_bonesBuffer.Clear();
}

void AnimationECS::EndPlay()
{
	ECS::TSystem<AnimationECS, AnimatorComponentData>::EndPlay();
	m_bonesBinding.Clear();
	m_bonesBuffer.Clear();
	m_cpuBoneMatrices.Clear();
	m_publishedBoneMatrices.Clear();
	m_boneSnapshotPool.Clear();
	m_animationRevision = 0ull;
	m_nextBoneOffset = 0;
	m_bGpuLayoutDirty = false;
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
	m_bGpuLayoutDirty = true;
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
		data.m_animationSet && (data.m_controllerAnimations.ContainsIf(
			[](const AnimationPtr& clip) { return clip.IsValid(); }) ||
			(data.m_bIsControllerRefreshPending && !data.m_currentMatrices.IsEmpty()));
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
	auto& data = GetComponentData(componentIndex);
	if (bResetInstance || data.m_controllerInstance.GetController() != data.m_controller)
	{
		data.m_controllerInstance.SetController(data.m_controller);
	}
	data.m_controllerRevision = data.m_controller ? data.m_controller->GetRevision() : 0;
	data.m_animationSetRevision = data.m_animationSet ? data.m_animationSet->GetRevision() : 0;
	data.m_animationRevision = data.m_animation ? data.m_animation->m_revision : 0;
	data.m_pendingControllerAnimations.Clear();
	data.m_bIsControllerRefreshPending = true;

	if (data.m_controllerInstance.IsValid() && data.m_animationSet)
	{
		const auto& states = data.m_controller->GetStates();
		data.m_pendingControllerAnimations.Resize(states.Num());
		auto* importer = App::GetSubmodule<AnimationImporter>();
		for (size_t stateIndex = 0; stateIndex < states.Num(); ++stateIndex)
		{
			const FileId* animationId = data.m_animationSet->FindAnimation(
				StringHash::Runtime(states[stateIndex].m_clipSlot));
			if (!animationId || !*animationId)
			{
				SAILOR_LOG_ERROR("Animation controller '%s' state '%s' has no clip for slot '%s'.",
					data.m_controller->GetFileId().ToString().c_str(), states[stateIndex].m_name.c_str(),
					states[stateIndex].m_clipSlot.c_str());
				continue;
			}
			if (importer)
			{
				AnimationPtr animation;
				data.m_pendingControllerAnimations[stateIndex] = importer->LoadAnimation(*animationId, animation);
			}
		}
	}
}

void AnimationECS::FinishControllerRefresh(size_t componentIndex)
{
	auto& data = GetComponentData(componentIndex);
	if (!data.m_bIsControllerRefreshPending) return;
	for (const auto& task : data.m_pendingControllerAnimations)
	{
		if (task && !task->IsFinished()) return;
	}
	data.m_controllerInstance.SynchronizeController();

	const uint32_t previousBonesCount = data.GetBonesCount();
	uint32_t bonesCount = 0;
	uint64_t skeletonSignature = 0;
	data.m_controllerAnimations.Resize(data.m_pendingControllerAnimations.Num());
	data.m_controllerAnimationRevisions.Resize(data.m_pendingControllerAnimations.Num());
	for (size_t stateIndex = 0; stateIndex < data.m_pendingControllerAnimations.Num(); ++stateIndex)
	{
		const auto& task = data.m_pendingControllerAnimations[stateIndex];
		AnimationPtr animation = task ? task->GetResult() : AnimationPtr{};
		if (animation && (animation->m_numBones == 0 ||
			animation->m_parentBoneIndices.Num() != animation->m_numBones ||
			animation->m_restPose.Num() != animation->m_numBones))
		{
			SAILOR_LOG_ERROR("Animation controller '%s' state '%s' has invalid skeleton data.",
				data.m_controller->GetFileId().ToString().c_str(),
				data.m_controller->GetStates()[stateIndex].m_name.c_str());
			animation.Clear();
		}
		if (animation && bonesCount > 0 &&
			(animation->m_numBones != bonesCount || animation->m_skeletonSignature != skeletonSignature))
		{
			SAILOR_LOG_ERROR("Animation controller '%s' state '%s' uses an incompatible skeleton.",
				data.m_controller->GetFileId().ToString().c_str(),
				data.m_controller->GetStates()[stateIndex].m_name.c_str());
			animation.Clear();
		}
		if (animation && bonesCount == 0)
		{
			bonesCount = animation->m_numBones;
			skeletonSignature = animation->m_skeletonSignature;
		}
		data.m_controllerAnimations[stateIndex] = animation;
		data.m_controllerAnimationRevisions[stateIndex] = animation ? animation->m_revision : 0;
	}
	data.m_pendingControllerAnimations.Clear();
	data.m_bIsControllerRefreshPending = false;
	data.SetBonesCount(bonesCount > 0 ? bonesCount : (data.m_animation ? data.m_animation->m_numBones : 0));
	data.m_skeletonSignature = bonesCount > 0 ? skeletonSignature :
		(data.m_animation ? data.m_animation->m_skeletonSignature : 0);
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

void AnimationECS::OnComponentUnregistered(size_t, AnimatorComponentData& component)
{
	if (!GetWorld() || !GetWorld()->IsClearing())
	{
		MarkMeshSkeletonDirty(component.m_owner.StaticCast<GameObject>());
		InvalidateGpuLayout();
	}
}

void AnimationECS::Tick(float deltaTime)
{
	for (size_t componentIndex = 0; componentIndex < m_components.Num(); ++componentIndex)
	{
		auto& data = m_components[componentIndex];
		if (!data.m_bIsActive) continue;
		bool bAnimationChanged = data.m_animation &&
			data.m_animationRevision != data.m_animation->m_revision;
		if (!bAnimationChanged && !data.m_bIsControllerRefreshPending &&
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
		FinishControllerRefresh(componentIndex);
	}

	bool bBoneDataChanged = false;

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

	if (m_bGpuLayoutDirty)
	{
		// Keep published offsets and matrices together until this tick replaces both.
		m_nextBoneOffset = 0;
		for (auto& data : m_components)
		{
			if (data.m_bIsActive)
			{
				data.m_gpuOffset = AnimatorComponentData::InvalidGpuOffset;
				MarkMeshSkeletonDirty(data.m_owner.StaticCast<GameObject>());
			}
		}
		m_bGpuLayoutDirty = false;
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

		bool bPoseChanged = false;
		// Keep the completed pose while importer tasks prepare the next clip set.
		if (!data.m_bIsControllerRefreshPending || data.m_currentMatrices.IsEmpty())
		{
			AnimationPtr source;
			AnimationPtr destination;
			float sourceTime = 0.0f;
			float destinationTime = 0.0f;
			bool bSourceLoops = false;
			bool bDestinationLoops = false;
			const bool bUseController = !data.m_bIsControllerRefreshPending &&
				data.m_controllerInstance.IsValid() && data.m_animationSet &&
				data.m_controllerAnimations.Num() == data.m_controller->GetStates().Num();
			if (bUseController)
			{
				auto& instance = data.m_controllerInstance;
				uint32_t index = instance.GetActiveStateIndex();
				const auto active = index < data.m_controllerAnimations.Num() ?
					data.m_controllerAnimations[index] : AnimationPtr{};
				const float delta = data.m_bIsPlaying ? deltaTime * (std::max)(data.m_playSpeed, 0.0f) : 0.0f;
				instance.Tick(delta, active ? active->m_duration : 0.0f);
				index = instance.GetActiveStateIndex();
				if (index < data.m_controllerAnimations.Num())
				{
					source = data.m_controllerAnimations[index];
					sourceTime = instance.GetActiveStateTime();
					bSourceLoops = data.m_controller->GetStates()[index].m_bLoop;
				}
				index = instance.GetDestinationStateIndex();
				if (instance.IsTransitioning() && index < data.m_controllerAnimations.Num())
				{
					destination = data.m_controllerAnimations[index];
					destinationTime = instance.GetDestinationStateTime();
					bDestinationLoops = data.m_controller->GetStates()[index].m_bLoop;
				}
			}
			else if (data.m_animation && data.m_animation->m_numBones == data.GetBonesCount())
			{
				source = data.m_animation;
				AdvanceAnimationFrame(data, *source, deltaTime);
				sourceTime = data.m_currentFrame / source->m_fps;
			}

			const bool bSampled = source && source->m_numBones == data.GetBonesCount() &&
				AnimationPose::Sample(source, sourceTime, bSourceLoops,
					data.m_sampledSkeleton, data.m_frameIndex, data.m_lerp);
			if (!bSampled)
			{
				if (source && source->m_numBones != data.GetBonesCount())
				{
					source.Clear();
				}
				if (!source && bUseController)
				{
					for (const auto& animation : data.m_controllerAnimations)
					{
						if (animation && animation->m_numBones == data.GetBonesCount())
						{
							source = animation;
							break;
						}
					}
				}
				if (!source && data.m_animation && data.m_animation->m_numBones == data.GetBonesCount())
				{
					source = data.m_animation;
				}
				if (source && source->m_restPose.Num() == data.GetBonesCount())
				{
					data.m_sampledSkeleton = source->m_restPose;
				}
				else
				{
					data.m_sampledSkeleton.Resize(data.GetBonesCount());
					for (auto& transform : data.m_sampledSkeleton)
					{
						transform = Math::Transform{};
					}
				}
			}
			if (bSampled && destination && destination->m_numBones == data.GetBonesCount())
			{
				uint32_t frame = 0;
				float lerp = 0.0f;
				if (AnimationPose::Sample(destination, destinationTime, bDestinationLoops,
					data.m_blendSkeleton, frame, lerp))
				{
					AnimationPose::BlendLocalPoses(data.m_sampledSkeleton, data.m_blendSkeleton,
						data.m_controllerInstance.GetTransitionAlpha(), data.m_sampledSkeleton);
				}
			}
			TVector<int32_t> rootBoneIndices;
			const auto& parents = source && source->m_parentBoneIndices.Num() == data.GetBonesCount() ?
				source->m_parentBoneIndices : rootBoneIndices;
			// Procedural clips edit samples without advancing time or the asset revision.
			// Reuse composed matrices only when the local pose and hierarchy still match.
			bPoseChanged = parents != data.m_poseParents ||
				!std::equal(data.m_sampledSkeleton.begin(), data.m_sampledSkeleton.end(),
					data.m_currentSkeleton.begin(), data.m_currentSkeleton.end(),
					[](const Math::Transform& lhs, const Math::Transform& rhs)
					{
						return lhs.m_position == rhs.m_position && lhs.m_scale == rhs.m_scale &&
							lhs.GetRotation() == rhs.GetRotation();
					});
			if (bPoseChanged)
			{
				TVector<Math::Transform>::Swap(data.m_currentSkeleton, data.m_sampledSkeleton);
				data.m_poseParents = parents;
				AnimationPose::ComposeLocalPose(data.m_currentSkeleton, parents,
					data.m_globalMatrices, data.m_composeState);
			}
		}

		ModelPtr model;
		if (auto mesh = owner->GetComponent<MeshRendererComponent>()) model = mesh->GetModel();
		if (model && !model->IsReady()) model.Clear();
		const Model* poseModel = model ? model.GetRawPtr() : nullptr;
		const uint64_t modelRevision = model ? model->GetSkeletonRevision() : 0;
		const bool bSkinningChanged = bPoseChanged || poseModel != data.m_poseModel ||
			modelRevision != data.m_poseModelRevision;
		if (bSkinningChanged)
		{
			data.m_currentMatrices.Resize(data.GetBonesCount());
			for (uint32_t index = 0; index < data.GetBonesCount(); ++index)
			{
				const glm::mat4 bind = model && index < model->GetInverseBind().Num() ?
					model->GetInverseBind()[index] : glm::mat4(1.0f);
				data.m_currentMatrices[index] = data.m_globalMatrices[index] * bind;
			}
			data.m_poseModel = poseModel;
			data.m_poseModelRevision = modelRevision;
		}

		const bool bAllocateRange = data.m_gpuOffset == AnimatorComponentData::InvalidGpuOffset;
		if (bAllocateRange)
		{
			if (!TryAllocateBoneRange(data.GetBonesCount(), m_nextBoneOffset, data.m_gpuOffset)) continue;
			MarkMeshSkeletonDirty(owner);
		}
		if (bSkinningChanged || bAllocateRange)
		{
			const size_t previousSize = m_cpuBoneMatrices.Num();
			const size_t requiredSize = static_cast<size_t>(data.m_gpuOffset) + data.m_currentMatrices.Num();
			if (previousSize < requiredSize) m_cpuBoneMatrices.Resize(requiredSize);
			for (size_t index = 0; index < data.m_currentMatrices.Num(); ++index)
			{
				const size_t offset = data.m_gpuOffset + index;
				if (offset >= previousSize || !Math::AreExactlyEqual(m_cpuBoneMatrices[offset], data.m_currentMatrices[index]))
				{
					m_cpuBoneMatrices[offset] = data.m_currentMatrices[index];
					bBoneDataChanged = true;
				}
			}
		}
	}

	if (m_cpuBoneMatrices.Num() != m_nextBoneOffset)
	{
		m_cpuBoneMatrices.Resize(m_nextBoneOffset);
		bBoneDataChanged = true;
	}
	if (bBoneDataChanged || !m_publishedBoneMatrices)
	{
		m_publishedBoneMatrices = AcquireBoneSnapshot(
			m_boneSnapshotPool,
			m_cpuBoneMatrices);
		++m_animationRevision;
	}
}

void AnimationECS::FillAnimationData(RHI::RHISceneViewPtr& sceneView)
{
	// The empty set is an immutable identity token for this AnimationECS. The
	// frame graph replaces it with a flight-local binding before recording draws.
	sceneView->m_boneMatrices = m_bonesBinding;
	sceneView->m_cpuBoneMatrices = m_publishedBoneMatrices;
	sceneView->m_animationRevision = m_animationRevision;
}
