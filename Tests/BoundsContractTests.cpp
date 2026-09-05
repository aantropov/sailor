#include "Math/Bounds.h"
#include "RHI/SceneView.h"

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

	bool IsNear(const glm::vec3& lhs, const glm::vec3& rhs, float epsilon = 0.0001f)
	{
		return std::abs(lhs.x - rhs.x) <= epsilon &&
			std::abs(lhs.y - rhs.y) <= epsilon &&
			std::abs(lhs.z - rhs.z) <= epsilon;
	}

	float ProjectDepth(const glm::mat4& projectionView, const glm::vec3& point)
	{
		const glm::vec4 clip = projectionView * glm::vec4(point, 1.0f);
		Require(std::abs(clip.w) > 0.000001f, "projected point must have a valid homogeneous coordinate");
		return clip.z / clip.w;
	}

	void TestValidityRejectsSentinelAndInvertedBounds()
	{
		const Math::AABB defaultBounds;
		Require(!defaultBounds.IsValid(), "default sentinel bounds must be invalid");

		Math::AABB invertedBounds;
		invertedBounds.m_min = glm::vec3(-1.0f, 2.0f, -1.0f);
		invertedBounds.m_max = glm::vec3(1.0f, 1.0f, 1.0f);
		Require(!invertedBounds.IsValid(), "bounds with min greater than max on any axis must be invalid");

		Math::AABB pointBounds;
		pointBounds.m_min = glm::vec3(3.0f, -2.0f, 5.0f);
		pointBounds.m_max = pointBounds.m_min;
		Require(pointBounds.IsValid(), "finite zero-volume bounds must remain valid");
	}

	void TestValidityRejectsNonFiniteBounds()
	{
		Math::AABB bounds(glm::vec3(0.0f), glm::vec3(1.0f));
		Require(bounds.IsValid(), "finite ordered bounds must be valid");

		bounds.m_min.x = std::numeric_limits<float>::quiet_NaN();
		Require(!bounds.IsValid(), "bounds containing NaN must be invalid");

		bounds = Math::AABB(glm::vec3(0.0f), glm::vec3(1.0f));
		bounds.m_max.z = std::numeric_limits<float>::infinity();
		Require(!bounds.IsValid(), "bounds containing infinity must be invalid");

		bounds.m_max.z = -std::numeric_limits<float>::infinity();
		Require(!bounds.IsValid(), "bounds containing negative infinity must be invalid");
	}

	void TestTransformPreservesAllNegativeBounds()
	{
		Math::AABB bounds(glm::vec3(1.0f), glm::vec3(1.0f));
		const glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(-10.0f, -20.0f, -30.0f));

		bounds.Apply(transform);

		Require(bounds.IsValid(), "transformed all-negative bounds must remain valid");
		Require(IsNear(bounds.m_min, glm::vec3(-10.0f, -20.0f, -30.0f)),
			"transformed all-negative minimum must include every corner");
		Require(IsNear(bounds.m_max, glm::vec3(-8.0f, -18.0f, -28.0f)),
			"transformed all-negative maximum must include every corner");
	}

	void TestReversedShadowProjectionUsesZeroToOneDepth()
	{
		Math::Frustum cameraSlice;
		const glm::mat4 cameraWorld = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 100.0f));
		cameraSlice.ExtractFrustumPlanes(cameraWorld, 16.0f / 9.0f, 60.0f, 1.0f, 10.0f);

		const glm::mat4 shadowProjection = cameraSlice.CalculateOrthoMatrixByView(glm::mat4(1.0f), 10.0f);
		for (const glm::vec3& corner : cameraSlice.GetCorners())
		{
			const float depth = ProjectDepth(shadowProjection, corner);
			Require(std::isfinite(depth) && depth >= -0.0001f && depth <= 1.0001f,
				"every fitted shadow-cascade corner must remain inside Vulkan's zero-to-one depth range");
		}
	}

	void TestFrustumCenterIsTheAverageOfItsCorners()
	{
		Math::Frustum cameraSlice;
		const glm::vec3 cameraPosition(13.0f, 7.0f, -19.0f);
		const glm::mat4 cameraWorld = glm::translate(glm::mat4(1.0f), cameraPosition);
		cameraSlice.ExtractFrustumPlanes(cameraWorld, 16.0f / 9.0f, 60.0f, 0.1f, 10.0f);

		glm::vec3 expectedCenter(0.0f);
		for (const glm::vec3& corner : cameraSlice.GetCorners())
		{
			expectedCenter += corner;
		}
		expectedCenter /= (float)cameraSlice.GetCorners().Num();

		Require(IsNear(cameraSlice.CalculateCenter(), expectedCenter),
			"frustum center must be the average of its corners rather than their unnormalized sum");
	}

	void TestReverseZFrustumCornersUseZeroToOneDepth()
	{
		const glm::mat4 projection = glm::orthoRH_ZO(-2.0f, 2.0f, -3.0f, 3.0f, 100.0f, 1.0f);
		const Math::Frustum frustum(projection);

		for (uint32_t i = 0; i < 4; i++)
		{
			Require(std::abs(ProjectDepth(projection, frustum.GetCorners()[i])) <= 0.0001f,
				"reverse-Z far corners must be reconstructed from the Vulkan depth-zero plane");
		}

		for (uint32_t i = 4; i < 8; i++)
		{
			Require(std::abs(ProjectDepth(projection, frustum.GetCorners()[i]) - 1.0f) <= 0.0001f,
				"reverse-Z near corners must be reconstructed from the Vulkan depth-one plane");
		}
	}

	void TestShadowProjectionIncludesCastersTowardLightSource()
	{
		Math::Frustum cameraSlice;
		const glm::mat4 cameraWorld = glm::translate(
			glm::mat4(1.0f),
			glm::vec3(0.0f, 0.0f, -50.0f));
		cameraSlice.ExtractFrustumPlanes(
			cameraWorld,
			1.0f,
			60.0f,
			1.0f,
			10.0f);

		const glm::mat4 shadowProjection = cameraSlice.CalculateOrthoMatrixByView(
			glm::mat4(1.0f),
			10.0f,
			glm::ivec2(4096),
			200.0f);
		const float casterDepth = ProjectDepth(
			shadowProjection,
			glm::vec3(0.0f, 0.0f, 100.0f));
		Require(std::isfinite(casterDepth) &&
			casterDepth >= -0.0001f &&
			casterDepth <= 1.0001f,
			"shadow projection must include casters behind the camera toward the light source");
	}

	void TestLodPolicyResolvesCoverageAndAvailableMeshes()
	{
		RHI::RHILodPolicy policy;
		policy.m_bEnabled = true;
		policy.m_minLod = 0;
		policy.m_maxLod = 2;
		policy.m_screenCoverageThresholds = { 0.25f, 0.05f };

		Require(policy.Resolve(0.5f, 3) == 0, "high coverage must select the highest-detail LOD");
		Require(policy.Resolve(0.1f, 3) == 1, "medium coverage must select the middle LOD");
		Require(policy.Resolve(0.01f, 3) == 2, "low coverage must select the lowest-detail LOD");
		Require(policy.Resolve(0.01f, 2) == 1, "LOD selection must clamp to the available mesh count");

		policy.m_minLod = 1;
		Require(policy.Resolve(1.0f, 3) == 1, "the configured minimum LOD must be respected");
	}

	void TestLodPolicyResolvesCameraDistance()
	{
		RHI::RHILodPolicy policy;
		policy.m_bEnabled = true;
		policy.m_minLod = 0;
		policy.m_maxLod = 2;
		policy.m_cameraDistanceThresholds = { 50.0f, 100.0f };

		Require(policy.Resolve(0.0f, 49.999f, 3) == 0,
			"distance below the first threshold must select the highest-detail LOD");
		Require(policy.Resolve(1.0f, 50.0f, 3) == 1,
			"the first distance threshold must select the middle LOD independently of coverage");
		Require(policy.Resolve(1.0f, 100.0f, 3) == 2,
			"the second distance threshold must select the lowest-detail LOD");
		Require(policy.Resolve(1.0f, std::numeric_limits<float>::infinity(), 3) == 2,
			"non-finite distance must resolve to the lowest configured LOD");
		Require(policy.Resolve(1.0f, 100.0f, 2) == 1,
			"distance LOD must clamp to the available mesh count");

		policy.m_minLod = 1;
		Require(policy.Resolve(0.0f, 0.0f, 3) == 1,
			"distance LOD must respect the configured minimum LOD");
	}

	void TestStabilizedShadowProjectionKeepsReceiverGuardBand()
	{
		Math::Frustum cameraSlice;
		cameraSlice.ExtractFrustumPlanes(
			glm::translate(glm::mat4(1.0f), glm::vec3(0.013f, 0.017f, 0.0f)),
			16.0f / 9.0f,
			60.0f,
			0.1f,
			40.0f);

		const glm::ivec2 resolution(1024);
		const glm::mat4 shadowProjection = cameraSlice.CalculateOrthoMatrixByView(
			glm::mat4(1.0f),
			10.0f,
			resolution,
			200.0f);
		const glm::vec2 guardUv(2.0f / (float)resolution.x);
		for (const glm::vec3& corner : cameraSlice.GetCorners())
		{
			const glm::vec4 clip = shadowProjection * glm::vec4(corner, 1.0f);
			const glm::vec2 uv = glm::vec2(clip) * 0.5f + 0.5f;
			Require(uv.x >= guardUv.x && uv.x <= 1.0f - guardUv.x &&
				uv.y >= guardUv.y && uv.y <= 1.0f - guardUv.y,
				"stabilized CSM projection must retain every receiver corner plus the PCF footprint");
		}
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "ValidityRejectsSentinelAndInvertedBounds", TestValidityRejectsSentinelAndInvertedBounds },
		{ "ValidityRejectsNonFiniteBounds", TestValidityRejectsNonFiniteBounds },
		{ "TransformPreservesAllNegativeBounds", TestTransformPreservesAllNegativeBounds },
		{ "ReversedShadowProjectionUsesZeroToOneDepth", TestReversedShadowProjectionUsesZeroToOneDepth },
		{ "FrustumCenterIsTheAverageOfItsCorners", TestFrustumCenterIsTheAverageOfItsCorners },
		{ "ReverseZFrustumCornersUseZeroToOneDepth", TestReverseZFrustumCornersUseZeroToOneDepth },
		{ "ShadowProjectionIncludesCastersTowardLightSource", TestShadowProjectionIncludesCastersTowardLightSource },
		{ "LodPolicyResolvesCoverageAndAvailableMeshes", TestLodPolicyResolvesCoverageAndAvailableMeshes },
		{ "LodPolicyResolvesCameraDistance", TestLodPolicyResolvesCameraDistance },
		{ "StabilizedShadowProjectionKeepsReceiverGuardBand", TestStabilizedShadowProjectionKeepsReceiverGuardBand },
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
			std::cerr << "[FAIL] " << test.first << ": " << error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
