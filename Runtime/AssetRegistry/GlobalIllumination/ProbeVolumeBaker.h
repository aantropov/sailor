#pragma once

#include "AssetRegistry/GlobalIllumination/ProbeVolumeData.h"
#include "Core/Defines.h"
#include "Math/Bounds.h"

#include <atomic>
#include <functional>
#include <string>

namespace Sailor
{
	struct SAILOR_SHARED_API ProbeVolumeBakeRaySample final
	{
		glm::vec3 m_radiance{};
		float m_distance = 0.0f;
		bool m_bHit = false;
	};

	class SAILOR_SHARED_API IProbeVolumeBakeRaySampler
	{
	public:
		virtual ~IProbeVolumeBakeRaySampler() = default;
		virtual bool Sample(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t randomSeed,
			ProbeVolumeBakeRaySample& outSample,
			std::string& outDiagnostic) const = 0;
	};

	struct SAILOR_SHARED_API ProbeVolumeBakeProgress final
	{
		uint32_t m_completedProbes = 0u;
		uint32_t m_totalProbes = 0u;
		float m_fraction = 0.0f;
		std::string m_stage{};
	};

	struct SAILOR_SHARED_API ProbeVolumeBakeRequest final
	{
		std::string m_stateName{};
		std::string m_bakerVersion{ "Sailor ProbeVolumeBaker/1" };
		glm::vec3 m_volumeMin{};
		glm::vec3 m_volumeMax{};
		ProbeVolumeBakeSettings m_settings{};
		TVector<Math::AABB> m_sceneGeometryBounds{};
		uint64_t m_sourceWorldHash = 0u;
		const ProbeVolumeData* m_layoutSource = nullptr;
		const std::atomic<bool>* m_cancel = nullptr;
		std::function<void(const ProbeVolumeBakeProgress&)> m_progress{};
	};

	enum class EProbeVolumeBakeStatus : uint8_t
	{
		Success = 0u,
		InvalidRequest,
		Cancelled,
		SamplingFailed,
		InvalidResult
	};

	struct SAILOR_SHARED_API ProbeVolumeBakeResult final
	{
		EProbeVolumeBakeStatus m_status =
			EProbeVolumeBakeStatus::InvalidRequest;
		ProbeVolumeDataPtr m_data{};
		std::string m_diagnostic{};

		bool IsSuccess() const noexcept
		{
			return m_status == EProbeVolumeBakeStatus::Success && m_data;
		}
	};

	class SAILOR_SHARED_API ProbeVolumeBaker final
	{
	public:
		static ProbeVolumeBakeResult Bake(
			const ProbeVolumeBakeRequest& request,
			const IProbeVolumeBakeRaySampler& sampler) noexcept;
	};
}
