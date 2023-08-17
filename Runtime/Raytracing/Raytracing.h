#pragma once
#include "Core/Defines.h"
#include "Math/Bounds.h"
#include "Containers/Vector.h"
#include "Containers/Vector.h"
#include "Memory/SharedPtr.hpp"
#include "Tasks/Scheduler.h"

namespace Sailor
{
	class Raytracing
	{
	public:

		struct Texture2D
		{
			const int32_t BlockSize = 16;

			TVector<u8> m_data;
			int32_t m_width{};
			int32_t m_height{};

			template<typename T>
			void Initialize(u8* linearData)
			{
				SAILOR_PROFILE_FUNCTION();

				m_data.Resize(m_width * m_height * sizeof(T));

				u8* blockData = m_data.GetData();
				const int32_t blockWidth = (m_width + BlockSize - 1) / BlockSize;
				const int32_t blockHeight = (m_height + BlockSize - 1) / BlockSize;

				for (int32_t by = 0; by < blockHeight; ++by)
				{
					for (int32_t bx = 0; bx < blockWidth; ++bx)
					{
						u8* dst = blockData + ((by * blockWidth + bx) * BlockSize * BlockSize) * sizeof(T);

						for (int32_t y = 0; y < BlockSize; ++y)
						{
							int32_t srcY = by * BlockSize + y;

							if (srcY < m_height)
							{
								u8* src = linearData + srcY * m_width * sizeof(T);

								if (bx * BlockSize + BlockSize - 1 < m_width)
								{
									std::memcpy(dst, src + bx * BlockSize * sizeof(T), BlockSize * sizeof(T));
									dst += BlockSize * sizeof(T);
								}
								else
								{
									for (int32_t x = 0; x < BlockSize; ++x)
									{
										int32_t srcX = bx * BlockSize + x;
										if (srcX < m_width)
										{
											T* srcElem = (T*)(src + srcX * sizeof(T));
											T* dstElem = (T*)dst;
											*dstElem = *srcElem;
											dst += sizeof(T);
										}
									}
								}
							}
						}
					}
				}
			}

			template<typename T>
			const T Sample(vec2 uv) const
			{
				SAILOR_PROFILE_FUNCTION();

				float fx = fmodf(uv.x * m_width, (float)m_width);
				float fy = fmodf(uv.y * m_height, (float)m_height);

				if (fx < 0.0f)
				{
					fx = m_width + fx;
				}

				if (fy < 0.0f)
				{
					fy = m_height + fy;
				}

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