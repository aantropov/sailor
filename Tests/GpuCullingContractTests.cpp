#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "FrameGraph/DepthPrepassNode.h"
#include "FrameGraph/RenderSceneNode.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "Core/StringHash.h"
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
#include <yaml-cpp/yaml.h>

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
		Require(input.is_open(), "renderer contract should be readable: " + path.generic_string());
		return std::string(
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	}

	std::string GetFrameGraphSetting(const YAML::Node& pass, const char* setting)
	{
		const YAML::Node settings = pass["string"];
		if (!settings || !settings.IsSequence())
		{
			return {};
		}

		for (const YAML::Node& entry : settings)
		{
			const YAML::Node value = entry[setting];
			if (value && value.IsScalar())
			{
				return value.as<std::string>();
			}
		}

		return {};
	}

	void TestRendererGpuCullingPassContract()
	{
		const std::filesystem::path contentRoot =
			std::filesystem::path(SAILOR_TEST_SOURCE_DIR) / "Content";
		const char* rendererPaths[] =
		{
			"DefaultRenderer.renderer",
			"EditorRenderer.renderer"
		};

		for (const char* rendererPath : rendererPaths)
		{
			const YAML::Node renderer = YAML::Load(ReadText(contentRoot / rendererPath));
			const YAML::Node frame = renderer["frame"];
			Require(frame && frame.IsSequence(),
				std::string(rendererPath) + " should contain a frame graph");

			uint32_t depthPasses = 0u;
			uint32_t mainPasses = 0u;
			for (const YAML::Node& pass : frame)
			{
				const YAML::Node nameNode = pass["name"];
				if (!nameNode || !nameNode.IsScalar())
				{
					continue;
				}

				const std::string name = nameNode.as<std::string>();
				const std::string tag = GetFrameGraphSetting(pass, "Tag");
				if (name == "DepthPrepass" && (tag == "Opaque" || tag == "Masked"))
				{
					++depthPasses;
					Require(GetFrameGraphSetting(pass, "GPUCulling") == "false",
						std::string(rendererPath) + " must keep GPU culling disabled in " + tag +
						" depth prepass");
					Require(GetFrameGraphSetting(pass, "VirtualizeInstancePayloads") == "true",
						std::string(rendererPath) + " must keep instance virtualization enabled in " + tag +
						" depth prepass");
				}
				else if (name == "RenderScene" && (tag == "Opaque" || tag == "Masked"))
				{
					++mainPasses;
					Require(GetFrameGraphSetting(pass, "GPUCulling") == "true",
						std::string(rendererPath) + " must keep GPU culling enabled in " + tag +
						" main pass");
					Require(GetFrameGraphSetting(pass, "VirtualizeInstancePayloads") == "true",
						std::string(rendererPath) + " must keep instance virtualization enabled in " + tag +
						" main pass");
				}
			}

			Require(depthPasses == 2u && mainPasses == 2u,
				std::string(rendererPath) +
				" should expose opaque and masked depth/main pass pairs");
		}
	}

	void TestMotionMrtFrameGraphContract()
	{
		for (const char* rendererPath : { "DefaultRenderer.renderer", "EditorRenderer.renderer" })
		{
			const auto scalar = [rendererPath](const YAML::Node& node, const char* key)
			{
				const auto value = node[key];
				Require(value && value.IsScalar(), std::string(rendererPath) + ": expected scalar " + key + " in " + YAML::Dump(node));
				return value.as<std::string>();
			};
			const auto renderer = YAML::Load(ReadText(std::filesystem::path(SAILOR_TEST_SOURCE_DIR) / "Content" / rendererPath));
			YAML::Node main, motion;
			const YAML::Node targets = renderer["renderTargets"];
			Require(targets && targets.IsSequence() && targets.size() > 0u, std::string(rendererPath) + ": expected render targets at " + SAILOR_TEST_SOURCE_DIR);
			for (const YAML::Node& target : targets)
			{
				if (scalar(target, "name") == "Main") main.reset(target);
				if (scalar(target, "name") == "MotionVectors") motion.reset(target);
			}
			Require(main.IsMap() && motion.IsMap() && scalar(motion, "format") == "R16G16B16A16_SFLOAT",
				std::string(rendererPath) + ": motion MRT must have signed floating-point velocity, depth and coverage channels; Main=" + YAML::Dump(main) + "; MotionVectors=" + YAML::Dump(motion));
			Require(scalar(main, "width") == scalar(motion, "width") &&
				scalar(main, "height") == scalar(motion, "height") &&
				main["bIsSurface"].as<bool>() == motion["bIsSurface"].as<bool>(),
				"colour and motion attachments must share render extent and MSAA surface policy");
			const auto resource = [](const YAML::Node& pass, const char* key)
			{
				for (const auto& entry : pass["renderTargets"])
					if (const auto value = entry[key]; value && value.IsScalar()) return value.as<std::string>();
				return std::string{};
			};
			bool cleared = false, consumed = false;
			uint32_t producers = 0u;
			for (const auto& pass : renderer["frame"])
			{
				const auto name = scalar(pass, "name");
				if (name == "Clear" && resource(pass, "target") == "MotionVectors") cleared = true;
				if (name == "DepthPrepass") Require(resource(pass, "motionVectors").empty(), "depth prepasses must not render motion geometry");
				const auto tag = GetFrameGraphSetting(pass, "Tag");
				if (name == "RenderScene" && (tag == "Opaque" || tag == "Masked" || tag == "Transparent"))
				{
					Require(cleared && !consumed && resource(pass, "color") == "Main" && resource(pass, "motionVectors") == "MotionVectors",
						"each ordinary scene pass must produce colour and motion together after clearing and before blur");
					++producers;
				}
				if (GetFrameGraphSetting(pass, "shader") == "Shaders/MotionBlur.shader")
				{
					Require(producers == 3u && resource(pass, "motionSampler") == "MotionVectors", "blur must consume all three MRT scene queues");
					consumed = true;
				}
			}
			Require(consumed && producers == 3u, "renderer must expose a complete per-object motion pipeline");
		}
	}

	void TestPcfRasterShadowBiasContract()
	{
		constexpr float configuredBias = 1.25f;
		Require(
			std::abs(ShadowPrepassNode::GetRasterShadowBias(
				RHI::EShadowType::PCF,
				configuredBias) - configuredBias) < 0.0001f,
			"the configured raster bias must apply to PCF shadow maps");
		Require(
			ShadowPrepassNode::GetRasterShadowBias(
				RHI::EShadowType::EVSM,
				configuredBias) == 0.0f,
			"EVSM shadow maps must retain their unbiased rasterization path");
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

		Require(sizeof(renderInstance) == 192u &&
			sizeof(depthInstance) == 96u &&
			sizeof(customDepthInstance) == 192u &&
			sizeof(shadowInstance) == 128u &&
			renderScaleOffset == 96u &&
			customDepthScaleOffset == 96u &&
			shadowScaleOffset == 96u &&
			shadowAlphaOffset == 112u &&
			renderScaleOffset + sizeof(vec4) + sizeof(RHI::RHIObjectMotionData) == sizeof(renderInstance),
			"main, ordinary depth, custom depth, and shadow passes must keep their independent std430 instance layouts");

		RHI::RHIMesh mesh;
		Require(mesh.m_bakedVolumeScale == glm::vec3(1.0f) &&
			renderInstance.bakedVolumeScale == glm::vec4(1.0f) &&
			customDepthInstance.bakedVolumeScale == glm::vec4(1.0f) &&
			shadowInstance.bakedVolumeScale == glm::vec4(1.0f),
			"procedural and legacy meshes must default to an identity baked volume scale");

	}

	void TestDepthPrepassSkinningContract()
	{
		const RHI::RHISceneViewProxy unskinnedProxy{};
		Require(unskinnedProxy.m_skeletonOffset ==
			(std::numeric_limits<uint32_t>::max)(),
			"scene proxies without an animator must use the invalid skeleton offset so meshes with unused bone attributes stay rigid in the depth pass");

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
			"Transparent"_h.GetHash()));
		Require(material->GetContentRevision() > revision,
			"changing render state must advance the material content revision");
		Require(material->GetRenderMetadataRevision() > renderMetadataRevision,
			"changing render state must advance the proxy metadata revision");

	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "RendererGpuCullingPassContract", TestRendererGpuCullingPassContract },
		{ "MotionMrtFrameGraphContract", TestMotionMrtFrameGraphContract },
		{ "PcfRasterShadowBiasContract", TestPcfRasterShadowBiasContract },
		{ "MipExtentUsesVulkanFloorAndClamp", TestMipExtentUsesVulkanFloorAndClamp },
		{ "PackedDrawMobilityPayloadVirtualization", TestPackedDrawMobilityPayloadVirtualization },
		{ "MaterialVersionPublicationContract", TestMaterialVersionPublicationContract },
		{ "DynamicSpatialRootIsolation", TestDynamicSpatialRootIsolation },
		{ "InstancedViewLodAndDistanceContract", TestInstancedViewLodAndDistanceContract },
		{ "BatchTextureBindingIdentityContract", TestBatchTextureBindingIdentityContract },
		{ "RenderResourceVirtualizationContract", TestRenderResourceVirtualizationContract },
		{ "ShaderReadOnlyBarrierSynchronizesShaderSampling", TestShaderReadOnlyBarrierSynchronizesShaderSampling },
		{ "BakedVolumeScalePerInstanceLayoutContract", TestBakedVolumeScalePerInstanceLayoutContract },
		{ "DepthPrepassSkinningContract", TestDepthPrepassSkinningContract },
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
