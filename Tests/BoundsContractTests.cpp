#include "Math/Bounds.h"
#include "Math/Math.h"
#include "RHI/SceneView.h"

#include <array>
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

	void TestAffineBoundsMatchTransformedCorners()
	{
		const Math::AABB original(glm::vec3(2.0f, -3.0f, 1.0f), glm::vec3(1.0f, 2.0f, 0.5f));
		glm::mat4 shear(1.0f);
		shear[1][0] = 0.6f;
		shear[2][1] = -0.3f;
		const glm::mat4 matrix = glm::translate(glm::mat4(1.0f), glm::vec3(-20.0f, 4.0f, -7.0f)) *
			glm::rotate(glm::mat4(1.0f), 0.7f, glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f))) *
			shear * glm::scale(glm::mat4(1.0f), glm::vec3(-2.0f, 0.5f, 3.0f));

		Math::AABB expected;
		for (uint32_t corner = 0; corner < 8; ++corner)
		{
			const glm::vec3 point(
				(corner & 1) ? original.m_max.x : original.m_min.x,
				(corner & 2) ? original.m_max.y : original.m_min.y,
				(corner & 4) ? original.m_max.z : original.m_min.z);
			expected.Extend(glm::vec3(matrix * glm::vec4(point, 1.0f)));
		}

		Math::AABB transformed = original;
		transformed.Apply(matrix);
		Require(IsNear(transformed.m_min, expected.m_min) && IsNear(transformed.m_max, expected.m_max),
			"affine bounds must enclose exactly the transformed corners, including negative scale and shear");
	}

	void TestTransformInversePointAndVectorMatchMatrices()
	{
		const glm::quat rotation = glm::angleAxis(0.7f, Math::vec3_Up) * glm::angleAxis(-0.4f, Math::vec3_Right);
		const glm::vec3 scales[] = { glm::vec3(2.0f), glm::vec3(2.0f, 3.0f, 0.5f), glm::vec3(-2.0f, 0.5f, -3.0f) };
		for (const glm::vec3& scale : scales)
		{
			const Math::Transform transform(glm::vec4(5.0f, -7.0f, 3.0f, 1.0f), rotation, glm::vec4(scale, 1.0f));
			const glm::mat4 matrix = transform.Matrix();
			for (float w : { 0.0f, 1.0f })
			{
				const glm::vec4 point(1.25f, -2.5f, 0.75f, w);
				const glm::vec4 transformedPoint = transform.TransformPosition(point);
				const glm::vec4 transformedVector = transform.TransformVector(point);
				Require(IsNear(glm::vec3(transformedPoint), glm::vec3(matrix * glm::vec4(glm::vec3(point), 1.0f))),
					"position transform must apply T * R * S to xyz");
				Require(IsNear(glm::vec3(transformedVector), glm::vec3(matrix * glm::vec4(glm::vec3(point), 0.0f))),
					"vector transform must omit translation");
				Require(IsNear(glm::vec3(transform.InverseTransformPosition(transformedPoint)), glm::vec3(point)),
					"inverse position must undo rotation before non-uniform scale");
				Require(IsNear(glm::vec3(transform.InverseTransformVector(transformedVector)), glm::vec3(point)),
					"inverse vector must undo rotation before non-uniform scale");
				Require(transformedPoint.w == w && transformedVector.w == w &&
					transform.InverseTransformPosition(transformedPoint).w == w &&
					transform.InverseTransformVector(transformedVector).w == w,
					"xyz helpers must preserve w rather than add the stored translation's w");
			}
		}
	}

	void TestTransformCompositionUsesParentRotationFirst()
	{
		const Math::Transform local(glm::vec4(1.0f, -2.0f, 3.0f, 0.0f),
			glm::angleAxis(0.7f, Math::vec3_Right), glm::vec4(1.0f, 2.0f, -0.5f, 1.0f));
		const Math::Transform parent(glm::vec4(-4.0f, 5.0f, 2.0f, 1.0f),
			glm::angleAxis(-0.5f, Math::vec3_Up), glm::vec4(3.0f, 3.0f, 3.0f, 1.0f));
		const glm::mat4 expected = parent.Matrix() * local.Matrix();
		Require(Math::AreNearlyEqual((local * parent).Matrix(), expected),
			"local * parent must match parent matrix times local matrix when TRS is exact");
		Math::Transform assigned = local;
		assigned *= parent;
		Require(Math::AreNearlyEqual(assigned.Matrix(), expected), "in-place composition must use the same order");

		const Math::Transform nonUniformParent(parent.m_position, parent.m_rotation, glm::vec4(2.0f, -3.0f, 0.5f, 1.0f));
		const Math::Transform unrotatedLocal(local.m_position, Math::quat_Identity, local.m_scale);
		Require(Math::AreNearlyEqual((unrotatedLocal * nonUniformParent).Matrix(),
			nonUniformParent.Matrix() * unrotatedLocal.Matrix()),
			"non-uniform scale composition must remain exact when local rotation does not introduce shear");

		const glm::vec4 point(1.0f, 2.0f, -3.0f, 1.0f);
		const glm::mat4 affine = nonUniformParent.Matrix() * local.Matrix();
		Require(IsNear(glm::vec3(affine * point),
			glm::vec3(nonUniformParent.TransformPosition(local.TransformPosition(point)))),
			"hierarchies with shear must be representable by the exact matrix path");
	}

	void TestTrsInverseForRepresentableTransforms()
	{
		const glm::quat rotation = glm::angleAxis(0.6f, Math::vec3_Up) * glm::angleAxis(0.3f, Math::vec3_Right);
		const Math::Transform transforms[] = {
			Math::Transform(glm::vec4(2.0f, -5.0f, 7.0f, 1.0f), rotation, glm::vec4(3.0f, 3.0f, 3.0f, 1.0f)),
			Math::Transform(glm::vec4(-3.0f, 1.0f, 5.0f, 0.0f), rotation, glm::vec4(-2.0f, -2.0f, -2.0f, 1.0f)),
			Math::Transform(glm::vec4(5.0f, -2.0f, 1.0f, 1.0f), Math::quat_Identity, glm::vec4(2.0f, -3.0f, 0.5f, 1.0f))
		};
		for (const Math::Transform& transform : transforms)
		{
			Require(Math::AreNearlyEqual(transform.Inverse().Matrix(), glm::inverse(transform.Matrix())),
				"TRS inverse must equal the affine inverse when rotation and scale commute");
			Require(Math::AreNearlyEqual(Math::Transform::FromMatrix(transform.Matrix()).Matrix(), transform.Matrix()),
				"matrix decomposition must preserve representable TRS including reflections");
		}
	}

	void TestPublicRotationWritesRemainNormalizedOnUse()
	{
		Math::Transform transform;
		const glm::quat rotation = glm::angleAxis(0.8f, Math::vec3_Up);
		transform.m_rotation = rotation * 5.0f;
		Require(IsNear(transform.GetForward(), rotation * Math::vec3_Forward),
			"public authored non-unit rotations must keep normalized direction behavior");
		Require(IsNear(glm::vec3(transform.TransformVector(Math::vec4_Forward)), rotation * Math::vec3_Forward),
			"vector and direction helpers must agree after a public rotation write");
		transform.m_rotation = glm::quat(0.0f, 0.0f, 0.0f, 0.0f);
		Require(Math::AreNearlyEqual(transform.Matrix(), glm::mat4(1.0f)), "zero rotation must retain identity fallback");
		transform.m_rotation.x = std::numeric_limits<float>::quiet_NaN();
		Require(Math::AllFinite(transform.Matrix()) && IsNear(transform.GetForward(), Math::vec3_Forward),
			"invalid authored rotation must retain finite identity fallback");
	}

	void TestPerspectiveFrustumMatchesCameraConstruction()
	{
		constexpr float aspect = 1.5f;
		constexpr float fov = 60.0f;
		constexpr float zNear = 0.5f;
		constexpr float zFar = 80.0f;
		const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 7.0f, -3.0f)) *
			glm::rotate(glm::mat4(1.0f), 0.6f, Math::vec3_Up) *
			glm::rotate(glm::mat4(1.0f), -0.2f, Math::vec3_Right);
		const glm::mat4 projectionView = glm::perspectiveRH_ZO(glm::radians(fov), aspect, zFar, zNear) * glm::inverse(world);
		const Math::Frustum projected(projectionView);
		Math::Frustum camera;
		camera.ExtractFrustumPlanes(world, aspect, fov, zNear, zFar);
		Math::Frustum rawPlanes;
		rawPlanes.ExtractFrustumPlanes(projectionView, false);
		for (uint32_t corner = 0; corner < 8; ++corner)
		{
			Require(IsNear(projected.GetCorners()[corner], camera.GetCorners()[corner], 0.001f),
				"perspective and camera frusta must reconstruct the same reverse-Z corners");
		}

		for (float depth : { 0.25f, 1.0f, 5.0f, 40.0f, 100.0f })
		{
			for (float x : { -1.25f, -0.75f, 0.0f, 0.75f, 1.25f })
			{
				for (float y : { -1.25f, -0.75f, 0.0f, 0.75f, 1.25f })
				{
					const float halfHeight = depth * std::tan(glm::radians(fov) * 0.5f);
					const glm::vec3 point = world * glm::vec4(x * halfHeight * aspect, y * halfHeight, -depth, 1.0f);
					const bool inside = depth > zNear && depth < zFar && std::abs(x) < 1.0f && std::abs(y) < 1.0f;
					Require(projected.ContainsPoint(point) == inside && camera.ContainsPoint(point) == inside,
						"perspective side planes must narrow toward the camera rather than form an orthographic box");
					Require(rawPlanes.ContainsPoint(point) == inside, "plane normalization must not change point clipping");
					const Math::Sphere sphere(point, 0.03f);
					const Math::AABB bounds(point, glm::vec3(0.03f));
					Require(projected.OverlapsSphere(sphere) == camera.OverlapsSphere(sphere) &&
						projected.ContainsSphere(sphere) == camera.ContainsSphere(sphere),
						"normalized perspective planes must preserve sphere distance queries");
					Require(projected.OverlapsAABB(bounds) == camera.OverlapsAABB(bounds) &&
						rawPlanes.OverlapsAABB(bounds) == camera.OverlapsAABB(bounds),
						"raw and normalized perspective planes must agree on AABB clipping");
				}
			}
		}
	}

	void TestBatchFrustumQueriesMatchScalarForAnyCount()
	{
		const Math::Frustum frustum(glm::orthoRH_ZO(-2.0f, 2.0f, -3.0f, 3.0f, 10.0f, 1.0f));
		struct alignas(16) SphereArray
		{
			int32_t padding = 0;
			Math::Sphere values[5] = {
				Math::Sphere(glm::vec3(0.0f, 0.0f, -5.0f), 0.5f),
				Math::Sphere(glm::vec3(3.0f, 0.0f, -5.0f), 0.25f),
				Math::Sphere(glm::vec3(1.75f, 0.0f, -5.0f), 0.5f),
				Math::Sphere(glm::vec3(1.5f, 0.0f, -5.0f), 0.5f),
				Math::Sphere(glm::vec3(2.5f, 0.0f, -5.0f), 0.5f)
			};
		} spheres;
		std::array<Math::AABB, 6> bounds;
		for (uint32_t i = 0; i < 5; ++i)
		{
			bounds[i + 1] = Math::AABB(spheres.values[i].m_center, glm::vec3(spheres.values[i].m_radius));
		}
		Require(frustum.ContainsSphere(spheres.values[0]) && !frustum.OverlapsSphere(spheres.values[1]) &&
			!frustum.ContainsSphere(spheres.values[2]) && frustum.ContainsSphere(spheres.values[3]) &&
			frustum.OverlapsSphere(spheres.values[4]), "sphere inside, outside and tangent cases must retain their scalar contract");

		frustum.OverlapsAABB(nullptr, 0, nullptr);
		frustum.OverlapsSphere(nullptr, 0, nullptr);
		frustum.ContainsSphere(nullptr, 0, nullptr);
		for (uint32_t count : { 0u, 1u, 3u, 4u, 5u })
		{
			alignas(16) std::array<int32_t, 7> results;
			auto checkRange = [&]()
			{
				Require(results[0] == -17, "batch query must not write before its output range");
				for (size_t i = count + 1; i < results.size(); ++i)
				{
					Require(results[i] == -17, "batch query must not write beyond the requested count");
				}
			};
			results.fill(-17);
			frustum.OverlapsAABB(bounds.data() + 1, count, results.data() + 1);
			checkRange();
			for (uint32_t i = 0; i < count; ++i)
			{
				Require(results[i + 1] == (frustum.OverlapsAABB(bounds[i + 1]) ? 0 : 1),
					"batch AABB overlap must preserve scalar geometry and culling polarity");
			}
			results.fill(-17);
			frustum.OverlapsSphere(spheres.values, count, results.data() + 1);
			checkRange();
			for (uint32_t i = 0; i < count; ++i)
			{
				Require(results[i + 1] == (frustum.OverlapsSphere(spheres.values[i]) ? 0 : 1),
					"batch sphere overlap must preserve scalar tangent behavior and culling polarity");
			}
			results.fill(-17);
			frustum.ContainsSphere(spheres.values, count, results.data() + 1);
			checkRange();
			for (uint32_t i = 0; i < count; ++i)
			{
				Require(results[i + 1] == (frustum.ContainsSphere(spheres.values[i]) ? 1 : 0),
					"batch sphere containment must preserve scalar results and containment polarity");
			}
		}
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
		{ "AffineBoundsMatchTransformedCorners", TestAffineBoundsMatchTransformedCorners },
		{ "TransformInversePointAndVectorMatchMatrices", TestTransformInversePointAndVectorMatchMatrices },
		{ "TransformCompositionUsesParentRotationFirst", TestTransformCompositionUsesParentRotationFirst },
		{ "TrsInverseForRepresentableTransforms", TestTrsInverseForRepresentableTransforms },
		{ "PublicRotationWritesRemainNormalizedOnUse", TestPublicRotationWritesRemainNormalizedOnUse },
		{ "PerspectiveFrustumMatchesCameraConstruction", TestPerspectiveFrustumMatchesCameraConstruction },
		{ "BatchFrustumQueriesMatchScalarForAnyCount", TestBatchFrustumQueriesMatchScalarForAnyCount },
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
