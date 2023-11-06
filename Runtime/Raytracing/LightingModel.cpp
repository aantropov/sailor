#include "LightingModel.h"
#include "Containers/Vector.h"
#include "Core/Utils.h"
#include "Math/Math.h"
#include "glm/glm/gtc/random.hpp"
#include "MaterialUtils.h"

using namespace glm;
using namespace Sailor;
using namespace Sailor::Math;
using namespace Sailor::Raytracing;

float LightingModel::DistributionBeckmann(const vec3& N, const vec3& H, float roughness)
{
	float alpha = std::max(roughness * roughness, 0.001f);
	float NdotH = std::max(std::abs(dot(N, H)), 0.0001f);
	float NdotH2 = NdotH * NdotH;

	float tan2Theta = (1.0f - NdotH2) / NdotH2;
	float alpha2 = alpha * alpha;

	float nom = std::exp(-tan2Theta / alpha2);
	float denom = glm::pi<float>() * alpha2 * NdotH2 * NdotH2;

	return nom / denom;
}

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

vec3 LightingModel::CalculateVolumetricBTDF(const vec3& viewDirection, const vec3& worldNormal, const vec3& lightDirection, const SampledData& sample, float envIor)
{
	const float metallic = sample.m_orm.z;
	const float transmission = sample.m_transmission;

	if (transmission <= 0.0f)
	{
		return vec3(0.0f);
	}

	const vec3 dielectricSpecular = vec3(0.04f);
	const vec3 F0 = mix(dielectricSpecular, vec3(sample.m_baseColor), metallic);
	// Ensure that the Fresnel term is always positive.
	vec3 F = FresnelSchlick(glm::clamp(glm::dot(viewDirection, worldNormal), 0.0f, 1.0f), F0);

	vec3 Ft = (1.0f - F) * transmission;
	float iorRatio = envIor / sample.m_ior;

	float cosThetaI = glm::clamp(glm::dot(viewDirection, worldNormal), 0.0f, 1.0f);
	float sinThetaTSquared = iorRatio * iorRatio * (1.0f - cosThetaI * cosThetaI);
	float transmissionCorrection = sinThetaTSquared <= 1.0f ? glm::sqrt(1.0f - sinThetaTSquared) : 0.0f;

	vec3 transmissionColor = vec3(sample.m_baseColor) * Ft * transmissionCorrection;

	return transmissionColor;
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

	const vec3 dielectricSpecular = vec3(0.04f);

	vec3 F0 = mix(dielectricSpecular, vec3(sample.m_baseColor), metallic);

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

vec3 LightingModel::ImportanceSampleBeckmann(vec2 Xi, float roughness, const vec3& n)
{
	float alpha = std::max(roughness * roughness, 0.001f);

	float phi = 2.0f * glm::pi<float>() * Xi.x;
	// The Beckmann sampling equations use tan^2(theta) instead of cos(theta)
	float tanTheta2 = -alpha * alpha * log(1.0f - Xi.y);
	float cosTheta = 1.0f / sqrt(1.0f + tanTheta2);
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

float LightingModel::PowerHeuristic(int nf, float fPdf, int ng, float gPdf)
{
	float f = nf * fPdf;
	float g = ng * gPdf;
	return (f * f) / (f * f + g * g);
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
	const float infCompensation = 0.001f;
	float a = std::max(roughness * roughness, infCompensation);
	float NdotH = std::max(dot(N, H), infCompensation);
	float VdotH = std::max(dot(V, H), infCompensation);

	//float D = a / (glm::pi<float>() * pow(NdotH * NdotH * (a - 1.0f) + 1.0f, 2.0f));
	float D = (a * a) / (glm::pi<float>() * pow(NdotH * NdotH * (a * a - 1.0f) + 1.0f, 2.0f));
	float pdf = D * NdotH / (4.0f * VdotH);

	return pdf;
}

float LightingModel::Beckmann_PDF(vec3 N, vec3 H, vec3 V, float roughness)
{
	const float infCompensation = 0.001f;
	float alpha = std::max(roughness * roughness, infCompensation);
	float NdotH = std::max(dot(N, H), infCompensation);
	float VdotH = std::max(dot(V, H), infCompensation);

	float tanThetaH = sqrt(1.0f - NdotH * NdotH) / NdotH;
	float tanThetaHSquared = tanThetaH * tanThetaH;
	float alphaSquared = alpha * alpha;

	// Computing Beckmann's D term (normal distribution function)
	float D = exp(-tanThetaHSquared / alphaSquared) / (glm::pi<float>() * alphaSquared * pow(NdotH, 4.0f));

	// The PDF using the D term
	float pdf = D * NdotH / (4.0f * VdotH);

	return pdf;
}

vec3 LightingModel::CalculateRefraction(const vec3& rayDirection, const vec3& worldNormal, float fromIor, float toIor)
{
	float eta = fromIor / toIor;

	float cosi = -glm::dot(worldNormal, rayDirection);
	float k = 1 - eta * eta * (1 - cosi * cosi);

	if (k < 0)
	{
		return glm::vec3(0.0f);
	}
	else
	{
		return normalize(eta * rayDirection + (eta * cosi - sqrt(k)) * worldNormal);
	}
}

bool LightingModel::Sample(const SampledData& sample, const vec3& worldNormal, const vec3& viewDirection, float fromIor, float toIor, vec3& outTerm, float& outPdf, bool& bOutTransmissionRay, vec3& inOutDirection, vec2 randomSample)
{
	const bool bFullMetallic = sample.m_orm.z == 1.0f;
	const bool bMirror = bFullMetallic && sample.m_orm.y <= 0.001f;
	const bool bOnlySpecularRay = bMirror;
	const bool bHasTransmission = !bFullMetallic && sample.m_transmission > 0.0f;
	const bool bIsThickVolume = bHasTransmission && sample.m_thicknessFactor > 0.0f;

	const bool bSpecular = bOnlySpecularRay || glm::linearRand(0.0f, 1.0f) > 0.5f;
	bOutTransmissionRay = bHasTransmission && (glm::linearRand(0.0f, 1.0f) > 0.5f);

	const float importanceRoughness = bSpecular ? sample.m_orm.y : 1.0f;
	const bool bSpecularBeckman = importanceRoughness < 0.2f;
	vec3 H;
	if (bSpecularBeckman)
	{
		H = bSpecular ? LightingModel::ImportanceSampleBeckmann(randomSample, sample.m_orm.y, worldNormal) :
			LightingModel::ImportanceSampleLambert(randomSample, worldNormal);
	}
	else
	{
		H = bSpecular ? LightingModel::ImportanceSampleGGX(randomSample, sample.m_orm.y, worldNormal) :
			LightingModel::ImportanceSampleLambert(randomSample, worldNormal);
	}

	if (inOutDirection == vec3(0, 0, 0))
	{
		inOutDirection = 2.0f * dot(viewDirection, H) * H - viewDirection;

		if (bOutTransmissionRay)
		{
			inOutDirection += 2.0f * worldNormal * dot(-inOutDirection, worldNormal);

			if (bIsThickVolume)
			{
				inOutDirection = CalculateRefraction(inOutDirection, worldNormal, fromIor, toIor);
				if (inOutDirection == vec3(0, 0, 0))
				{
					return false;
				}

				const float angle = abs(glm::dot(inOutDirection, worldNormal));
				float g = -0.55f;
				const vec3 scatterDirection = inOutDirection;
				outPdf = IsotropicPhaseFunctionPDF();
				if (g != 0.0f)
				{
					outPdf = HenyeyGreensteinPhaseFunctionPDF(viewDirection, scatterDirection, g);
				}

				outTerm = LightingModel::CalculateVolumetricBTDF(viewDirection, worldNormal, inOutDirection, sample, fromIor) * angle;
				outTerm /= outPdf;

				return true;
			}
		}
	}

	const float pdfSpec = bSpecularBeckman ?
		LightingModel::Beckmann_PDF(worldNormal, H, viewDirection, sample.m_orm.y) :
		LightingModel::GGX_PDF(worldNormal, H, viewDirection, sample.m_orm.y);

	const float pdfLambert = abs(dot(inOutDirection, worldNormal)) / Math::Pi;
	outPdf = bOnlySpecularRay ? pdfSpec : ((pdfSpec + pdfLambert) * 0.5f);

	if (bHasTransmission)
	{
		outPdf *= 0.5f;
	}

	if (!isnan(outPdf) && outPdf > 0.0001f)
	{
		const float angle = abs(glm::dot(inOutDirection, worldNormal));

		vec3 term = vec3(0.0f, 0.0f, 0.0f);
		if (bOutTransmissionRay)
		{
			term = LightingModel::CalculateBTDF(viewDirection, worldNormal, inOutDirection, sample);
		}
		else
		{
			term = LightingModel::CalculateBRDF(viewDirection, worldNormal, inOutDirection, sample);
		}

		const float weight = 1.0f / outPdf;

		outTerm = weight * term * angle;

		return true;
	}

	return false;
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