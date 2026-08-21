#pragma once
#include "Core/SpinLock.h"
#include "Containers/Map.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

using namespace GraphicsDriver::Vulkan;

namespace Sailor::RHI
{	
	class RHIBatch
	{
	public:

		RHIMaterialPtr m_material;
		RHIMaterialVersionPtr m_materialVersion;

		// Here we store the vertex and index bindings that could be shared during rendering (not meshes)
		RHIMeshPtr m_mesh;
		RHIShaderBindingSetPtr m_textureBindings;
		uint32_t m_supportedMeshesPerBatch = std::numeric_limits<uint32_t>::max();

		RHIBatch() = default;
		RHIBatch(const RHIMaterialPtr& material, const RHIMeshPtr& mesh) :
			m_material(material),
			m_materialVersion(material ? material->GetVersion() : RHIMaterialVersionPtr{}),
			m_mesh(mesh)
		{}
		RHIBatch(
			const RHIMaterialPtr& material,
			const RHIMeshPtr& mesh,
			uint64_t submissionId) :
			m_material(material),
			m_materialVersion(material ?
				material->GetVersionForSubmission(submissionId) : RHIMaterialVersionPtr{}),
			m_mesh(mesh)
		{}

		RHIShaderBindingSetPtr GetMaterialBindings() const
		{
			return m_materialVersion ? m_materialVersion->GetBindings() : RHIShaderBindingSetPtr{};
		}

		bool operator==(const RHIBatch& rhs) const
		{
			const auto bindings = GetMaterialBindings();
			const auto rhsBindings = rhs.GetMaterialBindings();
			if (!m_material || !rhs.m_material || !m_mesh || !rhs.m_mesh ||
				!m_mesh->m_vertexBuffer || !rhs.m_mesh->m_vertexBuffer ||
				!m_mesh->m_indexBuffer || !rhs.m_mesh->m_indexBuffer ||
				!bindings || !rhsBindings)
			{
				return m_material == rhs.m_material &&
					m_materialVersion == rhs.m_materialVersion &&
					m_mesh == rhs.m_mesh &&
					m_textureBindings == rhs.m_textureBindings;
			}

			const bool bSameBatch =
				m_materialVersion == rhs.m_materialVersion &&
				bindings->GetCompatibilityHashCode() == rhsBindings->GetCompatibilityHashCode() &&
				m_material->GetVertexShader() == rhs.m_material->GetVertexShader() &&
				m_material->GetFragmentShader() == rhs.m_material->GetFragmentShader() &&
				m_material->GetRenderState() == rhs.m_material->GetRenderState() &&
				m_mesh->m_vertexBuffer->GetCompatibilityHashCode() == rhs.m_mesh->m_vertexBuffer->GetCompatibilityHashCode() &&
				m_mesh->m_indexBuffer->GetCompatibilityHashCode() == rhs.m_mesh->m_indexBuffer->GetCompatibilityHashCode() &&
				m_textureBindings == rhs.m_textureBindings;

			return bSameBatch;
		}

		size_t GetHash() const
		{
			const auto bindings = GetMaterialBindings();
			size_t hash = bindings ? bindings->GetCompatibilityHashCode() : 0u;

			HashCombine(hash, m_materialVersion);
			if (m_material)
			{
				HashCombine(hash, Sailor::GetHash(m_material->GetRenderState()));
			}
			if (m_mesh && m_mesh->m_vertexBuffer && m_mesh->m_indexBuffer)
			{
				HashCombine(
					hash,
					m_mesh->m_vertexBuffer->GetCompatibilityHashCode(),
					m_mesh->m_indexBuffer->GetCompatibilityHashCode());
			}
			else
			{
				HashCombine(hash, m_mesh);
			}
			HashCombine(hash, m_textureBindings);
			return hash;
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

	template<typename TPerInstanceData>
	struct TPackedDrawItem
	{
		RHIBatch m_batch{};
		RHIMeshPtr m_mesh{};
		uint32_t m_instanceIndex = 0u;
		uint64_t m_stableSortKey = 0ull;
	};

	struct PackedDrawGroup
	{
		RHIBatch m_batch{};
		RHIMeshPtr m_mesh{};
		uint32_t m_firstInstance = 0u;
		uint32_t m_numInstances = 0u;
	};

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
	using TPackedDrawArenaPagePtr = TSharedPtr<TPackedDrawArenaPage<TPerInstanceData>>;

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
		uint64_t m_lastVisitSerial = 0ull;
		TSharedPtr<TVector<PackedDrawArenaItemOffset>> m_itemOffsets{};
		TSharedPtr<TVector<PackedDrawArenaMaterialRun>> m_materialVersionRuns{};
	};

	template<typename TPerInstanceData>
	struct TPackedDrawPacketPayload
	{
		TVector<TPerInstanceData> m_instances{};
		TVector<PackedDrawGroup> m_groups{};
		TVector<uint64_t> m_stableKeys{};
		TMap<uint64_t, uint32_t> m_instanceLookup{};
		TVector<TPackedDrawArenaPagePtr<TPerInstanceData>> m_arenaPages{};
		TMap<uint64_t, PackedDrawArenaRange> m_arenaRanges{};
		uint32_t m_arenaCapacity = 0u;

		bool IsPagedArena() const
		{
			return m_arenaCapacity > 0u || !m_arenaRanges.IsEmpty();
		}

		uint32_t GetNumStorageInstances() const
		{
			return IsPagedArena() ? m_arenaCapacity :
				static_cast<uint32_t>(m_instances.Num());
		}

		void RebuildInstanceLookup()
		{
			m_instanceLookup.Clear();
			for (uint32_t index = 0u; index < m_stableKeys.Num(); ++index)
			{
				m_instanceLookup[m_stableKeys[index]] = index;
			}
		}

		bool FindInstance(uint64_t stableKey, uint32_t& outInstanceIndex) const
		{
			const uint32_t* instanceIndex = nullptr;
			if (!m_instanceLookup.Find(stableKey, instanceIndex) || !instanceIndex)
			{
				return false;
			}
			outInstanceIndex = *instanceIndex;
			return true;
		}

		bool FindInstance(
			uint64_t rangeKey,
			uint64_t stableKey,
			uint32_t& outInstanceIndex,
			RHIMaterialVersionPtr* outMaterialVersion = nullptr) const
		{
			if (!IsPagedArena())
			{
				if (outMaterialVersion)
				{
					outMaterialVersion->Clear();
				}
				return FindInstance(stableKey, outInstanceIndex);
			}

			const PackedDrawArenaRange* range = nullptr;
			if (!m_arenaRanges.Find(rangeKey, range) || !range ||
				!range->m_itemOffsets)
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
	using TPackedDrawPacketPayloadPtr = TSharedPtr<TPackedDrawPacketPayload<TPerInstanceData>>;

	template<typename TPerInstanceData>
	struct TPackedDrawInstanceUpload
	{
		const TPerInstanceData* m_data = nullptr;
		uint32_t m_offset = 0u;
		uint32_t m_count = 0u;
	};

	template<typename TPerInstanceData>
	class TPackedDrawPacketPayloadCache
	{
	public:
		TPackedDrawPacketPayloadPtr<TPerInstanceData> Find(
			size_t slotKey,
			size_t revision,
			uint64_t frame)
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

		void Publish(
			size_t slotKey,
			size_t revision,
			TPackedDrawPacketPayloadPtr<TPerInstanceData> payload,
			uint64_t frame)
		{
			// Replacing a logical slot releases the cache reference to the old
			// revision immediately. Active submissions retain their own shared
			// reference until the corresponding completion fence is signalled.
			m_entries[slotKey] = { revision, std::move(payload), frame };
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
			m_expiredKeys.Clear();
		}

		size_t Num() const { return m_entries.Num(); }

	private:
		struct Entry
		{
			size_t m_revision = 0u;
			TPackedDrawPacketPayloadPtr<TPerInstanceData> m_payload{};
			uint64_t m_lastUsedFrame = 0ull;
		};

		TMap<size_t, Entry> m_entries{};
		TVector<size_t> m_expiredKeys{};
	};

	/**
	 * Builds immutable, view-independent instance arenas as copy-on-write pages.
	 * A logical producer owns one stable range. Unchanged ranges retain both their
	 * page pointers and stable-key lookup; changing one producer clones only the
	 * pages overlapped by that range. Published payloads remain alive through the
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

		void BeginUpdate(size_t slotKey, size_t revision, uint64_t frame)
		{
			m_buildingSlotKey = slotKey;
			m_buildingRevision = revision;
			m_buildingFrame = frame;
			if (++m_updateSerial == 0ull)
			{
				m_updateSerial = 1ull;
			}
			m_dirtyPages.Clear(false);
			m_freeRanges.Clear(false);
			m_removedRangeKeys.Clear(false);

			m_buildingPayload = PayloadPtr::Make();
			Entry* entry = nullptr;
			if (m_entries.Find(slotKey, entry) && entry && entry->m_payload &&
				entry->m_payload->IsPagedArena())
			{
				m_buildingPayload->m_arenaPages = entry->m_payload->m_arenaPages;
				m_buildingPayload->m_arenaRanges = entry->m_payload->m_arenaRanges;
				m_buildingPayload->m_arenaCapacity = entry->m_payload->m_arenaCapacity;
			}
			m_dirtyPages.Resize(m_buildingPayload->m_arenaPages.Num());
			if (!m_dirtyPages.IsEmpty())
			{
				std::memset(m_dirtyPages.GetData(), 0, m_dirtyPages.Num());
			}

			BuildFreeRanges();
		}

		bool TryReuseRange(uint64_t rangeKey, uint64_t contentRevision)
		{
			if (!m_buildingPayload)
			{
				return false;
			}

			PackedDrawArenaRange* range = nullptr;
			if (!m_buildingPayload->m_arenaRanges.Find(rangeKey, range) || !range ||
				range->m_contentRevision != contentRevision)
			{
				return false;
			}

			range->m_lastVisitSerial = m_updateSerial;
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

			PackedDrawArenaRange* existingRange = nullptr;
			m_buildingPayload->m_arenaRanges.Find(rangeKey, existingRange);
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
			range.m_lastVisitSerial = m_updateSerial;
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

			m_buildingPayload->m_arenaRanges[rangeKey] = std::move(range);
			return true;
		}

		PayloadPtr EndUpdate(bool bPublish = true)
		{
			if (!m_buildingPayload)
			{
				return {};
			}

			m_removedRangeKeys.Clear(false);
			for (const auto& entry : m_buildingPayload->m_arenaRanges)
			{
				if (!entry.Second() ||
					entry.Second()->m_lastVisitSerial != m_updateSerial)
				{
					m_removedRangeKeys.Add(entry.First());
				}
			}
			for (uint64_t key : m_removedRangeKeys)
			{
				m_buildingPayload->m_arenaRanges.Remove(key);
			}

			auto result = m_buildingPayload;
			if (bPublish)
			{
				m_entries[m_buildingSlotKey] = {
					m_buildingRevision,
					result,
					m_buildingFrame };
			}
			m_buildingPayload.Clear();
			m_dirtyPages.Clear(false);
			m_freeRanges.Clear(false);
			m_removedRangeKeys.Clear(false);
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
			m_dirtyPages.Clear(false);
			m_freeRanges.Clear();
			m_occupiedRangesScratch.Clear();
			m_removedRangeKeys.Clear();
			m_expiredKeys.Clear();
		}

		size_t Num() const { return m_entries.Num(); }

	private:
		struct Entry
		{
			size_t m_revision = 0u;
			PayloadPtr m_payload{};
			uint64_t m_lastUsedFrame = 0ull;
		};

		struct FreeRange
		{
			uint32_t m_offset = 0u;
			uint32_t m_capacity = 0u;
		};

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

		void BuildFreeRanges()
		{
			if (!m_buildingPayload || m_buildingPayload->m_arenaCapacity == 0u)
			{
				return;
			}

			auto& occupied = m_occupiedRangesScratch;
			occupied.Clear(false);
			occupied.Reserve(m_buildingPayload->m_arenaRanges.Num());
			for (const auto& entry : m_buildingPayload->m_arenaRanges)
			{
				if (entry.Second() && entry.Second()->m_capacity > 0u)
				{
					occupied.Add({ entry.Second()->m_offset, entry.Second()->m_capacity });
				}
			}
			occupied.Sort([](const FreeRange& lhs, const FreeRange& rhs)
			{
				return lhs.m_offset < rhs.m_offset;
			});

			uint32_t cursor = 0u;
			for (const auto& range : occupied)
			{
				if (range.m_offset > cursor)
				{
					m_freeRanges.Add({ cursor, range.m_offset - cursor });
				}
				cursor = (std::max)(cursor, range.m_offset + range.m_capacity);
			}
			if (cursor < m_buildingPayload->m_arenaCapacity)
			{
				m_freeRanges.Add({
					cursor,
					m_buildingPayload->m_arenaCapacity - cursor });
			}
			occupied.Clear(false);
		}

		void MergeFreeRanges()
		{
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
			m_dirtyPages.Resize(requiredPages);
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
			if (!m_dirtyPages[pageIndex])
			{
				const auto& source = m_buildingPayload->m_arenaPages[pageIndex];
				m_buildingPayload->m_arenaPages[pageIndex] = source ?
					TPackedDrawArenaPagePtr<TPerInstanceData>::Make(*source) :
					TPackedDrawArenaPagePtr<TPerInstanceData>::Make();
				m_dirtyPages[pageIndex] = 1u;
			}
			return m_buildingPayload->m_arenaPages[pageIndex].GetRawPtr();
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
		PayloadPtr m_buildingPayload{};
		TVector<uint8_t> m_dirtyPages{};
		TVector<FreeRange> m_freeRanges{};
		TVector<FreeRange> m_occupiedRangesScratch{};
		TVector<uint64_t> m_removedRangeKeys{};
		TVector<size_t> m_expiredKeys{};
		TPackedDrawArenaPagePtr<TPerInstanceData> m_zeroPage{};
		size_t m_buildingSlotKey = 0u;
		size_t m_buildingRevision = 0u;
		uint64_t m_buildingFrame = 0ull;
		uint64_t m_updateSerial = 0ull;
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
				segment.m_reorderVisited.Clear(false);
				segment.m_viewInstanceIndices.Clear(false);
				segment.m_groups.Clear(false);
				segment.m_sharedPayload.Clear();
				segment.m_localPayload.m_instances.Clear(false);
				segment.m_localPayload.m_groups.Clear(false);
				segment.m_localPayload.m_stableKeys.Clear(false);
				if (!segment.m_localPayload.m_instanceLookup.IsEmpty())
				{
					segment.m_localPayload.m_instanceLookup.Clear();
				}
				segment.m_localPayload.m_arenaPages.Clear(false);
				if (!segment.m_localPayload.m_arenaRanges.IsEmpty())
				{
					segment.m_localPayload.m_arenaRanges.Clear();
				}
				segment.m_localPayload.m_arenaCapacity = 0u;
				segment.m_bUsesArenaPayload = false;
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

		TPackedDrawPacketPayloadPtr<TPerInstanceData> SharePayload(
			EMobilityType mobility)
		{
			auto& segment = m_segments[ToSegmentIndex(mobility)];
			if (!segment.m_sharedPayload)
			{
				segment.m_sharedPayload = TPackedDrawPacketPayloadPtr<TPerInstanceData>::Make();
				segment.m_sharedPayload->m_instances = std::move(segment.m_localPayload.m_instances);
				segment.m_sharedPayload->m_groups = std::move(segment.m_localPayload.m_groups);
			}
			m_metrics.m_bSharedImmutablePayload = HasSharedImmutablePayload();
			return segment.m_sharedPayload;
		}

		TPackedDrawPacketPayloadPtr<TPerInstanceData> ShareArenaPayload(
			EMobilityType mobility)
		{
			auto& segment = m_segments[ToSegmentIndex(mobility)];
			if (!segment.m_sharedPayload)
			{
				segment.m_sharedPayload = TPackedDrawPacketPayloadPtr<TPerInstanceData>::Make();
				segment.m_sharedPayload->m_instances =
					std::move(segment.m_localPayload.m_instances);
				segment.m_sharedPayload->m_stableKeys =
					std::move(segment.m_localPayload.m_stableKeys);
				segment.m_sharedPayload->RebuildInstanceLookup();
			}
			segment.m_bUsesArenaPayload = true;
			m_metrics.m_bSharedImmutablePayload = HasSharedImmutablePayload();
			return segment.m_sharedPayload;
		}

		void UseSharedPayload(
			EMobilityType mobility,
			TPackedDrawPacketPayloadPtr<TPerInstanceData> payload)
		{
			auto& segment = m_segments[ToSegmentIndex(mobility)];
			segment.m_items.Clear(false);
			segment.m_localPayload.m_instances.Clear(false);
			segment.m_localPayload.m_groups.Clear(false);
			segment.m_sharedPayload = std::move(payload);
			segment.m_bUsesArenaPayload = false;
			m_metrics.m_bSharedImmutablePayload = HasSharedImmutablePayload();
		}

		void UseSharedArenaPayload(
			EMobilityType mobility,
			TPackedDrawPacketPayloadPtr<TPerInstanceData> payload)
		{
			auto& segment = m_segments[ToSegmentIndex(mobility)];
			segment.m_items.Clear(false);
			segment.m_localPayload.m_instances.Clear(false);
			segment.m_localPayload.m_groups.Clear(false);
			segment.m_localPayload.m_stableKeys.Clear(false);
			if (!segment.m_localPayload.m_instanceLookup.IsEmpty())
			{
				segment.m_localPayload.m_instanceLookup.Clear();
			}
			segment.m_localPayload.m_arenaPages.Clear(false);
			if (!segment.m_localPayload.m_arenaRanges.IsEmpty())
			{
				segment.m_localPayload.m_arenaRanges.Clear();
			}
			segment.m_localPayload.m_arenaCapacity = 0u;
			segment.m_sharedPayload = std::move(payload);
			segment.m_bUsesArenaPayload = true;
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
				TPackedDrawItem<TPerInstanceData>{
				std::move(batch),
				std::move(mesh),
				instanceIndex,
				stableSortKey });
		}

		bool AddArenaInstance(
			TPerInstanceData instanceData,
			uint64_t stableKey,
			EMobilityType mobility = EMobilityType::Static)
		{
			auto& segment = m_segments[ToSegmentIndex(mobility)];
			if (segment.m_sharedPayload)
			{
				return false;
			}
			uint32_t existingIndex = 0u;
			if (segment.m_localPayload.FindInstance(stableKey, existingIndex))
			{
				return false;
			}

			const uint32_t instanceIndex =
				static_cast<uint32_t>(segment.m_localPayload.m_instances.Num());
			segment.m_localPayload.m_instances.Emplace(std::move(instanceData));
			segment.m_localPayload.m_stableKeys.Add(stableKey);
			segment.m_localPayload.m_instanceLookup[stableKey] = instanceIndex;
			segment.m_bUsesArenaPayload = true;
			return true;
		}

		bool AddArenaView(
			RHIBatch batch,
			RHIMeshPtr mesh,
			uint64_t stableKey,
			EMobilityType mobility = EMobilityType::Static)
		{
			auto& segment = m_segments[ToSegmentIndex(mobility)];
			const auto& payload = GetPayload(mobility);
			uint32_t instanceIndex = 0u;
			if (!payload.FindInstance(stableKey, instanceIndex))
			{
				return false;
			}

			segment.m_items.Emplace(
				TPackedDrawItem<TPerInstanceData>{
				std::move(batch),
				std::move(mesh),
				instanceIndex,
				stableKey });
			segment.m_bUsesArenaPayload = true;
			return true;
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
				TPackedDrawItem<TPerInstanceData>{
				std::move(batch),
				std::move(mesh),
				instanceIndex,
				stableKey });
			segment.m_bUsesArenaPayload = true;
			return true;
		}

		void Finalize(bool bPreserveInstanceOrder = false)
		{
			for (auto& segment : m_segments)
			{
				segment.m_viewInstanceIndices.Clear(false);
				segment.m_groups.Clear(false);
				if (segment.m_sharedPayload && !segment.m_bUsesArenaPayload)
				{
					segment.m_items.Clear(false);
					continue;
				}

				auto& instances = segment.m_localPayload.m_instances;
				auto& groups = segment.m_bUsesArenaPayload ?
					segment.m_groups : segment.m_localPayload.m_groups;
				groups.Clear(false);

				if (!bPreserveInstanceOrder)
				{
					segment.m_items.Sort([](const auto& lhs, const auto& rhs)
					{
						const size_t lhsBatchHash = lhs.m_batch.GetHash();
						const size_t rhsBatchHash = rhs.m_batch.GetHash();
						if (lhsBatchHash != rhsBatchHash)
						{
							return lhsBatchHash < rhsBatchHash;
						}
						const auto lhsMaterialVersion = reinterpret_cast<uintptr_t>(
							lhs.m_batch.m_materialVersion.GetRawPtr());
						const auto rhsMaterialVersion = reinterpret_cast<uintptr_t>(
							rhs.m_batch.m_materialVersion.GetRawPtr());
						if (lhsMaterialVersion != rhsMaterialVersion)
						{
							return lhsMaterialVersion < rhsMaterialVersion;
						}
						const auto lhsMesh = reinterpret_cast<uintptr_t>(lhs.m_mesh.GetRawPtr());
						const auto rhsMesh = reinterpret_cast<uintptr_t>(rhs.m_mesh.GetRawPtr());
						if (lhsMesh != rhsMesh)
						{
							return lhsMesh < rhsMesh;
						}
						const auto lhsTextures = reinterpret_cast<uintptr_t>(
							lhs.m_batch.m_textureBindings.GetRawPtr());
						const auto rhsTextures = reinterpret_cast<uintptr_t>(
							rhs.m_batch.m_textureBindings.GetRawPtr());
						if (lhsTextures != rhsTextures)
						{
							return lhsTextures < rhsTextures;
						}
						return lhs.m_stableSortKey < rhs.m_stableSortKey;
					});

					if (!segment.m_bUsesArenaPayload)
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
							while (segment.m_items[destination].m_instanceIndex != start)
							{
								const uint32_t source =
									segment.m_items[destination].m_instanceIndex;
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
					const auto& item = segment.m_items[itemIndex];
					const bool bAppendToGroup = !bPreserveInstanceOrder &&
						!groups.IsEmpty() &&
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
					if (segment.m_bUsesArenaPayload)
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
				const auto& sourceGroups = segment.m_bUsesArenaPayload ?
					segment.m_groups : payload.m_groups;
				m_groups.Reserve(m_groups.Num() + sourceGroups.Num());
				for (const auto& sourceGroup : sourceGroups)
				{
					auto group = sourceGroup;
					group.m_firstInstance += firstViewInstance;
					m_groups.Emplace(std::move(group));
				}

				if (segment.m_bUsesArenaPayload)
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
			TVector<TPackedDrawItem<TPerInstanceData>> m_items{};
			TVector<uint8_t> m_reorderVisited{};
			TVector<uint32_t> m_viewInstanceIndices{};
			TVector<PackedDrawGroup> m_groups{};
			TPackedDrawPacketPayload<TPerInstanceData> m_localPayload{};
			TPackedDrawPacketPayloadPtr<TPerInstanceData> m_sharedPayload{};
			bool m_bUsesArenaPayload = false;
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

	template<typename TPerInstanceData, typename TShaderBindingsCallback, typename TBeforeDrawCallback>
	DrawCallStats RHIRecordPackedDrawPacketImpl(
		TPackedDrawPacket<TPerInstanceData>& packet,
		RHICommandListPtr graphicsCmdList,
		RHICommandListPtr transferCmdList,
		TShaderBindingsCallback&& collectShaderBindings,
		RHIShaderBindingSetPtr instanceBindings,
		RHIBufferPtr& indirectCommandBuffer,
		glm::ivec4 viewport,
		glm::uvec4 scissors,
		glm::vec2 depthRange,
		RHIShaderPtr computeCullingShader,
		RHIShaderBindingSetPtr* indirectCommandBufferBinding,
		const TVector<RHIShaderBindingSetPtr>& cullingDispatchBindings,
		RHICommandListPtr cullingCommandList,
		bool bEnableOcclusion,
		TBeforeDrawCallback&& beforeDraw)
	{
		SAILOR_PROFILE_FUNCTION();
		DrawCallStats stats;
		const uint32_t numInstances = packet.GetNumDrawInstances();
		const uint32_t numStorageInstances = packet.GetNumStorageInstances();
		const auto& groups = packet.GetGroups();
		if (numInstances == 0u || numStorageInstances == 0u ||
			groups.IsEmpty() || !instanceBindings)
		{
			return stats;
		}

		auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
		auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
		if (!cullingCommandList)
		{
			cullingCommandList = transferCmdList;
		}
		auto storageBinding = instanceBindings->GetOrAddShaderBinding("data");
		auto indexBinding = instanceBindings->GetOrAddShaderBinding("indices");
		const uint32_t firstStorageInstance = storageBinding->GetStorageInstanceIndex();
		const uint32_t firstCandidateInstance = indexBinding->GetStorageInstanceIndex();
		// GPU culling compacts into the second half of the same flight-local SSBO.
		// The first half remains immutable until the logical view changes, so the
		// shader can restore the compacted stream without another CPU upload.
		const uint32_t firstIndexInstance = computeCullingShader ?
			firstCandidateInstance + numInstances : firstCandidateInstance;

		const bool bStorageAllocationChanged =
			packet.m_uploadedStorageBinding != storageBinding ||
			packet.m_uploadedFirstStorageInstance != firstStorageInstance;
		auto& instanceUploads = packet.m_instanceUploads;
		instanceUploads.Clear(false);
		uint32_t segmentOffset = 0u;
		for (size_t index = 0u;
			index < TPackedDrawPacket<TPerInstanceData>::NumMobilitySegments;
			++index)
		{
			const auto mobility = static_cast<EMobilityType>(index);
			const auto& payload = packet.GetPayload(mobility);
			const auto& sharedPayload = packet.GetSharedPayload(mobility);
			const uint32_t segmentCount = payload.GetNumStorageInstances();
			const bool bSegmentLayoutChanged =
				packet.m_uploadedSegmentOffsets[index] != segmentOffset ||
				packet.m_uploadedSegmentCounts[index] != segmentCount;
			const bool bSharedPayloadChanged =
				packet.m_uploadedSharedPayloads[index] != sharedPayload;
			if (payload.IsPagedArena())
			{
				if (mobility == EMobilityType::Stationary)
				{
					// The contiguous fallback owns this comparison mirror. Invalidate it
					// while a paged version is active so a later mode switch cannot compare
					// against data older than the GPU's current arena pages.
					packet.m_uploadedStationaryInstances.Clear(false);
				}
				using ArenaPage = TPackedDrawArenaPage<TPerInstanceData>;
				const auto& previousPayload = packet.m_uploadedSharedPayloads[index];
				const bool bCanDiffPages = !bStorageAllocationChanged &&
					!bSegmentLayoutChanged && sharedPayload && previousPayload &&
					previousPayload->IsPagedArena() &&
					previousPayload->m_arenaCapacity == payload.m_arenaCapacity &&
					previousPayload->m_arenaPages.Num() == payload.m_arenaPages.Num();
				static const ArenaPage ZeroPage{};
				auto appendPage = [&](uint32_t pageIndex)
					{
						const uint32_t pageOffset = pageIndex * ArenaPage::NumInstances;
						if (pageOffset >= segmentCount)
						{
							return;
						}
						const uint32_t pageCount = (std::min)(
							ArenaPage::NumInstances,
							segmentCount - pageOffset);
						const auto& page = payload.m_arenaPages[pageIndex];
						instanceUploads.Add({
							page ? page->m_instances.data() : ZeroPage.m_instances.data(),
							segmentOffset + pageOffset,
							pageCount });
					};

				if (!bCanDiffPages)
				{
					if (segmentCount > 0u &&
						(bStorageAllocationChanged || bSegmentLayoutChanged ||
							!sharedPayload || bSharedPayloadChanged))
					{
						for (uint32_t pageIndex = 0u;
							pageIndex < payload.m_arenaPages.Num(); ++pageIndex)
						{
							appendPage(pageIndex);
						}
					}
				}
				else if (bSharedPayloadChanged)
				{
					for (uint32_t pageIndex = 0u;
						pageIndex < payload.m_arenaPages.Num(); ++pageIndex)
					{
						if (previousPayload->m_arenaPages[pageIndex] !=
							payload.m_arenaPages[pageIndex])
						{
							appendPage(pageIndex);
						}
					}
				}
			}
			else if (mobility == EMobilityType::Static)
			{
				// Static payload pointer identity is the immutable version. A freed
				// flight can patch a same-layout successor directly from the two
				// retained versions, without keeping another per-flight CPU mirror.
				const auto& previousPayload = packet.m_uploadedSharedPayloads[index];
				const bool bCanDiffVersions = !bStorageAllocationChanged &&
					!bSegmentLayoutChanged && sharedPayload && previousPayload &&
					previousPayload->m_instances.Num() == payload.m_instances.Num();
				if (!bCanDiffVersions)
				{
					if (segmentCount > 0u &&
						(bStorageAllocationChanged || bSegmentLayoutChanged ||
							!sharedPayload || bSharedPayloadChanged))
					{
						instanceUploads.Add({
							payload.m_instances.GetData(),
							segmentOffset,
							segmentCount });
					}
				}
				else if (bSharedPayloadChanged)
				{
					uint32_t dirtyBegin = (std::numeric_limits<uint32_t>::max)();
					auto appendDirtyRange = [&](uint32_t begin, uint32_t end)
						{
							instanceUploads.Add({
								&payload.m_instances[begin],
								segmentOffset + begin,
								end - begin });
						};
					for (uint32_t instanceIndex = 0u;
						instanceIndex < segmentCount; ++instanceIndex)
					{
						const bool bChanged = !(previousPayload->m_instances[instanceIndex] ==
							payload.m_instances[instanceIndex]);
						if (bChanged && dirtyBegin == (std::numeric_limits<uint32_t>::max)())
						{
							dirtyBegin = instanceIndex;
						}
						else if (!bChanged &&
							dirtyBegin != (std::numeric_limits<uint32_t>::max)())
						{
							appendDirtyRange(dirtyBegin, instanceIndex);
							dirtyBegin = (std::numeric_limits<uint32_t>::max)();
						}
					}
					if (dirtyBegin != (std::numeric_limits<uint32_t>::max)())
					{
						appendDirtyRange(dirtyBegin, segmentCount);
					}
				}
			}
			else if (mobility == EMobilityType::Stationary)
			{
				auto& uploaded = packet.m_uploadedStationaryInstances;
				const bool bStationaryCountChanged = uploaded.Num() != segmentCount;
				if (bStorageAllocationChanged || bSegmentLayoutChanged || bStationaryCountChanged)
				{
					if (segmentCount > 0u)
					{
						instanceUploads.Add({
							payload.m_instances.GetData(),
							segmentOffset,
							segmentCount });
						uploaded = payload.m_instances;
					}
					else
					{
						uploaded.Clear(false);
					}
				}
				else if (bSharedPayloadChanged || !sharedPayload)
				{
					uint32_t dirtyBegin = (std::numeric_limits<uint32_t>::max)();
					auto appendDirtyRange = [&](uint32_t begin, uint32_t end)
						{
							const uint32_t count = end - begin;
							instanceUploads.Add({
								&payload.m_instances[begin],
								segmentOffset + begin,
								count });
							std::memcpy(
								&uploaded[begin],
								&payload.m_instances[begin],
								sizeof(TPerInstanceData) * count);
						};
					for (uint32_t instanceIndex = 0u; instanceIndex < segmentCount; ++instanceIndex)
					{
						const bool bChanged =
							!(uploaded[instanceIndex] == payload.m_instances[instanceIndex]);
						if (bChanged && dirtyBegin == (std::numeric_limits<uint32_t>::max)())
						{
							dirtyBegin = instanceIndex;
						}
						else if (!bChanged && dirtyBegin != (std::numeric_limits<uint32_t>::max)())
						{
							appendDirtyRange(dirtyBegin, instanceIndex);
							dirtyBegin = (std::numeric_limits<uint32_t>::max)();
						}
					}
					if (dirtyBegin != (std::numeric_limits<uint32_t>::max)())
					{
						appendDirtyRange(dirtyBegin, segmentCount);
					}
				}
			}
			else if (segmentCount > 0u)
			{
				// Dynamic records deliberately remain flight-local and are rewritten
				// on every submission, independently of pointer or hash stability.
				instanceUploads.Add({
					payload.m_instances.GetData(),
					segmentOffset,
					segmentCount });
			}

			packet.m_uploadedSharedPayloads[index] = sharedPayload;
			packet.m_uploadedSegmentOffsets[index] = segmentOffset;
			packet.m_uploadedSegmentCounts[index] = segmentCount;
			segmentOffset += segmentCount;
		}
		packet.m_uploadedStorageBinding = storageBinding;
		packet.m_uploadedFirstStorageInstance = firstStorageInstance;
		packet.m_metrics.m_dirtyInstanceRanges = static_cast<uint32_t>(instanceUploads.Num());
		packet.m_metrics.m_bReusedInstancePayload = instanceUploads.IsEmpty();
		packet.m_metrics.m_bSharedImmutablePayload = packet.HasSharedImmutablePayload();

		const size_t instanceIndexBytes =
			sizeof(uint32_t) * packet.m_instanceIndices.Num();
		const bool bIndexContentsChanged =
			packet.m_uploadedInstanceIndices.Num() !=
				packet.m_instanceIndices.Num() ||
			(packet.m_instanceIndices.Num() > 0u && std::memcmp(
				packet.m_uploadedInstanceIndices.GetData(),
				packet.m_instanceIndices.GetData(),
				instanceIndexBytes) != 0);
		const bool bResetIndices = bStorageAllocationChanged ||
			packet.m_uploadedIndexBinding != indexBinding ||
			packet.m_uploadedFirstIndexInstance != firstCandidateInstance ||
			bIndexContentsChanged;
		packet.m_uploadedIndexBinding = indexBinding;
		packet.m_uploadedFirstIndexInstance = firstCandidateInstance;

		packet.m_indirectCommands.Resize(groups.Num());
		for (uint32_t groupIndex = 0u; groupIndex < groups.Num(); ++groupIndex)
		{
			const auto& group = groups[groupIndex];
			auto& command = packet.m_indirectCommands[groupIndex];
			command.m_indexCount = group.m_mesh->GetIndexCount();
			command.m_instanceCount = group.m_numInstances;
			command.m_firstIndex = group.m_mesh->GetFirstIndex();
			command.m_vertexOffset = group.m_mesh->GetVertexOffset();
			command.m_firstInstance = firstIndexInstance + group.m_firstInstance;
			stats.m_numInstances += group.m_numInstances;
		}

		for (const auto& upload : instanceUploads)
		{
			const size_t rangeSize = sizeof(TPerInstanceData) * upload.m_count;
			commands->UpdateShaderBinding(
				transferCmdList,
				storageBinding,
				upload.m_data,
				rangeSize,
				sizeof(TPerInstanceData) * upload.m_offset);
			packet.m_metrics.m_instanceUploadBytes += rangeSize;
		}
		if (bResetIndices)
		{
			packet.m_resolvedInstanceIndices.Resize(packet.m_instanceIndices.Num());
			for (uint32_t index = 0u; index < packet.m_instanceIndices.Num(); ++index)
			{
				packet.m_resolvedInstanceIndices[index] =
					firstStorageInstance + packet.m_instanceIndices[index];
			}
			commands->UpdateShaderBinding(
				transferCmdList,
				indexBinding,
				packet.m_resolvedInstanceIndices.GetData(),
				instanceIndexBytes,
				0u);
			packet.m_uploadedInstanceIndices = packet.m_instanceIndices;
			packet.m_metrics.m_indexUploadBytes += instanceIndexBytes;
		}

		const size_t indirectBufferSize = sizeof(DrawIndexedIndirectData) * packet.m_indirectCommands.Num();
		bool bIndirectBufferChanged = false;
		if (!indirectCommandBuffer || indirectCommandBuffer->GetSize() < indirectBufferSize)
		{
			constexpr size_t IndirectBufferSlack = 256u;
			indirectCommandBuffer = driver->CreateIndirectBuffer(indirectBufferSize + IndirectBufferSlack);
			bIndirectBufferChanged = true;
			if (indirectCommandBufferBinding && *indirectCommandBufferBinding)
			{
				driver->AddBufferToShaderBindings(
					*indirectCommandBufferBinding,
					indirectCommandBuffer,
					"drawIndexedIndirect",
					0u);
			}
		}
		else if (computeCullingShader && indirectCommandBufferBinding &&
			*indirectCommandBufferBinding &&
			!(*indirectCommandBufferBinding)->HasBinding("drawIndexedIndirect"))
		{
			driver->AddBufferToShaderBindings(
				*indirectCommandBufferBinding,
				indirectCommandBuffer,
				"drawIndexedIndirect",
				0u);
		}

		const bool bResetIndirectCommands = computeCullingShader ||
			bIndirectBufferChanged ||
			packet.m_uploadedIndirectBuffer != indirectCommandBuffer ||
			packet.m_uploadedIndirectCommands.Num() != packet.m_indirectCommands.Num() ||
			(packet.m_indirectCommands.Num() > 0u && std::memcmp(
				packet.m_uploadedIndirectCommands.GetData(),
				packet.m_indirectCommands.GetData(),
				indirectBufferSize) != 0);
		if (bResetIndirectCommands)
		{
			commands->UpdateBuffer(
				transferCmdList,
				indirectCommandBuffer,
				packet.m_indirectCommands.GetData(),
				indirectBufferSize,
				0u);
			packet.m_uploadedIndirectCommands = packet.m_indirectCommands;
			packet.m_uploadedIndirectBuffer = indirectCommandBuffer;
			packet.m_metrics.m_indirectUploadBytes += indirectBufferSize;
		}

		if (computeCullingShader)
		{
			struct PushConstants
			{
				uint32_t m_numBatches = 0u;
				uint32_t m_numInstances = 0u;
				uint32_t m_firstInstanceIndex = 0u;
				uint32_t m_firstStorageInstance = 0u;
				uint32_t m_firstCandidateInstance = 0u;
				uint32_t m_phase = 0u;
				uint32_t m_bEnableOcclusion = 0u;
			};

			PushConstants constants;
			constants.m_numBatches = static_cast<uint32_t>(groups.Num());
			constants.m_numInstances = numInstances;
			constants.m_firstInstanceIndex = firstIndexInstance;
			constants.m_firstStorageInstance = firstStorageInstance;
			constants.m_firstCandidateInstance = firstCandidateInstance;
			constants.m_bEnableOcclusion = bEnableOcclusion ? 1u : 0u;
			commands->BeginDebugRegion(cullingCommandList, "GPU Culling", DebugContext::Color_CmdCompute);
			const EAccessFlags uploadWrites = static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit) |
				static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit);
			const EAccessFlags shaderReadWrite = static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit) |
				static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit);
			commands->MemoryBarrier(cullingCommandList, uploadWrites, shaderReadWrite);

			const uint32_t cullingGroups = (std::max)(1u,
				(constants.m_numInstances + Renderer::GPUCullingGroupSize - 1u) / Renderer::GPUCullingGroupSize);
			commands->Dispatch(
				cullingCommandList,
				computeCullingShader,
				cullingGroups,
				1u,
				1u,
				cullingDispatchBindings,
				&constants,
				sizeof(constants));
			commands->MemoryBarrier(
				cullingCommandList,
				static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit),
				shaderReadWrite);

			constants.m_phase = 1u;
			const uint32_t compactionGroups = (std::max)(1u,
				(constants.m_numBatches + Renderer::GPUCullingGroupSize - 1u) / Renderer::GPUCullingGroupSize);
			commands->Dispatch(
				cullingCommandList,
				computeCullingShader,
				compactionGroups,
				1u,
				1u,
				cullingDispatchBindings,
				&constants,
				sizeof(constants));
			if (cullingCommandList == graphicsCmdList)
			{
				const EAccessFlags drawReads =
					static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit) |
					static_cast<EAccessFlags>(EAccessBit::IndirectCommandRead_Bit);
				commands->MemoryBarrier(
					cullingCommandList,
					static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit),
					drawReads);
			}
			commands->EndDebugRegion(cullingCommandList);
		}

		beforeDraw();

#if defined(_WIN32)
		constexpr uint32_t MaxMeshesPerIndirectBatch = 16384u;
#else
		constexpr uint32_t MaxMeshesPerIndirectBatch = 128u;
#endif

		uint32_t runBegin = 0u;
		auto& drawBindingSets = packet.m_drawBindingSets;
		while (runBegin < groups.Num())
		{
			const auto& firstGroup = groups[runBegin];
			const uint32_t runLimit = (std::max)(1u,
				(std::min)(MaxMeshesPerIndirectBatch, firstGroup.m_batch.m_supportedMeshesPerBatch));
			uint32_t runEnd = runBegin + 1u;
			while (runEnd < groups.Num() &&
				runEnd - runBegin < runLimit &&
				firstGroup.m_batch == groups[runEnd].m_batch)
			{
				++runEnd;
			}

			const auto& batch = firstGroup.m_batch;
			commands->BindMaterial(graphicsCmdList, batch.m_material);
			commands->SetViewport(
				graphicsCmdList,
				static_cast<float>(viewport.x),
				static_cast<float>(viewport.y),
				static_cast<float>(viewport.z),
				static_cast<float>(viewport.w),
				glm::vec2(scissors.x, scissors.y),
				glm::vec2(scissors.z, scissors.w),
				depthRange.x,
				depthRange.y);
			drawBindingSets.Clear(false);
			collectShaderBindings(batch, drawBindingSets);
			commands->BindShaderBindings(
				graphicsCmdList,
				batch.m_material,
				drawBindingSets);
			commands->BindVertexBuffer(graphicsCmdList, batch.m_mesh->m_vertexBuffer, 0u);
			commands->BindIndexBuffer(graphicsCmdList, batch.m_mesh->m_indexBuffer, 0u);
			commands->DrawIndexedIndirect(
				graphicsCmdList,
				indirectCommandBuffer,
				sizeof(DrawIndexedIndirectData) * runBegin,
				runEnd - runBegin,
				sizeof(DrawIndexedIndirectData));
			++stats.m_numBatches;
			runBegin = runEnd;
		}
		drawBindingSets.Clear(false);

		return stats;
	}

	template<typename TPerInstanceData, typename TShaderBindingsCallback>
	DrawCallStats RHIRecordPackedDrawPacket(
		TPackedDrawPacket<TPerInstanceData>& packet,
		RHICommandListPtr graphicsCmdList,
		RHICommandListPtr transferCmdList,
		TShaderBindingsCallback&& collectShaderBindings,
		RHIShaderBindingSetPtr instanceBindings,
		RHIBufferPtr& indirectCommandBuffer,
		glm::ivec4 viewport,
		glm::uvec4 scissors,
		glm::vec2 depthRange = glm::vec2(0.0f, 1.0f),
		RHIShaderPtr computeCullingShader = {},
		RHIShaderBindingSetPtr* indirectCommandBufferBinding = nullptr,
		const TVector<RHIShaderBindingSetPtr>& cullingDispatchBindings = {})
	{
		auto beforeDraw = []() {};
		return RHIRecordPackedDrawPacketImpl(
			packet,
			graphicsCmdList,
			transferCmdList,
			collectShaderBindings,
			instanceBindings,
			indirectCommandBuffer,
			viewport,
			scissors,
			depthRange,
			computeCullingShader,
			indirectCommandBufferBinding,
			cullingDispatchBindings,
			transferCmdList,
			false,
			beforeDraw);
	}

	template<typename TPerInstanceData, typename TShaderBindingsCallback, typename TBeforeDrawCallback>
	DrawCallStats RHIRecordPackedDrawPacketWithCurrentDepthOcclusion(
		TPackedDrawPacket<TPerInstanceData>& packet,
		RHICommandListPtr graphicsCmdList,
		RHICommandListPtr transferCmdList,
		TShaderBindingsCallback&& collectShaderBindings,
		RHIShaderBindingSetPtr instanceBindings,
		RHIBufferPtr& indirectCommandBuffer,
		glm::ivec4 viewport,
		glm::uvec4 scissors,
		glm::vec2 depthRange,
		RHIShaderPtr computeCullingShader,
		RHIShaderBindingSetPtr* indirectCommandBufferBinding,
		const TVector<RHIShaderBindingSetPtr>& cullingDispatchBindings,
		TBeforeDrawCallback&& beforeDraw)
	{
		return RHIRecordPackedDrawPacketImpl(
			packet,
			graphicsCmdList,
			transferCmdList,
			collectShaderBindings,
			instanceBindings,
			indirectCommandBuffer,
			viewport,
			scissors,
			depthRange,
			computeCullingShader,
			indirectCommandBufferBinding,
			cullingDispatchBindings,
			graphicsCmdList,
			true,
			beforeDraw);
	}

};
