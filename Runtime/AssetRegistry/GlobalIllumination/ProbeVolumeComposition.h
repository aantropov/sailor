#pragma once

#include "AssetRegistry/GlobalIllumination/ProbeVolumeData.h"
#include "Engine/GlobalIlluminationSettings.h"

#include <cstdint>
#include <string>

namespace Sailor
{
	struct SAILOR_SHARED_API ProbeVolumeCompositionInput final
	{
		std::string m_name{};
		ProbeVolumeDataPtr m_data{};
		EGlobalIlluminationProbeMode m_mode =
			EGlobalIlluminationProbeMode::Blend;
		float m_weight = 0.0f;
		FileId m_asset{};
	};

	enum class EProbeVolumeCompositionStatus : uint8_t
	{
		Success = 0,
		Disabled,
		BudgetExceeded,
		MissingData,
		InvalidInput,
		Incompatible
	};

	enum class EProbeVolumeCompositionValidation : uint8_t
	{
		Full = 0,
		TrustedAssetMetadata
	};

	struct SAILOR_SHARED_API ProbeVolumeCompositionPlan final
	{
		EProbeVolumeCompositionStatus m_status =
			EProbeVolumeCompositionStatus::InvalidInput;
		std::string m_diagnostic{};
		ProbeVolumeDataPtr m_layout{};
		TVector<ProbeVolumeDataPtr> m_states{};
		TVector<std::string> m_names{};
		TVector<FileId> m_assets{};
		TVector<float> m_effectiveWeights{};
		TVector<EGlobalIlluminationProbeMode> m_modes{};
		uint64_t m_lightingHash = 0u;

		bool IsSuccess() const noexcept
		{
			return m_status == EProbeVolumeCompositionStatus::Success &&
				m_layout && !m_states.IsEmpty();
		}
	};

	struct SAILOR_SHARED_API ProbeVolumeCompositionResult final
	{
		EProbeVolumeCompositionStatus m_status =
			EProbeVolumeCompositionStatus::InvalidInput;
		std::string m_diagnostic{};
		ProbeVolumeDataPtr m_data{};
		TVector<std::string> m_names{};
		TVector<float> m_effectiveWeights{};
		TVector<EGlobalIlluminationProbeMode> m_modes{};

		bool IsSuccess() const noexcept
		{
			return m_status == EProbeVolumeCompositionStatus::Success && m_data;
		}
	};

	class SAILOR_SHARED_API ProbeVolumeComposer final
	{
	public:
		static ProbeVolumeCompositionPlan BuildPlan(
			const TVector<ProbeVolumeCompositionInput>& inputs,
			uint32_t maxStatesPerSnapshot,
			EProbeVolumeCompositionValidation validation =
				EProbeVolumeCompositionValidation::Full) noexcept;

		static ProbeVolumeCompositionResult Compose(
			const TVector<ProbeVolumeCompositionInput>& inputs,
			uint32_t maxStatesPerSnapshot) noexcept;
	};
}
