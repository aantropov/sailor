#pragma once
#include "Core/Defines.h"
#include "Containers/Vector.h"
#include "Tasks/Scheduler.h"

#include "Math/Math.h"
#include "Math/Bounds.h"

#include <stb_image.h>
#include <tiny_gltf.h>

#include <filesystem>

using namespace Sailor;

namespace Sailor::Raytracing
{
	enum class SamplerClamping : uint8_t
	{
		Clamp = 0,
		Repeat
	};

	struct CombinedSampler2D
	{
		uint8_t m_channels = 3;
		SamplerClamping m_clamping = SamplerClamping::Clamp;

		int32_t m_width{};
		int32_t m_height{};
		TVector<u8> m_data;

		template<typename TOutputData>
		void Initialize(uint32_t width, uint32_t height, uint8_t channels = 3, SamplerClamping clamping = SamplerClamping::Clamp)
		{
			m_width = width;
			m_height = height;
			m_data.Resize(width * height * sizeof(TOutputData));
			m_channels = channels;
			m_clamping = clamping;
		}

		template<typename TOutputData, typename TInputData>
		void Initialize(TInputData* data, bool bConvertToLinear, bool bNormalMap = false)
		{
			SAILOR_PROFILE_FUNCTION();

			m_data.Resize(m_width * m_height * sizeof(TOutputData));

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

		template<typename T>
		__forceinline void SetPixel(uint32_t x, uint32_t y, const T& value)
		{
			auto ptr = reinterpret_cast<T*>(m_data.GetData());

			ptr[x + y * m_width] = value;
		}

		template<typename T>
		const T Sample(const vec2& uv) const
		{
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

			topLeft = (T*)m_data.GetData() + tX0 + tY0 * m_width;
			topRight = (T*)m_data.GetData() + tX1 + tY0 * m_width;
			bottomLeft = (T*)m_data.GetData() + tX0 + tY1 * m_width;
			bottomRight = (T*)m_data.GetData() + tX1 + tY1 * m_width;

			// Bilinear interpolation using direct memory access
			const T topMix = *topLeft + fracX * (*topRight - *topLeft);
			const T bottomMix = *bottomLeft + fracX * (*bottomRight - *bottomLeft);
			const T finalSample = topMix + fracY * (bottomMix - topMix);

			return finalSample;
		}

		CombinedSampler2D() = default;
		CombinedSampler2D(CombinedSampler2D&) = delete;
		CombinedSampler2D& operator=(CombinedSampler2D&) = delete;
	};

	enum BlendMode : uint8_t
	{
		Opaque = 0,
		Blend,
		Mask
	};

	struct Material
	{
		glm::mat3 m_uvTransform = mat3(1);

		glm::vec4 m_baseColorFactor = vec4(1, 1, 1, 1);
		glm::vec3 m_emissiveFactor = vec3(0, 0, 0);
		glm::vec3 m_specularColorFactor = vec3(1, 1, 1);
		glm::vec3 m_attenuationColor = vec3(1, 1, 1);

		float m_metallicFactor = 1.0f;
		float m_roughnessFactor = 1.0f;
		float m_indexOfRefraction = 1.5f;
		float m_occlusionFactor = 1;
		float m_transmissionFactor = 0;
		float m_specularFactor = 1;
		float m_alphaCutoff = 0.5f;
		float m_thicknessFactor = 0.0f;
		float m_attenuationDistance = std::numeric_limits<float>().max();

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

	/*SAILOR_API void ProcessNode_Assimp(TVector<Math::Triangle>& outScene, aiNode* node, const aiScene* scene, const glm::mat4& matrix);
	SAILOR_API void ProcessMesh_Assimp(aiMesh* mesh, TVector<Math::Triangle>& outScene, const aiScene* scene, const glm::mat4& matrix);

	SAILOR_API mat4 GetWorldTransformMatrix(const aiScene* scene, const char* name);*/

	SAILOR_API void GenerateTangentBitangent(vec3& outTangent, vec3& outBitangent, const vec3* vert, const vec2* uv);


	template<typename T>
	void LoadTexture(const tinygltf::Model& model,
		const std::filesystem::path& sceneDir,
		TVector<TSharedPtr<CombinedSampler2D>>& textures,
		uint32_t textureIndex,
		bool bConvertToLinear,
		bool bNormalMap = false)
	{
		auto ptr = textures[textureIndex] = TSharedPtr<CombinedSampler2D>::Make();

		SamplerClamping clamping = SamplerClamping::Clamp;
		const auto& gltfTex = model.textures[textureIndex];
		if (gltfTex.sampler >= 0)
		{
			const auto& sampler = model.samplers[gltfTex.sampler];
			if (sampler.wrapS == TINYGLTF_TEXTURE_WRAP_REPEAT || sampler.wrapT == TINYGLTF_TEXTURE_WRAP_REPEAT)
			{
				clamping = SamplerClamping::Repeat;
			}
		}
		ptr->m_clamping = clamping;

		if constexpr (IsSame<vec4, T>)
		{
			ptr->m_channels = 4;
		}
		else if constexpr (IsSame<vec3, T>)
		{
			ptr->m_channels = 3;
		}

		int32_t width = 0;
		int32_t height = 0;
		int32_t channels = 0;

		bool hdr = false;
		stbi_uc* pixelsU8 = nullptr;
		float* pixelsF = nullptr;

		const auto& image = model.images[gltfTex.source];

		if (!image.uri.empty())
		{
			std::filesystem::path imgPath = sceneDir / image.uri;
			hdr = stbi_is_hdr(imgPath.string().c_str());
			if (hdr)
				pixelsF = stbi_loadf(imgPath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
			else
				pixelsU8 = stbi_load(imgPath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
		}
		else if (image.bufferView >= 0)
		{
			const auto& view = model.bufferViews[image.bufferView];
			const auto& buffer = model.buffers[view.buffer];
			const unsigned char* data = buffer.data.data() + view.byteOffset;
			int length = (int)view.byteLength;
			hdr = stbi_is_hdr_from_memory(data, length);
			if (hdr)
				pixelsF = stbi_loadf_from_memory(data, length, &width, &height, &channels, STBI_rgb_alpha);
			else
				pixelsU8 = stbi_load_from_memory(data, length, &width, &height, &channels, STBI_rgb_alpha);
		}

		ptr->m_width = width;
		ptr->m_height = height;

		if (hdr && pixelsF)
		{
			ptr->Initialize<T, vec4>((vec4*)pixelsF, bConvertToLinear, bNormalMap);
			stbi_image_free(pixelsF);
		}
		else if (pixelsU8)
		{
			ptr->Initialize<T, u8vec4>((u8vec4*)pixelsU8, bConvertToLinear, bNormalMap);
			stbi_image_free(pixelsU8);
		}
	}
}