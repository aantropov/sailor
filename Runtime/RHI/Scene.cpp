#include "RHI/Scene.h"

#include <algorithm>
#include <cstring>

using namespace Sailor;
using namespace Sailor::RHI;

bool RHISceneVersion::Resolve(
	RenderInstanceHandle handle,
	const RHISceneInstanceRecord*& outRecord) const
{
	outRecord = nullptr;
	if (!handle.IsValid() || !m_recordsRoot)
	{
		return false;
	}

	const uint32_t pageIndex = handle.m_slot / RHISceneRecordPage::NumRecords;
	const uint32_t slotIndex = handle.m_slot % RHISceneRecordPage::NumRecords;
	if (pageIndex >= m_recordsRoot->m_pages.Num() || !m_recordsRoot->m_pages[pageIndex])
	{
		return false;
	}

	const auto& slot = m_recordsRoot->m_pages[pageIndex]->m_slots[slotIndex];
	if (!slot.m_bActive || slot.m_generation != handle.m_generation)
	{
		return false;
	}

	outRecord = &slot.m_record;
	return true;
}

bool RHISceneVersion::HasShadowChangesIntersecting(
	const RHISceneVersion& previousVersion,
	const Math::Frustum& shadowFrustum) const
{
	if (m_recordsRoot == previousVersion.m_recordsRoot)
	{
		return false;
	}
	if (!m_recordsRoot || !previousVersion.m_recordsRoot)
	{
		return true;
	}

	const size_t numPages = (std::max)(
		m_recordsRoot->m_pages.Num(),
		previousVersion.m_recordsRoot->m_pages.Num());
	for (size_t pageIndex = 0u; pageIndex < numPages; ++pageIndex)
	{
		const RHISceneRecordPage* currentPage =
			pageIndex < m_recordsRoot->m_pages.Num() ?
			m_recordsRoot->m_pages[pageIndex].GetRawPtr() : nullptr;
		const RHISceneRecordPage* previousPage =
			pageIndex < previousVersion.m_recordsRoot->m_pages.Num() ?
			previousVersion.m_recordsRoot->m_pages[pageIndex].GetRawPtr() : nullptr;
		if (currentPage == previousPage)
		{
			continue;
		}

		for (uint32_t slotIndex = 0u; slotIndex < RHISceneRecordPage::NumRecords; ++slotIndex)
		{
			const RHISceneRecordSlot* current = currentPage ?
				&currentPage->m_slots[slotIndex] : nullptr;
			const RHISceneRecordSlot* previous = previousPage ?
				&previousPage->m_slots[slotIndex] : nullptr;
			const bool bCurrentCaster = current && current->m_bActive &&
				(current->m_record.m_renderFlags & 1u) != 0u;
			const bool bPreviousCaster = previous && previous->m_bActive &&
				(previous->m_record.m_renderFlags & 1u) != 0u;
			if (!bCurrentCaster && !bPreviousCaster)
			{
				continue;
			}

			const bool bSameRecord = current && previous &&
				current->m_bActive == previous->m_bActive &&
				current->m_generation == previous->m_generation &&
				current->m_record.m_shadowRevision == previous->m_record.m_shadowRevision &&
				current->m_record.m_skeletonOffset == previous->m_record.m_skeletonOffset &&
				current->m_record.m_renderFlags == previous->m_record.m_renderFlags &&
				current->m_record.m_worldBounds == previous->m_record.m_worldBounds &&
				std::memcmp(
					&current->m_record.m_worldMatrix,
					&previous->m_record.m_worldMatrix,
					sizeof(glm::mat4)) == 0;
			if (bSameRecord)
			{
				continue;
			}

			auto intersects = [&shadowFrustum](
				const RHISceneRecordSlot* slot,
				bool bCaster)
				{
					if (!slot || !bCaster)
					{
						return false;
					}
					return !slot->m_record.m_worldBounds.IsValid() ||
						shadowFrustum.OverlapsAABB(slot->m_record.m_worldBounds);
				};
			if (intersects(current, bCurrentCaster) ||
				intersects(previous, bPreviousCaster))
			{
				return true;
			}
		}
	}

	return false;
}

uint32_t RHISceneRangeAllocator::AllocateCapacity(uint32_t count) const
{
	uint32_t result = 1u;
	while (result < count && result <= (std::numeric_limits<uint32_t>::max)() / 2u)
	{
		result *= 2u;
	}
	return (std::max)(result, count);
}

SceneRangeHandle RHISceneRangeAllocator::Allocate(uint32_t count)
{
	if (count == 0u)
	{
		return {};
	}

	const uint32_t requestedCapacity = AllocateCapacity(count);
	uint32_t slotIndex = 0u;
	if (!m_reusableSlots.IsEmpty())
	{
		slotIndex = *m_reusableSlots.Last();
		m_reusableSlots.RemoveLast();
	}
	else
	{
		slotIndex = static_cast<uint32_t>(m_slots.Num());
		m_slots.AddDefault(1u);
	}

	auto& slot = m_slots[slotIndex];
	slot.m_bActive = true;
	slot.m_retirementRevision = 0ull;
	slot.m_allocation = {};
	for (size_t index = 0u; index < m_freeRanges.Num(); ++index)
	{
		auto& freeRange = m_freeRanges[index];
		if (freeRange.m_bufferGeneration != m_bufferGeneration ||
			freeRange.m_capacity < requestedCapacity)
		{
			continue;
		}

		slot.m_allocation = {
			m_bufferGeneration,
			freeRange.m_offset,
			count,
			requestedCapacity };
		freeRange.m_offset += requestedCapacity;
		freeRange.m_capacity -= requestedCapacity;
		if (freeRange.m_capacity == 0u)
		{
			m_freeRanges.RemoveAt(index);
		}
		break;
	}

	if (!slot.m_allocation.IsValid())
	{
		const uint64_t required = static_cast<uint64_t>(m_nextOffset) + requestedCapacity;
		if (required > m_capacity)
		{
			uint32_t newCapacity = (std::max)(1u, m_capacity);
			while (newCapacity < required && newCapacity <= (std::numeric_limits<uint32_t>::max)() / 2u)
			{
				newCapacity *= 2u;
			}
			m_capacity = (std::max)(newCapacity, static_cast<uint32_t>(required));
			++m_bufferGeneration;
			if (m_bufferGeneration == 0u)
			{
				m_bufferGeneration = 1u;
			}
		}

		slot.m_allocation = {
			m_bufferGeneration,
			m_nextOffset,
			count,
			requestedCapacity };
		m_nextOffset += requestedCapacity;
	}

	return { slotIndex, slot.m_generation };
}

bool RHISceneRangeAllocator::Resolve(
	SceneRangeHandle handle,
	PhysicalAllocation& outAllocation) const
{
	outAllocation = {};
	if (!handle.IsValid() || handle.m_slot >= m_slots.Num())
	{
		return false;
	}

	const auto& slot = m_slots[handle.m_slot];
	if (!slot.m_bActive || slot.m_generation != handle.m_generation)
	{
		return false;
	}

	outAllocation = slot.m_allocation;
	return true;
}

bool RHISceneRangeAllocator::Retire(
	SceneRangeHandle handle,
	uint64_t retirementRevision)
{
	if (!handle.IsValid() || handle.m_slot >= m_slots.Num())
	{
		return false;
	}

	auto& slot = m_slots[handle.m_slot];
	if (!slot.m_bActive || slot.m_generation != handle.m_generation)
	{
		return false;
	}

	slot.m_bActive = false;
	slot.m_retirementRevision = retirementRevision;
	m_retiredSlots.Add(handle.m_slot);
	return true;
}

void RHISceneRangeAllocator::MergeFreeRanges()
{
	if (m_freeRanges.Num() < 2u)
	{
		return;
	}

	m_freeRanges.Sort([](const FreeRange& lhs, const FreeRange& rhs)
		{
			if (lhs.m_bufferGeneration != rhs.m_bufferGeneration)
			{
				return lhs.m_bufferGeneration < rhs.m_bufferGeneration;
			}
			return lhs.m_offset < rhs.m_offset;
		});

	size_t writeIndex = 0u;
	for (size_t readIndex = 1u; readIndex < m_freeRanges.Num(); ++readIndex)
	{
		auto& previous = m_freeRanges[writeIndex];
		const auto& range = m_freeRanges[readIndex];
		if (previous.m_bufferGeneration == range.m_bufferGeneration &&
			previous.m_offset + previous.m_capacity == range.m_offset)
		{
			previous.m_capacity += range.m_capacity;
			continue;
		}
		++writeIndex;
		m_freeRanges[writeIndex] = range;
	}
	m_freeRanges.Resize(writeIndex + 1u);
}

void RHISceneRangeAllocator::Collect(uint64_t minimumRetainedRevision)
{
	size_t writeIndex = 0u;
	for (size_t readIndex = 0u; readIndex < m_retiredSlots.Num(); ++readIndex)
	{
		const uint32_t slotIndex = m_retiredSlots[readIndex];
		auto& slot = m_slots[slotIndex];
		if (minimumRetainedRevision < slot.m_retirementRevision)
		{
			m_retiredSlots[writeIndex++] = slotIndex;
			continue;
		}

		m_freeRanges.Add({
			slot.m_allocation.m_bufferGeneration,
			slot.m_allocation.m_offset,
			slot.m_allocation.m_capacity });
		slot.m_allocation = {};
		slot.m_retirementRevision = 0ull;
		++slot.m_generation;
		if (slot.m_generation == 0u)
		{
			slot.m_generation = 1u;
		}
		m_reusableSlots.Add(slotIndex);
	}
	m_retiredSlots.Resize(writeIndex);
	MergeFreeRanges();
}

TVector<DirtySceneRange> RHISceneRangeAllocator::CoalesceDirtyRanges(
	TVector<DirtySceneRange> ranges)
{
	size_t validRangeWriteIndex = 0u;
	for (const auto& range : ranges)
	{
		if (range.m_count > 0u)
		{
			ranges[validRangeWriteIndex++] = range;
		}
	}
	ranges.Resize(validRangeWriteIndex);
	ranges.Sort([](const DirtySceneRange& lhs, const DirtySceneRange& rhs)
		{
			return lhs.m_offset < rhs.m_offset;
		});

	if (ranges.Num() < 2u)
	{
		return ranges;
	}

	size_t writeIndex = 0u;
	for (size_t readIndex = 1u; readIndex < ranges.Num(); ++readIndex)
	{
		auto& previous = ranges[writeIndex];
		const auto& range = ranges[readIndex];
		const uint64_t previousEnd = static_cast<uint64_t>(previous.m_offset) + previous.m_count;
		const uint64_t rangeEnd = static_cast<uint64_t>(range.m_offset) + range.m_count;
		if (range.m_offset <= previousEnd)
		{
			previous.m_count = static_cast<uint32_t>(
				(std::max)(previousEnd, rangeEnd) - previous.m_offset);
			continue;
		}
		++writeIndex;
		ranges[writeIndex] = range;
	}
	ranges.Resize(writeIndex + 1u);
	return ranges;
}

RHIScene::RHIScene(uint32_t numFlightSlots)
{
	m_currentRoot = RHISceneRecordRootPtr::Make();
	m_flights.Resize((std::max)(1u, numFlightSlots));
	for (uint32_t index = 0u; index < m_flights.Num(); ++index)
	{
		m_flights[index] = RHISceneFlightStatePtr::Make();
		m_flights[index]->m_flightSlot = index;
	}
}

bool RHIScene::ResolveSlot(RenderInstanceHandle handle, LogicalSlot*& outSlot)
{
	outSlot = nullptr;
	if (!handle.IsValid() || handle.m_slot >= m_slots.Num())
	{
		return false;
	}
	auto& slot = m_slots[handle.m_slot];
	if (!slot.m_bActive || slot.m_generation != handle.m_generation)
	{
		return false;
	}
	outSlot = &slot;
	return true;
}

bool RHIScene::ResolveSlot(RenderInstanceHandle handle, const LogicalSlot*& outSlot) const
{
	outSlot = nullptr;
	if (!handle.IsValid() || handle.m_slot >= m_slots.Num())
	{
		return false;
	}
	const auto& slot = m_slots[handle.m_slot];
	if (!slot.m_bActive || slot.m_generation != handle.m_generation)
	{
		return false;
	}
	outSlot = &slot;
	return true;
}

void RHIScene::AppendChange(
	RenderInstanceHandle handle,
	SceneChangeMask mask)
{
	++m_revision;
	m_journal.Add({ m_revision, handle, mask });
	if (m_dirtySlotFlags.Num() <= handle.m_slot)
	{
		m_dirtySlotFlags.Resize(static_cast<size_t>(handle.m_slot) + 1u);
	}
	if (m_dirtySlotFlags[handle.m_slot] == 0u)
	{
		m_dirtySlotFlags[handle.m_slot] = 1u;
		m_dirtySlots.Add(handle.m_slot);
	}
	++m_metrics.m_numDirtyChanges;
}

void RHIScene::BumpMobilityRevision(EMobilityType mobility)
{
	switch (mobility)
	{
	case EMobilityType::Static:
		++m_staticRevision;
		break;
	case EMobilityType::Stationary:
		++m_stationaryRevision;
		break;
	case EMobilityType::Dynamic:
		++m_dynamicRevision;
		break;
	}
}

RenderInstanceHandle RHIScene::AddInstance(const RHISceneInstanceRecord& record)
{
	m_lock.Lock();
	uint32_t slotIndex = 0u;
	if (!m_reusableSlots.IsEmpty())
	{
		slotIndex = *m_reusableSlots.Last();
		m_reusableSlots.RemoveLast();
	}
	else
	{
		slotIndex = static_cast<uint32_t>(m_slots.Num());
		m_slots.AddDefault(1u);
	}

	auto& slot = m_slots[slotIndex];
	slot.m_bActive = true;
	slot.m_retirementRevision = 0ull;
	slot.m_record = record;
	const RenderInstanceHandle handle{ slotIndex, slot.m_generation };
	AppendChange(handle, ToMask(ESceneChangeBit::Add));
	BumpMobilityRevision(record.m_mobility);
	m_bHandleListsDirty = true;
	m_lock.Unlock();
	return handle;
}

bool RHIScene::UpdateInstance(
	RenderInstanceHandle handle,
	const RHISceneInstanceRecord& record,
	SceneChangeMask changeMask)
{
	if (changeMask == ToMask(ESceneChangeBit::None))
	{
		return true;
	}

	m_lock.Lock();
	LogicalSlot* slot = nullptr;
	if (!ResolveSlot(handle, slot))
	{
		m_lock.Unlock();
		return false;
	}

	const EMobilityType oldMobility = slot->m_record.m_mobility;
	slot->m_record = record;
	AppendChange(handle, changeMask);
	BumpMobilityRevision(oldMobility);
	if (oldMobility != record.m_mobility)
	{
		BumpMobilityRevision(record.m_mobility);
	}
	if (oldMobility != record.m_mobility ||
		(changeMask & ToMask(ESceneChangeBit::Mobility)) != 0u)
	{
		m_bHandleListsDirty = true;
	}
	m_lock.Unlock();
	return true;
}

bool RHIScene::RemoveInstance(RenderInstanceHandle handle)
{
	m_lock.Lock();
	LogicalSlot* slot = nullptr;
	if (!ResolveSlot(handle, slot))
	{
		m_lock.Unlock();
		return false;
	}

	const EMobilityType oldMobility = slot->m_record.m_mobility;
	slot->m_bActive = false;
	AppendChange(handle, ToMask(ESceneChangeBit::Remove));
	BumpMobilityRevision(oldMobility);
	slot->m_retirementRevision = m_revision;
	m_retiredSlots.Add(handle.m_slot);
	m_bHandleListsDirty = true;
	m_lock.Unlock();
	return true;
}

bool RHIScene::ResolveCurrent(
	RenderInstanceHandle handle,
	RHISceneInstanceRecord& outRecord) const
{
	m_lock.Lock();
	const LogicalSlot* slot = nullptr;
	const bool bResult = ResolveSlot(handle, slot);
	outRecord = bResult ? slot->m_record : RHISceneInstanceRecord{};
	m_lock.Unlock();
	return bResult;
}

RHISceneRecordRootPtr RHIScene::BuildRecordRoot()
{
	if (m_dirtySlots.IsEmpty() && m_currentRoot)
	{
		return m_currentRoot;
	}

	auto root = RHISceneRecordRootPtr::Make();
	if (m_currentRoot)
	{
		root->m_pages = m_currentRoot->m_pages;
		root->m_generation = m_currentRoot->m_generation;
	}

	m_cowPageScratch.Clear(false);
	m_cowPageScratch.Resize(
		(m_slots.Num() + RHISceneRecordPage::NumRecords - 1u) /
		RHISceneRecordPage::NumRecords);
	if (!m_cowPageScratch.IsEmpty())
	{
		std::memset(m_cowPageScratch.GetData(), 0, m_cowPageScratch.Num());
	}
	for (uint32_t logicalSlotIndex : m_dirtySlots)
	{
		const uint32_t pageIndex = logicalSlotIndex / RHISceneRecordPage::NumRecords;
		const uint32_t pageSlot = logicalSlotIndex % RHISceneRecordPage::NumRecords;
		if (root->m_pages.Num() <= pageIndex)
		{
			root->m_pages.Resize(pageIndex + 1u);
		}

		if (m_cowPageScratch[pageIndex] == 0u)
		{
			root->m_pages[pageIndex] = root->m_pages[pageIndex] ?
				RHISceneRecordPagePtr::Make(*root->m_pages[pageIndex]) :
				RHISceneRecordPagePtr::Make();
			m_cowPageScratch[pageIndex] = 1u;
			++m_metrics.m_numCowPages;
			m_metrics.m_copiedCpuBytes += sizeof(RHISceneRecordPage);
		}

		auto& target = root->m_pages[pageIndex]->m_slots[pageSlot];
		const auto& source = m_slots[logicalSlotIndex];
		target.m_generation = source.m_generation;
		target.m_bActive = source.m_bActive;
		if (source.m_bActive)
		{
			target.m_record = source.m_record;
		}
		else
		{
			target.m_record = {};
		}
	}

	if (!m_dirtySlots.IsEmpty())
	{
		++root->m_generation;
		if (root->m_generation == 0u)
		{
			root->m_generation = 1u;
		}
	}
	return root;
}

void RHIScene::RebuildHandleLists(RHISceneVersion& version)
{
	version.m_staticHandles = TSharedPtr<TVector<RenderInstanceHandle>>::Make();
	version.m_stationaryHandles = TSharedPtr<TVector<RenderInstanceHandle>>::Make();
	version.m_dynamicHandles = TSharedPtr<TVector<RenderInstanceHandle>>::Make();
	m_metrics.m_numStaticInstances = 0u;
	m_metrics.m_numStationaryInstances = 0u;
	m_metrics.m_numDynamicInstances = 0u;

	for (uint32_t slotIndex = 0u; slotIndex < m_slots.Num(); ++slotIndex)
	{
		const auto& slot = m_slots[slotIndex];
		if (!slot.m_bActive)
		{
			continue;
		}
		const RenderInstanceHandle handle{ slotIndex, slot.m_generation };
		switch (slot.m_record.m_mobility)
		{
		case EMobilityType::Static:
			version.m_staticHandles->Add(handle);
			++m_metrics.m_numStaticInstances;
			break;
		case EMobilityType::Stationary:
			version.m_stationaryHandles->Add(handle);
			++m_metrics.m_numStationaryInstances;
			break;
		case EMobilityType::Dynamic:
			version.m_dynamicHandles->Add(handle);
			++m_metrics.m_numDynamicInstances;
			break;
		}
	}
}

RHISceneVersionPtr RHIScene::PublishVersion(
	uint64_t materialRevision,
	uint64_t shadowRevision,
	uint64_t spatialRevision)
{
	m_lock.Lock();
	if (m_currentVersion &&
		m_lastPublishedRevision == m_revision &&
		m_currentVersion->m_materialRevision == materialRevision &&
		m_currentVersion->m_shadowRevision == shadowRevision &&
		m_currentVersion->m_spatialRevision == spatialRevision)
	{
		auto result = m_currentVersion;
		m_lock.Unlock();
		return result;
	}

	auto version = RHISceneVersionPtr::Make();
	version->m_sceneRevision = m_revision;
	version->m_sceneIdentity = reinterpret_cast<uint64_t>(this);
	version->m_staticRevision = m_staticRevision;
	version->m_stationaryRevision = m_stationaryRevision;
	version->m_dynamicRevision = m_dynamicRevision;
	version->m_materialRevision = materialRevision;
	version->m_shadowRevision = shadowRevision;
	version->m_spatialRevision = spatialRevision;
	version->m_recordsRoot = BuildRecordRoot();
	version->m_staticRoot = version->m_recordsRoot;

	if (m_bHandleListsDirty || !m_currentVersion)
	{
		RebuildHandleLists(*version);
	}
	else
	{
		version->m_staticHandles = m_currentVersion->m_staticHandles;
		version->m_stationaryHandles = m_currentVersion->m_stationaryHandles;
		version->m_dynamicHandles = m_currentVersion->m_dynamicHandles;
	}

	if (m_currentVersion)
	{
		m_retainedVersions.Add(m_currentVersion);
	}
	m_currentVersion = version;
	m_currentRoot = version->m_recordsRoot;
	m_lastPublishedRevision = m_revision;
	m_bHandleListsDirty = false;
	for (uint32_t slotIndex : m_dirtySlots)
	{
		m_dirtySlotFlags[slotIndex] = 0u;
	}
	m_dirtySlots.Clear(false);
	m_lock.Unlock();
	return version;
}

RHISceneVersionPtr RHIScene::GetCurrentVersion() const
{
	m_lock.Lock();
	auto result = m_currentVersion;
	m_lock.Unlock();
	return result;
}

void RHIScene::RebuildFlight(
	RHISceneFlightState& flight,
	const RHISceneVersion& version)
{
	flight.m_stationaryHandles = version.m_stationaryHandles;
	flight.m_stationaryDirtyHandles.Clear(false);
	flight.m_bStationaryFullRebuild = true;
	++flight.m_metrics.m_numFullRebuilds;
}

RHISceneFlightStatePtr RHIScene::PrepareFlight(
	uint32_t flightSlot,
	RHISceneVersionPtr targetVersion)
{
	m_lock.Lock();
	if (!targetVersion)
	{
		targetVersion = m_currentVersion;
	}
	if (!targetVersion)
	{
		m_lock.Unlock();
		return {};
	}

	if (m_flights.Num() <= flightSlot)
	{
		const size_t previousNum = m_flights.Num();
		m_flights.Resize(flightSlot + 1u);
		for (size_t index = previousNum; index < m_flights.Num(); ++index)
		{
			m_flights[index] = RHISceneFlightStatePtr::Make();
			m_flights[index]->m_flightSlot = static_cast<uint32_t>(index);
		}
	}

	auto flight = m_flights[flightSlot];
	flight->m_metrics = {};
	flight->m_bStationaryFullRebuild = false;
	flight->m_stationaryDirtyHandles.Clear(false);
	const uint64_t firstJournalRevision = m_journal.IsEmpty() ?
		m_revision + 1ull : m_journal[0].m_revision;
	const bool bNeedsFullRebuild = flight->m_appliedRevision == 0ull ||
		flight->m_appliedRevision > targetVersion->m_sceneRevision ||
		flight->m_appliedRevision + 1ull < firstJournalRevision;

	if (bNeedsFullRebuild)
	{
		RebuildFlight(*flight, *targetVersion);
	}
	else if (flight->m_appliedRevision < targetVersion->m_sceneRevision)
	{
		auto& coalesced = flight->m_coalescedHandlesScratch;
		coalesced.Clear(false);
		if (flight->m_coalescedSlotFlags.Num() < m_slots.Num())
		{
			flight->m_coalescedSlotFlags.Resize(m_slots.Num());
		}
		uint32_t numJournalChanges = 0u;
		for (const auto& change : m_journal)
		{
			if (change.m_revision > flight->m_appliedRevision &&
				change.m_revision <= targetVersion->m_sceneRevision)
			{
				++numJournalChanges;
				if (flight->m_coalescedSlotFlags[change.m_handle.m_slot] == 0u)
				{
					flight->m_coalescedSlotFlags[change.m_handle.m_slot] = 1u;
					coalesced.Add(change.m_handle);
				}
			}
		}
		flight->m_metrics.m_numDirtyChanges = numJournalChanges;
		flight->m_metrics.m_numCoalescedChanges = static_cast<uint32_t>(coalesced.Num());
		flight->m_stationaryDirtyHandles.Reserve(coalesced.Num());
		for (const auto& handle : coalesced)
		{
			const RHISceneInstanceRecord* targetRecord = nullptr;
			const RHISceneInstanceRecord* previousRecord = nullptr;
			const bool bTouchesStationary =
				(targetVersion->Resolve(handle, targetRecord) && targetRecord &&
					targetRecord->m_mobility == EMobilityType::Stationary) ||
				(flight->m_appliedVersion &&
					flight->m_appliedVersion->Resolve(handle, previousRecord) &&
					previousRecord &&
					previousRecord->m_mobility == EMobilityType::Stationary);
			if (bTouchesStationary)
			{
				flight->m_stationaryDirtyHandles.Add(handle);
			}
			flight->m_coalescedSlotFlags[handle.m_slot] = 0u;
		}
		flight->m_metrics.m_copiedCpuBytes +=
			flight->m_stationaryDirtyHandles.Num() * sizeof(RenderInstanceHandle);
	}

	flight->m_stationaryHandles = targetVersion->m_stationaryHandles;
	flight->m_dynamicHandles = targetVersion->m_dynamicHandles;
	flight->m_appliedVersion = targetVersion;
	flight->m_appliedRevision = targetVersion->m_sceneRevision;
	m_metrics.m_numCoalescedChanges += flight->m_metrics.m_numCoalescedChanges;
	m_metrics.m_numFullRebuilds += flight->m_metrics.m_numFullRebuilds;
	m_metrics.m_copiedCpuBytes += flight->m_metrics.m_copiedCpuBytes;
	m_metrics.m_dynamicRewriteBytes += flight->m_metrics.m_dynamicRewriteBytes;
	m_lock.Unlock();
	return flight;
}

uint64_t RHIScene::MinimumRetainedRevision() const
{
	uint64_t result = m_currentVersion ? m_currentVersion->m_sceneRevision : m_revision;
	for (const auto& flight : m_flights)
	{
		if (flight && flight->m_appliedRevision > 0ull)
		{
			result = (std::min)(result, flight->m_appliedRevision);
		}
	}
	for (const auto& version : m_retainedVersions)
	{
		if (version && version.NumRefs() > 1u)
		{
			result = (std::min)(result, version->m_sceneRevision);
		}
	}
	return result;
}

void RHIScene::CollectGarbage()
{
	m_lock.Lock();
	const uint64_t minimumRetainedRevision = MinimumRetainedRevision();
	size_t firstRetainedJournalEntry = 0u;
	while (firstRetainedJournalEntry < m_journal.Num() &&
		m_journal[firstRetainedJournalEntry].m_revision <= minimumRetainedRevision)
	{
		++firstRetainedJournalEntry;
	}
	if (firstRetainedJournalEntry > 0u)
	{
		m_journal.RemoveAt(0u, firstRetainedJournalEntry);
	}

	size_t retiredWriteIndex = 0u;
	for (size_t retiredReadIndex = 0u;
		retiredReadIndex < m_retiredSlots.Num();
		++retiredReadIndex)
	{
		const uint32_t slotIndex = m_retiredSlots[retiredReadIndex];
		auto& slot = m_slots[slotIndex];
		if (minimumRetainedRevision < slot.m_retirementRevision)
		{
			m_retiredSlots[retiredWriteIndex++] = slotIndex;
			continue;
		}
		++slot.m_generation;
		if (slot.m_generation == 0u)
		{
			slot.m_generation = 1u;
		}
		slot.m_retirementRevision = 0ull;
		slot.m_record = {};
		m_reusableSlots.Add(slotIndex);
	}
	m_retiredSlots.Resize(retiredWriteIndex);

	size_t versionWriteIndex = 0u;
	for (size_t versionReadIndex = 0u;
		versionReadIndex < m_retainedVersions.Num();
		++versionReadIndex)
	{
		auto& version = m_retainedVersions[versionReadIndex];
		if (!version || version.NumRefs() == 1u)
		{
			continue;
		}
		if (versionWriteIndex != versionReadIndex)
		{
			m_retainedVersions[versionWriteIndex] = std::move(version);
		}
		++versionWriteIndex;
	}
	m_retainedVersions.Resize(versionWriteIndex);
	m_lock.Unlock();
}

uint64_t RHIScene::GetJournalFirstRevision() const
{
	m_lock.Lock();
	const uint64_t result = m_journal.IsEmpty() ? m_revision + 1ull : m_journal[0].m_revision;
	m_lock.Unlock();
	return result;
}
