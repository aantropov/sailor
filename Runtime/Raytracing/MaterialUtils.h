#pragma once
#include "Core/Defines.h"
#include "Containers/Vector.h"
#include "Tasks/Scheduler.h"

#include "Math/Math.h"
#include "Math/Bounds.h"

#include "stb/stb_image.h"
#include "assimp/scene.h"

#include <filesystem>

using namespace Sailor;

namespace Sailor::Raytracing
{
	enum class SamplerClamping
	{
		Clamp = 0,
		Repeat
	};

	struct Texture2D
	{
		SamplerClamping m_clamping = SamplerClamping::Clamp;

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

			vec2 wrappedUV{};

			switch (m_clamping)
			{
			case SamplerClamping::Clamp:
				wrappedUV.x = std::clamp(uv.x, 0.0f, 1.0f);
				wrappedUV.y = std::clamp(uv.y, 0.0f, 1.0f);
				break;

			case SamplerClamping::Repeat:
				wrappedUV.x = uv.x - std::floor(uv.x);
				wrappedUV.y = uv.y - std::floor(uv.y);
				break;
			}

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

	enum BlendMode : uint8_t
	{
		Opaque = 0,
		Blend,
		Mask
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
		float m_alphaCutoff = 0.5f;

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

		BlendMode m_blendMode = BlendMode::Opaque;
	};

	SAILOR_API uint PackVec3ToByte(vec3 v);
	SAILOR_API vec3 UnpackByteToVec3(uint byte);

	SAILOR_API void ProcessNode_Assimp(TVector<Math::Triangle>& outScene, aiNode* node, const aiScene* scene, const glm::mat4& matrix);
	SAILOR_API void ProcessMesh_Assimp(aiMesh* mesh, TVector<Math::Triangle>& outScene, const aiScene* scene, const glm::mat4& matrix);

	SAILOR_API mat4 GetWorldTransformMatrix(const aiScene* scene, const char* name);

	SAILOR_API void GenerateTangentBitangent(vec3& outTangent, vec3& outBitangent, const vec3* vert, const vec2* uv);

	template<typename T>
	Tasks::ITaskPtr LoadTexture_Task(TVector<TSharedPtr<Texture2D>>& m_textures,
		const std::filesystem::path& sceneFile,
		const aiScene* scene,
		uint32_t textureIndex,
		const std::string& filename,
		aiTextureMapMode clamping,
		bool bConvertToLinear,
		bool bNormalMap = false)
	{
		auto ptr = m_textures[textureIndex] = TSharedPtr<Texture2D>::Make();
		ptr->m_clamping = clamping == aiTextureMapMode::aiTextureMapMode_Wrap ? SamplerClamping::Repeat : SamplerClamping::Clamp;

		Tasks::ITaskPtr task = Tasks::CreateTask("Load Texture",
			[
				scene = scene,
					pTexture = ptr,
					sceneFile = sceneFile,
					fileName = filename,
					bConvertToLinear = bConvertToLinear,
					bNormalMap = bNormalMap
			]() mutable
			{
				int32_t texChannels = 0;
				void* pixels = nullptr;

				if (fileName[0] == '*')
				{
					const uint32 texIndex = atoi(&fileName[1]);
					aiTexture* pAITexture = scene->mTextures[texIndex];

					if (stbi_is_hdr_from_memory((stbi_uc*)pAITexture->pcData, pAITexture->mWidth))
					{
						pixels = stbi_loadf_from_memory((stbi_uc*)pAITexture->pcData, pAITexture->mWidth,
							&pTexture->m_width,
							&pTexture->m_height,
							&texChannels, STBI_rgb_alpha);

						pTexture->Initialize<T, vec4>((vec4*)pixels, bConvertToLinear, false, bNormalMap);
					}
					else
					{
						pixels = stbi_load_from_memory((stbi_uc*)pAITexture->pcData, pAITexture->mWidth,
							&pTexture->m_width,
							&pTexture->m_height,
							&texChannels, STBI_rgb_alpha);
						pTexture->Initialize<T, u8vec4>((u8vec4*)pixels, bConvertToLinear, false, bNormalMap);
					}
				}
				else
				{
					sceneFile.replace_filename(fileName);
					if (stbi_is_hdr(sceneFile.string().c_str()))
					{
						pixels = stbi_loadf(sceneFile.string().c_str(),
							&pTexture->m_width,
							&pTexture->m_height,
							&texChannels, STBI_rgb_alpha);

						pTexture->Initialize<T, vec4>((vec4*)pixels, bConvertToLinear, false, bNormalMap);
					}
					else
					{
						pixels = stbi_load(sceneFile.string().c_str(),
							&pTexture->m_width,
							&pTexture->m_height,
							&texChannels, STBI_rgb_alpha);

						pTexture->Initialize<T, u8vec4>((u8vec4*)pixels, bConvertToLinear, false, bNormalMap);
					}
				}

				if (pixels)
				{
					stbi_image_free(pixels);
				}
			})->Run();

			return task;
	};
}