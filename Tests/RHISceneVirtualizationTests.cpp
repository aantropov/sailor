#include "Support/TaskTestApp.h"
#include "Engine/World.h"
#include "FrameGraph/BlitFormatConversion.h"
#include "FrameGraph/FrameGraphNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "FrameGraph/RenderSceneNode.h"
#include "RHI/DebugContext.h"
#include "RHI/Material.h"
#include "RHI/RenderSubmission.h"
#include "RHI/Scene.h"
#include "RHI/SceneView.h"
#include "RHI/MotionHistory.h"
#include "RHI/PackedDraw.hpp"

#include <iostream>
#include <limits>
#include <atomic>
#include <barrier>
#include <thread>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace Sailor;
using namespace Sailor::Framegraph;
using namespace Sailor::RHI;

namespace
{
	static_assert(std::is_same_v<decltype(std::declval<RHISceneVersionPtr>().GetRawPtr()), const RHISceneVersion*>);
	static_assert(std::is_same_v<decltype(std::declval<RHISceneRecordRootPtr>().GetRawPtr()), const RHISceneRecordRoot*>);
	static_assert(std::is_same_v<decltype(std::declval<RHISceneRecordPagePtr>().GetRawPtr()), const RHISceneRecordPage*>);
	static_assert(std::is_same_v<decltype(std::declval<RHISceneVersionPtr>()->m_staticHandles.GetRawPtr()), const TVector<RenderInstanceHandle>*>);
	static_assert(std::is_same_v<decltype(std::declval<RHISceneInstanceRecord>().m_topology.GetRawPtr()), const RHIResource*>);
	static_assert(std::is_same_v<decltype(std::declval<RHISpatialSceneVersionPtr>().GetRawPtr()), const RHISpatialSceneVersion*>);
	static_assert(std::is_same_v<decltype(std::declval<RHISpatialSceneVersionPtr>()->m_staticOctree.GetRawPtr()), const RHISceneSpatialIndex*>);
	static_assert(std::is_same_v<decltype(std::declval<RHISceneViewSnapshot>().m_cpuLightsData.GetRawPtr()), const TVector<RHILightShaderData>*>);
	static_assert(std::is_same_v<decltype(std::declval<RHISceneViewSnapshot>().m_cpuBoneMatrices.GetRawPtr()), const TVector<glm::mat4>*>);
	static_assert(std::is_same_v<decltype(std::declval<RHISceneViewSnapshot>().m_sceneVersions.GetRawPtr()), const TVector<RHISceneVersionPtr>*>);
	static_assert(std::is_same_v<decltype(std::declval<RHISceneViewSnapshot>().m_previousMotionFrame.GetRawPtr()), const RHIMotionHistoryFrame*>);
	static_assert(std::is_same_v<decltype(std::declval<const RHISceneProxyResource&>().m_proxy.m_shadowCaster.GetRawPtr()), const RHIShadowCasterProxy*>);
	static_assert(std::is_same_v<decltype(std::declval<RHISceneProxyResourcePtr>().GetRawPtr()), const RHISceneProxyResource*>);
	static_assert(!std::is_default_constructible_v<RHIVisibleSceneProxy>);
	static_assert(!std::is_default_constructible_v<RHIVisibleShadowCaster>);

	class TestWorld final : public World
	{
	public:
		TestWorld() : World("RHISceneVirtualizationTests", 0u, {}) {}
	};

	void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
		}
	}

	RHISceneInstanceRecord MakeRecord(
		uint64_t producerKey,
		EMobilityType mobility,
		float translation = 0.0f)
	{
		RHISceneInstanceRecord record;
		record.m_producerKey = producerKey;
		record.m_mobility = mobility;
		record.m_worldMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(translation, 0.0f, 0.0f));
		record.m_worldBounds = Math::AABB(glm::vec3(translation), glm::vec3(1.0f));
		return record;
	}

	void TestGenerationalHandlesAndImmutableVersions()
	{
		auto scene = RHIScenePtr::Make(3u);
		const auto oldHandle = scene->AddInstance(MakeRecord(10ull, EMobilityType::Static));
		auto oldVersion = scene->PublishVersion();

		RHISceneInstanceRecord current;
		Require(scene->ResolveCurrent(oldHandle, current), "a live handle must resolve in the mutable scene");
		Require(current.m_producerKey == 10ull, "the resolved live record must preserve its payload");

		Require(scene->RemoveInstance(oldHandle), "a live handle must be removable");
		auto removedVersion = scene->PublishVersion();
		Require(!scene->ResolveCurrent(oldHandle, current), "a removed handle must be rejected by the mutable scene");

		const RHISceneInstanceRecord* retainedRecord = nullptr;
		Require(oldVersion->Resolve(oldHandle, retainedRecord) && retainedRecord,
			"an older retained version must still resolve a removed record");
		Require(!removedVersion->Resolve(oldHandle, retainedRecord),
			"the removal version must not resolve the removed record");

		for (uint32_t flight = 0u; flight < 3u; ++flight)
		{
			scene->PrepareFlight(flight, removedVersion);
		}
		oldVersion.Clear();
		scene->CollectGarbage();

		const auto reusedHandle = scene->AddInstance(MakeRecord(11ull, EMobilityType::Static));
		Require(reusedHandle.m_slot == oldHandle.m_slot,
			"a retired slot should be recycled once every referencing version and flight reaches its removal revision");
		Require(reusedHandle.m_generation != oldHandle.m_generation,
			"a recycled slot must advance its generation");
		Require(!scene->ResolveCurrent(oldHandle, current),
			"a stale generation must never resolve the recycled slot");

		auto advancedVersion = scene->PublishVersion();
		for (uint32_t flight = 0u; flight < 3u; ++flight)
		{
			scene->PrepareFlight(flight, advancedVersion);
		}
		removedVersion.Clear();
		scene->CollectGarbage();

		Require(scene->ResolveCurrent(reusedHandle, current),
			"collecting older versions must not invalidate a recycled live handle");
	}

	void TestBatchUpdatesPreserveVersionsAndFlights()
	{
		auto scene = RHIScenePtr::Make(2u);
		TVector<RenderInstanceHandle> handles;
		for (size_t i = 0u; i < 150u; ++i)
		{
			handles.Add(scene->AddInstance(MakeRecord(i, EMobilityType::Dynamic, float(i))));
		}
		auto before = scene->PublishVersion();
		scene->PrepareFlight(0u, before);
		TVector<RHISceneInstanceUpdate> updates;
		for (size_t i = 0u; i < handles.Num(); ++i)
		{
			const auto mobility = i % 3u == 0u ? EMobilityType::Stationary : EMobilityType::Dynamic;
			updates.Add({ handles[i], MakeRecord(i, mobility, float(i + 1000u)),
				ToMask(ESceneChangeBit::Transform) | ToMask(ESceneChangeBit::Bounds) |
				ToMask(ESceneChangeBit::Mobility) });
		}
		const uint64_t revision = scene->GetRevision();
		Require(scene->UpdateInstances(updates) == handles.Num(), "a batch must update every live handle");
		Require(scene->GetRevision() == revision + handles.Num(), "each changed handle must advance the scene revision");
		auto after = scene->PublishVersion();
		Require(after->m_stationaryHandles->Num() == 50u && after->m_dynamicHandles->Num() == 100u,
			"batch mobility changes must rebuild the published handle lists");
		for (size_t i = 0u; i < handles.Num(); ++i)
		{
			const RHISceneInstanceRecord* oldRecord = nullptr;
			const RHISceneInstanceRecord* newRecord = nullptr;
			Require(before->Resolve(handles[i], oldRecord) && after->Resolve(handles[i], newRecord),
				"both retained versions must resolve records across multiple copy-on-write pages");
			Require(oldRecord->m_worldMatrix[3].x == float(i) &&
				oldRecord->m_mobility == EMobilityType::Dynamic &&
				newRecord->m_worldMatrix[3].x == float(i + 1000u),
				"batch publication must preserve the old transform and mobility");
		}
		const auto flight = scene->PrepareFlight(0u, after);
		Require(flight->m_appliedVersion == after && flight->m_appliedRevision == after->m_sceneRevision,
			"a freed flight must retain the complete published batch");

		updates.Clear();
		auto stale = handles[0];
		++stale.m_generation;
		updates.Add({ stale, MakeRecord(999u, EMobilityType::Static), ToMask(ESceneChangeBit::Transform) });
		updates.Add({ stale, {}, ToMask(ESceneChangeBit::None) });
		updates.Add({ handles[1], {}, ToMask(ESceneChangeBit::None) });
		const auto unchangedRevision = scene->GetRevision();
		Require(scene->UpdateInstances(updates) == 1u && scene->GetRevision() == unchangedRevision,
			"stale generations and live no-ops must not replace records or advance the revision");
		Require(scene->PublishVersion() == after, "a no-op batch must reuse the published version");
		updates.Clear();
		Require(scene->UpdateInstances(updates) == 0u, "an empty batch must be a no-op");
	}

	void TestCopyOnWritePageSharing()
	{
		auto scene = RHIScenePtr::Make();
		TVector<RenderInstanceHandle> handles;
		for (uint32_t index = 0u; index <= RHISceneRecordPage::NumRecords; ++index)
		{
			handles.Add(scene->AddInstance(MakeRecord(index, EMobilityType::Static)));
		}

		auto first = scene->PublishVersion();
		Require(first->m_recordsRoot->m_pages.Num() == 2u,
			"records spanning the page boundary must create two COW pages");

		auto changed = MakeRecord(RHISceneRecordPage::NumRecords, EMobilityType::Static, 5.0f);
		Require(scene->UpdateInstance(
			handles[RHISceneRecordPage::NumRecords],
			changed,
			ToMask(ESceneChangeBit::Transform) | ToMask(ESceneChangeBit::Bounds)),
			"the record on the second page must update");
		auto second = scene->PublishVersion();

		Require(first->m_recordsRoot->m_pages[0] == second->m_recordsRoot->m_pages[0],
			"an untouched record page must be physically shared between versions");
		Require(first->m_recordsRoot->m_pages[1] != second->m_recordsRoot->m_pages[1],
			"a dirty record page must use copy-on-write");

		const RHISceneInstanceRecord* oldRecord = nullptr;
		const RHISceneInstanceRecord* newRecord = nullptr;
		const auto changedHandle = handles[RHISceneRecordPage::NumRecords];
		Require(first->Resolve(changedHandle, oldRecord) && oldRecord,
			"the old COW root must retain the original record");
		Require(second->Resolve(changedHandle, newRecord) && newRecord,
			"the new COW root must resolve the changed record");
		Require(oldRecord->m_worldMatrix[3].x == 0.0f && newRecord->m_worldMatrix[3].x == 5.0f,
			"copy-on-write must isolate record payloads between versions");

		auto materialOnlyVersion = scene->PublishVersion(1ull);
		Require(materialOnlyVersion->m_recordsRoot == second->m_recordsRoot,
			"publishing a version without record deltas must retain the immutable COW root");
	}

	void TestConcurrentPublishedSceneReaders()
	{
		auto scene = RHIScenePtr::Make();
		const auto makeRecord = [](uint32_t value)
		{
			auto record = MakeRecord(42u, EMobilityType::Dynamic, float(value));
			record.m_topologyRevision = value;
			record.m_skeletonOffset = value + 11u;
			RHISceneViewProxy proxy;
			auto shadow = TSharedPtr<RHIShadowCasterProxy>::Make();
			RHIShadowMeshProxy mesh;
			mesh.m_localMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(2, 3, 4));
			mesh.m_baseColorFactor = glm::vec4(float(value));
			shadow->m_meshes.Add(std::move(mesh));
			proxy.m_shadowCaster = std::move(shadow);
			record.m_topology = RHISceneProxyResourcePtr::Make(std::move(proxy));
			return record;
		};
		const auto handle = scene->AddInstance(makeRecord(0u));
		for (uint32_t i = 0; i < RHISceneRecordPage::NumRecords; ++i)
		{
			scene->AddInstance(MakeRecord(i + 100u, EMobilityType::Static));
		}
		const auto first = scene->PublishVersion();
		const auto firstPage = first->m_recordsRoot->m_pages[0];
		const auto firstShadow = firstPage->m_slots[handle.m_slot].m_record.m_topology.StaticCast<const RHISceneProxyResource>()->m_proxy.m_shadowCaster;
		const auto handles = first->m_dynamicHandles;
		std::barrier step(2);
		std::atomic<bool> bValid = true;
		constexpr uint32_t publications = 128;
		std::thread reader([&]()
		{
			for (uint32_t i = 0; i < publications; ++i)
			{
				step.arrive_and_wait();
				const auto current = scene->GetCurrentVersion();
				const RHISceneInstanceRecord* oldRecord = nullptr;
				const RHISceneInstanceRecord* newRecord = nullptr;
				if (!first->Resolve(handle, oldRecord) || !current->Resolve(handle, newRecord) ||
					oldRecord != &firstPage->m_slots[handle.m_slot].m_record || oldRecord->m_worldMatrix[3].x != 0.0f ||
					oldRecord->m_skeletonOffset != 11u || oldRecord->m_topologyRevision != 0u ||
					newRecord->m_worldMatrix[3].x != float(newRecord->m_topologyRevision) ||
					newRecord->m_skeletonOffset != newRecord->m_topologyRevision + 11u ||
					current->m_dynamicHandles != handles || handles->Num() != 1u || (*handles)[0] != handle ||
					current->m_recordsRoot->m_pages[1] != first->m_recordsRoot->m_pages[1])
				{
					bValid = false;
				}
				if (newRecord)
				{
					const auto topology = newRecord->m_topology.DynamicCast<const RHISceneProxyResource>();
					if (!topology ||
						!topology->m_proxy.m_shadowCaster ||
						topology->m_proxy.m_shadowCaster->m_meshes[0].m_baseColorFactor != glm::vec4(float(newRecord->m_topologyRevision)) ||
						topology->m_proxy.m_shadowCaster->m_meshes[0].m_localMatrix != glm::translate(glm::mat4(1.0f), glm::vec3(2, 3, 4))) bValid = false;
				}
				if (oldRecord && (oldRecord->m_topology.StaticCast<const RHISceneProxyResource>()->m_proxy.m_shadowCaster != firstShadow ||
					firstShadow->m_meshes[0].m_baseColorFactor != glm::vec4(0.0f) ||
					firstShadow->m_meshes[0].m_localMatrix != glm::translate(glm::mat4(1.0f), glm::vec3(2, 3, 4)))) bValid = false;
				step.arrive_and_wait();
			}
		});
		for (uint32_t value = 1; value <= publications; ++value)
		{
			step.arrive_and_wait();
			if (!scene->UpdateInstance(handle, makeRecord(value),
				ToMask(ESceneChangeBit::Transform) | ToMask(ESceneChangeBit::Bounds) |
				ToMask(ESceneChangeBit::MeshOrLodTopology) | ToMask(ESceneChangeBit::SkeletonOffset))) bValid = false;
			scene->PublishVersion();
			step.arrive_and_wait();
		}
		reader.join();
		Require(bValid, "concurrent publication must retain old records and reuse unchanged pages/lists while publishing coherent topology and transforms");
		const auto latest = scene->GetCurrentVersion();
		const RHISceneInstanceRecord* current = nullptr;
		Require(latest->Resolve(handle, current) && current->m_topologyRevision == publications &&
			latest->m_recordsRoot->m_pages[0] != firstPage,
			"the last publication must contain the new record in a distinct read-only page");
	}

	void TestShadowLocalTransformPublication()
	{
		RHISceneViewProxy source;
		auto shadow = TSharedPtr<RHIShadowCasterProxy>::Make();
		shadow->m_meshes.Add(RHIShadowMeshProxy{});
		source.m_shadowCaster = std::move(shadow);
		const auto first = RHISceneProxyResourcePtr::Make(source);
		shadow = TSharedPtr<RHIShadowCasterProxy>::Make(*source.m_shadowCaster);
		shadow->m_meshes[0].m_localMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(2, 3, 4));
		source.m_shadowCaster = std::move(shadow);
		const auto changed = RHISceneProxyResourcePtr::Make(std::move(source));
		Require(first->m_geometryRevision == changed->m_geometryRevision && first->m_mainRevision == changed->m_mainRevision &&
			first->m_shadowRevision != changed->m_shadowRevision &&
			first->m_proxy.m_shadowCaster->m_meshes[0].m_localMatrix == glm::mat4(1.0f),
			"a shadow-only local transform change must invalidate shadow packets without changing retained topology or main revisions");
	}

	void TestRenderedMotionHistoryReleasesOldScenePages()
	{
		auto scene = RHIScenePtr::Make(3u);
		const auto handle = scene->AddInstance(MakeRecord(1u, EMobilityType::Dynamic));
		RHISceneViewSnapshot snapshot;
		TSharedPtr<RHIMotionHistoryFrame> history;
		TVector<TWeakPtr<const RHISceneRecordPage>> oldPages;
		TVector<TWeakPtr<RHIMotionHistoryFrame>> oldHistory;
		for (uint32_t frame = 0u; frame < 256u; ++frame)
		{
			scene->UpdateInstance(handle, MakeRecord(1u, EMobilityType::Dynamic, static_cast<float>(frame)),
				ToMask(ESceneChangeBit::Transform));
			auto version = scene->PublishVersion();
			oldPages.Add(version->m_recordsRoot->m_pages[0]);
			snapshot.ResetForReuse();
			snapshot.m_previousMotionFrame = history;
			snapshot.m_sceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make(
				TVector<RHISceneVersionPtr>{ version });
			history = TSharedPtr<RHIMotionHistoryFrame>::Make();
			history->m_sceneVersions = snapshot.m_sceneVersions;
			oldHistory.Add(history);
			scene->PrepareFlight(frame % 3u, version);
			scene->CollectGarbage();
			if (frame >= 8u)
			{
				Require(!oldHistory[frame - 8u], "recycled snapshots must release older history frames");
				Require(!oldPages[frame - 8u],
					"submitted motion history and rotating flights must not retain older scene pages");
			}
		}
	}

	void TestTwoAndThreeFlightVersionRetention()
	{
		for (uint32_t numFlights : { 2u, 3u })
		{
			auto scene = RHIScenePtr::Make(numFlights);
			const auto stationary = scene->AddInstance(MakeRecord(1ull, EMobilityType::Stationary, 1.0f));
			const auto dynamic = scene->AddInstance(MakeRecord(2ull, EMobilityType::Dynamic, 2.0f));
			auto first = scene->PublishVersion();

			auto flight0 = scene->PrepareFlight(0u, first);
			auto flight1 = scene->PrepareFlight(1u, first);
			auto flight2 = numFlights == 3u ? scene->PrepareFlight(2u, first) : flight1;
			const RHISceneInstanceRecord* flight0Stationary = nullptr;
			Require(flight0->m_appliedVersion->Resolve(stationary, flight0Stationary) &&
				flight0Stationary->m_worldMatrix[3].x == 1.0f,
				"the first free flight must retain the immutable stationary version without copying complete records");
			Require(flight0->m_appliedVersion->m_dynamicHandles->Num() == 1u &&
				flight1->m_appliedVersion->m_dynamicHandles->Num() == 1u &&
				flight2->m_appliedVersion->m_dynamicHandles->Num() == 1u,
				"every flight must reference its version's dynamic handle list");

			Require(scene->UpdateInstance(
				stationary,
				MakeRecord(1ull, EMobilityType::Stationary, 10.0f),
				ToMask(ESceneChangeBit::Transform) | ToMask(ESceneChangeBit::Bounds)),
				"the stationary record must update");
			Require(scene->UpdateInstance(
				dynamic,
				MakeRecord(2ull, EMobilityType::Dynamic, 20.0f),
				ToMask(ESceneChangeBit::Transform) | ToMask(ESceneChangeBit::Bounds)),
				"the dynamic record must update");
			auto second = scene->PublishVersion();

			scene->PrepareFlight(0u, second);
			const RHISceneInstanceRecord* updatedStationary = nullptr;
			const RHISceneInstanceRecord* retainedStationary1 = nullptr;
			const RHISceneInstanceRecord* retainedStationary2 = nullptr;
			const RHISceneInstanceRecord* updatedDynamic = nullptr;
			Require(flight0->m_appliedVersion->Resolve(stationary, updatedStationary) &&
				updatedStationary->m_worldMatrix[3].x == 10.0f,
				"a freed flight must advance to the latest stationary delta by handle");
			Require(flight1->m_appliedVersion->Resolve(stationary, retainedStationary1) &&
				flight2->m_appliedVersion->Resolve(stationary, retainedStationary2) &&
				retainedStationary1->m_worldMatrix[3].x == 1.0f &&
				retainedStationary2->m_worldMatrix[3].x == 1.0f,
				"active flights must retain their previous immutable stationary version");
			Require(flight0->m_appliedVersion->Resolve(dynamic, updatedDynamic) &&
				updatedDynamic->m_worldMatrix[3].x == 20.0f,
				"the reused flight must resolve dynamic state without a duplicate record array");

			scene->PrepareFlight(1u, second);
			if (numFlights == 3u) scene->PrepareFlight(2u, second);
			Require(flight1->m_appliedVersion == second &&
				flight2->m_appliedVersion == second,
				"two and three-flight versions must converge only when each slot is reused");
		}
	}

	void TestFlightRetirementWithMotionHistory()
	{
		for (uint32_t numFlights : { 2u, 3u })
		{
			auto scene = RHIScenePtr::Make(numFlights);
			Require(!scene->PrepareFlight(0u, {}), "an unpublished scene has no flight version");
			const auto removed = scene->AddInstance(MakeRecord(1u, EMobilityType::Stationary, 1.0f));
			const auto moving = scene->AddInstance(MakeRecord(2u, EMobilityType::Dynamic, 2.0f));
			auto original = scene->PublishVersion();
			for (uint32_t slot = 0u; slot < numFlights; ++slot) scene->PrepareFlight(slot, original);
			auto history = TSharedPtr<RHIMotionHistoryFrame>::Make();
			history->m_sceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make(
				TVector<RHISceneVersionPtr>{ original });
			original.Clear();
			Require(scene->RemoveInstance(removed), "the retained stationary instance must be removable");
			for (uint32_t i = 0u; i < 100u; ++i)
			{
				Require(scene->UpdateInstance(moving, MakeRecord(2u, EMobilityType::Dynamic, float(i + 3u)),
					ToMask(ESceneChangeBit::Transform)), "unpublished dynamic changes must succeed");
			}
			auto latest = scene->PublishVersion();
			for (uint32_t slot = 0u; slot + 1u < numFlights; ++slot) scene->PrepareFlight(slot, latest);
			scene->CollectGarbage();
			const auto whileInFlight = scene->AddInstance(MakeRecord(3u, EMobilityType::Static));
			Require(whileInFlight.m_slot != removed.m_slot,
				"an older flight must prevent a removed slot from being reused");
			const auto lastFlight = scene->PrepareFlight(numFlights - 1u, latest);
			scene->CollectGarbage();
			const auto whileInHistory = scene->AddInstance(MakeRecord(4u, EMobilityType::Static));
			Require(whileInHistory.m_slot != removed.m_slot,
				"motion history must retain the old slot after every flight has advanced");
			const RHISceneInstanceRecord* record = nullptr;
			Require((*history->m_sceneVersions)[0]->Resolve(removed, record) && record->m_producerKey == 1u &&
				record->m_worldMatrix[3].x == 1.0f, "motion history must still resolve the removed owner");
			Require(!lastFlight->m_appliedVersion->Resolve(removed, record) &&
				lastFlight->m_appliedVersion->Resolve(moving, record) && record->m_worldMatrix[3].x == 102.0f,
				"the new flight must contain removal and the last unpublished update");
			history.Clear();
			scene->CollectGarbage();
			const auto reused = scene->AddInstance(MakeRecord(5u, EMobilityType::Stationary));
			Require(reused.m_slot == removed.m_slot && reused.m_generation != removed.m_generation,
				"the exact retired slot becomes reusable only after its last retained version is released");
			auto current = scene->PublishVersion();
			Require(!current->Resolve(removed, record) && current->Resolve(reused, record) && record->m_producerKey == 5u,
				"the recycled generation must not revive the old handle");
			Require(scene->PrepareFlight(numFlights + 1u, {})->m_appliedVersion == current,
				"a newly added flight must use the current version when no explicit target is supplied");
		}
	}

	void TestRangeRetirementAndDirtyCoalescing()
	{
		RHISceneRangeAllocator allocator;
		const SceneRangeHandle first = allocator.Allocate(3u);
		PhysicalAllocation firstAllocation;
		Require(allocator.Resolve(first, firstAllocation) && firstAllocation.m_capacity == 4u,
			"range allocation should grow geometrically");
		Require(allocator.Retire(first, 4ull), "a live range must enter deferred retirement");

		const SceneRangeHandle second = allocator.Allocate(3u);
		Require(second.m_slot != first.m_slot,
			"a range must not be recycled before its retirement revision completes");
		allocator.Collect(4ull);
		const SceneRangeHandle recycled = allocator.Allocate(3u);
		Require(recycled.m_slot == first.m_slot && recycled.m_generation != first.m_generation,
			"a completed range retirement must recycle the slot with a new generation");

		const auto ranges = RHISceneRangeAllocator::CoalesceDirtyRanges({
			{ 8u, 2u }, { 0u, 4u }, { 3u, 5u }, { 20u, 0u }, { 12u, 2u } });
		Require(ranges.Num() == 2u && ranges[0].m_offset == 0u && ranges[0].m_count == 10u &&
			ranges[1].m_offset == 12u && ranges[1].m_count == 2u,
			"overlapping and adjacent dirty ranges must be coalesced before upload");
	}

	void TestRangeCapacityBoundary()
	{
		constexpr auto limit = (std::numeric_limits<uint32_t>::max)();
		RHISceneRangeAllocator allocator;
		Require(!allocator.Allocate(0).IsValid() && allocator.GetCapacity() == 0,
			"empty allocations must not reserve capacity");
		const auto large = allocator.Allocate(limit - 1);
		PhysicalAllocation original;
		Require(allocator.Resolve(large, original) && original.m_offset == 0 && original.m_capacity == limit - 1,
			"the range metadata must represent capacities above the last power of two");
		const auto last = allocator.Allocate(1);
		PhysicalAllocation tail;
		Require(allocator.Resolve(last, tail) && tail.m_offset == limit - 1 && allocator.GetCapacity() == limit,
			"the final representable element must remain allocatable");
		const auto generation = allocator.GetBufferGeneration();
		Require(!allocator.Allocate(1).IsValid(), "range allocation must reject an unrepresentable end instead of wrapping");
		Require(allocator.GetCapacity() == limit && allocator.GetBufferGeneration() == generation,
			"failed growth must leave capacity and buffer generation unchanged");
		PhysicalAllocation retained;
		Require(allocator.Resolve(large, retained) && retained.m_bufferGeneration == original.m_bufferGeneration &&
			retained.m_offset == original.m_offset && retained.m_count == original.m_count && retained.m_capacity == original.m_capacity,
			"growth and failed allocation must preserve older physical allocations");
		Require(allocator.Retire(last, 3), "the last range must retire normally at capacity");
		allocator.Collect(3);
		const auto reused = allocator.Allocate(1);
		Require(allocator.Resolve(reused, retained) && reused.m_slot == last.m_slot &&
			reused.m_generation != last.m_generation && retained.m_offset == tail.m_offset &&
			retained.m_bufferGeneration == tail.m_bufferGeneration && !allocator.Resolve(last, tail),
			"a free range remains reusable at maximum capacity without reviving its old handle");
		std::cout << "Scene ranges: uint32 capacity, failed growth and retained generations passed\n";
	}

	void TestRangePhysicalReuse()
	{
		RHISceneRangeAllocator allocator;
		const auto older = allocator.Allocate(5);
		PhysicalAllocation original;
		Require(allocator.Resolve(older, original), "the first buffer generation must resolve");
		const auto first = allocator.Allocate(1);
		const auto second = allocator.Allocate(2);
		const auto third = allocator.Allocate(2);
		PhysicalAllocation firstRange, secondRange, thirdRange;
		Require(allocator.Resolve(first, firstRange) && allocator.Resolve(second, secondRange) &&
			allocator.Resolve(third, thirdRange) && firstRange.m_bufferGeneration != original.m_bufferGeneration,
			"growth must keep allocations from different buffer generations distinguishable");
		Require(allocator.Retire(first, 5) && allocator.Retire(second, 7) && !allocator.Retire(second, 7),
			"a range may enter deferred retirement only once");
		allocator.Collect(4);
		const auto pending = allocator.Allocate(1);
		PhysicalAllocation pendingRange;
		Require(allocator.Resolve(pending, pendingRange) && pendingRange.m_offset >= thirdRange.m_offset + thirdRange.m_capacity,
			"unfinished retired ranges must not be physically reused");
		allocator.Collect(5);
		const auto reused = allocator.Allocate(1);
		PhysicalAllocation reuse;
		Require(allocator.Resolve(reused, reuse) && reuse.m_offset == firstRange.m_offset &&
			reuse.m_bufferGeneration == firstRange.m_bufferGeneration && reused.m_slot == first.m_slot &&
			reused.m_generation != first.m_generation,
			"completed retirement must reuse its physical offset with a fresh logical generation");
		allocator.Collect(7);
		const auto splitA = allocator.Allocate(1);
		const auto splitB = allocator.Allocate(1);
		PhysicalAllocation a, b;
		Require(allocator.Resolve(splitA, a) && allocator.Resolve(splitB, b) &&
			a.m_offset == secondRange.m_offset && b.m_offset == a.m_offset + 1,
			"a free range must split into adjacent nonoverlapping allocations");
		Require(allocator.Retire(splitA, 8) && allocator.Retire(splitB, 8), "split ranges must retire");
		allocator.Collect(8);
		const auto merged = allocator.Allocate(2);
		PhysicalAllocation mergedRange;
		Require(allocator.Resolve(merged, mergedRange) && mergedRange.m_offset == secondRange.m_offset &&
			mergedRange.m_capacity == 2 && mergedRange.m_bufferGeneration == secondRange.m_bufferGeneration,
			"adjacent free ranges must merge and retain their original buffer generation");
		Require(allocator.Resolve(older, reuse) && reuse.m_bufferGeneration == original.m_bufferGeneration &&
			reuse.m_offset == original.m_offset && reuse.m_capacity == original.m_capacity,
			"recycling newer ranges must not change an older live allocation");
		std::cout << "Scene ranges: deferred physical reuse, splitting and coalescing passed\n";
	}

	void TestDirtyRangeCountBoundary()
	{
		constexpr auto limit = (std::numeric_limits<uint32_t>::max)();
		const auto ranges = RHISceneRangeAllocator::CoalesceDirtyRanges({ { limit, 1 }, { 0, limit } });
		Require(ranges.Num() == 2 && ranges[0].m_offset == 0 && ranges[0].m_count == limit &&
			ranges[1].m_offset == limit && ranges[1].m_count == 1,
			"a union too large for one uint32 count must retain both dirty ranges");
		const auto overlapping = RHISceneRangeAllocator::CoalesceDirtyRanges({ { 2, limit }, { 0, limit } });
		Require(overlapping.Num() == 2 && overlapping[0].m_count == limit && overlapping[1].m_count == limit,
			"an unrepresentable overlapping union must not wrap or lose its tail");
		const auto representable = RHISceneRangeAllocator::CoalesceDirtyRanges({ { 0, limit - 1 }, { limit - 1, 1 } });
		Require(representable.Num() == 1 && representable[0].m_offset == 0 && representable[0].m_count == limit,
			"a representable boundary union must still coalesce");
		std::cout << "Scene dirty ranges: representable and oversized unions passed\n";
	}

	void TestSubmissionSemaphoreRetention()
	{
		auto context = RHIRenderSubmissionContextPtr::Make();
		auto semaphore = RHISemaphorePtr::Make();
		Require(semaphore.NumRefs() == 1, "the semaphore fixture must start with one owner");
		context->SetResourceReadySemaphore(semaphore);
		Require(semaphore.NumRefs() == 2, "submission context must retain the resource-ready semaphore");
		context->BeginSubmission(1, 0);
		Require(semaphore.NumRefs() == 1, "a completed flight's next submission must release its old semaphore");
		context->SetResourceReadySemaphore(semaphore);
		context->InvalidateSubmissionResources();
		Require(semaphore.NumRefs() == 1, "invalidated submission resources must release the semaphore");
		context->SetResourceReadySemaphore(semaphore);
		context.Clear();
		Require(semaphore.NumRefs() == 1, "context destruction must release its retained semaphore");
		std::cout << "Submission semaphore: retention, reuse, invalidation and destruction passed\n";
	}

	void TestImmutableMaterialBindingVersions()
	{
		auto material = RHIMaterialPtr::Make(RenderState{}, RHIShaderPtr{}, RHIShaderPtr{});
		auto firstBindings = RHIShaderBindingSetPtr::Make();
		auto secondBindings = RHIShaderBindingSetPtr::Make();
		material->SetBindings(firstBindings);
		auto firstVersion = material->GetVersion();
		material->SetBindings(secondBindings);
		auto secondVersion = material->GetVersion();

		Require(firstVersion && secondVersion && firstVersion != secondVersion,
			"publishing material bindings must create a new immutable version");
		Require(firstVersion->GetBindings() == firstBindings && secondVersion->GetBindings() == secondBindings,
			"an older material version must retain its original binding allocation");
		Require(firstVersion->GetVersionId() < secondVersion->GetVersionId(),
			"material version identifiers must increase monotonically");
	}

	void TestLocalizedShadowVersionDiff()
	{
		auto scene = RHIScenePtr::Make();
		auto casterRecord = MakeRecord(20ull, EMobilityType::Static);
		casterRecord.m_renderFlags = 1u;
		const auto caster = scene->AddInstance(casterRecord);
		const auto nonCaster = scene->AddInstance(
			MakeRecord(21ull, EMobilityType::Static));
		auto first = scene->PublishVersion();

		Require(scene->UpdateInstance(
			nonCaster,
			MakeRecord(21ull, EMobilityType::Static, 4.0f),
			ToMask(ESceneChangeBit::Transform) | ToMask(ESceneChangeBit::Bounds)),
			"a non-caster update must publish successfully");
		auto nonShadowChange = scene->PublishVersion();
		const Math::Frustum shadowFrustum(glm::mat4(1.0f));
		Require(!nonShadowChange->HasShadowChangesIntersecting(*first, shadowFrustum),
			"a changed COW page must not invalidate a shadow view when only non-casters changed");

		auto movedCaster = MakeRecord(20ull, EMobilityType::Static, 0.25f);
		movedCaster.m_renderFlags = 1u;
		Require(scene->UpdateInstance(
			caster,
			movedCaster,
			ToMask(ESceneChangeBit::Transform) |
				ToMask(ESceneChangeBit::Bounds) |
				ToMask(ESceneChangeBit::ShadowState)),
			"a caster update must publish successfully");
		auto shadowChange = scene->PublishVersion();
		Require(shadowChange->HasShadowChangesIntersecting(
			*nonShadowChange,
			shadowFrustum),
			"a caster change intersecting the light frustum must invalidate that shadow view");
	}

	void TestReflectionProbeAssignmentPublication()
	{
		static_assert(offsetof(RenderSceneNode::PerInstanceData, reflectionProbeIndex) == 92u);
		static_assert(sizeof(RenderSceneNode::PerInstanceData) == 192u);
		static_assert(std::is_same_v<decltype(std::declval<RHISceneReflectionsSnapshotPtr>().GetRawPtr()),
			const RHISceneReflectionsSnapshot*>);

		auto scene = RHIScenePtr::Make(3u);
		auto record = MakeRecord(230u, EMobilityType::Static);
		record.m_topology = RHISceneProxyResourcePtr::Make();
		record.m_renderFlags = 1u;
		record.m_reflectionProbeIndex = 2u;
		const auto handle = scene->AddInstance(record);
		auto original = scene->PublishVersion();
		auto firstFlight = scene->PrepareFlight(0u, original);
		auto secondFlight = scene->PrepareFlight(1u, original);

		record.m_reflectionProbeIndex = 7u;
		Require(scene->UpdateInstance(handle, record, ToMask(ESceneChangeBit::ReflectionProbe)),
			"a static mesh must accept a probe-only update");
		auto changed = scene->PublishVersion();
		scene->PrepareFlight(0u, changed);
		const RHISceneInstanceRecord* retained = nullptr;
		const RHISceneInstanceRecord* updated = nullptr;
		Require(secondFlight->m_appliedVersion->Resolve(handle, retained) &&
			firstFlight->m_appliedVersion->Resolve(handle, updated) &&
			retained->m_reflectionProbeIndex == 2u && updated->m_reflectionProbeIndex == 7u,
			"a new probe assignment must not overwrite an older flight's instance data");
		Require(changed->m_staticRevision > original->m_staticRevision &&
			!changed->HasShadowChangesIntersecting(*original, Math::Frustum(glm::mat4(1.0f))),
			"a probe-only edit must invalidate static draw payloads without invalidating shadow casters");

		RHIPackedDrawSceneState previous;
		previous.m_sceneVersions = TSharedPtr<const TVector<RHISceneVersionPtr>>::Make(
			TVector<RHISceneVersionPtr>{ original });
		RHIPackedDrawSceneState current;
		current.m_sceneVersions = TSharedPtr<const TVector<RHISceneVersionPtr>>::Make(
			TVector<RHISceneVersionPtr>{ changed });
		RHIPackedDrawSceneChanges changes;
		changes.Gather(current, &previous, EMobilityType::Static);
		Require(changes.m_removed.IsEmpty() && changes.m_updated.Num() == 1u &&
			changes.m_updated[0].GetReflectionProbeIndex() == 7u &&
			changes.m_updated[0].m_record->m_topology == record.m_topology,
			"packed draw ranges must detect changed probe indices while retaining mesh topology");

		RenderSceneNode::PerInstanceData first{};
		RenderSceneNode::PerInstanceData second{};
		first.model = second.model = glm::mat4(1.0f);
		first.sphereBounds = second.sphereBounds = glm::vec4(0.0f);
		first.reflectionProbeIndex = 2u;
		second.reflectionProbeIndex = 7u;
		Require(!(first == second),
			"probe indices must participate in immutable per-instance payload comparison");
		TPackedDrawPacket<RenderSceneNode::PerInstanceData> packet;
		auto mesh = RHIMeshPtr::Make();
		RHIBatch sharedBatch;
		packet.Add(sharedBatch, mesh, first);
		packet.Add(sharedBatch, mesh, second);
		packet.Finalize(false);
		Require(packet.m_groups.Num() == 1u && packet.m_groups[0].m_numInstances == 2u,
			"different probe assignments must remain in the same instanced material batch");
	}

	void TestSubmissionCompletionToken()
	{
		auto successful = RHISubmissionCompletionTokenPtr::Make();
		Require(successful->IsPending() && !successful->IsSuccessful(),
			"a newly prepared resource version must not be reusable before submission");
		successful->Complete(true);
		Require(!successful->IsPending() && successful->IsSuccessful(),
			"a successfully submitted resource version must become reusable");
		successful->Reset();
		Require(successful->IsPending() && !successful->IsSuccessful(),
			"a freed flight slot must reset and reuse its completion token");

		auto failed = RHISubmissionCompletionTokenPtr::Make();
		failed->Complete(false);
		Require(!failed->IsPending() && !failed->IsSuccessful(),
			"a failed submission must never publish its prepared resource version");
	}

	void TestSubmissionContextSurvivesSnapshotPreparation()
	{
		TestWorld world;
		RHISceneView view;
		view.m_world = &world;

		CameraData camera;
		camera.SetAspect(1.0f);
		camera.SetFov(60.0f);
		view.m_cameras.Add(camera);
		view.m_cameraTransforms.Add(Math::Transform{});
		view.m_shadowMapsToUpdate.Resize(1u);
		view.m_shadowMapsToBlit.Resize(1u);
		view.m_shadowIndices.Resize(1u);
		view.m_shadowAtlasTiles.Resize(1u);
		view.m_shadowMatrices.Resize(1u);

		auto context = RHIRenderSubmissionContextPtr::Make();
		context->BeginSubmission(11ull, 1u);
		view.SetSubmissionContext(context);
		view.m_renderMode = ESceneViewRenderMode::Cascades;
		auto firstGlobalIllumination =
			RHIGlobalIlluminationSnapshotPtr::Make();
		firstGlobalIllumination->m_generation = 41u;
		view.m_globalIlluminationMode = EGlobalIlluminationMode::Baked;
		view.m_bGlobalIlluminationEnabled = false;
		view.m_globalIllumination = firstGlobalIllumination;
		view.PrepareSnapshots();

		Require(view.m_snapshots.Num() == 1u &&
			view.m_snapshots[0].m_submissionContext == context &&
			view.m_snapshots[0].m_renderMode == ESceneViewRenderMode::Cascades &&
			view.m_snapshots[0].m_globalIlluminationMode ==
				EGlobalIlluminationMode::Baked &&
			!view.m_snapshots[0].m_bGlobalIlluminationEnabled,
			"preparing a camera snapshot must retain the acquired flight submission context");
		view.m_renderMode = ESceneViewRenderMode::AmbientOcclusion;
		view.m_globalIlluminationMode = EGlobalIlluminationMode::NoGI;
		view.m_bGlobalIlluminationEnabled = true;
		Require(
			view.m_snapshots[0].m_renderMode == ESceneViewRenderMode::Cascades &&
			view.m_snapshots[0].m_globalIlluminationMode ==
				EGlobalIlluminationMode::Baked &&
			!view.m_snapshots[0].m_bGlobalIlluminationEnabled,
			"a prepared frame snapshot must keep its render and GI modes when the next frame changes");
		auto secondGlobalIllumination =
			RHIGlobalIlluminationSnapshotPtr::Make();
		secondGlobalIllumination->m_generation = 42u;
		view.m_globalIllumination = secondGlobalIllumination;
		Require(
			view.m_snapshots[0].m_globalIllumination == firstGlobalIllumination &&
			view.m_snapshots[0].m_globalIllumination->m_generation == 41u,
			"an in-flight camera snapshot must retain its exact immutable GI revision");

		Require(
			std::string(GetSceneViewRenderModeShaderDefine(
				ESceneViewRenderMode::AmbientOcclusion)) == "AO" &&
			std::string(GetSceneViewRenderModeShaderDefine(
				ESceneViewRenderMode::Cascades)) == "CASCADES" &&
			std::string(GetSceneViewRenderModeShaderDefine(
				ESceneViewRenderMode::LightTiles)) == "LIGHT_TILES" &&
			std::string(GetSceneViewRenderModeShaderDefine(
				ESceneViewRenderMode::Lit)).empty() &&
			std::string(GetSceneViewRenderModeShaderDefine(
				ESceneViewRenderMode::GlobalIlluminationVisibility)).empty(),
			"Scene View modes must resolve to immutable variants or GI runtime metadata");
		Require(
			!IsSceneViewDebugVisualization(ESceneViewRenderMode::Lit) &&
			IsSceneViewDebugVisualization(
				ESceneViewRenderMode::AmbientOcclusion) &&
			IsSceneViewDebugVisualization(
				ESceneViewRenderMode::GlobalIlluminationOnly) &&
			IsSceneViewDebugVisualization(
				ESceneViewRenderMode::GlobalIlluminationFallback) &&
			IsSceneViewDebugVisualization(
				ESceneViewRenderMode::GlobalIlluminationSubdivisions),
			"CPU path-traced Lit composition must yield to every Scene View debug visualization");
	}

	RHISceneInstanceRecord MakeVisibleRecord(uint64_t producerKey, const glm::vec3& position,
		float maxDistance = (std::numeric_limits<float>::max)())
	{
		auto record = MakeRecord(producerKey, EMobilityType::Static);
		record.m_worldMatrix = glm::translate(glm::mat4(1.0f), position);
		record.m_worldBounds = Math::AABB(position, glm::vec3(0.5f));
		RHISceneViewProxy proxy;
		proxy.m_lodPolicy.m_maxCameraDistance = maxDistance;
		record.m_topology = RHISceneProxyResourcePtr::Make(std::move(proxy));
		return record;
	}

	RHISpatialSceneVersionPtr PublishVisibleScene(RHIScenePtr& scene)
	{
		auto spatial = TSharedPtr<RHISpatialSceneVersion>::Make();
		spatial->m_scene = scene;
		spatial->m_sceneVersion = scene->PublishVersion();
		spatial->m_revision = spatial->m_sceneVersion->m_sceneRevision;
		for (const auto mobility : { EMobilityType::Static, EMobilityType::Stationary, EMobilityType::Dynamic })
		{
			auto index = TSharedPtr<RHISceneSpatialIndex>::Make(glm::ivec3(0), 512u, 1u);
			index->UpdatePartition(*spatial->m_sceneVersion->m_recordsRoot, nullptr, mobility, 0u);
			if (mobility == EMobilityType::Static)
			{
				spatial->m_staticOctree = std::move(index);
			}
			else if (mobility == EMobilityType::Stationary)
			{
				spatial->m_stationaryOctree = std::move(index);
			}
			else
			{
				spatial->m_dynamicOctree = std::move(index);
			}
		}
		return spatial;
	}

	void PrepareVisibleView(RHISceneView& view, TestWorld& world,
		const TVector<RHISpatialSceneVersionPtr>& versions, const CameraData& camera,
		const TVector<Math::Transform>& transforms = { Math::Transform{} })
	{
		view.Clear();
		view.m_world = &world;
		for (const auto& version : versions)
		{
			view.AddSceneVersion(version);
		}
		for (const auto& transform : transforms)
		{
			view.m_cameras.Add(camera);
			view.m_cameraTransforms.Add(transform);
		}
		view.m_shadowMapsToUpdate.Resize(transforms.Num());
		view.m_shadowMapsToBlit.Resize(transforms.Num());
		view.m_shadowIndices.Resize(transforms.Num());
		view.m_shadowAtlasTiles.Resize(transforms.Num());
		view.m_shadowMatrices.Resize(transforms.Num());
		view.PrepareSnapshots();
	}

	void TestVisibilityAcrossCameraChanges()
	{
		TestWorld world;
		RHISceneView view;
		auto scene = RHIScenePtr::Make(2u);
		const auto center = scene->AddInstance(MakeVisibleRecord(1ull, { 0, 0, -10 }));
		const auto side = scene->AddInstance(MakeVisibleRecord(2ull, { 10, 0, -10 }));
		const auto behind = scene->AddInstance(MakeVisibleRecord(3ull, { 0, 0, 10 }));
		const auto spatial = PublishVisibleScene(scene);
		CameraData camera;
		camera.SetAspect(1.0f);
		camera.SetFov(60.0f);
		camera.SetZFar(100.0f);
		PrepareVisibleView(view, world, { spatial }, camera);
		Require(view.m_snapshots[0].m_proxies.Num() == 1u && view.m_snapshots[0].m_proxies[0].m_handle == center,
			"the initial camera must see only the center object");
		const auto* record = view.m_snapshots[0].m_proxies[0].m_record;
		view.m_snapshots[0].m_proxies[0].m_meshLodOffset = 17u;
		view.m_snapshots[0].m_proxies[0].m_instancedLodOffset = 23u;
		PrepareVisibleView(view, world, { spatial }, camera);
		Require(view.m_snapshots[0].m_proxies.Num() == 1u && view.m_snapshots[0].m_proxies[0].m_record == record &&
			view.m_snapshots[0].m_proxies[0].m_meshLodOffset == RHIVisibleSceneProxy::InvalidIndex &&
			view.m_snapshots[0].m_proxies[0].m_instancedLodOffset == RHIVisibleSceneProxy::InvalidIndex,
			"reusing a pooled view must preserve visibility without carrying submission-local LOD offsets");

		camera.SetAspect(3.0f);
		PrepareVisibleView(view, world, { spatial }, camera);
		Require(view.m_snapshots[0].m_proxies.Num() == 2u && view.m_snapshots[0].m_proxies[0].m_handle == center &&
			view.m_snapshots[0].m_proxies[1].m_handle == side,
			"aspect changes must reveal side objects in stable handle order");
		camera.SetAspect(1.0f);
		PrepareVisibleView(view, world, { spatial }, camera);
		camera.SetFov(120.0f);
		PrepareVisibleView(view, world, { spatial }, camera);
		Require(view.m_snapshots[0].m_proxies.Num() == 2u, "FOV changes must rebuild visibility");
		camera.SetFov(60.0f);
		PrepareVisibleView(view, world, { spatial }, camera);
		camera.SetZFar(5.0f);
		PrepareVisibleView(view, world, { spatial }, camera);
		Require(view.m_snapshots[0].m_proxies.IsEmpty(), "far-plane changes must cull previously visible objects");
		camera.SetZFar(100.0f);
		PrepareVisibleView(view, world, { spatial }, camera);
		camera.SetZNear(15.0f);
		PrepareVisibleView(view, world, { spatial }, camera);
		Require(view.m_snapshots[0].m_proxies.IsEmpty(), "near-plane changes must rebuild visibility");
		camera.SetZNear(0.1f);
		PrepareVisibleView(view, world, { spatial }, camera);
		Math::Transform rotated;
		rotated.SetRotation(glm::angleAxis(glm::radians(180.0f), glm::vec3(0, 1, 0)));
		PrepareVisibleView(view, world, { spatial }, camera, { rotated });
		Require(view.m_snapshots[0].m_proxies.Num() == 1u && view.m_snapshots[0].m_proxies[0].m_handle == behind,
			"rotating the camera must replace the visible set");
		Math::Transform shifted;
		shifted.m_position.x = 10.0f;
		for (uint32_t reuse = 0u; reuse < 2u; ++reuse)
		{
			PrepareVisibleView(view, world, { spatial }, camera, { Math::Transform{}, shifted });
			Require(view.m_snapshots.Num() == 2u && view.m_snapshots[0].m_proxies.Num() == 1u &&
				view.m_snapshots[1].m_proxies.Num() == 1u && view.m_snapshots[0].m_proxies[0].m_handle == center &&
				view.m_snapshots[1].m_proxies[0].m_handle == side,
				"multiple cameras must retain independent visible sets across pooled-view reuse");
		}
		PrepareVisibleView(view, world, { spatial }, camera, { shifted });
		Require(view.m_snapshots.Num() == 1u && view.m_snapshots[0].m_proxies[0].m_handle == side,
			"removing or reordering cameras must not reuse another camera's visibility");
	}

	void TestVisibilityAcrossScenePublication()
	{
		TestWorld world;
		RHISceneView view;
		CameraData camera;
		camera.SetAspect(1.0f);
		camera.SetFov(60.0f);
		auto scene = RHIScenePtr::Make(2u);
		const auto handle = scene->AddInstance(MakeVisibleRecord(1ull, { 0, 0, -10 }, 5.0f));
		auto spatial = PublishVisibleScene(scene);
		PrepareVisibleView(view, world, { spatial }, camera);
		Require(view.m_snapshots[0].m_proxies.IsEmpty(), "distance policy must exclude a far object inside the frustum");
		Math::Transform close;
		close.m_position.z = -7.0f;
		PrepareVisibleView(view, world, { spatial }, camera, { close });
		Require(view.m_snapshots[0].m_proxies.Num() == 1u, "camera translation must reevaluate distance limits");
		PrepareVisibleView(view, world, { spatial }, camera);
		scene->UpdateInstance(handle, MakeVisibleRecord(1ull, { 0, 0, -10 }), ToMask(ESceneChangeBit::MeshOrLodTopology));
		spatial = PublishVisibleScene(scene);
		PrepareVisibleView(view, world, { spatial }, camera);
		Require(view.m_snapshots[0].m_proxies.Num() == 1u, "published topology and distance-policy changes must invalidate visibility");
		auto oldRetained = view.m_snapshots[0].m_sceneVersions;
		const auto* oldRecord = view.m_snapshots[0].m_proxies[0].m_record;
		auto moved = MakeVisibleRecord(1ull, { 0, 0, 10 });
		moved.m_mobility = EMobilityType::Dynamic;
		scene->UpdateInstance(handle, moved, ToMask(ESceneChangeBit::Transform) |
			ToMask(ESceneChangeBit::Bounds) | ToMask(ESceneChangeBit::Mobility));
		spatial = PublishVisibleScene(scene);
		PrepareVisibleView(view, world, { spatial }, camera);
		Require(view.m_snapshots[0].m_proxies.IsEmpty() && oldRecord->m_worldMatrix[3].z == -10.0f,
			"a new spatial publication must cull a moved object without changing an older retained record");
		moved.m_worldMatrix[3].z = -10.0f;
		moved.m_worldBounds = Math::AABB(glm::vec3(0, 0, -10), glm::vec3(0.5f));
		scene->UpdateInstance(handle, moved, ToMask(ESceneChangeBit::Transform) | ToMask(ESceneChangeBit::Bounds));
		spatial = PublishVisibleScene(scene);
		PrepareVisibleView(view, world, { spatial }, camera);
		Require(view.m_snapshots[0].m_proxies.Num() == 1u && view.m_snapshots[0].m_proxies[0].m_record != oldRecord &&
			view.m_snapshots[0].m_proxies[0].GetMobility() == EMobilityType::Dynamic,
			"dynamic publication must restore the object using its current immutable record");
		scene->RemoveInstance(handle);
		spatial = PublishVisibleScene(scene);
		PrepareVisibleView(view, world, { spatial }, camera);
		Require(view.m_snapshots[0].m_proxies.IsEmpty(), "removed handles must disappear from reused views");

		auto replacementScene = RHIScenePtr::Make(2u);
		replacementScene->AddInstance(MakeVisibleRecord(99ull, { 0, 0, -10 }));
		auto replacement = PublishVisibleScene(replacementScene);
		PrepareVisibleView(view, world, { replacement }, camera);
		Require(view.m_snapshots[0].m_proxies.Num() == 1u && view.m_snapshots[0].m_proxies[0].m_record->m_producerKey == 99ull,
			"replacing a scene must replace the visible records");
		auto otherScene = RHIScenePtr::Make(2u);
		otherScene->AddInstance(MakeVisibleRecord(100ull, { 0, 0, -10 }));
		const auto other = PublishVisibleScene(otherScene);
		Require(other->m_revision == replacement->m_revision, "independent scene revisions may coincide");
		PrepareVisibleView(view, world, { other }, camera);
		Require(view.m_snapshots[0].m_proxies.Num() == 1u && view.m_snapshots[0].m_proxies[0].m_record->m_producerKey == 100ull,
			"coincident handles and revisions must not alias independent scene publications");
		PrepareVisibleView(view, world, { other, replacement }, camera);
		Require(view.m_snapshots[0].m_proxies.Num() == 2u && view.m_snapshots[0].m_proxies[0].m_record->m_producerKey == 99ull &&
			view.m_snapshots[0].m_proxies[1].m_record->m_producerKey == 100ull,
			"adding a producer must rebuild the combined stable visible order");
		PrepareVisibleView(view, world, { replacement }, camera);
		TWeakPtr<const RHISpatialSceneVersion> weakSpatial = replacement;
		auto retainedVersion = replacement->m_sceneVersion;
		view.Clear();
		replacement.Clear();
		replacementScene.Clear();
		Require(!weakSpatial.TryLock() && retainedVersion.NumRefs() == 1u,
			"a pooled visibility cache must not retain expired scenes or their record pages");
		PrepareVisibleView(view, world, { other }, camera);
		Require(view.m_snapshots[0].m_proxies.Num() == 1u && view.m_snapshots[0].m_proxies[0].m_record->m_producerKey == 100ull,
			"an expired producer must never validate another producer's cached raw pointers");
		PrepareVisibleView(view, world, {}, camera);
		Require(view.m_snapshots[0].m_proxies.IsEmpty(), "clearing all producers must discard expired cached records");
		PrepareVisibleView(view, world, { spatial }, camera);
		Require(view.m_snapshots[0].m_proxies.IsEmpty(), "returning to an empty producer must keep the view empty");
	}

	void TestMotionHistoryContinuity()
	{
		RHIMotionHistoryFrame previous;
		previous.m_frameData.m_view = glm::mat4(1.0f);
		previous.m_frameData.m_projection = glm::mat4(1.0f);
		previous.m_frameData.m_cameraPosition = glm::vec4(0.0f);
		previous.m_frameData.m_viewportSize = glm::ivec2(1280, 720);
		previous.m_frameData.m_cameraZNearZFar = glm::vec2(0.1f, 1000.0f);
		previous.m_frameData.m_currentTime = 1.0f;
		auto current = previous;
		current.m_frameData.m_currentTime += 1.0f / 60.0f;
		Require(IsMotionHistoryContinuous(previous, current), "adjacent rendered views must retain motion history");
		++current.m_cameraRevision;
		Require(!IsMotionHistoryContinuous(previous, current), "a camera cut must invalidate all object and camera motion");
		--current.m_cameraRevision;
		current.m_frameData.m_viewportSize.x += 1;
		Require(!IsMotionHistoryContinuous(previous, current), "render-size changes must invalidate history");
		current.m_frameData.m_viewportSize = previous.m_frameData.m_viewportSize;
		current.m_frameData.m_currentTime = 2.0f;
		Require(!IsMotionHistoryContinuous(previous, current), "a long suspended render must not stretch the shutter");
		current.m_frameData.m_currentTime = 0.5f;
		Require(!IsMotionHistoryContinuous(previous, current), "time rewinds must invalidate history");
		current.m_frameData.m_currentTime = previous.m_frameData.m_currentTime;
		Require(!IsMotionHistoryContinuous(previous, current), "a paused frame must not reuse nonzero motion");
		current.m_frameData.m_currentTime += 0.02f;
		current.m_frameData.m_cameraPosition.x = 200.0f;
		Require(!IsMotionHistoryContinuous(previous, current), "large unannounced camera teleports must reset history");
		CameraData camera;
		const auto revision = camera.GetMotionHistoryRevision();
		camera.ResetMotionHistory();
		Require(camera.GetMotionHistoryRevision() != revision, "camera reset must survive snapshot copying");
	}

	void TestObjectMotionUsesRenderedVersions()
	{
		auto scene = RHIScenePtr::Make(2u);
		RHISceneViewProxy source;
		source.m_meshModelMatrices.Add(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, 0.0f)));
		auto topology = RHISceneProxyResourcePtr::Make(source);
		auto record = MakeRecord(71u, EMobilityType::Stationary);
		record.m_topology = topology;
		const auto handle = scene->AddInstance(record);
		auto first = scene->PublishVersion();
		record.m_worldMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.0f, 0.0f));
		Require(scene->UpdateInstance(handle, record, ToMask(ESceneChangeBit::Transform)), "a moved motion fixture must update");
		auto second = scene->PublishVersion();
		RHISceneViewSnapshot view;
		view.m_sceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make(TVector<RHISceneVersionPtr>{ second });
		auto previousFrame = TSharedPtr<RHIMotionHistoryFrame>::Make();
		previousFrame->m_sceneVersions = TSharedPtr<const TVector<RHISceneVersionPtr>>::Make(TVector<RHISceneVersionPtr>{ first });
		view.m_previousMotionFrame = std::move(previousFrame);
		const RHISceneInstanceRecord* currentRecord = nullptr;
		Require(second->Resolve(handle, currentRecord), "current motion fixture must resolve");
		RHIVisibleSceneProxy current(handle, *currentRecord, *topology);
		auto previous = current;
		Require(ResolvePreviousMotionProxy(view, current, previous), "retained scene generations must resolve the previously rendered model");
		const auto motion = MakeObjectMotionData(current.ResolveMeshWorldMatrix(0u), previous.ResolveMeshWorldMatrix(0u), previous.GetSkeletonOffset(), true);
		Require(motion.m_state.y == 1u && glm::vec3(motion.m_previousModel[3]) == glm::vec3(0.0f, 2.0f, 0.0f),
			"motion must preserve mesh-local transforms and the previous, not current, object position");
		previousFrame = TSharedPtr<RHIMotionHistoryFrame>::Make();
		previousFrame->m_sceneVersions = view.m_sceneVersions;
		view.m_previousMotionFrame = std::move(previousFrame);
		Require(ResolvePreviousMotionProxy(view, current, previous) && previous.ResolveMeshWorldMatrix(0u) == current.ResolveMeshWorldMatrix(0u),
			"after the next submitted frame a stationary object must have zero object motion without another ECS mutation");
		auto newbornRecord = MakeRecord(72u, EMobilityType::Dynamic);
		newbornRecord.m_topology = topology;
		const auto newborn = scene->AddInstance(newbornRecord);
		auto third = scene->PublishVersion();
		view.m_sceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make(TVector<RHISceneVersionPtr>{ third });
		current.m_handle = newborn;
		Require(third->Resolve(newborn, current.m_record), "newborn fixture must resolve");
		Require(!ResolvePreviousMotionProxy(view, current, previous), "new instances must never inherit motion from an older slot");
		auto otherScene = RHIScenePtr::Make(2u);
		const auto otherHandle = otherScene->AddInstance(record);
		auto otherVersion = otherScene->PublishVersion();
		view.m_sceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make(TVector<RHISceneVersionPtr>{ otherVersion });
		current.m_handle = otherHandle;
		Require(otherVersion->Resolve(otherHandle, current.m_record), "other scene fixture must resolve");
		Require(!ResolvePreviousMotionProxy(view, current, previous), "matching slots in distinct scenes must not share history");
		const auto teleported = MakeObjectMotionData(glm::translate(glm::mat4(1.0f), glm::vec3(1000.0f, 0.0f, 0.0f)), glm::mat4(1.0f), 0u, true);
		Require(teleported.m_state.y == 0u, "respawns and route wrapping must not create full-screen velocities");
		view.ResetForReuse();
		Require(!view.m_previousMotionFrame, "recycled snapshots must release old temporal data");
	}

	void TestBlitFormatConversionPath()
	{
		Require(
			RequiresShaderColorBlitForFormatConversion(
				ETextureFormat::R16G16B16A16_SFLOAT,
				ETextureFormat::B8G8R8A8_UNORM) &&
			RequiresShaderColorBlitForFormatConversion(
				ETextureFormat::R16G16B16A16_SFLOAT,
				ETextureFormat::B8G8R8A8_SRGB),
			"floating-point Scene View output must use the shader blit when targeting normalized editor output");
		Require(
			!RequiresShaderColorBlitForFormatConversion(
				ETextureFormat::R16G16B16A16_UNORM,
				ETextureFormat::B8G8R8A8_UNORM),
			"formats from the same normalized numeric class may use the native blit path");
		Require(
			!RequiresShaderColorBlitForFormatConversion(
				ETextureFormat::D32_SFLOAT,
				ETextureFormat::D32_SFLOAT),
			"matching depth formats must remain on the dedicated native depth path");
	}

	void TestDebugRecordingIsASubmissionPrerequisite()
	{
		for (const bool bIncludeDebugPass : { false, true })
		{
			Tests::TaskTestApp app;
			auto& scheduler = app.GetScheduler();
			scheduler.AttachCurrentThreadAsMainThread();
			auto graph = RHIFrameGraphPtr::Make();
			if (bIncludeDebugPass)
			{
				FrameGraphBuilder builder;
				auto node = builder.CreateNode("DebugDraw"_h);
				Require(static_cast<bool>(node),
					"the runtime factory must create the registered DebugDraw node");
				// No attachments: the node cannot consume its recording result.
				graph->GetGraph().Add(std::move(node));
			}
			auto sceneView = RHISceneViewPtr::Make();
			DebugContext::DrawSnapshot snapshot;
			{
				DebugContext context;
				snapshot = context.GetDrawSnapshot();
			}
			uint32_t completed = 0u;
			for (uint32_t camera = 0; camera < 2u; ++camera)
			{
				// Main-queue admission lets this test complete each camera independently.
				sceneView->m_debugDraw.Add(Tasks::CreateTask<RHICommandListPtr>(
					"Record retained debug snapshot"_h, [snapshot, &completed]()
					{
						DebugContext::DrawDebugMesh({}, glm::mat4(1.0f), snapshot, glm::ivec2(64));
						++completed;
						return RHICommandListPtr{};
					}, EThreadType::Main));
				RHISceneViewSnapshot view;
				view.m_camera = TUniquePtr<CameraData>::Make();
				view.m_cameraIndex = camera;
				sceneView->m_snapshots.Add(std::move(view));
			}
			const auto prerequisites = graph->Prepare(sceneView);
			Require(prerequisites.Num() == 2u,
				"every camera recording must belong to preparation even without a usable DebugDraw pass");
			uint32_t completedAtSubmission = 0u;
			auto frame = Tasks::CreateTask("Submit after all recording"_h,
				[&]() { completedAtSubmission = completed; }, EThreadType::Main);
			for (const auto& task : prerequisites)
			{
				frame->Join(task);
			}
			frame->Run();
			scheduler.ProcessTasksOnMainThread();
			const bool bInitiallyBlocked = !frame->IsStarted();
			prerequisites[0]->Run();
			scheduler.ProcessTasksOnMainThread();
			const bool bBlockedAfterFirstCamera = !frame->IsStarted() && completed == 1u;
			prerequisites[1]->Run();
			scheduler.ProcessTasksOnMainThread();
			Require(bInitiallyBlocked && bBlockedAfterFirstCamera && frame->IsFinished() &&
				completedAtSubmission == 2u,
				"submission must wait for both retained recordings, not depend on DebugDraw consuming them");
		}
	}
}

int main()
{
	try
	{
		TestGenerationalHandlesAndImmutableVersions();
		TestBatchUpdatesPreserveVersionsAndFlights();
		TestCopyOnWritePageSharing();
		TestConcurrentPublishedSceneReaders();
		TestShadowLocalTransformPublication();
		TestRenderedMotionHistoryReleasesOldScenePages();
		TestTwoAndThreeFlightVersionRetention();
		TestFlightRetirementWithMotionHistory();
		TestRangeRetirementAndDirtyCoalescing();
		TestDirtyRangeCountBoundary();
		TestRangeCapacityBoundary();
		TestRangePhysicalReuse();
		TestSubmissionSemaphoreRetention();
		TestImmutableMaterialBindingVersions();
		TestLocalizedShadowVersionDiff();
		TestReflectionProbeAssignmentPublication();
		TestSubmissionCompletionToken();
		TestSubmissionContextSurvivesSnapshotPreparation();
		TestVisibilityAcrossCameraChanges();
		TestVisibilityAcrossScenePublication();
		TestBlitFormatConversionPath();
		TestMotionHistoryContinuity();
		TestObjectMotionUsesRenderedVersions();
		TestDebugRecordingIsASubmissionPrerequisite();
	}
	catch (const std::exception& exception)
	{
		std::cerr << "RHIScene virtualization tests failed: " << exception.what() << '\n';
		return 1;
	}

	std::cout << "RHIScene virtualization tests passed\n";
	return 0;
}
