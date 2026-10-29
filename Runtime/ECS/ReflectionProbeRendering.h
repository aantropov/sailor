#pragma once

#include "ECS/LightingReflection.h"
#include "RHI/Reflections.h"

namespace Sailor
{
	struct ReflectionProbeRenderStatus
	{
		uint64_t m_slotGeneration = 0u;
		uint64_t m_requestedGeneration = 0u;
		uint64_t m_publishedGeneration = 0u;
		uint64_t m_capturesSubmitted = 0u;
	};

	// Retained by submissions; this state never retains its owning World or ECS.
	class ReflectionProbeRenderState final
	{
	public:
		SAILOR_API void Upsert(uint32_t slot, uint64_t slotGeneration,
			const Framegraph::LocalReflectionCaptureRequest& request,
			RHI::RHITexturePtr staticSource = {});
		SAILOR_API void Request(uint32_t slot, uint64_t slotGeneration);
		SAILOR_API void Remove(uint32_t slot, uint64_t slotGeneration);
		SAILOR_API void Reset();
		SAILOR_API RHI::RHISceneReflectionsSnapshotPtr GetSnapshot() const;
		SAILOR_API ReflectionProbeRenderStatus GetStatus(uint32_t slot) const;
		SAILOR_API TVector<TRefPtr<LocalReflectionCaptureJob>> Capture(const RHI::RHISceneViewPtr& source);
		SAILOR_API void Poll(const RHI::RHIFrameGraphPtr& graph);
		SAILOR_API void Cancel(const TRefPtr<LocalReflectionCaptureJob>& capture);

	private:
		struct Probe
		{
			Framegraph::LocalReflectionCaptureRequest m_request;
			RHI::RHITexturePtr m_staticSource;
			RHI::RHICubemapPtr m_specular;
			RHI::RHICubemapPtr m_sheen;
			glm::vec3 m_capturePosition{};
			uint64_t m_slotGeneration = 0u;
			uint64_t m_configuration = 0u;
			uint64_t m_pendingGeneration = 0u;
			uint64_t m_requestedGeneration = 0u;
			uint64_t m_publishedGeneration = 0u;
			uint64_t m_capturesSubmitted = 0u;
		};

		struct Work
		{
			uint32_t m_slot = 0u;
			uint64_t m_slotGeneration = 0u;
			uint64_t m_configuration = 0u;
			uint64_t m_generation = 0u;
			Framegraph::LocalReflectionCaptureRequest m_request;
			TRefPtr<LocalReflectionCaptureJob> m_capture;
			RHI::RHITexturePtr m_staticSource;
			RHI::RHICubemapPtr m_raw;
			RHI::RHICubemapPtr m_specular;
			RHI::RHICubemapPtr m_sheen;
			RHI::RHIFencePtr m_filterCompletion;
		};

		bool StartFiltering(Work& work);
		void PublishSnapshot();
		void EnsureWarmup();
		void CancelSlot(uint32_t slot);
		bool IsCurrent(const Work& work) const;

		mutable SpinLock m_lock;
		std::array<Probe, RHI::MaxReflectionProbes> m_probes{};
		std::array<uint64_t, RHI::MaxReflectionProbes> m_slotGenerations{};
		std::array<uint64_t, RHI::MaxReflectionProbes> m_configurations{};
		TVector<Work> m_work;
		RHI::RHISceneReflectionsSnapshotPtr m_snapshot;
		Tasks::TaskPtr<bool> m_warmup;
		ShaderSetPtr m_specularShader;
		ShaderSetPtr m_sheenShader;
		uint64_t m_generation = 0u;
		uint64_t m_revision = 0u;
	};
}
