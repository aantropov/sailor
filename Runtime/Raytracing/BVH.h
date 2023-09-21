#pragma once
#include "Core/Defines.h"
#include "Math/Bounds.h"
#include "Containers/Vector.h"

using namespace Sailor;

namespace Sailor::Raytracing
{
	//Inspired by https://jacco.ompf2.com/2022/04/13/how-to-build-a-bvh-part-1-basics/
	class BVH
	{
		struct BVHNode //40 bytes
		{
			union
			{
				struct { vec3 m_aabbMin; uint m_leftFirst; };
				__m128 m_aabbMin4;
			};
			union
			{
				struct { vec3 m_aabbMax; uint m_triCount; };
				__m128 m_aabbMax4;
			};

			bool IsLeaf() const { return m_triCount > 0; }

			float CalculateCost() const
			{
				const vec3 e = m_aabbMax - m_aabbMin;
				float surfaceArea = 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
				return m_triCount * surfaceArea;
			}
		};

	public:

		BVH(uint32_t numTriangles)
		{
			const uint32_t N = 2 * numTriangles - 1;
			m_nodes.AddDefault(N);
			m_triIdx.AddDefault(N);
		}

		void BuildBVH(const TVector<Math::Triangle>& tris);
		bool IntersectBVH(const Math::Ray& ray, Math::RaycastHit& outResult, const uint nodeIdx, float maxRayLength = std::numeric_limits<float>::max(), uint32_t ignoreTriangle = (uint32_t)(-1)) const;

	protected:

		void UpdateNodeBounds(uint32_t nodeIdx, const TVector<Math::Triangle>& tris);
		void Subdivide(uint32_t nodeIdx, const TVector<Math::Triangle>& tris);
		float EvaluateSAH(const BVHNode& node, const TVector<Math::Triangle>& tris, int32_t axis, float pos) const;
		float FindBestSplitPlane(const BVHNode& node, const TVector<Math::Triangle>& tris, int32_t& outAxis, float& outSplitPos) const;

		TVector<BVHNode> m_nodes;
		TVector<uint32_t> m_triIdx;
		TVector<Math::Triangle> m_triangles;
		TVector<uint32_t> m_triIdxMapping;

		uint32_t m_rootNodeIdx = 0;
		uint32_t m_nodesUsed = 1;
	};
}