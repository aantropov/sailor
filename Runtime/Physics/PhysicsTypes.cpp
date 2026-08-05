#include "Physics/PhysicsTypes.h"
#include "Math/Math.h"
#include "Math/Transform.h"
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>

using namespace Sailor;

namespace
{
	bool IsFiniteMatrix(const glm::mat4& matrix)
	{
		for (glm::length_t column = 0; column < matrix.length(); ++column)
		{
			for (glm::length_t row = 0; row < matrix[column].length(); ++row)
			{
				if (!std::isfinite(matrix[column][row]))
				{
					return false;
				}
			}
		}
		return true;
	}

	glm::quat NormalizeRotation(const glm::quat& rotation)
	{
		const glm::vec4 raw(
			rotation.x,
			rotation.y,
			rotation.z,
			rotation.w);
		const glm::vec4 normalized = Math::SafeNormalize(
			raw,
			glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
		return glm::quat(
			normalized.w,
			normalized.x,
			normalized.y,
			normalized.z);
	}
}

bool Physics::TryConvertWorldPoseToLocal(
	const glm::mat4& parentWorldMatrix,
	const glm::vec3& worldPosition,
	const glm::quat& worldRotation,
	glm::vec3& outLocalPosition,
	glm::quat& outLocalRotation)
{
	const glm::vec4 worldRotationVector(
		worldRotation.x,
		worldRotation.y,
		worldRotation.z,
		worldRotation.w);
	if (!IsFiniteMatrix(parentWorldMatrix) ||
		!Math::AllFinite(worldPosition) ||
		!Math::AllFinite(worldRotationVector))
	{
		return false;
	}

	const glm::vec3 axisX(parentWorldMatrix[0]);
	const glm::vec3 axisY(parentWorldMatrix[1]);
	const glm::vec3 axisZ(parentWorldMatrix[2]);
	const float axisLengthProduct =
		glm::length(axisX) * glm::length(axisY) * glm::length(axisZ);
	const float normalizedVolume = axisLengthProduct > 0.0f
		? std::abs(glm::dot(glm::cross(axisX, axisY), axisZ)) /
			axisLengthProduct
		: 0.0f;
	if (!std::isfinite(normalizedVolume) ||
		normalizedVolume <= 0.000001f)
	{
		return false;
	}

	const glm::mat4 inverseParent = glm::inverse(parentWorldMatrix);
	if (!IsFiniteMatrix(inverseParent))
	{
		return false;
	}

	const glm::vec4 localHomogeneous =
		inverseParent * glm::vec4(worldPosition, 1.0f);
	if (!Math::AllFinite(localHomogeneous) ||
		std::abs(localHomogeneous.w) <= 0.000001f)
	{
		return false;
	}

	const Math::Transform parentTransform =
		Math::Transform::FromMatrix(parentWorldMatrix);
	const glm::vec4 parentRotation(
		parentTransform.m_rotation.x,
		parentTransform.m_rotation.y,
		parentTransform.m_rotation.z,
		parentTransform.m_rotation.w);
	if (!Math::AllFinite(parentRotation))
	{
		return false;
	}

	outLocalPosition = glm::vec3(localHomogeneous) /
		localHomogeneous.w;
	outLocalRotation = NormalizeRotation(
		glm::inverse(NormalizeRotation(parentTransform.m_rotation)) *
		NormalizeRotation(worldRotation));
	return Math::AllFinite(outLocalPosition);
}
