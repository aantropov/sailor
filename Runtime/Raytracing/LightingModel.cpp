#include "LightingModel.h"
#include "Containers/Vector.h"
#include "Core/Utils.h"
#include "Math/Math.h"
#include "MaterialUtils.h"

#include <array>
#include <cmath>

using namespace glm;
using namespace Sailor;
using namespace Sailor::Math;
using namespace Sailor::Raytracing;

namespace
{
	constexpr float MinIor = 0.0001f;
	constexpr float DirectionEpsilon = 0.000001f;
	constexpr uint32_t SheenDirectionalAlbedoLutSize = 64u;
	constexpr uint32_t SheenDirectionalAlbedoSampleCount = 256u;

	struct LobeProbabilities
	{
		float m_specular = 0.0f;
		float m_diffuse = 0.0f;
		float m_transmission = 0.0f;
		float m_sheen = 0.0f;
		float m_clearcoat = 0.0f;
	};

	float ResolveIor(float ior)
	{
		return std::isfinite(ior) && ior > MinIor ? ior : 1.0f;
	}

	float Luminance(const vec3& color)
	{
		return dot(
			max(color, vec3(0.0f)),
			vec3(0.2126729f, 0.7151522f, 0.0721750f));
	}

	float MaxComponent(const vec3& color)
	{
		return (std::max)(color.x, (std::max)(color.y, color.z));
	}

	bool IsFinite(const vec3& value)
	{
		return std::isfinite(value.x) &&
			std::isfinite(value.y) &&
			std::isfinite(value.z);
	}

	vec3 ResolveClearcoatNormal(
		const LightingModel::SampledData& sample,
		const vec3& worldNormal)
	{
		const float lengthSquared = dot(
			sample.m_clearcoatNormal,
			sample.m_clearcoatNormal);
		return lengthSquared > DirectionEpsilon &&
			IsFinite(sample.m_clearcoatNormal) ?
			sample.m_clearcoatNormal * inversesqrt(lengthSquared) :
			worldNormal;
	}

	float RadicalInverseVdC(uint32_t bits)
	{
		bits = (bits << 16u) | (bits >> 16u);
		bits = ((bits & 0x55555555u) << 1u) |
			((bits & 0xAAAAAAAAu) >> 1u);
		bits = ((bits & 0x33333333u) << 2u) |
			((bits & 0xCCCCCCCCu) >> 2u);
		bits = ((bits & 0x0F0F0F0Fu) << 4u) |
			((bits & 0xF0F0F0F0u) >> 4u);
		bits = ((bits & 0x00FF00FFu) << 8u) |
			((bits & 0xFF00FF00u) >> 8u);
		return static_cast<float>(bits) * 2.3283064365386963e-10f;
	}

	struct SheenDirectionalAlbedoLut final
	{
		std::array<float,
			SheenDirectionalAlbedoLutSize *
			SheenDirectionalAlbedoLutSize> m_values{};

		SheenDirectionalAlbedoLut()
		{
			for (uint32_t y = 0u;
				y < SheenDirectionalAlbedoLutSize;
				++y)
			{
				const float roughness =
					(static_cast<float>(y) + 0.5f) /
					static_cast<float>(SheenDirectionalAlbedoLutSize);
				for (uint32_t x = 0u;
					x < SheenDirectionalAlbedoLutSize;
					++x)
				{
					const float nDotV = (std::max)(
						(static_cast<float>(x) + 0.5f) /
							static_cast<float>(SheenDirectionalAlbedoLutSize),
						0.001f);
					const vec3 viewDirection(
						sqrt((std::max)(
							1.0f - nDotV * nDotV,
							0.0f)),
						0.0f,
						nDotV);
					float directionalAlbedo = 0.0f;
					for (uint32_t sampleIndex = 0u;
						sampleIndex <
							SheenDirectionalAlbedoSampleCount;
						++sampleIndex)
					{
						const vec2 randomSample(
							static_cast<float>(sampleIndex) /
								static_cast<float>(
									SheenDirectionalAlbedoSampleCount),
							RadicalInverseVdC(sampleIndex));
						const vec3 halfVector =
							LightingModel::ImportanceSampleCharlie(
								randomSample,
								roughness,
								vec3(0.0f, 0.0f, 1.0f));
						const vec3 lightDirection =
							2.0f * dot(viewDirection, halfVector) *
								halfVector - viewDirection;
						const float nDotL = lightDirection.z;
						if (nDotL <= 0.0f)
						{
							continue;
						}
						const float nDotH = (std::max)(
							halfVector.z,
							0.001f);
						const float vDotH = (std::max)(
							dot(viewDirection, halfVector),
							0.0f);
						directionalAlbedo +=
							4.0f * LightingModel::VisibilitySheen(
								nDotL,
								nDotV,
								roughness) *
							nDotL * vDotH / nDotH;
					}
					m_values[x + y *
						SheenDirectionalAlbedoLutSize] = clamp(
							directionalAlbedo /
								static_cast<float>(
									SheenDirectionalAlbedoSampleCount),
							0.0f,
							1.0f);
				}
			}
		}
	};

	float ResolveSheenDirectionalAlbedo(
		float cosine,
		float roughness)
	{
		static const SheenDirectionalAlbedoLut lut;
		const float x = clamp(
			clamp(cosine, 0.0f, 1.0f) *
				static_cast<float>(SheenDirectionalAlbedoLutSize) - 0.5f,
			0.0f,
			static_cast<float>(
				SheenDirectionalAlbedoLutSize - 1u));
		const float y = clamp(
			clamp(roughness, 0.0f, 1.0f) *
				static_cast<float>(SheenDirectionalAlbedoLutSize) - 0.5f,
			0.0f,
			static_cast<float>(
				SheenDirectionalAlbedoLutSize - 1u));
		const int32_t x0 = clamp(
			static_cast<int32_t>(floor(x)),
			0,
			static_cast<int32_t>(
				SheenDirectionalAlbedoLutSize - 1u));
		const int32_t y0 = clamp(
			static_cast<int32_t>(floor(y)),
			0,
			static_cast<int32_t>(
				SheenDirectionalAlbedoLutSize - 1u));
		const int32_t x1 = (std::min)(
			x0 + 1,
			static_cast<int32_t>(
				SheenDirectionalAlbedoLutSize - 1u));
		const int32_t y1 = (std::min)(
			y0 + 1,
			static_cast<int32_t>(
				SheenDirectionalAlbedoLutSize - 1u));
		const float fractionX = clamp(x - floor(x), 0.0f, 1.0f);
		const float fractionY = clamp(y - floor(y), 0.0f, 1.0f);
		const auto sample = [](int32_t sampleX, int32_t sampleY)
		{
			return lut.m_values[static_cast<size_t>(sampleX) +
				static_cast<size_t>(sampleY) *
				SheenDirectionalAlbedoLutSize];
		};
		const float top = mix(
			sample(x0, y0),
			sample(x1, y0),
			fractionX);
		const float bottom = mix(
			sample(x0, y1),
			sample(x1, y1),
			fractionX);
		return mix(top, bottom, fractionY);
	}

	float ResolveSheenBaseScaling(
		const LightingModel::SampledData& sample,
		float nDotV,
		float nDotL)
	{
		const vec3 sheenColor = clamp(
			sample.m_sheenColor,
			vec3(0.0f),
			vec3(1.0f));
		const float maximumColor = MaxComponent(sheenColor);
		if (maximumColor <= 0.0f)
		{
			return 1.0f;
		}
		const float roughness = clamp(
			sample.m_sheenRoughness,
			0.0f,
			1.0f);
		const float viewScaling = 1.0f - maximumColor *
			ResolveSheenDirectionalAlbedo(abs(nDotV), roughness);
		const float lightScaling = 1.0f - maximumColor *
			ResolveSheenDirectionalAlbedo(abs(nDotL), roughness);
		return clamp(
			(std::min)(viewScaling, lightScaling),
			0.0f,
			1.0f);
	}

	float ResolveClearcoatLayerWeight(
		const LightingModel::SampledData& sample,
		const vec3& worldNormal,
		const vec3& viewDirection)
	{
		const float factor = clamp(
			sample.m_clearcoatFactor,
			0.0f,
			1.0f);
		if (factor <= 0.0f)
		{
			return 0.0f;
		}
		const vec3 clearcoatNormal = ResolveClearcoatNormal(
			sample,
			worldNormal);
		const float nDotV = clamp(
			dot(clearcoatNormal, viewDirection),
			0.0f,
			1.0f);
		return clamp(
			factor * LightingModel::FresnelSchlick(
				nDotV,
				vec3(0.04f)).x,
			0.0f,
			1.0f);
	}

	LobeProbabilities CalculateLobeProbabilities(
		const LightingModel::SampledData& sample,
		const vec3& worldNormal,
		const vec3& viewDirection,
		float fromIor,
		float toIor)
	{
		const float metallic = clamp(sample.m_orm.z, 0.0f, 1.0f);
		const float transmission =
			clamp(sample.m_transmission, 0.0f, 1.0f);
		const float nDotV = clamp(
			dot(worldNormal, viewDirection),
			0.0f,
			1.0f);
		const vec3 baseColor = clamp(
			vec3(sample.m_baseColor),
			vec3(0.0f),
			vec3(1.0f));
		const vec3 dielectricFresnel = vec3(
			LightingModel::FresnelDielectric(
				nDotV,
				fromIor,
				toIor));
		const vec3 metalFresnel = LightingModel::FresnelSchlick(
			nDotV,
			baseColor);
		const float reflectedEnergy = clamp(
			Luminance(mix(
				dielectricFresnel,
				metalFresnel,
				metallic)),
			0.0f,
			1.0f);
		const float dielectricTransmissionEnergy =
			1.0f - clamp(Luminance(dielectricFresnel), 0.0f, 1.0f);
		const float baseEnergy =
			(1.0f - metallic) *
			dielectricTransmissionEnergy *
			Luminance(vec3(sample.m_baseColor));

		const float clearcoatWeight = ResolveClearcoatLayerWeight(
			sample,
			worldNormal,
			viewDirection);
		const float belowClearcoat = 1.0f - clearcoatWeight;
		const vec3 sheenColor = clamp(
			sample.m_sheenColor,
			vec3(0.0f),
			vec3(1.0f));
		const float sheenDirectionalAlbedo =
			MaxComponent(sheenColor) > 0.0f ?
			ResolveSheenDirectionalAlbedo(
				nDotV,
				clamp(sample.m_sheenRoughness, 0.0f, 1.0f)) :
			0.0f;
		const float sheenEnergy = clamp(
			Luminance(sheenColor) * sheenDirectionalAlbedo,
			0.0f,
			1.0f);
		const float sheenBaseScaling = ResolveSheenBaseScaling(
			sample,
			nDotV,
			nDotV);

		LobeProbabilities result;
		result.m_specular = reflectedEnergy *
			sheenBaseScaling * belowClearcoat;
		result.m_diffuse = baseEnergy * (1.0f - transmission) *
			sheenBaseScaling * belowClearcoat;
		result.m_transmission = baseEnergy * transmission *
			sheenBaseScaling * belowClearcoat;
		result.m_sheen = sheenEnergy * belowClearcoat;
		result.m_clearcoat = clearcoatWeight;

		const float total = result.m_specular +
			result.m_diffuse + result.m_transmission +
			result.m_sheen + result.m_clearcoat;
		if (total > 0.0f && std::isfinite(total))
		{
			result.m_specular /= total;
			result.m_diffuse /= total;
			result.m_transmission /= total;
			result.m_sheen /= total;
			result.m_clearcoat /= total;
		}
		return result;
	}

	float GGXMicrofacetPdf(
		const vec3& worldNormal,
		const vec3& halfVector,
		float roughness)
	{
		const float nDotH = dot(worldNormal, halfVector);
		if (nDotH <= 0.0f)
		{
			return 0.0f;
		}
		return LightingModel::DistributionGGX(
			worldNormal,
			halfVector,
			roughness) * nDotH;
	}

	float CharlieReflectionPdf(
		const vec3& worldNormal,
		const vec3& halfVector,
		const vec3& viewDirection,
		float roughness)
	{
		const float nDotH = dot(worldNormal, halfVector);
		const float vDotH = abs(dot(viewDirection, halfVector));
		if (nDotH <= 0.0f || vDotH <= DirectionEpsilon)
		{
			return 0.0f;
		}
		const float result = LightingModel::DistributionCharlie(
			worldNormal,
			halfVector,
			roughness) * nDotH / (4.0f * vDotH);
		return std::isfinite(result) && result > 0.0f ?
			result : 0.0f;
	}

	bool IsTotalInternalReflection(
		const vec3& viewDirection,
		const vec3& halfVector,
		float fromIor,
		float toIor)
	{
		const float cosTheta = clamp(
			dot(viewDirection, halfVector),
			0.0f,
			1.0f);
		const float eta = ResolveIor(fromIor) / ResolveIor(toIor);
		const float sinThetaSquared =
			eta * eta * (1.0f - cosTheta * cosTheta);
		return sinThetaSquared >= 1.0f;
	}

	float ThinTransmissionPdf(
		const LightingModel::SampledData& sample,
		const vec3& worldNormal,
		const vec3& viewDirection,
		const vec3& lightDirection)
	{
		const vec3 mirroredLight = lightDirection -
			2.0f * worldNormal * dot(lightDirection, worldNormal);
		const vec3 halfVector = viewDirection + mirroredLight;
		const float halfVectorLengthSquared = dot(halfVector, halfVector);
		if (dot(worldNormal, lightDirection) >= 0.0f ||
			dot(worldNormal, mirroredLight) <= 0.0f ||
			halfVectorLengthSquared <= DirectionEpsilon)
		{
			return 0.0f;
		}
		const vec3 H = halfVector * inversesqrt(halfVectorLengthSquared);
		const float vDotH = abs(dot(viewDirection, H));
		if (vDotH <= DirectionEpsilon)
		{
			return 0.0f;
		}
		return GGXMicrofacetPdf(
			worldNormal,
			H,
			sample.m_orm.y) / (4.0f * vDotH);
	}

	float ThickTransmissionPdf(
		const LightingModel::SampledData& sample,
		const vec3& worldNormal,
		const vec3& viewDirection,
		const vec3& lightDirection,
		float fromIor,
		float toIor)
	{
		if (dot(worldNormal, viewDirection) <= 0.0f ||
			dot(worldNormal, lightDirection) >= 0.0f)
		{
			return 0.0f;
		}

		const float eta = ResolveIor(toIor) / ResolveIor(fromIor);
		vec3 halfVector = viewDirection + lightDirection * eta;
		const float halfVectorLengthSquared = dot(halfVector, halfVector);
		if (halfVectorLengthSquared <= DirectionEpsilon)
		{
			return 0.0f;
		}
		halfVector *= inversesqrt(halfVectorLengthSquared);
		if (dot(worldNormal, halfVector) < 0.0f)
		{
			halfVector *= -1.0f;
		}

		const float vDotH = dot(viewDirection, halfVector);
		const float lDotH = dot(lightDirection, halfVector);
		if (vDotH <= 0.0f || lDotH >= 0.0f)
		{
			return 0.0f;
		}
		const float denominator =
			lDotH + vDotH / eta;
		const float denominatorSquared =
			denominator * denominator;
		if (denominatorSquared <= DirectionEpsilon)
		{
			return 0.0f;
		}
		const float halfVectorJacobian =
			abs(lDotH) / denominatorSquared;
		return GGXMicrofacetPdf(
			worldNormal,
			halfVector,
			sample.m_orm.y) * halfVectorJacobian;
	}

	float ReflectionPdfForInterface(
		const LightingModel::SampledData& sample,
		const vec3& worldNormal,
		const vec3& viewDirection,
		const vec3& lightDirection,
		float fromIor,
		float toIor,
		bool bRouteTransmissionTir)
	{
		const float nDotV = dot(worldNormal, viewDirection);
		const float nDotL = dot(worldNormal, lightDirection);
		const vec3 halfVector = viewDirection + lightDirection;
		const float halfVectorLengthSquared = dot(halfVector, halfVector);
		if (nDotV <= 0.0f ||
			halfVectorLengthSquared <= DirectionEpsilon)
		{
			return 0.0f;
		}

		const vec3 H = halfVector * inversesqrt(halfVectorLengthSquared);
		const LobeProbabilities probabilities =
			CalculateLobeProbabilities(
				sample,
				worldNormal,
				viewDirection,
				fromIor,
				toIor);
		float specularProbability = probabilities.m_specular;
		if (nDotL > 0.0f && bRouteTransmissionTir &&
			probabilities.m_transmission > 0.0f &&
			IsTotalInternalReflection(
				viewDirection,
				H,
				fromIor,
				toIor))
		{
			specularProbability += probabilities.m_transmission;
		}
		const float specularPdf = nDotL > 0.0f ?
			LightingModel::GGX_PDF(
				worldNormal,
				H,
				viewDirection,
				sample.m_orm.y) : 0.0f;
		const float diffusePdf = (std::max)(nDotL, 0.0f) / Math::Pi;
		const float sheenPdf = nDotL > 0.0f ?
			CharlieReflectionPdf(
				worldNormal,
				H,
				viewDirection,
				sample.m_sheenRoughness) : 0.0f;
		const vec3 clearcoatNormal = ResolveClearcoatNormal(
			sample,
			worldNormal);
		const float clearcoatPdf =
			dot(clearcoatNormal, viewDirection) > 0.0f &&
			dot(clearcoatNormal, lightDirection) > 0.0f ?
			LightingModel::GGX_PDF(
				clearcoatNormal,
				H,
				viewDirection,
				sample.m_clearcoatRoughness) : 0.0f;
		const float result =
			specularProbability * specularPdf +
			probabilities.m_diffuse * diffusePdf +
			probabilities.m_sheen * sheenPdf +
			probabilities.m_clearcoat * clearcoatPdf;
		return std::isfinite(result) && result > 0.0f ? result : 0.0f;
	}

	vec3 CalculateBaseBRDF(
		const vec3& viewDirection,
		const vec3& worldNormal,
		const vec3& lightDirection,
		const LightingModel::SampledData& sample,
		float fromIor,
		float toIor)
	{
		const float roughness = clamp(sample.m_orm.y, 0.0f, 1.0f);
		const float metallic = clamp(sample.m_orm.z, 0.0f, 1.0f);
		const float nDotL = dot(worldNormal, lightDirection);
		const float nDotV = dot(worldNormal, viewDirection);
		if (nDotL <= 0.0f || nDotV <= 0.0f)
		{
			return vec3(0.0f);
		}

		const vec3 halfVectorValue = viewDirection + lightDirection;
		const float halfVectorLengthSquared = dot(
			halfVectorValue,
			halfVectorValue);
		if (halfVectorLengthSquared <= DirectionEpsilon)
		{
			return vec3(0.0f);
		}
		const vec3 halfVector = halfVectorValue *
			inversesqrt(halfVectorLengthSquared);
		const float distribution = LightingModel::DistributionGGX(
			worldNormal,
			halfVector,
			roughness);
		const float vDotH = dot(halfVector, viewDirection);
		const vec3 dielectricFresnel = vec3(
			LightingModel::FresnelDielectric(
				vDotH,
				fromIor,
				toIor));
		const vec3 baseColor = clamp(
			vec3(sample.m_baseColor),
			vec3(0.0f),
			vec3(1.0f));
		const vec3 metalFresnel = LightingModel::FresnelSchlick(
			vDotH,
			baseColor);
		const vec3 fresnel = mix(
			dielectricFresnel,
			metalFresnel,
			metallic);
		const float geometry =
			LightingModel::GeometrySmithGGXCorrelated(
				nDotL,
				nDotV,
				roughness);

		// glTF linearly mixes complete dielectric and metallic BRDFs. The
		// dielectric diffuse term must therefore be attenuated by dielectric
		// Fresnel before metalness is applied; using the already mixed Fresnel
		// darkens filtered and authored intermediate-metalness values.
		vec3 diffuseWeight =
			(vec3(1.0f) - dielectricFresnel) * (1.0f - metallic);
		diffuseWeight *=
			1.0f - clamp(sample.m_transmission, 0.0f, 1.0f);

		const float denominator = (std::max)(
			4.0f * nDotV * nDotL,
			1e-6f);
		const vec3 specular =
			fresnel * distribution * geometry / denominator;
		const vec3 diffuse = diffuseWeight *
			max(vec3(sample.m_baseColor), vec3(0.0f)) /
			glm::pi<float>();
		return diffuse + specular;
	}

	vec3 CalculateSheenBRDF(
		const vec3& viewDirection,
		const vec3& worldNormal,
		const vec3& lightDirection,
		const LightingModel::SampledData& sample)
	{
		const vec3 color = clamp(
			sample.m_sheenColor,
			vec3(0.0f),
			vec3(1.0f));
		const float nDotL = dot(worldNormal, lightDirection);
		const float nDotV = dot(worldNormal, viewDirection);
		if (MaxComponent(color) <= 0.0f ||
			nDotL <= 0.0f || nDotV <= 0.0f)
		{
			return vec3(0.0f);
		}
		const vec3 halfVectorValue = viewDirection + lightDirection;
		const float halfVectorLengthSquared = dot(
			halfVectorValue,
			halfVectorValue);
		if (halfVectorLengthSquared <= DirectionEpsilon)
		{
			return vec3(0.0f);
		}
		const vec3 halfVector = halfVectorValue *
			inversesqrt(halfVectorLengthSquared);
		return color * LightingModel::DistributionCharlie(
			worldNormal,
			halfVector,
			sample.m_sheenRoughness) *
			LightingModel::VisibilitySheen(
				nDotL,
				nDotV,
				sample.m_sheenRoughness);
	}

	vec3 CalculateClearcoatBRDF(
		const vec3& viewDirection,
		const vec3& worldNormal,
		const vec3& lightDirection,
		const LightingModel::SampledData& sample)
	{
		if (sample.m_clearcoatFactor <= 0.0f)
		{
			return vec3(0.0f);
		}
		const vec3 clearcoatNormal = ResolveClearcoatNormal(
			sample,
			worldNormal);
		const float nDotL = dot(clearcoatNormal, lightDirection);
		const float nDotV = dot(clearcoatNormal, viewDirection);
		if (nDotL <= 0.0f || nDotV <= 0.0f)
		{
			return vec3(0.0f);
		}
		const vec3 halfVectorValue = viewDirection + lightDirection;
		const float halfVectorLengthSquared = dot(
			halfVectorValue,
			halfVectorValue);
		if (halfVectorLengthSquared <= DirectionEpsilon)
		{
			return vec3(0.0f);
		}
		const vec3 halfVector = halfVectorValue *
			inversesqrt(halfVectorLengthSquared);
		const float roughness = clamp(
			sample.m_clearcoatRoughness,
			0.0f,
			1.0f);
		const float distribution = LightingModel::DistributionGGX(
			clearcoatNormal,
			halfVector,
			roughness);
		const float geometry =
			LightingModel::GeometrySmithGGXCorrelated(
				nDotL,
				nDotV,
				roughness);
		return vec3(distribution * geometry /
			(std::max)(4.0f * nDotL * nDotV, DirectionEpsilon));
	}
}

float LightingModel::DistributionGGX(const vec3& N, const vec3& H, float roughness)
{
	const float nDotH = dot(N, H);
	if (nDotH <= 0.0f)
	{
		return 0.0f;
	}
	const float clampedRoughness = clamp(roughness, 0.0f, 1.0f);
	const float a = (std::max)(
		clampedRoughness * clampedRoughness,
		0.001f);
	const float a2 = a * a;
	const float nDotH2 = nDotH * nDotH;

	const float denominatorBase =
		nDotH2 * (a2 - 1.0f) + 1.0f;
	const float denominator =
		glm::pi<float>() * denominatorBase * denominatorBase;

	return a2 / (std::max)(denominator, 1e-7f);
}

float LightingModel::DistributionCharlie(
	const vec3& N,
	const vec3& H,
	float roughness)
{
	const float nDotH = dot(N, H);
	if (nDotH <= 0.0f)
	{
		return 0.0f;
	}
	const float alpha = (std::max)(
		clamp(roughness, 0.0f, 1.0f) *
			clamp(roughness, 0.0f, 1.0f),
		0.000001f);
	const float inverseAlpha = 1.0f / alpha;
	const float sineSquared = (std::max)(
		1.0f - nDotH * nDotH,
		0.0f);
	return (2.0f + inverseAlpha) *
		pow(sineSquared, 0.5f * inverseAlpha) /
		(2.0f * glm::pi<float>());
}

vec3 LightingModel::FresnelSchlick(float cosTheta, const vec3& F0)
{
	const float clampedCosine = clamp(cosTheta, 0.0f, 1.0f);
	const vec3 clampedF0 = clamp(F0, vec3(0.0f), vec3(1.0f));
	return clampedF0 +
		(1.0f - clampedF0) *
		glm::pow(1.0f - clampedCosine, 5.0f);
}

float LightingModel::FresnelDielectric(
	float cosTheta,
	float fromIor,
	float toIor)
{
	const float etaI = ResolveIor(fromIor);
	const float etaT = ResolveIor(toIor);
	if (abs(etaI - etaT) <= MinIor)
	{
		return 0.0f;
	}
	if (etaI >= 100000.0f || etaT >= 100000.0f)
	{
		return 1.0f;
	}
	const float cosThetaI = clamp(abs(cosTheta), 0.0f, 1.0f);
	const float sinThetaISquared =
		(std::max)(0.0f, 1.0f - cosThetaI * cosThetaI);
	const float eta = etaI / etaT;
	const float sinThetaTSquared = eta * eta * sinThetaISquared;
	if (sinThetaTSquared >= 1.0f)
	{
		return 1.0f;
	}

	const float cosThetaT = sqrt(
		(std::max)(0.0f, 1.0f - sinThetaTSquared));
	const float parallelNumerator =
		etaT * cosThetaI - etaI * cosThetaT;
	const float parallelDenominator =
		etaT * cosThetaI + etaI * cosThetaT;
	const float perpendicularNumerator =
		etaI * cosThetaI - etaT * cosThetaT;
	const float perpendicularDenominator =
		etaI * cosThetaI + etaT * cosThetaT;
	const float parallel = parallelDenominator != 0.0f ?
		parallelNumerator / parallelDenominator : 1.0f;
	const float perpendicular = perpendicularDenominator != 0.0f ?
		perpendicularNumerator / perpendicularDenominator : 1.0f;
	return clamp(
		0.5f * (parallel * parallel + perpendicular * perpendicular),
		0.0f,
		1.0f);
}

float LightingModel::GeometrySmithGGXCorrelated(
	float NdotL,
	float NdotV,
	float roughness)
{
	const float nDotL = clamp(NdotL, 0.0f, 1.0f);
	const float nDotV = clamp(NdotV, 0.0f, 1.0f);
	if (nDotL <= 0.0f || nDotV <= 0.0f)
	{
		return 0.0f;
	}

	const float alpha = (std::max)(
		clamp(roughness, 0.0f, 1.0f) *
			clamp(roughness, 0.0f, 1.0f),
		0.001f);
	const float alphaSquared = alpha * alpha;
	const float lambdaV = nDotL * sqrt(
		nDotV * nDotV * (1.0f - alphaSquared) + alphaSquared);
	const float lambdaL = nDotV * sqrt(
		nDotL * nDotL * (1.0f - alphaSquared) + alphaSquared);
	return 2.0f * nDotL * nDotV /
		(std::max)(lambdaV + lambdaL, 1e-7f);
}

float LightingModel::VisibilitySheen(
	float NdotL,
	float NdotV,
	float roughness)
{
	const float nDotL = clamp(NdotL, 0.0f, 1.0f);
	const float nDotV = clamp(NdotV, 0.0f, 1.0f);
	if (nDotL <= 0.0f || nDotV <= 0.0f)
	{
		return 0.0f;
	}

	const float alpha = (std::max)(
		clamp(roughness, 0.0f, 1.0f) *
			clamp(roughness, 0.0f, 1.0f),
		0.000001f);
	const auto lambdaNumericHelper = [alpha](float cosine)
	{
		const float oneMinusAlphaSquared =
			(1.0f - alpha) * (1.0f - alpha);
		const float a = mix(
			21.5473f,
			25.3245f,
			oneMinusAlphaSquared);
		const float b = mix(
			3.82987f,
			3.32435f,
			oneMinusAlphaSquared);
		const float c = mix(
			0.19823f,
			0.16801f,
			oneMinusAlphaSquared);
		const float d = mix(
			-1.97760f,
			-1.27393f,
			oneMinusAlphaSquared);
		const float e = mix(
			-4.32054f,
			-4.85967f,
			oneMinusAlphaSquared);
		const float x = clamp(cosine, 0.0f, 1.0f);
		return a / (1.0f + b * pow(x, c)) + d * x + e;
	};
	const auto lambda = [&lambdaNumericHelper](float cosine)
	{
		const float x = clamp(abs(cosine), 0.0f, 1.0f);
		return x < 0.5f ?
			exp(lambdaNumericHelper(x)) :
			exp(
				2.0f * lambdaNumericHelper(0.5f) -
				lambdaNumericHelper(1.0f - x));
	};
	const float denominator =
		(1.0f + lambda(nDotV) + lambda(nDotL)) *
		4.0f * nDotV * nDotL;
	return 1.0f / (std::max)(denominator, 1e-7f);
}

vec3 LightingModel::CalculateBTDF(
	const vec3& viewDirection,
	const vec3& worldNormal,
	const vec3& lightDirection,
	const SampledData& sample,
	float fromIor,
	float toIor,
	bool bThickVolume)
{
	const float metallic = clamp(sample.m_orm.z, 0.0f, 1.0f);
	const float transmission =
		clamp(sample.m_transmission, 0.0f, 1.0f) *
		(1.0f - metallic);
	const float nDotV = dot(worldNormal, viewDirection);
	const float nDotL = dot(worldNormal, lightDirection);
	if (transmission <= 0.0f || nDotV <= 0.0f || nDotL >= 0.0f)
	{
		return vec3(0.0f);
	}
	const float upperLayerScaling =
		ResolveSheenBaseScaling(
			sample,
			nDotV,
			nDotL) *
		(1.0f - ResolveClearcoatLayerWeight(
			sample,
			worldNormal,
			viewDirection));

	const vec3 tint =
		max(vec3(sample.m_baseColor), vec3(0.0f)) * transmission;
	const float roughness = clamp(sample.m_orm.y, 0.0f, 1.0f);
	if (!bThickVolume)
	{
		const vec3 mirroredLight = lightDirection -
			2.0f * worldNormal * nDotL;
		const vec3 halfVectorValue = viewDirection + mirroredLight;
		const float halfVectorLengthSquared = dot(
			halfVectorValue,
			halfVectorValue);
		const float mirroredCosine = dot(worldNormal, mirroredLight);
		if (mirroredCosine <= 0.0f ||
			halfVectorLengthSquared <= DirectionEpsilon)
		{
			return vec3(0.0f);
		}
		const vec3 halfVector = halfVectorValue *
			inversesqrt(halfVectorLengthSquared);
		const float vDotH = abs(dot(viewDirection, halfVector));
		const float F = FresnelDielectric(vDotH, fromIor, toIor);
		const float distribution = DistributionGGX(
			worldNormal,
			halfVector,
			roughness);
		const float geometry = GeometrySmithGGXCorrelated(
			mirroredCosine,
			nDotV,
			roughness);
		const float denominator = (std::max)(
			4.0f * nDotV * mirroredCosine,
			DirectionEpsilon);
		return tint * upperLayerScaling * (1.0f - F) *
			distribution * geometry / denominator;
	}

	const float eta = ResolveIor(toIor) / ResolveIor(fromIor);
	vec3 halfVector = viewDirection + lightDirection * eta;
	const float halfVectorLengthSquared = dot(halfVector, halfVector);
	if (halfVectorLengthSquared <= DirectionEpsilon)
	{
		return vec3(0.0f);
	}
	halfVector *= inversesqrt(halfVectorLengthSquared);
	if (dot(worldNormal, halfVector) < 0.0f)
	{
		halfVector *= -1.0f;
	}

	const float vDotH = dot(viewDirection, halfVector);
	const float lDotH = dot(lightDirection, halfVector);
	if (vDotH <= 0.0f || lDotH >= 0.0f)
	{
		return vec3(0.0f);
	}
	const float mappingDenominator =
		lDotH + vDotH / eta;
	const float mappingDenominatorSquared =
		mappingDenominator * mappingDenominator;
	if (mappingDenominatorSquared <= DirectionEpsilon)
	{
		return vec3(0.0f);
	}

	const float F = FresnelDielectric(vDotH, fromIor, toIor);
	const float distribution = DistributionGGX(
		worldNormal,
		halfVector,
		roughness);
	const float geometry = GeometrySmithGGXCorrelated(
		abs(nDotL),
		nDotV,
		roughness);
	const float directionalDenominator =
		mappingDenominatorSquared * nDotV * abs(nDotL);
	const float radianceCorrection = 1.0f / (eta * eta);
	const float btdf =
		(1.0f - F) * distribution * geometry *
		abs(lDotH * vDotH / directionalDenominator) *
		radianceCorrection;
	return std::isfinite(btdf) ?
		tint * upperLayerScaling * btdf : vec3(0.0f);
}

vec3 LightingModel::CalculateBRDF(const vec3& viewDirection, const vec3& worldNormal, const vec3& lightDirection, const LightingModel::SampledData& sample)
{
	return CalculateBRDF(
		viewDirection,
		worldNormal,
		lightDirection,
		sample,
		1.0f,
		sample.m_ior);
}

vec3 LightingModel::CalculateBRDF(
	const vec3& viewDirection,
	const vec3& worldNormal,
	const vec3& lightDirection,
	const LightingModel::SampledData& sample,
	float fromIor,
	float toIor)
{
	const vec3 base = CalculateBaseBRDF(
		viewDirection,
		worldNormal,
		lightDirection,
		sample,
		fromIor,
		toIor);
	const vec3 sheen = CalculateSheenBRDF(
		viewDirection,
		worldNormal,
		lightDirection,
		sample);
	const float sheenBaseScaling = ResolveSheenBaseScaling(
		sample,
		dot(worldNormal, viewDirection),
		dot(worldNormal, lightDirection));
	const vec3 belowClearcoat = sheen + base * sheenBaseScaling;
	const float clearcoatWeight = ResolveClearcoatLayerWeight(
		sample,
		worldNormal,
		viewDirection);
	const vec3 clearcoat = CalculateClearcoatBRDF(
		viewDirection,
		worldNormal,
		lightDirection,
		sample);
	return mix(belowClearcoat, clearcoat, clearcoatWeight);
}

vec3 LightingModel::CalculateBRDFCosineWeighted(
	const vec3& viewDirection,
	const vec3& worldNormal,
	const vec3& lightDirection,
	const SampledData& sample,
	float fromIor,
	float toIor)
{
	const vec3 base = CalculateBaseBRDF(
		viewDirection,
		worldNormal,
		lightDirection,
		sample,
		fromIor,
		toIor);
	const vec3 sheen = CalculateSheenBRDF(
		viewDirection,
		worldNormal,
		lightDirection,
		sample);
	const float sheenBaseScaling = ResolveSheenBaseScaling(
		sample,
		dot(worldNormal, viewDirection),
		dot(worldNormal, lightDirection));
	const vec3 belowClearcoat =
		(sheen + base * sheenBaseScaling) *
		(std::max)(dot(worldNormal, lightDirection), 0.0f);
	const vec3 clearcoatNormal = ResolveClearcoatNormal(
		sample,
		worldNormal);
	const vec3 clearcoat = CalculateClearcoatBRDF(
		viewDirection,
		worldNormal,
		lightDirection,
		sample) * (std::max)(
			dot(clearcoatNormal, lightDirection),
			0.0f);
	const float clearcoatWeight = ResolveClearcoatLayerWeight(
		sample,
		worldNormal,
		viewDirection);
	return mix(belowClearcoat, clearcoat, clearcoatWeight);
}

vec3 LightingModel::CalculateEmittedRadiance(
	const SampledData& sample,
	const vec3& worldNormal,
	const vec3& viewDirection)
{
	return max(sample.m_emissive, vec3(0.0f)) *
		(1.0f - ResolveClearcoatLayerWeight(
			sample,
			worldNormal,
			viewDirection));
}

vec3 LightingModel::ImportanceSampleGGX(vec2 Xi, float roughness, const vec3& n)
{
	float a = std::max(roughness * roughness, 0.001f);

	float phi = 2.0f * glm::pi<float>() * Xi.x;
	float cosTheta = sqrt((1.0f - Xi.y) / (1.0f + (a * a - 1.0f) * Xi.y));
	float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

	vec3 H;
	H.x = sinTheta * cos(phi);
	H.y = sinTheta * sin(phi);
	H.z = cosTheta;

	vec3 up = abs(n.z) < 0.999f ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, n));
	vec3 bitangent = cross(n, tangent);

	vec3 sampleVec = tangent * H.x + bitangent * H.y + n * H.z;
	return normalize(sampleVec);
}

vec3 LightingModel::ImportanceSampleCharlie(
	vec2 Xi,
	float roughness,
	const vec3& n)
{
	const float clampedRoughness = clamp(roughness, 0.0f, 1.0f);
	const float alpha = (std::max)(
		clampedRoughness * clampedRoughness,
		0.000001f);
	const float sineTheta = pow(
		clamp(Xi.y, 0.000001f, 0.999999f),
		alpha / (2.0f * alpha + 1.0f));
	const float cosineTheta = sqrt((std::max)(
		1.0f - sineTheta * sineTheta,
		0.0f));
	const float phi = 2.0f * glm::pi<float>() * Xi.x;
	const vec3 localHalfVector(
		sineTheta * cos(phi),
		sineTheta * sin(phi),
		cosineTheta);

	const vec3 up = abs(n.z) < 0.999f ?
		vec3(0.0f, 0.0f, 1.0f) :
		vec3(1.0f, 0.0f, 0.0f);
	const vec3 tangent = normalize(cross(up, n));
	const vec3 bitangent = cross(n, tangent);
	return normalize(
		tangent * localHalfVector.x +
		bitangent * localHalfVector.y +
		n * localHalfVector.z);
}

float LightingModel::PowerHeuristic(int32_t nf, float fPdf, int32_t ng, float gPdf)
{
	float f = nf * fPdf;
	float g = ng * gPdf;
	const float denominator = f * f + g * g;
	return denominator > 0.0f ? (f * f) / denominator : 0.0f;
}

float LightingModel::ReflectionPdf(
	const SampledData& sample,
	const vec3& worldNormal,
	const vec3& viewDirection,
	const vec3& lightDirection)
{
	return ReflectionPdfForInterface(
		sample,
		worldNormal,
		viewDirection,
		lightDirection,
		1.0f,
		sample.m_ior,
		false);
}

vec3 LightingModel::ImportanceSampleLambert(vec2 Xi, const vec3& n)
{
	float phi = 2.0f * glm::pi<float>() * Xi.x;
	float cosTheta = sqrt(1.0f - Xi.y);
	float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

	vec3 s;
	s.x = sinTheta * cos(phi);
	s.y = sinTheta * sin(phi);
	s.z = cosTheta;

	vec3 up = abs(n.z) < 0.999f ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, n));
	vec3 bitangent = cross(n, tangent);

	vec3 sampleVec = tangent * s.x + bitangent * s.y + n * s.z;
	return normalize(sampleVec);
}

vec3 LightingModel::ImportanceSampleHemisphere(vec2 Xi, const vec3& n)
{
	float phi = 2.0f * glm::pi<float>() * Xi.x;
	float cosTheta = 1.0f - Xi.y;
	float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

	vec3 s;
	s.x = sinTheta * cos(phi);
	s.y = sinTheta * sin(phi);
	s.z = cosTheta;

	vec3 up = abs(n.z) < 0.999f ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, n));
	vec3 bitangent = cross(n, tangent);

	vec3 sampleVec = tangent * s.x + bitangent * s.y + n * s.z;
	return normalize(sampleVec);
}

float LightingModel::GGX_PDF(vec3 N, vec3 H, vec3 V, float roughness)
{
	const float vDotH = abs(dot(V, H));
	if (vDotH <= DirectionEpsilon)
	{
		return 0.0f;
	}
	const float result = GGXMicrofacetPdf(N, H, roughness) /
		(4.0f * vDotH);
	return std::isfinite(result) && result > 0.0f ? result : 0.0f;
}

vec3 LightingModel::CalculateRefraction(const vec3& rayDirection, const vec3& worldNormal, float fromIor, float toIor)
{
	const float eta = ResolveIor(fromIor) / ResolveIor(toIor);
	const float cosTheta = clamp(
		-glm::dot(worldNormal, rayDirection),
		0.0f,
		1.0f);
	const float transmittedCosineSquared =
		1.0f - eta * eta *
		(1.0f - cosTheta * cosTheta);
	if (transmittedCosineSquared <= 0.0f)
	{
		return vec3(0.0f);
	}

	const vec3 result =
		eta * rayDirection +
		(eta * cosTheta - sqrt(transmittedCosineSquared)) *
		worldNormal;
	const float lengthSquared = dot(result, result);
	if (lengthSquared <= DirectionEpsilon || !IsFinite(result))
	{
		return vec3(0.0f);
	}
	return result * inversesqrt(lengthSquared);
}

bool LightingModel::Sample(const SampledData& sample, const vec3& worldNormal, const vec3& viewDirection, float fromIor, float toIor, vec3& outTerm, float& outPdf, bool& bOutTransmissionRay, vec3& inOutDirection, vec2 randomSample, vec2 selectionSample)
{
	outTerm = vec3(0.0f);
	outPdf = 0.0f;
	bOutTransmissionRay = false;
	inOutDirection = vec3(0.0f);

	if (dot(worldNormal, viewDirection) <= 0.0f)
	{
		return false;
	}

	const LobeProbabilities probabilities =
		CalculateLobeProbabilities(
			sample,
			worldNormal,
			viewDirection,
			fromIor,
			toIor);
	const float totalProbability =
		probabilities.m_specular + probabilities.m_diffuse +
		probabilities.m_transmission + probabilities.m_sheen +
		probabilities.m_clearcoat;
	if (totalProbability <= 0.0f)
	{
		return false;
	}

	enum class ELobe
	{
		Specular,
		Diffuse,
		Transmission,
		Sheen,
		Clearcoat
	};

	const float selector = clamp(
		selectionSample.x,
		0.0f,
		std::nextafter(1.0f, 0.0f));
	ELobe lobe = ELobe::Clearcoat;
	if (selector < probabilities.m_specular)
	{
		lobe = ELobe::Specular;
	}
	else if (selector <
		probabilities.m_specular + probabilities.m_diffuse)
	{
		lobe = ELobe::Diffuse;
	}
	else if (selector <
		probabilities.m_specular + probabilities.m_diffuse +
		probabilities.m_transmission)
	{
		lobe = ELobe::Transmission;
	}
	else if (selector <
		probabilities.m_specular + probabilities.m_diffuse +
		probabilities.m_transmission + probabilities.m_sheen)
	{
		lobe = ELobe::Sheen;
	}

	const bool bThickVolume =
		probabilities.m_transmission > 0.0f &&
		sample.m_thicknessFactor > 0.0f;
	vec3 halfVector = worldNormal;
	if (lobe == ELobe::Specular || lobe == ELobe::Transmission)
	{
		halfVector = ImportanceSampleGGX(
			randomSample,
			sample.m_orm.y,
			worldNormal);
	}
	else if (lobe == ELobe::Sheen)
	{
		halfVector = ImportanceSampleCharlie(
			randomSample,
			sample.m_sheenRoughness,
			worldNormal);
	}
	else if (lobe == ELobe::Clearcoat)
	{
		halfVector = ImportanceSampleGGX(
			randomSample,
			sample.m_clearcoatRoughness,
			ResolveClearcoatNormal(sample, worldNormal));
	}

	if (lobe == ELobe::Diffuse)
	{
		inOutDirection = ImportanceSampleLambert(
			randomSample,
			worldNormal);
	}
	else if (lobe == ELobe::Specular ||
		lobe == ELobe::Sheen ||
		lobe == ELobe::Clearcoat)
	{
		inOutDirection =
			2.0f * dot(viewDirection, halfVector) * halfVector -
			viewDirection;
	}
	else if (bThickVolume)
	{
		if (abs(ResolveIor(fromIor) - ResolveIor(toIor)) <= MinIor)
		{
			inOutDirection = -viewDirection;
			bOutTransmissionRay = true;
			outPdf = probabilities.m_transmission;
			const float metallic = clamp(sample.m_orm.z, 0.0f, 1.0f);
			const float upperLayerScaling =
				ResolveSheenBaseScaling(
					sample,
					dot(worldNormal, viewDirection),
					dot(worldNormal, -viewDirection)) *
				(1.0f - ResolveClearcoatLayerWeight(
					sample,
					worldNormal,
					viewDirection));
			outTerm = max(vec3(sample.m_baseColor), vec3(0.0f)) *
				clamp(sample.m_transmission, 0.0f, 1.0f) *
				(1.0f - metallic) * upperLayerScaling / outPdf;
			return IsFinite(outTerm) && outPdf > 0.0f;
		}
		inOutDirection = CalculateRefraction(
			-viewDirection,
			halfVector,
			fromIor,
			toIor);
		if (inOutDirection == vec3(0.0f))
		{
			// The same sampled microfacet reflects all energy under total
			// internal reflection; routing it to reflection avoids a dark bias.
			lobe = ELobe::Specular;
			inOutDirection =
				2.0f * dot(viewDirection, halfVector) * halfVector -
				viewDirection;
		}
		else
		{
			bOutTransmissionRay = true;
		}
	}
	else
	{
		const vec3 reflected =
			2.0f * dot(viewDirection, halfVector) * halfVector -
			viewDirection;
		inOutDirection = reflected -
			2.0f * worldNormal * dot(reflected, worldNormal);
		bOutTransmissionRay = true;
	}

	const float directionLengthSquared = dot(
		inOutDirection,
		inOutDirection);
	if (directionLengthSquared <= DirectionEpsilon ||
		!IsFinite(inOutDirection))
	{
		return false;
	}
	inOutDirection *= inversesqrt(directionLengthSquared);
	const float nDotL = dot(worldNormal, inOutDirection);
	const float clearcoatNdotL = dot(
		ResolveClearcoatNormal(sample, worldNormal),
		inOutDirection);
	const bool bValidReflection = lobe == ELobe::Clearcoat ?
		clearcoatNdotL > 0.0f : nDotL > 0.0f;
	if ((!bOutTransmissionRay && !bValidReflection) ||
		(bOutTransmissionRay && nDotL >= 0.0f))
	{
		return false;
	}

	if (bOutTransmissionRay)
	{
		const float transmissionPdf = bThickVolume ?
			ThickTransmissionPdf(
				sample,
				worldNormal,
				viewDirection,
				inOutDirection,
				fromIor,
				toIor) :
			ThinTransmissionPdf(
				sample,
				worldNormal,
				viewDirection,
				inOutDirection);
		outPdf = probabilities.m_transmission * transmissionPdf;
	}
	else
	{
		outPdf = ReflectionPdfForInterface(
			sample,
			worldNormal,
			viewDirection,
			inOutDirection,
			fromIor,
			toIor,
			bThickVolume);
	}
	if (!std::isfinite(outPdf) || outPdf <= 0.0f)
	{
		return false;
	}

	if (bOutTransmissionRay)
	{
		const vec3 scattering = CalculateBTDF(
			viewDirection,
			worldNormal,
			inOutDirection,
			sample,
			fromIor,
			toIor,
			bThickVolume);
		outTerm = scattering * (abs(nDotL) / outPdf);
	}
	else
	{
		outTerm = CalculateBRDFCosineWeighted(
			viewDirection,
			worldNormal,
			inOutDirection,
			sample,
			fromIor,
			toIor) / outPdf;
	}
	if (!IsFinite(outTerm) ||
		any(lessThan(outTerm, vec3(0.0f))))
	{
		outTerm = vec3(0.0f);
		return false;
	}
	return true;
}

float LightingModel::IsotropicPhaseFunctionPDF()
{
	return 1.0f / (4.0f * Math::Pi);
}

float LightingModel::HenyeyGreensteinPhaseFunctionPDF(const vec3& viewDirection, const vec3& scatterDirection, float g)
{
	// Implement the Henyey-Greenstein phase function PDF calculation here
	// g is the anisotropy parameter, typically between -1 (backscatter) and 1 (forward scatter)
	float cosTheta = glm::dot(viewDirection, scatterDirection);
	float denominator = 1 + g * g - 2 * g * cosTheta;
	float HG = (1 - g * g) / (4 * Math::Pi * denominator * glm::sqrt(denominator));

	return HG;
}
