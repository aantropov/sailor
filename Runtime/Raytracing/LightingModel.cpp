#include "LightingModel.h"
#include "Containers/Vector.h"
#include "Core/Utils.h"
#include "Math/Math.h"

using namespace glm;
using namespace Sailor;
using namespace Sailor::Math;
using namespace Sailor::Raytracing;

float LightingModel::DistributionGGX(const vec3& N, const vec3& H, float roughness)
{
	float a = std::max(roughness * roughness, 0.001f);
	float a2 = a * a;
	float NdotH = std::max(dot(N, H), 0.0001f);
	float NdotH2 = NdotH * NdotH;

	float nom = a2;
	float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
	denom = glm::pi<float>() * denom * denom;

	return nom / denom;
}

vec3 LightingModel::FresnelSchlick(float cosTheta, const vec3& F0)
{
	return F0 + (1.0f - F0) * glm::pow(1.0f - cosTheta, 5.0f);
}

float LightingModel::GeometrySchlickGGX(float NdotV, float roughness)
{
	float k = (roughness * roughness) / 2.0f; // Different k value calculation
	float denom = NdotV * (1.0f - k) + k;
	return NdotV / denom;
}

vec3 LightingModel::CalculateBTDF(const vec3& viewDirection, const vec3& worldNormal, const vec3& lDirection, const LightingModel::SampledData& sample)
{
	// Flip light direction
	const vec3 lightDirection = lDirection + 2.0f * worldNormal * dot(-lDirection, worldNormal);

	const float ambientOcclusion = sample.m_orm.x;
	const float roughness = sample.m_orm.y;
	const float metallic = sample.m_orm.z;
	const float transmission = sample.m_transmission;

	if (transmission <= 0.0f)
	{
		return vec3(0.0f);
	}

	const float nDotL = abs(glm::dot(worldNormal, lightDirection));
	const float nDotV = abs(glm::dot(worldNormal, viewDirection));

	const vec3 F0 = vec3(0.04f);

	const vec3 halfwayVector = normalize(viewDirection + lightDirection);
	const float NDF = DistributionGGX(worldNormal, halfwayVector, roughness);
	const vec3 F = FresnelSchlick(std::max(abs(glm::dot(halfwayVector, viewDirection)), 0.0f), F0);

	float geomA = GeometrySchlickGGX(nDotL, roughness);
	float geomB = GeometrySchlickGGX(nDotV, roughness);
	float geometricTerm = geomA * geomB;

	vec3 kT = (1.0f - F) * transmission * (1.0f - metallic) * sample.m_baseColor.xyz;

	// TODO: Two sided
	if (nDotL < 0.0f || nDotV < 0.0f)
	{
		return vec3(0.0f);
	}

	float denominator = (4.0f * std::max(nDotV, 0.0f) * std::max(nDotL, 0.0f)) + 0.001f;
	vec3 specularTerm = (kT * NDF * geometricTerm) / denominator;
	vec3 lighting = (specularTerm);

	return lighting;
}

vec3 LightingModel::CalculateBRDF(const vec3& viewDirection, const vec3& worldNormal, const vec3& lightDirection, const LightingModel::SampledData& sample)
{
	const float ambientOcclusion = sample.m_orm.x;
	const float roughness = sample.m_orm.y;
	const float metallic = sample.m_orm.z;

	const float nDotL = glm::dot(worldNormal, lightDirection);
	const float nDotV = glm::dot(worldNormal, viewDirection);

	vec3 F0 = mix(vec3(0.04f), vec3(sample.m_baseColor), metallic);

	vec3 halfwayVector = normalize(viewDirection + lightDirection);
	float NDF = DistributionGGX(worldNormal, halfwayVector, roughness);
	vec3 F = FresnelSchlick(std::max(glm::dot(halfwayVector, viewDirection), 0.0f), F0);
	float geomA = GeometrySchlickGGX(nDotL, roughness);
	float geomB = GeometrySchlickGGX(nDotV, roughness);
	float geometricTerm = geomA * geomB;

	vec3 kS = F;
	vec3 kD = vec3(1.0f) - kS;
	kD *= 1.0f - metallic;
	kD *= 1.0f - sample.m_transmission;

	// TODO: Two sided
	if (nDotL < 0.0f || nDotV < 0.0f)
	{
		return vec3(0.0f);
	}

	float denominator = (4.0f * std::max(nDotV, 0.0f) * std::max(nDotL, 0.0f)) + 0.001f;
	vec3 specularTerm = (F * NDF * geometricTerm) / denominator;
	vec3 diffuseTerm = (kD * sample.m_baseColor.xyz) / glm::pi<float>();

	vec3 lighting = (diffuseTerm + specularTerm);// *(vec3(1.0f) - vec3(ambientOcclusion));

	return lighting;
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

float LightingModel::GGX_PDF(vec3 N, vec3 H, vec3 V, float roughness)
{
	const float infCompensation = 0.001f;
	float a = std::max(roughness * roughness, infCompensation);
	float NdotH = std::max(dot(N, H), infCompensation);
	float VdotH = std::max(dot(V, H), infCompensation);

	//float D = a / (glm::pi<float>() * pow(NdotH * NdotH * (a - 1.0f) + 1.0f, 2.0f));
	float D = (a * a) / (glm::pi<float>() * pow(NdotH * NdotH * (a * a - 1.0f) + 1.0f, 2.0f));
	float pdf = D * NdotH / (4.0f * VdotH);

	return pdf;
}