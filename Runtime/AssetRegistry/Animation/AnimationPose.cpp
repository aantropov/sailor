#include "AnimationPose.h"
#include "Math/Math.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;

bool AnimationPose::Sample(
	const AnimationPtr& animation,
	float time,
	bool bLoop,
	TVector<Math::Transform>& outLocalPose,
	uint32_t& outFrameIndex,
	float& outLerp)
{
	if (!animation || !std::isfinite(time) ||
		animation->m_numFrames == 0 || animation->m_numBones == 0 ||
		animation->m_frames.Num() !=
			static_cast<size_t>(animation->m_numFrames) * animation->m_numBones)
	{
		return false;
	}

	const float lastFrame = static_cast<float>(animation->m_numFrames - 1);
	float frame = time * animation->m_fps;
	if (bLoop && lastFrame > 0.0f)
	{
		frame = std::fmod(frame, lastFrame);
		if (frame < 0.0f)
		{
			frame += lastFrame;
		}
	}
	else
	{
		frame = std::clamp(frame, 0.0f, lastFrame);
	}

	outFrameIndex = (std::min)(
		static_cast<uint32_t>(std::floor(frame)),
		animation->m_numFrames - 1);
	outLerp = frame - std::floor(frame);
	const uint32_t nextFrame = (std::min)(
		outFrameIndex + 1,
		animation->m_numFrames - 1);
	if (outLocalPose.Num() != animation->m_numBones)
	{
		outLocalPose.Resize(animation->m_numBones);
	}
	for (uint32_t boneIndex = 0; boneIndex < animation->m_numBones; ++boneIndex)
	{
		const Math::Transform& from = animation->m_frames[
			static_cast<size_t>(outFrameIndex) * animation->m_numBones + boneIndex];
		const Math::Transform& to = animation->m_frames[
			static_cast<size_t>(nextFrame) * animation->m_numBones + boneIndex];
		outLocalPose[boneIndex] = Math::Lerp(from, to, outLerp);
	}
	return true;
}

bool AnimationPose::ComposeLocalPose(
	const TVector<Math::Transform>& localPose,
	const TVector<int32_t>& parentBoneIndices,
	TVector<glm::mat4>& outGlobalMatrices,
	TVector<uint8_t>& scratchComposeState)
{
	if (localPose.IsEmpty() ||
		(!parentBoneIndices.IsEmpty() && parentBoneIndices.Num() != localPose.Num()))
	{
		return false;
	}

	outGlobalMatrices.Resize(localPose.Num());
	scratchComposeState.Resize(localPose.Num());
	for (auto& state : scratchComposeState)
	{
		state = 0;
	}

	bool bValid = true;
	auto composeBone = [&](auto&& self, size_t boneIndex) -> void
	{
		if (scratchComposeState[boneIndex] == 2)
		{
			return;
		}
		const glm::mat4 localMatrix = localPose[boneIndex].Matrix();
		if (scratchComposeState[boneIndex] == 1)
		{
			outGlobalMatrices[boneIndex] = localMatrix;
			scratchComposeState[boneIndex] = 2;
			bValid = false;
			return;
		}

		scratchComposeState[boneIndex] = 1;
		const int32_t parentBoneIndex = parentBoneIndices.IsEmpty() ?
			-1 : parentBoneIndices[boneIndex];
		if (parentBoneIndex >= 0)
		{
			if (static_cast<size_t>(parentBoneIndex) >= localPose.Num() ||
				static_cast<size_t>(parentBoneIndex) == boneIndex)
			{
				outGlobalMatrices[boneIndex] = localMatrix;
				bValid = false;
			}
			else
			{
				self(self, static_cast<size_t>(parentBoneIndex));
				outGlobalMatrices[boneIndex] =
					outGlobalMatrices[static_cast<size_t>(parentBoneIndex)] * localMatrix;
			}
		}
		else
		{
			outGlobalMatrices[boneIndex] = localMatrix;
		}
		scratchComposeState[boneIndex] = 2;
	};

	for (size_t boneIndex = 0; boneIndex < localPose.Num(); ++boneIndex)
	{
		composeBone(composeBone, boneIndex);
	}
	return bValid;
}

bool AnimationPose::BlendLocalPoses(
	const TVector<Math::Transform>& from,
	const TVector<Math::Transform>& to,
	float alpha,
	TVector<Math::Transform>& outPose)
{
	if (from.IsEmpty() || from.Num() != to.Num() || !std::isfinite(alpha))
	{
		return false;
	}

	alpha = std::clamp(alpha, 0.0f, 1.0f);
	if (outPose.Num() != from.Num())
	{
		outPose.Resize(from.Num());
	}
	for (size_t boneIndex = 0; boneIndex < from.Num(); ++boneIndex)
	{
		outPose[boneIndex] = Math::Lerp(from[boneIndex], to[boneIndex], alpha);
	}
	return true;
}
