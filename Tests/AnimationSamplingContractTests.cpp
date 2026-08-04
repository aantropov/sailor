#include "AssetRegistry/Animation/AnimationClipSampler.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Sailor;

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	bool NearlyEqual(float lhs, float rhs, float epsilon = 0.0001f)
	{
		return std::abs(lhs - rhs) <= epsilon;
	}

	void TestTimestampValidationAndNonUniformSpan()
	{
		Require(!AnimationClipSampler::ValidateTimestamps({}),
			"empty timestamp arrays must be rejected");
		Require(!AnimationClipSampler::ValidateTimestamps({ -1.0f, 0.0f }),
			"negative glTF timestamps must be rejected");
		Require(!AnimationClipSampler::ValidateTimestamps({ 0.0f, 0.0f }),
			"duplicate glTF timestamps must be rejected");
		Require(!AnimationClipSampler::ValidateTimestamps({
			0.0f,
			(std::numeric_limits<float>::quiet_NaN)()
		}), "non-finite glTF timestamps must be rejected");

		AnimationKeyframeSpan span;
		Require(AnimationClipSampler::ResolveKeyframeSpan(
			{ 0.0f, 0.25f, 2.0f },
			1.125f,
			span), "valid non-uniform timestamps must resolve");
		Require(span.m_first == 1 && span.m_second == 2,
			"sampling must select keyframes by time rather than array index");
		Require(NearlyEqual(span.m_alpha, 0.5f) && NearlyEqual(span.m_duration, 1.75f),
			"non-uniform keyframe interpolation must use the selected time interval");
	}

	void TestLinearAndStepVectorSampling()
	{
		const TVector<float> timestamps{ 0.0f, 2.0f };
		const TVector<glm::vec4> values{
			glm::vec4(0.0f),
			glm::vec4(10.0f, 4.0f, -2.0f, 0.0f)
		};

		glm::vec4 sampled;
		Require(AnimationClipSampler::SampleVector(
			timestamps,
			values,
			EAnimationInterpolation::Linear,
			0.5f,
			sampled), "LINEAR vector channels must sample");
		Require(NearlyEqual(sampled.x, 2.5f) &&
			NearlyEqual(sampled.y, 1.0f) &&
			NearlyEqual(sampled.z, -0.5f),
			"LINEAR vector channels must interpolate using glTF timestamps");

		Require(AnimationClipSampler::SampleVector(
			timestamps,
			values,
			EAnimationInterpolation::Step,
			1.999f,
			sampled), "STEP vector channels must sample");
		Require(NearlyEqual(sampled.x, 0.0f),
			"STEP vector channels must retain the preceding keyframe");

		Require(AnimationClipSampler::SampleVector(
			timestamps,
			values,
			EAnimationInterpolation::Step,
			2.0f,
			sampled) && NearlyEqual(sampled.x, 10.0f),
			"STEP vector channels must reach the final keyframe at its timestamp");
	}

	void TestCubicSplineSamplingScalesTangentsByInterval()
	{
		const TVector<float> timestamps{ 0.0f, 2.0f };
		const TVector<glm::vec4> values{
			glm::vec4(0.0f),
			glm::vec4(0.0f),
			glm::vec4(2.0f, 0.0f, 0.0f, 0.0f),
			glm::vec4(2.0f, 0.0f, 0.0f, 0.0f),
			glm::vec4(4.0f, 0.0f, 0.0f, 0.0f),
			glm::vec4(0.0f)
		};

		glm::vec4 sampled;
		Require(AnimationClipSampler::SampleVector(
			timestamps,
			values,
			EAnimationInterpolation::CubicSpline,
			1.0f,
			sampled), "CUBICSPLINE vector channels must sample");
		Require(NearlyEqual(sampled.x, 2.0f),
			"CUBICSPLINE tangents must be scaled by the keyframe interval");
	}

	void TestRotationSamplingProducesNormalizedShortestPath()
	{
		const float halfSqrt = std::sqrt(0.5f);
		glm::quat sampled;
		Require(AnimationClipSampler::SampleRotation(
			{ 0.0f, 1.0f },
			{
				glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
				glm::vec4(0.0f, 0.0f, 0.0f, -1.0f)
			},
			EAnimationInterpolation::Linear,
			0.5f,
			sampled), "LINEAR rotation channels must sample");
		Require(NearlyEqual(std::abs(sampled.w), 1.0f),
			"LINEAR quaternion sampling must use the shortest normalized path");

		Require(AnimationClipSampler::SampleRotation(
			{ 0.0f, 1.0f },
			{
				glm::vec4(0.0f),
				glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
				glm::vec4(0.0f),
				glm::vec4(0.0f),
				glm::vec4(0.0f, 0.0f, 1.0f, 0.0f),
				glm::vec4(0.0f)
			},
			EAnimationInterpolation::CubicSpline,
			0.5f,
			sampled), "CUBICSPLINE rotation channels must sample");
		Require(NearlyEqual(sampled.z, halfSqrt) &&
			NearlyEqual(sampled.w, halfSqrt) &&
			NearlyEqual(glm::length(sampled), 1.0f),
			"CUBICSPLINE quaternion output must be normalized after Hermite interpolation");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "TimestampValidationAndNonUniformSpan", TestTimestampValidationAndNonUniformSpan },
		{ "LinearAndStepVectorSampling", TestLinearAndStepVectorSampling },
		{ "CubicSplineSamplingScalesTangentsByInterval", TestCubicSplineSamplingScalesTangentsByInterval },
		{ "RotationSamplingProducesNormalizedShortestPath", TestRotationSamplingProducesNormalizedShortestPath }
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": "
				<< error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
