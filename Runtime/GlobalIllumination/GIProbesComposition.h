#pragma once

#include "GlobalIllumination/GISettings.h"
#include "GlobalIllumination/GIProbesData.h"

#include <cstdint>
#include <string>

namespace Sailor
{
	struct SAILOR_SHARED_API GIProbesCompositionInput final
	{
		std::string m_name{};
		GIProbesDataPtr m_data{};
		EGlobalIlluminationProbeMode m_mode =
			EGlobalIlluminationProbeMode::Blend;
		float m_weight = 0.0f;
		FileId m_asset{};
	};

	enum class EGIProbesCompositionStatus : uint8_t
	{
		Success = 0,
		Disabled,
		BudgetExceeded,
		MissingData,
		InvalidInput,
		Incompatible
	};

	enum class EGIProbesCompositionValidation : uint8_t
	{
		Full = 0,
		TrustedAssetMetadata
	};

	struct SAILOR_SHARED_API GIProbesCompositionPlan final
	{
		EGIProbesCompositionStatus m_status =
			EGIProbesCompositionStatus::InvalidInput;
		std::string m_diagnostic{};
		GIProbesDataPtr m_layout{};
		TVector<GIProbesDataPtr> m_states{};
		TVector<std::string> m_names{};
		TVector<FileId> m_assets{};
		TVector<float> m_effectiveWeights{};
		TVector<EGlobalIlluminationProbeMode> m_modes{};
		uint64_t m_lightingHash = 0u;

		bool IsSuccess() const noexcept
		{
			return m_status == EGIProbesCompositionStatus::Success &&
				m_layout && !m_states.IsEmpty();
		}
	};

	struct SAILOR_SHARED_API GIProbesCompositionResult final
	{
		EGIProbesCompositionStatus m_status =
			EGIProbesCompositionStatus::InvalidInput;
		std::string m_diagnostic{};
		GIProbesDataPtr m_data{};
		TVector<std::string> m_names{};
		TVector<float> m_effectiveWeights{};
		TVector<EGlobalIlluminationProbeMode> m_modes{};

		bool IsSuccess() const noexcept
		{
			return m_status == EGIProbesCompositionStatus::Success && m_data;
		}
	};

	class SAILOR_SHARED_API GIProbesComposer final
	{
	public:
		static GIProbesCompositionPlan BuildPlan(
			const TVector<GIProbesCompositionInput>& inputs,
			uint32_t maxStatesPerSnapshot,
			EGIProbesCompositionValidation validation =
				EGIProbesCompositionValidation::Full) noexcept;

		static GIProbesCompositionResult Compose(
			const TVector<GIProbesCompositionInput>& inputs,
			uint32_t maxStatesPerSnapshot) noexcept;
	};
}
