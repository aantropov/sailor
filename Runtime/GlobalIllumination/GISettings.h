#pragma once

#include "AssetRegistry/FileId.h"
#include "Containers/Map.h"
#include "Core/Defines.h"
#include "Engine/Types.h"
#include "GlobalIllumination/RuntimeGIProbesSettings.h"

#include <cstdint>
#include <string>

#include <yaml-cpp/yaml.h>

namespace Sailor
{
	constexpr bool IsGlobalIlluminationBakeContributor(
		EMobilityType mobility) noexcept
	{
		return mobility == EMobilityType::Static ||
			mobility == EMobilityType::Stationary;
	}

	enum class EGlobalIlluminationProbeMode : uint8_t
	{
		Blend = 0,
		Additive
	};

	enum class EGlobalIlluminationMode : uint8_t
	{
		Realtime = 0,
		RealtimeAndBaked,
		BakedOnly
	};

	enum class EGlobalIlluminationProbeSource : uint8_t
	{
		BakedAssets = 0,
		RuntimeExperimental
	};

	constexpr bool UsesBakedGlobalIllumination(
		EGlobalIlluminationMode mode) noexcept
	{
		return mode != EGlobalIlluminationMode::Realtime;
	}

	struct SAILOR_SHARED_API GlobalIlluminationProbeBinding final
	{
		FileId m_asset{};
		EGlobalIlluminationProbeMode m_mode =
			EGlobalIlluminationProbeMode::Blend;
		float m_initialWeight = 0.0f;
		bool m_bPreload = false;
	};

	struct SAILOR_SHARED_API GISettings final
	{
		EGlobalIlluminationMode m_mode =
			EGlobalIlluminationMode::RealtimeAndBaked;
		EGlobalIlluminationProbeSource m_probeSource =
			EGlobalIlluminationProbeSource::BakedAssets;
		RuntimeGIProbesSettings m_runtimeProbes{};
		TMap<std::string, GlobalIlluminationProbeBinding> m_probes{};

		bool Validate(std::string& outDiagnostic) const noexcept;
		YAML::Node Serialize() const;
		bool Deserialize(
			const YAML::Node& worldRoot,
			std::string& outDiagnostic) noexcept;
	};

}
