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

	class Raytracing
	{
	public:

		struct Texture2D
		{
			const int32_t BlockSize = 8;

			TVector<u8> m_data;
			int32_t m_width{};
			int32_t m_height{};

			template<typename T>
			void Initialize(u8* linearData)
			{
				m_data.Resize(m_width * m_height * sizeof(T));

				u8* blockData = m_data.GetData();
				const int32_t blockWidth = (m_width + BlockSize - 1) / BlockSize;
				const int32_t blockHeight = (m_height + BlockSize - 1) / BlockSize;

				for (int32_t by = 0; by < blockHeight; ++by)
				{
					for (int32_t bx = 0; bx < blockWidth; ++bx)
					{
						for (int32_t y = 0; y < BlockSize; ++y)
						{
							for (int32_t x = 0; x < BlockSize; ++x)
							{
								int32_t srcX = bx * BlockSize + x;
								int32_t srcY = by * BlockSize + y;
								if (srcX < m_width && srcY < m_height)
								{
									T* src = (T*)(linearData + (srcY * m_width + srcX) * sizeof(T));
									T* dst = (T*)(blockData + ((by * blockWidth + bx) * BlockSize * BlockSize + y * BlockSize + x) * sizeof(T));
									*dst = *src;
								}
							}
						}
					}
				}
			}

			template<typename T>
			const T Sample(vec2 uv) const
			{
				float fx = uv.x * m_width;
				float fy = uv.y * m_height;

				int32_t tX = static_cast<int32_t>(floor(fx));
				int32_t tY = static_cast<int32_t>(floor(fy));

				const float fracX = fx - tX;
				const float fracY = fy - tY;

				int32_t blockWidth = (m_width + BlockSize - 1) / BlockSize;
				int32_t blockIndex = (tY / BlockSize) * blockWidth + (tX / BlockSize);
				int32_t inBlockX = tX % BlockSize;
				int32_t inBlockY = tY % BlockSize;

				const T* blockStart = (T*)(m_data.GetData() + blockIndex * BlockSize * BlockSize * sizeof(T));
				const T* topLeft = blockStart + (inBlockY * BlockSize + inBlockX);
				const T* topRight = (inBlockX + 1 < BlockSize && tX + 1 < m_width) ? topLeft + 1 : topLeft;
				const T* bottomLeft = (inBlockY + 1 < BlockSize && tY + 1 < m_height) ? topLeft + BlockSize : topLeft;
				const T* bottomRight = (inBlockX + 1 < BlockSize && inBlockY + 1 < BlockSize && tX + 1 < m_width && tY + 1 < m_height) ? bottomLeft + 1 : bottomLeft;

				T interpolatedTop = glm::mix(*topLeft, *topRight, fracX);
				T interpolatedBottom = glm::mix(*bottomLeft, *bottomRight, fracX);

				return glm::mix(interpolatedTop, interpolatedBottom, fracY);
			}

			Texture2D() = default;
			Texture2D(Texture2D&) = delete;
			Texture2D& operator=(Texture2D&) = delete;

			~Texture2D();
		};

		struct Material
		{
			u8 m_diffuseIndex = -1;
			u8 m_ambientIndex = -1;
			u8 m_specularIndex = -1;
			u8 m_emissionIndex = -1;
			u8 m_normalIndex = -1;

			glm::vec4 m_diffuse{};
			glm::vec4 m_ambient{};
			glm::vec4 m_emission{};
			glm::vec4 m_specular{};
		};

		void Run();

	protected:

		TVector<Math::Triangle> m_triangles{};
		TVector<Material> m_materials{};
		TVector<TSharedPtr<Texture2D>> m_textures{};
		TMap<std::string, uint32_t> m_textureMapping{};

		Tasks::TaskPtr<TSharedPtr<Texture2D>> LoadTexture(const char* file);
	};
}