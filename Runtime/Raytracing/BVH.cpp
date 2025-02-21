#include "BVH.h"
#include "Tasks/Scheduler.h"
#include "Containers/Vector.h"
#include "Core/LogMacros.h"
#include "Core/Utils.h"
#include "Math/Math.h"
#include "Math/Bounds.h"
#include "Core/StringHash.h"

using namespace Sailor;
using namespace Sailor::Math;
using namespace Sailor::Raytracing;

float BVH::FindBestSplitPlane(const BVHNode& node, const TVector<Math::Triangle>& tris, int32_t& outAxis, float& outSplitPos) const
{
	SAILOR_PROFILE_FUNCTION();

	struct Bin { Math::AABB m_bounds{}; int m_triCount = 0; };

	const uint32_t NumBins = 8;

	float bestCost = std::numeric_limits<float>::max();
	for (uint32_t a = 0; a < 3; a++)
	{
		float boundsMin = std::numeric_limits<float>::max();
		float boundsMax = -30000000.0f;

		for (uint32_t i = 0; i < node.m_triCount; i++)
		{
			const Math::Triangle& triangle = tris[m_triIdx[node.m_leftFirst + i]];
			boundsMin = std::min(boundsMin, triangle.m_centroid[a]);
			boundsMax = std::max(boundsMax, triangle.m_centroid[a]);
		}

		if (boundsMin == boundsMax)
		{
			continue;
		}

		Bin bin[NumBins];
		float scale = NumBins / (boundsMax - boundsMin);
		for (uint i = 0; i < node.m_triCount; i++)
		{
			const Math::Triangle& triangle = tris[m_triIdx[node.m_leftFirst + i]];
			int32_t binIdx = std::min((int32_t)NumBins - 1,
				(int32_t)((triangle.m_centroid[a] - boundsMin) * scale));
			bin[binIdx].m_triCount++;
			bin[binIdx].m_bounds.Extend(triangle.m_vertices[0]);
			bin[binIdx].m_bounds.Extend(triangle.m_vertices[1]);
			bin[binIdx].m_bounds.Extend(triangle.m_vertices[2]);
		}

		float leftArea[NumBins - 1];
		float rightArea[NumBins - 1];
		int32_t leftCount[NumBins - 1];
		int32_t rightCount[NumBins - 1];

		Math::AABB leftBox;
		Math::AABB rightBox;
		int32_t leftSum = 0;
		int32_t rightSum = 0;
		for (int32_t i = 0; i < NumBins - 1; i++)
		{
			leftSum += bin[i].m_triCount;
			leftCount[i] = leftSum;
			leftBox.Extend(bin[i].m_bounds);
			leftArea[i] = leftBox.Area();
			rightSum += bin[NumBins - 1 - i].m_triCount;
			rightCount[NumBins - 2 - i] = rightSum;
			rightBox.Extend(bin[NumBins - 1 - i].m_bounds);
			rightArea[NumBins - 2 - i] = rightBox.Area();
		}

		scale = (boundsMax - boundsMin) / NumBins;
		for (int32_t i = 0; i < NumBins - 1; i++)
		{
			float planeCost = leftCount[i] * leftArea[i] + rightCount[i] * rightArea[i];
			if (planeCost < bestCost)
			{
				outAxis = a;
				outSplitPos = boundsMin + scale * (i + 1);
				bestCost = planeCost;
			}
		}
	}
	return bestCost;
}

float BVH::EvaluateSAH(const BVHNode& node, const TVector<Math::Triangle>& tris, int32_t axis, float pos) const
{
	SAILOR_PROFILE_FUNCTION();

	Math::AABB leftBox;
	Math::AABB rightBox;

	int32_t leftCount = 0;
	int32_t rightCount = 0;

	for (uint i = 0; i < node.m_triCount; i++)
	{
		const Math::Triangle& triangle = tris[m_triIdx[node.m_leftFirst + i]];
		if (triangle.m_centroid[axis] < pos)
		{
			leftCount++;
			leftBox.Extend(triangle.m_vertices[0]);
			leftBox.Extend(triangle.m_vertices[1]);
			leftBox.Extend(triangle.m_vertices[2]);
		}
		else
		{
			rightCount++;
			rightBox.Extend(triangle.m_vertices[0]);
			rightBox.Extend(triangle.m_vertices[1]);
			rightBox.Extend(triangle.m_vertices[2]);
		}
	}
	float cost = leftCount * leftBox.Area() + rightCount * rightBox.Area();
	return cost > 0 ? cost : 1e30f;
}

bool BVH::IntersectBVH(const Math::Ray& ray, Math::RaycastHit& outResult, const uint nodeIdx, float maxRayLength, uint32_t ignoreTriangle) const
{
	SAILOR_PROFILE_FUNCTION();

	const BVHNode* node = &m_nodes[m_rootNodeIdx], * stack[64];
	uint stackPtr = 0;
	Math::RaycastHit res{};
	while (1)
	{
		if (node->IsLeaf())
		{
			for (uint i = 0; i < node->m_triCount; i++)
			{
				const uint32_t triangleIndex = m_triIdxMapping[node->m_leftFirst + i];

				if (ignoreTriangle != triangleIndex && Math::IntersectRayTriangle(ray, m_triangles[node->m_leftFirst + i], res, maxRayLength))
				{
					outResult = res;
					outResult.m_triangleIndex = triangleIndex;

					maxRayLength = std::min(maxRayLength, res.m_rayLenght);
				}
			}
			if (stackPtr == 0)
			{
				break;
			}
			else
			{
				node = stack[--stackPtr];
			}

			continue;
		}

		const BVH::BVHNode* child1 = &m_nodes[node->m_leftFirst];
		const BVH::BVHNode* child2 = &m_nodes[node->m_leftFirst + 1];

		float dist1 = IntersectRayAABB(ray, child1->m_aabbMin, child1->m_aabbMax, maxRayLength);
		float dist2 = IntersectRayAABB(ray, child2->m_aabbMin, child2->m_aabbMax, maxRayLength);

		if (dist1 > dist2)
		{
			std::swap(dist1, dist2);
			std::swap(child1, child2);
		}

		if (dist1 == std::numeric_limits<float>::max())
		{
			if (stackPtr == 0)
			{
				break;
			}
			else
			{
				node = stack[--stackPtr];
			}
		}
		else
		{
			node = child1;
			if (dist2 != std::numeric_limits<float>::max())
			{
				stack[stackPtr++] = child2;
			}
		}
	}

	return outResult.HasIntersection();
}

void BVH::UpdateNodeBounds(uint32_t nodeIdx, const TVector<Math::Triangle>& tris)
{
	SAILOR_PROFILE_FUNCTION();

	BVH::BVHNode& node = m_nodes[nodeIdx];

	node.m_aabbMin = vec3(1e30f);
	node.m_aabbMax = vec3(-1e30f);

	for (uint first = node.m_leftFirst, i = 0; i < node.m_triCount; i++)
	{
		uint leafTriIdx = m_triIdx[first + i];
		const Triangle& leafTri = tris[leafTriIdx];
		node.m_aabbMin = glm::min(node.m_aabbMin, leafTri.m_vertices[0]);
		node.m_aabbMin = glm::min(node.m_aabbMin, leafTri.m_vertices[1]);
		node.m_aabbMin = glm::min(node.m_aabbMin, leafTri.m_vertices[2]);
		node.m_aabbMax = glm::max(node.m_aabbMax, leafTri.m_vertices[0]);
		node.m_aabbMax = glm::max(node.m_aabbMax, leafTri.m_vertices[1]);
		node.m_aabbMax = glm::max(node.m_aabbMax, leafTri.m_vertices[2]);
	}
}

void BVH::Subdivide(uint32_t nodeIdx, const TVector<Math::Triangle>& tris)
{
	SAILOR_PROFILE_FUNCTION();

	// terminate recursion
	BVH::BVHNode& node = m_nodes[nodeIdx];

	if (node.m_triCount <= 4)
	{
		return;
	}

	int32_t axis{};
	float splitPos{};
	float splitCost = FindBestSplitPlane(node, tris, axis, splitPos);

	float nosplitCost = node.CalculateCost();
	if (splitCost >= nosplitCost)
	{
		return;
	}

	// in-place partition
	int32_t i = node.m_leftFirst;
	int32_t j = i + node.m_triCount - 1;

	while (i <= j)
	{
		if (tris[m_triIdx[i]].m_centroid[axis] < splitPos)
		{
			i++;
		}
		else
		{
			std::swap(m_triIdx[i], m_triIdx[j--]);
		}
	}

	// abort split if one of the sides is empty
	uint32_t leftCount = i - node.m_leftFirst;

	if (leftCount == 0 || leftCount == node.m_triCount)
	{
		return;
	}

	// create child nodes
	uint32_t leftChildIdx = m_nodesUsed++;
	uint32_t rightChildIdx = m_nodesUsed++;

	m_nodes[leftChildIdx].m_leftFirst = node.m_leftFirst;
	m_nodes[leftChildIdx].m_triCount = leftCount;
	m_nodes[rightChildIdx].m_leftFirst = i;
	m_nodes[rightChildIdx].m_triCount = node.m_triCount - leftCount;

	node.m_leftFirst = leftChildIdx;
	node.m_triCount = 0;

	UpdateNodeBounds(leftChildIdx, tris);
	UpdateNodeBounds(rightChildIdx, tris);

	Subdivide(leftChildIdx, tris);
	Subdivide(rightChildIdx, tris);
}

void BVH::BuildBVH(const TVector<Math::Triangle>& tris)
{
	SAILOR_PROFILE_FUNCTION();

	check(tris.Num() * 2 - 1 == m_nodes.Num());

	for (uint32_t i = 0; i < m_nodes.Num(); i++)
	{
		m_triIdx[i] = i;
	}

	BVHNode& root = m_nodes[m_rootNodeIdx];
	root.m_leftFirst = 0;
	root.m_triCount = (uint32_t)tris.Num();

	UpdateNodeBounds(m_rootNodeIdx, tris);
	Subdivide(m_rootNodeIdx, tris);

	{
		SAILOR_PROFILE_SCOPE("Copy/Locality triangle data");
		// Cache locality
		m_triangles.Reserve(tris.Num());
		m_triIdxMapping.AddDefault(tris.Num());

		// TODO: Parallelize
		for (uint32_t i = 0; i < m_nodes.Num(); i++)
		{
			if (m_nodes[i].IsLeaf())
			{
				const uint32_t triIndex = m_nodes[i].m_leftFirst;
				m_nodes[i].m_leftFirst = (int32_t)m_triangles.Num();

				TVector<uint32_t> sorted(m_nodes[i].m_triCount);

				{
					SAILOR_PROFILE_SCOPE("Sort Triangles by area");
					for (uint32_t j = 0; j < m_nodes[i].m_triCount; j++)
					{
						sorted[j] = m_triIdx[triIndex + j];
					}

					sorted.Sort([&](const auto& lhs, const auto& rhs)
						{
							return tris[lhs].SquareArea() > tris[rhs].SquareArea();
						});
				}

				{
					SAILOR_PROFILE_SCOPE("Copy data");
					for (uint32_t j = 0; j < m_nodes[i].m_triCount; j++)
					{
						const uint32_t triId = sorted[j];// m_triIdx[triIndex + j];
						m_triIdxMapping[m_triangles.Num()] = triId;
						m_triangles.Add(tris[triId]);
					}
				}
			}
		}
	}
}