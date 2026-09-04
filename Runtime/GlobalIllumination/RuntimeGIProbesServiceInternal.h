#pragma once

#include "GlobalIllumination/GIProbesTracing.h"
#include "GlobalIllumination/RuntimeGIProbesService.h"
#include "Tasks/Tasks.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace Sailor::RuntimeGIProbesInternal
{
	struct ProbeCellKey final
	{
		glm::ivec3 m_worldCell{};
		uint64_t m_geometryGeneration = 0u;

		bool operator==(const ProbeCellKey&) const noexcept = default;
	};
}

namespace Sailor
{
	struct RuntimeGIProbesService::Impl final
	{
		struct ProbeWork final
		{
			RuntimeGIProbesInternal::ProbeCellKey m_key{};
			GIProbeIrradianceAccumulator m_accumulator{};
			GIProbe m_probe{};
			glm::vec3 m_layoutPosition{};
			uint32_t m_brickIndex = 0u;
			bool m_bHasTransport = false;
			bool m_bReady = false;
			bool m_bRefined = false;
			bool m_bDirty = false;
		};

		struct Generation final
		{
			RuntimeGIProbesStartRequest m_request{};
			GIProbesDataPtr m_data{};
			std::vector<ProbeWork> m_probes{};
			std::vector<uint32_t> m_initialQueue{};
			std::deque<uint32_t> m_warmingQueue{};
			std::deque<uint32_t> m_refinementQueue{};
			std::atomic<bool> m_cancel{false};
			std::chrono::steady_clock::time_point m_started{};
			uint64_t m_id = 0u;
			uint64_t m_initialLayoutHash = 0u;
			uint32_t m_effectiveCapacity = 0u;
			size_t m_initialCursor = 0u;
			uint32_t m_readyCount = 0u;
			uint32_t m_refinedCount = 0u;
			uint32_t m_dirtyCount = 0u;
			uint64_t m_progressSampleCount = 0u;
			bool m_bFailed = false;
			bool m_bPublished = false;
		};

		struct Job final
		{
			std::shared_ptr<Generation> m_generation{};
			ProbeWork m_work{};
			uint32_t m_probeIndex = 0u;
		};

		struct DispatchBatch final
		{
			std::shared_ptr<Generation> m_generation{};
			uint32_t m_workerCount = 0u;
		};

		mutable std::mutex m_mutex;
		std::vector<Tasks::ITaskPtr> m_workerTasks{};
		std::shared_ptr<Generation> m_generation{};
		GIProbesDataPtr m_publishedData{};
		RuntimeGIProbesStartRequest m_lastRequest{};
		RuntimeGIProbesStatus m_status{};
		std::chrono::steady_clock::time_point m_lastPublication{};
		uint64_t m_nextGenerationId = 1u;
		uint64_t m_nextPublishedRevision = 1u;
		uint32_t m_workerCount = 0u;
		bool m_bHasLastRequest = false;
		bool m_bPaused = false;
		bool m_bWorkAllowed = true;
		double m_workTokensMilliseconds = 0.0;

		~Impl();

		void StopWorkerTasks() noexcept;
		static uint32_t GetProgressSampleCount(const Generation& generation, const ProbeWork& probe) noexcept;
		static bool AreTransportSettingsCompatible(const GIProbesBakeSettings& lhs,
			const GIProbesBakeSettings& rhs) noexcept;
		static bool AreIrradianceSettingsCompatible(const GIProbesBakeSettings& lhs,
			const GIProbesBakeSettings& rhs) noexcept;
		static std::vector<uint32_t> BuildProgressiveProbeOrder(const Generation& generation);
		std::shared_ptr<Generation> BuildGeneration(const RuntimeGIProbesStartRequest& request,
			uint64_t generationId,
			std::string& outDiagnostic);
		void ReuseGenerationState(Generation& next, const Generation& previous);

		bool TryTakeJob(const std::shared_ptr<Generation>& generation, Job& outJob);
		static bool HasQueuedWork(const Generation& generation) noexcept;
		bool CanDispatchWorkLocked() const noexcept;
		DispatchBatch GetDispatchBatch() const noexcept;
		void WorkerBatch(std::shared_ptr<Generation> generation,
			uint32_t maximumJobCount = (std::numeric_limits<uint32_t>::max)());
		void PumpWorkerTasks();
		bool ExecuteJob(Job& job, std::string& outDiagnostic);

		void FailGenerationLocked(Generation& generation, std::string diagnostic);
		void CommitJob(const Job& job, bool bSuccess, std::string diagnostic, double elapsedMilliseconds);
		void UpdateStatusLocked();
		void PublishIfNeeded();
	};
}
