#include "Components/Tests/GlobalIlluminationLandscapeTestScene.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;
using namespace Sailor::GlobalIlluminationLandscapeTestScene;

namespace
{
	float BrushFalloff(
		float x,
		float z,
		const TVector<float>& stamps,
		size_t offset)
	{
		const float radius = (std::max)(stamps[offset + 2u], 0.001f);
		const float distance = glm::distance(
			glm::vec2(x, z),
			glm::vec2(stamps[offset], stamps[offset + 1u]));
		const float linear = glm::clamp(
			1.0f - distance / radius,
			0.0f,
			1.0f);
		return linear * linear * (3.0f - 2.0f * linear);
	}

	void AppendTriangle(
		TVector<Math::Triangle>& triangles,
		const glm::vec3& a,
		const glm::vec3& b,
		const glm::vec3& c,
		const glm::vec3& normal,
		uint8_t materialIndex)
	{
		Math::Triangle triangle{};
		triangle.m_vertices[0] = a;
		triangle.m_vertices[1] = b;
		triangle.m_vertices[2] = c;
		const glm::vec3 tangent = glm::normalize(b - a);
		const glm::vec3 bitangent = glm::normalize(
			glm::cross(normal, tangent));
		for (size_t vertex = 0u; vertex < 3u; ++vertex)
		{
			triangle.m_normals[vertex] = normal;
			triangle.m_tangent[vertex] = tangent;
			triangle.m_bitangent[vertex] = bitangent;
			triangle.m_uvs[vertex] = glm::vec2(
				triangle.m_vertices[vertex].x,
				triangle.m_vertices[vertex].z) *
				LandscapeTextureTiling;
			triangle.m_uvs2[vertex] = triangle.m_uvs[vertex];
			triangle.m_colors[vertex] = glm::vec4(
				1.0f,
				0.0f,
				0.0f,
				0.0f);
		}
		triangle.m_materialIndex = materialIndex;
		triangle.m_centroid = (a + b + c) / 3.0f;
		triangles.Add(std::move(triangle));
	}

	void AppendQuad(
		TVector<Math::Triangle>& triangles,
		const glm::vec3& a,
		const glm::vec3& b,
		const glm::vec3& c,
		const glm::vec3& d,
		const glm::vec3& normal,
		uint8_t materialIndex)
	{
		AppendTriangle(triangles, a, b, c, normal, materialIndex);
		AppendTriangle(triangles, a, c, d, normal, materialIndex);
	}

	void AppendBox(
		TVector<Math::Triangle>& triangles,
		const Box& box)
	{
		const glm::vec3 minimum = box.m_position - box.m_scale;
		const glm::vec3 maximum = box.m_position + box.m_scale;
		const uint8_t materialIndex = static_cast<uint8_t>(box.m_material);

		AppendQuad(triangles,
			{ maximum.x, minimum.y, minimum.z },
			{ maximum.x, maximum.y, minimum.z },
			{ maximum.x, maximum.y, maximum.z },
			{ maximum.x, minimum.y, maximum.z },
			{ 1.0f, 0.0f, 0.0f },
			materialIndex);
		AppendQuad(triangles,
			{ minimum.x, minimum.y, maximum.z },
			{ minimum.x, maximum.y, maximum.z },
			{ minimum.x, maximum.y, minimum.z },
			{ minimum.x, minimum.y, minimum.z },
			{ -1.0f, 0.0f, 0.0f },
			materialIndex);
		AppendQuad(triangles,
			{ minimum.x, maximum.y, maximum.z },
			{ maximum.x, maximum.y, maximum.z },
			{ maximum.x, maximum.y, minimum.z },
			{ minimum.x, maximum.y, minimum.z },
			{ 0.0f, 1.0f, 0.0f },
			materialIndex);
		AppendQuad(triangles,
			{ minimum.x, minimum.y, minimum.z },
			{ maximum.x, minimum.y, minimum.z },
			{ maximum.x, minimum.y, maximum.z },
			{ minimum.x, minimum.y, maximum.z },
			{ 0.0f, -1.0f, 0.0f },
			materialIndex);
		AppendQuad(triangles,
			{ maximum.x, minimum.y, maximum.z },
			{ maximum.x, maximum.y, maximum.z },
			{ minimum.x, maximum.y, maximum.z },
			{ minimum.x, minimum.y, maximum.z },
			{ 0.0f, 0.0f, 1.0f },
			materialIndex);
		AppendQuad(triangles,
			{ minimum.x, minimum.y, minimum.z },
			{ minimum.x, maximum.y, minimum.z },
			{ maximum.x, maximum.y, minimum.z },
			{ maximum.x, minimum.y, minimum.z },
			{ 0.0f, 0.0f, -1.0f },
			materialIndex);
	}
}

const TVector<Box>& GlobalIlluminationLandscapeTestScene::GetBoxes()
{
	static const TVector<Box> boxes{
		{
			"Evening Shadow Ridge",
			glm::vec3(1.5f, 1.0f, -7.0f),
			glm::vec3(11.0f, 5.0f, 1.25f),
			EMaterial::ShadowRidge,
			"Tests/Visual/GlobalIlluminationRidge.mat"
		},
		{
			"Sunlit Bounce Cliff",
			glm::vec3(14.0f, 4.0f, -2.0f),
			glm::vec3(1.25f, 8.0f, 11.0f),
			EMaterial::BounceCliff,
			"Tests/Visual/GlobalIlluminationBounceCliff.mat"
		},
		{
			"Shadow Valley Receiver",
			glm::vec3(3.5f, -1.5f, 1.0f),
			glm::vec3(2.5f, 2.5f, 3.5f),
			EMaterial::ShadowReceiver,
			"Tests/Visual/GlobalIlluminationReceiver.mat"
		},
		{
			"Shadow Valley Lintel",
			glm::vec3(3.5f, 3.5f, 1.0f),
			glm::vec3(4.5f, 0.6f, 4.2f),
			EMaterial::ShadowReceiver,
			"Tests/Visual/GlobalIlluminationReceiver.mat"
		}
	};
	return boxes;
}

TVector<float>
GlobalIlluminationLandscapeTestScene::GetLandscapeSculptStamps()
{
	// x, z, radius, strength, operation (0 = raise). The center stays a
	// readable valley while the outer stamps make the horizon unmistakably a
	// landscape rather than a flat test plane.
	return {
		-23.0f, -7.0f, 20.0f, 9.0f, 0.0f,
		23.0f, -9.0f, 21.0f, 8.0f, 0.0f,
		-5.0f, 26.0f, 27.0f, 6.0f, 0.0f,
		4.0f, -28.0f, 22.0f, 5.0f, 0.0f
	};
}

float GlobalIlluminationLandscapeTestScene::SampleLandscapeHeight(
	float x,
	float z)
{
	float height = 0.0f;
	const TVector<float> stamps = GetLandscapeSculptStamps();
	for (size_t offset = 0u; offset + 4u < stamps.Num(); offset += 5u)
	{
		const float falloff = BrushFalloff(x, z, stamps, offset);
		const float strength = stamps[offset + 3u] * falloff;
		const uint32_t operation = static_cast<uint32_t>(stamps[offset + 4u]);
		if (operation == 0u)
		{
			height += strength;
		}
		else if (operation == 1u)
		{
			height -= strength;
		}
		else
		{
			height = glm::mix(
				height,
				0.0f,
				glm::clamp(strength, 0.0f, 1.0f));
		}
	}
	return height + LandscapeWorldY;
}

void GlobalIlluminationLandscapeTestScene::BuildBakeTriangles(
	TVector<Math::Triangle>& outTriangles,
	Math::AABB& outBounds)
{
	outTriangles.Clear();
	outBounds = {};

	const uint32_t resolutionX =
		LandscapeChunksX * LandscapeChunkResolution;
	const uint32_t resolutionZ =
		LandscapeChunksZ * LandscapeChunkResolution;
	const float width = LandscapeChunksX * LandscapeChunkSize;
	const float depth = LandscapeChunksZ * LandscapeChunkSize;
	const float stepX = width / static_cast<float>(resolutionX);
	const float stepZ = depth / static_cast<float>(resolutionZ);
	for (uint32_t z = 0u; z < resolutionZ; ++z)
	{
		for (uint32_t x = 0u; x < resolutionX; ++x)
		{
			const float x0 = -width * 0.5f + x * stepX;
			const float x1 = x0 + stepX;
			const float z0 = -depth * 0.5f + z * stepZ;
			const float z1 = z0 + stepZ;
			const glm::vec3 a(x0, SampleLandscapeHeight(x0, z0), z0);
			const glm::vec3 b(x0, SampleLandscapeHeight(x0, z1), z1);
			const glm::vec3 c(x1, SampleLandscapeHeight(x1, z0), z0);
			const glm::vec3 d(x1, SampleLandscapeHeight(x1, z1), z1);
			const glm::vec3 normal0 = glm::normalize(glm::cross(b - a, c - a));
			const glm::vec3 normal1 = glm::normalize(glm::cross(b - c, d - c));
			AppendTriangle(
				outTriangles,
				a,
				b,
				c,
				normal0,
				static_cast<uint8_t>(EMaterial::Landscape));
			AppendTriangle(
				outTriangles,
				c,
				b,
				d,
				normal1,
				static_cast<uint8_t>(EMaterial::Landscape));
		}
	}

	for (const Box& box : GetBoxes())
	{
		AppendBox(outTriangles, box);
	}
	for (const Math::Triangle& triangle : outTriangles)
	{
		for (const glm::vec3& vertex : triangle.m_vertices)
		{
			outBounds.Extend(vertex);
		}
	}
}

glm::vec3 GlobalIlluminationLandscapeTestScene::GetEveningLightDirection()
{
	const float sunAngleRadians = glm::radians(EveningSunAngleDegrees);
	return glm::normalize(glm::vec3(
		0.2f,
		std::sin(-sunAngleRadians),
		std::cos(sunAngleRadians)));
}

glm::vec3 GlobalIlluminationLandscapeTestScene::GetEveningLightIntensity()
{
	return glm::vec3(6.5f, 3.2f, 1.35f);
}

glm::vec3 GlobalIlluminationLandscapeTestScene::GetReceiverEvidencePoint()
{
	return glm::vec3(3.5f, 1.05f, 1.0f);
}

glm::vec3 GlobalIlluminationLandscapeTestScene::GetBounceCliffEvidencePoint()
{
	return glm::vec3(12.70f, 9.0f, -2.0f);
}

glm::vec3 GlobalIlluminationLandscapeTestScene::GetCameraPosition()
{
	return glm::vec3(3.0f, 13.0f, 39.0f);
}

glm::vec3 GlobalIlluminationLandscapeTestScene::GetCameraTarget()
{
	return glm::vec3(3.0f, 1.75f, 0.0f);
}
