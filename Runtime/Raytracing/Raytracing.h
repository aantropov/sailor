#pragma once
#include "Core/Defines.h"
#include "Math/Bounds.h"
#include "Containers/Vector.h"
#include "Containers/Vector.h"
#include "Memory/SharedPtr.hpp"
#include "Tasks/Scheduler.h"

namespace Sailor
{
	class BVH
	{
		struct BVHNode //40 bytes
		{
			vec3 m_aabbMin{};
			vec3 m_aabbMax{};
			uint32_t m_leftNode{};
			uint32_t m_firstTriIdx{};
			uint32_t m_triCount{};

			bool IsLeaf() const { return m_triCount > 0; }
		};

	public:

		BVH(uint32_t numTriangles)
		{
			const uint32_t N = 2 * numTriangles - 1;
			m_nodes.AddDefault(N);
			m_triIdx.AddDefault(N);
		}
		
		void BuildBVH(const TVector<Math::Triangle>& tris);
		bool IntersectBVH(const Math::Ray& ray, const TVector<Math::Triangle>& tris, Math::RaycastHit& outResult, const uint nodeIdx, float maxRayLength = 10e30) const;

	protected:

		void UpdateNodeBounds(uint32_t nodeIdx, const TVector<Math::Triangle>& tris);
		void Subdivide(uint32_t nodeIdx, const TVector<Math::Triangle>& tris);
		
		TVector<BVHNode> m_nodes;
		TVector<uint32_t> m_triIdx;

		uint32_t m_rootNodeIdx = 0;
		uint32_t m_nodesUsed = 1;
	};

	class SAILOR_API Raytracing
	{

	public:

		struct Material
		{
			u8 m_albedoIndex = -1;
		};

		template<typename T>
		struct Texture2D
		{
			TVector<T> m_data{};
			uint32_t m_width{};
			uint32_t m_height{};

			const T& Sample(vec2 uv) const
			{
				uint32_t tX = floor(uv.x * m_width);
				uint32_t tY = floor(uv.y * m_height);

				return m_data[tY.y * m_width + tX];
			}
		};

		void Run();

	protected:

		TVector<Math::Triangle> m_triangles{};
		TVector<Material> m_materials{};

		Tasks::TaskPtr<TSharedPtr<Texture2D<u8>>> LoadTexture();

		TVector<TSharedPtr<Texture2D<u8>>> m_textures;
	};
}