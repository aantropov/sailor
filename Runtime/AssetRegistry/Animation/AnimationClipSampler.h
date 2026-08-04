#pragma once

#include "Core/Defines.h"
#include "Memory/LockFreeHeapAllocator.h"
#include "Containers/Vector.h"
#include <glm/vec4.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Sailor
{
	enum class EAnimationInterpolation : uint8_t
	{
		Linear,
		Step,
		CubicSpline
	};

	struct AnimationKeyframeSpan
	{
		uint32_t m_first = 0;
		uint32_t m_second = 0;
		float m_alpha = 0.0f;
		float m_duration = 0.0f;
	};

	class AnimationClipSampler final
	{
	public:
		SAILOR_API static bool ValidateTimestamps(const TVector<float>& timestamps);
		SAILOR_API static bool ResolveKeyframeSpan(
			const TVector<float>& timestamps,
			float time,
			AnimationKeyframeSpan& outSpan);

		SAILOR_API static bool SampleVector(
			const TVector<float>& timestamps,
			const TVector<glm::vec4>& values,
			EAnimationInterpolation interpolation,
			float time,
			glm::vec4& outValue);

		SAILOR_API static bool SampleRotation(
			const TVector<float>& timestamps,
			const TVector<glm::vec4>& values,
			EAnimationInterpolation interpolation,
			float time,
			glm::quat& outValue);
	};
}
