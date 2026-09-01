#include "Engine/World.h"
#include "FrameGraph/BlitFormatConversion.h"
#include "RHI/Material.h"
#include "RHI/RenderSubmission.h"
#include "RHI/Scene.h"
#include "RHI/SceneView.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace Sailor;
using namespace Sailor::Framegraph;
using namespace Sailor::RHI;

namespace
{
	class TestWorld final : public World
	{
	public:
		TestWorld() : World("RHISceneVirtualizationTests", 0u, {}) {}
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
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

	void TestTwoAndThreeFlightRevisionReplay()
	{
		auto scene = RHIScenePtr::Make(3u);
		const auto stationary = scene->AddInstance(MakeRecord(1ull, EMobilityType::Stationary, 1.0f));
		const auto dynamic = scene->AddInstance(MakeRecord(2ull, EMobilityType::Dynamic, 2.0f));
		auto first = scene->PublishVersion();

		auto flight0 = scene->PrepareFlight(0u, first);
		auto flight1 = scene->PrepareFlight(1u, first);
		auto flight2 = scene->PrepareFlight(2u, first);
		const RHISceneInstanceRecord* flight0Stationary = nullptr;
		Require(flight0->m_appliedVersion->Resolve(stationary, flight0Stationary) &&
			flight0Stationary->m_worldMatrix[3].x == 1.0f &&
			flight0->m_bStationaryFullRebuild,
			"the first free flight must retain the immutable stationary version without copying complete records");
		Require(flight0->m_dynamicHandles->Num() == 1u &&
			flight1->m_dynamicHandles->Num() == 1u &&
			flight2->m_dynamicHandles->Num() == 1u,
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
			updatedStationary->m_worldMatrix[3].x == 10.0f &&
			flight0->m_stationaryDirtyHandles.Contains(stationary),
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
		scene->PrepareFlight(2u, second);
		Require(flight1->m_appliedVersion == second &&
			flight2->m_appliedVersion == second,
			"two and three-flight replay must converge only when each slot is reused");
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
		view.m_globalIlluminationMode = EGlobalIlluminationMode::BakedOnly;
		view.m_bGlobalIlluminationEnabled = false;
		view.m_globalIllumination = firstGlobalIllumination;
		view.PrepareSnapshots();

		Require(view.m_snapshots.Num() == 1u &&
			view.m_snapshots[0].m_submissionContext == context &&
			view.m_snapshots[0].m_renderMode == ESceneViewRenderMode::Cascades &&
			view.m_snapshots[0].m_globalIlluminationMode ==
				EGlobalIlluminationMode::BakedOnly &&
			!view.m_snapshots[0].m_bGlobalIlluminationEnabled,
			"preparing a camera snapshot must retain the acquired flight submission context");
		view.m_renderMode = ESceneViewRenderMode::AmbientOcclusion;
		view.m_globalIlluminationMode = EGlobalIlluminationMode::Realtime;
		view.m_bGlobalIlluminationEnabled = true;
		Require(
			view.m_snapshots[0].m_renderMode == ESceneViewRenderMode::Cascades &&
			view.m_snapshots[0].m_globalIlluminationMode ==
				EGlobalIlluminationMode::BakedOnly &&
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
				ESceneViewRenderMode::GlobalIlluminationClipmapCascades),
			"CPU path-traced Lit composition must yield to every Scene View debug visualization");
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
}

int main()
{
	try
	{
		TestGenerationalHandlesAndImmutableVersions();
		TestCopyOnWritePageSharing();
		TestTwoAndThreeFlightRevisionReplay();
		TestRangeRetirementAndDirtyCoalescing();
		TestImmutableMaterialBindingVersions();
		TestLocalizedShadowVersionDiff();
		TestSubmissionCompletionToken();
		TestSubmissionContextSurvivesSnapshotPreparation();
		TestBlitFormatConversionPath();
	}
	catch (const std::exception& exception)
	{
		std::cerr << "RHIScene virtualization tests failed: " << exception.what() << '\n';
		return 1;
	}

	std::cout << "RHIScene virtualization tests passed\n";
	return 0;
}
