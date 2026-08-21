#pragma once

#include "Containers/Map.h"
#include "Containers/Vector.h"
#include "Core/SpinLock.h"
#include "RHI/Types.h"

#include <atomic>
#include <cstdint>
#include <utility>

namespace Sailor::RHI
{
	class RHISubmissionCompletionToken final : public RHIResource
	{
	public:
		void Reset() const
		{
			m_state.store(0u, std::memory_order_release);
		}

		void Complete(bool bSucceeded) const
		{
			m_state.store(bSucceeded ? 2u : 1u, std::memory_order_release);
		}

		bool IsPending() const
		{
			return m_state.load(std::memory_order_acquire) == 0u;
		}

		bool IsSuccessful() const
		{
			return m_state.load(std::memory_order_acquire) == 2u;
		}

	private:
		mutable std::atomic<uint8_t> m_state{ 0u };
	};

	using RHISubmissionCompletionTokenPtr = TRefPtr<RHISubmissionCompletionToken>;

	struct RHIFrameGraphResourceKey
	{
		const void* m_owner = nullptr;
		uint32_t m_cameraIndex = 0u;
		uint32_t m_variant = 0u;
		uint64_t m_generation = 0ull;

		bool operator==(const RHIFrameGraphResourceKey& rhs) const
		{
			return m_owner == rhs.m_owner &&
				m_cameraIndex == rhs.m_cameraIndex &&
				m_variant == rhs.m_variant &&
				m_generation == rhs.m_generation;
		}

		size_t GetHash() const
		{
			size_t result = std::hash<const void*>{}(m_owner);
			HashCombine(result, m_cameraIndex);
			HashCombine(result, m_variant);
			HashCombine(result, m_generation);
			return result;
		}
	};

	class RHIFrameGraphSubmissionResource : public RHIResource
	{
	public:
		virtual void ResetForSubmission() = 0;
		virtual void InvalidateSubmission() { ResetForSubmission(); }
	};

	using RHIFrameGraphSubmissionResourcePtr = TRefPtr<RHIFrameGraphSubmissionResource>;

	/**
	 * Resources retained by one driver flight slot. The renderer calls BeginSubmission
	 * only after the slot completion fence has signalled, so derived resources can keep
	 * capacity and safely overwrite their mutable GPU ranges on the next use.
	 */
	class RHIRenderSubmissionContext final : public RHIResource
	{
	public:
		void BeginSubmission(
			uint64_t submissionId,
			uint32_t flightSlot,
			uint64_t sceneRevision = 0ull,
			uint64_t materialRevision = 0ull,
			uint64_t resourceGeneration = 0ull)
		{
			m_lock.Lock();
			m_submissionId = submissionId;
			m_flightSlot = flightSlot;
			m_sceneRevision = sceneRevision;
			m_materialRevision = materialRevision;
			m_resourceGeneration = resourceGeneration;
			m_resourceReadySemaphore.Clear();
			m_retainedResources.Clear(false);
			m_expiredFrameGraphResourcesScratch.Clear(false);
			m_expiredFrameGraphResourcesScratch.Reserve(m_frameGraphResources.Num());
			for (const auto& entry : m_frameGraphResources)
			{
				if (entry.First().m_generation != resourceGeneration)
				{
					m_expiredFrameGraphResourcesScratch.Add(entry.First());
				}
			}
			for (const auto& key : m_expiredFrameGraphResourcesScratch)
			{
				m_frameGraphResources.Remove(key);
			}

			for (const auto& entry : m_frameGraphResources)
			{
				if (entry.Second() && *entry.Second())
				{
					(*entry.Second())->ResetForSubmission();
				}
			}
			m_lock.Unlock();
		}

		template<typename TResource, typename... TArgs>
		TRefPtr<TResource> GetOrAddFrameGraphResources(
			const void* owner,
			uint32_t cameraIndex,
			uint32_t variant,
			TArgs&&... args) const
			requires IsBaseOf<RHIFrameGraphSubmissionResource, TResource>
		{
			const RHIFrameGraphResourceKey key{
				owner,
				cameraIndex,
				variant,
				m_resourceGeneration };
			TRefPtr<TResource> result;

			m_lock.Lock();
			RHIFrameGraphSubmissionResourcePtr* existing = nullptr;
			if (m_frameGraphResources.Find(key, existing) && existing && *existing)
			{
				result = existing->template DynamicCast<TResource>();
			}

			if (!result)
			{
				result = TRefPtr<TResource>::Make(std::forward<TArgs>(args)...);
				m_frameGraphResources[key] = result;
			}
			m_lock.Unlock();

			return result;
		}

		void RetainResource(RHIResourcePtr resource)
		{
			if (!resource)
			{
				return;
			}

			m_lock.Lock();
			m_retainedResources.Emplace(std::move(resource));
			m_lock.Unlock();
		}

		void SetResourceReadySemaphore(RHISemaphorePtr semaphore)
		{
			m_lock.Lock();
			m_resourceReadySemaphore = std::move(semaphore);
			m_lock.Unlock();
		}

		void InvalidateSubmissionResources()
		{
			m_lock.Lock();
			m_resourceReadySemaphore.Clear();
			for (const auto& entry : m_frameGraphResources)
			{
				if (entry.Second() && *entry.Second())
				{
					(*entry.Second())->InvalidateSubmission();
				}
			}
			m_lock.Unlock();
		}

		uint64_t GetSubmissionId() const { return m_submissionId; }
		uint32_t GetFlightSlot() const { return m_flightSlot; }
		uint64_t GetSceneRevision() const { return m_sceneRevision; }
		uint64_t GetMaterialRevision() const { return m_materialRevision; }
		const RHISemaphorePtr& GetResourceReadySemaphore() const { return m_resourceReadySemaphore; }

	private:
		mutable SpinLock m_lock;
		uint64_t m_submissionId = 0ull;
		uint32_t m_flightSlot = 0u;
		uint64_t m_sceneRevision = 0ull;
		uint64_t m_materialRevision = 0ull;
		uint64_t m_resourceGeneration = 0ull;
		mutable TMap<RHIFrameGraphResourceKey, RHIFrameGraphSubmissionResourcePtr> m_frameGraphResources;
		TVector<RHIFrameGraphResourceKey> m_expiredFrameGraphResourcesScratch;
		TVector<RHIResourcePtr> m_retainedResources;
		RHISemaphorePtr m_resourceReadySemaphore{};
	};

	using RHIRenderSubmissionContextPtr = TRefPtr<RHIRenderSubmissionContext>;
}
