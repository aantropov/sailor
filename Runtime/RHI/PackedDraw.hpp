#pragma once

#include "RHI/Batch.hpp"
#include "RHI/SceneView.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace Sailor::RHI
{
	struct RHIPackedDrawSceneState
	{
		TSharedPtr<const TVector<RHISceneVersionPtr>> m_sceneVersions{};
		TSharedPtr<const TVector<RHISceneVersionPtr>> m_motionSceneVersions{};
		size_t m_configurationRevision = 0u;
	};

	// Borrowed proxies are valid while the current and cached scene states live.
	struct RHIPackedDrawSceneChanges
	{
		TVector<RHIVisibleSceneProxy> m_updated{};
		TVector<RHIVisibleSceneProxy> m_removed{};
		uint32_t m_numComparedRecords = 0u;

		SAILOR_API void Gather(const RHIPackedDrawSceneState& current,
			const RHIPackedDrawSceneState* previous, EMobilityType mobility, bool bShadowCastersOnly = false);
		void Clear()
		{
			m_updated.Clear(false);
			m_removed.Clear(false);
			m_numComparedRecords = 0u;
		}
	};

	inline uint64_t BuildPackedDrawStableKey(
		RenderInstanceHandle handle,
		uint64_t producerKey,
		uint32_t groupIndex,
		uint32_t meshIndex,
		uint32_t instanceIndex)
	{
		size_t result = handle.IsValid() ? std::hash<uint32_t>{}(handle.m_slot) :
			std::hash<uint64_t>{}(producerKey);
		HashCombine(
			result,
			producerKey,
			handle.m_generation,
			groupIndex,
			meshIndex,
			instanceIndex);
		return static_cast<uint64_t>(result);
	}

	inline uint64_t BuildPackedDrawRangeKey(
		RenderInstanceHandle handle,
		uint64_t producerKey,
		const void* topologyIdentity = nullptr)
	{
		size_t result = handle.IsValid() ? std::hash<uint32_t>{}(handle.m_slot) :
			std::hash<uint64_t>{}(producerKey);
		HashCombine(result, producerKey, handle.m_generation, topologyIdentity);
		return static_cast<uint64_t>(result);
	}

	struct PackedDrawItem
	{
		RHIBatch m_batch{};
		RHIMeshPtr m_mesh{};
		uint32_t m_instanceIndex = 0u;
		uint64_t m_stableSortKey = 0ull;
		size_t m_batchSortHash = 0u;
	};

	SAILOR_API void SortPackedDrawItems(TVector<PackedDrawItem>& items, TVector<uint32_t>& indices);

	struct PackedDrawGroup
	{
		RHIBatch m_batch{};
		RHIMeshPtr m_mesh{};
		uint32_t m_firstInstance = 0u;
		uint32_t m_numInstances = 0u;
	};

	SAILOR_API uint32_t GetPackedDrawRunEnd(const TVector<PackedDrawGroup>& groups, uint32_t runBegin);

	struct PackedDrawPacketMetrics
	{
		uint64_t m_instanceUploadBytes = 0ull;
		uint64_t m_indexUploadBytes = 0ull;
		uint64_t m_indirectUploadBytes = 0ull;
		uint32_t m_dirtyInstanceRanges = 0u;
		bool m_bReusedInstancePayload = false;
		bool m_bSharedImmutablePayload = false;
	};

	template<typename TPerInstanceData>
	struct TPackedDrawArenaPage
	{
		static constexpr uint32_t NumInstances = 64u;
		std::array<TPerInstanceData, NumInstances> m_instances{};
	};

	template<typename TPerInstanceData>
	using TPackedDrawArenaPagePtr = TSharedPtr<const TPackedDrawArenaPage<TPerInstanceData>>;

	struct PackedDrawArenaMaterialRun
	{
		uint32_t m_first = 0u;
		uint32_t m_count = 0u;
		RHIMaterialVersionPtr m_version{};
	};

	inline void AppendPackedDrawArenaMaterialVersion(
		TVector<PackedDrawArenaMaterialRun>& runs,
		RHIMaterialVersionPtr version)
	{
		if (!runs.IsEmpty() && runs.Last()->m_version == version)
		{
			++runs.Last()->m_count;
			return;
		}
		const uint32_t first = runs.IsEmpty() ? 0u :
			runs.Last()->m_first + runs.Last()->m_count;
		runs.Add({ first, 1u, std::move(version) });
	}

	struct PackedDrawArenaItemOffset
	{
		uint64_t m_stableKey = 0ull;
		uint32_t m_relativeIndex = 0u;
	};

	struct PackedDrawArenaRange
	{
		uint64_t m_contentRevision = 0ull;
		uint32_t m_offset = 0u;
		uint32_t m_count = 0u;
		uint32_t m_capacity = 0u;
		TSharedPtr<const TVector<PackedDrawArenaItemOffset>> m_itemOffsets{};
		TSharedPtr<const TVector<PackedDrawArenaMaterialRun>> m_materialVersionRuns{};
	};

	struct PackedDrawArenaRangePage
	{
		static constexpr uint32_t NumRanges = 64u;
		std::array<PackedDrawArenaRange, NumRanges> m_ranges{};
	};

	using PackedDrawArenaRangeIndices = TMap<uint64_t, uint32_t>;

	template<typename TPerInstanceData>
	struct TPackedDrawPacketPayload
	{
		TVector<TPerInstanceData> m_instances{};
		TVector<PackedDrawGroup> m_groups{};
		TVector<TPackedDrawArenaPagePtr<TPerInstanceData>> m_arenaPages{};
		TSharedPtr<const PackedDrawArenaRangeIndices> m_arenaRangeIndices{};
		TVector<TSharedPtr<const PackedDrawArenaRangePage>> m_arenaRangePages{};
		uint32_t m_numArenaRangeSlots = 0u;
		uint32_t m_arenaCapacity = 0u;

		bool IsPagedArena() const
		{
			return m_arenaRangeIndices != nullptr;
		}

		const PackedDrawArenaRange* FindRange(uint64_t rangeKey) const
		{
			const uint32_t* index = nullptr;
			if (!m_arenaRangeIndices || !m_arenaRangeIndices->Find(rangeKey, index)) return nullptr;
			return &m_arenaRangePages[*index / PackedDrawArenaRangePage::NumRanges]->
				m_ranges[*index % PackedDrawArenaRangePage::NumRanges];
		}

		uint32_t GetNumStorageInstances() const
		{
			return IsPagedArena() ? m_arenaCapacity :
				static_cast<uint32_t>(m_instances.Num());
		}

		bool FindInstance(
			uint64_t rangeKey,
			uint64_t stableKey,
			uint32_t& outInstanceIndex,
			RHIMaterialVersionPtr* outMaterialVersion = nullptr) const
		{
			const auto* range = FindRange(rangeKey);
			if (!range || !range->m_itemOffsets)
			{
				return false;
			}
			uint32_t relativeIndex = 0u;
			size_t first = 0u;
			size_t last = range->m_itemOffsets->Num();
			while (first < last)
			{
				const size_t middle = first + (last - first) / 2u;
				const auto& candidate = (*range->m_itemOffsets)[middle];
				if (candidate.m_stableKey < stableKey)
				{
					first = middle + 1u;
				}
				else
				{
					last = middle;
				}
			}
			if (first >= range->m_itemOffsets->Num() ||
				(*range->m_itemOffsets)[first].m_stableKey != stableKey)
			{
				return false;
			}
			relativeIndex = (*range->m_itemOffsets)[first].m_relativeIndex;
			if (relativeIndex >= range->m_count)
			{
				return false;
			}
			outInstanceIndex = range->m_offset + relativeIndex;
			if (outMaterialVersion)
			{
				outMaterialVersion->Clear();
				if (range->m_materialVersionRuns)
				{
					for (const auto& run : *range->m_materialVersionRuns)
					{
						if (relativeIndex >= run.m_first &&
							relativeIndex - run.m_first < run.m_count)
						{
							*outMaterialVersion = run.m_version;
							break;
						}
					}
				}
			}
			return true;
		}
	};

	template<typename TPerInstanceData>
	using TPackedDrawPacketPayloadPtr = TSharedPtr<const TPackedDrawPacketPayload<TPerInstanceData>>;

	template<typename TPerInstanceData>
	struct TPackedDrawInstanceUpload
	{
		const TPerInstanceData* m_data = nullptr;
		uint32_t m_offset = 0u;
		uint32_t m_count = 0u;
	};

	/**
	 * Builds immutable, view-independent instance arenas as copy-on-write pages.
	 * A logical producer owns one stable range. Unchanged ranges retain both their
	 * data and metadata pages; the key index changes only on insertion/removal.
	 * Free ranges belong to the cache, not to an in-flight publication.
	 * Published payloads remain alive through the
	 * packets owned by submissions that are still in flight.
	 */
	template<typename TPerInstanceData>
	class TPackedDrawPagedArenaCache
	{
	public:
		using Payload = TPackedDrawPacketPayload<TPerInstanceData>;
		using PayloadPtr = TPackedDrawPacketPayloadPtr<TPerInstanceData>;
		static constexpr uint32_t PageSize =
			TPackedDrawArenaPage<TPerInstanceData>::NumInstances;

		PayloadPtr Find(size_t slotKey, size_t revision, uint64_t frame)
		{
			Entry* entry = nullptr;
			if (m_entries.Find(slotKey, entry) && entry && entry->m_payload &&
				entry->m_revision == revision)
			{
				entry->m_lastUsedFrame = frame;
				return entry->m_payload;
			}
			return {};
		}

		const RHIPackedDrawSceneState* GetSceneState(size_t slotKey) const
		{
			const Entry* entry = nullptr;
			return m_entries.Find(slotKey, entry) ? &entry->m_sceneState : nullptr;
		}

		void BeginUpdate(size_t slotKey, size_t revision, uint64_t frame,
			const RHIPackedDrawSceneState& sceneState = {})
		{
			m_buildingSlotKey = slotKey;
			m_buildingRevision = revision;
			m_buildingFrame = frame;
			m_buildingSceneState = sceneState;
			m_bHasUnmergedFreeRanges = false;
			m_writablePages.Clear(false);
			m_writableRangePages.Clear(false);
			m_writableRangeIndices.Clear();
			m_freeRanges.Clear(false);
			m_freeRangeSlots.Clear(false);

			m_buildingPayload = TSharedPtr<Payload>::Make();
			Entry* entry = nullptr;
			if (m_entries.Find(slotKey, entry) && entry && entry->m_payload &&
				entry->m_payload->IsPagedArena())
			{
				m_buildingPayload->m_arenaPages = entry->m_payload->m_arenaPages;
				m_buildingPayload->m_arenaRangeIndices = entry->m_payload->m_arenaRangeIndices;
				m_buildingPayload->m_arenaRangePages = entry->m_payload->m_arenaRangePages;
				m_buildingPayload->m_numArenaRangeSlots = entry->m_payload->m_numArenaRangeSlots;
				m_buildingPayload->m_arenaCapacity = entry->m_payload->m_arenaCapacity;
				m_freeRanges = entry->m_freeRanges;
				m_freeRangeSlots = entry->m_freeRangeSlots;
			}
			else
			{
				m_writableRangeIndices = TSharedPtr<PackedDrawArenaRangeIndices>::Make();
				m_buildingPayload->m_arenaRangeIndices = m_writableRangeIndices;
			}
			m_writablePages.Resize(m_buildingPayload->m_arenaPages.Num());
			std::fill(m_writablePages.begin(), m_writablePages.end(), nullptr);
			m_writableRangePages.Resize(m_buildingPayload->m_arenaRangePages.Num());
			std::fill(m_writableRangePages.begin(), m_writableRangePages.end(), nullptr);
		}

		bool TryReuseRange(uint64_t rangeKey, uint64_t contentRevision)
		{
			if (!m_buildingPayload)
			{
				return false;
			}

			const uint32_t* slot = nullptr;
			if (!m_buildingPayload->m_arenaRangeIndices->Find(rangeKey, slot) ||
				GetRange(*slot).m_contentRevision != contentRevision)
			{
				return false;
			}

			return true;
		}

		bool ReplaceRange(
			uint64_t rangeKey,
			uint64_t contentRevision,
			const TVector<TPerInstanceData>& instances,
			const TVector<uint64_t>& stableKeys,
			const TVector<PackedDrawArenaMaterialRun>* materialVersionRuns = nullptr)
		{
			if (!m_buildingPayload || instances.Num() != stableKeys.Num())
			{
				return false;
			}
			if (materialVersionRuns)
			{
				uint32_t coveredInstances = 0u;
				for (const auto& run : *materialVersionRuns)
				{
					if (run.m_count == 0u || run.m_first != coveredInstances ||
						run.m_count > instances.Num() - coveredInstances)
					{
						return false;
					}
					coveredInstances += run.m_count;
				}
				if (coveredInstances != instances.Num())
				{
					return false;
				}
			}
			auto itemOffsets = TSharedPtr<TVector<PackedDrawArenaItemOffset>>::Make();
			itemOffsets->Reserve(stableKeys.Num());
			for (uint32_t index = 0u; index < stableKeys.Num(); ++index)
			{
				itemOffsets->Add({ stableKeys[index], index });
			}
			itemOffsets->Sort([](const auto& lhs, const auto& rhs)
			{
				return lhs.m_stableKey < rhs.m_stableKey;
			});
			for (uint32_t index = 1u; index < itemOffsets->Num(); ++index)
			{
				if ((*itemOffsets)[index - 1u].m_stableKey ==
					(*itemOffsets)[index].m_stableKey)
				{
					return false;
				}
			}

			const uint32_t* existingSlot = nullptr;
			m_buildingPayload->m_arenaRangeIndices->Find(rangeKey, existingSlot);
			const auto* existingRange = existingSlot ? &GetRange(*existingSlot) : nullptr;
			const uint32_t count = static_cast<uint32_t>(instances.Num());
			const uint32_t requiredCapacity = AllocateCapacity(count);
			PackedDrawArenaRange range = existingRange ? *existingRange :
				PackedDrawArenaRange{};
			if (!existingRange || range.m_capacity < requiredCapacity)
			{
				if (existingRange && range.m_capacity > 0u)
				{
					m_freeRanges.Add({ range.m_offset, range.m_capacity });
					MergeFreeRanges();
				}
				range.m_offset = AllocateRange(requiredCapacity);
				range.m_capacity = requiredCapacity;
			}

			range.m_contentRevision = contentRevision;
			range.m_count = count;
			range.m_itemOffsets = std::move(itemOffsets);
			if (materialVersionRuns && !materialVersionRuns->IsEmpty())
			{
				auto runs = TSharedPtr<TVector<PackedDrawArenaMaterialRun>>::Make();
				*runs = *materialVersionRuns;
				range.m_materialVersionRuns = std::move(runs);
			}
			else
			{
				range.m_materialVersionRuns.Clear();
			}
			for (uint32_t index = 0u; index < count; ++index)
			{
				WriteInstance(range.m_offset + index, instances[index]);
			}

			uint32_t slot = 0u;
			if (existingSlot)
			{
				slot = *existingSlot;
			}
			else
			{
				if (!m_freeRangeSlots.IsEmpty())
				{
					slot = *m_freeRangeSlots.Last();
					m_freeRangeSlots.RemoveLast();
				}
				else
				{
					slot = m_buildingPayload->m_numArenaRangeSlots++;
				}
				MakeRangeIndicesWritable()[rangeKey] = slot;
			}
			MakeRangeWritable(slot) = std::move(range);
			return true;
		}

		void RemoveRange(uint64_t rangeKey)
		{
			const uint32_t* existingSlot = nullptr;
			if (!m_buildingPayload->m_arenaRangeIndices->Find(rangeKey, existingSlot)) return;
			const uint32_t slot = *existingSlot;
			const auto& range = GetRange(slot);
			if (range.m_capacity > 0u) m_freeRanges.Add({ range.m_offset, range.m_capacity });
			MakeRangeWritable(slot) = {};
			m_freeRangeSlots.Add(slot);
			MakeRangeIndicesWritable().Remove(rangeKey);
			m_bHasUnmergedFreeRanges = true;
		}

		PayloadPtr EndUpdate(bool bPublish = true)
		{
			if (!m_buildingPayload)
			{
				return {};
			}

			if (m_bHasUnmergedFreeRanges) MergeFreeRanges();

			PayloadPtr result = std::move(m_buildingPayload);
			if (bPublish)
			{
				m_entries[m_buildingSlotKey] = {
					m_buildingRevision,
					result,
					m_buildingFrame,
					std::move(m_freeRanges),
					std::move(m_freeRangeSlots),
					std::move(m_buildingSceneState) };
			}
			m_buildingSceneState = {};
			m_writablePages.Clear(false);
			m_writableRangePages.Clear(false);
			m_writableRangeIndices.Clear();
			m_freeRanges.Clear(false);
			m_freeRangeSlots.Clear(false);
			return result;
		}

		void Evict(uint64_t frame, uint64_t retentionFrames = 3ull)
		{
			m_expiredKeys.Clear(false);
			for (const auto& entry : m_entries)
			{
				if (entry.Second() && frame > entry.Second()->m_lastUsedFrame &&
					frame - entry.Second()->m_lastUsedFrame > retentionFrames)
				{
					m_expiredKeys.Add(entry.First());
				}
			}
			for (size_t key : m_expiredKeys)
			{
				m_entries.Remove(key);
			}
			m_expiredKeys.Clear(false);
		}

		void Clear()
		{
			m_entries.Clear();
			m_buildingPayload.Clear();
			m_buildingSceneState = {};
			m_writablePages.Clear(false);
			m_writableRangePages.Clear(false);
			m_writableRangeIndices.Clear();
			m_freeRanges.Clear();
			m_freeRangeSlots.Clear();
			m_expiredKeys.Clear();
		}

		size_t Num() const { return m_entries.Num(); }

	private:
		struct FreeRange
		{
			uint32_t m_offset = 0u;
			uint32_t m_capacity = 0u;
		};

		struct Entry
		{
			size_t m_revision = 0u;
			PayloadPtr m_payload{};
			uint64_t m_lastUsedFrame = 0ull;
			TVector<FreeRange> m_freeRanges{};
			TVector<uint32_t> m_freeRangeSlots{};
			RHIPackedDrawSceneState m_sceneState{};
		};

		const PackedDrawArenaRange& GetRange(uint32_t slot) const
		{
			return m_buildingPayload->m_arenaRangePages[slot / PackedDrawArenaRangePage::NumRanges]->
				m_ranges[slot % PackedDrawArenaRangePage::NumRanges];
		}

		PackedDrawArenaRangeIndices& MakeRangeIndicesWritable()
		{
			if (!m_writableRangeIndices)
			{
				m_writableRangeIndices = TSharedPtr<PackedDrawArenaRangeIndices>::Make(
					*m_buildingPayload->m_arenaRangeIndices);
				m_buildingPayload->m_arenaRangeIndices = m_writableRangeIndices;
			}
			return *m_writableRangeIndices;
		}

		PackedDrawArenaRange& MakeRangeWritable(uint32_t slot)
		{
			const uint32_t pageIndex = slot / PackedDrawArenaRangePage::NumRanges;
			if (pageIndex == m_buildingPayload->m_arenaRangePages.Num())
			{
				auto page = TSharedPtr<PackedDrawArenaRangePage>::Make();
				m_writableRangePages.Add(page.GetRawPtr());
				m_buildingPayload->m_arenaRangePages.Add(std::move(page));
			}
			else if (!m_writableRangePages[pageIndex])
			{
				auto page = TSharedPtr<PackedDrawArenaRangePage>::Make(
					*m_buildingPayload->m_arenaRangePages[pageIndex]);
				m_writableRangePages[pageIndex] = page.GetRawPtr();
				m_buildingPayload->m_arenaRangePages[pageIndex] = std::move(page);
			}
			return m_writableRangePages[pageIndex]->m_ranges[slot % PackedDrawArenaRangePage::NumRanges];
		}

		static uint32_t AllocateCapacity(uint32_t count)
		{
			if (count == 0u)
			{
				return 0u;
			}
			uint32_t result = 1u;
			while (result < count && result <= (std::numeric_limits<uint32_t>::max)() / 2u)
			{
				result *= 2u;
			}
			return (std::max)(result, count);
		}

		void MergeFreeRanges()
		{
			m_bHasUnmergedFreeRanges = false;
			if (m_freeRanges.Num() < 2u)
			{
				return;
			}
			m_freeRanges.Sort([](const FreeRange& lhs, const FreeRange& rhs)
			{
				return lhs.m_offset < rhs.m_offset;
			});
			uint32_t writeIndex = 0u;
			for (uint32_t readIndex = 1u; readIndex < m_freeRanges.Num(); ++readIndex)
			{
				auto& current = m_freeRanges[writeIndex];
				const auto& next = m_freeRanges[readIndex];
				const uint32_t currentEnd = current.m_offset + current.m_capacity;
				if (next.m_offset <= currentEnd)
				{
					current.m_capacity = (std::max)(currentEnd,
						next.m_offset + next.m_capacity) - current.m_offset;
				}
				else
				{
					++writeIndex;
					m_freeRanges[writeIndex] = next;
				}
			}
			m_freeRanges.Resize(writeIndex + 1u);
		}

		uint32_t AllocateRange(uint32_t capacity)
		{
			if (capacity == 0u)
			{
				return 0u;
			}
			if (m_bHasUnmergedFreeRanges) MergeFreeRanges();
			for (uint32_t index = 0u; index < m_freeRanges.Num(); ++index)
			{
				auto& freeRange = m_freeRanges[index];
				if (freeRange.m_capacity < capacity)
				{
					continue;
				}
				const uint32_t result = freeRange.m_offset;
				freeRange.m_offset += capacity;
				freeRange.m_capacity -= capacity;
				if (freeRange.m_capacity == 0u)
				{
					m_freeRanges.RemoveAt(index);
				}
				return result;
			}

			const uint32_t result = m_buildingPayload->m_arenaCapacity;
			m_buildingPayload->m_arenaCapacity += capacity;
			const uint32_t requiredPages =
				(m_buildingPayload->m_arenaCapacity + PageSize - 1u) / PageSize;
			const uint32_t previousPages =
				static_cast<uint32_t>(m_buildingPayload->m_arenaPages.Num());
			m_buildingPayload->m_arenaPages.Resize(requiredPages);
			m_writablePages.Resize(requiredPages);
			for (uint32_t pageIndex = previousPages; pageIndex < requiredPages; ++pageIndex)
			{
				m_buildingPayload->m_arenaPages[pageIndex] = GetZeroPage();
			}
			return result;
		}

		TPackedDrawArenaPagePtr<TPerInstanceData> GetZeroPage()
		{
			if (!m_zeroPage)
			{
				m_zeroPage = TPackedDrawArenaPagePtr<TPerInstanceData>::Make();
			}
			return m_zeroPage;
		}

		TPackedDrawArenaPage<TPerInstanceData>* MakePageWritable(uint32_t pageIndex)
		{
			if (pageIndex >= m_buildingPayload->m_arenaPages.Num())
			{
				return nullptr;
			}
			if (!m_writablePages[pageIndex])
			{
				const auto& source = m_buildingPayload->m_arenaPages[pageIndex];
				auto page = source ?
					TSharedPtr<TPackedDrawArenaPage<TPerInstanceData>>::Make(*source) :
					TSharedPtr<TPackedDrawArenaPage<TPerInstanceData>>::Make();
				m_writablePages[pageIndex] = page.GetRawPtr();
				m_buildingPayload->m_arenaPages[pageIndex] = std::move(page);
			}
			return m_writablePages[pageIndex];
		}

		void WriteInstance(uint32_t instanceIndex, const TPerInstanceData& instance)
		{
			const uint32_t pageIndex = instanceIndex / PageSize;
			const uint32_t pageOffset = instanceIndex % PageSize;
			if (auto* page = MakePageWritable(pageIndex))
			{
				page->m_instances[pageOffset] = instance;
			}
		}

		TMap<size_t, Entry> m_entries{};
		TSharedPtr<Payload> m_buildingPayload{};
		RHIPackedDrawSceneState m_buildingSceneState{};
		bool m_bHasUnmergedFreeRanges = false;
		TVector<TPackedDrawArenaPage<TPerInstanceData>*> m_writablePages{};
		TSharedPtr<PackedDrawArenaRangeIndices> m_writableRangeIndices{};
		TVector<PackedDrawArenaRangePage*> m_writableRangePages{};
		TVector<FreeRange> m_freeRanges{};
		TVector<uint32_t> m_freeRangeSlots{};
		TVector<size_t> m_expiredKeys{};
		TPackedDrawArenaPagePtr<TPerInstanceData> m_zeroPage{};
		size_t m_buildingSlotKey = 0u;
		size_t m_buildingRevision = 0u;
		uint64_t m_buildingFrame = 0ull;
	};

	template<typename TPerInstanceData>
	class TPackedDrawPacket
	{
	public:
		static constexpr size_t NumMobilitySegments = 3u;

		void Reset()
		{
			for (auto& segment : m_segments)
			{
				segment.m_items.Clear(false);
				segment.m_sortedItemIndices.Clear(false);
				segment.m_reorderVisited.Clear(false);
				segment.m_viewInstanceIndices.Clear(false);
				segment.m_groups.Clear(false);
				segment.m_sharedPayload.Clear();
				segment.m_localPayload.m_instances.Clear(false);
				segment.m_localPayload.m_groups.Clear(false);
			}
			m_groups.Clear(false);
			m_instanceIndices.Clear(false);
			m_metrics = {};
		}

		void InvalidateUploadedState()
		{
			m_uploadedStorageBinding.Clear();
			m_uploadedIndexBinding.Clear();
			m_uploadedIndirectBuffer.Clear();
			m_uploadedStationaryInstances.Clear(false);
			m_uploadedInstanceIndices.Clear(false);
			m_resolvedInstanceIndices.Clear(false);
			m_instanceUploads.Clear(false);
			m_uploadedFirstStorageInstance = 0u;
			m_uploadedFirstIndexInstance = 0u;
			for (size_t index = 0u; index < NumMobilitySegments; ++index)
			{
				m_uploadedSharedPayloads[index].Clear();
				m_uploadedSegmentOffsets[index] = 0u;
				m_uploadedSegmentCounts[index] = 0u;
			}
			m_metrics = {};
		}

		uint32_t GetNumStorageInstances() const
		{
			uint32_t result = 0u;
			for (size_t index = 0u; index < NumMobilitySegments; ++index)
			{
				result += GetPayload(index).GetNumStorageInstances();
			}
			return result;
		}

		uint32_t GetNumDrawInstances() const
		{
			return static_cast<uint32_t>(m_instanceIndices.Num());
		}

		uint32_t GetNumInstances() const
		{
			return GetNumDrawInstances();
		}

		const TVector<PackedDrawGroup>& GetGroups() const
		{
			return m_groups;
		}

		const TVector<uint32_t>& GetInstanceIndices() const
		{
			return m_instanceIndices;
		}

		const TPackedDrawPacketPayload<TPerInstanceData>& GetPayload(
			EMobilityType mobility) const
		{
			return GetPayload(ToSegmentIndex(mobility));
		}

		void UseSharedArenaPayload(
			EMobilityType mobility,
			TPackedDrawPacketPayloadPtr<TPerInstanceData> payload)
		{
			auto& segment = m_segments[ToSegmentIndex(mobility)];
			segment.m_items.Clear(false);
			segment.m_localPayload.m_instances.Clear(false);
			segment.m_localPayload.m_groups.Clear(false);
			segment.m_sharedPayload = std::move(payload);
			m_metrics.m_bSharedImmutablePayload = HasSharedImmutablePayload();
		}

		const TPackedDrawPacketPayloadPtr<TPerInstanceData>& GetSharedPayload(
			EMobilityType mobility) const
		{
			return m_segments[ToSegmentIndex(mobility)].m_sharedPayload;
		}

		bool HasSharedImmutablePayload() const
		{
			return m_segments[ToSegmentIndex(EMobilityType::Static)].m_sharedPayload.IsValid() ||
				m_segments[ToSegmentIndex(EMobilityType::Stationary)].m_sharedPayload.IsValid();
		}

		void Add(
			RHIBatch batch,
			RHIMeshPtr mesh,
			TPerInstanceData instanceData,
			uint64_t stableSortKey = 0ull,
			EMobilityType mobility = EMobilityType::Dynamic)
		{
			auto& segment = m_segments[ToSegmentIndex(mobility)];
			const uint32_t instanceIndex =
				static_cast<uint32_t>(segment.m_localPayload.m_instances.Num());
			segment.m_localPayload.m_instances.Emplace(std::move(instanceData));
			segment.m_items.Emplace(
				PackedDrawItem{
				std::move(batch),
				std::move(mesh),
				instanceIndex,
				stableSortKey });
		}

		bool AddArenaView(
			RHIBatch batch,
			RHIMeshPtr mesh,
			uint64_t rangeKey,
			uint64_t stableKey,
			EMobilityType mobility = EMobilityType::Static)
		{
			auto& segment = m_segments[ToSegmentIndex(mobility)];
			const auto& payload = GetPayload(mobility);
			uint32_t instanceIndex = 0u;
			RHIMaterialVersionPtr materialVersion;
			if (!payload.FindInstance(
				rangeKey,
				stableKey,
				instanceIndex,
				&materialVersion))
			{
				return false;
			}
			if (materialVersion)
			{
				batch.m_materialVersion = std::move(materialVersion);
			}

			segment.m_items.Emplace(
				PackedDrawItem{
				std::move(batch),
				std::move(mesh),
				instanceIndex,
				stableKey });
			return true;
		}

		void Finalize(bool bPreserveInstanceOrder = false)
		{
			SAILOR_PROFILE_FUNCTION();
			for (auto& segment : m_segments)
			{
				segment.m_viewInstanceIndices.Clear(false);
				segment.m_groups.Clear(false);
				auto& instances = segment.m_localPayload.m_instances;
				auto& groups = segment.m_sharedPayload ?
					segment.m_groups : segment.m_localPayload.m_groups;
				groups.Clear(false);

				if (!bPreserveInstanceOrder)
				{
					SortPackedDrawItems(segment.m_items, segment.m_sortedItemIndices);

					if (!segment.m_sharedPayload)
					{
						segment.m_reorderVisited.Resize(instances.Num());
						std::memset(
							segment.m_reorderVisited.GetData(),
							0,
							segment.m_reorderVisited.Num());
						for (uint32_t start = 0u; start < instances.Num(); ++start)
						{
							if (segment.m_reorderVisited[start])
							{
								continue;
							}

							uint32_t destination = start;
							TPerInstanceData displaced = std::move(instances[destination]);
							while (segment.m_items[segment.m_sortedItemIndices[destination]].m_instanceIndex != start)
							{
								const uint32_t source =
									segment.m_items[segment.m_sortedItemIndices[destination]].m_instanceIndex;
								instances[destination] = std::move(instances[source]);
								segment.m_reorderVisited[destination] = 1u;
								destination = source;
							}
							instances[destination] = std::move(displaced);
							segment.m_reorderVisited[destination] = 1u;
						}
					}
				}

				groups.Reserve(segment.m_items.Num());
				for (uint32_t itemIndex = 0u; itemIndex < segment.m_items.Num(); ++itemIndex)
				{
					const auto& item = segment.m_items[bPreserveInstanceOrder ?
						itemIndex : segment.m_sortedItemIndices[itemIndex]];
					const bool bAppendToGroup = !bPreserveInstanceOrder &&
						!groups.IsEmpty() &&
						groups.Last()->m_numInstances < RHIBatch::MaxInstancesPerBatch &&
						groups.Last()->m_mesh == item.m_mesh &&
						groups.Last()->m_batch == item.m_batch;
					if (!bAppendToGroup)
					{
						PackedDrawGroup group;
						group.m_batch = item.m_batch;
						group.m_mesh = item.m_mesh;
						group.m_firstInstance = itemIndex;
						groups.Emplace(std::move(group));
					}

					++groups.Last()->m_numInstances;
					if (segment.m_sharedPayload)
					{
						segment.m_viewInstanceIndices.Add(item.m_instanceIndex);
					}
				}

				// The packed arrays replace the duplicate per-item payload. Retain the
				// builder capacity for the next use of this flight slot.
				segment.m_items.Clear(false);
			}

			RebuildCombinedGroups();
			m_metrics.m_bSharedImmutablePayload = HasSharedImmutablePayload();
		}

		const TPackedDrawPacketPayload<TPerInstanceData>& GetPayload(size_t index) const
		{
			const auto& segment = m_segments[index];
			return segment.m_sharedPayload ? *segment.m_sharedPayload : segment.m_localPayload;
		}

		static size_t ToSegmentIndex(EMobilityType mobility)
		{
			const size_t result = static_cast<size_t>(mobility);
			return result < NumMobilitySegments ? result :
				static_cast<size_t>(EMobilityType::Dynamic);
		}

		void RebuildCombinedGroups()
		{
			m_groups.Clear(false);
			m_instanceIndices.Clear(false);
			uint32_t firstStorageInstance = 0u;
			uint32_t firstViewInstance = 0u;
			for (size_t index = 0u; index < NumMobilitySegments; ++index)
			{
				auto& segment = m_segments[index];
				const auto& payload = GetPayload(index);
				const auto& sourceGroups = segment.m_sharedPayload ?
					segment.m_groups : payload.m_groups;
				m_groups.Reserve(m_groups.Num() + sourceGroups.Num());
				for (const auto& sourceGroup : sourceGroups)
				{
					auto group = sourceGroup;
					group.m_firstInstance += firstViewInstance;
					m_groups.Emplace(std::move(group));
				}

				if (segment.m_sharedPayload)
				{
					m_instanceIndices.Reserve(
						m_instanceIndices.Num() + segment.m_viewInstanceIndices.Num());
					for (uint32_t instanceIndex : segment.m_viewInstanceIndices)
					{
						m_instanceIndices.Add(firstStorageInstance + instanceIndex);
					}
					firstViewInstance +=
						static_cast<uint32_t>(segment.m_viewInstanceIndices.Num());
				}
				else
				{
					const uint32_t numInstances = payload.GetNumStorageInstances();
					m_instanceIndices.Reserve(m_instanceIndices.Num() + numInstances);
					for (uint32_t instanceIndex = 0u; instanceIndex < numInstances; ++instanceIndex)
					{
						m_instanceIndices.Add(firstStorageInstance + instanceIndex);
					}
					firstViewInstance += numInstances;
				}
				firstStorageInstance += payload.GetNumStorageInstances();
			}
		}

		struct Segment
		{
			TVector<PackedDrawItem> m_items{};
			TVector<uint32_t> m_sortedItemIndices{};
			TVector<uint8_t> m_reorderVisited{};
			TVector<uint32_t> m_viewInstanceIndices{};
			TVector<PackedDrawGroup> m_groups{};
			TPackedDrawPacketPayload<TPerInstanceData> m_localPayload{};
			TPackedDrawPacketPayloadPtr<TPerInstanceData> m_sharedPayload{};
		};

		std::array<Segment, NumMobilitySegments> m_segments{};
		TVector<PackedDrawGroup> m_groups{};
		TVector<uint32_t> m_instanceIndices{};
		TVector<uint32_t> m_resolvedInstanceIndices{};
		TVector<uint32_t> m_uploadedInstanceIndices{};
		TVector<DrawIndexedIndirectData> m_indirectCommands{};
		TVector<TPackedDrawInstanceUpload<TPerInstanceData>> m_instanceUploads{};
		TVector<RHIShaderBindingSetPtr> m_drawBindingSets{};
		TVector<TPerInstanceData> m_uploadedStationaryInstances{};
		TVector<DrawIndexedIndirectData> m_uploadedIndirectCommands{};
		RHIShaderBindingPtr m_uploadedStorageBinding{};
		RHIShaderBindingPtr m_uploadedIndexBinding{};
		RHIBufferPtr m_uploadedIndirectBuffer{};
		uint32_t m_uploadedFirstStorageInstance = 0u;
		uint32_t m_uploadedFirstIndexInstance = 0u;
		std::array<TPackedDrawPacketPayloadPtr<TPerInstanceData>, NumMobilitySegments>
			m_uploadedSharedPayloads{};
		std::array<uint32_t, NumMobilitySegments> m_uploadedSegmentOffsets{};
		std::array<uint32_t, NumMobilitySegments> m_uploadedSegmentCounts{};
		PackedDrawPacketMetrics m_metrics{};
	};
}
