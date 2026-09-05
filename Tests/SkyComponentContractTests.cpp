#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "Components/LightComponent.h"
#include "Components/SkyComponent.h"
#include "Core/Reflection.h"
#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "FrameGraph/SkyParameters.h"
#include "Engine/World.h"
#include "FrameGraph/SkyNode.h"
#include "Raytracing/SkyEnvironmentGenerator.h"

using namespace Sailor;

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	bool IsNear(float lhs, float rhs, float tolerance = 0.0001f)
	{
		const float scale = std::max({ 1.0f, std::abs(lhs), std::abs(rhs) });
		return std::abs(lhs - rhs) <= tolerance * scale;
	}

	bool IsNear(const glm::vec3& lhs, const glm::vec3& rhs, float tolerance = 0.0001f)
	{
		return IsNear(lhs.x, rhs.x, tolerance) &&
			IsNear(lhs.y, rhs.y, tolerance) &&
			IsNear(lhs.z, rhs.z, tolerance);
	}

	bool IsNear(const glm::vec4& lhs, const glm::vec4& rhs, float tolerance = 0.0001f)
	{
		return IsNear(lhs.x, rhs.x, tolerance) &&
			IsNear(lhs.y, rhs.y, tolerance) &&
			IsNear(lhs.z, rhs.z, tolerance) &&
			IsNear(lhs.w, rhs.w, tolerance);
	}

	glm::vec4 ExpectedLightDirection(float sunAngleDegrees)
	{
		const float sunAngleRadians = glm::radians(sunAngleDegrees);
		const glm::vec2 horizontalDirection =
			glm::normalize(glm::vec2(0.2f, 1.0f));
		const float horizontalScale = std::cos(sunAngleRadians);
		return glm::vec4(
			horizontalDirection.x * horizontalScale,
			-std::sin(sunAngleRadians),
			horizontalDirection.y * horizontalScale,
			0.0f);
	}

	glm::mat4 CalculateWorldMatrix(GameObjectPtr gameObject)
	{
		if (!gameObject)
		{
			return glm::mat4(1.0f);
		}

		const glm::mat4 local =
			gameObject->GetTransformComponent().GetTransform().Matrix();
		const GameObjectPtr& parent = gameObject->GetParent();
		return parent
			? CalculateWorldMatrix(parent) * local
			: local;
	}


	class SkyTestWorld final : public World
	{
	public:

		SkyTestWorld() :
			World("SkyComponentContractTests", 0, CreateEcs()) {}

	private:

		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<LightingECS>::Make());
			return systems;
		}
	};

	class SkyPrefabTestAsset final : public Prefab
	{
	public:

		SkyPrefabTestAsset(const FileId& fileId) :
			Prefab(fileId) {}

		static PrefabPtr Capture(
			SkyTestWorld& world,
			GameObjectPtr root)
		{
			auto result =
				TObjectPtr<SkyPrefabTestAsset>::Make(
					world.GetAllocator(),
					FileId::Invalid);
			SerializeGameObject(
				root,
				static_cast<uint32_t>(-1),
				result->m_components,
				result->m_gameObjects,
				nullptr);

			std::string diagnostic;
			result->m_bIsReady.store(
				result->ValidateForInstantiation(diagnostic),
				std::memory_order_release);
			Require(result->IsReady(),
				"captured sky prefab should be valid: " +
					diagnostic);
			return result;
		}
	};

	class SkyNodeMailboxProbe final :
		public Framegraph::SkyNode
	{
	public:

		using Framegraph::SkyNode::ConsumePendingSkyParams;
		using Framegraph::SkyNode::CreateEnvironmentProjectionMatrix;
		using Framegraph::SkyNode::CreateEnvironmentViewMatrices;
	};

	glm::vec3 ReconstructEnvironmentDirection(
		const glm::mat4& projection,
		const glm::mat4& view,
		const glm::vec2& uv)
	{
		glm::vec4 viewDirection = glm::inverse(projection) *
			glm::vec4(uv * 2.0f - 1.0f, 1.0f, 1.0f);
		viewDirection /= viewDirection.w;
		return glm::normalize(glm::vec3(
			glm::inverse(view) * glm::vec4(
				glm::vec3(viewDirection), 0.0f)));
	}

	void TestEnvironmentCubemapOrientation()
	{
		const glm::mat4 projection =
			SkyNodeMailboxProbe::CreateEnvironmentProjectionMatrix();
		const auto views =
			SkyNodeMailboxProbe::CreateEnvironmentViewMatrices();
		Require(views.Num() == 6,
			"the procedural environment should define all six cubemap faces");

		const glm::vec3 centerDirections[] = {
			Math::vec3_Right, Math::vec3_Left,
			Math::vec3_Up, Math::vec3_Down,
			Math::vec3_Backward, Math::vec3_Forward
		};
		const glm::vec3 rightDirections[] = {
			glm::normalize(Math::vec3_Right + Math::vec3_Forward),
			glm::normalize(Math::vec3_Left + Math::vec3_Backward),
			glm::normalize(Math::vec3_Up + Math::vec3_Right),
			glm::normalize(Math::vec3_Down + Math::vec3_Right),
			glm::normalize(Math::vec3_Backward + Math::vec3_Right),
			glm::normalize(Math::vec3_Forward + Math::vec3_Left)
		};
		const glm::vec3 topDirections[] = {
			glm::normalize(Math::vec3_Right + Math::vec3_Up),
			glm::normalize(Math::vec3_Left + Math::vec3_Up),
			glm::normalize(Math::vec3_Up + Math::vec3_Forward),
			glm::normalize(Math::vec3_Down + Math::vec3_Backward),
			glm::normalize(Math::vec3_Backward + Math::vec3_Up),
			glm::normalize(Math::vec3_Forward + Math::vec3_Up)
		};

		for (uint32_t face = 0; face < views.Num(); face++)
		{
			Require(
				IsNear(ReconstructEnvironmentDirection(
					projection, views[face], glm::vec2(0.5f)),
					centerDirections[face]) &&
				IsNear(ReconstructEnvironmentDirection(
					projection, views[face], glm::vec2(1.0f, 0.5f)),
					rightDirections[face]) &&
				IsNear(ReconstructEnvironmentDirection(
					projection, views[face], glm::vec2(0.5f, 1.0f)),
					topDirections[face]),
				"procedural environment face orientation should match Vulkan cubemap sampling");
		}
	}

	void RequireRange(
		const TypeInfo& type,
		const std::string& property,
		double expectedMin,
		double expectedMax)
	{
		const auto range = type.PropertyRanges().Find(property);
		Require(range != type.PropertyRanges().end(),
			"SkyComponent should expose a range for " + property);
		Require(
			range.Value().m_min == expectedMin &&
				range.Value().m_max == expectedMax,
			"SkyComponent should preserve the declared range for " +
				property);
	}

	void RequireFloatClamp(
		SkyComponent& component,
		void (SkyComponent::*setter)(float),
		float (SkyComponent::*getter)() const,
		float minimum,
		float maximum,
		const std::string& property)
	{
		(component.*setter)(minimum - 1000.0f);
		Require(IsNear((component.*getter)(), minimum),
			property + " should clamp values below its minimum");
		(component.*setter)(maximum + 1000.0f);
		Require(IsNear((component.*getter)(), maximum),
			property + " should clamp values above its maximum");
	}

	void RequireIntClamp(
		SkyComponent& component,
		void (SkyComponent::*setter)(int32_t),
		int32_t (SkyComponent::*getter)() const,
		int32_t minimum,
		int32_t maximum,
		const std::string& property)
	{
		(component.*setter)(minimum - 1000);
		Require((component.*getter)() == minimum,
			property + " should clamp values below its minimum");
		(component.*setter)(maximum + 1000);
		Require((component.*getter)() == maximum,
			property + " should clamp values above its maximum");
	}

	bool IsNullObjectReference(const YAML::Node& node)
	{
		return !node.IsDefined() ||
			node.IsNull() ||
			(!node["fileId"] && !node["instanceId"]);
	}


	void TestSkyParameterGpuLayoutAndDefaults()
	{
		static_assert(
			std::is_standard_layout_v<SkyParameters>,
			"SkyParameters must remain suitable for direct UBO upload");
		static_assert(
			sizeof(SkyParameters) == 116,
			"SkyParameters must preserve its shader-facing byte size");

		Require(offsetof(SkyParameters, m_lightDirection) == 0,
			"light direction should start the sky UBO");
		Require(offsetof(SkyParameters, m_sunIlluminance) == 16,
			"sun illuminance should follow the vec4 direction");
		Require(offsetof(SkyParameters, m_groundRadiance) == 32,
			"ground radiance should follow sun illuminance");
		Require(offsetof(SkyParameters, m_cloudsAttenuation1) == 48,
			"cloud attenuation 1 should follow ground radiance");
		Require(offsetof(SkyParameters, m_cloudsAttenuation2) == 52,
			"cloud attenuation 2 should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_cloudsDensity) == 56,
			"cloud density should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_cloudsCoverage) == 60,
			"cloud coverage should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_phaseInfluence1) == 64,
			"phase influence 1 should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_phaseInfluence2) == 68,
			"phase influence 2 should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_eccentrisy1) == 72,
			"eccentricity 1 should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_eccentrisy2) == 76,
			"eccentricity 2 should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_fog) == 80,
			"horizon blend should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_cloudScatteringScale) == 84,
			"cloud scattering scale should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_ambient) == 88,
			"ambient should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_scatteringSteps) == 92,
			"scattering steps should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_scatteringDensity) == 96,
			"scattering density should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_scatteringIntensity) == 100,
			"scattering intensity should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_scatteringPhase) == 104,
			"scattering phase should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_sunShaftsIntensity) == 108,
			"sun shafts intensity should preserve its UBO offset");
		Require(offsetof(SkyParameters, m_sunShaftsDistance) == 112,
			"sun shafts distance should preserve its UBO offset");

		const SkyParameters defaults;
		Require(
			IsNear(
				defaults.m_lightDirection,
				Math::SafeNormalize(
					glm::vec4(0.0f, -1.0f, 1.0f, 0.0f),
					Math::vec4_Down)),
			"SkyParameters should preserve the shader defaults");
		Require(
			IsNear(defaults.m_sunIlluminance, glm::vec4(120000.0f, 120000.0f, 120000.0f, 0.0f)) &&
				IsNear(defaults.m_groundRadiance, glm::vec4(0.0f)) &&
				IsNear(defaults.m_cloudsAttenuation1, 0.3f) &&
				IsNear(defaults.m_cloudsAttenuation2, 0.06f) &&
				IsNear(defaults.m_cloudsDensity, 0.3f) &&
				IsNear(defaults.m_cloudsCoverage, 0.56f) &&
				IsNear(defaults.m_phaseInfluence1, 0.025f) &&
				IsNear(defaults.m_phaseInfluence2, 0.9f) &&
				IsNear(defaults.m_eccentrisy1, 0.95f) &&
				IsNear(defaults.m_eccentrisy2, 0.51f) &&
				IsNear(defaults.m_fog, 10.0f) &&
				IsNear(defaults.m_cloudScatteringScale, 1.0f) &&
				IsNear(defaults.m_ambient, 0.5f) &&
				defaults.m_scatteringSteps == 5 &&
				IsNear(defaults.m_scatteringDensity, 0.5f) &&
				IsNear(defaults.m_scatteringIntensity, 0.5f) &&
				IsNear(defaults.m_scatteringPhase, 0.5f) &&
				IsNear(defaults.m_sunShaftsIntensity, 0.45f) &&
				defaults.m_sunShaftsDistance == 60,
			"SkyParameters scalar defaults should match the authored sky");

		SkyTestWorld world;
		auto owner = world.Instantiate("Sky");
		auto sky = owner->AddComponent<SkyComponent>();
		Require(
			IsNear(sky->GetSunAngle(), 60.0f) &&
				IsNear(sky->GetGiIndirectIntensity(), 1.0f) &&
				IsNear(
					sky->GetSkyParameters().m_lightDirection,
					ExpectedLightDirection(60.0f)) &&
				IsNear(
					sky->GetSunIlluminance(),
					glm::vec3(120000.0f)) &&
				!sky->GetDirectionalLight(),
			"SkyComponent should expose stable component defaults");
		world.Clear();
	}

	void TestReflectionMetadataRangesAndStableLightType()
	{
		const TypeInfo& type = TypeInfo::Get<SkyComponent>();
		Require(type.Name() == "Sailor::SkyComponent",
			"SkyComponent should expose its stable reflected typename");
		Require(type.Base() == "Sailor::Component",
			"SkyComponent should remain a reflected Component");
		Require(
			type.Properties().ContainsKey("m_directionalLight") &&
				type.Properties()["m_directionalLight"] ==
					"TObjectPtr<Sailor::LightComponent>",
			"m_directionalLight metadata should use a stable TObjectPtr typename");

		RequireRange(type, "sunAngle", -25.0, 89.0);
		RequireRange(type, "cloudsDensity", 0.0, 1.0);
		RequireRange(type, "cloudsCoverage", 0.0, 2.0);
		RequireRange(type, "cloudsAttenuation1", 0.1, 0.3);
		RequireRange(type, "cloudsAttenuation2", 0.001, 0.1);
		RequireRange(type, "cloudsPhaseInfluence1", 0.0, 1.0);
		RequireRange(type, "cloudsPhaseEccentricity1", 0.0, 1.0);
		RequireRange(type, "cloudsPhaseInfluence2", 0.0, 1.0);
		RequireRange(type, "cloudsPhaseEccentricity2", 0.01, 1.0);
		RequireRange(type, "cloudsHorizonBlend", 0.0, 20.0);
		RequireRange(type, "cloudScatteringScale", 0.0, 8.0);
		RequireRange(type, "ambient", 0.0, 10.0);
		RequireRange(type, "giIndirectIntensity", 0.0, 16.0);
		RequireRange(type, "scatteringSteps", 1.0, 10.0);
		RequireRange(type, "scatteringDensity", 0.1, 1.0);
		RequireRange(type, "scatteringIntensity", 0.01, 1.0);
		RequireRange(type, "scatteringPhase", 0.001, 1.0);
		RequireRange(type, "sunShaftsDistance", 1.0, 100.0);
		RequireRange(type, "sunShaftsIntensity", 0.001, 1.0);

		Require(
			!type.PropertyRanges().ContainsKey("m_directionalLight") &&
				!type.PropertyRanges().ContainsKey(
					"sunIlluminance"),
			"non-scalar light properties should not advertise slider ranges");

		const YAML::Node metadata = type.Serialize();
		Require(
			metadata["properties"]["m_directionalLight"].
				as<std::string>() ==
					"TObjectPtr<Sailor::LightComponent>" &&
				metadata["propertyRanges"]["sunAngle"]["min"].
					as<double>() == -25.0 &&
				metadata["propertyRanges"]["sunAngle"]["max"].
					as<double>() == 89.0,
			"serialized editor metadata should preserve type names and ranges");

		const ReflectedData& cdo =
			Reflection::GetCDO(type.Name());
		Require(
			cdo.GetProperties()["sunAngle"].as<float>() == 60.0f &&
				cdo.GetProperties()["cloudsDensity"].
					as<float>() == 0.3f &&
				cdo.GetProperties()["cloudsCoverage"].
					as<float>() == 0.56f &&
				cdo.GetProperties()["cloudScatteringScale"].
					as<float>() == 1.0f &&
				cdo.GetProperties()["giIndirectIntensity"].
					as<float>() == 1.0f &&
				cdo.GetProperties()["scatteringSteps"].
					as<int32_t>() == 5 &&
				IsNullObjectReference(
					cdo.GetProperties()["m_directionalLight"]),
			"the reflected CDO should expose the authored SkyComponent defaults");
	}

	void TestSettersClampAndUpdateDirection()
	{
		SkyTestWorld world;
		auto owner = world.Instantiate("Sky");
		auto sky = owner->AddComponent<SkyComponent>();
		SkyComponent& component = *sky;

		RequireFloatClamp(
			component,
			&SkyComponent::SetSunAngle,
			&SkyComponent::GetSunAngle,
			-25.0f,
			89.0f,
			"sunAngle");
		Require(
			IsNear(
				component.GetSkyParameters().m_lightDirection,
				ExpectedLightDirection(89.0f)),
			"clamped sunAngle should immediately update light direction");
		Require(
			IsNear(
				-component.GetSkyParameters().m_lightDirection.y,
				std::sin(glm::radians(89.0f))),
			"sunAngle should represent the physical solar elevation");

		RequireFloatClamp(
			component,
			&SkyComponent::SetCloudsDensity,
			&SkyComponent::GetCloudsDensity,
			0.0f,
			1.0f,
			"cloudsDensity");
		RequireFloatClamp(
			component,
			&SkyComponent::SetCloudsCoverage,
			&SkyComponent::GetCloudsCoverage,
			0.0f,
			2.0f,
			"cloudsCoverage");
		RequireFloatClamp(
			component,
			&SkyComponent::SetCloudsAttenuation1,
			&SkyComponent::GetCloudsAttenuation1,
			0.1f,
			0.3f,
			"cloudsAttenuation1");
		RequireFloatClamp(
			component,
			&SkyComponent::SetCloudsAttenuation2,
			&SkyComponent::GetCloudsAttenuation2,
			0.001f,
			0.1f,
			"cloudsAttenuation2");
		RequireFloatClamp(
			component,
			&SkyComponent::SetCloudsPhaseInfluence1,
			&SkyComponent::GetCloudsPhaseInfluence1,
			0.0f,
			1.0f,
			"cloudsPhaseInfluence1");
		RequireFloatClamp(
			component,
			&SkyComponent::SetCloudsPhaseEccentricity1,
			&SkyComponent::GetCloudsPhaseEccentricity1,
			0.0f,
			1.0f,
			"cloudsPhaseEccentricity1");
		RequireFloatClamp(
			component,
			&SkyComponent::SetCloudsPhaseInfluence2,
			&SkyComponent::GetCloudsPhaseInfluence2,
			0.0f,
			1.0f,
			"cloudsPhaseInfluence2");
		RequireFloatClamp(
			component,
			&SkyComponent::SetCloudsPhaseEccentricity2,
			&SkyComponent::GetCloudsPhaseEccentricity2,
			0.01f,
			1.0f,
			"cloudsPhaseEccentricity2");
		RequireFloatClamp(
			component,
			&SkyComponent::SetCloudsHorizonBlend,
			&SkyComponent::GetCloudsHorizonBlend,
			0.0f,
			20.0f,
			"cloudsHorizonBlend");
		RequireFloatClamp(
			component,
			&SkyComponent::SetCloudScatteringScale,
			&SkyComponent::GetCloudScatteringScale,
			0.0f,
			8.0f,
			"cloudScatteringScale");
		RequireFloatClamp(
			component,
			&SkyComponent::SetAmbient,
			&SkyComponent::GetAmbient,
			0.0f,
			10.0f,
			"ambient");
		const SkyParameters skyBeforeGiIntensity =
			component.GetSkyParameters();
		RequireFloatClamp(
			component,
			&SkyComponent::SetGiIndirectIntensity,
			&SkyComponent::GetGiIndirectIntensity,
			0.0f,
			16.0f,
			"giIndirectIntensity");
		Require(
			component.GetSkyParameters() == skyBeforeGiIntensity,
			"GI indirect intensity must remain a bake-only SkyComponent control");
		component.SetGiIndirectIntensity(
			(std::numeric_limits<float>::quiet_NaN)());
		Require(IsNear(component.GetGiIndirectIntensity(), 0.0f),
			"non-finite GI indirect intensity should resolve to zero");
		RequireIntClamp(
			component,
			&SkyComponent::SetScatteringSteps,
			&SkyComponent::GetScatteringSteps,
			1,
			10,
			"scatteringSteps");
		RequireFloatClamp(
			component,
			&SkyComponent::SetScatteringDensity,
			&SkyComponent::GetScatteringDensity,
			0.1f,
			1.0f,
			"scatteringDensity");
		RequireFloatClamp(
			component,
			&SkyComponent::SetScatteringIntensity,
			&SkyComponent::GetScatteringIntensity,
			0.01f,
			1.0f,
			"scatteringIntensity");
		RequireFloatClamp(
			component,
			&SkyComponent::SetScatteringPhase,
			&SkyComponent::GetScatteringPhase,
			0.001f,
			1.0f,
			"scatteringPhase");
		RequireIntClamp(
			component,
			&SkyComponent::SetSunShaftsDistance,
			&SkyComponent::GetSunShaftsDistance,
			1,
			100,
			"sunShaftsDistance");
		RequireFloatClamp(
			component,
			&SkyComponent::SetSunShaftsIntensity,
			&SkyComponent::GetSunShaftsIntensity,
			0.001f,
			1.0f,
			"sunShaftsIntensity");

		component.SetSunIlluminance(
			glm::vec3(-10.0f, 2.0f, -3.0f));
		Require(
			IsNear(
				component.GetSunIlluminance(),
				glm::vec3(0.0f, 2.0f, 0.0f)),
			"sun illuminance should clamp each channel to zero");
		Require(
			IsNear(
				component.GetSkyParameters().m_sunIlluminance,
				glm::vec4(0.0f, 2.0f, 0.0f, 0.0f)),
			"sun illuminance should update the shader parameters immediately");

		world.Clear();
	}

	void TestEnvironmentKeyHashEquality()
	{
		SkyParameters lhs;
		SkyParameters rhs;
		Require(
			lhs.GetEnvironmentKey() == rhs.GetEnvironmentKey() &&
				lhs.GetEnvironmentKey().GetHash() ==
					rhs.GetEnvironmentKey().GetHash(),
			"equal default sky environments should have equal keys and hashes");

		lhs.m_sunIlluminance = glm::vec4(100000.0f, 95000.0f, 90000.0f, 0.0f);
		rhs.m_sunIlluminance = glm::vec4(120000.0f, 115000.0f, 110000.0f, 0.0f);
		Require(
			!(lhs.GetEnvironmentKey() == rhs.GetEnvironmentKey()),
			"sun illuminance should invalidate the physical sky environment");

		lhs.m_sunIlluminance = rhs.m_sunIlluminance = glm::vec4(120000.0f, 120000.0f, 120000.0f, 0.0f);
		lhs.m_lightDirection =
			glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
		rhs.m_lightDirection =
			glm::vec4(1.01f, 0.01f, 0.0f, 0.0f);
		Require(
			lhs.GetEnvironmentKey() == rhs.GetEnvironmentKey() &&
				lhs.GetEnvironmentKey().GetHash() ==
					rhs.GetEnvironmentKey().GetHash(),
			"directions in one quantized bucket should share an environment key");

		rhs.m_lightDirection =
			glm::vec4(1.1f, 0.0f, 0.0f, 0.0f);
		Require(
			!(lhs.GetEnvironmentKey() == rhs.GetEnvironmentKey()),
			"crossing a direction bucket should invalidate the environment key");

		lhs.m_lightDirection = Math::vec4_Up;
		rhs.m_lightDirection =
			Math::SafeNormalize(
				glm::vec4(0.01f, 1.0f, 0.01f, 0.0f),
				Math::vec4_Up);
		const SkyEnvironmentKey lhsFallback =
			lhs.GetEnvironmentKey();
		const SkyEnvironmentKey rhsFallback =
			rhs.GetEnvironmentKey();
		Require(
			!lhsFallback.m_bUsesLightDirection &&
				!rhsFallback.m_bUsesLightDirection &&
				lhsFallback == rhsFallback &&
				lhsFallback.GetHash() == rhsFallback.GetHash(),
			"below-horizon directions should share the fallback environment key");
	}

	void TestTransientBakeEnvironmentUsesClearSkyParameters()
	{
		SkyParameters source;
		source.m_lightDirection = ExpectedLightDirection(23.5f);
		const glm::uvec2 extent(24u, 12u);
		TVector<glm::vec4> environment;
		uint32_t finalCompletedRows = 0u;
		Require(
			Raytracing::GenerateSkyEnvironmentEquirectangular(
				source,
				extent,
				environment,
				[&finalCompletedRows](
					uint32_t completedRows,
					uint32_t)
				{
					finalCompletedRows = completedRows;
					return true;
				}),
			"the probe baker should generate a transient longitude sky map");
		Require(
			environment.Num() ==
				static_cast<size_t>(extent.x) * extent.y &&
			finalCompletedRows == extent.y,
			"the transient sky map should fill every requested equirectangular texel");

		float strongestSky = 0.0f;
		for (uint32_t y = 0u; y < extent.y; ++y)
		{
			for (uint32_t x = 0u; x < extent.x; ++x)
			{
				const glm::vec4 sample = environment[x + y * extent.x];
				Require(
					std::isfinite(sample.x) &&
					std::isfinite(sample.y) &&
					std::isfinite(sample.z) &&
					sample.x >= 0.0f && sample.y >= 0.0f &&
					sample.z >= 0.0f && sample.w == 1.0f,
					"the transient clear-sky radiance should stay finite and non-negative");
				strongestSky = std::max(
					strongestSky,
					glm::length(glm::vec3(sample)));
				if (y >= extent.y / 2u)
				{
					Require(
						glm::length(glm::vec3(sample)) <= 0.000001f,
						"the no-cloud bake map should keep the opaque lower hemisphere black");
				}
			}
		}
		Require(strongestSky > 0.01f,
			"the upper hemisphere should contain atmospheric lighting");

		SkyParameters cloudOnlyChanges = source;
		cloudOnlyChanges.m_cloudsDensity = 1.0f;
		cloudOnlyChanges.m_cloudsCoverage = 2.0f;
		cloudOnlyChanges.m_cloudScatteringScale = 8.0f;
		cloudOnlyChanges.m_ambient = 10.0f;
		cloudOnlyChanges.m_scatteringSteps = 10;
		cloudOnlyChanges.m_scatteringDensity = 1.0f;
		cloudOnlyChanges.m_scatteringIntensity = 1.0f;
		cloudOnlyChanges.m_scatteringPhase = 1.0f;
		TVector<glm::vec4> withoutClouds;
		Require(
			Raytracing::GenerateSkyEnvironmentEquirectangular(
				cloudOnlyChanges,
				extent,
				withoutClouds),
			"cloud parameters should not prevent clear-sky map generation");
		Require(withoutClouds.Num() == environment.Num(),
			"cloud-free map variants should keep the same extent");
		for (size_t index = 0u; index < environment.Num(); ++index)
		{
			Require(
				withoutClouds[index] == environment[index],
				"cloud and cloud-scattering controls must not enter the bake sky map");
		}

		SkyParameters lowSun = source;
		lowSun.m_sunIlluminance = glm::vec4(1000.0f, 900.0f, 800.0f, 0.0f);
		SkyParameters halfSun = lowSun;
		halfSun.m_sunIlluminance *= 0.5f;
		TVector<glm::vec4> lowSunEnvironment;
		TVector<glm::vec4> halfSunEnvironment;
		Require(
			Raytracing::GenerateSkyEnvironmentEquirectangular(
				lowSun,
				extent,
				lowSunEnvironment) &&
			Raytracing::GenerateSkyEnvironmentEquirectangular(
				halfSun,
				extent,
				halfSunEnvironment),
			"physical sky variants should generate for different sun illuminance");
		for (size_t index = 0u; index < lowSunEnvironment.Num(); ++index)
		{
			Require(
				IsNear(
					halfSunEnvironment[index],
					lowSunEnvironment[index] *
						glm::vec4(0.5f, 0.5f, 0.5f, 1.0f),
					0.0001f),
				"clear-sky radiance should scale linearly with sun illuminance");
		}

		SkyParameters movedSun = source;
		movedSun.m_lightDirection = ExpectedLightDirection(70.0f);
		TVector<glm::vec4> movedEnvironment;
		Require(
			Raytracing::GenerateSkyEnvironmentEquirectangular(
				movedSun,
				extent,
				movedEnvironment),
			"a second sun direction should generate another transient map");
		bool bLightingChanged = false;
		for (size_t index = 0u; index < environment.Num(); ++index)
		{
			bLightingChanged |= glm::length(
				glm::vec3(movedEnvironment[index] - environment[index])) >
				0.0001f;
		}
		Require(bLightingChanged,
			"the transient sky map should follow the SkyComponent sun direction");

		SkyParameters afterSunset = source;
		afterSunset.m_lightDirection = ExpectedLightDirection(-6.75f);
		TVector<glm::vec4> afterSunsetEnvironment;
		Require(
			Raytracing::GenerateSkyEnvironmentEquirectangular(
				afterSunset,
				extent,
				afterSunsetEnvironment),
			"an after-sunset SkyComponent should generate a transient map");
		const auto integratedLuminance = [](const TVector<glm::vec4>& image)
		{
			double result = 0.0;
			for (const glm::vec4& pixel : image)
			{
				result +=
					0.2126 * static_cast<double>(pixel.r) +
					0.7152 * static_cast<double>(pixel.g) +
					0.0722 * static_cast<double>(pixel.b);
			}
			return result;
		};
		Require(
			integratedLuminance(afterSunsetEnvironment) <
				integratedLuminance(environment) * 0.25,
			"the clear-sky bake environment must become substantially darker after sunset");

		SkyParameters eveningSky = source;
		eveningSky.m_lightDirection = ExpectedLightDirection(5.0f);
		eveningSky.m_sunIlluminance =
			glm::vec4(30000.0f, 30000.0f, 30000.0f, 0.0f);
		const glm::uvec2 eveningExtent(64u, 32u);
		TVector<glm::vec4> eveningEnvironment;
		Require(
			Raytracing::GenerateSkyEnvironmentEquirectangular(
				eveningSky,
				eveningExtent,
				eveningEnvironment),
			"the physical evening sky should generate for photometric validation");
		const double dTheta = glm::pi<double>() /
			static_cast<double>(eveningExtent.y);
		const double dPhi = 2.0 * glm::pi<double>() /
			static_cast<double>(eveningExtent.x);
		double skyHorizontalIlluminance = 0.0;
		for (uint32_t y = 0u; y < eveningExtent.y / 2u; ++y)
		{
			const double theta = dTheta *
				(static_cast<double>(y) + 0.5);
			const double solidAngleRow =
				std::sin(theta) * dTheta * dPhi;
			const double projectedCosine = std::cos(theta);
			for (uint32_t x = 0u; x < eveningExtent.x; ++x)
			{
				const glm::vec3 radiance(
					eveningEnvironment[x + y * eveningExtent.x]);
				const double luminance = glm::dot(
					radiance,
					glm::vec3(0.2126f, 0.7152f, 0.0722f));
				skyHorizontalIlluminance +=
					luminance * projectedCosine * solidAngleRow;
			}
		}
		const glm::vec3 directNormalIlluminance =
			Raytracing::CalculateDirectSunIlluminance(eveningSky);
		const double directHorizontalIlluminance =
			static_cast<double>(glm::dot(
				directNormalIlluminance,
				glm::vec3(0.2126f, 0.7152f, 0.0722f))) *
			static_cast<double>(
				std::max(-eveningSky.m_lightDirection.y, 0.0f));
		Require(
			directHorizontalIlluminance > 0.0 &&
			directHorizontalIlluminance <
				30000.0 * static_cast<double>(
					std::max(-eveningSky.m_lightDirection.y, 0.0f)),
			"the low-sun direct illuminance must include atmospheric extinction");
		Require(
			skyHorizontalIlluminance >
				directHorizontalIlluminance * 0.05 &&
			skyHorizontalIlluminance <
				directHorizontalIlluminance * 2.0,
			"normalized atmospheric scattering must remain comparable to the "
			"attenuated direct low-sun illuminance without collapsing; sky " +
				std::to_string(skyHorizontalIlluminance) + ", direct " +
				std::to_string(directHorizontalIlluminance));

		TVector<glm::vec4> cancelled;
		Require(
			!Raytracing::GenerateSkyEnvironmentEquirectangular(
				source,
				extent,
				cancelled,
				[](uint32_t completedRows, uint32_t)
				{
					return completedRows < 2u;
				}) &&
			cancelled.IsEmpty(),
			"cancelling transient sky generation should release its partial texture");
	}

	void TestGroundEnvironmentUsesTheSameSkyAndSun()
	{
		SkyTestWorld world;
		auto sky = world.Instantiate("GroundEnvironment")->AddComponent<SkyComponent>();
		sky->SetSunAngle(38.0f);
		sky->SetSunIlluminance(glm::vec3(2000.0f, 1800.0f, 1600.0f));
		const SkyParameters withoutGround = sky->GetSkyParameters();
		Require(sky->GetGroundAlbedo() == glm::vec3(0.0f) &&
			withoutGround.m_groundRadiance == glm::vec4(0.0f),
			"terrain reflection must be opt-in");
		const glm::vec3 albedo(0.2f, 0.3f, 0.4f);
		sky->SetGroundAlbedo(albedo);
		const SkyParameters withGround = sky->GetSkyParameters();
		const auto serialized = sky->GetReflectedData().Serialize();
		Require(IsNear(serialized["overrideProperties"]["groundAlbedo"].as<glm::vec3>(), albedo),
			"ground reflectance must serialize through the editor/runtime reflected interface");
		const glm::vec3 radiance(withGround.m_groundRadiance);
		const glm::vec3 directLambert = albedo * Raytracing::CalculateDirectSunIlluminance(withGround) *
			(-withGround.m_lightDirection.y / glm::pi<float>());
		Require(Math::AllFinite(radiance) && glm::all(glm::greaterThan(radiance, directLambert)),
			"ground must reflect both direct horizontal sunlight and diffuse skylight");
		Require(!(withoutGround == withGround) &&
			!(withoutGround.GetEnvironmentKey() == withGround.GetEnvironmentKey()),
			"changing ground must invalidate sky upload and filtered environment caches");

		TVector<glm::vec4> original, reflected;
		const glm::uvec2 extent(16u, 8u);
		Require(Raytracing::GenerateSkyEnvironmentEquirectangular(withoutGround, extent, original) &&
			Raytracing::GenerateSkyEnvironmentEquirectangular(withGround, extent, reflected),
			"both environment variants should generate successfully");
		for (uint32_t y = 0u; y < extent.y; ++y)
			for (uint32_t x = 0u; x < extent.x; ++x)
			{
				const size_t index = x + y * extent.x;
				Require(y < extent.y / 2u ? reflected[index] == original[index] :
					IsNear(glm::vec3(reflected[index]), radiance),
					"probe environment must keep the sky and use the raster ground boundary below it");
			}
		sky->SetGroundAlbedo(albedo * 0.5f);
		Require(IsNear(glm::vec3(sky->GetSkyParameters().m_groundRadiance), radiance * 0.5f, 0.001f),
			"Lambertian ground radiance must scale linearly with albedo");
		sky->SetGroundAlbedo(albedo);
		sky->SetSunIlluminance(glm::vec3(withGround.m_sunIlluminance) * 0.5f);
		Require(IsNear(glm::vec3(sky->GetSkyParameters().m_groundRadiance), radiance * 0.5f, 0.001f),
			"sunlight and skylight must both follow source illuminance changes");
		sky->SetSunAngle(-12.0f);
		Require(glm::length(glm::vec3(sky->GetSkyParameters().m_groundRadiance)) < glm::length(radiance) * 0.05f,
			"daylight ground must not remain bright at night");
		sky->SetSunIlluminance(glm::vec3(0.0f));
		Require(sky->GetSkyParameters().m_groundRadiance == glm::vec4(0.0f),
			"ground reflection cannot emit light when its sources are dark");
		sky->SetGroundAlbedo(glm::vec3(-1.0f, 0.4f, 2.0f));
		Require(sky->GetGroundAlbedo() == glm::vec3(0.0f, 0.4f, 1.0f), "ground albedo must be bounded");
		world.Clear();
	}

	void TestSkyNodeMailboxHandoff()
	{
		auto node = TRefPtr<SkyNodeMailboxProbe>::Make();
		const SkyParameters defaults = node->GetSkyParams();

		SkyParameters first = defaults;
		first.m_ambient = 2.0f;
		first.m_cloudsCoverage = 1.2f;
		node->SetSkyParams(first);
		Require(
			node->GetSkyParams() == defaults,
			"SetSkyParams should publish through the pending mailbox");
		node->ConsumePendingSkyParams();
		Require(
			node->GetSkyParams() == first,
			"the render-side consume should atomically publish pending sky parameters");

		SkyParameters second = first;
		second.m_ambient = 3.0f;
		SkyParameters latest = second;
		latest.m_ambient = 4.0f;
		node->SetSkyParams(second);
		node->SetSkyParams(latest);
		node->ConsumePendingSkyParams();
		Require(
			node->GetSkyParams() == latest,
			"the sky mailbox should publish the latest pending update");

		std::atomic<bool> bStart = false;
		std::atomic<bool> bWriterDone = false;
		std::atomic<bool> bSawInvalidValue = false;
		std::thread writer([&]()
			{
				while (!bStart.load(std::memory_order_acquire))
				{
				}

				for (int32_t index = 0; index < 2000; ++index)
				{
					SkyParameters params = defaults;
					params.m_ambient =
						static_cast<float>(index % 11);
					params.m_scatteringSteps =
						(index % 10) + 1;
					node->SetSkyParams(params);
				}
				bWriterDone.store(
					true,
					std::memory_order_release);
			});
		std::thread reader([&]()
			{
				bStart.store(true, std::memory_order_release);
				while (!bWriterDone.load(
					std::memory_order_acquire))
				{
					node->ConsumePendingSkyParams();
					const SkyParameters params =
						node->GetSkyParams();
					if (params.m_ambient < 0.0f ||
						params.m_ambient > 10.0f ||
						params.m_scatteringSteps < 1 ||
						params.m_scatteringSteps > 10)
					{
						bSawInvalidValue.store(
							true,
							std::memory_order_release);
					}
				}
			});
		writer.join();
		reader.join();
		Require(
			!bSawInvalidValue.load(std::memory_order_acquire),
			"concurrent mailbox publication should not expose torn sky parameters");

		node->ResetSkyParams();
		Require(
			!(node->GetSkyParams() == defaults),
			"ResetSkyParams should also use the pending mailbox");
		node->ConsumePendingSkyParams();
		Require(
			node->GetSkyParams() == defaults,
			"consuming a reset should restore default sky parameters");
	}

	void TestExplicitDirectionalLightSynchronization()
	{
		SkyTestWorld world;

		auto unrelatedOwner =
			world.Instantiate("UnrelatedLight");
		auto unrelatedLight =
			unrelatedOwner->AddComponent<LightComponent>();
		unrelatedOwner->GetTransformComponent().SetPosition(
			glm::vec3(1.0f, 2.0f, 3.0f));
		unrelatedLight->SetLightType(ELightType::Point);
		unrelatedLight->SetIntensity(
			glm::vec3(2.0f, 3.0f, 4.0f));

		auto skyOwner = world.Instantiate("Sky");
		auto sky =
			skyOwner->AddComponent<SkyComponent>();
		sky->SetSunAngle(45.0f);
		sky->Tick(0.0f);
		Require(
			unrelatedLight->GetLightType() == ELightType::Point &&
				IsNear(
					unrelatedLight->GetIntensity(),
					glm::vec3(2.0f, 3.0f, 4.0f)) &&
				IsNear(
					glm::vec3(
						unrelatedOwner->
							GetTransformComponent().
								GetPosition()),
					glm::vec3(1.0f, 2.0f, 3.0f)),
			"a sky without an explicit light should not discover or mutate scene lights");

		auto lightParent =
			world.Instantiate("LightParent");
		lightParent->GetTransformComponent().SetPosition(
			glm::vec3(100.0f, 20.0f, -50.0f));
		lightParent->GetTransformComponent().SetRotation(
			glm::angleAxis(
				glm::radians(32.0f),
				Math::vec3_Up));
		auto lightOwner =
			world.Instantiate("DirectionalLight");
		lightOwner->SetParent(lightParent);
		auto light =
			lightOwner->AddComponent<LightComponent>();

		sky->SetDirectionalLight(light);
		sky->SetSunIlluminance(
			glm::vec3(4000.0f, 5000.0f, 6000.0f));
		sky->SetSunAngle(60.0f);
		sky->Tick(0.0f);

		const glm::vec3 expectedDirection =
			glm::vec3(ExpectedLightDirection(60.0f));
		const glm::vec3 expectedDirectIlluminance =
			Raytracing::CalculateDirectSunIlluminance(
				sky->GetSkyParameters());
		const glm::mat4 lightWorld =
			CalculateWorldMatrix(lightOwner);
		const glm::vec3 actualPosition =
			glm::vec3(lightWorld[3]);
		const glm::vec3 actualDirection =
			Math::SafeNormalize(
				glm::mat3(lightWorld) * Math::vec3_Forward,
				Math::vec3_Down);
		Require(
			light->GetLightType() == ELightType::Directional &&
				IsNear(
					light->GetIntensity(),
					expectedDirectIlluminance) &&
				glm::all(glm::greaterThan(
					expectedDirectIlluminance,
					glm::vec3(0.0f))) &&
				glm::all(glm::lessThan(
					expectedDirectIlluminance,
					glm::vec3(4000.0f, 5000.0f, 6000.0f))) &&
				IsNear(
					actualPosition,
					-expectedDirection * 9000.0f,
					0.001f) &&
				IsNear(
					actualDirection,
					expectedDirection,
					0.001f),
			"SkyComponent should synchronize its explicit light in world space");

		sky->SetSunAngle(-10.0f);
		sky->Tick(0.0f);
		Require(
			IsNear(light->GetIntensity(), glm::vec3(0.0f)),
			"a below-horizon sun should disable the explicit directional light");

		sky->SetDirectionalLight({});
		light->SetLightType(ELightType::Spot);
		light->SetIntensity(glm::vec3(9.0f));
		const glm::vec4 positionBeforeNullApply =
			lightOwner->GetTransformComponent().GetPosition();
		sky->SetSunAngle(20.0f);
		sky->Tick(0.0f);
		Require(
			light->GetLightType() == ELightType::Spot &&
				IsNear(light->GetIntensity(), glm::vec3(9.0f)) &&
				lightOwner->GetTransformComponent().GetPosition() ==
					positionBeforeNullApply,
			"clearing the explicit light should stop synchronization");

		world.Clear();
	}

	void TestDestroyedLightReferencesSerializeAsNull()
	{
		SkyTestWorld world;
		auto skyOwner = world.Instantiate("Sky");
		auto sky =
			skyOwner->AddComponent<SkyComponent>();

		auto lightOwner = world.Instantiate("Light");
		auto light = lightOwner->AddComponent<LightComponent>();
		lightOwner->Tick(0.0f);
		const InstanceId liveLightId = light->GetInstanceId();
		sky->SetDirectionalLight(light);

		YAML::Node reflection =
			sky->GetReflectedData().Serialize();
		Require(
			reflection["overrideProperties"]["m_directionalLight"]
				["instanceId"].as<InstanceId>() == liveLightId,
			"a live explicit light should serialize its component instance id");

		Require(
			lightOwner->RemoveComponent(light),
			"the referenced light component should be removable");
		Require(
			!sky->GetDirectionalLight() &&
				sky->GetDirectionalLight().IsInited(),
			"an external TObjectPtr should observe a destroyed light before cleanup");

		reflection = sky->GetReflectedData().Serialize();
		Require(
			IsNullObjectReference(
				reflection["overrideProperties"]
					["m_directionalLight"]),
			"an expired component reference should serialize as null without dereferencing freed storage");
		sky->Tick(0.0f);
		Require(
			!sky->GetDirectionalLight().IsInited(),
			"SkyComponent should clear an expired light reference during synchronization");

		lightOwner = world.Instantiate("DestroyedLightOwner");
		light = lightOwner->AddComponent<LightComponent>();
		lightOwner->Tick(0.0f);
		sky->SetDirectionalLight(light);
		world.DestroyImmediate(lightOwner);
		Require(
			!sky->GetDirectionalLight() &&
				sky->GetDirectionalLight().IsInited(),
			"destroying a light game object should expire the explicit reference");
		reflection = sky->GetReflectedData().Serialize();
		Require(
			IsNullObjectReference(
				reflection["overrideProperties"]
					["m_directionalLight"]),
			"a light reference expired through owner destruction should serialize as null");
		sky->Tick(0.0f);
		Require(
			!sky->GetDirectionalLight().IsInited(),
			"SkyComponent should clear a light reference expired through owner destruction");

		world.Clear();
	}

	void TestPrefabRoundTripRemapsExplicitLight()
	{
		SkyTestWorld world;
		auto source = world.Instantiate("SkyPrefab");
		auto sourceLight =
			source->AddComponent<LightComponent>();
		auto sourceSky =
			source->AddComponent<SkyComponent>();
		sourceSky->SetDirectionalLight(sourceLight);
		sourceSky->SetSunAngle(35.0f);
		sourceSky->SetCloudsCoverage(1.25f);
		sourceSky->SetAmbient(2.5f);
		sourceSky->SetGiIndirectIntensity(0.35f);
		sourceSky->SetSunIlluminance(
			glm::vec3(7000.0f, 8000.0f, 9000.0f));
		source->Tick(0.0f);

		const PrefabPtr prefab =
			SkyPrefabTestAsset::Capture(world, source);
		world.DestroyImmediate(source);

		auto first = world.Instantiate(prefab);
		Require(static_cast<bool>(first),
			"the captured sky prefab should instantiate once");
		auto firstLight =
			first->GetComponent<LightComponent>();
		auto firstSky =
			first->GetComponent<SkyComponent>();
		Require(
			firstLight &&
				firstSky &&
				firstSky->GetDirectionalLight() == firstLight &&
				IsNear(firstSky->GetSunAngle(), 35.0f) &&
				IsNear(firstSky->GetCloudsCoverage(), 1.25f) &&
				IsNear(firstSky->GetAmbient(), 2.5f) &&
				IsNear(firstSky->GetGiIndirectIntensity(), 0.35f) &&
				IsNear(
					firstSky->GetSunIlluminance(),
					glm::vec3(7000.0f, 8000.0f, 9000.0f)),
			"prefab loading should restore sky properties and its internal light reference");

		auto second = world.Instantiate(prefab);
		Require(static_cast<bool>(second),
			"the captured sky prefab should instantiate with remapped ids");
		auto secondLight =
			second->GetComponent<LightComponent>();
		auto secondSky =
			second->GetComponent<SkyComponent>();
		Require(
			secondLight &&
				secondSky &&
				secondSky->GetDirectionalLight() == secondLight &&
				secondSky->GetDirectionalLight() != firstLight &&
				secondLight->GetInstanceId() !=
					firstLight->GetInstanceId(),
			"each prefab instance should remap the sky reference to its own light");

		const YAML::Node reflection =
			secondSky->GetReflectedData().Serialize();
		Require(
			reflection["overrideProperties"]["m_directionalLight"]
				["instanceId"].as<InstanceId>() ==
					secondLight->GetInstanceId(),
			"the remapped prefab reference should serialize the live light id");

		world.Clear();
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "SkyParameterGpuLayoutAndDefaults", TestSkyParameterGpuLayoutAndDefaults },
		{ "ReflectionMetadataRangesAndStableLightType", TestReflectionMetadataRangesAndStableLightType },
		{ "SettersClampAndUpdateDirection", TestSettersClampAndUpdateDirection },
		{ "EnvironmentKeyHashEquality", TestEnvironmentKeyHashEquality },
		{ "TransientBakeEnvironmentUsesClearSkyParameters", TestTransientBakeEnvironmentUsesClearSkyParameters },
		{ "GroundEnvironmentUsesTheSameSkyAndSun", TestGroundEnvironmentUsesTheSameSkyAndSun },
		{ "SkyNodeMailboxHandoff", TestSkyNodeMailboxHandoff },
		{ "EnvironmentCubemapOrientation", TestEnvironmentCubemapOrientation },
		{ "ExplicitDirectionalLightSynchronization", TestExplicitDirectionalLightSynchronization },
		{ "DestroyedLightReferencesSerializeAsNull", TestDestroyedLightReferencesSerializeAsNull },
		{ "PrefabRoundTripRemapsExplicitLight", TestPrefabRoundTripRemapsExplicitLight },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr
				<< "[FAIL] "
				<< test.first
				<< ": "
				<< error.what()
				<< std::endl;
			return 1;
		}
	}

	return 0;
}
