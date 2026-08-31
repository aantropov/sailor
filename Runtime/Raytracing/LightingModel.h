#pragma once
#include "Core/Defines.h"
#include "Engine/Types.h"
#include <glm/glm.hpp>

using namespace Sailor;
using namespace glm;

namespace Sailor::Raytracing
{
	struct DirectionalLight
	{
		vec3 m_direction{ 0.0f, -1.0f, 0.0f };
		vec3 m_intensity{ 1.0f,1.0f,1.0f };
	};

	struct LightProxy
	{
		ELightType m_type = ELightType::Point;
		vec3 m_worldPosition{ 0.0f, 0.0f, 0.0f };
		vec3 m_direction{ 0.0f, -1.0f, 0.0f };
		// Point and spot lights use candela; directional lights use lux.
		vec3 m_intensity{ 1.0f, 1.0f, 1.0f };
		float m_indirectLightingIntensity = 1.0f;
		// The x component stores the smooth local-light range in metres.
		vec3 m_bounds{ 100.0f, 100.0f, 100.0f };
		vec2 m_cutOff{ 0.0f, 0.0f };
		bool m_bCastShadows = true;

		bool operator==(const LightProxy& rhs) const
		{
			return m_type == rhs.m_type &&
				m_worldPosition == rhs.m_worldPosition &&
				m_direction == rhs.m_direction &&
				m_intensity == rhs.m_intensity &&
				m_indirectLightingIntensity == rhs.m_indirectLightingIntensity &&
				m_bounds == rhs.m_bounds &&
				m_cutOff == rhs.m_cutOff &&
				m_bCastShadows == rhs.m_bCastShadows;
		}
	};

	class LightingModel
	{
	public:

		struct SampledData
		{
			glm::vec4 m_baseColor{};
			glm::vec3 m_orm{};
			glm::vec3 m_emissive{};
			glm::vec3 m_normal{};
			glm::vec3 m_clearcoatNormal{};
			glm::vec3 m_sheenColor{};
			float m_clearcoatFactor = 0.0f;
			float m_clearcoatRoughness = 0.0f;
			float m_sheenRoughness = 0.0f;
			float m_ior = 1.5f;
			float m_thicknessFactor = 0.0f;
			float m_transmission = 0.0f;
			bool m_bIsOpaque = true;
		};

		static bool Sample(const SampledData& sample, const vec3& worldNormal, const vec3& viewDirection,
			float fromIor, float toIor, vec3& outTerm, float& outPdf, bool& bOutTransmissionRay, vec3& inOutDirection,
			vec2 randomSample, vec2 selectionSample);

		static vec3 CalculateBRDF(const vec3& viewDirection, const vec3& worldNormal, const vec3& lightDirection, const SampledData& sample);
		static vec3 CalculateBRDF(const vec3& viewDirection, const vec3& worldNormal, const vec3& lightDirection,
			const SampledData& sample, float fromIor, float toIor);
		static vec3 CalculateBRDFCosineWeighted(
			const vec3& viewDirection,
			const vec3& worldNormal,
			const vec3& lightDirection,
			const SampledData& sample,
			float fromIor,
			float toIor);
		static vec3 CalculateBTDF(const vec3& viewDirection, const vec3& worldNormal, const vec3& lightDirection,
			const SampledData& sample, float fromIor, float toIor, bool bThickVolume);
		static vec3 CalculateEmittedRadiance(
			const SampledData& sample,
			const vec3& worldNormal,
			const vec3& viewDirection);

		static vec3 CalculateRefraction(const vec3& rayDirection, const vec3& worldNormal, float fromIor, float toIor);

		static float GeometrySmithGGXCorrelated(
			float NdotL,
			float NdotV,
			float roughness);
		static vec3 FresnelSchlick(float cosTheta, const vec3& F0);
		static float FresnelDielectric(float cosTheta, float fromIor, float toIor);
		static float DistributionGGX(const vec3& N, const vec3& H, float roughness);
		static float DistributionCharlie(const vec3& N, const vec3& H, float roughness);
		static float VisibilitySheen(float NdotL, float NdotV, float roughness);

		static float PowerHeuristic(int nf, float fPdf, int ng, float gPdf);
		static float ReflectionPdf(
			const SampledData& sample,
			const vec3& worldNormal,
			const vec3& viewDirection,
			const vec3& lightDirection);
		static vec3 ImportanceSampleLambert(vec2 Xi, const vec3& n);
		static vec3 ImportanceSampleHemisphere(vec2 Xi, const vec3& n);
		static vec3 ImportanceSampleGGX(vec2 Xi, float roughness, const vec3& n);
		static vec3 ImportanceSampleCharlie(vec2 Xi, float roughness, const vec3& n);
		
		static float GGX_PDF(vec3 N, vec3 H, vec3 V, float roughness);
		static float IsotropicPhaseFunctionPDF();
		static float HenyeyGreensteinPhaseFunctionPDF(const vec3& viewDirection, const vec3& scatterDirection, float g);
	};
}
