#include "AnimationClipSampler.h"
#include "Math/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Sailor;

namespace
{
	bool IsQuaternionFinite(const glm::quat& value)
	{
		return std::isfinite(value.x) &&
			std::isfinite(value.y) &&
			std::isfinite(value.z) &&
			std::isfinite(value.w);
	}

	size_t GetValueIndex(uint32_t keyframe, EAnimationInterpolation interpolation)
	{
		return interpolation == EAnimationInterpolation::CubicSpline ?
			static_cast<size_t>(keyframe) * 3 + 1 : keyframe;
	}

	bool HasExpectedValueCount(
		const TVector<float>& timestamps,
		const TVector<glm::vec4>& values,
		EAnimationInterpolation interpolation)
	{
		const size_t multiplier = interpolation == EAnimationInterpolation::CubicSpline ? 3 : 1;
		return timestamps.Num() <= (std::numeric_limits<size_t>::max)() / multiplier &&
			values.Num() == timestamps.Num() * multiplier;
	}

	glm::vec4 CubicHermite(
		const glm::vec4& first,
		const glm::vec4& firstOutTangent,
		const glm::vec4& second,
		const glm::vec4& secondInTangent,
		float alpha,
		float duration)
	{
		const float alpha2 = alpha * alpha;
		const float alpha3 = alpha2 * alpha;
		const float h00 = 2.0f * alpha3 - 3.0f * alpha2 + 1.0f;
		const float h10 = alpha3 - 2.0f * alpha2 + alpha;
		const float h01 = -2.0f * alpha3 + 3.0f * alpha2;
		const float h11 = alpha3 - alpha2;
		return h00 * first +
			h10 * duration * firstOutTangent +
			h01 * second +
			h11 * duration * secondInTangent;
	}
}

bool AnimationClipSampler::ValidateTimestamps(const TVector<float>& timestamps)
{
	if (timestamps.IsEmpty())
	{
		return false;
	}

	float previous = timestamps[0];
	if (!std::isfinite(previous) || previous < 0.0f)
	{
		return false;
	}

	for (size_t i = 1; i < timestamps.Num(); ++i)
	{
		const float current = timestamps[i];
		if (!std::isfinite(current) || current <= previous)
		{
			return false;
		}
		previous = current;
	}

	return true;
}

bool AnimationClipSampler::ResolveKeyframeSpan(
	const TVector<float>& timestamps,
	float time,
	AnimationKeyframeSpan& outSpan)
{
	outSpan = {};
	if (!ValidateTimestamps(timestamps) || !std::isfinite(time))
	{
		return false;
	}

	if (time <= timestamps[0] || timestamps.Num() == 1)
	{
		return true;
	}

	const uint32_t last = static_cast<uint32_t>(timestamps.Num() - 1);
	if (time >= timestamps[last])
	{
		outSpan.m_first = last;
		outSpan.m_second = last;
		return true;
	}

	uint32_t left = 1;
	uint32_t right = last;
	while (left < right)
	{
		const uint32_t middle = left + (right - left) / 2;
		if (timestamps[middle] <= time)
		{
			left = middle + 1;
		}
		else
		{
			right = middle;
		}
	}

	outSpan.m_first = left - 1;
	outSpan.m_second = left;
	outSpan.m_duration = timestamps[left] - timestamps[left - 1];
	outSpan.m_alpha = std::clamp(
		(time - timestamps[left - 1]) / outSpan.m_duration,
		0.0f,
		1.0f);
	return true;
}

bool AnimationClipSampler::SampleVector(
	const TVector<float>& timestamps,
	const TVector<glm::vec4>& values,
	EAnimationInterpolation interpolation,
	float time,
	glm::vec4& outValue)
{
	AnimationKeyframeSpan span;
	if (!HasExpectedValueCount(timestamps, values, interpolation) ||
		!ResolveKeyframeSpan(timestamps, time, span))
	{
		return false;
	}

	const size_t firstValue = GetValueIndex(span.m_first, interpolation);
	if (span.m_first == span.m_second || interpolation == EAnimationInterpolation::Step)
	{
		outValue = values[firstValue];
	}
	else if (interpolation == EAnimationInterpolation::Linear)
	{
		outValue = glm::mix(values[firstValue], values[span.m_second], span.m_alpha);
	}
	else
	{
		const size_t firstBase = static_cast<size_t>(span.m_first) * 3;
		const size_t secondBase = static_cast<size_t>(span.m_second) * 3;
		outValue = CubicHermite(
			values[firstBase + 1],
			values[firstBase + 2],
			values[secondBase + 1],
			values[secondBase],
			span.m_alpha,
			span.m_duration);
	}

	return Math::AllFinite(outValue);
}

bool AnimationClipSampler::SampleRotation(
	const TVector<float>& timestamps,
	const TVector<glm::vec4>& values,
	EAnimationInterpolation interpolation,
	float time,
	glm::quat& outValue)
{
	AnimationKeyframeSpan span;
	if (!HasExpectedValueCount(timestamps, values, interpolation) ||
		!ResolveKeyframeSpan(timestamps, time, span))
	{
		return false;
	}

	const auto toQuaternion = [](const glm::vec4& value)
	{
		return glm::quat(value.w, value.x, value.y, value.z);
	};
	const auto normalize = [](const glm::quat& value, glm::quat& out)
	{
		const float lengthSquared = glm::dot(value, value);
		if (!std::isfinite(lengthSquared) ||
			lengthSquared <= std::numeric_limits<float>::epsilon())
		{
			return false;
		}
		out = glm::normalize(value);
		return IsQuaternionFinite(out);
	};

	const size_t firstValue = GetValueIndex(span.m_first, interpolation);
	if (span.m_first == span.m_second || interpolation == EAnimationInterpolation::Step)
	{
		return normalize(toQuaternion(values[firstValue]), outValue);
	}

	if (interpolation == EAnimationInterpolation::Linear)
	{
		glm::quat first;
		glm::quat second;
		if (!normalize(toQuaternion(values[firstValue]), first) ||
			!normalize(toQuaternion(values[span.m_second]), second))
		{
			return false;
		}
		outValue = glm::slerp(first, second, span.m_alpha);
		return IsQuaternionFinite(outValue);
	}

	const size_t firstBase = static_cast<size_t>(span.m_first) * 3;
	const size_t secondBase = static_cast<size_t>(span.m_second) * 3;
	const glm::vec4 value = CubicHermite(
		values[firstBase + 1],
		values[firstBase + 2],
		values[secondBase + 1],
		values[secondBase],
		span.m_alpha,
		span.m_duration);
	return normalize(toQuaternion(value), outValue);
}
