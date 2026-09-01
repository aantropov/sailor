#pragma once

#include "GlobalIllumination/GIProbesBaker.h"
#include "GlobalIllumination/RuntimeGIProbesSettings.h"
#include "Memory/SharedPtr.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace Sailor
{
	enum class ERuntimeGIProbesLifecycle : uint8_t
	{
		Disabled = 0u,
		PreparingScene,
		Tracing,
		Ready,
		Paused,
		Throttled,
		Failed
	};

	enum class ERuntimeGIProbeState : uint8_t
	{
		Uninitialized = 0u,
		Placement,
		Transport,
		Warming,
		Ready,
		Refining,
		Dirty,
		Recycling
	};

	struct SAILOR_SHARED_API RuntimeGIProbeCellKey final
	{
		glm::ivec3 m_worldCell{};
		uint64_t m_geometryGeneration = 0u;
		uint32_t m_cascade = 0u;
		uint32_t m_slotGeneration = 0u;

		bool operator==(const RuntimeGIProbeCellKey&) const noexcept = default;
	};

	struct SAILOR_SHARED_API RuntimeGIProbesStartRequest final
	{
		RuntimeGIProbesSettings m_worldSettings{};
		RuntimeGIProbesQualitySettings m_qualitySettings{};
		TSharedPtr<IGIProbeBakeRaySampler> m_sampler{};
		glm::vec3 m_cameraPosition{};
		uint64_t m_geometryGeneration = 1u;
		uint64_t m_lightingGeneration = 1u;
		uint32_t m_randomSeed = 0u;
		bool m_bReuseExistingCells = true;
	};

	struct SAILOR_SHARED_API RuntimeGIProbesStatus final
	{
		ERuntimeGIProbesLifecycle m_lifecycle =
			ERuntimeGIProbesLifecycle::Disabled;
		uint64_t m_sceneGeneration = 0u;
		uint64_t m_lightingGeneration = 0u;
		uint64_t m_publishedRevision = 0u;
		uint32_t m_capacity = 0u;
		uint32_t m_activeProbeCount = 0u;
		uint32_t m_readyProbeCount = 0u;
		uint32_t m_dirtyProbeCount = 0u;
		uint32_t m_queuedProbeCount = 0u;
		uint32_t m_workerCount = 0u;
		uint64_t m_tracedRayCount = 0u;
		uint64_t m_publishedBytes = 0u;
		float m_coverage = 0.0f;
		float m_refinement = 0.0f;
		float m_raysPerSecond = 0.0f;
		float m_workerCpuMilliseconds = 0.0f;
		float m_lastPublicationMilliseconds = 0.0f;
		bool m_bEnabled = false;
		bool m_bPaused = false;
		bool m_bThrottled = false;
		std::string m_diagnostic{};
	};

	class SAILOR_SHARED_API RuntimeGIProbesService final
	{
	public:
		RuntimeGIProbesService();
		~RuntimeGIProbesService();

		RuntimeGIProbesService(const RuntimeGIProbesService&) = delete;
		RuntimeGIProbesService& operator=(
			const RuntimeGIProbesService&) = delete;

		bool Start(
			const RuntimeGIProbesStartRequest& request,
			std::string& outDiagnostic);
		bool Restart(std::string& outDiagnostic);
		void Disable() noexcept;
		void SetPaused(bool bPaused) noexcept;
		void SetWorkAllowed(bool bAllowed) noexcept;
		void Tick(float deltaTimeSeconds = 1.0f / 60.0f);

		GIProbesDataPtr GetPublishedData() const;
		RuntimeGIProbesStatus GetStatus() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
