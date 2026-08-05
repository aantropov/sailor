#pragma once

#include "AnimationImporter.h"
#include "Containers/Vector.h"

#include <glm/mat4x4.hpp>

namespace Sailor::AnimationPose
{
	SAILOR_API bool Sample(
		const AnimationPtr& animation,
		float time,
		bool bLoop,
		TVector<Math::Transform>& outLocalPose,
		uint32_t& outFrameIndex,
		float& outLerp);

	SAILOR_API bool ComposeLocalPose(
		const TVector<Math::Transform>& localPose,
		const TVector<int32_t>& parentBoneIndices,
		TVector<glm::mat4>& outGlobalMatrices,
		TVector<uint8_t>& scratchComposeState);

	SAILOR_API bool BlendLocalPoses(
		const TVector<Math::Transform>& from,
		const TVector<Math::Transform>& to,
		float alpha,
		TVector<Math::Transform>& outPose);
}
