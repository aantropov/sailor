#pragma once
#include "Core/Defines.h"
#include "Math/Bounds.h"
#include "Containers/Vector.h"
#include "Containers/Vector.h"
#include "Memory/SharedPtr.hpp"
#include "Tasks/Scheduler.h"
#include "BVH.h"
#include <filesystem>

namespace Sailor
{
	class Raytracing
	{
		const float VariableShaderRate = 0.005f;

	public:

		struct Params
		{
			std::filesystem::path m_pathToModel;
			std::filesystem::path m_output;
			uint32_t m_height;
			uint32_t m_numSamples;
			uint32_t m_numBounces;
		};

		static void ParseParamsFromCommandLineArgs(Params& params, const char** args, int32_t num);

		struct Texture2D
		{
			const uint32_t BlockSize = 16;

			TVector<u8> m_data;
			int32_t m_width{};
			int32_t m_height{};
			bool m_bUseBlockEncoding = false;

			template<typename TOutputData, typename TInputData>
			void Initialize(TInputData* data, bool bConvertToLinear, bool useBlockEncoding = false, bool bNormalMap = false)
			{
				m_bUseBlockEncoding = useBlockEncoding;
				SAILOR_PROFILE_FUNCTION();

				m_data.Resize(m_width * m_height * sizeof(TOutputData));

				if (useBlockEncoding)
				{
					for (uint32_t blockY = 0; blockY < (uint32_t)m_height; blockY += BlockSize)
					{
						for (uint32_t blockX = 0; blockX < (uint32_t)m_width; blockX += BlockSize)
						{
							for (uint32_t y = blockY; y < blockY + BlockSize && y < (uint32_t)m_height; ++y)
							{
								for (uint32_t x = blockX; x < blockX + BlockSize && x < (uint32_t)m_width; ++x)
								{
									uint32_t idx = y * m_width + x;

									TOutputData* dst = (TOutputData*)(m_data.GetData() + sizeof(TOutputData) * idx);
									TInputData* src = data + idx;

									if (bNormalMap)
									{
										*dst = (TOutputData(*src) * (1.0f / 127.5f)) - 1.0f;

									}
									else
									{
										*dst = bConvertToLinear ?
											((TOutputData)Utils::SRGBToLinear(TOutputData(*src) * (1.0f / 255.0f))) :
											(TOutputData(*src) * (1.0f / 255.0f));
									}
								}
							}
						}
					}
				}
				else
				{
					for (uint32_t i = 0; i < (uint32_t)m_width * m_height; i++)
					{
						TOutputData* dst = (TOutputData*)(m_data.GetData() + sizeof(TOutputData) * i);
						TInputData* src = data + i;

						if (bNormalMap)
						{
							*dst = (TOutputData(*src) * (1.0f / 127.5f)) - 1.0f;
						}
						else
						{
							*dst = bConvertToLinear ?
								(TOutputData)Utils::SRGBToLinear(TOutputData(*src) * (1.0f / 255.0f)) :
								(TOutputData(*src) * (1.0f / 255.0f));
						}
					}
				}
			}

			template<typename T>
			const T Sample(const vec2& uv) const
			{
				SAILOR_PROFILE_FUNCTION();

				vec2 wrappedUV;
				wrappedUV.x = uv.x - std::floor(uv.x);
				wrappedUV.y = uv.y - std::floor(uv.y);

				// Convert UV to pixel space once, and compute the required values.
				const float fx = wrappedUV.x * (m_width - 1);
				const float fy = wrappedUV.y * (m_height - 1);

				const int32_t tX0 = static_cast<int32_t>(fx);
				const int32_t tY0 = static_cast<int32_t>(fy);
				const int32_t tX1 = std::min(tX0 + 1, m_width - 1);
				const int32_t tY1 = std::min(tY0 + 1, m_height - 1);

				// Compute the fractional parts
				const float fracX = fx - tX0;
				const float fracY = fy - tY0;

				T* topLeft;
				T* topRight;
				T* bottomLeft;
				T* bottomRight;

				if (m_bUseBlockEncoding)
				{
					const uint32_t BlockSize = 16;

					const int32_t blockBaseIdxX0 = (tX0 / BlockSize) * BlockSize;
					const int32_t blockBaseIdxY0 = (tY0 / BlockSize) * m_width * BlockSize;
					const int32_t blockBaseIdxX1 = (tX1 / BlockSize) * BlockSize;
					const int32_t blockBaseIdxY1 = (tY1 / BlockSize) * m_width * BlockSize;

					topLeft = (T*)m_data.GetData() + blockBaseIdxY0 + blockBaseIdxX0 + (tY0 % BlockSize) * m_width + tX0 % BlockSize;
					topRight = (T*)m_data.GetData() + blockBaseIdxY0 + blockBaseIdxX1 + (tY0 % BlockSize) * m_width + tX1 % BlockSize;
					bottomLeft = (T*)m_data.GetData() + blockBaseIdxY1 + blockBaseIdxX0 + (tY1 % BlockSize) * m_width + tX0 % BlockSize;
					bottomRight = (T*)m_data.GetData() + blockBaseIdxY1 + blockBaseIdxX1 + (tY1 % BlockSize) * m_width + tX1 % BlockSize;
				}
				else
				{
					topLeft = (T*)m_data.GetData() + tX0 + tY0 * m_width;
					topRight = (T*)m_data.GetData() + tX1 + tY0 * m_width;
					bottomLeft = (T*)m_data.GetData() + tX0 + tY1 * m_width;
					bottomRight = (T*)m_data.GetData() + tX1 + tY1 * m_width;
				}

				// Bilinear interpolation using direct memory access
				const T topMix = *topLeft + fracX * (*topRight - *topLeft);
				const T bottomMix = *bottomLeft + fracX * (*bottomRight - *bottomLeft);
				const T finalSample = topMix + fracY * (bottomMix - topMix);

				return finalSample;
			}

			Texture2D() = default;
			Texture2D(Texture2D&) = delete;
			Texture2D& operator=(Texture2D&) = delete;
		};

		struct SampledData
		{
			glm::vec4 m_baseColor{};
			glm::vec3 m_orm{};
			glm::vec3 m_emissive{};
			glm::vec3 m_normal{};
		};

		struct Material
		{
			glm::vec4 m_baseColorFactor = vec4(1, 1, 1, 1);
			glm::vec3 m_emissiveFactor = vec3(0, 0, 0);

			glm::vec3 m_specularColorFactor = vec3(1, 1, 1);

			float m_metallicFactor = 1.0f;
			float m_roughnessFactor = 1.0f;
			float m_indexOfRefraction = 1.5f;
			float m_occlusionFactor = 1;
			float m_transmissionFactor = 0;
			float m_specularFactor = 1;

			bool HasEmissiveTexture() const { return m_emissiveIndex != u8(-1); }
			bool HasBaseTexture() const { return m_baseColorIndex != u8(-1); }
			bool HasAmbientTexture() const { return m_ambientIndex != u8(-1); }
			bool HasNormalTexture() const { return m_normalIndex != u8(-1); }
			bool HasMetallicRoughnessTexture() const { return m_metallicRoughnessIndex != u8(-1); }
			bool HasSpecularTexture() const { return m_specularColorIndex != u8(-1); }
			bool HasOcclusionTexture() const { return m_occlusionIndex != u8(-1); }
			bool HasTransmissionTexture() const { return m_transmissionIndex != u8(-1); }

			u8 m_baseColorIndex = -1;
			u8 m_ambientIndex = -1;
			u8 m_specularIndex = -1;
			u8 m_emissiveIndex = -1;
			u8 m_normalIndex = -1;
			u8 m_metallicRoughnessIndex = -1; // already in linear
			u8 m_occlusionIndex = -1;
			u8 m_transmissionIndex = -1;
			u8 m_specularColorIndex = -1;
		};

		void Run(const Raytracing::Params& params);

	protected:

		__forceinline Raytracing::SampledData GetSampledData(const size_t& materialIndex, glm::vec2 uv) const;

		vec3 CalculatePBR(const vec3& viewDirection, const vec3& worldNormal, const vec3& lightDirection, const SampledData& sample) const;
		vec3 Raytrace(const Math::Ray& r, const BVH& bvh, uint32_t bounceLimit, uint32_t ignoreTriangle) const;

		TVector<Math::Triangle> m_triangles{};
		TVector<Material> m_materials{};
		TVector<TSharedPtr<Texture2D>> m_textures{};
		TMap<std::string, uint32_t> m_textureMapping{};

		Tasks::TaskPtr<TSharedPtr<Texture2D>> LoadTexture(const char* file);
	};
}