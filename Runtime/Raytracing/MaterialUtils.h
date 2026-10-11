#pragma once
#include "Core/Defines.h"
#include "Containers/Vector.h"
#include "Math/Math.h"
#include "Memory/LockFreeHeapAllocator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

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
		SamplerClamping m_clampingU = SamplerClamping::Clamp;
		SamplerClamping m_clampingV = SamplerClamping::Clamp;

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
			m_clampingU = m_clampingV = clamping;
		}

		template<typename TOutputData, typename TInputData>
		void Initialize(const TInputData* data, bool bConvertToLinear, bool bNormalMap = false)
		{
			Initialize<TOutputData, TInputData>(data, bConvertToLinear, bNormalMap, []() { return true; });
		}

		template<typename TOutputData, typename TInputData, typename TContinue>
		bool Initialize(const TInputData* data, bool bConvertToLinear, bool bNormalMap, const TContinue& shouldContinue)
		{
			SAILOR_PROFILE_FUNCTION();

			if (!shouldContinue()) return false;
			m_data.Resize(m_width * m_height * sizeof(TOutputData));

			for (uint32_t i = 0; i < (uint32_t)m_width * m_height; i++)
			{
				if (i % 1024u == 0u && !shouldContinue()) return false;
				TOutputData* dst = (TOutputData*)(m_data.GetData() + sizeof(TOutputData) * i);
				const TInputData* src = data + i;
				const TOutputData normalized =
					std::is_floating_point_v<typename TInputData::value_type> ?
						TOutputData(*src) :
						TOutputData(*src) * (1.0f / 255.0f);

				if (bNormalMap)
				{
					*dst = normalized * 2.0f - 1.0f;
				}
				else
				{
					*dst = bConvertToLinear ?
						(TOutputData)Utils::SRGBToLinear(normalized) :
						normalized;
				}
			}
			return shouldContinue();
		}

		template<typename T>
		__forceinline void SetPixel(uint32_t x, uint32_t y, const T& value)
		{
			auto ptr = reinterpret_cast<T*>(m_data.GetData());

			ptr[x + y * m_width] = value;
		}

		template<typename T>
		T Sample(const vec2& uv) const
		{
			SAILOR_PROFILE_FUNCTION();

			const auto pixelCoordinate = [](float coordinate, int32_t size, SamplerClamping clamping)
			{
				coordinate = clamping == SamplerClamping::Repeat ?
					coordinate - std::floor(coordinate) : std::clamp(coordinate, 0.0f, 1.0f);
				// Normalized coordinates put texel centres at (i + 0.5) / size.
				return coordinate * static_cast<float>(size) - 0.5f;
			};
			const auto address = [](int32_t index, int32_t size, SamplerClamping clamping)
			{
				if (clamping == SamplerClamping::Clamp)
				{
					return std::clamp(index, 0, size - 1);
				}
				return index < 0 ? size - 1 : index >= size ? 0 : index;
			};

			const float fx = pixelCoordinate(uv.x, m_width, m_clampingU);
			const float fy = pixelCoordinate(uv.y, m_height, m_clampingV);
			const int32_t x = static_cast<int32_t>(std::floor(fx));
			const int32_t y = static_cast<int32_t>(std::floor(fy));
			const float fracX = fx - x;
			const float fracY = fy - y;
			const int32_t x0 = address(x, m_width, m_clampingU);
			const int32_t x1 = address(x + 1, m_width, m_clampingU);
			const int32_t y0 = address(y, m_height, m_clampingV);
			const int32_t y1 = address(y + 1, m_height, m_clampingV);
			const T* pixels = reinterpret_cast<const T*>(m_data.GetData());
			const T top = glm::mix(pixels[x0 + y0 * m_width], pixels[x1 + y0 * m_width], fracX);
			const T bottom = glm::mix(pixels[x0 + y1 * m_width], pixels[x1 + y1 * m_width], fracX);
			return glm::mix(top, bottom, fracY);
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

	enum class FaceCullMode : uint8_t
	{
		None = 0,
		Front,
		Back,
		FrontAndBack
	};

	struct Material
	{
		static constexpr uint16_t InvalidTextureIndex =
			(std::numeric_limits<uint16_t>::max)();

		glm::mat3 m_uvTransform = mat3(1);
		glm::vec4 m_layerUvScale = vec4(1);

		glm::vec4 m_baseColorFactor = vec4(1, 1, 1, 1);
		glm::vec3 m_emissiveFactor = vec3(0, 0, 0);
		glm::vec3 m_specularColorFactor = vec3(1, 1, 1);
		glm::vec3 m_attenuationColor = vec3(1, 1, 1);
		glm::vec3 m_sheenColorFactor = vec3(0, 0, 0);

		float m_metallicFactor = 1.0f;
		float m_roughnessFactor = 1.0f;
		float m_normalScale = 1.0f;
		float m_clearcoatFactor = 0.0f;
		float m_clearcoatRoughnessFactor = 0.0f;
		float m_clearcoatNormalScale = 1.0f;
		float m_sheenRoughnessFactor = 0.0f;
		float m_indexOfRefraction = 1.5f;
		float m_occlusionFactor = 1;
		float m_transmissionFactor = 0;
		float m_specularFactor = 1;
		float m_alphaCutoff = 0.5f;
		float m_thicknessFactor = 0.0f;
		float m_attenuationDistance = std::numeric_limits<float>().max();

		bool HasEmissiveTexture() const { return m_emissiveIndex != InvalidTextureIndex; }
		bool HasBaseTexture() const { return m_baseColorIndex != InvalidTextureIndex; }
		bool HasAmbientTexture() const { return m_ambientIndex != InvalidTextureIndex; }
		bool HasNormalTexture() const { return m_normalIndex != InvalidTextureIndex; }
		bool HasMetallicRoughnessTexture() const { return m_metallicRoughnessIndex != InvalidTextureIndex; }
		bool HasRoughnessTexture() const { return m_roughnessIndex != InvalidTextureIndex; }
		bool HasMetallicTexture() const { return m_metallicIndex != InvalidTextureIndex; }
		bool HasSpecularTexture() const { return m_specularColorIndex != InvalidTextureIndex; }
		bool HasOcclusionTexture() const { return m_occlusionIndex != InvalidTextureIndex; }
		bool HasTransmissionTexture() const { return m_transmissionIndex != InvalidTextureIndex; }
		bool HasThicknessTexture() const { return m_thicknessIndex != InvalidTextureIndex; }
		bool HasClearcoatTexture() const { return m_clearcoatIndex != InvalidTextureIndex; }
		bool HasClearcoatRoughnessTexture() const { return m_clearcoatRoughnessIndex != InvalidTextureIndex; }
		bool HasClearcoatNormalTexture() const { return m_clearcoatNormalIndex != InvalidTextureIndex; }
		bool HasSheenColorTexture() const { return m_sheenColorIndex != InvalidTextureIndex; }
		bool HasSheenRoughnessTexture() const { return m_sheenRoughnessIndex != InvalidTextureIndex; }
		bool HasLayerColorTexture(size_t layer) const
		{
			return layer < 4u &&
				m_layerColorIndices[layer] != InvalidTextureIndex;
		}
		bool HasLayerColorTextures() const
		{
			return HasLayerColorTexture(0u) ||
				HasLayerColorTexture(1u) ||
				HasLayerColorTexture(2u) ||
				HasLayerColorTexture(3u);
		}

		uint16_t m_baseColorIndex = InvalidTextureIndex;
		uint16_t m_ambientIndex = InvalidTextureIndex;
		uint16_t m_specularIndex = InvalidTextureIndex;
		uint16_t m_emissiveIndex = InvalidTextureIndex;
		uint16_t m_normalIndex = InvalidTextureIndex;
		uint16_t m_metallicRoughnessIndex = InvalidTextureIndex;
		uint16_t m_roughnessIndex = InvalidTextureIndex;
		uint16_t m_metallicIndex = InvalidTextureIndex;
		uint16_t m_occlusionIndex = InvalidTextureIndex;
		uint16_t m_transmissionIndex = InvalidTextureIndex;
		uint16_t m_thicknessIndex = InvalidTextureIndex;
		uint16_t m_specularColorIndex = InvalidTextureIndex;
		uint16_t m_clearcoatIndex = InvalidTextureIndex;
		uint16_t m_clearcoatRoughnessIndex = InvalidTextureIndex;
		uint16_t m_clearcoatNormalIndex = InvalidTextureIndex;
		uint16_t m_sheenColorIndex = InvalidTextureIndex;
		uint16_t m_sheenRoughnessIndex = InvalidTextureIndex;
		uint16_t m_layerColorIndices[4] = {
			InvalidTextureIndex,
			InvalidTextureIndex,
			InvalidTextureIndex,
			InvalidTextureIndex
		};

		BlendMode m_blendMode = BlendMode::Opaque;
		FaceCullMode m_faceCullMode = FaceCullMode::Back;
	};

	SAILOR_API uint PackVec3ToByte(vec3 v);
	SAILOR_API vec3 UnpackByteToVec3(uint byte);

	SAILOR_API void GenerateTangentBitangent(vec3& outTangent, vec3& outBitangent, const vec3* vert, const vec2* uv);
}
