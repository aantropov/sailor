#pragma once
#include "Core/Defines.h"
#include "glm/glm/glm.hpp"

using namespace Sailor;
using namespace glm;

namespace Sailor::Raytracing
{
	struct DirectionalLight
	{
		vec3 m_direction{ 0.0f, -1.0f, 0.0f };
		vec3 m_intensity{ 1.0f,1.0f,1.0f };
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
			float m_transmission = 0.0f;
			bool m_bIsOpaque = true;
		};

		static vec3 CalculateBRDF(const vec3& viewDirection, const vec3& worldNormal, const vec3& lightDirection, const SampledData& sample);
		static vec3 CalculateBTDF(const vec3& viewDirection, const vec3& worldNormal, const vec3& lightDirection, const SampledData& sample);

		static float GeometrySchlickGGX(float NdotV, float roughness);
		static vec3 FresnelSchlick(float cosTheta, const vec3& F0);
		static float DistributionGGX(const vec3& N, const vec3& H, float roughness);

		static float PowerHeuristic(int nf, float fPdf, int ng, float gPdf);
		static vec3 ImportanceSampleDiffuse(vec2 Xi, const vec3& n);
		static vec3 ImportanceSampleGGX(vec2 Xi, float roughness, const vec3& n);
		static float GGX_PDF(vec3 N, vec3 H, vec3 V, float roughness);
	};
}