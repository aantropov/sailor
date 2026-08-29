#pragma once

#include "GlobalIllumination/GIProbesData.h"
#include "Core/Defines.h"
#include "Math/Bounds.h"

#include <atomic>
#include <functional>
#include <string>

namespace Sailor
{
	inline constexpr uint32_t GIProbesMaxBakeThreadCount = 64u;

	struct SAILOR_SHARED_API GIProbeBakeRaySample final
	{
		glm::vec3 m_radiance{};
		float m_distance = 0.0f;
		bool m_bHit = false;
		bool m_bBackFace = false;
	};

	class SAILOR_SHARED_API IGIProbeBakeRaySampler
	{
	public:
		virtual ~IGIProbeBakeRaySampler() = default;
		// Bake may call Sample concurrently when the request uses more than one
		// thread. Implementations must treat their prepared scene as immutable.
		virtual bool Sample(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t randomSeed,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const = 0;
		virtual bool SampleVisibility(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float maxDistance,
			uint32_t randomSeed,
			GIProbeBakeRaySample& outSample,
			std::string& outDiagnostic) const
		{
			return Sample(
				origin,
				direction,
				maxDistance,
				randomSeed,
				outSample,
				outDiagnostic);
		}
	};

	struct SAILOR_SHARED_API GIProbesBakeProgress final
	{
		uint32_t m_completedProbes = 0u;
		uint32_t m_totalProbes = 0u;
		float m_fraction = 0.0f;
		std::string m_stage{};
	};

	struct SAILOR_SHARED_API GIProbesBakeRequest final
	{
		std::string m_stateName{};
		std::string m_bakerVersion{ GIProbesCurrentBakerVersion };
		glm::vec3 m_volumeMin{};
		glm::vec3 m_volumeMax{};
		GIProbesBakeSettings m_settings{};
		TVector<Math::AABB> m_sceneGeometryBounds{};
		uint64_t m_sourceWorldHash = 0u;
		uint32_t m_threadCount = 1u;
		const GIProbesData* m_layoutSource = nullptr;
		const std::atomic<bool>* m_cancel = nullptr;
		std::function<void(const GIProbesBakeProgress&)> m_progress{};
	};

	enum class EGIProbesBakeStatus : uint8_t
	{
		Success = 0u,
		InvalidRequest,
		Cancelled,
		SamplingFailed,
		InvalidResult
	};

	struct SAILOR_SHARED_API GIProbesBakeResult final
	{
		EGIProbesBakeStatus m_status =
			EGIProbesBakeStatus::InvalidRequest;
		GIProbesDataPtr m_data{};
		std::string m_diagnostic{};

		bool IsSuccess() const noexcept
		{
			return m_status == EGIProbesBakeStatus::Success && m_data;
		}
	};

	class SAILOR_SHARED_API GIProbesBaker final
	{
	public:
		static GIProbesBakeResult Bake(
			const GIProbesBakeRequest& request,
			const IGIProbeBakeRaySampler& sampler) noexcept;
	};
}
