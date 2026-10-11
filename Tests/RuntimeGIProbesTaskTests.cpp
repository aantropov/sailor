#include "Sailor.h"
#include "GlobalIllumination/RuntimeGIProbesServiceInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>

using namespace Sailor;

namespace Sailor
{
	class RuntimeGIProbesServiceTestAccess
	{
	public:
		static auto Generation(const RuntimeGIProbesService& service) { return service.m_impl->m_generation; }
		static auto Tasks(const RuntimeGIProbesService& service) { return service.m_impl->m_workerTasks; }

		static uint64_t CompletedSamples(const RuntimeGIProbesService& service)
		{
			const std::lock_guard<std::mutex> lock(service.m_impl->m_mutex);
			uint64_t samples = 0;
			if (const auto& generation = service.m_impl->m_generation)
			{
				for (const auto& probe : generation->m_probes) samples += probe.m_accumulator.m_sampleCount;
			}
			return samples;
		}
	};
}

namespace
{
	using Access = RuntimeGIProbesServiceTestAccess;

	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	template<typename TPredicate>
	bool WaitUntil(TPredicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5))
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!predicate())
		{
			if (std::chrono::steady_clock::now() >= deadline) return false;
			std::this_thread::yield();
		}
		return true;
	}

	class GatedSampler final : public IGIProbeBakeRaySampler
	{
	public:
		GatedSampler(uint32_t blockedRays, glm::vec3 radiance) : m_blockedRays(blockedRays), m_radiance(radiance) {}

		bool Sample(const glm::vec3&, const glm::vec3&, float distance, uint32_t,
			GIProbeBakeRaySample& sample, std::string& diagnostic) const override
		{
			if (!CheckQueue(diagnostic)) return false;
			++m_irradianceRays;
			sample = {};
			sample.m_distance = distance;
			sample.m_radiance = m_radiance;
			return true;
		}

		bool SampleVisibility(const glm::vec3&, const glm::vec3&, float distance, uint32_t,
			GIProbeBakeRaySample& sample, std::string& diagnostic) const override
		{
			if (!CheckQueue(diagnostic)) return false;
			const uint32_t ray = m_visibilityRays++;
			if (ray < m_blockedRays)
			{
				m_threads[ray] = std::this_thread::get_id();
				++m_entered;
				m_resume.wait();
				++m_left;
			}
			sample = {};
			sample.m_distance = distance;
			return true;
		}

		void Release() { if (!m_released.exchange(true)) m_resume.count_down(); }

		mutable std::atomic<uint32_t> m_entered{ 0 }, m_left{ 0 }, m_visibilityRays{ 0 }, m_irradianceRays{ 0 };
		mutable std::array<std::thread::id, 2> m_threads{};
		mutable std::atomic<bool> m_wrongQueue{ false };

	private:
		bool CheckQueue(std::string& diagnostic) const
		{
			if (App::GetSubmodule<Tasks::Scheduler>()->GetCurrentThreadType() == EThreadType::GI) return true;
			m_wrongQueue = true;
			diagnostic = "runtime probe work did not run on the GI queue";
			return false;
		}

		const uint32_t m_blockedRays;
		const glm::vec3 m_radiance;
		mutable std::latch m_resume{ 1 };
		std::atomic<bool> m_released{ false };
	};

	struct ReleaseSamplesOnExit
	{
		GatedSampler& sampler;
		~ReleaseSamplesOnExit() { sampler.Release(); }
	};

	RuntimeGIProbesStartRequest Request(const TSharedPtr<GatedSampler>& sampler, uint32_t workers)
	{
		RuntimeGIProbesStartRequest request;
		request.m_sampler = sampler;
		request.m_geometryBounds.m_min = glm::vec3(-0.1f);
		request.m_geometryBounds.m_max = glm::vec3(0.1f);
		request.m_worldSettings.m_bounceCount = 1;
		request.m_worldSettings.m_minProbeSpacing = 1.0f;
		request.m_worldSettings.m_maxRayDistance = 10.0f;
		request.m_qualitySettings.m_bEnabled = true;
		request.m_qualitySettings.m_maxActiveProbes = 8;
		request.m_qualitySettings.m_initialSamplesPerProbe = 16;
		request.m_qualitySettings.m_targetSamplesPerProbe = 32;
		request.m_qualitySettings.m_workerCount = workers;
		request.m_qualitySettings.m_cpuDutyFraction = 1.0f;
		request.m_qualitySettings.m_maxPublicationsPerSecond = 60.0f;
		return request;
	}

	void WaitBlocked(const GatedSampler& sampler, uint32_t workers)
	{
		Require(WaitUntil([&]() { return sampler.m_entered == workers || sampler.m_wrongQueue; }) &&
			!sampler.m_wrongQueue, "the actual GI workers must enter the held transport rays");
		for (uint32_t i = 0; i < workers; ++i)
		{
			Require(sampler.m_threads[i] != std::this_thread::get_id(), "probe work must not execute inline on Main");
			if (i > 0) Require(sampler.m_threads[i] != sampler.m_threads[0], "held probes must use distinct GI workers");
		}
	}

	void WaitReady(RuntimeGIProbesService& service)
	{
		Require(WaitUntil([&]()
			{
				service.Tick();
				return service.GetStatus().m_lifecycle == ERuntimeGIProbesLifecycle::Ready &&
					service.GetPublishedData() && service.GetStatus().m_publishedRevision > 0;
			}), "the scheduler-backed solver must refine and publish its replacement grid");
	}

	void TestReplacementAndDisable(uint32_t workers, bool disable)
	{
		auto oldSampler = TSharedPtr<GatedSampler>::Make(workers, glm::vec3(4, 0, 0));
		RuntimeGIProbesService service;
		ReleaseSamplesOnExit release{ *oldSampler };
		std::string diagnostic;
		Require(service.Start(Request(oldSampler, workers), diagnostic), diagnostic.c_str());
		WaitBlocked(*oldSampler, workers);
		const auto oldGeneration = Access::Generation(service);
		const auto oldTasks = Access::Tasks(service);
		Require(oldTasks.size() == workers && !service.GetPublishedData(),
			"the initial grid must have actual outstanding tasks and no premature publication");

		auto replacement = TSharedPtr<GatedSampler>::Make(0u, glm::vec3(0, 0, 0.5f));
		if (disable) service.Disable();
		else
		{
			auto request = Request(replacement, workers);
			++request.m_geometryGeneration;
			Require(service.Start(request, diagnostic), diagnostic.c_str());
		}
		const auto accepted = service.GetStatus();
		Require(oldGeneration->m_cancel, "replacing or disabling must cancel the held generation");
		oldSampler->Release();
		for (const auto& task : oldTasks) task->Wait();
		const auto afterOldWork = service.GetStatus();
		Require(oldSampler->m_left == workers && !service.GetPublishedData() &&
			afterOldWork.m_publishedRevision == 0 && afterOldWork.m_readyProbeCount == 0 &&
			afterOldWork.m_lifecycle == accepted.m_lifecycle && afterOldWork.m_diagnostic == accepted.m_diagnostic,
			"late GI tasks must not publish progress, failure or lighting into a replaced or disabled owner");

		if (disable)
		{
			service.Tick();
			Require(service.GetStatus().m_lifecycle == ERuntimeGIProbesLifecycle::Disabled && !service.GetPublishedData(),
				"Tick must not revive disabled work after its old tasks finish");
			return;
		}

		WaitReady(service);
		const auto published = service.GetPublishedData();
		Require(replacement->m_irradianceRays == 8 * 32 && Access::CompletedSamples(service) == 8 * 32,
			"the new generation must execute every irradiance ray exactly once on the GI queue");
		for (const auto& probe : published->m_probes)
		{
			Require(probe.m_irradiance[0].x == 0 && probe.m_irradiance[0].z > 0,
				"replacement coefficients must contain only the new blue environment, not stale red rays");
		}
		const auto coefficients = published->m_probes[0].m_irradiance;
		service.Disable();
		Require(!service.GetPublishedData() && published->m_probes[0].m_irradiance == coefficients,
			"disabling the solver must not invalidate a frame's retained coefficient snapshot");
	}

	void TestOwnerTeardown(uint32_t workers)
	{
		auto sampler = TSharedPtr<GatedSampler>::Make(workers, glm::vec3(0.25f));
		std::atomic<bool> destroyed{ false };
		std::jthread owner;
		auto service = TUniquePtr<RuntimeGIProbesService>::Make();
		ReleaseSamplesOnExit release{ *sampler };
		std::string diagnostic;
		Require(service->Start(Request(sampler, workers), diagnostic), diagnostic.c_str());
		WaitBlocked(*sampler, workers);
		const auto generation = Access::Generation(*service);
		const auto tasks = Access::Tasks(*service);
		owner = std::jthread([service = std::move(service), &destroyed]() mutable
			{
				service.Clear();
				destroyed = true;
			});
		Require(WaitUntil([&]() { return generation->m_cancel.load() || destroyed.load(); }) &&
			generation->m_cancel, "owner destruction must request cancellation while GI tasks are outstanding");
		const bool returnedEarly = WaitUntil([&]() { return destroyed.load(); }, std::chrono::milliseconds(25));
		sampler->Release();
		owner.join();
		Require(!returnedEarly && destroyed && sampler->m_left == workers,
			"owner destruction must wait for the held samples to return");
		for (const auto& task : tasks)
			Require(task->IsFinished(), "every captured GI task must be finished when owner destruction returns");
	}
}

namespace Sailor::Tests
{
	uint64_t CountRuntimeGICompletedSamples(const RuntimeGIProbesService& service)
	{
		return Access::CompletedSamples(service);
	}

	void RunRuntimeGIProbesTaskTests()
	{
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		Require(scheduler && scheduler->GetNumThreads(EThreadType::GI) > 0, "this test requires the real App GI pool");
		const uint32_t workers = (std::min)(2u, scheduler->GetNumThreads(EThreadType::GI));
		TestReplacementAndDisable(workers, false);
		TestReplacementAndDisable(workers, true);
		TestOwnerTeardown(workers);
		std::cout << "Runtime GI workers observed: " << workers << '\n';
		std::cout << "Runtime GI tasks: GI queue, replaced/disabled jobs, retained snapshots and joined destruction passed\n";
	}
}
