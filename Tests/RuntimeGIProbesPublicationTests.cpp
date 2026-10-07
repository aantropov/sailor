#include "GlobalIllumination/RuntimeGIProbesServiceInternal.h"
#include "RHI/GlobalIllumination.h"

#include <barrier>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
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

	void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
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

	void TestWorkAndPublicationProgress()
	{
		class MixedValiditySampler final : public IGIProbeBakeRaySampler
		{
		public:
			explicit MixedValiditySampler(uint32_t embeddedProbeCount) : m_embeddedProbeCount(embeddedProbeCount) {}
			bool Sample(const glm::vec3& origin, const glm::vec3& direction, float distance, uint32_t,
				GIProbeBakeRaySample& sample, std::string&) const override
			{
				sample = {};
				sample.m_bHit = m_embeddedProbeCount == 8 || (m_embeddedProbeCount == 4 && origin.x < 0.0f);
				sample.m_bBackFace = sample.m_bHit;
				sample.m_distance = sample.m_bHit ? 0.01f : distance;
				sample.m_radiance = glm::vec3(0.5f) + direction * 0.25f;
				return true;
			}
		private:
			uint32_t m_embeddedProbeCount;
		};

		for (uint32_t embeddedProbeCount : { 0u, 4u, 8u })
		{
			RuntimeGIProbesService service;
			auto request = MakeRequest();
			request.m_sampler = TSharedPtr<MixedValiditySampler>::Make(embeddedProbeCount);
			WarmGrid(service, request);
			auto publication = Access::Capture(service);
			Require(Access::Prepare(publication).empty(), "mixed-validity publication must validate");
			Access::Commit(service, publication, {});
			const auto retained = service.GetPublishedData();
			const auto original = *retained;
			uint32_t invalidCount = 0;
			for (const auto& probe : retained->m_probes) invalidCount += probe.m_validity <= 0.05f;
			Require(invalidCount == embeddedProbeCount, "the fixture must contain zero, four or eight embedded probes");

			const uint32_t remainingBatches = (8 - invalidCount) * 2;
			for (uint32_t batch = 0; batch <= remainingBatches; ++batch)
			{
				const auto status = service.GetStatus();
				const float expected = float(invalidCount * 48 + (8 - invalidCount) * 16 + batch * 16) / (8 * 48);
				Require(std::abs(status.m_refinement - expected) < 0.000001f &&
					status.m_readyProbeCount == 8 && status.m_coverage == 1.0f,
					"coverage and refinement must account for valid samples and terminal embedded probes independently");
				Require(status.m_lifecycle == (batch == remainingBatches ? ERuntimeGIProbesLifecycle::Ready : ERuntimeGIProbesLifecycle::Tracing),
					"full initial coverage must not report Ready before the final refinement batch commits");
				if (batch == remainingBatches) break;
				auto job = Access::TakeJob(service);
				Require(service.GetStatus().m_refinement == status.m_refinement,
					"taking a job must not count samples that have not been committed");
				Access::CompleteJob(service, job);
			}
			for (size_t i = 0; i < retained->m_probes.Num(); ++i)
			{
				Require(SameLighting(retained->m_probes[i], original.m_probes[i]) &&
					retained->m_probes[i].m_position == original.m_probes[i].m_position,
					"working accumulators and resolved results must not mutate an already published snapshot");
			}

			std::string diagnostic;
			service.SetWorkAllowed(false);
			Require(service.Start(request, diagnostic), diagnostic);
			service.SetWorkAllowed(true);
			Require(service.GetStatus().m_lifecycle == ERuntimeGIProbesLifecycle::Ready && service.GetStatus().m_refinement == 1.0f,
				"reusing a completed generation must reconstruct exact progress without tracing again");
			service.SetWorkAllowed(false);
			++request.m_lightingGeneration;
			Require(service.Start(request, diagnostic), diagnostic);
			service.SetWorkAllowed(true);
			const auto relighting = service.GetStatus();
			Require(relighting.m_readyProbeCount == invalidCount && relighting.m_refinement == float(invalidCount) / 8 &&
				relighting.m_lifecycle == (invalidCount == 8 ? ERuntimeGIProbesLifecycle::Ready : ERuntimeGIProbesLifecycle::Tracing) &&
				service.GetPublishedData() == retained,
				"relighting must reuse terminal transport, reset irradiance progress and retain the prior snapshot");
			service.Disable();
		}
		std::cout << "Runtime GI progress: valid/embedded probes, every refinement batch, retained snapshots and reused lighting passed\n";
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
		const auto capturedData = publication.m_snapshot;
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
		const auto newData = newer.m_snapshot;
		Access::Commit(service, newer, {});
		const auto revision = service.GetStatus().m_publishedRevision;
		Access::Commit(service, older, {});
		Require(service.GetPublishedData() == newData && service.GetStatus().m_publishedRevision == revision,
			"an older publication must not replace a newer snapshot of the same generation");
		older.m_snapshot->m_probes[0].m_irradiance[0].x = std::numeric_limits<float>::quiet_NaN();
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
		const auto currentData = current.m_snapshot;
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
		badData.m_snapshot->m_probes[0].m_irradiance[0].x = std::numeric_limits<float>::quiet_NaN();
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
		TestWorkAndPublicationProgress();
		std::cout << "Runtime GI publication tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Runtime GI publication tests failed: " << error.what() << '\n';
		return 1;
	}
}
