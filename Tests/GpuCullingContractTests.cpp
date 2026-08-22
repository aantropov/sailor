#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "FrameGraph/DepthPrepassNode.h"
#include "FrameGraph/RenderSceneNode.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "Raytracing/MaterialUtils.h"
#include "RHI/Buffer.h"
#include "RHI/Material.h"
#include "RHI/Mesh.h"
#include "RHI/Texture.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	class RenderSceneNodeProbe : public Framegraph::RenderSceneNode
	{
	public:
		using TextureBindingCacheKeyProbe = TextureBindingCacheKey;
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "test source should be readable: " + path.generic_string());
		std::string text = std::string(
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
		text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
		return text;
	}

	std::string ExtractFunctionBody(const std::string& source, const std::string& signature)
	{
		const size_t signatureOffset = source.find(signature);
		Require(signatureOffset != std::string::npos, "function signature should exist: " + signature);

		const size_t bodyOffset = source.find('{', signatureOffset + signature.size());
		Require(bodyOffset != std::string::npos, "function body should exist: " + signature);

		size_t depth = 0;
		for (size_t offset = bodyOffset; offset < source.size(); ++offset)
		{
			if (source[offset] == '{')
			{
				++depth;
			}
			else if (source[offset] == '}' && --depth == 0)
			{
				return source.substr(bodyOffset, offset - bodyOffset + 1);
			}
		}

		throw std::runtime_error("function body should be balanced: " + signature);
	}

	RHI::RHITexturePtr MakeMipTexture(uint32_t width, uint32_t height, uint32_t baseMipLevel)
	{
		auto image = VulkanImagePtr::Make(VulkanDevicePtr{});
		image->m_extent = { width, height, 1u };
		image->m_mipLevels = baseMipLevel + 1u;
		image->m_arrayLayers = 1u;

		auto imageView = VulkanImageViewPtr::Make(VulkanDevicePtr{}, image);
		imageView->m_subresourceRange.baseMipLevel = baseMipLevel;
		imageView->m_subresourceRange.levelCount = 1u;

		auto texture = RHI::RHITexturePtr::Make(
			RHI::ETextureFiltration::Nearest,
			RHI::ETextureClamping::Clamp,
			true);
		texture->m_vulkan.m_image = image;
		texture->m_vulkan.m_imageView = imageView;
		return texture;
	}

	void TestMipExtentUsesVulkanFloorAndClamp()
	{
		Require(MakeMipTexture(1919u, 1079u, 1u)->GetExtent() == glm::ivec2(959, 539),
			"odd mip dimensions must use Vulkan integer floor semantics");
		Require(MakeMipTexture(3u, 5u, 1u)->GetExtent() == glm::ivec2(1, 2),
			"both odd dimensions must be halved independently");
		Require(MakeMipTexture(3u, 5u, 2u)->GetExtent() == glm::ivec2(1, 1),
			"the final mip must clamp both dimensions to one");
		Require(MakeMipTexture(1u, 720u, 9u)->GetExtent() == glm::ivec2(1, 1),
			"a one-pixel dimension must never become zero");
	}

	void TestPackedDrawMobilityPayloadVirtualization()
	{
		struct TestInstance
		{
			uint32_t m_value = 0u;
		};

		RHI::TPackedDrawPacket<TestInstance> source;
		source.Add({}, {}, { 10u }, 10ull, EMobilityType::Static);
		source.Add({}, {}, { 20u }, 20ull, EMobilityType::Stationary);
		source.Add({}, {}, { 30u }, 30ull, EMobilityType::Dynamic);
		source.Finalize(false);
		Require(source.GetNumInstances() == 3u && source.GetGroups().Num() == 3u,
			"mobility segments must materialize as one logical packed packet");
		Require(source.GetGroups()[0].m_firstInstance == 0u &&
			source.GetGroups()[1].m_firstInstance == 1u &&
			source.GetGroups()[2].m_firstInstance == 2u,
			"combined packet groups must reference contiguous static, stationary, and dynamic ranges");

		auto staticPayload = source.SharePayload(EMobilityType::Static);
		auto stationaryPayload = source.SharePayload(EMobilityType::Stationary);
		Require(staticPayload && stationaryPayload && source.HasSharedImmutablePayload(),
			"static and stationary payloads must be publishable as immutable shared storage");

		RHI::TPackedDrawPacket<TestInstance> nextFlight;
		nextFlight.UseSharedPayload(EMobilityType::Static, staticPayload);
		nextFlight.UseSharedPayload(EMobilityType::Stationary, stationaryPayload);
		nextFlight.Add({}, {}, { 31u }, 31ull, EMobilityType::Dynamic);
		nextFlight.Finalize(false);
		Require(nextFlight.GetSharedPayload(EMobilityType::Static) == staticPayload &&
			nextFlight.GetSharedPayload(EMobilityType::Stationary) == stationaryPayload,
			"independent flight packets must retain the same immutable payload identities");
		Require(nextFlight.GetPayload(EMobilityType::Dynamic).m_instances[0].m_value == 31u &&
			!nextFlight.GetSharedPayload(EMobilityType::Dynamic),
			"dynamic records must remain flight-local and be rebuilt for the new submission");

		RHI::TPackedDrawPacket<TestInstance> arenaSource;
		Require(arenaSource.AddArenaInstance({ 10u }, 101ull, EMobilityType::Static) &&
			arenaSource.AddArenaInstance({ 20u }, 102ull, EMobilityType::Static) &&
			arenaSource.AddArenaInstance({ 30u }, 103ull, EMobilityType::Static),
			"a static arena must register immutable records independently of a view");
		auto arenaPayload = arenaSource.ShareArenaPayload(EMobilityType::Static);
		RHI::TPackedDrawPacket<TestInstance> arenaView;
		arenaView.UseSharedArenaPayload(EMobilityType::Static, arenaPayload);
		Require(arenaView.AddArenaView({}, {}, 103ull, EMobilityType::Static) &&
			arenaView.AddArenaView({}, {}, 101ull, EMobilityType::Static),
			"a view packet must resolve visible items through stable arena keys");
		arenaView.Finalize(false);
		Require(arenaView.GetNumStorageInstances() == 3u &&
			arenaView.GetNumDrawInstances() == 2u &&
			arenaView.GetInstanceIndices().Num() == 2u &&
			arenaView.GetInstanceIndices()[0] == 0u &&
			arenaView.GetInstanceIndices()[1] == 2u,
			"view sorting must rebuild compact indices without copying the three static records");

		auto baseLodMesh = RHI::RHIMeshPtr::Make();
		auto selectedLodMesh = RHI::RHIMeshPtr::Make();
		RHI::TPackedDrawPacket<TestInstance> nearView;
		nearView.UseSharedArenaPayload(EMobilityType::Static, arenaPayload);
		Require(nearView.AddArenaView(
			{}, baseLodMesh, 101ull, EMobilityType::Static),
			"the near view must resolve the shared static record");
		nearView.Finalize(false);
		RHI::TPackedDrawPacket<TestInstance> farView;
		farView.UseSharedArenaPayload(EMobilityType::Static, arenaPayload);
		Require(farView.AddArenaView(
			{}, selectedLodMesh, 101ull, EMobilityType::Static),
			"the far view must resolve the same shared static record");
		farView.Finalize(false);
		Require(nearView.GetSharedPayload(EMobilityType::Static) ==
				farView.GetSharedPayload(EMobilityType::Static) &&
			nearView.GetGroups().Num() == 1u && farView.GetGroups().Num() == 1u &&
			nearView.GetGroups()[0].m_mesh == baseLodMesh &&
			farView.GetGroups()[0].m_mesh == selectedLodMesh &&
			nearView.GetInstanceIndices() == farView.GetInstanceIndices(),
			"CPU LOD selection must change only flight/view-local draw groups while retaining the immutable per-instance arena");

		RHI::TPackedDrawPacket<TestInstance> disjointCameraView;
		disjointCameraView.UseSharedArenaPayload(
			EMobilityType::Static,
			arenaPayload);
		Require(disjointCameraView.AddArenaView(
			{}, baseLodMesh, 102ull, EMobilityType::Static),
			"a disjoint camera must resolve records absent from another camera's visible set");
		disjointCameraView.Finalize(false);
		Require(disjointCameraView.GetSharedPayload(EMobilityType::Static) ==
				arenaView.GetSharedPayload(EMobilityType::Static) &&
			disjointCameraView.GetNumStorageInstances() == 3u &&
			disjointCameraView.GetNumDrawInstances() == 1u &&
			disjointCameraView.GetInstanceIndices()[0] == 1u,
			"camera-independent arenas must retain the complete immutable scene while each view owns only compact indices");

		Require(RHI::BuildPackedDrawStableKey({ 7u, 1u }, 11ull, 0u, 0u, 0u) !=
			RHI::BuildPackedDrawStableKey({ 7u, 1u }, 12ull, 0u, 0u, 0u),
			"identical handle slots from independent scene producers must not alias");

		RHI::TPackedDrawPacket<TestInstance> reordered;
		reordered.Add({}, {}, { 30u }, 30ull, EMobilityType::Static);
		reordered.Add({}, {}, { 10u }, 10ull, EMobilityType::Static);
		reordered.Add({}, {}, { 20u }, 20ull, EMobilityType::Static);
		reordered.Finalize(false);
		const auto& reorderedInstances =
			reordered.GetPayload(EMobilityType::Static).m_instances;
		Require(reorderedInstances[0].m_value == 10u &&
			reorderedInstances[1].m_value == 20u &&
			reorderedInstances[2].m_value == 30u,
			"metadata sorting must reorder the single instance array in place without a duplicate payload");

		RHI::TPackedDrawPacketPayloadCache<TestInstance> cache;
		cache.Publish(7u, 101u, staticPayload, 1ull);
		Require(cache.Find(7u, 101u, 2ull) == staticPayload,
			"payload cache must preserve immutable identity across flight slots");
		auto replacementPayload = RHI::TPackedDrawPacketPayloadPtr<TestInstance>::Make();
		replacementPayload->m_instances.Add({ 91u });
		cache.Publish(7u, 102u, replacementPayload, 3ull);
		Require(!cache.Find(7u, 101u, 3ull) &&
			cache.Find(7u, 102u, 3ull) == replacementPayload &&
			cache.Num() == 1u,
			"a logical cache slot must retain only its current immutable revision");
		cache.Evict(12ull, 8ull);
		Require(!cache.Find(7u, 102u, 12ull),
			"unreferenced payload cache entries must expire after the retention window");

		RHI::TPackedDrawPagedArenaCache<TestInstance> pagedCache;
		TVector<TestInstance> rangeA;
		TVector<TestInstance> rangeB;
		TVector<uint64_t> keysA;
		TVector<uint64_t> keysB;
		for (uint32_t index = 0u; index < 64u; ++index)
		{
			rangeA.Add({ 100u + index });
			rangeB.Add({ 200u + index });
			keysA.Add(1000ull + index);
			keysB.Add(2000ull + index);
		}
		const uint64_t rangeKeyA = RHI::BuildPackedDrawRangeKey({ 1u, 1u }, 10ull);
		const uint64_t rangeKeyB = RHI::BuildPackedDrawRangeKey({ 2u, 1u }, 20ull);
		int topologyA = 0;
		int topologyB = 0;
		Require(RHI::BuildPackedDrawRangeKey({ 1u, 1u }, 10ull, &topologyA) !=
			RHI::BuildPackedDrawRangeKey({ 1u, 1u }, 10ull, &topologyB),
			"static arena ranges from different scene topology roots must not alias");
		pagedCache.BeginUpdate(3u, 1u, 1ull);
		Require(pagedCache.ReplaceRange(rangeKeyA, 1ull, rangeA, keysA) &&
			pagedCache.ReplaceRange(rangeKeyB, 1ull, rangeB, keysB),
			"paged static arena must allocate independent stable producer ranges");
		auto pagedV1 = pagedCache.EndUpdate();
		Require(pagedV1 && pagedV1->GetNumStorageInstances() == 128u &&
			pagedV1->m_arenaPages.Num() == 2u,
			"two full producer ranges must occupy two arena pages");

		pagedCache.BeginUpdate(3u, 2u, 2ull);
		Require(pagedCache.TryReuseRange(rangeKeyA, 1ull) &&
			pagedCache.TryReuseRange(rangeKeyB, 1ull),
			"unchanged producer ranges must be reusable without visiting their instances");
		auto pagedV2 = pagedCache.EndUpdate();
		Require(pagedV2->m_arenaPages[0] == pagedV1->m_arenaPages[0] &&
			pagedV2->m_arenaPages[1] == pagedV1->m_arenaPages[1],
			"an unchanged arena version must share every immutable page");

		rangeB[5].m_value = 999u;
		pagedCache.BeginUpdate(3u, 3u, 3ull);
		Require(pagedCache.TryReuseRange(rangeKeyA, 1ull) &&
			pagedCache.ReplaceRange(rangeKeyB, 2ull, rangeB, keysB),
			"a changed producer must replace only its logical range");
		auto pagedV3 = pagedCache.EndUpdate();
		uint32_t changedIndex = 0u;
		Require(pagedV3->m_arenaPages[0] == pagedV2->m_arenaPages[0] &&
			pagedV3->m_arenaPages[1] != pagedV2->m_arenaPages[1] &&
			pagedV3->FindInstance(rangeKeyB, keysB[5], changedIndex) &&
			changedIndex == 69u &&
			pagedV3->m_arenaPages[1]->m_instances[5].m_value == 999u &&
			pagedV2->m_arenaPages[1]->m_instances[5].m_value == 205u,
			"range mutation must clone one page while older in-flight versions stay immutable");

		RHI::TPackedDrawPacket<TestInstance> stationaryArenaView;
		stationaryArenaView.UseSharedArenaPayload(
			EMobilityType::Stationary,
			pagedV3);
		Require(stationaryArenaView.AddArenaView(
			{},
			{},
			rangeKeyB,
			keysB[5],
			EMobilityType::Stationary),
			"stationary records must resolve through the same immutable paged arena path");
		stationaryArenaView.Finalize(false);
		Require(stationaryArenaView.GetPayload(EMobilityType::Stationary).IsPagedArena() &&
			stationaryArenaView.GetNumStorageInstances() == 128u &&
			stationaryArenaView.GetNumDrawInstances() == 1u &&
			stationaryArenaView.GetInstanceIndices()[0] == 69u,
			"a stationary view must share arena pages and rebuild only its compact view indices");

		rangeA[7].m_value = 777u;
		pagedCache.BeginUpdate(3u, 4u, 4ull);
		Require(pagedCache.ReplaceRange(rangeKeyA, 2ull, rangeA, keysA) &&
			pagedCache.TryReuseRange(rangeKeyB, 2ull),
			"a later arena version must retain earlier mutations while applying a new delta");
		auto pagedV4 = pagedCache.EndUpdate();
		Require(pagedV4->m_arenaPages[0] != pagedV1->m_arenaPages[0] &&
			pagedV4->m_arenaPages[1] != pagedV1->m_arenaPages[1],
			"a flight that skips versions must observe every page changed since its upload");

		auto versionedMaterial = RHI::RHIMaterialPtr::Make(
			RHI::RenderState{},
			RHI::RHIShaderPtr{},
			RHI::RHIShaderPtr{});
		auto bindingsV1 = RHI::RHIShaderBindingSetPtr::Make();
		versionedMaterial->SetBindings(bindingsV1);
		const auto materialV1 = versionedMaterial->GetVersion();
		TVector<RHI::PackedDrawArenaMaterialRun> rangeMaterialVersionRuns({
			{ 0u, static_cast<uint32_t>(rangeA.Num()), materialV1 } });
		pagedCache.BeginUpdate(4u, 1u, 5ull);
		Require(pagedCache.ReplaceRange(
			rangeKeyA,
			3ull,
			rangeA,
			keysA,
			&rangeMaterialVersionRuns),
			"paged arena ranges must retain the material generation used to encode each record");
		auto materialPayload = pagedCache.EndUpdate();
		const RHI::PackedDrawArenaRange* materialRange = nullptr;
		Require(materialPayload->m_arenaRanges.Find(rangeKeyA, materialRange) &&
			materialRange && materialRange->m_itemOffsets &&
			materialRange->m_itemOffsets->Num() == rangeA.Num() &&
			materialRange->m_materialVersionRuns &&
			materialRange->m_materialVersionRuns->Num() == 1u &&
			(*materialRange->m_materialVersionRuns)[0].m_count == rangeA.Num(),
			"arena lookup must stay dense and repeated material versions must collapse into one compact run");

		auto bindingsV2 = RHI::RHIShaderBindingSetPtr::Make();
		versionedMaterial->SetBindings(bindingsV2);
		RHI::RHIBatch currentBatch(versionedMaterial, {});
		Require(currentBatch.m_materialVersion != materialV1,
			"the fixture must publish a newer material generation");
		RHI::TPackedDrawPacket<TestInstance> materialView;
		materialView.UseSharedArenaPayload(EMobilityType::Static, materialPayload);
		Require(materialView.AddArenaView(
			currentBatch,
			{},
			rangeKeyA,
			keysA[0],
			EMobilityType::Static),
			"a view must resolve a record from the versioned producer range");
		materialView.Finalize(false);
		Require(materialView.GetGroups().Num() == 1u &&
			materialView.GetGroups()[0].m_batch.m_materialVersion == materialV1 &&
			materialView.GetGroups()[0].m_batch.GetMaterialBindings() == bindingsV1,
			"a reused static record must bind the exact material version that produced its material index");
	}

	void TestMaterialVersionPublicationContract()
	{
		class TestDependency final : public RHI::RHIResource
		{
		public:
			TestDependency() = default;
		};

		auto material = RHI::RHIMaterialPtr::Make(
			RHI::RenderState{},
			RHI::RHIShaderPtr{},
			RHI::RHIShaderPtr{});
		auto oldBindings = RHI::RHIShaderBindingSetPtr::Make();
		material->SetBindings(oldBindings);
		const auto oldVersion = material->GetVersion();
		const auto submissionVersion = material->GetVersionForSubmission(100ull);

		auto pendingBindings = RHI::RHIShaderBindingSetPtr::Make();
		pendingBindings->AddDependency(TRefPtr<TestDependency>::Make());
		material->StageBindings(pendingBindings);
		Require(material->GetVersion() == oldVersion &&
			!material->TryPublishPendingBindings(),
			"a pending material upload must not replace the generation visible to submitted frames");
		Require(material->GetVersion() == oldVersion &&
			material->GetBindings() == oldBindings,
			"failed publication must leave the previous material bindings untouched");

		pendingBindings->ClearDependencies();
		Require(material->TryPublishPendingBindings() &&
			material->GetVersion() != oldVersion &&
			material->GetBindings() == pendingBindings,
			"a completed upload must publish a new immutable material generation atomically");
		Require(oldVersion->GetBindings() == oldBindings,
			"publishing the next generation must not mutate the bindings retained by an older flight");
		Require(submissionVersion == oldVersion &&
			material->GetVersionForSubmission(100ull) == oldVersion &&
			material->GetVersionForSubmission(101ull) == material->GetVersion(),
			"one submission must keep its first captured material generation while the next submission observes the publication");
		RHI::RHIBatch oldBatch(material, {}, 100ull);
		RHI::RHIBatch nextBatch(material, {}, 101ull);
		Require(oldBatch.GetMaterialBindings() == oldBindings &&
			nextBatch.GetMaterialBindings() == pendingBindings,
			"main, depth, and shadow batch construction must use the submission-scoped material capture");

		const auto cutoffVersion = material->GetVersion();
		const uint64_t cutoffRevision =
			RHI::RHIMaterial::BeginSubmissionVersionCapture(200ull);
		for (uint32_t updateIndex = 0u; updateIndex < 6u; ++updateIndex)
		{
			material->SetBindings(RHI::RHIShaderBindingSetPtr::Make());
		}
		const auto latestVersion = material->GetVersion();
		const auto delayedCapture = material->GetVersionForSubmission(200ull);
		RHI::RHIMaterial::EndSubmissionVersionCapture(200ull);
		Require(delayedCapture == cutoffVersion &&
			cutoffVersion->GetPublicationRevision() <= cutoffRevision &&
			latestVersion != cutoffVersion &&
			material->GetVersionForSubmission(201ull) == latestVersion,
			"a submission must resolve the material generation at its begin revision even when several publications happen before the first batch is built");

		auto retentionMaterial = RHI::RHIMaterialPtr::Make(
			RHI::RenderState{},
			RHI::RHIShaderPtr{},
			RHI::RHIShaderPtr{});
		retentionMaterial->SetBindings(RHI::RHIShaderBindingSetPtr::Make());
		auto retiredVersion = retentionMaterial->GetVersion();
		RHI::RHIMaterial::BeginSubmissionVersionCapture(300ull);
		retentionMaterial->SetBindings(RHI::RHIShaderBindingSetPtr::Make());
		auto retainedBySubmission =
			retentionMaterial->GetVersionForSubmission(300ull);
		Require(retainedBySubmission == retiredVersion,
			"the active cutoff must retain its exact material generation until packet capture");
		RHI::RHIMaterial::EndSubmissionVersionCapture(300ull);
		retainedBySubmission.Clear();
		Require(retiredVersion.NumRefs() == 1u,
			"ending an active submission must release material history that is no longer retained by a packet");
	}

	void TestDynamicSpatialRootIsolation()
	{
		RHI::RHISceneViewProxy proxy;
		proxy.m_staticMeshEcs = 41u;
		proxy.m_mobility = EMobilityType::Dynamic;
		proxy.m_worldMatrix = glm::mat4(1.0f);
		proxy.m_worldAabb = Math::AABB(
			glm::vec3(0.0f, 0.0f, -5.0f),
			glm::vec3(1.0f));
		auto resource = RHI::RHISceneProxyResourcePtr::Make(std::move(proxy));
		auto scene = RHI::RHIScenePtr::Make(3u);
		RHI::RHISceneInstanceRecord record;
		record.m_producerKey = 41u;
		record.m_mobility = EMobilityType::Dynamic;
		record.m_worldMatrix = glm::mat4(1.0f);
		record.m_worldBounds = resource->m_proxy.m_worldAabb;
		record.m_topology = resource;
		const auto handle = scene->AddInstance(record);

		auto spatial = RHI::RHISpatialSceneVersionPtr::Make();
		spatial->m_scene = scene;
		spatial->m_sceneVersion = scene->PublishVersion();
		spatial->m_dynamicOctree =
			TSharedPtr<TOctree<RHI::RenderInstanceHandle>>::Make(
				glm::ivec3(0), 128, 4);
		Require(spatial->m_dynamicOctree->Update(
			glm::ivec3(0, 0, -5),
			glm::ivec3(1),
			handle),
			"the dynamic spatial fixture must publish its handle");

		RHI::RHISceneView view;
		view.AddSceneVersion(spatial);
		Math::Frustum frustum;
		frustum.ExtractFrustumPlanes(
			glm::mat4(1.0f),
			1.0f,
			60.0f,
			0.1f,
			10.0f);
		const auto visible = view.TraceScene(frustum, false);
		Require(visible.Num() == 1u && visible[0].m_handle == handle &&
			visible[0].GetMobility() == EMobilityType::Dynamic &&
			!spatial->m_staticOctree && !spatial->m_stationaryOctree,
			"dynamic instances must remain visible through an independent spatial root without allocating static roots");
	}

	void TestInstancedViewLodAndDistanceContract()
	{
		auto baseMesh = RHI::RHIMeshPtr::Make();
		auto lodMesh = RHI::RHIMeshPtr::Make();
		baseMesh->m_bounds = Math::AABB(glm::vec3(0.0f), glm::vec3(0.5f));
		lodMesh->m_bounds = baseMesh->m_bounds;
		baseMesh->m_lods.Add(lodMesh);

		RHI::RHIInstancedMeshGroup group;
		group.m_meshes.Add(baseMesh);
		glm::mat4 nearTransform(1.0f);
		nearTransform[3].z = -2.0f;
		glm::mat4 farTransform(1.0f);
		farTransform[3].z = -20.0f;
		group.m_instanceTransforms = { nearTransform, farTransform };

		const glm::mat4 view(1.0f);
		const glm::mat4 projection = glm::perspective(
			glm::radians(60.0f),
			1.0f,
			0.1f,
			100.0f);
		Math::AABB nearBounds = baseMesh->m_bounds;
		nearBounds.Apply(nearTransform);
		Math::AABB farBounds = baseMesh->m_bounds;
		farBounds.Apply(farTransform);
		const float nearCoverage = RHI::CalculateScreenCoverage(
			nearBounds,
			view,
			projection);
		const float farCoverage = RHI::CalculateScreenCoverage(
			farBounds,
			view,
			projection);
		Require(nearCoverage > farCoverage,
			"the LOD fixture must distinguish near and far instance coverage");

		RHI::RHISceneViewProxy proxy;
		proxy.m_worldMatrix = glm::mat4(1.0f);
		proxy.m_worldAabb = Math::AABB(glm::vec3(0.0f, 0.0f, -11.0f),
			glm::vec3(1.0f, 1.0f, 10.0f));
		proxy.m_lodPolicy.m_bEnabled = true;
		proxy.m_lodPolicy.m_minLod = 0u;
		proxy.m_lodPolicy.m_maxLod = 1u;
		proxy.m_lodPolicy.m_screenCoverageThresholds = {
			(nearCoverage + farCoverage) * 0.5f };
		proxy.m_instancedGroups.Add(group);
		auto resource = RHI::RHISceneProxyResourcePtr::Make(std::move(proxy));

		RHI::RHIVisibleSceneProxy cameraView;
		cameraView.m_resource = resource.GetRawPtr();
		Require(cameraView.ResolveInstancedMesh(group, 0u, 0u, view, projection) == baseMesh &&
			cameraView.ResolveInstancedMesh(group, 1u, 0u, view, projection) == lodMesh,
			"main and depth view classification must select LOD per vegetation instance instead of per chunk");
		Require(cameraView.IsInstancedMeshWithinDistance(
				group, 0u, 0u, glm::vec3(0.0f), 10.0f) &&
			!cameraView.IsInstancedMeshWithinDistance(
				group, 1u, 0u, glm::vec3(0.0f), 10.0f),
			"vegetation distance culling must use per-instance bounds");

		RHI::RHIVisibleShadowCaster shadowView;
		shadowView.m_resource = resource.GetRawPtr();
		const glm::mat4 shadowViewProjection = projection * view;
		Require(shadowView.ResolveInstancedMesh(
				group, 0u, 0u, shadowViewProjection) == baseMesh &&
			shadowView.ResolveInstancedMesh(
				group, 1u, 0u, shadowViewProjection) == lodMesh,
			"shadow views must retain independent per-instance LOD classification");
	}

	void TestGpuCullingRangeAndSynchronizationContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string batchHeader = ReadText(sourceRoot / "Runtime/RHI/Batch.hpp");
		const std::string cullingBody = ExtractFunctionBody(
			batchHeader,
			"const TVector<RHIShaderBindingSetPtr>& cullingDispatchBindings = {})");

		Require(cullingBody.find("constants.m_firstInstanceIndex = firstIndexInstance") != std::string::npos,
			"packed culling must address the flight-local compact index stream");
		Require(cullingBody.find("constants.m_firstStorageInstance = firstStorageInstance") != std::string::npos &&
			cullingBody.find("constants.m_firstCandidateInstance = firstCandidateInstance") != std::string::npos &&
			cullingBody.find("firstCandidateInstance + numInstances") != std::string::npos &&
			cullingBody.find("const bool bResetIndices = computeCullingShader") == std::string::npos &&
			cullingBody.find("packet.m_resolvedInstanceIndices") != std::string::npos,
			"GPU culling must retain immutable candidates and restore its compacted output without a per-frame CPU upload");
		Require(cullingBody.find("constants.m_phase = 1u") != std::string::npos &&
			cullingBody.find("uint32_t m_phase = 0u") != std::string::npos,
			"culling and indirect compaction must be recorded as separate dispatch phases");
		Require(cullingBody.find("EAccessBit::ShaderWrite_Bit") != std::string::npos &&
			cullingBody.find("commands->MemoryBarrier") != std::string::npos,
			"the compaction dispatch must wait for culling shader writes");
		Require(cullingBody.find("m_bEnableOcclusion = 0") != std::string::npos,
			"previous-frame Hi-Z must not cull current-frame transforms");

		Require(cullingBody.find(
			"driver->AddBufferToShaderBindings(") != std::string::npos &&
			cullingBody.find(
			"*indirectCommandBufferBinding") != std::string::npos,
			"the packed path must update the GPU-culling descriptor when its shared indirect buffer grows");
	}

	void TestBatchTextureBindingIdentityContract()
	{
		using TextureBindingCacheKey = RenderSceneNodeProbe::TextureBindingCacheKeyProbe;

		TextureBindingCacheKey firstTextureSet;
		firstTextureSet.m_requestedTextures = { 0u, 4u, 8u };
		TextureBindingCacheKey secondTextureSet;
		secondTextureSet.m_requestedTextures = { 0u, 5u, 8u };

		TMap<TextureBindingCacheKey, uint32_t> textureBindingCache;
		Require(textureBindingCache.Insert(firstTextureSet, 1u),
			"the first texture binding cache key should be inserted");
		Require(textureBindingCache.Insert(secondTextureSet, 2u),
			"a different equal-sized texture binding cache key must not collapse into the first");
		Require(textureBindingCache.Num() == 2,
			"texture sets with the same count and layout capacity must retain distinct cache identities");
		TSet<uint32_t> lookupTextures{ 8u, 4u };
		TextureBindingCacheKey lookupKey(lookupTextures);
		Require(lookupKey.m_requestedTextures.IsEmpty() &&
			lookupKey == firstTextureSet &&
			lookupKey.GetHash() == firstTextureSet.GetHash(),
			"a cache-hit lookup key must compare and hash a non-owning texture set without materializing a vector");
		uint32_t* lookupValue = nullptr;
		Require(textureBindingCache.Find(lookupKey, lookupValue) &&
			lookupValue && *lookupValue == 1u,
			"a non-owning texture key must resolve the canonical cached entry");
		lookupKey.Materialize();
		Require(lookupKey.m_requestedTextures.Num() == 3u &&
			lookupKey.m_requestedTextures[0] == 0u &&
			lookupKey.m_requestedTextures[1] == 4u &&
			lookupKey.m_requestedTextures[2] == 8u,
			"a cache miss must materialize one sorted canonical key including the default texture slot");

		const auto materialBindings = RHI::RHIShaderBindingSetPtr::Make();
		auto material = RHI::RHIMaterialPtr::Make(
			RHI::RenderState{},
			RHI::RHIShaderPtr{},
			RHI::RHIShaderPtr{});
		material->SetBindings(materialBindings);

		const RHI::EBufferUsageFlags bufferUsage =
			RHI::EBufferUsageBit::VertexBuffer_Bit |
			RHI::EBufferUsageBit::IndexBuffer_Bit;
		const auto meshBuffer = RHI::RHIBufferPtr::Make(
			bufferUsage,
			RHI::EMemoryPropertyBit::DeviceLocal);
		auto mesh = RHI::RHIMeshPtr::Make();
		mesh->m_vertexBuffer = meshBuffer;
		mesh->m_indexBuffer = meshBuffer;

		RHI::RHIBatch firstBatch(material, mesh);
		RHI::RHIBatch secondBatch(material, mesh);
		firstBatch.m_textureBindings = RHI::RHIShaderBindingSetPtr::Make();
		secondBatch.m_textureBindings = RHI::RHIShaderBindingSetPtr::Make();
		firstBatch.m_textureBindings->SetVariableDescriptorCount(8u);
		secondBatch.m_textureBindings->SetVariableDescriptorCount(8u);

		TSet<RHI::RHIBatch> batches;
		Require(batches.Insert(firstBatch),
			"the first texture-binding batch should be inserted");
		Require(batches.Insert(secondBatch),
			"a different equal-sized texture-binding batch must not collapse into the first");
		Require(batches.Num() == 2,
			"batch identity must include the texture binding set handle, not its descriptor count");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string batchHeader = ReadText(sourceRoot / "Runtime/RHI/Batch.hpp");
		Require(batchHeader.find("m_textureBindings == rhs.m_textureBindings") != std::string::npos &&
			batchHeader.find("HashCombine(hash, m_textureBindings)") != std::string::npos &&
			batchHeader.find("m_materialVersion == rhs.m_materialVersion") != std::string::npos &&
			batchHeader.find("HashCombine(hash, m_materialVersion)") != std::string::npos,
			"render batches must include immutable material and texture binding versions in equality and hashing");

		const std::string drawBody = ExtractFunctionBody(
			batchHeader,
			"const TVector<RHIShaderBindingSetPtr>& cullingDispatchBindings = {})");
		Require(drawBody.find("firstGroup.m_batch == groups[runEnd].m_batch") != std::string::npos &&
			drawBody.find("collectShaderBindings(batch, drawBindingSets)") != std::string::npos &&
			drawBody.find("commands->BindShaderBindings(") != std::string::npos &&
			drawBody.find("drawBindingSets);") != std::string::npos,
			"packed MDI runs must split on immutable material-version and texture-binding identity");
	}

	void TestSceneViewProxyMaterialAlignmentContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string sceneViewSource = ReadText(sourceRoot / "Runtime/RHI/SceneView.cpp");
		const std::string traceBody = ExtractFunctionBody(
			sceneViewSource,
			"void RHISceneView::TraceScene(");

		Require(traceBody.find("sceneVersion->Resolve(handle, record)") != std::string::npos &&
			traceBody.find("record->m_topology.GetRawPtr()") != std::string::npos &&
			traceBody.find("auto topology = record->m_topology") == std::string::npos &&
			traceBody.find("RHIVisibleSceneProxy visible") != std::string::npos &&
			traceBody.find("visible.m_record = record") != std::string::npos &&
			traceBody.find("visible.m_worldMatrix =") == std::string::npos &&
			traceBody.find("visible.m_worldAabb =") == std::string::npos,
			"scene tracing must resolve compact handles into immutable shared proxy resources");
		Require(traceBody.find("CollectRenderData(") == std::string::npos &&
			traceBody.find("viewProxy.m_overrideMaterials") == std::string::npos,
			"scene tracing must not rebuild or copy mesh and material topology per camera");
		Require(sceneViewSource.find("TraceScene(frustum, res.m_proxies, false)") != std::string::npos &&
			sceneViewSource.find("proxy.m_screenCoverage = CalculateScreenCoverage(") != std::string::npos,
			"camera snapshots must retain lightweight visible references and view-local LOD coverage");
		Require(sceneViewSource.find("m_snapshots.Resize(m_cameras.Num())") != std::string::npos &&
			sceneViewSource.find("res.ResetForReuse()") != std::string::npos &&
			sceneViewSource.find("m_proxies.Clear(false)") != std::string::npos &&
			sceneViewSource.find("m_snapshots.Emplace(std::move(res))") == std::string::npos,
			"pooled scene views must retain per-camera visible-list capacity instead of allocating snapshots every frame");

		const std::string depthSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/DepthPrepassNode.cpp");
		const std::string depthPrepareBody = ExtractFunctionBody(
			depthSource,
			"Tasks::TaskPtr<void, void> DepthPrepassNode::Prepare(");
		const std::string pendingMaterialBody = ExtractFunctionBody(
			depthPrepareBody,
			"if (source->GetMaterials()[i] == nullptr)");
		Require(pendingMaterialBody.find("continue;") != std::string::npos &&
			pendingMaterialBody.find("break;") == std::string::npos,
			"a pending material slot must not discard later ready meshes from the depth pass");
	}

	void TestModelHierarchyRenderPropagationContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string sceneViewSource = ReadText(
			sourceRoot / "Runtime/RHI/SceneView.cpp");
		const std::string normalizeBody = ExtractFunctionBody(
			sceneViewSource,
			"bool TryNormalizeProxyTransforms(");
		Require(normalizeBody.find("inverseWorld * meshMatrix") != std::string::npos &&
			normalizeBody.find("inverseWorld * shadowMesh.m_worldMatrix") != std::string::npos &&
			normalizeBody.find("m_bMeshTransformsAreLocal = true") != std::string::npos,
			"immutable topology must normalize model hierarchy transforms once for reuse across transform-only scene versions");

		const std::string staticMeshSource = ReadText(
			sourceRoot / "Runtime/ECS/StaticMeshRendererECS.cpp");
		const std::string collectBody = ExtractFunctionBody(
			staticMeshSource,
			"bool CollectComponentRenderData(");
		Require(collectBody.find("data.GetMeshIndex()") != std::string::npos &&
			collectBody.find("ownerWorldMatrix * modelMatrix") !=
				std::string::npos,
			"static proxies must resolve the selected source mesh and compose its owner transform");
		Require(staticMeshSource.find(
			"proxy.m_meshModelMatrices = std::move(selectedMatrices)") !=
				std::string::npos,
			"cached static proxies must retain one world matrix per selected render part");
		Require(staticMeshSource.find("previousResource->m_bMeshTransformsAreLocal") != std::string::npos &&
			staticMeshSource.find("bCanReuseTopology = previousResource") != std::string::npos,
			"transform-only updates must rebuild topology when a singular transform prevented local-space normalization");

		const struct
		{
			const char* m_path;
			const char* m_signature;
			const char* m_matrixExpression;
			const char* m_matrixAssignment;
			const char* m_label;
		} frameGraphPasses[] = {
			{
				"Runtime/FrameGraph/RenderSceneNode.cpp",
				"Tasks::TaskPtr<void, void> RenderSceneNode::Prepare(",
				"proxy.ResolveMeshWorldMatrix(i)",
				"data.model = meshWorldMatrix",
				"raster"
			},
			{
				"Runtime/FrameGraph/DepthPrepassNode.cpp",
				"Tasks::TaskPtr<void, void> DepthPrepassNode::Prepare(",
				"proxy.ResolveMeshWorldMatrix(i)",
				"data.model = meshWorldMatrix",
				"depth"
			},
			{
				"Runtime/FrameGraph/ShadowPrepassNode.cpp",
				"void ShadowPrepassNode::Process(",
				"proxy.ResolveMeshWorldMatrix(shadowMesh)",
				"data.model = meshWorldMatrix",
				"shadow"
			}
		};
		for (const auto& pass : frameGraphPasses)
		{
			const std::string source = ReadText(sourceRoot / pass.m_path);
			const std::string body = ExtractFunctionBody(
				source,
				pass.m_signature);
			Require(body.find(pass.m_matrixExpression) !=
					std::string::npos &&
				body.find(pass.m_matrixAssignment) !=
					std::string::npos,
				std::string(pass.m_label) +
					" pass must consume the selected render part world matrix");
		}

		const std::string pathTracerEcsSource = ReadText(
			sourceRoot / "Runtime/ECS/PathTracerECS.cpp");
		const std::string pathTracerTick = ExtractFunctionBody(
			pathTracerEcsSource,
			"Tasks::ITaskPtr PathTracerECS::Tick(");
		Require(pathTracerTick.find("pMeshRenderer->GetMeshIndex()") !=
				std::string::npos &&
			pathTracerTick.find("pModel->HasBLAS(meshIndex)") !=
				std::string::npos &&
			pathTracerTick.find("instance.m_meshIndex = meshIndex") !=
				std::string::npos,
			"path-tracer ECS must publish the selected source mesh and its matching BLAS");

		const std::string pathTracerSource = ReadText(
			sourceRoot / "Runtime/Raytracing/PathTracer.cpp");
		Require(pathTracerSource.find(
			"GetBLAS(instance.m_meshIndex)") != std::string::npos &&
			pathTracerSource.find(
				"GetBLASTriangles(instance.m_meshIndex)") !=
				std::string::npos,
			"path tracing must intersect and shade the same selected source-mesh geometry");
	}

	void TestRenderResourceVirtualizationContract()
	{
		Framegraph::TextureDependencyCollector textureDependencies;
		textureDependencies.Reset();
		textureDependencies.Insert(42u);
		textureDependencies.Insert(7u);
		textureDependencies.Insert(42u);
		textureDependencies.Insert(
			static_cast<uint32_t>(Framegraph::TextureDependencyCollector::MaxTrackedTextures));
		const auto& textureIndices = textureDependencies.GetIndices();
		Require(textureIndices.Num() == 3u &&
			textureIndices[0] == 0u &&
			textureIndices[1] == 7u &&
			textureIndices[2] == 42u,
			"the reusable texture dependency collector must deduplicate, bound, and sort indices without transient sets");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string rendererSource = ReadText(
			sourceRoot / "Runtime/RHI/Renderer.cpp");
		const std::string pushFrameBody = ExtractFunctionBody(
			rendererSource,
			"bool Renderer::PushFrame(");
		const size_t materialCutoffCapture = pushFrameBody.find(
			"RHIMaterial::BeginSubmissionVersionCapture(submissionId)");
		const size_t acquireTaskCreation = pushFrameBody.find(
			"auto acquireRenderSubmission = Tasks::CreateTask(");
		const size_t acquireFlight = pushFrameBody.find("BeginRenderSubmission(");
		const size_t waitForFlight = pushFrameBody.find(
			"acquireRenderSubmission->Wait()");
		const size_t prepareLighting = pushFrameBody.find(
			"FillLightingData(rhiSceneView)");
		const size_t prepareAnimation = pushFrameBody.find(
			"FillAnimationData(rhiSceneView)");
		const size_t prepareFrameGraph = pushFrameBody.find(
			"rhiFrameGraph->Prepare(rhiSceneView)");
		Require(pushFrameBody.find("BeginRenderSubmission(") != std::string::npos &&
			pushFrameBody.find("BeginSubmissionVersionCapture(") != std::string::npos &&
			pushFrameBody.find("EndSubmissionVersionCapture(") != std::string::npos &&
			pushFrameBody.find("acquireRenderSubmission->Join(m_previousRenderFrame)") != std::string::npos &&
			pushFrameBody.find("renderFrame1->Join(t)") != std::string::npos,
			"the selected flight fence must complete before any FrameGraph prepare task can reuse mutable resources");
		Require(materialCutoffCapture != std::string::npos &&
			acquireTaskCreation > materialCutoffCapture &&
			acquireFlight > materialCutoffCapture &&
			waitForFlight > acquireFlight &&
			prepareAnimation > waitForFlight &&
			prepareLighting > waitForFlight &&
			prepareLighting > prepareAnimation &&
			prepareFrameGraph > prepareLighting &&
			pushFrameBody.find("submissionBeginState->m_materialRevision") != std::string::npos,
			"a frame must freeze material publication, acquire a freed flight, and only then prepare flight-local lighting and FrameGraph resources");
		Require(pushFrameBody.find("chainSemaphore = frameGraphChainSemaphore") != std::string::npos &&
			pushFrameBody.find("InvalidateSubmissionResources()") != std::string::npos &&
			pushFrameBody.find("CompleteSubmissionResources(") != std::string::npos &&
			pushFrameBody.find("App::HasEditor() && m_driverInstance->SubmitFrameWithoutPresent") == std::string::npos,
			"partial failures and no-image frames must fence the acquired slot and reject unsubmitted resource versions");

		const std::string submissionHeader = ReadText(
			sourceRoot / "Runtime/RHI/RenderSubmission.h");
		const std::string batchHeader = ReadText(
			sourceRoot / "Runtime/RHI/Batch.hpp");
		const std::string materialImporterSource = ReadText(
			sourceRoot / "Runtime/AssetRegistry/Material/MaterialImporter.cpp");
		const std::string forceMaterialUpdateBody = ExtractFunctionBody(
			materialImporterSource,
			"void Material::ForcelyUpdateUniforms()");
		const size_t cloneBindings = forceMaterialUpdateBody.find(
			"CloneMaterialShaderBindings(");
		const size_t updateUniforms = forceMaterialUpdateBody.find(
			"UpdateUniforms(cmdList)");
		const size_t trackUpload = forceMaterialUpdateBody.find(
			"TrackDelayedInitialization(m_commonShaderBindings.GetRawPtr(), fence)");
		const size_t stageBindings = forceMaterialUpdateBody.find(
			"material->StageBindings(m_commonShaderBindings)");
		Require(cloneBindings != std::string::npos &&
			updateUniforms != std::string::npos &&
			trackUpload != std::string::npos &&
			stageBindings != std::string::npos &&
			cloneBindings < updateUniforms &&
			updateUniforms < trackUpload &&
			trackUpload < stageBindings,
			"material updates must write a fresh binding generation and stage it only after attaching the upload fence");
		const std::string materialReadyBody = ExtractFunctionBody(
			materialImporterSource,
			"bool Material::IsReady() const");
		Require(materialReadyBody.find("TryPublishPendingBindings()") !=
			std::string::npos,
			"ready material uploads must publish their pending immutable RHI generations");

		const std::string rhiMaterialSource = ReadText(
			sourceRoot / "Runtime/RHI/Material.cpp");
		const std::string publishMaterialBody = ExtractFunctionBody(
			rhiMaterialSource,
			"bool RHIMaterial::TryPublishPendingBindings()");
		Require(publishMaterialBody.find("pending->GetBindings()->IsReady()") !=
			std::string::npos &&
			publishMaterialBody.find("PublishVersionLocked(") !=
				std::string::npos &&
			publishMaterialBody.find("m_pendingVersion.Clear()") !=
				std::string::npos &&
			publishMaterialBody.find("g_publishedRhiMaterialRevision.store") !=
				std::string::npos,
			"an RHI material generation must become globally visible only after its upload is ready");

		const std::string vulkanDriverSource = ReadText(
			sourceRoot /
			"Runtime/GraphicsDriver/Vulkan/VulkanGraphicsDriver.cpp");
		const std::string cloneMaterialBody = ExtractFunctionBody(
			vulkanDriverSource,
			"RHI::RHIShaderBindingSetPtr VulkanGraphicsDriver::CloneMaterialShaderBindings(");
		Require(cloneMaterialBody.find("GetUniformBufferAllocator(") !=
				std::string::npos &&
			cloneMaterialBody.find("GetMaterialSsboAllocator()") !=
				std::string::npos &&
			cloneMaterialBody.find("UpdateDescriptorSet(result)") !=
				std::string::npos,
			"copy-on-write material bindings must allocate fresh UBO/SSBO ranges and a fresh descriptor set");
		Require(submissionHeader.find("virtual void InvalidateSubmission()") != std::string::npos &&
			submissionHeader.find("void InvalidateSubmissionResources()") != std::string::npos &&
			batchHeader.find("void InvalidateUploadedState()") != std::string::npos,
			"failed submissions must force a later full upload without discarding retained flight capacity");

		const std::string frameGraphSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RHIFrameGraph.cpp");
		const std::string processBody = ExtractFunctionBody(
			frameGraphSource,
			"bool RHIFrameGraph::Process(");
		Require(processBody.find("FrameGraph:SharedResourceUpload") != std::string::npos &&
			processBody.find("rhiSceneView->m_snapshots[0]") != std::string::npos &&
			processBody.find("PrepareViewSubmissionResources(this, transferCmdList, snapshot, false)") != std::string::npos,
			"light and bone payloads must be uploaded once in an explicit submission dependency before per-camera processing");
		Require(processBody.find("submissionProgress->SetLastSuccessfulSemaphore") != std::string::npos &&
			processBody.find("outWaitSemaphore = submissionProgress->GetLastSuccessfulSemaphore()") != std::string::npos,
			"a partial FrameGraph submit failure must expose the last successful semaphore for slot fencing");

		const std::string renderSceneSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RenderSceneNode.cpp");
		const std::string renderSceneHeader = ReadText(
			sourceRoot / "Runtime/FrameGraph/RenderSceneNode.h");
		const std::string textureCacheHeader = ReadText(
			sourceRoot / "Runtime/FrameGraph/RenderSceneTextureCache.h");
		const std::string textureImporterSource = ReadText(
			sourceRoot / "Runtime/AssetRegistry/Texture/TextureImporter.cpp");
		const std::string renderPrepareBody = ExtractFunctionBody(
			renderSceneSource,
			"Tasks::TaskPtr<void, void> RenderSceneNode::Prepare(");
		const std::string depthSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/DepthPrepassNode.cpp");
		const std::string depthHeader = ReadText(
			sourceRoot / "Runtime/FrameGraph/DepthPrepassNode.h");
		const std::string depthPrepareBody = ExtractFunctionBody(
			depthSource,
			"Tasks::TaskPtr<void, void> DepthPrepassNode::Prepare(");
		Require(renderPrepareBody.find("sceneViewSnapshot.m_visibilityRevision") == std::string::npos &&
			renderPrepareBody.find("bContributesToQueue") != std::string::npos &&
			renderPrepareBody.find("m_mainRevision") != std::string::npos,
			"main packet reuse must depend only on objects relevant to its render queue");
		Require(depthPrepareBody.find("sceneViewSnapshot.m_visibilityRevision") == std::string::npos &&
			depthPrepareBody.find("bContributesToQueue") != std::string::npos &&
			depthPrepareBody.find("m_depthRevision") != std::string::npos,
			"depth packet reuse must ignore unrelated main-pass and render-queue changes");
		const size_t pagedUploadBranch = batchHeader.find("if (payload.IsPagedArena())");
		const size_t stationaryUploadBranch =
			batchHeader.find("else if (mobility == EMobilityType::Stationary)");
		Require(batchHeader.find("NumMobilitySegments = 3u") != std::string::npos &&
			batchHeader.find("m_uploadedStationaryInstances") != std::string::npos &&
			pagedUploadBranch != std::string::npos &&
			stationaryUploadBranch != std::string::npos &&
			pagedUploadBranch < stationaryUploadBranch &&
			batchHeader.find("Dynamic records deliberately remain flight-local") != std::string::npos,
			"packed payload storage must diff paged static/stationary arenas before the contiguous stationary fallback and keep dynamic records flight-local");
		Require(renderPrepareBody.find("m_packetPayloadCache.Find") != std::string::npos &&
			renderPrepareBody.find("m_packet.SharePayload(mobility)") != std::string::npos &&
			depthPrepareBody.find("m_customPacketPayloadCache.Find") != std::string::npos &&
			depthPrepareBody.find("m_customPacket.SharePayload(mobility)") != std::string::npos,
			"main and ordinary/custom depth streams must share immutable payload versions independently");
		Require(renderPrepareBody.find("GetMaterialRevision()") != std::string::npos &&
			depthPrepareBody.find("GetMaterialRevision()") != std::string::npos &&
			depthPrepareBody.find("hashCustomDepthMaterialVersion") != std::string::npos &&
			depthPrepareBody.find("IsRequiredCustomDepthShader()") != std::string::npos,
			"main arenas must version all material bindings while depth ranges version only custom-depth dependencies");
		const std::string shadowPayloadSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/ShadowPrepassNode.cpp");
		const std::string shadowPayloadHeader = ReadText(
			sourceRoot / "Runtime/FrameGraph/ShadowPrepassNode.h");
		const std::string shadowPayloadProcessBody = ExtractFunctionBody(
			shadowPayloadSource,
			"void ShadowPrepassNode::Process(");
		const std::string textureDependencyRevisionBody = ExtractFunctionBody(
			renderSceneSource,
			"uint64_t Details::CalculateTextureDependencyRevision(");
		const std::string textureSamplerRevisionBody = ExtractFunctionBody(
			textureImporterSource,
			"uint64_t TextureImporter::CalculateTextureSamplersRevision(");
		Require(shadowPayloadProcessBody.find("m_packetPayloadCache.Find") != std::string::npos &&
			shadowPayloadProcessBody.find("bBuildShadowPayloads") != std::string::npos &&
			shadowPayloadProcessBody.find("packet.SharePayload(mobility)") != std::string::npos &&
			shadowPayloadProcessBody.find("GetMaterialRevision()") != std::string::npos &&
			shadowPayloadProcessBody.find("m_customDepthMaterial->GetVersionForSubmission(") !=
				std::string::npos &&
			shadowPayloadProcessBody.find("material->GetRenderState().IsRequiredCustomDepthShader()") !=
				std::string::npos,
			"shadow views must reuse ordinary caster pages and version only exact custom-depth material dependencies");
		Require(renderPrepareBody.find("stationaryPayloadIndex") != std::string::npos &&
			renderPrepareBody.find("m_packet.UseSharedArenaPayload(mobility") != std::string::npos &&
			depthPrepareBody.find("stationaryPayloadIndex") != std::string::npos &&
			depthPrepareBody.find("m_customPacket.UseSharedArenaPayload(") != std::string::npos &&
			shadowPayloadProcessBody.find("stationaryPayloadIndex") != std::string::npos &&
			shadowPayloadProcessBody.find("viewResources.m_packet.UseSharedArenaPayload(") != std::string::npos,
			"main, ordinary/custom depth, and shadow passes must virtualize stationary records through camera-independent paged arenas");
		Require(renderPrepareBody.find(
				"sceneViewSnapshot.ForEachSceneProxy(mobility") != std::string::npos &&
			depthPrepareBody.find(
				"sceneViewSnapshot.ForEachSceneProxy(mobility") != std::string::npos &&
			shadowPayloadProcessBody.find(
				"sceneView.ForEachShadowCaster(mobility") != std::string::npos &&
			renderPrepareBody.find(
				"0u : sceneViewSnapshot.m_cameraIndex") != std::string::npos &&
			depthPrepareBody.find(
				"0u : sceneViewSnapshot.m_cameraIndex") != std::string::npos &&
			shadowPayloadProcessBody.find(
				"1469598103934665603ull : viewKey") != std::string::npos,
			"paged main, depth, and shadow arenas must be built from complete scene-version handle lists and shared across disjoint camera views");
		Require(textureCacheHeader.find("class TextureDependencyCollector") != std::string::npos &&
			textureCacheHeader.find("std::bitset<MaxTrackedTextures> m_seen") != std::string::npos &&
			renderPrepareBody.find("m_requestedPacketTextures") != std::string::npos &&
			depthPrepareBody.find("m_requestedPacketTextures") != std::string::npos &&
			shadowPayloadProcessBody.find("m_requestedPacketTextures") != std::string::npos &&
			renderPrepareBody.find("std::array<TSet<uint32_t>") == std::string::npos &&
			depthPrepareBody.find("std::array<TSet<uint32_t>") == std::string::npos &&
			shadowPayloadProcessBody.find("std::array<TSet<uint32_t>") == std::string::npos &&
			renderSceneSource.find("TSet<uint32_t> requestedTextures;") == std::string::npos &&
			depthSource.find("TSet<uint32_t> requestedTextures;") == std::string::npos &&
			shadowPayloadSource.find("TSet<uint32_t> requestedTextures;") == std::string::npos &&
			textureDependencyRevisionBody.find("CalculateTextureSamplersRevision(requestedTextures)") != std::string::npos &&
			textureSamplerRevisionBody.find("m_textureSamplerSlotRevisions") != std::string::npos &&
			textureSamplerRevisionBody.find("GetTextureSamplersSnapshot") == std::string::npos,
			"main, depth, and shadow dependency hashing must reuse bounded collectors and inspect slot revisions without transient snapshots or set copies");
		Require(batchHeader.find("TVector<RHIShaderBindingSetPtr> m_drawBindingSets") != std::string::npos &&
			batchHeader.find("typename TShaderBindingsCallback") != std::string::npos &&
			batchHeader.find("collectShaderBindings(batch, drawBindingSets)") != std::string::npos &&
			batchHeader.find("std::function<TVector<RHIShaderBindingSetPtr>") == std::string::npos &&
			renderSceneSource.find("TVector<RHIShaderBindingSetPtr> sets") == std::string::npos &&
			depthSource.find("TVector<RHIShaderBindingSetPtr> sets") == std::string::npos &&
			shadowPayloadSource.find("TVector<RHIShaderBindingSetPtr> sets") == std::string::npos &&
			renderSceneSource.find("TVector<RHIShaderBindingSetPtr> cullingBindings") == std::string::npos &&
			depthSource.find("TVector<RHIShaderBindingSetPtr> cullingBindings") == std::string::npos &&
			renderSceneSource.find("TVector<RHI::RHITexturePtr>{") == std::string::npos &&
			depthSource.find("TVector<RHI::RHITexturePtr>{") == std::string::npos &&
			shadowPayloadSource.find("TVector<RHI::RHITexturePtr>{") == std::string::npos &&
			shadowPayloadSource.find("TVector<RHITexturePtr>{") == std::string::npos &&
			renderSceneSource.find("m_cullingDispatchBindings") != std::string::npos &&
			depthSource.find("m_cullingDispatchBindings") != std::string::npos &&
			shadowPayloadSource.find("m_blurDrawBindingSets") != std::string::npos,
			"packed draw and render-pass recording must reuse flight-local binding and attachment scratch without per-batch or per-pass vectors");
		Require(renderSceneHeader.find("m_arenaRangeInstances") != std::string::npos &&
			depthHeader.find("m_customArenaRangeInstances") != std::string::npos &&
			shadowPayloadHeader.find("m_arenaRangeInstances") != std::string::npos &&
			renderPrepareBody.find("TVector<PerInstanceData> rangeInstances") == std::string::npos &&
			depthPrepareBody.find("TVector<CustomPerInstanceData> customRangeInstances") == std::string::npos &&
			shadowPayloadProcessBody.find("TVector<PerInstanceData> rangeInstances") == std::string::npos,
			"changed static ranges must reuse flight-local builders across main, ordinary/custom depth, and shadow passes");
		Require(batchHeader.find("TVector<FreeRange> m_occupiedRangesScratch") != std::string::npos &&
			batchHeader.find("TVector<FreeRange> occupied;") == std::string::npos,
			"paged static arena free-range discovery must retain its metadata scratch capacity across updates");
		Require(batchHeader.find("TDrawCalls") == std::string::npos &&
			renderSceneSource.find("ProcessLegacy") == std::string::npos &&
			depthSource.find("ProcessLegacy") == std::string::npos,
			"the migrated renderer must not retain duplicate legacy per-instance containers");
		Require(renderPrepareBody.find("TryGetString(\"VirtualizeInstancePayloads\"") != std::string::npos &&
			depthPrepareBody.find("TryGetString(\"VirtualizeInstancePayloads\"") != std::string::npos &&
			shadowPayloadProcessBody.find("TryGetString(\"VirtualizeInstancePayloads\"") != std::string::npos,
			"immutable payload sharing must remain independently disableable without changing packet ABI");
		for (const auto& rendererPath : {
			sourceRoot / "Content/DefaultRenderer.renderer",
			sourceRoot / "Content/EditorRenderer.renderer",
			sourceRoot / "Content/ExperimentalRenderer.renderer" })
		{
			Require(ReadText(rendererPath).find("VirtualizeInstancePayloads: true") != std::string::npos,
				"shipped renderer graphs must opt into instance payload virtualization");
		}

		const std::string sceneViewHeader = ReadText(
			sourceRoot / "Runtime/RHI/SceneView.h");
		const std::string sceneViewImplementation = ReadText(
			sourceRoot / "Runtime/RHI/SceneView.cpp");
		const std::string sceneImplementation = ReadText(
			sourceRoot / "Runtime/RHI/Scene.cpp");
		const std::string staticMeshSource = ReadText(
			sourceRoot / "Runtime/ECS/StaticMeshRendererECS.cpp");
		Require(sceneViewHeader.find("TSharedPtr<TOctree<RenderInstanceHandle>> m_dynamicOctree") != std::string::npos &&
			sceneViewHeader.find("TSharedPtr<TOctree<RenderInstanceHandle>> m_stationaryOctree") != std::string::npos &&
			sceneViewHeader.find("TSharedPtr<TOctree<RenderInstanceHandle>> m_staticOctree") != std::string::npos &&
			sceneViewHeader.find("record->m_topology.GetRawPtr()") != std::string::npos &&
			sceneViewHeader.find("auto topology = record->m_topology") == std::string::npos &&
			staticMeshSource.find("version->m_staticOctree = m_publishedSceneVersion->m_staticOctree") != std::string::npos &&
			staticMeshSource.find("version->m_stationaryOctree = m_publishedSceneVersion->m_stationaryOctree") != std::string::npos &&
			staticMeshSource.find("version->m_dynamicOctree = m_publishedSceneVersion->m_dynamicOctree") != std::string::npos &&
			staticMeshSource.find("PublishSceneVersion(spatialChangeMask)") != std::string::npos &&
			staticMeshSource.find("else if (bMaterialsDirty)") != std::string::npos &&
			staticMeshSource.find("else if (!bTransformDirty)") == std::string::npos,
			"scene traversal must avoid topology refcount copies and retain unaffected immutable spatial roots");
		const std::string collectGarbageBody = ExtractFunctionBody(
			sceneImplementation,
			"void RHIScene::CollectGarbage()");
		const std::string prepareSnapshotsBody = ExtractFunctionBody(
			sceneViewImplementation,
			"void RHISceneView::PrepareSnapshots()");
		Require(collectGarbageBody.find("m_journal.RemoveAll") == std::string::npos &&
			collectGarbageBody.find("m_retainedVersions.RemoveAll") == std::string::npos &&
			prepareSnapshotsBody.find("m_proxies.RemoveAll") == std::string::npos,
			"flight collection and camera filtering must compact retained storage in place");
		const size_t textureCacheLookup =
			renderSceneSource.find("textureBindingCache.Find(key, cachedEntry)");
		const size_t textureKeyMaterialization =
			renderSceneSource.find("key.Materialize()");
		Require(renderSceneSource.find("TextureBindingCacheKey key(requestedTextures)") !=
				std::string::npos &&
			textureCacheLookup != std::string::npos &&
			textureKeyMaterialization != std::string::npos &&
			textureCacheLookup < textureKeyMaterialization &&
			renderSceneSource.find("requestedTextures.ToVector()") == std::string::npos,
			"macOS texture cache hits must use a non-owning key and allocate canonical storage only after a miss");
		const std::string pendingProxyBody = ExtractFunctionBody(
			staticMeshSource,
			"if (update.m_state == EPreparedProxyState::Pending)");
		Require(pendingProxyBody.find("data.m_bIsDirty = true") != std::string::npos &&
			pendingProxyBody.find("RemoveInstance") == std::string::npos,
			"a static proxy must retain its last ready topology until a pending material upload completes");

		const std::string landscapeSource = ReadText(
			sourceRoot / "Runtime/ECS/LandscapeECS.cpp");
		const std::string landscapeTickBody = ExtractFunctionBody(
			landscapeSource,
			"Tasks::ITaskPtr LandscapeECS::Tick(");
		Require(landscapeTickBody.find("chunksToBuild = data.m_dirtyChunks.ToVector()") != std::string::npos &&
			landscapeTickBody.find("validChunkWriteIndex") != std::string::npos &&
			landscapeTickBody.find("chunk.m_buildRevision = ++data.m_buildRevision") != std::string::npos &&
			landscapeTickBody.find("bVegetationMaterialRenderMetadataRevisionChanged") != std::string::npos &&
			landscapeTickBody.find("data.m_runtimeMaterial->SynchronizeUniformValues(*data.m_material)") !=
				std::string::npos &&
			landscapeSource.find("CalculateVegetationMaterialRenderMetadataRevision") != std::string::npos &&
			landscapeSource.find("material->GetRenderMetadataRevision()") != std::string::npos &&
			landscapeTickBody.find("instanceGroup.m_meshes = std::move(vegetationMeshes)") != std::string::npos &&
			landscapeTickBody.find("instanceGroup.m_meshTransforms = std::move(vegetationModelMatrices)") != std::string::npos,
			"landscape edits and render metadata changes must publish immutable chunks while uniform-only changes version bindings in place");
		Require(landscapeSource.find("m_spatialHash != spatialHash") != std::string::npos &&
			landscapeSource.find("version->m_staticOctree = m_publishedSceneVersion->m_staticOctree") != std::string::npos,
			"landscape chunk payload changes must reuse the immutable spatial root while coarse bounds stay unchanged");

		const std::string lightingSource = ReadText(
			sourceRoot / "Runtime/ECS/LightingECS.cpp");
		const std::string shadowReuseBody = ExtractFunctionBody(
			lightingSource,
			"bool CSMLightState::CanReuse(");
		Require(shadowReuseBody.find("HasShadowChangesIntersecting") != std::string::npos &&
			shadowReuseBody.find("m_casterSceneVersions == sceneVersions") != std::string::npos &&
			shadowReuseBody.find("m_submissionToken->IsSuccessful()") != std::string::npos &&
			shadowReuseBody.find("m_submissionToken != currentSubmissionToken") != std::string::npos &&
			shadowReuseBody.find("m_payloadCompletionToken->IsSuccessful()") != std::string::npos &&
			shadowReuseBody.find("m_bContainsDynamicCasters") != std::string::npos &&
			shadowReuseBody.find("m_bContainsAnimatedCasters") != std::string::npos &&
			shadowReuseBody.find("m_animationRevision != animationRevision") != std::string::npos &&
			lightingSource.find("snapshot.Equals(") == std::string::npos &&
			lightingSource.find("cascadeNeedsUpdate[cascadeIndex]") != std::string::npos,
			"shadow cache invalidation must diff retained COW scene versions and only reuse successfully submitted resources");
		const std::string shadowSceneDiffBody = ExtractFunctionBody(
			sceneImplementation,
			"bool RHISceneVersion::HasShadowChangesIntersecting(");
		Require(shadowSceneDiffBody.find("const RHISceneRecordPage* currentPage") != std::string::npos &&
			shadowSceneDiffBody.find("const RHISceneRecordPagePtr currentPage") == std::string::npos,
			"shadow scene diffs must traverse pages through retained non-owning pointers without per-page shared-ref churn");

		const std::string shadowPrepassSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/ShadowPrepassNode.cpp");
		const std::string customShadowMaterialBody = ExtractFunctionBody(
			shadowPrepassSource,
			"RHI::RHIMaterialPtr ShadowPrepassNode::GetOrAddCustomShadowMaterial(");
		Require(customShadowMaterialBody.find(
				"sourceMaterialVersion->GetBindings()") != std::string::npos &&
			customShadowMaterialBody.find(
				"entry.m_sourceVersion == sourceMaterialVersion") != std::string::npos &&
			customShadowMaterialBody.find(
				"sourceMaterial->GetShaderBindings()") == std::string::npos,
			"custom shadow packets must bind the published generation through a bounded logical material cache");
		const std::string shadowProcessBody = ExtractFunctionBody(
			shadowPrepassSource,
			"void ShadowPrepassNode::Process(");
		Require(shadowProcessBody.find("m_shadowViewCache[viewKey]") != std::string::npos &&
			shadowProcessBody.find("GetHash(shadowPass.m_shadowMap)") == std::string::npos,
			"shadow packet buffers must be cached by logical view slot instead of transient COW texture identity");
	}

	void TestGpuCullingShaderSafetyContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string cullingShader = ReadText(sourceRoot / "Content/Shaders/ComputeMeshCulling.shader");
		const std::string highZShader = ReadText(sourceRoot / "Content/Shaders/ComputeDepthHighZ.shader");

		Require(cullingShader.find("PushConstants.phase == 0") != std::string::npos &&
			cullingShader.find("globalIndex < PushConstants.numBatches") != std::string::npos,
			"the shader must keep culling and compaction in distinct dispatch phases");
		Require(cullingShader.find("InstanceIndicesSSBO") != std::string::npos &&
			cullingShader.find("uint candidateIndex = PushConstants.firstCandidateInstance + globalIndex") != std::string::npos &&
			cullingShader.find("uint instanceId = instanceIndices.instance[candidateIndex]") != std::string::npos &&
			cullingShader.find("instanceIndices.instance[compactIndex] = bIsCulled") != std::string::npos &&
			cullingShader.find("instanceIndices.instance[writeIndex] = instanceId") != std::string::npos &&
			cullingShader.find("data.instance[writeIndex] = data.instance[readIndex]") == std::string::npos,
			"GPU culling must compact uint indices without copying or mutating full per-instance records");
		Require(cullingShader.find("column0") != std::string::npos &&
			cullingShader.find("column1") != std::string::npos &&
			cullingShader.find("column2") != std::string::npos,
			"sphere scaling must account for every transformed basis vector");
		Require(cullingShader.find(
			"SphereFrustumOverlaps(center.xyz, radius, frustum, frame.cameraZNearZFar.x, frame.cameraZNearZFar.y)") != std::string::npos,
			"mesh culling must pass near and far planes in the shared frustum helper's canonical order");
		Require(cullingShader.find("textureQueryLevels(depthHighZ)") != std::string::npos &&
			cullingShader.find("ceil(log2") != std::string::npos,
			"occlusion LOD selection must cover the projected bounds and clamp to the pyramid");
		Require(cullingShader.find("texelBegin") != std::string::npos &&
			cullingShader.find("texelEnd") != std::string::npos &&
			cullingShader.find("texelFetch") != std::string::npos,
			"occlusion must reduce every mip texel touched by projected bounds");
		Require(highZShader.find("binding = 1, r32f") != std::string::npos,
			"the storage image format must match the R32_SFLOAT Hi-Z render target");
		Require(highZShader.find("greaterThanEqual(pos, outputSize)") != std::string::npos,
			"rounded-up dispatch groups must guard storage-image writes");
		Require(highZShader.find("sourceBegin") != std::string::npos &&
			highZShader.find("sourceEnd") != std::string::npos &&
			highZShader.find("texelFetch") != std::string::npos,
			"odd-sized Hi-Z mips must explicitly reduce their complete source footprint");

		const std::string linearizeDepthSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/LinearizeDepthNode.cpp");
		const size_t linearizeDepthAspect = linearizeDepthSource.find(
			"sampledDepthAttachment = depthAspect");
		const size_t linearizeDepthBinding = linearizeDepthSource.find(
			"AddSamplerToShaderBindings(m_linearizeDepth, \"depthSampler\", sampledDepthAttachment",
			linearizeDepthAspect);
		Require(linearizeDepthAspect != std::string::npos &&
			linearizeDepthBinding > linearizeDepthAspect,
			"linear depth sampling must exclude the stencil aspect from combined depth-stencil targets");
	}

	void TestForwardPlusTileSynchronizationContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string cullingShader = ReadText(
			sourceRoot / "Content/Shaders/ComputeLightCulling.shader");
		Require(cullingShader.find(
				"const uint offset = tileIndex * LIGHTS_PER_TILE") !=
				std::string::npos,
			"each Forward+ tile must own a deterministic light-list range");
		Require(cullingShader.find("culledLights.indices[0]") ==
				std::string::npos &&
			cullingShader.find("atomicAdd(culledLights") ==
				std::string::npos,
			"Forward+ light-list allocation must not race across workgroups");
		Require(cullingShader.find("candidateImpact") == std::string::npos &&
			cullingShader.find("BitonicSortCandidates") == std::string::npos &&
			cullingShader.find("uncappedNumLights > LIGHTS_PER_TILE") !=
				std::string::npos,
			"Forward+ must mark tiles whose compact 128-light list overflows");
		Require(cullingShader.find("const bool isInsideViewport") !=
				std::string::npos &&
			cullingShader.find("if (isInsideViewport)") !=
				std::string::npos &&
			cullingShader.find("texelFetch(linearDepth, location, 0).x") !=
				std::string::npos,
			"Forward+ must reduce the matching texels from the pre-linearized depth target");
		Require(cullingShader.find("bool SphereTileOverlaps(") !=
				std::string::npos &&
			cullingShader.find("ViewFrustum CreateTileFrustum(") !=
				std::string::npos &&
			cullingShader.find("ScreenSpaceToViewSpace(") !=
				std::string::npos &&
			cullingShader.find("frame.invProjection") !=
				std::string::npos &&
			cullingShader.find("dot(frustum.planes[i].xyz, lightPosition)") !=
				std::string::npos,
			"Forward+ sphere bounds must use the inverse-projected tile frustum");
		Require(cullingShader.find("const vec2 framebufferTileMin") !=
				std::string::npos &&
			cullingShader.find("const vec2 framebufferTileMax") !=
				std::string::npos &&
			cullingShader.find("viewportSize.y - framebufferTileMax.y") !=
				std::string::npos &&
			cullingShader.find("viewportSize.y - framebufferTileMin.y") !=
				std::string::npos &&
			cullingShader.find("vec4(projectionTileMin, NdcNearPlane") !=
				std::string::npos &&
			cullingShader.find("vec4(projectionTileMax, NdcNearPlane") !=
				std::string::npos,
			"Forward+ tile frusta must convert negative-viewport framebuffer Y before inverse projection");
		Require(cullingShader.find(
				"dot(lightPosition, lightPosition) <= radius * radius") !=
				std::string::npos &&
			cullingShader.find("if(lightPosition.z <= radius)") ==
				std::string::npos,
			"Forward+ may bypass tile rejection only when the camera is inside the complete light sphere");
		Require(cullingShader.find("const float zNear = uintBitsToFloat(minDepthInt)") !=
				std::string::npos &&
			cullingShader.find("const float zFar = uintBitsToFloat(maxDepthInt)") !=
				std::string::npos,
			"Forward+ light culling must use the exact reduced near-far tile interval");

		const std::string lightingShader = ReadText(
			sourceRoot / "Content/Shaders/Lighting.glsl");
		const std::string standardShader = ReadText(
			sourceRoot / "Content/Shaders/Standard.shader");
		const std::string gltfShader = ReadText(
			sourceRoot / "Content/Shaders/Standard_glTF.shader");
		const std::string landscapeShader = ReadText(
			sourceRoot / "Content/Shaders/Landscape.shader");
		Require(lightingShader.find("CalculateLocalLightRangeAttenuation") !=
				std::string::npos &&
			lightingShader.find("smoothstep(0.9f, 1.0f, normalizedDistance)") != std::string::npos &&
			lightingShader.find("return attenuation * rangeWindow") != std::string::npos &&
			standardShader.find("CalculateLocalLightRangeAttenuation(light, distance)") !=
				std::string::npos &&
			gltfShader.find("CalculateLocalLightRangeAttenuation(light, distance)") !=
				std::string::npos &&
			landscapeShader.find("CalculateLocalLightRangeAttenuation(light, distance)") !=
				std::string::npos,
			"all Forward+ surface shaders must preserve attenuation and only fade the final edge of the culling radius");

		const std::string lightCullingSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/LightCullingNode.cpp");
		const std::string processBody = ExtractFunctionBody(
			lightCullingSource,
			"void LightCullingNode::Process(");
		const size_t depthLookup = processBody.find(
			"GetRHIResource(\"linearDepth\")");
		const size_t depthBarrierOffset = processBody.find(
			"ImageMemoryBarrierForComputeSampling(commandList, linearDepthAttachment)",
			depthLookup);
		const size_t dispatchOffset = processBody.find("commands->Dispatch(");
		Require(depthLookup != std::string::npos &&
			depthBarrierOffset != std::string::npos &&
			depthBarrierOffset < dispatchOffset &&
			dispatchOffset != std::string::npos &&
			processBody.find("sceneView.m_rhiLightCullingData", dispatchOffset) !=
				std::string::npos,
			"Forward+ must transition pre-linearized R32F depth for compute sampling before dispatch");

		const std::string frameGraphSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RHIFrameGraph.cpp");
		const std::string prepareViewBody = ExtractFunctionBody(
			frameGraphSource,
			"void PrepareViewSubmissionResources(");
		const size_t createCullingBindings = prepareViewBody.find(
			"resources->m_lightCullingBindings = driver->CreateShaderBindings()");
		const size_t bindCullingOutputs = prepareViewBody.find(
			"resources->m_lightCullingBindings->GetOrAddShaderBinding(\"culledLights\")");
		const size_t publishCullingBindings = prepareViewBody.find(
			"snapshot.m_rhiLightCullingData = resources->m_lightCullingBindings");
		Require(createCullingBindings != std::string::npos &&
			bindCullingOutputs > createCullingBindings &&
			publishCullingBindings > createCullingBindings &&
			prepareViewBody.find("bRecreateLightCulling") != std::string::npos &&
			processBody.find("AddShaderBinding(") == std::string::npos,
			"Forward+ buffers must be flight-local and fully bound before graph nodes record commands");

		const std::string driverSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanGraphicsDriver.cpp");
		const std::string computeSamplingBarrierBody = ExtractFunctionBody(
			driverSource,
			"void VulkanGraphicsDriver::ImageMemoryBarrierForComputeSampling(");
		Require(computeSamplingBarrierBody.find("VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL") !=
				std::string::npos &&
			computeSamplingBarrierBody.find("VulkanCommandBuffer::GetAccessFlags(oldVkLayout)") !=
				std::string::npos &&
			computeSamplingBarrierBody.find("VulkanCommandBuffer::GetPipelineStage(oldVkLayout)") !=
				std::string::npos &&
			computeSamplingBarrierBody.find("VK_ACCESS_SHADER_READ_BIT") !=
				std::string::npos &&
			computeSamplingBarrierBody.find("VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT") !=
				std::string::npos,
			"Forward+ linear depth transition must synchronize its producer with the compute sampler without changing descriptor layout");
		const size_t barrierOffset = processBody.find(
			"commands->MemoryBarrier(",
			dispatchOffset);
		Require(dispatchOffset != std::string::npos &&
			barrierOffset != std::string::npos &&
			processBody.find("EAccessBit::ShaderWrite_Bit", barrierOffset) !=
				std::string::npos &&
			processBody.find("EAccessBit::ShaderRead_Bit", barrierOffset) !=
				std::string::npos,
			"fragment lighting must wait for Forward+ compute shader writes");
		Require(prepareViewBody.find(
				"resources->m_lightCullingDepth != linearDepthAttachment") !=
				std::string::npos &&
			prepareViewBody.find(
				"resources->m_lightCullingViewportSize != lightCullingViewportSize") !=
				std::string::npos,
			"Forward+ buffers must be recreated when their resource identity or extent changes");
		const std::string lightingLibrary = ReadText(
			sourceRoot / "Content/Shaders/Lighting.glsl");
		Require(lightingLibrary.find("uint GetLightTileIndex(") !=
				std::string::npos &&
			lightingLibrary.find("const ivec2 tileId = clamp(") !=
				std::string::npos &&
			lightingLibrary.find("LIGHT_TILE_OVERFLOW_BIT") !=
				std::string::npos,
			"Forward+ consumers must clamp screen coordinates to the tile grid");
		Require(lightingLibrary.find(
				"max(roughness * roughness, 0.001)") !=
				std::string::npos &&
			lightingLibrary.find(
				"max(PI * denom * denom, 1e-7)") !=
				std::string::npos,
			"zero-roughness GGX must remain finite");

		for (const std::filesystem::path shaderPath : {
			sourceRoot / "Content/Shaders/Standard.shader",
			sourceRoot / "Content/Shaders/Standard_glTF.shader",
			sourceRoot / "Content/Shaders/Debug.shader" })
		{
			const std::string shader = ReadText(shaderPath);
			Require(shader.find(
					"GetLightTileIndex(gl_FragCoord.xy, frame.viewportSize)") !=
					std::string::npos &&
				shader.find("lightsGrid.instance[tileIndex].num") !=
					std::string::npos &&
				shader.find("usesOverflowList") == std::string::npos &&
				shader.find(
					"min(lightsGrid.instance[tileIndex].num, uint(LIGHTS_PER_TILE))") ==
					std::string::npos &&
				shader.find("lightsGrid.instance.length()") !=
					std::string::npos &&
				shader.find("culledLights.indices.length()") !=
					std::string::npos,
				"Forward+ shader must use the shared bounded tile lookup: " +
					shaderPath.generic_string());
		}

		for (const std::filesystem::path shaderPath : {
			sourceRoot / "Content/Shaders/Standard.shader",
			sourceRoot / "Content/Shaders/Standard_glTF.shader" })
		{
			const std::string shader = ReadText(shaderPath);
			Require(shader.find("SUPPORT_LIGHTS_OVERFLOW") !=
					std::string::npos &&
				shader.find("lightsOverflow ? uint(i)") !=
					std::string::npos &&
				shader.find("light.instance.length()") !=
					std::string::npos &&
				shader.find("light.instance[index].type == INVALID_LIGHT_TYPE") !=
					std::string::npos,
				"Forward+ lighting must reject invalid light indices: " +
					shaderPath.generic_string());
		}
	}

	void TestRegionBlitDoesNotPromoteToFullImageCopy()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string source = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanCommandBuffer.cpp");
		const std::string body = ExtractFunctionBody(
			source,
			"bool VulkanCommandBuffer::BlitImage(");
		Require(body.find("bRegionsHaveSameExtent") != std::string::npos &&
			body.find("srcRegion.extent.width") != std::string::npos &&
			body.find("srcRegion.extent.height") != std::string::npos,
			"equal-format image operations must use copy only when source and destination regions have equal dimensions");
		Require(body.find("copy.extent = {\n\t\t\t\tsrcRegion.extent.width") != std::string::npos &&
			body.find("copy.extent = src->GetImage()->m_extent") == std::string::npos,
			"a regional copy must never silently expand to the complete source image");
	}

	void TestVulkanMemoryBarrierRecordsPipelineBarrier()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string commandBufferSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanCommandBuffer.cpp");
		const std::string barrierBody = ExtractFunctionBody(
			commandBufferSource,
			"void VulkanCommandBuffer::MemoryBarrier(");

		Require(barrierBody.find("vkCmdPipelineBarrier") != std::string::npos,
			"the RHI buffer barrier must record a Vulkan pipeline barrier");
		Require(barrierBody.find("VkMemoryBarrier") != std::string::npos,
			"the Vulkan barrier must carry the requested access masks");

		const std::string bufferSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanBuffer.cpp");
		Require(bufferSource.find("queues.m_computeFamily.value()") != std::string::npos,
			"concurrently shared culling buffers must include a dedicated compute queue family");
	}

	void TestShaderReadOnlyBarrierSynchronizesShaderSampling()
	{
		const VkAccessFlags shaderReadAccess =
			VulkanCommandBuffer::GetAccessFlags(
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		Require((shaderReadAccess & VK_ACCESS_SHADER_READ_BIT) != 0,
			"shader-read image layouts must wait for prior image writes");

		const VkPipelineStageFlags shaderReadStages =
			VulkanCommandBuffer::GetPipelineStage(
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		Require((shaderReadStages & VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT) != 0,
			"shader-read image layouts must synchronize graphics shader stages");
	}

	void TestCommandListImageTrackingPreservesPublishedContents()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string driverSource = ReadText(
			sourceRoot /
			"Runtime/GraphicsDriver/Vulkan/VulkanGraphicsDriver.cpp");
		const std::string barrierBody = ExtractFunctionBody(
			driverSource,
			"void VulkanGraphicsDriver::ImageMemoryBarrier(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr image, RHI::EImageLayout newLayout)");

		Require(barrierBody.find("image->GetDefaultLayout()") !=
			std::string::npos,
			"a new command list must begin from the layout restored by the previous command list");
		Require(barrierBody.find("m_initialLayout") ==
			std::string::npos,
			"published image contents must not be discarded as if every command list were first use");

		const std::string createTextureBody = ExtractFunctionBody(
			driverSource,
			"RHI::RHITexturePtr VulkanGraphicsDriver::CreateTexture(");
		Require(createTextureBody.find("RHI::EImageLayout::Undefined") !=
			std::string::npos &&
			createTextureBody.find("ImageMemoryBarrier") !=
			std::string::npos,
			"empty textures must be initialized before their default layout is assumed");

		const std::string createMsaaBody = ExtractFunctionBody(
			driverSource,
			"RHI::RHITexturePtr VulkanGraphicsDriver::GetOrAddMsaaFramebufferRenderTarget(");
		Require(createMsaaBody.find("defaultLayout") !=
			std::string::npos &&
			createMsaaBody.find("RHI::EImageLayout::Undefined") !=
			std::string::npos &&
			createMsaaBody.find("ImageMemoryBarrier") !=
			std::string::npos,
			"cached MSAA targets must be initialized in their declared attachment layout");
	}

	void TestTransmissionFramebufferMipContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		for (const std::filesystem::path rendererPath : {
			sourceRoot / "Content/DefaultRenderer.renderer",
			sourceRoot / "Content/EditorRenderer.renderer" })
		{
			const std::string renderer = ReadText(rendererPath);
			const size_t secondaryOffset = renderer.find("- name: Secondary");
			const size_t nextTargetOffset = renderer.find(
				"- name:", secondaryOffset + 1u);
			const std::string secondary = renderer.substr(
				secondaryOffset,
				nextTargetOffset - secondaryOffset);
			Require(secondary.find("bGenerateMips: true") !=
				std::string::npos,
				"the transmission snapshot must allocate a mip chain");
			Require(secondary.find("maxMipLevel:") ==
				std::string::npos,
				"the transmission snapshot must retain its complete mip chain");

			const size_t snapshotOffset = renderer.find("- src: Main");
			const size_t transparentOffset = renderer.find(
				"- Tag: Transparent",
				snapshotOffset);
			const std::string snapshot = renderer.substr(
				snapshotOffset > 128u ? snapshotOffset - 128u : 0u,
				transparentOffset -
					(snapshotOffset > 128u ? snapshotOffset - 128u : 0u));
			Require(snapshot.find("GenerateMips: true") !=
				std::string::npos,
				"only the opaque snapshot blit should request mip generation");
		}

		const std::string blitSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/BlitNode.cpp");
		const std::string blitBody = ExtractFunctionBody(
			blitSource,
			"void BlitNode::Process(");
		Require(blitBody.find("TryGetString(\"GenerateMips\"") !=
			std::string::npos &&
			blitBody.find("bResolvedBlitSuccessful") !=
			std::string::npos &&
			blitBody.find("commands->GenerateMipMaps(commandList, dst)") !=
			std::string::npos,
			"snapshot mip generation must be explicit and require a valid base level");

		const std::string shader = ReadText(
			sourceRoot / "Content/Shaders/Standard_glTF.shader");
		Require(shader.find("textureQueryLevels(g_transmissionFramebufferSampler)") !=
			std::string::npos &&
			shader.find("log2(transmissionFramebufferWidth)") !=
			std::string::npos &&
			shader.find("textureLod(") != std::string::npos &&
			shader.find("transmissionRoughness") != std::string::npos,
			"transmission roughness must select the opaque snapshot mip");
		Require(shader.find("canTransmit") != std::string::npos &&
			shader.find("GetRefractionDirection") != std::string::npos &&
			shader.find("transmissionBrdf") != std::string::npos,
			"transmission must reject total internal reflection independently of volume thickness and use the reflected IBL Fresnel model");
		Require(shader.find("dot(transmissionRay, transmissionRay)") ==
				std::string::npos &&
			shader.find("transmissionRayLength > Epsilon") ==
				std::string::npos,
			"zero-thickness surfaces must retain transmission while volume attenuation remains disabled");
		Require(shader.find("exitEdgeDistance") !=
				std::string::npos &&
			shader.find("environmentRadiance") != std::string::npos &&
			shader.find("framebufferRadiance") != std::string::npos &&
			shader.find("transmissionUvWeight") != std::string::npos,
			"off-screen refraction must smoothly fall back from the opaque framebuffer to environment radiance");
		Require(shader.find("return refractedDirection * thickness * modelScale") !=
				std::string::npos &&
			shader.find("flat vec3 modelScale") != std::string::npos,
			"volume transmission must include runtime instance scale");
		Require(shader.find("NdfGGXAlpha") != std::string::npos &&
			shader.find("VisibilityGGXAlpha") != std::string::npos &&
			shader.find("transmissionFalloff") != std::string::npos,
			"punctual lights must contribute a rough dielectric transmission BTDF");

		const std::string shaderCompilerSource = ReadText(
			sourceRoot /
			"Runtime/AssetRegistry/Shader/ShaderCompiler.cpp");
		Require(shaderCompilerSource.find("DefaultBoneIdsBinding") !=
				std::string::npos &&
			shaderCompilerSource.find("DefaultBoneWeightsBinding") !=
				std::string::npos,
			"generated shader constants must include the skinned vertex bindings");
		const std::string shaderCompilerHeader = ReadText(
			sourceRoot /
			"Runtime/AssetRegistry/Shader/ShaderCompiler.h");
		const size_t cacheVersionOffset = shaderCompilerHeader.find(
			"CacheProducerVersion =");
		Require(cacheVersionOffset != std::string::npos &&
			std::stoul(shaderCompilerHeader.substr(
				shaderCompilerHeader.find('=', cacheVersionOffset) + 1u)) >= 6u,
			"the constants cache version must regenerate existing libraries with the skinned bindings");
		const size_t fragmentStage = shader.find("glslFragment: |");
		Require(fragmentStage != std::string::npos &&
			shader.find("BoneMatricesSSBO", fragmentStage) ==
				std::string::npos,
			"the fragment SKINNING permutation must not redeclare vertex-only bone data");

		const std::string commandBufferSource = ReadText(
			sourceRoot /
			"Runtime/GraphicsDriver/Vulkan/VulkanCommandBuffer.cpp");
		const std::string generateMipsBody = ExtractFunctionBody(
			commandBufferSource,
			"void VulkanCommandBuffer::GenerateMipMaps(");
		Require(generateMipsBody.find(
			"VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL") !=
			std::string::npos,
			"generated mip levels must end in a shader-readable layout");
		const std::string driverSource = ReadText(
			sourceRoot /
			"Runtime/GraphicsDriver/Vulkan/VulkanGraphicsDriver.cpp");
		const std::string driverMipsBody = ExtractFunctionBody(
			driverSource,
			"void VulkanGraphicsDriver::GenerateMipMaps(");
		Require(driverMipsBody.find("ShaderReadOnlyOptimal") !=
			std::string::npos,
			"the command-list layout tracker must match mip generation");
	}

	void TestTransmissionFramebufferBindingUsesNodeAttachment()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string frameGraphSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RHIFrameGraph.cpp");
		const std::string frameGraphBody = ExtractFunctionBody(
			frameGraphSource,
			"void PrepareViewSubmissionResources(");
		Require(frameGraphBody.find("GetRenderTarget(\"Secondary\")") ==
			std::string::npos,
			"transmission bindings must not depend on a hardcoded render-target name");
		Require(frameGraphBody.find(
				"\"g_transmissionFramebufferSampler\"") !=
				std::string::npos &&
			frameGraphBody.find("GetDefaultTexture()") !=
				std::string::npos,
			"a graph without a transmission attachment must replace a stale texture binding");

		const std::string renderSceneSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RenderSceneNode.cpp");
		const std::string renderSceneBody = ExtractFunctionBody(
			renderSceneSource,
			"void RenderSceneNode::Process(");
		Require(renderSceneBody.find(
				"GetResolvedAttachment(\"transmissionFramebuffer\")") !=
				std::string::npos &&
			renderSceneBody.find("g_transmissionFramebufferSampler") !=
				std::string::npos &&
			renderSceneBody.find("AddSamplerToShaderBindings") !=
				std::string::npos,
			"RenderScene must bind the exact transmission attachment that it samples");
	}

	void TestBakedVolumeScalePerInstanceLayoutContract()
	{
		Framegraph::RenderSceneNode::PerInstanceData renderInstance{};
		DepthPrepassNode::PerInstanceData depthInstance{};
		DepthPrepassNode::CustomPerInstanceData customDepthInstance{};
		ShadowPrepassNode::PerInstanceData shadowInstance{};
		const size_t renderScaleOffset = static_cast<size_t>(
			reinterpret_cast<const uint8_t*>(&renderInstance.bakedVolumeScale) -
			reinterpret_cast<const uint8_t*>(&renderInstance));
		const size_t customDepthScaleOffset = static_cast<size_t>(
			reinterpret_cast<const uint8_t*>(&customDepthInstance.bakedVolumeScale) -
			reinterpret_cast<const uint8_t*>(&customDepthInstance));
		const size_t shadowScaleOffset = static_cast<size_t>(
			reinterpret_cast<const uint8_t*>(&shadowInstance.bakedVolumeScale) -
			reinterpret_cast<const uint8_t*>(&shadowInstance));
		const size_t shadowAlphaOffset = static_cast<size_t>(
			reinterpret_cast<const uint8_t*>(&shadowInstance.baseColorAlpha) -
			reinterpret_cast<const uint8_t*>(&shadowInstance));

		Require(sizeof(renderInstance) == 112u &&
			sizeof(depthInstance) == 96u &&
			sizeof(customDepthInstance) == 112u &&
			sizeof(shadowInstance) == 128u &&
			renderScaleOffset == 96u &&
			customDepthScaleOffset == 96u &&
			shadowScaleOffset == 96u &&
			shadowAlphaOffset == 112u &&
			renderScaleOffset + sizeof(vec4) == sizeof(renderInstance),
			"main, ordinary depth, custom depth, and shadow passes must keep their independent std430 instance layouts");

		RHI::RHIMesh mesh;
		Require(mesh.m_bakedVolumeScale == glm::vec3(1.0f) &&
			renderInstance.bakedVolumeScale == glm::vec4(1.0f) &&
			customDepthInstance.bakedVolumeScale == glm::vec4(1.0f) &&
			shadowInstance.bakedVolumeScale == glm::vec4(1.0f),
			"procedural and legacy meshes must default to an identity baked volume scale");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::pair<std::filesystem::path, size_t> mainShaders[] = {
			{ sourceRoot / "Content/Shaders/Simple.shader", 1u },
			{ sourceRoot / "Content/Shaders/Standard.shader", 2u },
			{ sourceRoot / "Content/Shaders/Standard_glTF.shader", 2u },
			{ sourceRoot / "Content/Shaders/Unlit.shader", 2u }
		};
		for (const auto& [shaderPath, expectedLayoutCount] : mainShaders)
		{
			const std::string shader = ReadText(shaderPath);
			Require(shader.find("InstanceIndicesSSBO") != std::string::npos &&
				shader.find("instanceIndices.instance[gl_InstanceIndex]") != std::string::npos,
				"main shaders must dereference the compact instance-index stream: " +
					shaderPath.generic_string());
			size_t layoutCount = 0;
			size_t searchOffset = 0;
			while ((searchOffset = shader.find(
				"struct PerInstanceData",
				searchOffset)) != std::string::npos)
			{
				const size_t layoutEnd = shader.find("};", searchOffset);
				Require(layoutEnd != std::string::npos,
					"shared per-instance shader layout must remain balanced: " +
						shaderPath.generic_string());
				const std::string layout = shader.substr(
					searchOffset,
					layoutEnd - searchOffset);
				size_t memberOffset = 0;
				for (const char* member : {
					"mat4 model;",
					"vec4 sphereBounds;",
					"uint materialInstance;",
					"uint skeletonOffset;",
					"uint isCulled;",
					"uint padding;",
					"vec4 bakedVolumeScale;" })
				{
					memberOffset = layout.find(member, memberOffset);
					Require(memberOffset != std::string::npos,
						"main per-instance shader layouts must preserve the 112-byte std430 member order: " +
							shaderPath.generic_string());
					memberOffset += std::char_traits<char>::length(member);
				}

				++layoutCount;
				searchOffset = layoutEnd + 2u;
			}
			Require(layoutCount == expectedLayoutCount,
				"every main per-instance shader stage must use the main-pass std430 layout: " +
					shaderPath.generic_string());
		}

		const std::string depthShader = ReadText(
			sourceRoot / "Content/Shaders/DepthOnly.shader");
		Require(depthShader.find("uint reserved;") != std::string::npos &&
			depthShader.find("vec4 bakedVolumeScale;") == std::string::npos &&
			depthShader.find("InstanceIndicesSSBO") != std::string::npos &&
			depthShader.find("instanceIndices.instance[gl_InstanceIndex]") != std::string::npos,
			"ordinary depth must use its compact 96-byte layout and the shared index indirection");

		const std::string cullingShader = ReadText(
			sourceRoot / "Content/Shaders/ComputeMeshCulling.shader");
		Require(cullingShader.find("#ifdef DEPTH_INSTANCE_LAYOUT") != std::string::npos &&
			cullingShader.find("InstanceIndicesSSBO") != std::string::npos &&
			cullingShader.find("data.instance[writeIndex] = data.instance[readIndex]") == std::string::npos,
			"GPU culling must select the pass layout and compact uint indices instead of full instance records");

		const std::string gltfShader = ReadText(
			sourceRoot / "Content/Shaders/Standard_glTF.shader");
		Require(gltfShader.find(
				"data.instance[instanceIndex].bakedVolumeScale.xyz") !=
				std::string::npos,
			"glTF transmission must combine runtime and baked node scale");

		const std::string renderSceneSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RenderSceneNode.cpp");
		const std::string depthPrepassSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/DepthPrepassNode.cpp");
		Require(renderSceneSource.find(
				"vec4(mesh->m_bakedVolumeScale, 1.0f)") !=
				std::string::npos &&
			depthPrepassSource.find(
				"customData.bakedVolumeScale = vec4(mesh->m_bakedVolumeScale, 1.0f)") !=
				std::string::npos,
			"main and custom-depth streams must upload each RHIMesh baked volume scale");
	}

	void TestDepthPrepassSkinningContract()
	{
		const RHI::RHISceneViewProxy unskinnedProxy{};
		Require(unskinnedProxy.m_skeletonOffset ==
			(std::numeric_limits<uint32_t>::max)(),
			"scene proxies without an animator must use the invalid skeleton offset so meshes with unused bone attributes stay rigid in the depth pass");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string depthShader = ReadText(
			sourceRoot / "Content/Shaders/DepthOnly.shader");
		Require(depthShader.find("- SKINNING") != std::string::npos &&
			depthShader.find("DefaultBoneIdsBinding") != std::string::npos &&
			depthShader.find("DefaultBoneWeightsBinding") != std::string::npos &&
			depthShader.find("layout(std430, set = 2, binding = 0) readonly buffer BoneMatricesSSBO") != std::string::npos &&
			depthShader.find("data.instance[instanceIndex].skeletonOffset") != std::string::npos &&
			depthShader.find("modelMatrix *= skinMatrix") != std::string::npos,
			"the skinned depth permutation must apply the scene bone palette before projection");

		const std::string depthPrepassSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/DepthPrepassNode.cpp");
		const std::string getDepthMaterialBody = ExtractFunctionBody(
			depthPrepassSource,
			"RHI::RHIMaterialPtr DepthPrepassNode::GetOrAddDepthMaterial(");
		Require(getDepthMaterialBody.find("m_skinnedDepthOnlyMaterials") != std::string::npos &&
			getDepthMaterialBody.find("defines.Add(\"SKINNING\")") != std::string::npos,
			"DepthPrepass must cache a separate material for the skinned shader permutation");

		const std::string prepareBody = ExtractFunctionBody(
			depthPrepassSource,
			"Tasks::TaskPtr<void, void> DepthPrepassNode::Prepare(");
		Require(prepareBody.find("proxy.GetSkeletonOffset()") != std::string::npos &&
			prepareBody.find("HasAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding)") != std::string::npos &&
			prepareBody.find("HasAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding)") != std::string::npos &&
			prepareBody.find("GetOrAddDepthMaterial(") != std::string::npos &&
			prepareBody.find("bSkinned,") != std::string::npos &&
			prepareBody.find("bMaskedQueue,") != std::string::npos,
			"DepthPrepass must select its skinned material from both the skeleton offset and the concrete mesh vertex layout");
		Require(getDepthMaterialBody.find("DepthMaterialKey key") != std::string::npos &&
			getDepthMaterialBody.find("cullMode") != std::string::npos &&
			prepareBody.find("sourceMaterial->GetRenderState().GetCullMode()") != std::string::npos,
			"DepthPrepass must preserve the source material cull mode so its depth contributors match the visible geometry");

		Require(depthShader.find("- MASKED") != std::string::npos &&
			depthShader.find("ResolveTextureSamplerIndex") != std::string::npos &&
			depthShader.find("alpha < inAlphaCutoff") != std::string::npos &&
			depthShader.find("float alpha = inVertexAlpha") != std::string::npos &&
			prepareBody.find("m_baseColorSamplers") != std::string::npos &&
			prepareBody.find("effectiveAlphaCutoff") != std::string::npos &&
			prepareBody.find("const bool bRequiredCustomDepth = !bMaskedQueue") != std::string::npos,
			"masked depth prepass must apply the same alpha silhouette as the forward material");

		const std::string processBody = ExtractFunctionBody(
			depthPrepassSource,
			"void DepthPrepassNode::Process(");
		Require(processBody.find("{ \"DEPTH_INSTANCE_LAYOUT\" }") !=
				std::string::npos &&
			processBody.find("OCCLUSION_CULLING") == std::string::npos,
			"the depth prepass must keep GPU frustum culling but cannot occlusion-cull the geometry that produces current-frame Hi-Z");
	}

	void TestShadowPrepassSkinningContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string shadowShader = ReadText(
			sourceRoot / "Content/Shaders/ShadowCaster.shader");
		Require(shadowShader.find("- SKINNING") != std::string::npos &&
			shadowShader.find("DefaultBoneIdsBinding") != std::string::npos &&
			shadowShader.find("DefaultBoneWeightsBinding") != std::string::npos &&
			shadowShader.find("layout(std430, set = 2, binding = 0) readonly buffer BoneMatricesSSBO") != std::string::npos &&
			shadowShader.find("data.instance[instanceIndex].skeletonOffset") != std::string::npos &&
			shadowShader.find("modelMatrix *= skinMatrix") != std::string::npos,
			"the skinned shadow permutation must apply the scene bone palette before light projection");

		const std::string shadowPrepassSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/ShadowPrepassNode.cpp");
		const std::string getShadowMaterialBody = ExtractFunctionBody(
			shadowPrepassSource,
			"RHI::RHIMaterialPtr ShadowPrepassNode::GetOrAddShadowMaterial(");
		Require(getShadowMaterialBody.find("m_skinnedShadowMaterials_Evsm") != std::string::npos &&
			getShadowMaterialBody.find("m_skinnedShadowMaterials_Pcf") != std::string::npos &&
			getShadowMaterialBody.find("defines.Add(\"SKINNING\")") != std::string::npos,
			"PCF and EVSM shadow passes must cache separate skinned shader permutations");

		const std::string processBody = ExtractFunctionBody(
			shadowPrepassSource,
			"void ShadowPrepassNode::Process(");
		Require((processBody.find("proxy.GetSkeletonOffset()") != std::string::npos ||
			processBody.find("proxy->m_skeletonOffset") != std::string::npos) &&
			processBody.find("HasAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding)") != std::string::npos &&
			processBody.find("HasAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding)") != std::string::npos &&
			processBody.find("GetOrAddShadowMaterial(mesh->m_vertexDescription, shadowPass.m_shadowType, bSkinned, bMasked)") != std::string::npos &&
			processBody.find("sets.Add(sceneView.m_boneMatrices)") != std::string::npos,
			"ShadowPrepass must select skinned batches and bind the current bone matrices");
	}

	void TestCustomDepthVertexAnimationReachesShadows()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string sceneViewHeader = ReadText(
			sourceRoot / "Runtime/RHI/SceneView.h");
		const std::string meshRendererSource = ReadText(
			sourceRoot / "Runtime/ECS/StaticMeshRendererECS.cpp");
		const std::string shadowPrepassSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/ShadowPrepassNode.cpp");
		const std::string shadowCasterShader = ReadText(
			sourceRoot / "Content/Shaders/ShadowCaster.shader");
		const std::string customShadowCasterShader = ReadText(
			sourceRoot / "Content/Experimental/MeshParticles/Particle.shader");

		Require(sceneViewHeader.find("RHIMaterialPtr m_customDepthMaterial") != std::string::npos &&
			sceneViewHeader.find("ShaderSetPtr m_customDepthShader") != std::string::npos &&
			meshRendererSource.find("IsRequiredCustomDepthShader()") != std::string::npos &&
			meshRendererSource.find("shadowMesh.m_customDepthMaterial = material->GetOrAddRHI(") != std::string::npos,
			"shadow proxies must retain immutable custom-depth RHI material and shader identities");
		Require(shadowPrepassSource.find("GetOrAddCustomShadowMaterial(") != std::string::npos &&
			shadowPrepassSource.find("m_customDepthMaterial->GetVersionForSubmission(") != std::string::npos &&
			shadowPrepassSource.find("GetSupportedDefines().Contains(\"PACKED_SHADOW_CASTER\")") != std::string::npos &&
			shadowPrepassSource.find("defines.Add(\"PACKED_SHADOW_CASTER\")") != std::string::npos &&
			shadowPrepassSource.find("defines.Add(\"ALPHA_CUTOUT\")") != std::string::npos &&
			shadowPrepassSource.find("sceneView.m_rhiLightsData") != std::string::npos &&
			shadowPrepassSource.find("batch.GetMaterialBindings()") != std::string::npos,
			"supported custom shadow permutations must bind the same frame, light and material data as the forward vertex shader");
		Require(shadowPrepassSource.find("auto depthMaterial = GetOrAddShadowMaterial(") != std::string::npos &&
			shadowPrepassSource.find("if (customShadowMaterial)") != std::string::npos &&
			shadowPrepassSource.find("depthMaterial = customShadowMaterial") != std::string::npos,
			"unsupported or failed custom shadow permutations must fall back to the working main shadow caster");
		Require(shadowPrepassSource.find("RHI::RHIMaterialPtr pushConstantsMaterial") != std::string::npos &&
			shadowPrepassSource.find("PushConstants(commandList, pushConstantsMaterial, 64") != std::string::npos,
			"custom shadow permutations must preserve the main CSM cascade-matrix recording path");
		Require(meshRendererSource.find("if (shadowMesh.m_customDepthMaterial)") != std::string::npos &&
			meshRendererSource.find("for (const auto& sampler : material->GetSamplers())") != std::string::npos &&
			meshRendererSource.find("shadowMesh.m_materialTextureSamplers.Insert(textureIndex)") != std::string::npos,
			"custom shadow vertex animation must receive the material texture remap even for opaque casters");
		Require(shadowCasterShader.find("vec4 sphereBounds;") != std::string::npos &&
			shadowCasterShader.find("uint materialInstance;") != std::string::npos &&
			shadowCasterShader.find("vec4 bakedVolumeScale;") != std::string::npos &&
			shadowCasterShader.find("float baseColorAlpha;") != std::string::npos &&
			shadowCasterShader.find("uint maskedPadding;") != std::string::npos &&
			shadowCasterShader.find("vec4 baseColorFactor;") == std::string::npos,
			"the default shadow caster must keep only alpha metadata in its compact instance layout");
		Require(customShadowCasterShader.find("- SHADOW_CASTER") != std::string::npos &&
			customShadowCasterShader.find("- PACKED_SHADOW_CASTER") != std::string::npos &&
			customShadowCasterShader.find("- EVSM") != std::string::npos &&
			customShadowCasterShader.find("vec4 sphereBounds;") != std::string::npos &&
			customShadowCasterShader.find("float baseColorAlpha;") != std::string::npos &&
			customShadowCasterShader.find("InstanceIndicesSSBO") != std::string::npos &&
			customShadowCasterShader.find("instanceIndices.instance[gl_InstanceIndex]") != std::string::npos &&
			customShadowCasterShader.find("PushConstants.lightMatrix") != std::string::npos,
			"custom shadow permutations must use the compact shadow ABI, draw-index indirection, and pass-local light matrix");
		Require(sizeof(ShadowPrepassNode::PerInstanceData) == 128u &&
			sizeof(Framegraph::RenderSceneNode::PerInstanceData) == 112u,
			"CPU instance layouts must match the 128-byte shadow and 112-byte forward std430 strides; shadow=" +
			std::to_string(sizeof(ShadowPrepassNode::PerInstanceData)) +
			", forward=" + std::to_string(sizeof(Framegraph::RenderSceneNode::PerInstanceData)));
	}

	void TestVertexDescriptionAttributeIdentityContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string vertexDescriptionSource = ReadText(
			sourceRoot / "Runtime/RHI/VertexDescription.cpp");
		Require(vertexDescriptionSource.find("SetAttributeFormat(m_bits, location, format)") != std::string::npos &&
			vertexDescriptionSource.find("attribute.m_location == location") != std::string::npos,
			"vertex descriptions must identify and query attributes by shader location, not buffer binding");
	}

	void TestGraphicsPipelineAttachmentCacheContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string materialSource = ReadText(
			sourceRoot / "Runtime/RHI/Material.cpp");
		const std::string getPipelineBody = ExtractFunctionBody(
			materialSource,
			"GraphicsDriver::Vulkan::VulkanGraphicsPipelinePtr RHIMaterial::Vulkan::GetOrAddPipeline(");
		Require(getPipelineBody.find("ComputeAspectFlagsForFormat(depthStencilAttachment)") != std::string::npos &&
			getPipelineBody.find("Fits(colorAttachments, depthStencilAttachment, stencilAttachmentFormat)") != std::string::npos,
			"graphics pipeline lookup must compare a depth-only attachment with an undefined stencil format");
		Require(getPipelineBody.find("m_pipelinesLock.Lock()") != std::string::npos &&
			getPipelineBody.find("m_pipelinesLock.Unlock()") != std::string::npos,
			"parallel command-list recording must serialize graphics pipeline cache misses");

		const std::string pipelineStatesSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanPipileneStates.cpp");
		const std::string buildPipelineBody = ExtractFunctionBody(
			pipelineStatesSource,
			"const TVector<VulkanPipelineStatePtr>& VulkanPipelineStateBuilder::BuildPipeline(");
		Require(buildPipelineBody.find("vertexDescription->GetVertexStride()") != std::string::npos &&
			buildPipelineBody.find("vertexDescription->GetAttributeDescriptions()") != std::string::npos &&
			buildPipelineBody.find("vertexAttributeBindings.ToVector()") != std::string::npos &&
			buildPipelineBody.find("orderedVertexAttributeBindings.Sort()") != std::string::npos,
			"pipeline-state cache identity must include the concrete vertex layout and shader input locations");

		const std::string pipelineSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanPipeline.cpp");
		const std::string commandBufferSource = ReadText(
			sourceRoot / "Runtime/GraphicsDriver/Vulkan/VulkanCommandBuffer.cpp");
		Require(pipelineSource.find("result != VK_SUCCESS || m_pipeline == VK_NULL_HANDLE") != std::string::npos &&
			commandBufferSource.find("if (!m_bGraphicsPipelineBound)") != std::string::npos,
			"failed graphics pipeline compilation must not record draw commands without a valid pipeline");
	}

	void TestRenderSceneSurfaceResolveContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string renderSceneSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RenderSceneNode.cpp");
		const std::string processBody = ExtractFunctionBody(
			renderSceneSource,
			"void RenderSceneNode::Process(");

		Require(processBody.find("if (colorSurface)") != std::string::npos &&
			processBody.find("resources->m_renderPassColorSurfaces") !=
				std::string::npos &&
			processBody.find("colorSurface->NeedsResolve()") !=
				std::string::npos &&
			processBody.find("colorSurface->GetResolved()") !=
				std::string::npos &&
			processBody.find("renderPassColorSurfaces.Add(colorSurface)") !=
				std::string::npos &&
			processBody.find("commands->BeginRenderPass(\n\t\t\tcommandList,\n\t\t\trenderPassColorSurfaces") !=
				std::string::npos,
			"RenderScene must pass an RHISurface to the surface render-pass overload so an MSAA target is resolved exactly once");
		Require(processBody.find("renderPassColorAttachments.Add(colorAttachment)") !=
				std::string::npos,
			"RenderScene must retain the direct texture attachment fallback for non-surface frame-graph outputs");
	}

	void TestPostProcessPingPongMipIsolationContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string motionBlur = ReadText(
			sourceRoot / "Content/Shaders/MotionBlur.shader");
		const size_t firstBaseMipSample = motionBlur.find(
			"textureLod(colorSampler");
		const size_t secondBaseMipSample = motionBlur.find(
			"textureLod(colorSampler",
			firstBaseMipSample == std::string::npos ? 0u : firstBaseMipSample + 1u);
		Require(firstBaseMipSample != std::string::npos &&
			secondBaseMipSample != std::string::npos &&
			motionBlur.find("texture(colorSampler") == std::string::npos,
			"motion blur must sample only the tone-mapped base mip after Secondary was used as an HDR transmission snapshot");
		Require(motionBlur.find("cameraRelativeDepth") == std::string::npos &&
			motionBlur.find("speedPixels > data.maxSpeed") != std::string::npos &&
			motionBlur.find("min(1, velocity") == std::string::npos,
			"depth-reprojection motion blur must clamp signed velocity symmetrically without distance-based geometry exclusions");

		const std::string standardGltf = ReadText(
			sourceRoot / "Content/Shaders/Standard_glTF.shader");
		Require(standardGltf.find("#ifndef DISABLE_SCREEN_SPACE_AO") != std::string::npos &&
			standardGltf.find("float occlusion = 1.0") != std::string::npos,
			"late-rendered viewmodels must be able to skip screen-space AO while retaining material occlusion");

		const std::string eyeAdaptationSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/EyeAdaptationNode.cpp");
		const std::string eyeAdaptationBody = ExtractFunctionBody(
			eyeAdaptationSource,
			"void EyeAdaptationNode::Process(");
		Require(eyeAdaptationBody.find("GetMipLayer(0)") !=
				std::string::npos &&
			eyeAdaptationBody.find(
				"TVector<RHI::RHITexturePtr>{colorTarget}") !=
				std::string::npos,
			"eye adaptation must overwrite Secondary through its base-mip attachment view");

		for (const std::filesystem::path rendererPath : {
			sourceRoot / "Content/DefaultRenderer.renderer",
			sourceRoot / "Content/EditorRenderer.renderer" })
		{
			const std::string renderer = ReadText(rendererPath);
			const size_t motionBlurOffset = renderer.find(
				"- shader: Shaders/MotionBlur.shader");
			const size_t viewmodelOffset = renderer.find(
				"- Tag: Viewmodel",
				motionBlurOffset);
			const size_t eyeAdaptationOffset = renderer.find(
				"- name: EyeAdaptation",
				viewmodelOffset);
			const size_t debugDrawOffset = renderer.find(
				"- name: DebugDraw",
				eyeAdaptationOffset);
			const size_t outputBlitOffset = renderer.find(
				"- name: Blit",
				debugDrawOffset);
			const size_t renderImGuiOffset = renderer.find(
				"- name: RenderImGui",
				outputBlitOffset);
			Require(motionBlurOffset != std::string::npos &&
				viewmodelOffset != std::string::npos &&
				eyeAdaptationOffset != std::string::npos &&
				debugDrawOffset != std::string::npos &&
				outputBlitOffset != std::string::npos &&
				renderImGuiOffset != std::string::npos,
				"the post-process, debug overlay, and final output sequence must be explicit");
			const std::string toneMappingTail = renderer.substr(
				eyeAdaptationOffset,
				debugDrawOffset - eyeAdaptationOffset);
			Require(toneMappingTail.find("- src: Secondary") != std::string::npos &&
				toneMappingTail.find("- dst: Main") != std::string::npos,
				"the tone-mapped Secondary target must be copied back to Main before final output");

			const std::string debugDraw = renderer.substr(
				debugDrawOffset,
				outputBlitOffset - debugDrawOffset);
			Require(debugDraw.find("- color: Main") !=
					std::string::npos,
				"DebugDraw must use the HDR-compatible Main surface after MotionBlur");

			const std::string outputBlit = renderer.substr(
				outputBlitOffset,
				renderImGuiOffset - outputBlitOffset);
			Require(outputBlit.find("- src: Main") !=
					std::string::npos &&
				outputBlit.find("- dst: EditorOutput") !=
					std::string::npos,
				"the final blit must preserve the post-MotionBlur debug overlay");
		}

		const std::string editorRenderer = ReadText(
			sourceRoot / "Content/EditorRenderer.renderer");
		const size_t editorOutput = editorRenderer.find("- name: EditorOutput");
		Require(editorOutput != std::string::npos,
			"the editor renderer must declare its transport output");
		const size_t nextRenderTarget = editorRenderer.find("\n- name:", editorOutput + 1u);
		const std::string editorOutputDeclaration = editorRenderer.substr(
			editorOutput,
			nextRenderTarget - editorOutput);
		Require(editorOutputDeclaration.find("format: B8G8R8A8_SRGB") != std::string::npos,
			"the editor transport output must encode linear HDR content to sRGB before IOSurface presentation");
	}

	void TestTransparentBackToFrontOrderingContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		for (const std::filesystem::path rendererPath : {
			sourceRoot / "Content/DefaultRenderer.renderer",
			sourceRoot / "Content/EditorRenderer.renderer" })
		{
			const std::string renderer = ReadText(rendererPath);
			const size_t transparentOffset = renderer.find(
				"- Tag: Transparent");
			Require(transparentOffset != std::string::npos,
				"the renderer must contain a Transparent scene pass");
			const size_t renderTargetsOffset = renderer.find(
				"renderTargets:",
				transparentOffset);
			Require(renderTargetsOffset != std::string::npos,
				"the Transparent pass must declare render targets");
			const std::string settings = renderer.substr(
				transparentOffset,
				renderTargetsOffset - transparentOffset);
			Require(settings.find("- Sorting: BackToFront") !=
					std::string::npos,
				"transparent rendering must explicitly request back-to-front sorting");
			Require(settings.find("- GPUCulling: false") !=
					std::string::npos,
				"the ordered Transparent path must not use order-destroying GPU batch compaction");
		}

		const std::string renderSceneSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/RenderSceneNode.cpp");
		const std::string sortingBody = ExtractFunctionBody(
			renderSceneSource,
			"RHI::ESortingOrder RenderSceneNode::GetSortingOrder() const");
		Require(sortingBody.find("TryGetString(\"Sorting\"") !=
				std::string::npos &&
			sortingBody.find("\n\tGetString(\"Sorting\"") ==
				std::string::npos,
			"render nodes without an explicit sorting parameter must retain the default order safely");
		const std::string prepareBody = ExtractFunctionBody(
			renderSceneSource,
			"Tasks::TaskPtr<void, void> RenderSceneNode::Prepare(");
		Require(prepareBody.find("m_orderedDrawItems.Emplace") !=
				std::string::npos &&
			prepareBody.find("GetViewMatrix()") !=
				std::string::npos &&
			prepareBody.find("-viewCenter.z") !=
				std::string::npos,
			"each transparent mesh instance must retain camera-space center depth");
		Require(prepareBody.find("m_orderedDrawItems.Sort") !=
				std::string::npos &&
			prepareBody.find("lhs.m_cameraDepth > rhs.m_cameraDepth") !=
				std::string::npos &&
			prepareBody.find("lhs.m_staticMeshEcs < rhs.m_staticMeshEcs") !=
				std::string::npos &&
			prepareBody.find("lhs.m_meshIndex < rhs.m_meshIndex") !=
				std::string::npos,
			"transparent instances must use deterministic far-to-near ordering");

		Require(prepareBody.find("m_packet.Add(std::move(item.m_batch)") !=
				std::string::npos &&
			prepareBody.find("m_packet.Finalize(true)") !=
				std::string::npos,
			"the sorted transparent stream must be finalized without batch reordering");

		const std::string batchHeader = ReadText(
			sourceRoot / "Runtime/RHI/Batch.hpp");
		Require(batchHeader.find("firstGroup.m_batch == groups[runEnd].m_batch") !=
				std::string::npos &&
			batchHeader.find("commands->DrawIndexedIndirect") !=
				std::string::npos,
			"only adjacent compatible transparent packet groups may share an MDI run");

		const std::string processBody = ExtractFunctionBody(
			renderSceneSource,
			"void RenderSceneNode::Process(");
		Require(processBody.find("RHIRecordPackedDrawPacket(") !=
				std::string::npos &&
			processBody.find("ProcessBackToFront(") ==
				std::string::npos,
			"transparent and opaque rendering must use the same flight-local packed packet path");
	}

	void TestShadowCasterRenderQueueContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string shadowSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/ShadowPrepassNode.cpp");
		const std::string processBody = ExtractFunctionBody(
			shadowSource,
			"void ShadowPrepassNode::Process(");

		Require(processBody.find("GetHash(std::string(\"Opaque\"))") !=
				std::string::npos &&
			processBody.find("GetHash(std::string(\"Masked\"))") !=
				std::string::npos &&
			processBody.find("renderQueueTag != opaqueQueueTag && renderQueueTag != maskedQueueTag") !=
				std::string::npos,
			"the shadow pass must accept only Opaque and Masked render queues");
		Require(processBody.find("shadowMesh.m_renderQueueTag") !=
				std::string::npos,
			"shadow filtering must use lightweight queue metadata instead of material bindings");

		const std::string sceneViewHeader = ReadText(
			sourceRoot / "Runtime/RHI/SceneView.h");
		const std::string shadowMeshProxy = ExtractFunctionBody(
			sceneViewHeader,
			"struct RHIShadowMeshProxy");
		Require(shadowMeshProxy.find("RHIMeshPtr m_mesh") != std::string::npos &&
			shadowMeshProxy.find("size_t m_renderQueueTag") != std::string::npos &&
			shadowMeshProxy.find("uint32_t m_baseColorSampler") != std::string::npos &&
			shadowMeshProxy.find("float m_alphaCutoff") != std::string::npos &&
			shadowMeshProxy.find("RHIMaterialPtr m_customDepthMaterial") != std::string::npos &&
			shadowMeshProxy.find("RHIShaderBindingSetPtr") == std::string::npos,
			"ordinary shadow casters must retain lightweight alpha-mask metadata while custom depth retains only its immutable RHI identity");

		const std::string staticMeshSource = ReadText(
			sourceRoot / "Runtime/ECS/StaticMeshRendererECS.cpp");
		Require(staticMeshSource.find("shadowMesh.m_renderQueueTag = renderQueueTag") !=
				std::string::npos,
			"dirty mesh updates must cache the render queue in their lightweight shadow proxy");

		const std::string shadowShader = ReadText(
			sourceRoot / "Content/Shaders/ShadowCaster.shader");
		Require(shadowShader.find("- MASKED") != std::string::npos &&
			shadowShader.find("alpha *= texture(") != std::string::npos &&
			shadowShader.find("if(alpha < inAlphaCutoff)") != std::string::npos,
			"masked shadow casters must discard fragments using their base-color alpha texture");
	}

	void TestShadowDepthRangeContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string lightingSource = ReadText(
			sourceRoot / "Content/Shaders/Lighting.glsl");
		const std::string lightingHeader = ReadText(
			sourceRoot / "Runtime/ECS/LightingECS.h");
		const std::string standardShader = ReadText(
			sourceRoot / "Content/Shaders/Standard.shader");
		const std::string landscapeShader = ReadText(
			sourceRoot / "Content/Shaders/Landscape.shader");
		const std::string standardGltfShader = ReadText(
			sourceRoot / "Content/Shaders/Standard_glTF.shader");

		Require(lightingHeader.find("ShadowMapFormat = RHI::EFormat::R16_UNORM") !=
				std::string::npos &&
			lightingHeader.find("ShadowMapFormat = RHI::EFormat::R16_SFLOAT") ==
				std::string::npos,
			"PCF shadow depth must use uniformly distributed fixed-point precision");

		Require(lightingSource.find("projCoords.xy = projCoords.xy * 0.5 + 0.5;") !=
				std::string::npos,
			"shadow sampling must remap only XY from clip space to texture coordinates");
		Require(lightingSource.find("projCoords = projCoords * 0.5 + 0.5;") ==
				std::string::npos,
			"Vulkan zero-to-one shadow depth must not be remapped as OpenGL depth");
		Require(lightingSource.find("const float pcfDepth = texture(shadowMap, sampleUv).r;") !=
				std::string::npos &&
			lightingSource.find("texture(shadowMap, projCoords.xy + offset).r * 0.5 + 0.5") ==
				std::string::npos,
			"PCF must compare raw reverse-Z Vulkan depth values");
		Require(lightingSource.find("any(lessThan(sampleUv, vec2(0.0f)))") !=
				std::string::npos &&
			lightingSource.find("shadow += 1.0f;") != std::string::npos,
			"PCF taps outside a cascade projection must remain lit instead of repeating clamped border depth");
		Require(lightingSource.find("projCoords.z < 0.0f || projCoords.z > 1.0f") !=
				std::string::npos,
			"shadow sampling must reject coordinates outside Vulkan's zero-to-one depth range");
		Require(lightingSource.find("shadowType == SHADOW_TYPE_NONE") !=
				std::string::npos &&
			lightingSource.find("dot(normal, toLight)") !=
				std::string::npos,
			"directional shadow sampling must skip disabled shadows and derive receiver offset from the direction toward the light");
		Require(lightingSource.find(
				"return cascadeLayer == 0 ? lightShadowType : SHADOW_TYPE_PCF;") !=
				std::string::npos &&
			standardShader.find(
				"GetDirectionalCascadeShadowType(light.shadowType, cascadeLayer)") !=
				std::string::npos &&
			standardShader.find(
				"GetDirectionalCascadeShadowType(light.shadowType, nextCascadeLayer)") !=
				std::string::npos &&
			landscapeShader.find(
				"GetDirectionalCascadeShadowType(light.shadowType, cascadeLayer)") !=
				std::string::npos &&
			landscapeShader.find(
				"GetDirectionalCascadeShadowType(light.shadowType, nextCascadeLayer)") !=
				std::string::npos &&
			standardGltfShader.find(
				"GetDirectionalCascadeShadowType(light.shadowType, cascadeLayer)") !=
				std::string::npos &&
			standardGltfShader.find(
				"GetDirectionalCascadeShadowType(light.shadowType, nextCascadeLayer)") !=
				std::string::npos,
			"CSM sampling and blending must interpret the near EVSM map and the wider PCF maps using their actual formats");
		Require(lightingSource.find("float CalculateShadowDistanceFade(") !=
				std::string::npos &&
			lightingSource.find("cascadeLayer != int(safeCascadeCount) - 1") !=
				std::string::npos &&
			standardShader.find("shadow = mix(shadow, 1.0f, shadowDistanceFade);") !=
				std::string::npos &&
			landscapeShader.find("shadow = mix(shadow, 1.0f, shadowDistanceFade);") !=
				std::string::npos &&
			standardGltfShader.find("shadow = mix(shadow, 1.0f, shadowDistanceFade);") !=
				std::string::npos,
			"the final active CSM cascade must fade to unshadowed lighting instead of ending at a visible hard boundary");
		Require(lightingSource.find("SHADOW_RECEIVER_LIGHT_OFFSET") !=
				std::string::npos &&
			lightingSource.find("SHADOW_RECEIVER_NORMAL_OFFSET") != std::string::npos &&
			lightingSource.find("vec3 OffsetDirectionalShadowReceiver(") != std::string::npos &&
			lightingSource.find("normal * (SHADOW_RECEIVER_NORMAL_OFFSET * sinTheta)") != std::string::npos &&
			lightingSource.find("currentDepth + bias") == std::string::npos &&
			lightingSource.find("exp(EVSM_C1 * projCoords.z)") !=
				std::string::npos &&
			lightingSource.find("-exp(-EVSM_C2 * projCoords.z)") !=
				std::string::npos &&
			standardShader.find("cascadeLightMatrix * vec4(shadowReceiverPosition, 1.0f)") != std::string::npos &&
			landscapeShader.find("cascadeLightMatrix * vec4(shadowReceiverPosition, 1.0f)") != std::string::npos &&
			standardGltfShader.find("cascadeLightMatrix * vec4(shadowReceiverPosition, 1.0f)") != std::string::npos,
			"all cascades and material shaders must project the same world-space-offset receiver to avoid split bands");

		const std::string boundsSource = ReadText(
			sourceRoot / "Runtime/Math/Bounds.cpp");
		const std::string projectionBody = ExtractFunctionBody(
			boundsSource,
			"glm::mat4 Frustum::CalculateOrthoMatrixByView(");
		Require(projectionBody.find("glm::orthoRH_ZO") != std::string::npos &&
			projectionBody.find("glm::orthoRH_NO") == std::string::npos,
			"shadow cascade projection and shader sampling must use the same Vulkan depth range");

		const std::string shadowPassSource = ReadText(
			sourceRoot / "Runtime/FrameGraph/ShadowPrepassNode.cpp");
		const std::string processBody = ExtractFunctionBody(
			shadowPassSource,
			"void ShadowPrepassNode::Process(");
		Require(processBody.find("shadowPass.m_shadowType == EShadowType::EVSM") !=
				std::string::npos &&
			processBody.find("glm::vec4(1.0f, 1.0f, -1.0f, 1.0f)") !=
				std::string::npos,
			"empty reverse-Z EVSM texels must be cleared to the moments encoded for depth zero");
		const std::string finalShadowBarrier =
			"commands->ImageMemoryBarrier(commandList, shadowPass.m_shadowMap, EImageLayout::ShaderReadOnlyOptimal);";
		const size_t finalShadowBarrierOffset = processBody.rfind(finalShadowBarrier);
		const size_t blurReleaseOffset = processBody.find("driver->ReleaseTemporaryRenderTarget(blurAttachment);");
		const size_t depthReleaseOffset = processBody.find("driver->ReleaseTemporaryRenderTarget(depthAttachment);");
		Require(finalShadowBarrierOffset != std::string::npos &&
			blurReleaseOffset != std::string::npos &&
			depthReleaseOffset != std::string::npos &&
			blurReleaseOffset < finalShadowBarrierOffset &&
			finalShadowBarrierOffset < depthReleaseOffset,
			"completed PCF and EVSM shadow targets must be published for shader reads in the same graphics command list");

		const std::string standardSource = ReadText(
			sourceRoot / "Content/Shaders/Standard.shader");
		const std::string gltfSource = ReadText(
			sourceRoot / "Content/Shaders/Standard_glTF.shader");
		Require(standardSource.find("CalculateDirectionalShadow(") != std::string::npos &&
			gltfSource.find("CalculateDirectionalShadow(") != std::string::npos &&
			standardSource.find("dot(normal, light.direction)") == std::string::npos &&
			gltfSource.find("dot(normal, light.direction)") == std::string::npos,
			"all lit material paths must use the shared reverse-Z shadow and slope-bias calculation");
	}

	void TestHbaoMeterScaleContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string hbaoSource = ReadText(
			sourceRoot / "Content/Shaders/HBAO.shader");
		const std::string hbaoBlurSource = ReadText(
			sourceRoot / "Content/Shaders/HBAO_Blur.shader");
		const std::string editorRenderer = ReadText(
			sourceRoot / "Content/EditorRenderer.renderer");
		const std::string defaultRenderer = ReadText(
			sourceRoot / "Content/DefaultRenderer.renderer");

		Require(hbaoSource.find("ScreenSpaceToViewSpace(uv, depth, frame.invProjection)") !=
				std::string::npos &&
			hbaoSource.find("ClipSpaceToViewSpace(vec4(uv.x, uv.y, depth") ==
				std::string::npos,
			"HBAO must convert normalized texture coordinates to clip space before reconstructing view positions");
		Require(hbaoSource.find("horizonVectorLengthSquared") != std::string::npos &&
			hbaoSource.find("horizonVectorLength < data.occlusionRadius * data.occlusionRadius") ==
				std::string::npos,
			"HBAO must compare squared distances with its squared world-space radius");
		Require(hbaoSource.find("depth <= 0.000001f") != std::string::npos,
			"HBAO must leave reverse-Z clear-depth sky pixels unoccluded");
		Require(hbaoBlurSource.find("GetViewDepth") != std::string::npos &&
			hbaoBlurSource.find("viewDepth - CenterViewDepth") != std::string::npos,
			"HBAO bilateral blur must compare view-space meter distances rather than nonlinear depth values");
		Require(editorRenderer.find("data.occlusionRadius: 1.5") != std::string::npos &&
			defaultRenderer.find("data.occlusionRadius: 1.5") != std::string::npos &&
			editorRenderer.find("data.occlusionRadius: 700") == std::string::npos &&
			defaultRenderer.find("data.occlusionRadius: 700") == std::string::npos,
			"editor and game HBAO configurations must use the same meter-scale radius");

		const std::string skySource = ReadText(
			sourceRoot / "Content/Shaders/Sky.shader");
		Require(skySource.find("float PlanetHeight(vec3 position)") != std::string::npos &&
			skySource.find("vec2 RaySphereAtAltitude") != std::string::npos &&
			skySource.find("ResolveAtmosphereRayOrigin") != std::string::npos &&
			skySource.find("origin.y = max(origin.y, 0.0f)") == std::string::npos &&
			skySource.find("length(position) - earthRadius") == std::string::npos,
			"near-surface atmosphere math must remain stable for arbitrary camera height without clamping world coordinates");
	}

	void TestPathTracerThicknessSamplerContract()
	{
		Raytracing::Material material{};
		Require(!material.HasThicknessTexture(),
			"a path-tracing material must default to no thickness texture");
		material.m_thicknessIndex = 0;
		material.m_thicknessFactor = 2.0f;
		Require(material.HasThicknessTexture(),
			"a valid thickness texture slot must be detected");

		Raytracing::CombinedSampler2D sampler;
		sampler.Initialize<vec3>(1u, 1u, 3u);
		sampler.SetPixel(0u, 0u, vec3(0.25f, 0.4f, 0.75f));
		const float sampledThickness = material.m_thicknessFactor *
			sampler.Sample<vec3>(vec2(0.5f)).g;
		Require(std::abs(sampledThickness - 0.8f) < 0.0001f,
			"glTF thickness must use the texture's green channel");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string pathTracerSource = ReadText(
			sourceRoot / "Runtime/Raytracing/PathTracer.cpp");
		const std::string materialBuilderBody = ExtractFunctionBody(
			pathTracerSource,
			"bool BuildRaytracingMaterialsFromRuntimeMaterials(");
		Require(materialBuilderBody.find("samplerName == \"thicknessSampler\"") !=
				std::string::npos &&
			materialBuilderBody.find("outMaterial.m_thicknessIndex") !=
				std::string::npos,
			"runtime thickness samplers must be copied into path-tracing materials");

		const std::string materialDataBody = ExtractFunctionBody(
			pathTracerSource,
			"LightingModel::SampledData PathTracer::GetMaterialData(");
		Require(materialDataBody.find("HasThicknessTexture()") !=
				std::string::npos &&
			materialDataBody.find("m_thicknessIndex") !=
				std::string::npos &&
			materialDataBody.find("Sample<vec3>(uv).g") !=
				std::string::npos,
			"path-traced thickness must multiply the factor by the sampled glTF green channel");

		const std::string raytraceBody = ExtractFunctionBody(
			pathTracerSource,
			"vec3 PathTracer::Raytrace(");
		Require(raytraceBody.find("material.m_thicknessFactor > 0.0f") !=
				std::string::npos &&
			raytraceBody.find("sample.m_thicknessFactor > 0.0f") ==
				std::string::npos &&
			raytraceBody.find("IsThickVolumeAtHit") !=
				std::string::npos,
			"thick-volume classification must use the scalar factor while sampled thickness remains available for attenuation");

		const std::string traceSkyBody = ExtractFunctionBody(
			pathTracerSource,
			"vec3 PathTracer::TraceSky(");
		Require(traceSkyBody.find("IsThickVolumeAtHit") !=
				std::string::npos &&
			traceSkyBody.find("GetMaterialData") == std::string::npos,
			"sky traversal must use the lightweight thick-volume hit sampler");

		const std::string thickVolumeBody = ExtractFunctionBody(
			pathTracerSource,
			"bool PathTracer::IsThickVolumeAtHit(");
		Require(thickVolumeBody.find("ResolveHitTextureCoordinates") !=
				std::string::npos &&
			thickVolumeBody.find("HasTransmissionTexture()") !=
				std::string::npos &&
			thickVolumeBody.find("Sample<vec3>") != std::string::npos &&
			thickVolumeBody.find(".r") != std::string::npos &&
			thickVolumeBody.find("HasThicknessTexture()") ==
				std::string::npos &&
			thickVolumeBody.find(".g") == std::string::npos &&
			thickVolumeBody.find("material.m_thicknessFactor > 0.0f") !=
				std::string::npos &&
			thickVolumeBody.find("GetMaterialData") == std::string::npos,
			"the hit predicate must let transmission texture gate the ray without letting thickness texture change the volume type");
	}

	void TestPathTracerMaterialContentRevisionContract()
	{
		auto allocator = Memory::ObjectAllocatorPtr::Make(
			Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		MaterialPtr material = MaterialPtr::Make(allocator, FileId::Invalid);
		TexturePtr texture = TexturePtr::Make(allocator, FileId::Invalid);

		uint64_t revision = material->GetContentRevision();
		const uint64_t globalRevision = Material::GetGlobalContentRevision();
		uint64_t renderMetadataRevision = material->GetRenderMetadataRevision();
		material->SetUniform("material.transmissionFactor", 1.0f);
		Require(material->GetContentRevision() > revision,
			"changing a scalar uniform must advance the material content revision");
		Require(Material::GetGlobalContentRevision() > globalRevision,
			"a material mutation must publish the global revision gate used by dirty mesh updates");
		Require(material->GetRenderMetadataRevision() == renderMetadataRevision,
			"main-pass-only uniforms must not invalidate cached depth, shadow, or scene topology");

		revision = material->GetContentRevision();
		material->SetUniform("material.attenuationColor", glm::vec4(0.9f, 0.6f, 0.1f, 1.0f));
		Require(material->GetContentRevision() > revision,
			"changing a vector uniform must advance the material content revision");
		Require(material->GetRenderMetadataRevision() == renderMetadataRevision,
			"ordinary vector uniforms must remain material-version-only changes");

		revision = material->GetContentRevision();
		material->SetUniform("material.baseColorFactor", glm::vec4(0.5f));
		Require(material->GetContentRevision() > revision &&
			material->GetRenderMetadataRevision() > renderMetadataRevision,
			"masked depth alpha metadata must invalidate cached proxy metadata");
		renderMetadataRevision = material->GetRenderMetadataRevision();

		revision = material->GetContentRevision();
		material->SetUniform(
			"material.baseColorFactor",
			glm::vec4(0.8f, 0.7f, 0.6f, 0.5f));
		Require(material->GetContentRevision() > revision,
			"changing base color RGB must advance the material binding version");
		Require(material->GetRenderMetadataRevision() == renderMetadataRevision,
			"base color RGB must not invalidate alpha-only depth and shadow metadata");

		revision = material->GetContentRevision();
		material->SetUniform(
			"material.baseColorFactor",
			glm::vec4(0.8f, 0.7f, 0.6f, 0.5f));
		Require(material->GetContentRevision() == revision,
			"assigning the same material uniform must not publish a redundant revision");

		revision = material->GetContentRevision();
		material->SetUniform("material.alphaCutoff", 0.5f);
		Require(material->GetContentRevision() > revision,
			"adding an explicit alpha cutoff must advance the material binding version");
		Require(material->GetRenderMetadataRevision() == renderMetadataRevision,
			"the default alpha cutoff must not invalidate equivalent proxy metadata");

		material->SetUniform("material.alphaCutoff", 0.35f);
		Require(material->GetRenderMetadataRevision() > renderMetadataRevision,
			"changing the effective alpha cutoff must invalidate depth and shadow metadata");
		renderMetadataRevision = material->GetRenderMetadataRevision();

		revision = material->GetContentRevision();
		material->SetSampler("thicknessSampler", texture);
		Require(material->GetContentRevision() > revision,
			"changing a sampler must advance the material content revision");
		Require(material->GetRenderMetadataRevision() > renderMetadataRevision,
			"sampler changes must invalidate immutable texture dependency metadata");
		renderMetadataRevision = material->GetRenderMetadataRevision();

		revision = material->GetContentRevision();
		material->SetRenderState(RHI::RenderState(
			true,
			true,
			0.0f,
			false,
			RHI::ECullMode::Back,
			RHI::EBlendMode::None,
			RHI::EFillMode::Fill,
			GetHash(std::string("Transparent"))));
		Require(material->GetContentRevision() > revision,
			"changing render state must advance the material content revision");
		Require(material->GetRenderMetadataRevision() > renderMetadataRevision,
			"changing render state must advance the proxy metadata revision");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string pathTracerSource = ReadText(
			sourceRoot / "Runtime/Raytracing/PathTracer.cpp");
		const std::string signatureBody = ExtractFunctionBody(
			pathTracerSource,
			"size_t ComputeMaterialsSignature(");
		Require(signatureBody.find("GetContentRevision()") !=
				std::string::npos,
			"the path-tracing cache signature must include same-pointer material mutations");

		const std::string materialSource = ReadText(
			sourceRoot / "Runtime/AssetRegistry/Material/MaterialImporter.cpp");
		const std::string hotReloadBody = ExtractFunctionBody(
			materialSource,
			"Tasks::ITaskPtr Material::OnHotReload(");
		Require(hotReloadBody.find("AdvanceContentRevision()") !=
				std::string::npos,
			"dependency hot reloads must invalidate the cached CPU material and texture snapshot");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "MipExtentUsesVulkanFloorAndClamp", TestMipExtentUsesVulkanFloorAndClamp },
		{ "PackedDrawMobilityPayloadVirtualization", TestPackedDrawMobilityPayloadVirtualization },
		{ "MaterialVersionPublicationContract", TestMaterialVersionPublicationContract },
		{ "DynamicSpatialRootIsolation", TestDynamicSpatialRootIsolation },
		{ "InstancedViewLodAndDistanceContract", TestInstancedViewLodAndDistanceContract },
		{ "GpuCullingRangeAndSynchronizationContract", TestGpuCullingRangeAndSynchronizationContract },
		{ "BatchTextureBindingIdentityContract", TestBatchTextureBindingIdentityContract },
		{ "SceneViewProxyMaterialAlignmentContract", TestSceneViewProxyMaterialAlignmentContract },
		{ "ModelHierarchyRenderPropagationContract", TestModelHierarchyRenderPropagationContract },
		{ "RenderResourceVirtualizationContract", TestRenderResourceVirtualizationContract },
		{ "GpuCullingShaderSafetyContract", TestGpuCullingShaderSafetyContract },
		{ "ForwardPlusTileSynchronizationContract", TestForwardPlusTileSynchronizationContract },
		{ "RegionBlitDoesNotPromoteToFullImageCopy", TestRegionBlitDoesNotPromoteToFullImageCopy },
		{ "VulkanMemoryBarrierRecordsPipelineBarrier", TestVulkanMemoryBarrierRecordsPipelineBarrier },
		{ "ShaderReadOnlyBarrierSynchronizesShaderSampling", TestShaderReadOnlyBarrierSynchronizesShaderSampling },
		{ "CommandListImageTrackingPreservesPublishedContents", TestCommandListImageTrackingPreservesPublishedContents },
		{ "TransmissionFramebufferMipContract", TestTransmissionFramebufferMipContract },
		{ "TransmissionFramebufferBindingUsesNodeAttachment", TestTransmissionFramebufferBindingUsesNodeAttachment },
		{ "BakedVolumeScalePerInstanceLayoutContract", TestBakedVolumeScalePerInstanceLayoutContract },
		{ "DepthPrepassSkinningContract", TestDepthPrepassSkinningContract },
		{ "ShadowPrepassSkinningContract", TestShadowPrepassSkinningContract },
		{ "CustomDepthVertexAnimationReachesShadows", TestCustomDepthVertexAnimationReachesShadows },
		{ "VertexDescriptionAttributeIdentityContract", TestVertexDescriptionAttributeIdentityContract },
		{ "GraphicsPipelineAttachmentCacheContract", TestGraphicsPipelineAttachmentCacheContract },
		{ "RenderSceneSurfaceResolveContract", TestRenderSceneSurfaceResolveContract },
		{ "PostProcessPingPongMipIsolationContract", TestPostProcessPingPongMipIsolationContract },
		{ "TransparentBackToFrontOrderingContract", TestTransparentBackToFrontOrderingContract },
		{ "ShadowCasterRenderQueueContract", TestShadowCasterRenderQueueContract },
		{ "ShadowDepthRangeContract", TestShadowDepthRangeContract },
		{ "HbaoMeterScaleContract", TestHbaoMeterScaleContract },
		{ "PathTracerThicknessSamplerContract", TestPathTracerThicknessSamplerContract },
		{ "PathTracerMaterialContentRevisionContract", TestPathTracerMaterialContentRevisionContract },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": " << error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
