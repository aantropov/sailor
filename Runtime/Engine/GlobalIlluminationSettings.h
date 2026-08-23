#pragma once

#include "AssetRegistry/FileId.h"
#include "Containers/Map.h"
#include "Core/Defines.h"

#include <cstdint>
#include <string>

#include <yaml-cpp/yaml.h>

namespace Sailor
{
	enum class EGlobalIlluminationProbeMode : uint8_t
	{
		Blend = 0,
		Additive
	};

	struct SAILOR_SHARED_API GlobalIlluminationProbeBinding final
	{
		FileId m_asset{};
		EGlobalIlluminationProbeMode m_mode =
			EGlobalIlluminationProbeMode::Blend;
		float m_initialWeight = 0.0f;
		bool m_bPreload = false;
	};

	struct SAILOR_SHARED_API GlobalIlluminationWorldSettings final
	{
		TMap<std::string, GlobalIlluminationProbeBinding> m_probes{};

		bool Validate(std::string& outDiagnostic) const noexcept;
		YAML::Node Serialize() const;
		bool Deserialize(
			const YAML::Node& worldRoot,
			std::string& outDiagnostic) noexcept;
	};

	SAILOR_SHARED_API const char* GlobalIlluminationProbeModeToString(
		EGlobalIlluminationProbeMode mode) noexcept;
	SAILOR_SHARED_API bool TryParseGlobalIlluminationProbeMode(
		const std::string& value,
		EGlobalIlluminationProbeMode& outMode) noexcept;
}
