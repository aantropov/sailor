#include "GlobalIllumination/RuntimeGIProbesServiceInternal.h"
#include "RHI/GlobalIllumination.h"

#include <barrier>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace Sailor;

namespace Sailor
{
	class RuntimeGIProbesServiceTestAccess
	{
	public:
		using Job = RuntimeGIProbesService::Impl::Job;
		using Publication = RuntimeGIProbesService::Impl::Publication;

		static Job TakeJob(RuntimeGIProbesService& service)
		{
			Job job;
			if (!service.m_impl->TryTakeJob(service.m_impl->m_generation, job))
			{
				throw std::runtime_error("expected a queued runtime probe job");
			}
			return job;
		}

		static void CompleteJob(RuntimeGIProbesService& service, Job& job)
		{
			std::string diagnostic;
			const bool success = service.m_impl->ExecuteJob(job, diagnostic);
			service.m_impl->CommitJob(job, success, diagnostic, 0.0);
			if (!success)
			{
				throw std::runtime_error(diagnostic);
			}
		}

		static Publication Capture(RuntimeGIProbesService& service)
		{
			Publication publication;
			if (!service.m_impl->CapturePublication(publication))
			{
				throw std::runtime_error("expected ready, unpublished probe data");
			}
			return publication;
		}

		static std::string Prepare(Publication& publication)
		{
			return RuntimeGIProbesService::Impl::PreparePublication(publication);
		}

		static void Commit(RuntimeGIProbesService& service, Publication& publication, std::string diagnostic)
		{
			service.m_impl->CommitPublication(publication, std::move(diagnostic));
		}

		static void FailJob(RuntimeGIProbesService& service, const Job& job)
		{
			service.m_impl->CommitJob(job, false, "injected trace failure", 0.0);
		}
	};
}

namespace
{
	using Access = RuntimeGIProbesServiceTestAccess;

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	class DirectionalSampler final : public IGIProbeBakeRaySampler
	{
	public:
		bool Sample(const glm::vec3&, const glm::vec3& direction, float distance, uint32_t,
			GIProbeBakeRaySample& sample, std::string&) const override
		{
			sample = {};
			sample.m_distance = distance;
			sample.m_radiance = glm::vec3(0.5f) + direction * 0.25f;
			return true;
		}
	};

	RuntimeGIProbesStartRequest MakeRequest()
	{
		RuntimeGIProbesStartRequest request;
		request.m_sampler = TSharedPtr<DirectionalSampler>::Make();
		request.m_geometryBounds.m_min = glm::vec3(-0.1f);
		request.m_geometryBounds.m_max = glm::vec3(0.1f);
		request.m_worldSettings.m_bounceCount = 1;
		request.m_worldSettings.m_minProbeSpacing = 1.0f;
		request.m_worldSettings.m_maxRayDistance = 10.0f;
		request.m_qualitySettings.m_bEnabled = true;
		request.m_qualitySettings.m_maxActiveProbes = 8;
		request.m_qualitySettings.m_initialSamplesPerProbe = 16;
		request.m_qualitySettings.m_targetSamplesPerProbe = 48;
		request.m_qualitySettings.m_workerCount = 1;
		request.m_qualitySettings.m_cpuDutyFraction = 1.0f;
		request.m_qualitySettings.m_maxPublicationsPerSecond = 60.0f;
		return request;
	}

	void WarmGrid(RuntimeGIProbesService& service, const RuntimeGIProbesStartRequest& request)
	{
		service.SetWorkAllowed(false);
		std::string diagnostic;
		Require(service.Start(request, diagnostic), diagnostic);
		service.SetWorkAllowed(true);
		for (uint32_t i = 0; i < service.GetStatus().m_activeProbeCount; ++i)
		{
			auto job = Access::TakeJob(service);
			Access::CompleteJob(service, job);
		}
		Require(service.GetStatus().m_readyProbeCount == 8 && !service.GetPublishedData(),
			"all eight initial probes must be ready but not published");
	}

	bool SameLighting(const GIProbe& lhs, const GIProbe& rhs)
	{
		return std::memcmp(lhs.m_irradiance.data(), rhs.m_irradiance.data(), sizeof(lhs.m_irradiance)) == 0;
	}

	void WaitForPublication(RuntimeGIProbesService& service, uint64_t previousRevision)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (service.GetStatus().m_publishedRevision <= previousRevision && std::chrono::steady_clock::now() < deadline)
		{
			service.Tick();
			std::this_thread::yield();
		}
		Require(service.GetStatus().m_publishedRevision > previousRevision,
			"a job taken before publication must still publish its later completed refinement");
	}

	void TestJobTakenBeforePublication()
	{
		RuntimeGIProbesService service;
		WarmGrid(service, MakeRequest());
		auto job = Access::TakeJob(service);
		service.SetWorkAllowed(false);
		service.Tick();
		const auto first = service.GetPublishedData();
		const uint64_t revision = service.GetStatus().m_publishedRevision;
		Require(first && revision > 0, "the initial ready grid must publish");
		Access::CompleteJob(service, job);
		Require(!SameLighting(first->m_probes[job.m_probeIndex], job.m_work.m_probe),
			"the real refinement job must produce different lighting samples");
		WaitForPublication(service, revision);
		const auto refined = service.GetPublishedData();
		Require(refined != first && SameLighting(refined->m_probes[job.m_probeIndex], job.m_work.m_probe),
			"the second immutable snapshot must contain the completed refinement");
		Require(!SameLighting(first->m_probes[job.m_probeIndex], job.m_work.m_probe),
			"publishing refinement must not modify an older retained snapshot");
	}

	void TestWorkerCommitDuringPublication()
	{
		RuntimeGIProbesService service;
		WarmGrid(service, MakeRequest());
		auto job = Access::TakeJob(service);
		auto publication = Access::Capture(service);
		const auto capturedData = publication.m_data;
		std::barrier captured(2);
		std::exception_ptr workerError;
		std::jthread worker([&]()
		{
			captured.arrive_and_wait();
			try
			{
				Access::CompleteJob(service, job);
				Require(service.GetStatus().m_readyProbeCount == 8,
					"worker commit and status reads must proceed during publication preparation");
			}
			catch (...)
			{
				workerError = std::current_exception();
			}
		});
		captured.arrive_and_wait();
		auto diagnostic = Access::Prepare(publication);
		worker.join();
		if (workerError)
		{
			std::rethrow_exception(workerError);
		}
		Require(diagnostic.empty(), diagnostic);
		service.SetWorkAllowed(false);
		Access::Commit(service, publication, {});
		Require(service.GetPublishedData() == capturedData &&
			!SameLighting(capturedData->m_probes[job.m_probeIndex], job.m_work.m_probe),
			"publication must contain its captured data, not partially incorporate a later commit");
		WaitForPublication(service, service.GetStatus().m_publishedRevision);
		Require(SameLighting(service.GetPublishedData()->m_probes[job.m_probeIndex], job.m_work.m_probe),
			"a concurrent worker's completed update must remain unpublished until the next snapshot");
		Require(!SameLighting(capturedData->m_probes[job.m_probeIndex], job.m_work.m_probe),
			"the retained earlier snapshot must remain immutable");
	}

	void TestOutOfOrderPublication()
	{
		RuntimeGIProbesService service;
		WarmGrid(service, MakeRequest());
		auto older = Access::Capture(service);
		Require(Access::Prepare(older).empty(), "the older capture must be valid");
		auto job = Access::TakeJob(service);
		Access::CompleteJob(service, job);
		auto newer = Access::Capture(service);
		Require(Access::Prepare(newer).empty(), "the newer capture must be valid");
		const auto newData = newer.m_data;
		Access::Commit(service, newer, {});
		const auto revision = service.GetStatus().m_publishedRevision;
		Access::Commit(service, older, {});
		Require(service.GetPublishedData() == newData && service.GetStatus().m_publishedRevision == revision,
			"an older publication must not replace a newer snapshot of the same generation");
		older.m_data->m_probes[0].m_irradiance[0].x = std::numeric_limits<float>::quiet_NaN();
		auto diagnostic = Access::Prepare(older);
		Require(!diagnostic.empty(), "an invalid late payload must fail actual validation");
		Access::Commit(service, older, std::move(diagnostic));
		Require(service.GetStatus().m_lifecycle != ERuntimeGIProbesLifecycle::Failed &&
			service.GetPublishedData() == newData,
			"failure of an obsolete publication must not fail the newer data");
		service.SetWorkAllowed(false);
		for (uint32_t i = 0; i < 4; ++i)
		{
			service.Tick();
		}
		Require(service.GetStatus().m_publishedRevision == revision,
			"publication alone must not create another dirty revision");
	}

	void TestReplacedAndDisabledPublication()
	{
		RuntimeGIProbesService service;
		auto request = MakeRequest();
		WarmGrid(service, request);
		auto older = Access::Capture(service);
		Require(Access::Prepare(older).empty(), "the old generation capture must prepare");
		++request.m_geometryGeneration;
		request.m_bReuseExistingProbes = false;
		WarmGrid(service, request);
		auto current = Access::Capture(service);
		Require(Access::Prepare(current).empty(), "the replacement capture must prepare");
		const auto currentData = current.m_data;
		Access::Commit(service, current, {});
		const auto revision = service.GetStatus().m_publishedRevision;
		Access::Commit(service, older, {});
		Require(service.GetPublishedData() == currentData && service.GetStatus().m_publishedRevision == revision &&
			service.GetStatus().m_sceneGeneration == request.m_geometryGeneration,
			"a prepared old generation must not overwrite its replacement");

		RuntimeGIProbesService disabled;
		WarmGrid(disabled, MakeRequest());
		auto pending = Access::Capture(disabled);
		Require(Access::Prepare(pending).empty(), "the pending capture must prepare before disable");
		disabled.Disable();
		Access::Commit(disabled, pending, {});
		Require(!disabled.GetPublishedData() && disabled.GetStatus().m_publishedRevision == 0 &&
			disabled.GetStatus().m_lifecycle == ERuntimeGIProbesLifecycle::Disabled,
			"disable must prevent a prepared publication from reviving the provider");
	}

	void TestFailedPublication()
	{
		RuntimeGIProbesService service;
		WarmGrid(service, MakeRequest());
		auto pending = Access::Capture(service);
		auto job = Access::TakeJob(service);
		Access::FailJob(service, job);
		auto diagnostic = Access::Prepare(pending);
		Require(!diagnostic.empty(), "preparation must observe generation cancellation");
		Access::Commit(service, pending, std::move(diagnostic));
		Require(!service.GetPublishedData() && service.GetStatus().m_lifecycle == ERuntimeGIProbesLifecycle::Failed &&
			service.GetStatus().m_diagnostic == "injected trace failure",
			"a pending publication must preserve the worker failure and not publish cancelled data");

		RuntimeGIProbesService invalid;
		WarmGrid(invalid, MakeRequest());
		auto badData = Access::Capture(invalid);
		badData.m_data->m_probes[0].m_irradiance[0].x = std::numeric_limits<float>::quiet_NaN();
		diagnostic = Access::Prepare(badData);
		Require(!diagnostic.empty(), "non-finite captured lighting must fail preparation");
		Access::Commit(invalid, badData, std::move(diagnostic));
		Require(!invalid.GetPublishedData() && invalid.GetStatus().m_lifecycle == ERuntimeGIProbesLifecycle::Failed,
			"invalid current data must fail without publishing a partial payload");
	}

	uint64_t PackedUploadBytes(const GIProbesDataPtr& data)
	{
		RHI::RHIGlobalIlluminationGpuLayout layout;
		std::string diagnostic;
		Require(RHI::BuildGlobalIlluminationGpuLayout(*data, layout, diagnostic), diagnostic);
		RHI::RHIGlobalIlluminationSnapshot snapshot;
		snapshot.m_layout = data;
		RHI::RHIGlobalIlluminationState state;
		state.m_data = data;
		state.m_effectiveWeight = 1.0f;
		snapshot.m_states.Add(state);
		TVector<RHI::RHIGlobalIlluminationGpuCoefficients> coefficients;
		TVector<RHI::RHIGlobalIlluminationGpuState> states;
		Require(RHI::BuildGlobalIlluminationGpuCoefficients(snapshot, coefficients, diagnostic), diagnostic);
		Require(RHI::BuildGlobalIlluminationGpuStates(snapshot, states, diagnostic), diagnostic);
		return sizeof(RHI::RHIGlobalIlluminationGpuHeader) +
			layout.m_nodes.Num() * sizeof(RHI::RHIGlobalIlluminationGpuBvhNode) +
			layout.m_bricks.Num() * sizeof(RHI::RHIGlobalIlluminationGpuBrick) +
			layout.m_probes.Num() * sizeof(RHI::RHIGlobalIlluminationGpuProbe) +
			coefficients.Num() * sizeof(RHI::RHIGlobalIlluminationGpuCoefficients) +
			states.Num() * sizeof(RHI::RHIGlobalIlluminationGpuState);
	}

	void TestPackedPublicationBudget()
	{
		RuntimeGIProbesService reference;
		auto request = MakeRequest();
		WarmGrid(reference, request);
		reference.SetWorkAllowed(false);
		reference.Tick();
		const auto packedBytes = PackedUploadBytes(reference.GetPublishedData());
		Require(reference.GetStatus().m_publishedBytes == packedBytes,
			"published byte telemetry must describe the packed GPU payload, not CPU object sizes");
		request.m_qualitySettings.m_maxDirtyUploadBytesPerFrame = static_cast<uint32_t>(packedBytes);
		request.m_qualitySettings.m_maxActiveProbes = 64;
		RuntimeGIProbesService exact;
		WarmGrid(exact, request);
		Require(exact.GetStatus().m_capacity == 8, "exact eight-probe upload budget must admit exactly eight probes");
		exact.SetWorkAllowed(false);
		exact.Tick();
		Require(exact.GetStatus().m_publishedBytes == packedBytes &&
			PackedUploadBytes(exact.GetPublishedData()) == packedBytes,
			"a budget equal to the packed grid size must publish the complete grid");
		--request.m_qualitySettings.m_maxDirtyUploadBytesPerFrame;
		std::string diagnostic;
		RuntimeGIProbesService insufficient;
		Require(!insufficient.Start(request, diagnostic),
			"one byte below the minimum complete grid must fail before tracing starts");
	}
}

int main()
{
	try
	{
		TestJobTakenBeforePublication();
		TestWorkerCommitDuringPublication();
		TestOutOfOrderPublication();
		TestReplacedAndDisabledPublication();
		TestFailedPublication();
		TestPackedPublicationBudget();
		std::cout << "Runtime GI publication tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Runtime GI publication tests failed: " << error.what() << '\n';
		return 1;
	}
}
