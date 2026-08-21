#pragma once

#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/Vector.h"
#include "Core/SpinLock.h"
#include "Engine/Types.h"
#include "Math/Math.h"
#include "Math/Bounds.h"
#include "Memory/SharedPtr.hpp"
#include "RHI/Types.h"

#include <limits>

namespace Sailor::RHI
{
	template<typename TTag>
	struct TGenerationalRenderHandle
	{
		uint32_t m_slot = (std::numeric_limits<uint32_t>::max)();
		uint32_t m_generation = 0u;

		bool IsValid() const
		{
			return m_slot != (std::numeric_limits<uint32_t>::max)() && m_generation != 0u;
		}

		bool operator==(const TGenerationalRenderHandle& rhs) const
		{
			return m_slot == rhs.m_slot && m_generation == rhs.m_generation;
		}

		size_t GetHash() const
		{
			size_t result = std::hash<uint32_t>{}(m_slot);
			HashCombine(result, m_generation);
			return result;
		}
	};

	struct RenderInstanceHandleTag;
	struct RenderItemHandleTag;
	struct SceneRangeHandleTag;
	struct MaterialVersionHandleTag;
	struct ShadowTileHandleTag;

	using RenderInstanceHandle = TGenerationalRenderHandle<RenderInstanceHandleTag>;
	using RenderItemHandle = TGenerationalRenderHandle<RenderItemHandleTag>;
	using SceneRangeHandle = TGenerationalRenderHandle<SceneRangeHandleTag>;
	using MaterialVersionHandle = TGenerationalRenderHandle<MaterialVersionHandleTag>;
	using ShadowTileHandle = TGenerationalRenderHandle<ShadowTileHandleTag>;

	enum class ESceneChangeBit : uint32_t
	{
		None = 0u,
		Add = 1u << 0u,
		Remove = 1u << 1u,
		Transform = 1u << 2u,
		Bounds = 1u << 3u,
		MeshOrLodTopology = 1u << 4u,
		Material = 1u << 5u,
		RenderState = 1u << 6u,
		ShadowState = 1u << 7u,
		SkeletonOffset = 1u << 8u,
		Mobility = 1u << 9u,
		ReplaceChunkRange = 1u << 10u,
		ReplaceBulkRange = 1u << 11u
	};

	using SceneChangeMask = uint32_t;

	constexpr SceneChangeMask ToMask(ESceneChangeBit bit)
	{
		return static_cast<SceneChangeMask>(bit);
	}

	constexpr SceneChangeMask operator|(ESceneChangeBit lhs, ESceneChangeBit rhs)
	{
		return ToMask(lhs) | ToMask(rhs);
	}

	struct PhysicalAllocation
	{
		uint32_t m_bufferGeneration = 0u;
		uint32_t m_offset = 0u;
		uint32_t m_count = 0u;
		uint32_t m_capacity = 0u;

		bool IsValid() const { return m_bufferGeneration != 0u && m_capacity != 0u; }
	};

	struct DirtySceneRange
	{
		uint32_t m_offset = 0u;
		uint32_t m_count = 0u;
	};

	struct RHISceneInstanceRecord
	{
		uint64_t m_producerKey = 0ull;
		EMobilityType m_mobility = EMobilityType::Static;
		glm::mat4 m_worldMatrix{ 1.0f };
		Math::AABB m_worldBounds{};
		RHIResourcePtr m_topology{};
		uint64_t m_topologyRevision = 0ull;
		uint64_t m_materialRevision = 0ull;
		uint64_t m_shadowRevision = 0ull;
		uint32_t m_skeletonOffset = (std::numeric_limits<uint32_t>::max)();
		uint32_t m_renderFlags = 0u;
	};

	static_assert(sizeof(RHISceneInstanceRecord) <= 160u,
		"Persistent scene records must not accumulate pass-local allocation metadata.");

	struct RHISceneChangeEntry
	{
		uint64_t m_revision = 0ull;
		RenderInstanceHandle m_handle{};
		SceneChangeMask m_changeMask = ToMask(ESceneChangeBit::None);
	};

	static_assert(sizeof(RHISceneChangeEntry) <= 24u,
		"The change journal must not duplicate complete scene records.");

	struct RHISceneMetrics
	{
		uint32_t m_numStaticInstances = 0u;
		uint32_t m_numStationaryInstances = 0u;
		uint32_t m_numDynamicInstances = 0u;
		uint32_t m_numDirtyChanges = 0u;
		uint32_t m_numCoalescedChanges = 0u;
		uint32_t m_numCowPages = 0u;
		uint32_t m_numFullRebuilds = 0u;
		uint64_t m_copiedCpuBytes = 0ull;
		uint64_t m_dynamicRewriteBytes = 0ull;
	};

	struct RHISceneRecordSlot
	{
		uint32_t m_generation = 0u;
		bool m_bActive = false;
		RHISceneInstanceRecord m_record{};
	};

	struct RHISceneRecordPage
	{
		static constexpr uint32_t NumRecords = 64u;
		RHISceneRecordSlot m_slots[NumRecords]{};
	};

	using RHISceneRecordPagePtr = TSharedPtr<RHISceneRecordPage>;

	class RHISceneRecordRoot final : public RHIResource
	{
	public:
		TVector<RHISceneRecordPagePtr> m_pages{};
		uint32_t m_generation = 1u;
	};

	using RHISceneRecordRootPtr = TRefPtr<RHISceneRecordRoot>;

	class RHISceneVersion final : public RHIResource
	{
	public:
		bool Resolve(RenderInstanceHandle handle, const RHISceneInstanceRecord*& outRecord) const;
		bool HasShadowChangesIntersecting(
			const RHISceneVersion& previousVersion,
			const Math::Frustum& shadowFrustum) const;

		uint64_t m_sceneRevision = 0ull;
		uint64_t m_sceneIdentity = 0ull;
		uint64_t m_staticRevision = 0ull;
		uint64_t m_stationaryRevision = 0ull;
		uint64_t m_dynamicRevision = 0ull;
		uint64_t m_materialRevision = 0ull;
		uint64_t m_shadowRevision = 0ull;
		uint64_t m_spatialRevision = 0ull;
		RHISceneRecordRootPtr m_staticRoot{};
		RHISceneRecordRootPtr m_recordsRoot{};
		TSharedPtr<TVector<RenderInstanceHandle>> m_staticHandles{};
		TSharedPtr<TVector<RenderInstanceHandle>> m_stationaryHandles{};
		TSharedPtr<TVector<RenderInstanceHandle>> m_dynamicHandles{};
	};

	using RHISceneVersionPtr = TRefPtr<RHISceneVersion>;

	class RHISceneFlightState final : public RHIResource
	{
	public:
		uint32_t m_flightSlot = 0u;
		uint64_t m_appliedRevision = 0ull;
		RHISceneVersionPtr m_appliedVersion{};
		TSharedPtr<TVector<RenderInstanceHandle>> m_stationaryHandles{};
		TSharedPtr<TVector<RenderInstanceHandle>> m_dynamicHandles{};
		TVector<RenderInstanceHandle> m_stationaryDirtyHandles{};
		TVector<RenderInstanceHandle> m_coalescedHandlesScratch{};
		TVector<uint8_t> m_coalescedSlotFlags{};
		bool m_bStationaryFullRebuild = false;
		RHISceneMetrics m_metrics{};
	};

	using RHISceneFlightStatePtr = TRefPtr<RHISceneFlightState>;

	class RHISceneRangeAllocator
	{
	public:
		SceneRangeHandle Allocate(uint32_t count);
		bool Resolve(SceneRangeHandle handle, PhysicalAllocation& outAllocation) const;
		bool Retire(SceneRangeHandle handle, uint64_t retirementRevision);
		void Collect(uint64_t minimumRetainedRevision);
		uint32_t GetBufferGeneration() const { return m_bufferGeneration; }
		uint32_t GetCapacity() const { return m_capacity; }

		static TVector<DirtySceneRange> CoalesceDirtyRanges(TVector<DirtySceneRange> ranges);

	private:
		struct RangeSlot
		{
			uint32_t m_generation = 1u;
			bool m_bActive = false;
			uint64_t m_retirementRevision = 0ull;
			PhysicalAllocation m_allocation{};
		};

		struct FreeRange
		{
			uint32_t m_bufferGeneration = 0u;
			uint32_t m_offset = 0u;
			uint32_t m_capacity = 0u;
		};

		uint32_t AllocateCapacity(uint32_t count) const;
		void MergeFreeRanges();

		TVector<RangeSlot> m_slots{};
		TVector<uint32_t> m_reusableSlots{};
		TVector<uint32_t> m_retiredSlots{};
		TVector<FreeRange> m_freeRanges{};
		uint32_t m_bufferGeneration = 1u;
		uint32_t m_capacity = 0u;
		uint32_t m_nextOffset = 0u;
	};

	class RHIScene final : public RHIResource
	{
	public:
		explicit RHIScene(uint32_t numFlightSlots = 2u);

		RenderInstanceHandle AddInstance(const RHISceneInstanceRecord& record);
		bool UpdateInstance(
			RenderInstanceHandle handle,
			const RHISceneInstanceRecord& record,
			SceneChangeMask changeMask);
		bool RemoveInstance(RenderInstanceHandle handle);
		bool ResolveCurrent(RenderInstanceHandle handle, RHISceneInstanceRecord& outRecord) const;

		RHISceneVersionPtr PublishVersion(
			uint64_t materialRevision = 0ull,
			uint64_t shadowRevision = 0ull,
			uint64_t spatialRevision = 0ull);
		RHISceneVersionPtr GetCurrentVersion() const;
		RHISceneFlightStatePtr PrepareFlight(uint32_t flightSlot, RHISceneVersionPtr targetVersion);
		void CollectGarbage();

		uint64_t GetRevision() const { return m_revision; }
		uint64_t GetJournalFirstRevision() const;
		const RHISceneMetrics& GetMetrics() const { return m_metrics; }

	private:
		struct LogicalSlot
		{
			uint32_t m_generation = 1u;
			bool m_bActive = false;
			uint64_t m_retirementRevision = 0ull;
			RHISceneInstanceRecord m_record{};
		};

		bool ResolveSlot(RenderInstanceHandle handle, LogicalSlot*& outSlot);
		bool ResolveSlot(RenderInstanceHandle handle, const LogicalSlot*& outSlot) const;
		void AppendChange(RenderInstanceHandle handle, SceneChangeMask mask);
		void BumpMobilityRevision(EMobilityType mobility);
		void RebuildHandleLists(RHISceneVersion& version);
		RHISceneRecordRootPtr BuildRecordRoot();
		void RebuildFlight(RHISceneFlightState& flight, const RHISceneVersion& version);
		uint64_t MinimumRetainedRevision() const;

		mutable SpinLock m_lock;
		uint64_t m_revision = 0ull;
		uint64_t m_staticRevision = 0ull;
		uint64_t m_stationaryRevision = 0ull;
		uint64_t m_dynamicRevision = 0ull;
		uint64_t m_lastPublishedRevision = (std::numeric_limits<uint64_t>::max)();
		bool m_bHandleListsDirty = true;
		TVector<LogicalSlot> m_slots{};
		TVector<uint32_t> m_reusableSlots{};
		TVector<uint32_t> m_retiredSlots{};
		TVector<uint32_t> m_dirtySlots{};
		TVector<uint8_t> m_dirtySlotFlags{};
		TVector<uint8_t> m_cowPageScratch{};
		TVector<RHISceneChangeEntry> m_journal{};
		RHISceneRecordRootPtr m_currentRoot{};
		RHISceneVersionPtr m_currentVersion{};
		TVector<RHISceneVersionPtr> m_retainedVersions{};
		TVector<RHISceneFlightStatePtr> m_flights{};
		RHISceneMetrics m_metrics{};
	};

	using RHIScenePtr = TRefPtr<RHIScene>;
}
