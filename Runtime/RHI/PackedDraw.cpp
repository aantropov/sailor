#include "PackedDraw.hpp"

using namespace Sailor;
using namespace Sailor::RHI;

uint32_t Sailor::RHI::GetPackedDrawRunEnd(const TVector<PackedDrawGroup>& groups, uint32_t runBegin)
{
#if defined(_WIN32)
	constexpr uint32_t MaxMeshesPerIndirectBatch = 16384u;
#else
	constexpr uint32_t MaxMeshesPerIndirectBatch = 128u;
#endif
	const auto& firstGroup = groups[runBegin];
	const uint32_t runLimit = (std::max)(1u,
		(std::min)(MaxMeshesPerIndirectBatch, firstGroup.m_batch.m_supportedMeshesPerBatch));
	uint32_t runInstances = firstGroup.m_numInstances;
	uint32_t runEnd = runBegin + 1u;
	while (runEnd < groups.Num() &&
		runEnd - runBegin < runLimit &&
		groups[runEnd].m_numInstances <= RHIBatch::MaxInstancesPerBatch - runInstances &&
		firstGroup.m_batch == groups[runEnd].m_batch)
	{
		runInstances += groups[runEnd].m_numInstances;
		++runEnd;
	}
	return runEnd;
}

void Sailor::RHI::SortPackedDrawItems(TVector<PackedDrawItem>& items, TVector<uint32_t>& indices)
{
	SAILOR_PROFILE_SCOPE("Sort packed draw items");
	// A batch is immutable during finalization. Calculate its key once
	// instead of retaining shared material bindings on every comparison.
	indices.Resize(items.Num());
	for (uint32_t index = 0u; index < items.Num(); ++index)
	{
		items[index].m_batchSortHash = items[index].m_batch.GetHash();
		indices[index] = index;
	}
	// Sort indices so shared resource references stay in place. Moving
	// the owning items repeatedly contends with other render passes.
	indices.Sort([&items](uint32_t lhsIndex, uint32_t rhsIndex)
	{
		const auto& lhs = items[lhsIndex];
		const auto& rhs = items[rhsIndex];
		const size_t lhsBatchHash = lhs.m_batchSortHash;
		const size_t rhsBatchHash = rhs.m_batchSortHash;
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
}

void RHIPackedDrawSceneChanges::Gather(const RHIPackedDrawSceneState& current,
	const RHIPackedDrawSceneState* previous, EMobilityType mobility, bool bShadowCastersOnly)
{
	Clear();
	const bool bRefreshMaterials = !previous || current.m_configurationRevision != previous->m_configurationRevision;
	auto findScene = [](const TSharedPtr<const TVector<RHISceneVersionPtr>>& scenes, uint64_t identity)
		-> const RHISceneVersion*
	{
		if (scenes)
		{
			for (const auto& scene : *scenes)
			{
				if (scene && scene->m_sceneIdentity == identity)
				{
					return scene.GetRawPtr();
				}
			}
		}
		return nullptr;
	};
	auto getPage = [](const RHISceneVersion* scene, size_t pageIndex) -> const RHISceneRecordPage*
	{
		return scene && scene->m_recordsRoot && pageIndex < scene->m_recordsRoot->m_pages.Num() ?
			scene->m_recordsRoot->m_pages[pageIndex].GetRawPtr() : nullptr;
	};
	auto getResource = [&](const RHISceneRecordSlot* slot) -> const RHISceneProxyResource*
	{
		if (!slot || !slot->m_bActive || slot->m_record.m_mobility != mobility)
		{
			return nullptr;
		}
		const auto* resource = dynamic_cast<const RHISceneProxyResource*>(slot->m_record.m_topology.GetRawPtr());
		if (resource && bShadowCastersOnly &&
			((slot->m_record.m_renderFlags & 1u) == 0u || !resource->m_proxy.m_shadowCaster))
		{
			return nullptr;
		}
		return resource;
	};
	auto getMotionRecord = [](const RHISceneVersion* scene, RenderInstanceHandle handle,
		const RHISceneInstanceRecord& currentRecord) -> const RHISceneInstanceRecord*
	{
		const RHISceneInstanceRecord* record = nullptr;
		return scene && scene->Resolve(handle, record) && record->m_producerKey == currentRecord.m_producerKey &&
			record->m_topology == currentRecord.m_topology ? record : nullptr;
	};
	auto visitScene = [&](const RHISceneVersion* next, const RHISceneVersion* old)
	{
		const uint64_t identity = next ? next->m_sceneIdentity : old->m_sceneIdentity;
		const auto* nextMotion = findScene(current.m_motionSceneVersions, identity);
		const auto* oldMotion = previous ? findScene(previous->m_motionSceneVersions, identity) : nullptr;
		const size_t numPages = (std::max)(next && next->m_recordsRoot ? next->m_recordsRoot->m_pages.Num() : 0u,
			old && old->m_recordsRoot ? old->m_recordsRoot->m_pages.Num() : 0u);
		for (size_t pageIndex = 0u; pageIndex < numPages; ++pageIndex)
		{
			const auto* nextPage = getPage(next, pageIndex);
			const auto* oldPage = getPage(old, pageIndex);
			if (!bRefreshMaterials && nextPage == oldPage &&
				getPage(nextMotion, pageIndex) == getPage(oldMotion, pageIndex))
			{
				continue;
			}
			for (uint32_t index = 0u; index < RHISceneRecordPage::NumRecords; ++index)
			{
				++m_numComparedRecords;
				const auto* nextSlot = nextPage ? &nextPage->m_slots[index] : nullptr;
				const auto* oldSlot = oldPage ? &oldPage->m_slots[index] : nullptr;
				const auto* nextResource = getResource(nextSlot);
				const auto* oldResource = getResource(oldSlot);
				const uint32_t slotIndex = static_cast<uint32_t>(pageIndex) * RHISceneRecordPage::NumRecords + index;
				const bool bSameRange = nextResource && oldResource && nextResource == oldResource &&
					nextSlot->m_generation == oldSlot->m_generation &&
					nextSlot->m_record.m_producerKey == oldSlot->m_record.m_producerKey;
				if (oldResource && !bSameRange)
				{
					m_removed.Emplace(RenderInstanceHandle{ slotIndex, oldSlot->m_generation }, oldSlot->m_record, *oldResource);
				}
				if (!nextResource)
				{
					continue;
				}
				const RenderInstanceHandle handle{ slotIndex, nextSlot->m_generation };
				const auto& record = nextSlot->m_record;
				bool bChanged = bRefreshMaterials || !bSameRange;
				if (!bChanged)
				{
					const auto& oldRecord = oldSlot->m_record;
					bChanged = record.m_worldMatrix != oldRecord.m_worldMatrix ||
						record.m_skeletonOffset != oldRecord.m_skeletonOffset ||
						record.m_topologyRevision != oldRecord.m_topologyRevision ||
						record.m_materialRevision != oldRecord.m_materialRevision ||
						record.m_shadowRevision != oldRecord.m_shadowRevision ||
						record.m_renderFlags != oldRecord.m_renderFlags;
					const auto* nextMotionRecord = getMotionRecord(nextMotion, handle, record);
					const auto* oldMotionRecord = getMotionRecord(oldMotion, handle, oldRecord);
					bChanged |= (nextMotionRecord != nullptr) != (oldMotionRecord != nullptr);
					if (nextMotionRecord && oldMotionRecord)
					{
						bChanged |= nextMotionRecord->m_worldMatrix != oldMotionRecord->m_worldMatrix ||
							nextMotionRecord->m_skeletonOffset != oldMotionRecord->m_skeletonOffset;
					}
				}
				if (bChanged)
				{
					m_updated.Emplace(handle, record, *nextResource);
				}
			}
		}
	};
	if (current.m_sceneVersions)
	{
		for (const auto& scene : *current.m_sceneVersions)
		{
			if (scene)
			{
				visitScene(scene.GetRawPtr(), previous ? findScene(previous->m_sceneVersions, scene->m_sceneIdentity) : nullptr);
			}
		}
	}
	if (previous && previous->m_sceneVersions)
	{
		for (const auto& scene : *previous->m_sceneVersions)
		{
			if (scene && !findScene(current.m_sceneVersions, scene->m_sceneIdentity))
			{
				visitScene(nullptr, scene.GetRawPtr());
			}
		}
	}
}
