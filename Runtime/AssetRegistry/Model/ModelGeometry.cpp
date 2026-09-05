#include "AssetRegistry/Model/ModelGeometry.h"

#include "Math/Math.h"

#include <algorithm>
#include <cmath>

namespace
{
	bool TryNormalizeDirection(const glm::vec3& value, glm::vec3& outDirection)
	{
		outDirection = glm::vec3(0.0f);
		if (!Sailor::Math::AllFinite(value))
		{
			return false;
		}

		const float maxComponent = (std::max)(std::abs(value.x), (std::max)(std::abs(value.y), std::abs(value.z)));
		if (maxComponent <= 1e-4f)
		{
			return false;
		}

		const glm::vec3 scaled = value / maxComponent;
		const float lengthSquared = glm::dot(scaled, scaled);
		if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0f)
		{
			return false;
		}

		outDirection = scaled / std::sqrt(lengthSquared);
		return Sailor::Math::AllFinite(outDirection);
	}
}

void Sailor::ModelGeometry::SanitizeVertexFrame(RHI::VertexP3N3T3B3UV2C4I4W4& vertex)
{
	glm::vec3 normal;
	if (!TryNormalizeDirection(vertex.m_normal, normal))
	{
		normal = glm::vec3(0.0f, 1.0f, 0.0f);
	}

	glm::vec3 tangentInput;
	glm::vec3 tangent(0.0f);
	if (TryNormalizeDirection(vertex.m_tangent, tangentInput))
	{
		TryNormalizeDirection(tangentInput - normal * glm::dot(tangentInput, normal), tangent);
	}

	if (!TryNormalizeDirection(tangent, tangent))
	{
		const glm::vec3 axis = std::abs(normal.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
		if (!TryNormalizeDirection(glm::cross(axis, normal), tangent))
		{
			tangent = glm::vec3(1.0f, 0.0f, 0.0f);
		}
	}

	glm::vec3 sourceBitangent;
	const float handedness = TryNormalizeDirection(vertex.m_bitangent, sourceBitangent) &&
									 glm::dot(glm::cross(normal, tangent), sourceBitangent) < 0.0f
								 ? -1.0f
								 : 1.0f;

	glm::vec3 bitangent;
	if (!TryNormalizeDirection(glm::cross(normal, tangent), bitangent))
	{
		bitangent = glm::vec3(0.0f, 0.0f, 1.0f);
	}

	vertex.m_normal = normal;
	vertex.m_tangent = tangent;
	vertex.m_bitangent = bitangent * handedness;
}
