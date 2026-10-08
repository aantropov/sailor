#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "FrameGraph/DepthPrepassNode.h"
#include "FrameGraph/RenderSceneNode.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "FrameGraph/AtmosphericFogNode.h"
#include "FrameGraph/BlitNode.h"
#include "FrameGraph/BloomNode.h"
#include "FrameGraph/CopyTextureToRamNode.h"
#include "FrameGraph/EnvironmentNode.h"
#include "FrameGraph/EyeAdaptationNode.h"
#include "FrameGraph/PostProcessNode.h"
#include "GraphicsDriver/Vulkan/VulkanPipileneStates.h"
#include "Settings/GraphicsSettings.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "RHI/GpuCulling.h"
#include "Core/StringHash.h"
#include "Raytracing/MaterialUtils.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Material.h"
#include "RHI/MaterialPreparationCache.h"
#include "RHI/Mesh.h"
#include "RHI/Texture.h"
#include "RHI/VertexDescription.h"
#include "Support/FrameGraphContract.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <thread>
#include <vector>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
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

	class ShadowPrepassNodeProbe : public ShadowPrepassNode
	{
	public:
		using MaterialKey = CustomShadowMaterialKey;
	};

	void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
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

	std::string GetFrameGraphSetting(const YAML::Node& pass, std::string_view setting)
	{
		return Tests::GetSequenceMapping(pass["string"], setting);
	}

	std::string GetFrameGraphAttachment(const YAML::Node& pass, std::string_view name)
	{
		return Tests::GetSequenceMapping(pass["renderTargets"], name);
	}

	void TestFrameGraphSequenceMappings()
	{
		const auto document = YAML::Load(R"(
string:
  - Tag: Opaque
  - ignored
  - Shader: shaders/Lit.shader
  - Malformed: [one, two]
renderTargets:
  - src: DepthBuffer
  - dst: DepthPyramid
)");
		Require(GetFrameGraphSetting(document, "Tag") == "Opaque" &&
			GetFrameGraphSetting(document, "Shader") == "shaders/Lit.shader",
			"parsed pass settings must retain their scalar text");
		Require(GetFrameGraphAttachment(document, "src") == "DepthBuffer" &&
			GetFrameGraphAttachment(document, "dst") == "DepthPyramid",
			"parsed attachment mappings must retain resource identity");
		Require(GetFrameGraphSetting(document, "Absent").empty() &&
			GetFrameGraphSetting(document, "Malformed").empty() &&
			Tests::GetSequenceMapping(YAML::Node{}, "Tag").empty() &&
			Tests::GetSequenceMapping(document, "Tag").empty(),
			"missing or non-scalar sequence mappings must not become valid contract values");
	}

	void TestCurrentDepthPyramidReadiness()
	{
		RHI::RHIFrameGraph graph;
		auto pyramid = RHI::RHITexturePtr::Make(
			RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, false);
		auto replacement = RHI::RHITexturePtr::Make(
			RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, false);
		Require(!graph.HasCurrentDepthPyramid(pyramid),
			"an allocated texture is not evidence of current-view depth production");
		graph.MarkCurrentDepthPyramid({});
		Require(!graph.HasCurrentDepthPyramid({}), "missing depth must fail open");
		graph.MarkCurrentDepthPyramid(pyramid);
		Require(graph.HasCurrentDepthPyramid(pyramid), "recorded depth must become available");
		Require(!graph.HasCurrentDepthPyramid(replacement),
			"a resized or different view target must not inherit readiness");
		graph.ResetCurrentDepthPyramids();
		Require(!graph.HasCurrentDepthPyramid(pyramid),
			"another view or a skipped producer must not reuse stale depth");
		graph.MarkCurrentDepthPyramid(replacement);
		graph.Clear();
		Require(!graph.HasCurrentDepthPyramid(replacement),
			"clearing the frame graph must invalidate recorded depth");
	}

	void TestQueueShaderStagesUseCapabilities()
	{
		VulkanQueueFamilyIndices queues;
		queues.m_graphicsFamily = 0u;
		queues.m_computeFamily = 1u;
		queues.m_transferFamily = 2u;
		queues.m_familyFlags = {
			VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
			VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
			VK_QUEUE_TRANSFER_BIT };
		Require(VulkanCommandBuffer::GetShaderPipelineStages(queues.GetFlags(0u)) ==
			(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
			"graphics-list compute writes need compute-stage barriers even with a separate compute queue");
		Require(VulkanCommandBuffer::GetShaderPipelineStages(queues.GetFlags(1u)) ==
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			"a dedicated compute queue must not receive graphics stages");
		Require(VulkanCommandBuffer::GetShaderPipelineStages(queues.GetFlags(2u)) == 0u &&
			VulkanCommandBuffer::GetShaderPipelineStages(queues.GetFlags(99u)) == 0u,
			"transfer-only and unknown queues must not advertise shader stages");
	}

	void TestGpuCullingDispatchOrdering()
	{
		struct Event
		{
			bool m_dispatch = false;
			RHI::EAccessFlags m_src{};
			RHI::EAccessFlags m_dst{};
			uint32_t m_groups{};
			RHI::GpuCullingPushConstants m_constants{};
		};
		struct Recorder
		{
			TVector<Event> m_events;
			void MemoryBarrier(RHI::RHICommandListPtr, RHI::EAccessFlags src, RHI::EAccessFlags dst)
			{
				m_events.Add(Event{ false, src, dst, 0u, {} });
			}
			void Dispatch(RHI::RHICommandListPtr, RHI::RHIShaderPtr, uint32_t x,
				uint32_t y, uint32_t z, const TVector<RHI::RHIShaderBindingSetPtr>&,
				const void* data, size_t size)
			{
				Require(y == 1u && z == 1u && size == sizeof(RHI::GpuCullingPushConstants),
					"culling must dispatch one-dimensional work with the complete push layout");
				Event event;
				event.m_dispatch = true;
				event.m_groups = x;
				std::memcpy(&event.m_constants, data, size);
				m_events.Add(event);
			}
		};

		for (bool sameList : { false, true })
		{
			Recorder recorder;
			RHI::GpuCullingPushConstants constants{ 513u, 1025u, 17u, 31u, 1059u, 99u, sameList ? 1u : 0u };
			RHI::RecordGpuCullingDispatches(recorder, {}, {}, {}, constants, 256u, sameList);
			const auto& events = recorder.m_events;
			Require(events.Num() == (sameList ? 5u : 4u),
				"same-list draws need an additional shader-to-draw dependency");
			Require(!events[0].m_dispatch && events[1].m_dispatch &&
				!events[2].m_dispatch && events[3].m_dispatch,
				"uploads must precede culling and culling writes must precede compaction");
			Require(events[1].m_groups == 5u && events[3].m_groups == 3u,
				"culling and compaction must cover different instance/batch counts");
			Require(events[0].m_src == (static_cast<RHI::EAccessFlags>(RHI::EAccessBit::TransferWrite_Bit) |
					static_cast<RHI::EAccessFlags>(RHI::EAccessBit::HostWrite_Bit)) &&
				events[0].m_dst == (static_cast<RHI::EAccessFlags>(RHI::EAccessBit::ShaderRead_Bit) |
					static_cast<RHI::EAccessFlags>(RHI::EAccessBit::ShaderWrite_Bit)) &&
				events[2].m_src == static_cast<RHI::EAccessFlags>(RHI::EAccessBit::ShaderWrite_Bit) &&
				events[2].m_dst == events[0].m_dst,
				"upload and inter-dispatch barriers must expose their actual writes");
			for (uint32_t phase = 0u; phase != 2u; ++phase)
			{
				auto expected = constants;
				expected.m_phase = phase;
				Require(std::memcmp(&events[1u + phase * 2u].m_constants, &expected, sizeof(expected)) == 0,
					"both dispatches must retain flight-local offsets and the requested occlusion mode");
			}
			if (sameList)
			{
				Require(!events[4].m_dispatch &&
					events[4].m_src == static_cast<RHI::EAccessFlags>(RHI::EAccessBit::ShaderWrite_Bit) &&
					events[4].m_dst == (static_cast<RHI::EAccessFlags>(RHI::EAccessBit::ShaderRead_Bit) |
						static_cast<RHI::EAccessFlags>(RHI::EAccessBit::IndirectCommandRead_Bit)),
					"compacted indices and indirect counts must be visible before graphics drawing");
			}
		}
	}

	void TestDepthAttachmentFrameGraphContract()
	{
		for (const char* rendererPath : { "DefaultRenderer.renderer", "EditorRenderer.renderer", "ExperimentalRenderer.renderer" })
		{
			const auto renderer = YAML::LoadFile(
				(std::filesystem::path(SAILOR_TEST_SOURCE_DIR) / "Content" / rendererPath).string());
			TMap<std::string, YAML::Node> targets;
			for (const auto& target : renderer["renderTargets"])
			{
				targets[target["name"].as<std::string>()].reset(target);
			}

			for (const auto& pass : renderer["frame"])
			{
				const auto depthName = GetFrameGraphAttachment(pass, "depthStencil");
				if (depthName.empty())
				{
					continue;
				}
				Require(targets.ContainsKey(depthName),
					std::string(rendererPath) + " must declare consumed depth attachment " + depthName);
				const auto& depth = targets[depthName];
				Require(depth["format"].as<std::string>() == "D32_SFLOAT_S8_UINT",
					"engine graph depth attachments must retain their depth/stencil format");

				const auto colorName = GetFrameGraphAttachment(pass, "color");
				if (colorName.empty())
				{
					continue;
				}
				if (colorName == "BackBuffer")
				{
					Require(depth["width"].as<std::string>() == "ViewportWidth" &&
						depth["height"].as<std::string>() == "ViewportHeight",
						"back-buffer depth must use the viewport extent, not the scaled render extent");
				}
				else
				{
					Require(targets.ContainsKey(colorName), "the paired color attachment must be declared");
					const auto& color = targets[colorName];
					Require(depth["width"].as<std::string>() == color["width"].as<std::string>() &&
						depth["height"].as<std::string>() == color["height"].as<std::string>(),
						"depth and color attachments in the same pass must share their extent");
				}
			}
		}
	}

	void TestRendererGpuCullingPassContract()
	{
		const std::filesystem::path contentRoot =
			std::filesystem::path(SAILOR_TEST_SOURCE_DIR) / "Content";
		for (const char* rendererPath : { "DefaultRenderer.renderer", "EditorRenderer.renderer", "ExperimentalRenderer.renderer" })
		{
			const auto renderer = YAML::LoadFile((contentRoot / rendererPath).string());
			uint32_t pyramids = 0;
			for (const auto& target : renderer["renderTargets"])
			{
				if (target["name"].as<std::string>() != "DepthHighZ") continue;
				Require(target["reduction"].as<std::string>("Average") == "Average" && target["bGenerateMips"].as<bool>() &&
					target["bIsCompatibleWithComputeShaders"].as<bool>() && target["format"].as<std::string>() == "R32_SFLOAT",
					"the explicitly reduced depth pyramid must not require optional hardware min/max filtering");
				++pyramids;
			}
			Require(pyramids == 1, std::string(rendererPath) + " must declare one depth pyramid");
		}
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
			uint32_t pyramidPasses = 0u;
			std::string currentDepthPyramid;
			for (const YAML::Node& pass : frame)
			{
				const YAML::Node nameNode = pass["name"];
				if (!nameNode || !nameNode.IsScalar())
				{
					continue;
				}

				const std::string name = nameNode.as<std::string>();
				const std::string tag = GetFrameGraphSetting(pass, "Tag");
				if (name == "DepthHighZ")
				{
					++pyramidPasses;
					Require(depthPasses == pyramidPasses && pyramidPasses <= 2u && mainPasses == 0u,
						"Hi-Z must follow opaque depth, then refresh after masked depth before main drawing");
					Require(GetFrameGraphAttachment(pass, "src") == "DepthBuffer",
						"Hi-Z must reduce full-resolution depth, not a nearest-filtered depth blit");
					currentDepthPyramid = GetFrameGraphAttachment(pass, "dst");
					Require(!currentDepthPyramid.empty(), "Hi-Z must publish a named target");
				}
				if (GetFrameGraphSetting(pass, "OcclusionCulling") == "true")
				{
					Require((name == "RenderScene" && (tag == "Opaque" || tag == "Masked")) ||
						(name == "DepthPrepass" && tag == "Masked"),
						"only masked depth and opaque/masked main drawing may consume current-frame occlusion");
				}
				if (name == "DepthPrepass" && (tag == "Opaque" || tag == "Masked"))
				{
					++depthPasses;
					if (tag == "Opaque")
					{
						Require(depthPasses == 1u && pyramidPasses == 0u &&
							GetFrameGraphSetting(pass, "GPUCulling") == "false" &&
							GetFrameGraphSetting(pass, "OcclusionCulling") != "true",
							"opaque depth must seed current-frame occlusion without sampling stale depth");
					}
					else
					{
						Require(depthPasses == 2u && pyramidPasses == 1u &&
							GetFrameGraphSetting(pass, "GPUCulling") == "true" &&
							GetFrameGraphSetting(pass, "OcclusionCulling") == "true" &&
							!currentDepthPyramid.empty() &&
							GetFrameGraphAttachment(pass, "depthHighZ") == currentDepthPyramid,
							"masked depth must cull against the preceding opaque depth pyramid");
					}
					Require(GetFrameGraphSetting(pass, "VirtualizeInstancePayloads") == "true",
						std::string(rendererPath) + " must keep instance virtualization enabled in " + tag +
						" depth prepass");
					currentDepthPyramid.clear();
				}
				else if (name == "RenderScene" && (tag == "Opaque" || tag == "Masked"))
				{
					++mainPasses;
					Require(GetFrameGraphSetting(pass, "OcclusionCulling") == "true" &&
						!currentDepthPyramid.empty() &&
						GetFrameGraphAttachment(pass, "depthHighZ") == currentDepthPyramid,
						"main-pass occlusion must consume the preceding current-frame depth pyramid");
					Require(GetFrameGraphSetting(pass, "GPUCulling") == "true",
						std::string(rendererPath) + " must keep GPU culling enabled in " + tag +
						" main pass");
					Require(GetFrameGraphSetting(pass, "VirtualizeInstancePayloads") == "true",
						std::string(rendererPath) + " must keep instance virtualization enabled in " + tag +
						" main pass");
				}
			}

			Require(depthPasses == 2u && pyramidPasses == 2u && mainPasses == 2u,
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
					Require(name == "MotionBlur" && !consumed && producers == 3u && resource(pass, "motionSampler") == "MotionVectors",
						"the motion blur node must own temporal preparation and consume all three MRT scene queues");
					consumed = true;
				}
			}
			Require(consumed && producers == 3u, "renderer must expose a complete per-object motion pipeline");
		}
	}

	void TestPcfRasterShadowBiasContract()
	{
		constexpr float configuredBias = 1.25f;
		constexpr float receiverDepth = 0.75f;
		constexpr float depthUnit = 1.0f / 16777216.0f;
		const auto biasedCasterDepth = [=](float casterDepth, float bias)
		{
			return casterDepth + depthUnit * ShadowPrepassNode::GetRasterShadowBias(
				RHI::EShadowType::PCF, bias);
		};

		// A coplanar receiver must stop shadowing itself after the caster offset.
		const float selfShadowDepth = receiverDepth;
		Require(receiverDepth <= biasedCasterDepth(selfShadowDepth, 0.0f) &&
			receiverDepth > biasedCasterDepth(selfShadowDepth, configuredBias),
			"positive bias must reduce self-shadowing with reverse-Z depth comparison");
		Require(biasedCasterDepth(selfShadowDepth, 0.0f) == selfShadowDepth,
			"zero configured bias must preserve the caster depth");

		const float initiallyLitDepth = receiverDepth - depthUnit;
		Require(receiverDepth > initiallyLitDepth &&
			receiverDepth <= biasedCasterDepth(initiallyLitDepth, -configuredBias),
			"negative configured bias must reverse the caster offset toward the light");
		Require(receiverDepth < biasedCasterDepth(0.8f, configuredBias),
			"the default raster bias must preserve a separated blocker shadow");

		for (float bias : { -configuredBias, 0.0f, configuredBias })
		{
			Require(ShadowPrepassNode::GetRasterShadowBias(
				RHI::EShadowType::EVSM, bias) == 0.0f,
				"EVSM shadow maps must retain their unbiased rasterization path");
		}
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

	void TestPackedDrawBatchInstanceLimit()
	{
		struct TestInstance
		{
			uint32_t m_value = 0u;
		};
		constexpr uint32_t limit = RHI::RHIBatch::MaxInstancesPerBatch;
#if defined(_WIN32)
		Require(limit == 2048u, "Windows draw batches must be limited to 2048 instances");
#endif
		auto validate = [&](const RHI::TPackedDrawPacket<TestInstance>& packet, uint32_t count)
		{
			Require(packet.GetNumDrawInstances() == count,
				"batch splitting must not drop or duplicate instances");
			const auto& groups = packet.GetGroups();
			uint32_t firstInstance = 0u;
			for (const auto& group : groups)
			{
				Require(group.m_numInstances > 0u && group.m_numInstances <= limit &&
					group.m_firstInstance == firstInstance,
					"indirect commands must cover contiguous, bounded instance ranges");
				firstInstance += group.m_numInstances;
			}
			Require(firstInstance == count, "indirect ranges must cover the complete packet");
			for (uint32_t begin = 0u; begin < groups.Num();)
			{
				const uint32_t end = RHI::GetPackedDrawRunEnd(groups, begin);
				Require(end > begin && end <= groups.Num(), "draw runs must make bounded progress");
				uint64_t numInstances = 0u;
				for (uint32_t index = begin; index < end; ++index)
				{
					numInstances += groups[index].m_numInstances;
				}
				Require(numInstances <= limit, "indirect submission must not rejoin oversized batches");
				begin = end;
			}
		};

		for (uint32_t count : { 0u, 1u, 2047u, 2048u, 2049u, 4096u, 4097u, 40001u })
		{
			RHI::TPackedDrawPacket<TestInstance> packet;
			for (uint32_t index = count; index > 0u; --index)
			{
				packet.Add({}, {}, { index - 1u }, index - 1u, EMobilityType::Static);
			}
			packet.Finalize(false);
			validate(packet, count);
			Require(packet.GetGroups().Num() == count / limit + (count % limit != 0u),
				"identical instances must split only at the platform batch limit");
			for (uint32_t index = 0u; index < count; ++index)
			{
				Require(packet.GetPayload(EMobilityType::Static).m_instances[index].m_value == index &&
					packet.GetInstanceIndices()[index] == index,
					"batch boundaries must preserve sorted payload and index correspondence");
			}

			RHI::TPackedDrawPacket<TestInstance> nextFlight;
			RHI::TPackedDrawPagedArenaCache<TestInstance> cache;
			TVector<uint64_t> keys;
			for (uint32_t index = 0; index < count; ++index) keys.Add(index);
			cache.BeginUpdate(1u, 1u, 1u);
			Require(cache.ReplaceRange(1u, 1u, packet.GetPayload(EMobilityType::Static).m_instances, keys),
				"the active arena must publish the complete sorted instance range");
			nextFlight.UseSharedArenaPayload(EMobilityType::Static, cache.EndUpdate());
			for (uint32_t index = 0; index < count; ++index)
				Require(nextFlight.AddArenaView({}, {}, 1u, index), "every visible arena key must resolve");
			nextFlight.Finalize(false);
			validate(nextFlight, count);
			Require(nextFlight.GetInstanceIndices() == packet.GetInstanceIndices(),
				"shared payload reuse must retain split command offsets across flights");
		}

		constexpr uint32_t orderedCount = 4097u;
		RHI::TPackedDrawPacket<TestInstance> ordered;
		for (uint32_t index = 0u; index < orderedCount; ++index)
		{
			ordered.Add({}, {}, { orderedCount - index }, orderedCount - index);
		}
		ordered.Finalize(true);
		validate(ordered, orderedCount);
		for (uint32_t index = 0u; index < orderedCount; ++index)
		{
			Require(ordered.GetPayload(EMobilityType::Dynamic).m_instances[index].m_value == orderedCount - index,
				"transparent draw splitting must preserve the supplied back-to-front order");
		}

		RHI::TPackedDrawPagedArenaCache<TestInstance> arena;
		TVector<TestInstance> arenaInstances;
		TVector<uint64_t> arenaKeys;
		for (uint32_t index = 0u; index < orderedCount * 2u; ++index)
		{
			arenaInstances.Add({ index });
			arenaKeys.Add(index);
		}
		arena.BeginUpdate(1u, 1u, 1u);
		Require(arena.ReplaceRange(1u, 1u, arenaInstances, arenaKeys), "the arena must store visible and invisible records");
		RHI::TPackedDrawPacket<TestInstance> view;
		view.UseSharedArenaPayload(EMobilityType::Static, arena.EndUpdate());
		for (uint32_t index = orderedCount; index > 0u; --index)
		{
			Require(view.AddArenaView({}, {}, 1u, (index - 1u) * 2u), "visible keys must resolve");
		}
		view.Finalize(false);
		validate(view, orderedCount);
		Require(view.GetNumStorageInstances() == 16384u,
			"splitting must retain all 8194 records in the shared arena's reserved range");
		for (uint32_t index = 0u; index < orderedCount; ++index)
		{
			Require(view.GetInstanceIndices()[index] == index * 2u,
				"split arena commands must retain sparse visible-to-storage indices");
		}

		TVector<RHI::PackedDrawGroup> textureLimited;
		textureLimited.Resize(3u);
		for (auto& group : textureLimited)
		{
			group.m_numInstances = 1u;
			group.m_batch.m_supportedMeshesPerBatch = 2u;
		}
		Require(RHI::GetPackedDrawRunEnd(textureLimited, 0u) == 2u,
			"a smaller platform texture/command limit must still take precedence");
	}

	void TestPackedDrawMixedMaterialSort()
	{
		struct Instance { uint32_t m_id = 0u; };
		std::array<RHI::RHIMaterialPtr, 4> materials;
		for (auto& material : materials)
		{
			material = RHI::RHIMaterialPtr::Make(
				RHI::RenderState{}, RHI::RHIShaderPtr{}, RHI::RHIShaderPtr{});
		}
		std::array<RHI::RHIMeshPtr, 2> meshes{
			RHI::RHIMeshPtr::Make(), RHI::RHIMeshPtr::Make() };
		std::array<RHI::RHIShaderBindingSetPtr, 2> textures{
			RHI::RHIShaderBindingSetPtr::Make(), RHI::RHIShaderBindingSetPtr::Make() };
		RHI::TPackedDrawPacket<Instance> packet;
		constexpr uint32_t count = 4097u;
		for (uint32_t flight = 0u; flight < 2u; ++flight)
		{
			packet.Reset();
			for (auto& material : materials)
			{
				material->SetBindings(RHI::RHIShaderBindingSetPtr::Make());
			}
			for (uint32_t index = 0u; index < count; ++index)
			{
				const uint32_t id = (index * 37u) % count;
				const auto& mesh = meshes[(id / materials.size()) % meshes.size()];
				RHI::RHIBatch batch(materials[id % materials.size()], mesh);
				batch.m_textureBindings = textures[(id / (materials.size() * meshes.size())) % textures.size()];
				const uint64_t stableKey = (uint64_t(id) << 40u) | (count - id);
				packet.Add(std::move(batch), mesh, {id}, stableKey);
			}
			packet.Finalize(false);
			std::array<bool, count> seen{};
			uint32_t visited = 0u;
			for (const auto& group : packet.GetGroups())
			{
				uint32_t previousId = 0u;
				for (uint32_t offset = 0u; offset < group.m_numInstances; ++offset)
				{
					const uint32_t storageIndex = packet.GetInstanceIndices()[group.m_firstInstance + offset];
					const uint32_t id = packet.GetPayload(EMobilityType::Dynamic).m_instances[storageIndex].m_id;
					Require(id < count && !seen[id], "sorted packets must retain every instance exactly once");
					seen[id] = true;
					++visited;
					Require(group.m_batch.m_materialVersion == materials[id % materials.size()]->GetVersion() &&
						group.m_mesh == meshes[(id / materials.size()) % meshes.size()] &&
						group.m_batch.m_textureBindings == textures[(id / (materials.size() * meshes.size())) % textures.size()],
						"sorting and packet reuse must preserve each instance's mesh, textures and material generation");
					Require(offset == 0u || previousId < id, "instances sharing a draw must retain stable key order");
					previousId = id;
				}
			}
			Require(visited == count, "all mixed-material instances must reach the draw packet");
		}
	}

	void TestPackedDrawEqualKeysPreserveBindingOrder()
	{
		std::array<RHI::RHIMaterialPtr, 2u> materials;
		for (auto& material : materials)
		{
			material = RHI::RHIMaterialPtr::Make(
				RHI::RenderState{}, RHI::RHIShaderPtr{}, RHI::RHIShaderPtr{});
		}
		materials[0]->SetBindings(RHI::RHIShaderBindingSetPtr::Make());
		const auto version = materials[0]->GetVersion();
		RHI::TPackedDrawPacket<uint32_t> packet;
		for (uint32_t index = 0u; index < 32u; ++index)
		{
			RHI::RHIBatch batch(materials[index % materials.size()], {});
			batch.m_materialVersion = version;
			packet.Add(std::move(batch), {}, index, 17ull);
		}
		packet.Finalize(false);
		Require(packet.GetGroups().Num() == 32u,
			"equal sort keys must not merge distinct binding owners across intervening draws");
		for (uint32_t index = 0u; index < 32u; ++index)
		{
			Require(packet.GetPayload(EMobilityType::Dynamic).m_instances[index] == index &&
				packet.GetGroups()[index].m_batch.m_material == materials[index % materials.size()] &&
				packet.GetGroups()[index].m_batch.m_materialVersion == version,
				"equal sort keys must preserve insertion order and each draw's retained binding owner");
		}
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

		RHI::TPackedDrawPagedArenaCache<TestInstance> sharedCache;
		sharedCache.BeginUpdate(7u, 101u, 1u);
		Require(sharedCache.ReplaceRange(1u, 1u, { { 10u } }, { 10ull }), "static range must publish");
		auto staticPayload = sharedCache.EndUpdate();
		sharedCache.BeginUpdate(8u, 101u, 1u);
		Require(sharedCache.ReplaceRange(2u, 1u, { { 20u } }, { 20ull }), "stationary range must publish");
		auto stationaryPayload = sharedCache.EndUpdate();

		RHI::TPackedDrawPacket<TestInstance> nextFlight;
		nextFlight.UseSharedArenaPayload(EMobilityType::Static, staticPayload);
		nextFlight.UseSharedArenaPayload(EMobilityType::Stationary, stationaryPayload);
		Require(nextFlight.AddArenaView({}, {}, 1u, 10ull, EMobilityType::Static) &&
			nextFlight.AddArenaView({}, {}, 2u, 20ull, EMobilityType::Stationary), "both shared mobility ranges must resolve");
		nextFlight.Add({}, {}, { 31u }, 31ull, EMobilityType::Dynamic);
		nextFlight.Finalize(false);
		Require(nextFlight.GetSharedPayload(EMobilityType::Static) == staticPayload &&
			nextFlight.GetSharedPayload(EMobilityType::Stationary) == stationaryPayload,
			"independent flight packets must retain the same immutable payload identities");
		Require(nextFlight.GetPayload(EMobilityType::Dynamic).m_instances[0].m_value == 31u &&
			!nextFlight.GetSharedPayload(EMobilityType::Dynamic),
			"dynamic records must remain flight-local and be rebuilt for the new submission");

		RHI::TPackedDrawPagedArenaCache<TestInstance> arenaSource;
		arenaSource.BeginUpdate(1u, 1u, 1u);
		Require(arenaSource.ReplaceRange(1u, 1u, { { 10u }, { 20u }, { 30u } }, { 101ull, 102ull, 103ull }),
			"a static arena must register immutable records independently of a view");
		auto arenaPayload = arenaSource.EndUpdate();
		RHI::TPackedDrawPacket<TestInstance> arenaView;
		arenaView.UseSharedArenaPayload(EMobilityType::Static, arenaPayload);
		Require(arenaView.AddArenaView({}, {}, 1u, 103ull, EMobilityType::Static) &&
			arenaView.AddArenaView({}, {}, 1u, 101ull, EMobilityType::Static),
			"a view packet must resolve visible items through stable arena keys");
		arenaView.Finalize(false);
		Require(arenaView.GetNumStorageInstances() == 4u &&
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
			{}, baseLodMesh, 1u, 101ull, EMobilityType::Static),
			"the near view must resolve the shared static record");
		nearView.Finalize(false);
		RHI::TPackedDrawPacket<TestInstance> farView;
		farView.UseSharedArenaPayload(EMobilityType::Static, arenaPayload);
		Require(farView.AddArenaView(
			{}, selectedLodMesh, 1u, 101ull, EMobilityType::Static),
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
			{}, baseLodMesh, 1u, 102ull, EMobilityType::Static),
			"a disjoint camera must resolve records absent from another camera's visible set");
		disjointCameraView.Finalize(false);
		Require(disjointCameraView.GetSharedPayload(EMobilityType::Static) ==
				arenaView.GetSharedPayload(EMobilityType::Static) &&
			disjointCameraView.GetNumStorageInstances() == 4u &&
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

		Require(sharedCache.Find(7u, 101u, 2ull) == staticPayload,
			"payload cache must preserve immutable identity across flight slots");
		sharedCache.BeginUpdate(7u, 102u, 3u);
		Require(sharedCache.ReplaceRange(1u, 2u, { { 91u } }, { 10ull }), "replacement range must publish");
		auto replacementPayload = sharedCache.EndUpdate();
		Require(!sharedCache.Find(7u, 101u, 3ull) &&
			sharedCache.Find(7u, 102u, 3ull) == replacementPayload &&
			staticPayload->m_arenaPages[0]->m_instances[0].m_value == 10u &&
			replacementPayload->m_arenaPages[0]->m_instances[0].m_value == 91u,
			"a logical cache slot must retain only its current immutable revision");
		sharedCache.Evict(12ull, 8ull);
		Require(!sharedCache.Find(7u, 102u, 12ull) &&
			nextFlight.GetSharedPayload(EMobilityType::Static)->m_arenaPages[0]->m_instances[0].m_value == 10u,
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
		const auto* materialRange = materialPayload->FindRange(rangeKeyA);
		Require(materialRange && materialRange->m_itemOffsets &&
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

	void TestPagedArenaMetadataPublication()
	{
		RHI::TPackedDrawPagedArenaCache<uint32_t> arena;
		const TVector<uint64_t> keys{ 0u, 1u, 2u, 3u };
		arena.BeginUpdate(1u, 1u, 1u);
		for (uint32_t range = 0; range < 130u; ++range)
			Require(arena.ReplaceRange(range, 1u, { range, range, range, range }, keys), "initial range must publish");
		const auto first = arena.EndUpdate();
		arena.BeginUpdate(1u, 2u, 2u);
		Require(arena.ReplaceRange(64u, 2u, { 999u, 999u, 999u, 999u }, keys),
			"one changed range must preserve unvisited producers");
		const auto second = arena.EndUpdate();
		Require(first->m_arenaRangeIndices == second->m_arenaRangeIndices &&
			first->m_arenaRangePages[0] == second->m_arenaRangePages[0] &&
			first->m_arenaRangePages[1] != second->m_arenaRangePages[1] &&
			first->m_arenaRangePages[2] == second->m_arenaRangePages[2],
			"content changes must share the key index and unchanged metadata pages");
		Require(first->FindRange(64u)->m_contentRevision == 1u && second->FindRange(64u)->m_contentRevision == 2u &&
			first->m_arenaPages[4]->m_instances[0] == 64u && second->m_arenaPages[4]->m_instances[0] == 999u,
			"retained publications must keep both their metadata and instance values");
		arena.BeginUpdate(1u, 3u, 3u);
		for (uint64_t range : { 1u, 2u, 3u, 1u }) arena.RemoveRange(range);
		const auto removed = arena.EndUpdate();
		Require(!removed->FindRange(1u) && !removed->FindRange(2u) && !removed->FindRange(3u) &&
			second->FindRange(1u) && removed->m_arenaRangeIndices != second->m_arenaRangeIndices &&
			removed->m_arenaRangePages[1] == second->m_arenaRangePages[1],
			"removal must detach only the index and affected metadata page");
		arena.BeginUpdate(1u, 4u, 4u);
		Require(arena.ReplaceRange(1000u, 4u, { 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u },
			{ 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u }) && arena.ReplaceRange(2u, 4u, { 8u, 8u, 8u, 8u }, keys),
			"adjacent retired ranges must be reusable by differently sized producers");
		const auto reused = arena.EndUpdate();
		Require(reused->m_arenaCapacity == first->m_arenaCapacity &&
			reused->m_numArenaRangeSlots == first->m_numArenaRangeSlots &&
			reused->FindRange(1000u)->m_offset == 4u && reused->FindRange(2u)->m_offset == 12u &&
			second->m_arenaPages[0]->m_instances[4] == 1u,
			"coalesced holes and metadata slots must be reused while older frames retain their data");

		arena.BeginUpdate(1u, 5u, 5u);
		arena.RemoveRange(0u);
		Require(arena.ReplaceRange(2000u, 5u, TVector<uint32_t>(129u, 42u), [&]()
		{
			TVector<uint64_t> result;
			for (uint32_t i = 0; i < 129u; ++i) result.Add(i);
			return result;
		}()), "a speculative update must allocate its own pages");
		const auto unpublished = arena.EndUpdate(false);
		Require(arena.Find(1u, 4u, 5u) == reused && !arena.Find(1u, 5u, 5u) && unpublished->FindRange(2000u),
			"a failed producer pass must not commit index or allocator state");
		arena.BeginUpdate(2u, 1u, 6u);
		Require(arena.ReplaceRange(50u, 1u, { 50u }, { 0u }), "another slot must build independently");
		const auto otherSlot = arena.EndUpdate();
		arena.BeginUpdate(1u, 6u, 6u);
		for (const auto& entry : *reused->m_arenaRangeIndices)
		{
			const auto* range = reused->FindRange(entry.First());
			Require(arena.TryReuseRange(entry.First(), range->m_contentRevision), "published ranges must survive an abandoned pass");
		}
		const auto restored = arena.EndUpdate();
		Require(restored->m_arenaRangeIndices == reused->m_arenaRangeIndices &&
			restored->m_arenaRangePages == reused->m_arenaRangePages &&
			restored->m_arenaPages == reused->m_arenaPages &&
			restored->m_arenaCapacity == reused->m_arenaCapacity && otherSlot->m_arenaCapacity == 1u,
			"an unchanged publication must share all storage independently of other cache slots");

		arena.BeginUpdate(1u, 7u, 7u);
		for (const auto& entry : *restored->m_arenaRangeIndices) arena.RemoveRange(entry.First());
		const auto empty = arena.EndUpdate();
		Require(empty->m_arenaRangeIndices->IsEmpty() && empty->m_arenaCapacity == first->m_arenaCapacity,
			"an empty scene may retain its reusable high-water capacity, but no live ranges");
		arena.BeginUpdate(1u, 8u, 8u);
		TVector<uint32_t> values(65u, 123u);
		TVector<uint64_t> crossingKeys;
		for (uint32_t i = 0; i < 65u; ++i) crossingKeys.Add(i);
		Require(arena.ReplaceRange(3000u, 8u, values, crossingKeys), "a range crossing page boundaries must reuse the cleared arena");
		const auto afterSpike = arena.EndUpdate();
		uint32_t offset = 0u;
		Require(afterSpike->m_arenaCapacity == first->m_arenaCapacity &&
			afterSpike->m_numArenaRangeSlots == first->m_numArenaRangeSlots &&
			afterSpike->FindInstance(3000u, 64u, offset) && offset == 64u &&
			afterSpike->m_arenaPages[1]->m_instances[0] == 123u && first->m_arenaPages[1]->m_instances[0] == 16u,
			"spike capacity must remain reachable and reusable without changing retained generations");
		arena.Clear();
		Require(first->FindRange(0u) && afterSpike->FindRange(3000u), "cache release must not invalidate retained range tables");
	}

	void TestPackedDrawSceneChanges()
	{
		auto scene = RHI::RHIScenePtr::Make();
		RHI::RHISceneViewProxy topology;
		topology.m_shadowCaster = TSharedPtr<RHI::RHIShadowCasterProxy>::Make();
		const auto resource = RHI::RHISceneProxyResourcePtr::Make(std::move(topology));
		RHI::RHISceneInstanceRecord record;
		record.m_topology = resource;
		record.m_mobility = EMobilityType::Stationary;
		record.m_renderFlags = 1u;
		TVector<RHI::RenderInstanceHandle> handles;
		for (uint32_t i = 0; i < 130u; ++i)
		{
			record.m_producerKey = i;
			handles.Add(scene->AddInstance(record));
		}
		auto capture = [](RHI::RHIScenePtr& owner)
		{
			auto versions = TSharedPtr<TVector<RHI::RHISceneVersionPtr>>::Make();
			versions->Add(owner->PublishVersion());
			return RHI::RHIPackedDrawSceneState{ std::move(versions), {}, 10u };
		};
		const auto first = capture(scene);
		RHI::RHIPackedDrawSceneChanges changes;
		changes.Gather(first, nullptr, EMobilityType::Stationary);
		Require(changes.m_updated.Num() == 130u && changes.m_removed.IsEmpty(), "the initial draw publication must include all producers");
		changes.Gather(first, &first, EMobilityType::Stationary);
		Require(changes.m_updated.IsEmpty() && changes.m_removed.IsEmpty() && changes.m_numComparedRecords == 0u,
			"unchanged scene pages must not visit producers");
		Require(scene->ResolveCurrent(handles[65], record), "the moved producer must exist");
		record.m_worldMatrix[3].x = 12.0f;
		record.m_skeletonOffset = 42u;
		Require(scene->UpdateInstance(handles[65], record, RHI::ESceneChangeBit::Transform | RHI::ESceneChangeBit::SkeletonOffset),
			"transform and skeleton changes must publish");
		const auto moved = capture(scene);
		changes.Gather(moved, &first, EMobilityType::Stationary);
		Require(changes.m_updated.Num() == 1u && changes.m_updated[0].m_handle == handles[65] &&
			changes.m_removed.IsEmpty() && changes.m_numComparedRecords == RHI::RHISceneRecordPage::NumRecords,
			"a single changed producer must not rebuild its unchanged neighbours");
		auto movingMotion = moved;
		movingMotion.m_motionSceneVersions = first.m_sceneVersions;
		auto stoppedMotion = moved;
		stoppedMotion.m_motionSceneVersions = moved.m_sceneVersions;
		changes.Gather(stoppedMotion, &movingMotion, EMobilityType::Stationary);
		Require(changes.m_updated.Num() == 1u && changes.m_updated[0].m_handle == handles[65],
			"an object stopping must clear its previous motion even if its current record is unchanged");
		changes.Gather(moved, &stoppedMotion, EMobilityType::Stationary);
		Require(changes.m_updated.Num() == 130u, "a camera cut must invalidate per-object motion history");
		auto materialChange = moved;
		++materialChange.m_configurationRevision;
		changes.Gather(materialChange, &moved, EMobilityType::Stationary);
		Require(changes.m_updated.Num() == 130u, "material or queue configuration changes must revisit unchanged records");

		RHI::TPackedDrawPagedArenaCache<uint32_t> arena;
		auto updateArena = [&](const RHI::RHIPackedDrawSceneState& state, size_t revision, bool bPublish)
		{
			changes.Gather(state, arena.GetSceneState(1u), EMobilityType::Stationary);
			arena.BeginUpdate(1u, revision, revision, state);
			for (const auto& proxy : changes.m_removed)
				arena.RemoveRange(RHI::BuildPackedDrawRangeKey(proxy.m_handle, proxy.m_record->m_producerKey, proxy.m_resource));
			for (const auto& proxy : changes.m_updated)
				Require(arena.ReplaceRange(RHI::BuildPackedDrawRangeKey(proxy.m_handle, proxy.m_record->m_producerKey, proxy.m_resource),
					revision, { static_cast<uint32_t>(proxy.GetWorldMatrix()[3].x) }, { 0u }), "changed producer must update its range");
			changes.Clear();
			return arena.EndUpdate(bPublish);
		};
		const auto firstPayload = updateArena(first, 1u, true);
		const auto unpublished = updateArena(moved, 2u, false);
		const uint64_t movedKey = RHI::BuildPackedDrawRangeKey(handles[65], 65u, resource.GetRawPtr());
		Require(arena.GetSceneState(1u)->m_sceneVersions == first.m_sceneVersions &&
			arena.Find(1u, 1u, 2u) == firstPayload && unpublished->FindRange(movedKey)->m_contentRevision == 2u,
			"failed preparation must not advance the cache's consumed scene publication");
		const auto movedPayload = updateArena(moved, 2u, true);
		Require(firstPayload->m_arenaRangeIndices == movedPayload->m_arenaRangeIndices &&
			firstPayload->m_arenaPages[0] == movedPayload->m_arenaPages[0] &&
			firstPayload->m_arenaPages[1] != movedPayload->m_arenaPages[1] &&
			firstPayload->m_arenaPages[1]->m_instances[1] == 0u && movedPayload->m_arenaPages[1]->m_instances[1] == 12u,
			"the scene delta must update one data page and retain the older frame");
		const auto unchangedPayload = updateArena(moved, 3u, true);
		Require(unchangedPayload->m_arenaPages == movedPayload->m_arenaPages &&
			unchangedPayload->m_arenaRangePages == movedPayload->m_arenaRangePages,
			"an empty producer delta must preserve all ranges without visit marking");

		Require(scene->ResolveCurrent(handles[0], record), "mobility fixture must resolve");
		record.m_mobility = EMobilityType::Static;
		Require(scene->UpdateInstance(handles[0], record, RHI::ToMask(RHI::ESceneChangeBit::Mobility)), "mobility change must succeed");
		Require(scene->RemoveInstance(handles[1]), "producer removal must succeed");
		Require(scene->ResolveCurrent(handles[2], record), "topology fixture must resolve");
		record.m_topology = RHI::RHISceneProxyResourcePtr::Make(resource->m_proxy);
		Require(scene->UpdateInstance(handles[2], record, RHI::ToMask(RHI::ESceneChangeBit::MeshOrLodTopology)), "topology change must succeed");
		Require(scene->ResolveCurrent(handles[3], record), "shadow fixture must resolve");
		record.m_renderFlags = 0u;
		Require(scene->UpdateInstance(handles[3], record, RHI::ToMask(RHI::ESceneChangeBit::ShadowState)), "shadow change must succeed");
		Require(scene->ResolveCurrent(handles[4], record), "producer-key fixture must resolve");
		record.m_producerKey = 400u;
		Require(scene->UpdateInstance(handles[4], record, RHI::ToMask(RHI::ESceneChangeBit::MeshOrLodTopology)), "producer key change must succeed");
		const auto replaced = capture(scene);
		changes.Gather(replaced, &moved, EMobilityType::Stationary);
		Require(changes.m_updated.Num() == 3u && changes.m_removed.Num() == 4u,
			"mobility, topology and producer identity changes must retire the old arena keys");
		changes.Gather(replaced, &moved, EMobilityType::Stationary, true);
		Require(changes.m_updated.Num() == 2u && changes.m_removed.Num() == 5u,
			"a disabled shadow caster must leave the shadow arena without removing its main-pass producer");
		changes.Gather(replaced, &moved, EMobilityType::Static);
		Require(changes.m_updated.Num() == 1u && changes.m_updated[0].m_handle == handles[0] && changes.m_removed.IsEmpty(),
			"the receiving mobility segment must add the migrated producer");
		const auto replacedPayload = updateArena(replaced, 4u, true);
		Require(!replacedPayload->FindRange(RHI::BuildPackedDrawRangeKey(handles[0], 0u, resource.GetRawPtr())) &&
			!replacedPayload->FindRange(RHI::BuildPackedDrawRangeKey(handles[1], 1u, resource.GetRawPtr())) &&
			replacedPayload->FindRange(movedKey) && movedPayload->FindRange(RHI::BuildPackedDrawRangeKey(handles[1], 1u, resource.GetRawPtr())),
			"explicit removals must affect only the new arena publication");

		auto other = RHI::RHIScenePtr::Make();
		record.m_topology = resource;
		record.m_producerKey = 1000u;
		other->AddInstance(record);
		const auto otherState = capture(other);
		auto versions = TSharedPtr<TVector<RHI::RHISceneVersionPtr>>::Make();
		versions->Add((*replaced.m_sceneVersions)[0]);
		versions->Add((*otherState.m_sceneVersions)[0]);
		RHI::RHIPackedDrawSceneState pair{ versions, {}, replaced.m_configurationRevision };
		auto reorderedVersions = TSharedPtr<TVector<RHI::RHISceneVersionPtr>>::Make();
		reorderedVersions->Add((*versions)[1]);
		reorderedVersions->Add((*versions)[0]);
		RHI::RHIPackedDrawSceneState reordered{ reorderedVersions, {}, pair.m_configurationRevision };
		changes.Gather(reordered, &pair, EMobilityType::Stationary);
		Require(changes.m_updated.IsEmpty() && changes.m_removed.IsEmpty() && changes.m_numComparedRecords == 0u,
			"scene identity, not vector position, must match independently retained producers");
		changes.Gather(replaced, &pair, EMobilityType::Stationary);
		Require(changes.m_updated.IsEmpty() && changes.m_removed.Num() == 1u && changes.m_removed[0].m_record->m_producerKey == 1000u,
			"removing an entire producer scene must not erase another scene with the same handle slot");

		auto recycledScene = RHI::RHIScenePtr::Make();
		const auto retiredHandle = recycledScene->AddInstance(record);
		Require(recycledScene->RemoveInstance(retiredHandle), "the generation fixture must retire its producer");
		const auto retiredState = capture(recycledScene);
		recycledScene->CollectGarbage();
		const auto recycledHandle = recycledScene->AddInstance(record);
		Require(recycledHandle.m_slot == retiredHandle.m_slot && recycledHandle.m_generation != retiredHandle.m_generation,
			"the fixture must reuse a retired slot with a new generation");
		const auto recycledState = capture(recycledScene);
		changes.Gather(recycledState, &retiredState, EMobilityType::Stationary);
		Require(changes.m_updated.Num() == 1u && changes.m_updated[0].m_handle == recycledHandle && changes.m_removed.IsEmpty(),
			"an already removed range must be reintroduced with its new generation, not the stale key");
	}

	void TestConcurrentArenaPublications()
	{
		using Payload = RHI::TPackedDrawPacketPayload<uint32_t>;
		using PayloadPtr = RHI::TPackedDrawPacketPayloadPtr<uint32_t>;
		using Page = RHI::TPackedDrawArenaPage<uint32_t>;
		static_assert(std::is_same_v<decltype(std::declval<PayloadPtr>().GetRawPtr()), const Payload*>);
		static_assert(std::is_same_v<decltype(std::declval<PayloadPtr>()->m_arenaPages[0].GetRawPtr()), const Page*>);
		static_assert(std::is_same_v<decltype(std::declval<PayloadPtr>()->m_arenaRangePages[0].GetRawPtr()),
			const RHI::PackedDrawArenaRangePage*>);
		static_assert(std::is_same_v<decltype(std::declval<PayloadPtr>()->m_arenaRangeIndices.GetRawPtr()),
			const RHI::PackedDrawArenaRangeIndices*>);
		static_assert(std::is_same_v<decltype(std::declval<RHI::PackedDrawArenaRange>().m_itemOffsets.GetRawPtr()),
			const TVector<RHI::PackedDrawArenaItemOffset>*>);
		static_assert(std::is_same_v<decltype(std::declval<RHI::PackedDrawArenaRange>().m_materialVersionRuns.GetRawPtr()),
			const TVector<RHI::PackedDrawArenaMaterialRun>*>);

		RHI::TPackedDrawPagedArenaCache<uint32_t> arena;
		TVector<uint32_t> values;
		TVector<uint64_t> keys;
		for (uint32_t i = 0; i < Page::NumInstances; ++i)
		{
			values.Add(i);
			keys.Add(i + 100u);
		}
		arena.BeginUpdate(1u, 0u, 0u);
		Require(arena.ReplaceRange(1u, 0u, values, keys) && arena.ReplaceRange(2u, 0u, values, keys),
			"the fixture must publish two independent arena pages");
		const auto first = arena.EndUpdate();
		std::barrier step(2);
		std::atomic<bool> bValid = true;
		constexpr uint32_t publications = 128u;
		std::thread reader([&]()
		{
			for (uint32_t iteration = 0; iteration < publications; ++iteration)
			{
				step.arrive_and_wait();
				for (uint32_t i = 0; i < Page::NumInstances; ++i)
				{
					uint32_t offset = 0;
					if (!first->FindInstance(1u, keys[i], offset) || offset != i ||
						first->m_arenaPages[0]->m_instances[i] != i ||
						first->m_arenaPages[1]->m_instances[i] != i) bValid = false;
				}
				step.arrive_and_wait();
			}
		});
		for (uint32_t revision = 1; revision <= publications; ++revision)
		{
			step.arrive_and_wait();
			arena.BeginUpdate(1u, revision, revision);
			values[0] = revision;
			if (!arena.ReplaceRange(1u, revision, values, keys) || !arena.TryReuseRange(2u, 0u)) bValid = false;
			const auto current = arena.EndUpdate();
			if (current->m_arenaPages[0] == first->m_arenaPages[0] ||
				current->m_arenaPages[1] != first->m_arenaPages[1] ||
				current->m_arenaPages[0]->m_instances[0] != revision) bValid = false;
			step.arrive_and_wait();
		}
		reader.join();
		arena.Clear();
		Require(bValid && first->m_arenaPages[0]->m_instances[0] == 0u,
			"concurrent arena updates and cache release must preserve a retained payload's pages and lookup");
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

	void TestMaterialVersionsSurviveConcurrentWorldUpdates()
	{
		constexpr uint32_t NumInstances = 64u;
		auto material = RHI::RHIMaterialPtr::Make(RHI::RenderState{}, RHI::RHIShaderPtr{}, RHI::RHIShaderPtr{});
		std::array<RHI::RHIMaterialVersionPtr, 3u> versions;
		std::array<RHI::RHIShaderBindingSetPtr, 3u> bindings;
		std::array<std::array<RHI::TPackedDrawPacket<uint32_t>, 3u>, 3u> packets;
		for (size_t frame = 0u; frame < versions.size(); ++frame)
		{
			bindings[frame] = RHI::RHIShaderBindingSetPtr::Make();
			bindings[frame]->GetOrAddShaderBinding("material"_h)->m_vulkan.m_storageInstanceIndex =
				static_cast<uint32_t>(100u + frame);
			material->SetBindings(bindings[frame]);
			versions[frame] = material->GetVersion();
			RHI::RHIMaterial::BeginSubmissionVersionCapture(400ull + frame);
		}
		std::barrier sync(4);
		std::atomic<bool> valid{ true };
		std::vector<std::thread> renderWorkers;
		for (size_t frame = 0u; frame < versions.size(); ++frame)
		{
			renderWorkers.emplace_back([&, frame]()
				{
					RHI::RHIMaterialPreparationCache preparedMaterials(400ull + frame);
					for (size_t nextFrame = 0u; nextFrame < 32u; ++nextFrame)
					{
						sync.arrive_and_wait();
						// Simulate main, depth, and shadow packets prepared after
						// the game thread has started publishing later materials.
						for (size_t pass = 0u; pass < 3u; ++pass)
						{
							auto& packet = packets[frame][pass];
							packet.Reset();
							for (uint32_t index = 0u; index < NumInstances; ++index)
							{
								packet.Add(preparedMaterials.MakeBatch(material, {}), {},
									NumInstances - index, NumInstances - index);
							}
							packet.Finalize(false);
							if (packet.GetGroups().Num() != 1u || packet.GetNumDrawInstances() != NumInstances)
							{
								valid.store(false);
								continue;
							}
							const auto& batch = packet.GetGroups()[0].m_batch;
							if (batch.m_materialVersion != versions[frame] || batch.GetMaterialBindings() != bindings[frame] ||
								preparedMaterials.Get(material).m_materialInstance != 100u + frame ||
								batch.GetMaterialBindingsRaw() != bindings[frame].GetRawPtr())
							{
								valid.store(false);
							}
							for (uint32_t index = 0u; index < NumInstances; ++index)
							{
								if (packet.GetPayload(EMobilityType::Dynamic).m_instances[index] != index + 1u)
								{
									valid.store(false);
								}
							}
						}
						sync.arrive_and_wait();
					}
				});
		}
		for (size_t nextFrame = 0u; nextFrame < 32u; ++nextFrame)
		{
			sync.arrive_and_wait();
			material->SetBindings(RHI::RHIShaderBindingSetPtr::Make());
			sync.arrive_and_wait();
		}
		for (auto& worker : renderWorkers)
		{
			worker.join();
		}
		for (size_t frame = 0u; frame < versions.size(); ++frame)
		{
			RHI::RHIMaterial::EndSubmissionVersionCapture(400ull + frame);
		}
		Require(valid.load(), "three in-flight frames must bind their saved material generations during concurrent publication");
		for (size_t frame = 0u; frame < versions.size(); ++frame)
		{
			for (const auto& packet : packets[frame])
			{
				Require(packet.GetGroups()[0].m_batch.GetMaterialBindings() == bindings[frame],
					"finalized draw packets must own their descriptors after the submission cutoff is released");
			}
		}
	}

	void TestParallelSpatialIndexVisibility()
	{
		constexpr size_t NumPartitions = 8u;
		constexpr size_t NumElements = 768u;
		const auto centerFor = [](size_t i)
			{
				return glm::ivec3(int(i % 32u) * 6 - 96, int(i % 5u) - 2, -int(i / 32u) * 8 - 4);
			};
		const auto extentsFor = [](size_t i) { return glm::ivec3(i % 11u == 0u ? 9 : 1); };
		auto spatial = TSharedPtr<RHI::RHISceneSpatialIndex>::Make(glm::ivec3(0), 4096u, 4u, NumPartitions);
		std::vector<std::thread> workers;
		for (size_t partition = 0u; partition < NumPartitions; ++partition)
		{
			workers.emplace_back([&, partition]()
				{
					for (size_t i = partition; i < NumElements; i += NumPartitions)
					{
						spatial->Update(centerFor(i), extentsFor(i), { uint32_t(i), 1u }, partition);
					}
				});
		}
		for (auto& worker : workers)
		{
			worker.join();
		}
		Require(spatial->Num() == NumElements, "independent writers must preserve every spatial handle");
		for (float cameraX : { -80.0f, 0.0f, 80.0f })
		{
			Math::Frustum frustum;
			frustum.ExtractFrustumPlanes(glm::translate(glm::mat4(1.0f), glm::vec3(cameraX, 0.0f, 0.0f)),
				1.0f, 60.0f, 0.1f, 150.0f);
			std::vector<uint32_t> expected, visible;
			for (size_t i = 0u; i < NumElements; ++i)
			{
				if (frustum.OverlapsAABB(Math::AABB(centerFor(i), extentsFor(i))))
				{
					expected.push_back(uint32_t(i));
				}
			}
			spatial->Trace(frustum, [&](const RHI::RenderInstanceHandle& handle) { visible.push_back(handle.m_slot); });
			std::sort(visible.begin(), visible.end());
			Require(!expected.empty() && expected.size() < NumElements && visible == expected,
				"partitioned culling must match brute-force bounds with no missing or duplicated handles");
		}
	}

	void TestDynamicSpatialRootIsolation()
	{
		RHI::RHISceneViewProxy proxy;
		auto resource = RHI::RHISceneProxyResourcePtr::Make(std::move(proxy));
		auto scene = RHI::RHIScenePtr::Make(3u);
		RHI::RHISceneInstanceRecord record;
		record.m_producerKey = 41u;
		record.m_mobility = EMobilityType::Dynamic;
		record.m_worldMatrix = glm::mat4(1.0f);
		record.m_worldBounds = Math::AABB(glm::vec3(0.0f, 0.0f, -5.0f), glm::vec3(1.0f));
		record.m_topology = resource;
		const auto handle = scene->AddInstance(record);

		auto spatial = TSharedPtr<RHI::RHISpatialSceneVersion>::Make();
		spatial->m_scene = scene;
		spatial->m_sceneVersion = scene->PublishVersion();
		auto index = TSharedPtr<RHI::RHISceneSpatialIndex>::Make(glm::ivec3(0), 128, 4);
		Require(index->Update(
			glm::ivec3(0, 0, -5),
			glm::ivec3(1),
			handle),
			"the dynamic spatial fixture must publish its handle");
		spatial->m_dynamicOctree = std::move(index);

		RHI::RHISceneView view;
		view.AddSceneVersion(spatial);
		Math::Frustum frustum;
		frustum.ExtractFrustumPlanes(
			glm::mat4(1.0f),
			1.0f,
			60.0f,
			0.1f,
			10.0f);
		const auto visible = view.TraceScene(frustum);
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
		proxy.m_lodPolicy.m_bEnabled = true;
		proxy.m_lodPolicy.m_minLod = 0u;
		proxy.m_lodPolicy.m_maxLod = 1u;
		proxy.m_lodPolicy.m_screenCoverageThresholds = {
			(nearCoverage + farCoverage) * 0.5f };
		proxy.m_instancedGroups.Add(group);
		auto resource = RHI::RHISceneProxyResourcePtr::Make(std::move(proxy));

		RHI::RHISceneInstanceRecord record;
		record.m_topology = resource;
		record.m_worldBounds = Math::AABB(glm::vec3(0.0f, 0.0f, -11.0f), glm::vec3(1.0f, 1.0f, 10.0f));
		RHI::RHIVisibleSceneProxy cameraView({}, record, *resource);
		RHI::RHISceneViewSnapshot snapshot;
		snapshot.m_proxies.Add(cameraView);
		snapshot.m_shadowMapsToUpdate.Resize(2u);
		RHI::RHIVisibleShadowCaster shadowView({}, record, *resource);
		for (auto& pass : snapshot.m_shadowMapsToUpdate)
		{
			pass.m_meshList.Add(shadowView);
		}
		snapshot.m_shadowMapsToUpdate[0].m_lightMatrix = glm::mat4(1.0f);
		snapshot.m_shadowMapsToUpdate[1].m_lightMatrix = projection * glm::scale(glm::mat4(1.0f), glm::vec3(10.0f));
		snapshot.PrepareLods(view, projection);
		cameraView = snapshot.m_proxies[0];
		Require(snapshot.ResolveInstancedMesh(cameraView, 0u, 0u, 0u) == baseMesh &&
			snapshot.ResolveInstancedMesh(cameraView, 0u, 1u, 0u) == lodMesh,
			"main and depth view classification must select LOD per vegetation instance instead of per chunk");
		Require(cameraView.IsInstancedMeshWithinDistance(
				group, 0u, 0u, glm::vec3(0.0f), 10.0f) &&
			!cameraView.IsInstancedMeshWithinDistance(
				group, 1u, 0u, glm::vec3(0.0f), 10.0f),
			"vegetation distance culling must use per-instance bounds");

		for (const auto& pass : snapshot.m_shadowMapsToUpdate)
		{
			Require(&snapshot.ResolveInstancedMesh(pass.m_meshList[0], 0u, 1u, 0u) ==
				&snapshot.ResolveInstancedMesh(cameraView, 0u, 1u, 0u),
				"every shadow projection must read the camera snapshot's shared per-instance mesh selection");
		}

		auto biasedProxy = resource->m_proxy;
		biasedProxy.m_instancedGroups[0].m_instanceLodBiases = { 1, -1 };
		auto biasedResource = RHI::RHISceneProxyResourcePtr::Make(std::move(biasedProxy));
		RHI::RHISceneViewSnapshot biasedSnapshot;
		auto biasedRecord = record;
		biasedRecord.m_topology = biasedResource;
		cameraView = RHI::RHIVisibleSceneProxy({}, biasedRecord, *biasedResource);
		biasedSnapshot.m_proxies.Add(cameraView);
		biasedSnapshot.PrepareLods(view, projection);
		Require(biasedSnapshot.ResolveInstancedMesh(biasedSnapshot.m_proxies[0], 0u, 0u, 0u) == lodMesh &&
			biasedSnapshot.ResolveInstancedMesh(biasedSnapshot.m_proxies[0], 0u, 1u, 0u) == baseMesh,
			"snapshot preparation must retain positive and negative vegetation instance LOD biases");
		auto distanceProxy = resource->m_proxy;
		distanceProxy.m_lodPolicy.m_cameraDistanceThresholds = { 10.0f };
		distanceProxy.m_lodPolicy.m_screenCoverageThresholds = { 0.0f };
		auto distanceResource = RHI::RHISceneProxyResourcePtr::Make(std::move(distanceProxy));
		auto distanceRecord = record;
		distanceRecord.m_topology = distanceResource;
		biasedSnapshot.m_proxies[0] = RHI::RHIVisibleSceneProxy({}, distanceRecord, *distanceResource);
		biasedSnapshot.PrepareLods(view, projection);
		Require(biasedSnapshot.ResolveInstancedMesh(biasedSnapshot.m_proxies[0], 0u, 0u, 0u) == baseMesh &&
			biasedSnapshot.ResolveInstancedMesh(biasedSnapshot.m_proxies[0], 0u, 1u, 0u) == lodMesh,
			"camera distance policies must classify instances by their own world bounds");
	}

	void TestSnapshotCameraLodContract()
	{
		auto baseMesh = RHI::RHIMeshPtr::Make();
		auto lod1 = RHI::RHIMeshPtr::Make();
		auto lod2 = RHI::RHIMeshPtr::Make();
		baseMesh->m_lods = { lod1, lod2 };
		auto shorterMesh = RHI::RHIMeshPtr::Make();
		shorterMesh->m_lods = { lod1 };
		auto missingLodMesh = RHI::RHIMeshPtr::Make();
		missingLodMesh->m_lods = { lod1, {} };
		RHI::RHISceneViewProxy source;
		source.m_lodPolicy.m_bEnabled = true;
		source.m_lodPolicy.m_cameraDistanceThresholds = { 5.0f, 15.0f };
		source.m_meshes = { baseMesh, shorterMesh, missingLodMesh, {} };
		auto shadowCaster = TSharedPtr<RHI::RHIShadowCasterProxy>::Make();
		for (const auto& mesh : { shorterMesh, baseMesh, missingLodMesh })
		{
			RHI::RHIShadowMeshProxy shadowMesh;
			shadowMesh.m_mesh = mesh;
			shadowCaster->m_meshes.Add(shadowMesh);
		}
		source.m_shadowCaster = std::move(shadowCaster);
		auto resource = RHI::RHISceneProxyResourcePtr::Make(std::move(source));
		RHI::RHISceneInstanceRecord farRecord;
		farRecord.m_topology = resource;
		farRecord.m_worldBounds = Math::AABB(glm::vec3(0.0f, 0.0f, -20.0f), glm::vec3(0.5f));
		RHI::RHISceneInstanceRecord nearRecord;
		nearRecord.m_topology = resource;
		nearRecord.m_worldBounds = Math::AABB(glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.5f));
		RHI::RHIVisibleSceneProxy visible({}, farRecord, *resource);
		RHI::RHISceneViewSnapshot snapshot;
		snapshot.m_proxies.Add(visible);
		RHI::RHIVisibleShadowCaster caster({}, farRecord, *resource);
		snapshot.m_shadowMapsToUpdate.Resize(2u);
		for (auto& pass : snapshot.m_shadowMapsToUpdate)
		{
			pass.m_meshList.Add(caster);
			caster.m_record = &nearRecord;
			pass.m_meshList.Add(caster);
			caster.m_record = &farRecord;
		}
		const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
		snapshot.PrepareLods(glm::mat4(1.0f), projection);
		Require(snapshot.ResolveMesh(snapshot.m_proxies[0], 0u) == lod2 &&
			snapshot.ResolveMesh(snapshot.m_proxies[0], 1u) == lod1 &&
			snapshot.ResolveMesh(snapshot.m_proxies[0], 2u) == missingLodMesh &&
			!snapshot.ResolveMesh(snapshot.m_proxies[0], 3u),
			"camera LOD selection must respect each mesh's available chain and preserve missing meshes");
		for (const auto& pass : snapshot.m_shadowMapsToUpdate)
		{
			Require(snapshot.ResolveMesh(pass.m_meshList[0], 0u) == lod1 &&
				snapshot.ResolveMesh(pass.m_meshList[0], 1u) == lod2,
				"shadow mesh order must not change the camera's selected LOD");
			Require(snapshot.ResolveMesh(pass.m_meshList[1], 1u) == baseMesh,
				"shadow-only records must select from the camera independently of another record sharing their topology and handle");
		}
		RHI::RHISceneViewSnapshot movedSnapshot;
		movedSnapshot.m_proxies = snapshot.m_proxies;
		movedSnapshot.m_shadowMapsToUpdate = snapshot.m_shadowMapsToUpdate;
		movedSnapshot.m_cameraTransform.m_position.z = -20.0f;
		movedSnapshot.PrepareLods(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 20.0f)), projection);
		Require(movedSnapshot.ResolveMesh(movedSnapshot.m_proxies[0], 0u) == baseMesh &&
			snapshot.ResolveMesh(snapshot.m_proxies[0], 0u) == lod2,
			"a second camera or submission must not overwrite the retained snapshot's LOD selection");
		movedSnapshot.ResetForReuse();
		Require(snapshot.ResolveMesh(snapshot.m_proxies[0], 0u) == lod2,
			"recycling another snapshot must not release this submission's selected meshes");
		auto disabledSource = resource->m_proxy;
		disabledSource.m_lodPolicy.m_bEnabled = false;
		auto disabledResource = RHI::RHISceneProxyResourcePtr::Make(std::move(disabledSource));
		visible.m_resource = disabledResource.GetRawPtr();
		movedSnapshot.m_proxies.Add(visible);
		movedSnapshot.PrepareLods(glm::mat4(1.0f), projection);
		Require(movedSnapshot.ResolveMesh(movedSnapshot.m_proxies[0], 0u) == baseMesh,
			"disabling LOD must preserve base geometry regardless of camera distance");
		RHI::RHISceneViewProxy baseOnlySource;
		auto baseOnlyMesh = RHI::RHIMeshPtr::Make();
		baseOnlySource.m_lodPolicy.m_bEnabled = true;
		baseOnlySource.m_meshes.Add(baseOnlyMesh);
		auto baseOnlyResource = RHI::RHISceneProxyResourcePtr::Make(std::move(baseOnlySource));
		movedSnapshot.m_proxies[0].m_resource = baseOnlyResource.GetRawPtr();
		movedSnapshot.PrepareLods(glm::mat4(1.0f), projection);
		Require(movedSnapshot.ResolveMesh(movedSnapshot.m_proxies[0], 0u) == baseOnlyMesh,
			"meshes without an LOD chain must remain drawable with LOD enabled");
	}
	void TestCustomShadowMaterialKey()
	{
		using Key = ShadowPrepassNodeProbe::MaterialKey;
		auto first = RHI::RHIMaterialPtr::Make(RHI::RenderState{}, RHI::RHIShaderPtr{}, RHI::RHIShaderPtr{});
		auto second = RHI::RHIMaterialPtr::Make(RHI::RenderState{}, RHI::RHIShaderPtr{}, RHI::RHIShaderPtr{});
		const Key keys[] = {
			{ first.GetRawPtr(), 1, RHI::EShadowType::PCF, false },
			{ second.GetRawPtr(), 1, RHI::EShadowType::PCF, false },
			{ first.GetRawPtr(), 2, RHI::EShadowType::PCF, false },
			{ first.GetRawPtr(), 1, RHI::EShadowType::EVSM, false },
			{ first.GetRawPtr(), 1, RHI::EShadowType::PCF, true }
		};
		TMap<Key, uint32_t> entries;
		for (uint32_t i = 0; i < std::size(keys); ++i)
		{
			Require(entries.Insert(keys[i], i), "each source, vertex, shadow-type and masked combination must have its own entry");
			Require(keys[i] == Key(keys[i]) && keys[i].GetHash() == Key(keys[i]).GetHash(),
				"equal typed shadow keys must hash equally");
		}
		Require(entries.Num() == std::size(keys), "shadow cache identity must retain every typed key field");
		for (uint32_t i = 0; i < std::size(keys); ++i)
		{
			uint32_t* value = nullptr;
			Require(entries.Find(keys[i], value) && value && *value == i, "shadow key lookup must return the exact requested variant");
		}
	}

	void TestBatchTextureBindingIdentityContract()
	{
		using TextureBindingCacheKey = RenderSceneNodeProbe::TextureBindingCacheKeyProbe;

		const TVector<uint32_t> firstTextures{ 0u, 4u, 8u };
		const TVector<uint32_t> secondTextures{ 0u, 5u, 8u };
		TextureBindingCacheKey firstTextureSet(firstTextures);
		TextureBindingCacheKey secondTextureSet(secondTextures);
		firstTextureSet.Materialize();
		secondTextureSet.Materialize();

		TMap<TextureBindingCacheKey, uint32_t> textureBindingCache;
		Require(textureBindingCache.Insert(firstTextureSet, 1u),
			"the first texture binding cache key should be inserted");
		Require(textureBindingCache.Insert(secondTextureSet, 2u),
			"a different equal-sized texture binding cache key must not collapse into the first");
		Require(textureBindingCache.Num() == 2,
			"texture sets with the same count and layout capacity must retain distinct cache identities");
		TVector<uint32_t> lookupTextures{ 0u, 4u, 8u };
		TextureBindingCacheKey lookupKey(lookupTextures);
		Require(lookupKey.GetTextures().GetData() == lookupTextures.GetData() &&
			lookupKey == firstTextureSet &&
			lookupKey.GetHash() == firstTextureSet.GetHash(),
			"a cache-hit lookup key must compare and hash the published vector without copying it");
		uint32_t* lookupValue = nullptr;
		Require(textureBindingCache.Find(lookupKey, lookupValue) &&
			lookupValue && *lookupValue == 1u,
			"a non-owning texture key must resolve the canonical cached entry");
		lookupKey.Materialize();
		Require(lookupKey.GetTextures() == firstTextures &&
			lookupKey.GetTextures().GetData() != lookupTextures.GetData(),
			"an inserted key must own the canonical indices independently of the lookup source");
		lookupTextures = { 0u };
		Require(lookupKey == firstTextureSet && lookupKey.GetHash() == firstTextureSet.GetHash() &&
			!(lookupKey == TextureBindingCacheKey(lookupTextures)),
			"changing the borrowed source after materialization must not change a stored key");
		const auto copiedKey = lookupKey;
		const auto movedKey = std::move(lookupKey);
		Require(copiedKey == movedKey && textureBindingCache.Find(movedKey, lookupValue) && *lookupValue == 1u,
			"copied and moved owning keys must retain the original cache identity");

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

	void TestTextureSamplerPublication()
	{
#if defined(__APPLE__)
		constexpr uint32_t Limit = TextureImporter::MaxTexturesInScene;
		const TVector<uint32_t> expected{ 0u, 4u, 8u, Limit - 1u };
		auto makeProxy = [](bool alternate)
		{
			RHI::RHISceneViewProxy proxy;
			proxy.m_materialTextureSamplers = { { 8u, 4u, 8u, 0u, Limit, Limit - 1u }, {} };
			if (alternate)
			{
				proxy.m_materialTextureSamplers = {
					{ Limit - 1u, 8u, 4u, 4u }, { 0u, 0u, (std::numeric_limits<uint32_t>::max)() } };
			}
			RHI::RHIInstancedMeshGroup group;
			group.m_materialTextureSamplers = proxy.m_materialTextureSamplers;
			proxy.m_instancedGroups.Add(std::move(group));
			auto shadowCaster = TSharedPtr<RHI::RHIShadowCasterProxy>::Make();
			for (const auto& textures : proxy.m_materialTextureSamplers)
			{
				RHI::RHIShadowMeshProxy mesh;
				mesh.m_localMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(2, 3, 4));
				mesh.m_materialTextureSamplers = textures;
				shadowCaster->m_meshes.Add(std::move(mesh));
			}
			proxy.m_shadowCaster = std::move(shadowCaster);
			return proxy;
		};
		auto requireIndices = [](const TVector<uint32_t>& textures, const TVector<uint32_t>& indices)
		{
			Require(textures == indices,
				"published material indices must be sorted, unique, bounded and include the default slot");
		};
		for (const auto& world : { glm::mat4(1.0f), glm::mat4(0.0f), glm::scale(glm::mat4(1.0f), glm::vec3(-2, 3, 4)) })
		{
			auto source = makeProxy(false);
			const auto originalTextures = source.m_materialTextureSamplers;
			const auto originalShadow = source.m_shadowCaster;
			const auto originalShadowTextures = originalShadow->m_meshes[0].m_materialTextureSamplers;
			const auto originalShadowMatrix = originalShadow->m_meshes[0].m_localMatrix;
			auto copied = RHI::RHISceneProxyResourcePtr::Make(source);
			Require(source.m_materialTextureSamplers == originalTextures,
				"publication must not rewrite the producer's texture metadata");
			auto moved = RHI::RHISceneProxyResourcePtr::Make(std::move(source));
			for (const auto& resource : { copied, moved })
			{
				const auto& proxy = resource->m_proxy;
				for (size_t i = 0; i < 2u; ++i)
				{
					const TVector<uint32_t> indices = i == 0u ? expected : TVector<uint32_t>{ 0u };
					requireIndices(proxy.m_materialTextureSamplers[i], indices);
					requireIndices(proxy.m_instancedGroups[0].m_materialTextureSamplers[i], indices);
					requireIndices(proxy.m_shadowCaster->m_meshes[i].m_materialTextureSamplers, indices);
				}
				Require(proxy.m_shadowCaster != originalShadow &&
					originalShadow->m_meshes[0].m_materialTextureSamplers == originalShadowTextures &&
					originalShadow->m_meshes[0].m_localMatrix == originalShadowMatrix,
					"copy and move publication must isolate shared shadow metadata");
				RHI::RHISceneInstanceRecord record;
				record.m_worldMatrix = world;
				record.m_topology = resource;
				RHI::RHIVisibleShadowCaster visible({}, record, *resource);
				Require(visible.ResolveMeshWorldMatrix(proxy.m_shadowCaster->m_meshes[0]) == world * originalShadowMatrix,
					"local shadow topology must compose directly with singular and mirrored instance transforms");
			}
			auto alternate = makeProxy(true);
			auto equivalent = RHI::RHISceneProxyResourcePtr::Make(alternate);
			Require(copied->m_mainRevision == equivalent->m_mainRevision &&
				copied->m_shadowRevision == equivalent->m_shadowRevision &&
				copied->m_mainRevision == moved->m_mainRevision &&
				copied->m_shadowRevision == moved->m_shadowRevision,
				"permutations, duplicates, invalid indices and implicit slot zero must share publication revisions");
			alternate.m_materialTextureSamplers[0] = { 0u, 4u, 9u, Limit - 1u };
			auto replacementShadow = TSharedPtr<RHI::RHIShadowCasterProxy>::Make(*alternate.m_shadowCaster);
			replacementShadow->m_meshes[0].m_materialTextureSamplers = alternate.m_materialTextureSamplers[0];
			alternate.m_shadowCaster = std::move(replacementShadow);
			auto changed = RHI::RHISceneProxyResourcePtr::Make(std::move(alternate));
			Require(changed->m_mainRevision != copied->m_mainRevision &&
				changed->m_shadowRevision != copied->m_shadowRevision,
				"an actual sampler replacement must still invalidate main and shadow publications");
		}
#endif
	}

	void TestShaderReadOnlyBarrierSynchronizesShaderSampling()
	{
		constexpr VkQueueFlags queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
		const VkAccessFlags shaderReadAccess =
			VulkanCommandBuffer::GetAccessFlags(
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, queueFlags);
		Require((shaderReadAccess & VK_ACCESS_SHADER_READ_BIT) != 0,
			"shader-read image layouts must wait for prior image writes");

		const VkPipelineStageFlags shaderReadStages =
			VulkanCommandBuffer::GetPipelineStage(
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, queueFlags);
		Require(shaderReadStages == (VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
			"shader-read image layouts must synchronize both graphics and compute sampling on a combined queue");
	}

	void TestDepthSamplingBarrierScopes()
	{
		constexpr VkPipelineStageFlags depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		struct QueueCase
		{
			VkQueueFlags m_flags;
			VkPipelineStageFlags m_shaderStages;
			bool m_graphics;
		};
		const QueueCase queues[]
		{
			{ VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, true },
			{ VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, true },
			{ VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, false },
			{ VK_QUEUE_TRANSFER_BIT, 0u, false }
		};
		for (const auto& queue : queues)
		{
			const VkAccessFlags depthRead = queue.m_graphics ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT : 0u;
			const VkAccessFlags depthWrite = queue.m_graphics ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0u;
			const VkAccessFlags shaderRead = queue.m_shaderStages ? VK_ACCESS_SHADER_READ_BIT : 0u;
			const VkPipelineStageFlags samplingStages = queue.m_shaderStages ? queue.m_shaderStages : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			const VkPipelineStageFlags attachmentStages = queue.m_graphics ? depthStages : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			const VkPipelineStageFlags readOnlyStages = queue.m_graphics ? depthStages | queue.m_shaderStages : samplingStages;

			for (VkImageLayout layout : { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL })
			{
				Require(VulkanCommandBuffer::GetAccessFlags(layout, queue.m_flags) == (depthRead | depthWrite) &&
					VulkanCommandBuffer::GetPipelineStage(layout, queue.m_flags) == attachmentStages,
					"depth attachment writes and later reads must synchronize early and late tests only on a graphics queue");
			}
			Require(VulkanCommandBuffer::GetAccessFlags(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, queue.m_flags) == shaderRead &&
				VulkanCommandBuffer::GetPipelineStage(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, queue.m_flags) == samplingStages,
				"depth sampling and its return transition must include every shader stage supported by the recording queue");

			for (VkImageLayout layout : { VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
				VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL })
			{
				Require(VulkanCommandBuffer::GetAccessFlags(layout, queue.m_flags) == (depthRead | shaderRead) &&
					VulkanCommandBuffer::GetPipelineStage(layout, queue.m_flags) == readOnlyStages,
					"read-only depth layouts must cover shader sampling and depth tests without claiming a depth write");
			}
			for (VkImageLayout layout : { VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL })
			{
				Require(VulkanCommandBuffer::GetAccessFlags(layout, queue.m_flags) == (depthRead | depthWrite | shaderRead) &&
					VulkanCommandBuffer::GetPipelineStage(layout, queue.m_flags) == readOnlyStages,
					"mixed depth/stencil layouts must retain attachment writes and sampling of the read-only aspect");
			}
			Require(VulkanCommandBuffer::GetAccessFlags(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, queue.m_flags) ==
				(queue.m_graphics ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0u) &&
				VulkanCommandBuffer::GetPipelineStage(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, queue.m_flags) ==
				(queue.m_graphics ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
				"compute and transfer queues must not advertise color attachment operations");
			Require(VulkanCommandBuffer::GetAccessFlags(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, queue.m_flags) == VK_ACCESS_TRANSFER_READ_BIT &&
				VulkanCommandBuffer::GetAccessFlags(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, queue.m_flags) == VK_ACCESS_TRANSFER_WRITE_BIT &&
				VulkanCommandBuffer::GetPipelineStage(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, queue.m_flags) == VK_PIPELINE_STAGE_TRANSFER_BIT &&
				VulkanCommandBuffer::GetPipelineStage(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, queue.m_flags) == VK_PIPELINE_STAGE_TRANSFER_BIT,
				"transfer dependencies must retain their access masks and stages on every command queue");
		}
	}

	class ComputeBarrierForwardingProbe final : public VulkanGraphicsDriver
	{
	public:
		using VulkanGraphicsDriver::ImageMemoryBarrier;

		void ImageMemoryBarrier(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr image, RHI::EFormat format,
			RHI::EImageLayout oldLayout, RHI::EImageLayout newLayout) override
		{
			m_cmd = cmd;
			m_image = image;
			m_format = format;
			m_oldLayout = oldLayout;
			m_newLayout = newLayout;
			++m_calls;
		}

		RHI::RHICommandListPtr m_cmd;
		RHI::RHITexturePtr m_image;
		RHI::EFormat m_format = RHI::EFormat::UNDEFINED;
		RHI::EImageLayout m_oldLayout = RHI::EImageLayout::Undefined;
		RHI::EImageLayout m_newLayout = RHI::EImageLayout::Undefined;
		uint32_t m_calls = 0u;
	};

	void TestComputeWriteBarrierOverloadDirections()
	{
		ComputeBarrierForwardingProbe driver;
		const auto cmd = RHI::RHICommandListPtr::Make(RHI::ECommandListQueue::Graphics);
		const auto image = RHI::RHITexturePtr::Make(RHI::ETextureFiltration::Nearest, RHI::ETextureClamping::Clamp, false);
		constexpr auto format = RHI::EFormat::D32_SFLOAT_S8_UINT;
		for (auto layout : { RHI::EImageLayout::DepthStencilAttachmentOptimal, RHI::EImageLayout::ShaderReadOnlyOptimal })
		{
			for (bool allowWrite : { false, true })
			{
				const uint32_t calls = driver.m_calls;
				driver.ImageMemoryBarrier(cmd, image, format, layout, allowWrite);
				Require(driver.m_calls == calls + 1u && driver.m_cmd == cmd && driver.m_image == image && driver.m_format == format,
					"the compute-write overload must forward the original command list and image to the typed barrier");
				Require(driver.m_oldLayout == (allowWrite ? layout : RHI::EImageLayout::ComputeWrite) &&
					driver.m_newLayout == (allowWrite ? RHI::EImageLayout::ComputeWrite : layout),
					"allowing compute writes must enter ComputeWrite, and finishing them must restore the requested layout");
			}
		}
	}

	void TestBakedVolumeScalePerInstanceLayoutContract()
	{
		static_assert(std::is_same_v<DepthPrepassNode::CustomPerInstanceData, Framegraph::RenderSceneNode::PerInstanceData>);
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
			"custom depth must match the main layout while generic depth and shadows keep their compact std430 records");

		RHI::RHIMesh mesh;
		Require(mesh.m_bakedVolumeScale == glm::vec3(1.0f) &&
			renderInstance.bakedVolumeScale == glm::vec4(1.0f) &&
			customDepthInstance.bakedVolumeScale == glm::vec4(1.0f) &&
			shadowInstance.bakedVolumeScale == glm::vec4(1.0f),
			"procedural and legacy meshes must default to an identity baked volume scale");

	}

	void TestUnskinnedInstanceDefaults()
	{
		const RHI::RHISceneInstanceRecord unskinnedInstance{};
		Require(unskinnedInstance.m_skeletonOffset ==
			(std::numeric_limits<uint32_t>::max)(),
			"instances without an animator must use the invalid skeleton offset so meshes with unused bone attributes stay rigid in the depth pass");

	}

	void TestPathTracerThicknessSlotContract()
	{
		Raytracing::Material material{};
		Require(!material.HasThicknessTexture(),
			"a path-tracing material must default to no thickness texture");
		material.m_thicknessIndex = 0;
		Require(material.HasThicknessTexture(),
			"a valid thickness texture slot must be detected");
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
		material->SetUniform("material.transmissionFactor"_h, 1.0f);
		Require(material->GetContentRevision() > revision,
			"changing a scalar uniform must advance the material content revision");
		Require(Material::GetGlobalContentRevision() > globalRevision,
			"a material mutation must publish the global revision gate used by dirty mesh updates");
		Require(material->GetRenderMetadataRevision() == renderMetadataRevision,
			"main-pass-only uniforms must not invalidate cached depth, shadow, or scene topology");

		revision = material->GetContentRevision();
		material->SetUniform("material.attenuationColor"_h, glm::vec4(0.9f, 0.6f, 0.1f, 1.0f));
		Require(material->GetContentRevision() > revision,
			"changing a vector uniform must advance the material content revision");
		Require(material->GetRenderMetadataRevision() == renderMetadataRevision,
			"ordinary vector uniforms must remain material-version-only changes");

		revision = material->GetContentRevision();
		material->SetUniform("material.baseColorFactor"_h, glm::vec4(0.5f));
		Require(material->GetContentRevision() > revision &&
			material->GetRenderMetadataRevision() > renderMetadataRevision,
			"masked depth alpha metadata must invalidate cached proxy metadata");
		renderMetadataRevision = material->GetRenderMetadataRevision();

		revision = material->GetContentRevision();
		material->SetUniform(
			"material.baseColorFactor"_h,
			glm::vec4(0.8f, 0.7f, 0.6f, 0.5f));
		Require(material->GetContentRevision() > revision,
			"changing base color RGB must advance the material binding version");
		Require(material->GetRenderMetadataRevision() == renderMetadataRevision,
			"base color RGB must not invalidate alpha-only depth and shadow metadata");

		revision = material->GetContentRevision();
		material->SetUniform(
			"material.baseColorFactor"_h,
			glm::vec4(0.8f, 0.7f, 0.6f, 0.5f));
		Require(material->GetContentRevision() == revision,
			"assigning the same material uniform must not publish a redundant revision");

		revision = material->GetContentRevision();
		material->SetUniform("material.alphaCutoff"_h, 0.5f);
		Require(material->GetContentRevision() > revision,
			"adding an explicit alpha cutoff must advance the material binding version");
		Require(material->GetRenderMetadataRevision() == renderMetadataRevision,
			"the default alpha cutoff must not invalidate equivalent proxy metadata");

		material->SetUniform("material.alphaCutoff"_h, 0.35f);
		Require(material->GetRenderMetadataRevision() > renderMetadataRevision,
			"changing the effective alpha cutoff must invalidate depth and shadow metadata");
		renderMetadataRevision = material->GetRenderMetadataRevision();

		revision = material->GetContentRevision();
		material->SetSampler("thicknessSampler"_h, texture);
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

namespace
{
	void TestWorkspaceFrameGraphNodeExports()
	{
		const std::pair<StringHash, std::string_view> names[] = {
			{ Framegraph::BlitNode::GetName(), "Blit" },
			{ Framegraph::BloomNode::GetName(), "Bloom" },
			{ Framegraph::CopyTextureToRamNode::GetName(), "CopyTextureToRam" },
			{ Framegraph::EnvironmentNode::GetName(), "Environment" },
			{ Framegraph::EyeAdaptationNode::GetName(), "EyeAdaptation" },
			{ Framegraph::PostProcessNode::GetName(), "PostProcess" }
		};
		for (const auto& [actual, expected] : names)
		{
			Require(!actual.IsEmpty() && actual.ToString() == expected,
				"workspace consumers must resolve frame-graph node identities across the runtime library boundary");
		}
	}

	void TestAtmosphericFogBlendAndDisabledPass()
	{
		VulkanPipelineStateBuilder builder(nullptr);
		auto vertices = RHI::RHIVertexDescriptionPtr::Make();
		vertices->SetVertexStride(sizeof(glm::vec3));
		vertices->AddAttribute(0, 0, RHI::EFormat::R32G32B32_SFLOAT, 0);
		// Prime the cache with a different blend/cull combination that previously
		// aliased the fog state. Validate the compiled pipeline, not its hash.
		const RHI::RenderState opaqueState(false, false, 0, false, RHI::ECullMode::Front,
			RHI::EBlendMode::None, RHI::EFillMode::Fill, 0, false);
		builder.BuildPipeline(vertices, { 0u }, RHI::EPrimitiveTopology::TriangleList,
			opaqueState, { VK_FORMAT_R16G16B16A16_SFLOAT }, VK_FORMAT_UNDEFINED);
		const RHI::RenderState renderState(false, false, 0, false, RHI::ECullMode::None,
			RHI::EBlendMode::AlphaBlendingPreserveAlpha, RHI::EFillMode::Fill, 0, false);
		const auto& states = builder.BuildPipeline(vertices, { 0u }, RHI::EPrimitiveTopology::TriangleList,
			renderState, { VK_FORMAT_R16G16B16A16_SFLOAT }, VK_FORMAT_UNDEFINED);
		VkGraphicsPipelineCreateInfo pipeline{};
		for (const auto& state : states)
		{
			state->Apply(pipeline);
		}
		Require(pipeline.pColorBlendState && pipeline.pColorBlendState->attachmentCount == 1,
			"Fog compositing must affect a single colour attachment");
		const auto& blend = pipeline.pColorBlendState->pAttachments[0];
		Require(blend.blendEnable && blend.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA &&
			blend.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA && blend.colorBlendOp == VK_BLEND_OP_ADD &&
			blend.srcAlphaBlendFactor == VK_BLEND_FACTOR_ZERO && blend.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
			blend.alphaBlendOp == VK_BLEND_OP_ADD, "Fog must composite RGB without changing HDR alpha metadata");

		Framegraph::AtmosphericFogNode node;
		Require(Framegraph::AtmosphericFogNode::GetName() == "AtmosphericFog"_h &&
			node.GetDebugName() == "AtmosphericFog", "Fog node identity must be accessible across the runtime library boundary");
		RHI::RHISceneViewSnapshot scene{};
		// A disabled or invalid optional node must need no renderer/resources and issue no draw.
		for (float density : { 0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN() })
		{
			node.SetVec4("fog"_h, glm::vec4(density, 0, 0, 0));
			node.Process(nullptr, nullptr, nullptr, scene);
			Require(node.GetDrawCallStats().m_numBatches == 0, "Disabled fog must not submit a pass");
		}
	}

	void TestShadowDistanceSettings()
	{
		const auto path = std::filesystem::path(SAILOR_TEST_SOURCE_DIR) / "ProjectSettings.yaml";
		const auto source = YAML::Load(ReadText(path));
		for (float distance : { 1.0f, 600.0f, 10000.0f, 0.0f, 10001.0f,
			std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
		{
			auto document = YAML::Clone(source);
			document["graphics"]["presets"]["Ultra"]["shadowDistance"] = distance;
			const auto parsed = Settings::ParseProjectGraphicsSettings(YAML::Dump(document), path.string());
			const bool valid = std::isfinite(distance) && distance >= 1 && distance <= 10000;
			Require(parsed.IsLoaded() == valid, "Native shadow distance must require a finite value in [1, 10000]");
			if (valid) Require(parsed.m_settings.GetProfile(Settings::EGraphicsQuality::Ultra).m_shadowDistance == distance,
				"Native shadow distance parser must preserve the requested range");
		}
	}
}

namespace
{
	template<typename Instance>
	void BenchmarkDensePackets(std::string_view name)
	{
		using Clock = std::chrono::steady_clock;
		constexpr uint32_t numRanges = 10000u;
		constexpr uint32_t instancesPerRange = 4u;
		constexpr uint32_t repetitions = 9u;
		TVector<Instance> values(instancesPerRange);
		for (auto& value : values)
		{
			value.model = glm::mat4(1.0f);
			value.sphereBounds = glm::vec4(1.0f);
		}
		TVector<uint64_t> keys(instancesPerRange);
		for (uint32_t numViews : { 1u, 4u })
		{
			RHI::TPackedDrawPagedArenaCache<Instance> cache;
			std::array<std::array<double, repetitions>, 2> times{};
			for (uint32_t iteration = 0; iteration <= repetitions; ++iteration)
			{
				values[0].model[3].x = static_cast<float>(iteration);
				std::array<RHI::TPackedDrawPacket<Instance>, 4> arenaPackets;
				std::array<RHI::TPackedDrawPacket<Instance>, 4> packedPackets;
				const auto arenaStart = Clock::now();
				cache.BeginUpdate(1u, iteration, iteration);
				for (uint32_t range = 0; range < numRanges; ++range)
				{
					for (uint32_t i = 0; i < instancesPerRange; ++i) keys[i] = uint64_t(range) * instancesPerRange + i;
					Require(cache.ReplaceRange(range, iteration, values, keys), "dense update must succeed");
				}
				const auto payload = cache.EndUpdate();
				for (uint32_t view = 0; view < numViews; ++view)
				{
					auto& packet = arenaPackets[view];
					packet.UseSharedArenaPayload(EMobilityType::Static, payload);
					for (uint32_t range = 0; range < numRanges; ++range)
						for (uint32_t i = 0; i < instancesPerRange; ++i)
							Require(packet.AddArenaView({}, {}, range, uint64_t(range) * instancesPerRange + i),
								"dense view must resolve every instance");
					packet.Finalize(false);
				}
				const auto arenaEnd = Clock::now();
				const auto packedStart = Clock::now();
				for (uint32_t view = 0; view < numViews; ++view)
				{
					auto& packet = packedPackets[view];
					for (uint32_t range = 0; range < numRanges; ++range)
						for (uint32_t i = 0; i < instancesPerRange; ++i)
							packet.Add({}, {}, values[i], uint64_t(range) * instancesPerRange + i, EMobilityType::Static);
					packet.Finalize(false);
				}
				const auto packedEnd = Clock::now();
				for (uint32_t view = 0; view < numViews; ++view)
				{
					Require(arenaPackets[view].GetNumDrawInstances() == numRanges * instancesPerRange &&
						packedPackets[view].GetNumDrawInstances() == arenaPackets[view].GetNumDrawInstances() &&
						packedPackets[view].GetInstanceIndices() == arenaPackets[view].GetInstanceIndices(),
						"dense paths must emit the same instances in the same order for every view");
					const auto& packed = packedPackets[view].GetPayload(EMobilityType::Static).m_instances;
					for (uint32_t i = 0; i < numRanges * instancesPerRange; ++i)
						Require(payload->m_arenaPages[i / RHI::TPackedDrawArenaPage<Instance>::NumInstances]->
							m_instances[i % RHI::TPackedDrawArenaPage<Instance>::NumInstances] == packed[i] &&
							packed[i] == values[i % instancesPerRange], "dense payload fields must match outside the timed section");
				}
				if (iteration > 0u)
				{
					times[0][iteration - 1u] = std::chrono::duration<double, std::micro>(arenaEnd - arenaStart).count();
					times[1][iteration - 1u] = std::chrono::duration<double, std::micro>(packedEnd - packedStart).count();
				}
			}
			std::cout << name << ',' << sizeof(Instance) << ',' << numViews;
			for (auto& samples : times)
			{
				std::sort(samples.begin(), samples.end());
				std::cout << ',' << samples[repetitions / 2u];
			}
			std::cout << '\n';
		}
	}

	void BenchmarkPagedArena()
	{
		using Clock = std::chrono::steady_clock;
		using Instance = std::array<uint32_t, 32>;
		using Cache = RHI::TPackedDrawPagedArenaCache<Instance>;
		constexpr uint32_t numRanges = 10000u;
		constexpr uint32_t instancesPerRange = 4u;
		constexpr uint32_t repetitions = 9u;
		TVector<Instance> values(instancesPerRange);
		TVector<uint64_t> keys{ 0u, 1u, 2u, 3u };
		std::cout << "ranges,changed,begin_us,producers_us,end_us,total_us,copied_ranges,changed_pages,dirty_page_bytes\n";
		for (uint32_t changed : { 1u, 100u, numRanges })
		{
			Cache cache;
			cache.BeginUpdate(1u, 0u, 0u);
			for (uint32_t range = 0; range < numRanges; ++range)
				Require(cache.ReplaceRange(range, 0u, values, keys), "benchmark range must initialize");
			auto previous = cache.EndUpdate();
			std::array<std::array<double, repetitions>, 4> times{};
			uint32_t copiedRanges = 0u;
			uint32_t changedPages = 0u;
			for (uint32_t iteration = 0; iteration < repetitions; ++iteration)
			{
				const uint64_t revision = iteration + 1u;
				values[0][0] = static_cast<uint32_t>(revision);
				const auto start = Clock::now();
				cache.BeginUpdate(1u, revision, revision);
				const auto begun = Clock::now();
				for (uint32_t range = 0; range < numRanges; ++range)
				{
					// Spread changes across the arena, as independent scene producers do.
					const bool bChanged = range % (numRanges / changed) == 0u;
					if (bChanged)
						Require(cache.ReplaceRange(range, revision, values, keys), "benchmark update must succeed");
					else
						Require(cache.TryReuseRange(range, 0u), "benchmark unchanged range must remain reusable");
				}
				const auto replaced = Clock::now();
				auto current = cache.EndUpdate();
				const auto ended = Clock::now();
				auto micros = [](auto duration) { return std::chrono::duration<double, std::micro>(duration).count(); };
				times[0][iteration] = micros(begun - start);
				times[1][iteration] = micros(replaced - begun);
				times[2][iteration] = micros(ended - replaced);
				times[3][iteration] = micros(ended - start);
				copiedRanges = changedPages = 0u;
				for (uint32_t range = 0; range < numRanges; ++range)
				{
					const auto* oldRange = previous->FindRange(range);
					const auto* newRange = current->FindRange(range);
					Require(oldRange && newRange,
						"benchmark publications must retain all ranges");
					copiedRanges += oldRange != newRange;
				}
				for (uint32_t page = 0; page < current->m_arenaPages.Num(); ++page)
					changedPages += previous->m_arenaPages[page] != current->m_arenaPages[page];
				previous = std::move(current);
			}
			std::cout << numRanges << ',' << changed;
			for (auto& samples : times)
			{
				std::sort(samples.begin(), samples.end());
				std::cout << ',' << samples[repetitions / 2u];
			}
			std::cout << ',' << copiedRanges << ',' << changedPages << ','
				<< uint64_t(changedPages) * Cache::PageSize * sizeof(Instance) << '\n';
		}
		std::cout << "delta_ranges,changed,compared_slots,producer_visits,total_us\n";
		for (uint32_t changed : { 1u, 100u, numRanges })
		{
			auto scene = RHI::RHIScenePtr::Make();
			const auto topology = RHI::RHISceneProxyResourcePtr::Make(RHI::RHISceneViewProxy{});
			RHI::RHISceneInstanceRecord record;
			record.m_mobility = EMobilityType::Stationary;
			record.m_topology = topology;
			TVector<RHI::RenderInstanceHandle> handles;
			for (uint32_t i = 0; i < numRanges; ++i)
			{
				record.m_producerKey = i;
				handles.Add(scene->AddInstance(record));
			}
			Cache cache;
			RHI::RHIPackedDrawSceneChanges changes;
			std::array<double, repetitions> times{};
			uint32_t comparedRecords = 0u;
			uint32_t producerVisits = 0u;
			for (uint32_t iteration = 0; iteration <= repetitions; ++iteration)
			{
				if (iteration > 0u)
				{
					for (uint32_t i = 0; i < numRanges; i += numRanges / changed)
					{
						record.m_producerKey = i;
						record.m_worldMatrix[3].x = static_cast<float>(iteration);
						Require(scene->UpdateInstance(handles[i], record, RHI::ToMask(RHI::ESceneChangeBit::Transform)),
							"benchmark scene mutation must succeed");
					}
				}
				auto versions = TSharedPtr<TVector<RHI::RHISceneVersionPtr>>::Make();
				versions->Add(scene->PublishVersion());
				const RHI::RHIPackedDrawSceneState state{ std::move(versions) };
				values[0][0] = iteration;
				const auto started = Clock::now();
				changes.Gather(state, cache.GetSceneState(1u), EMobilityType::Stationary);
				cache.BeginUpdate(1u, iteration, iteration, state);
				for (const auto& proxy : changes.m_updated)
				{
					const auto rangeKey = RHI::BuildPackedDrawRangeKey(proxy.m_handle, proxy.m_record->m_producerKey, proxy.m_resource);
					Require(!cache.TryReuseRange(rangeKey, iteration) && cache.ReplaceRange(rangeKey, iteration, values, keys),
						"benchmark must rebuild each changed producer exactly once");
				}
				const auto payload = cache.EndUpdate();
				const auto ended = Clock::now();
				producerVisits = static_cast<uint32_t>(changes.m_updated.Num());
				comparedRecords = changes.m_numComparedRecords;
				Require(payload->m_arenaRangeIndices->Num() == numRanges &&
					producerVisits == (iteration == 0u ? numRanges : changed) && changes.m_removed.IsEmpty(),
					"delta benchmark must retain all ranges and visit exactly the changed producers");
				if (iteration > 0u) times[iteration - 1u] = std::chrono::duration<double, std::micro>(ended - started).count();
				changes.Clear();
			}
			std::sort(times.begin(), times.end());
			std::cout << numRanges << ',' << changed << ',' << comparedRecords << ',' << producerVisits << ','
				<< times[repetitions / 2u] << '\n';
		}
		std::cout << "dense_pass,instance_bytes,views,paged_us,packed_us\n";
		BenchmarkDensePackets<Framegraph::RenderSceneNode::PerInstanceData>("main");
		BenchmarkDensePackets<DepthPrepassNode::PerInstanceData>("depth");
		BenchmarkDensePackets<ShadowPrepassNode::PerInstanceData>("shadow");
	}
}

int main(int argc, char** argv)
{
	if (argc == 2 && std::string_view(argv[1]) == "--benchmark-arena")
	{
		BenchmarkPagedArena();
		return 0;
	}
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "WorkspaceFrameGraphNodeExports", TestWorkspaceFrameGraphNodeExports },
		{ "AtmosphericFogBlendAndDisabledPass", TestAtmosphericFogBlendAndDisabledPass },
		{ "ShadowDistanceSettings", TestShadowDistanceSettings },
		{ "DepthAttachmentFrameGraphContract", TestDepthAttachmentFrameGraphContract },
		{ "RendererGpuCullingPassContract", TestRendererGpuCullingPassContract },
		{ "CurrentDepthPyramidReadiness", TestCurrentDepthPyramidReadiness },
		{ "FrameGraphSequenceMappings", TestFrameGraphSequenceMappings },
		{ "GpuCullingDispatchOrdering", TestGpuCullingDispatchOrdering },
		{ "QueueShaderStagesUseCapabilities", TestQueueShaderStagesUseCapabilities },
		{ "MotionMrtFrameGraphContract", TestMotionMrtFrameGraphContract },
		{ "PcfRasterShadowBiasContract", TestPcfRasterShadowBiasContract },
		{ "MipExtentUsesVulkanFloorAndClamp", TestMipExtentUsesVulkanFloorAndClamp },
		{ "PackedDrawMobilityPayloadVirtualization", TestPackedDrawMobilityPayloadVirtualization },
		{ "PackedDrawBatchInstanceLimit", TestPackedDrawBatchInstanceLimit },
		{ "PackedDrawMixedMaterialSort", TestPackedDrawMixedMaterialSort },
		{ "MaterialVersionPublicationContract", TestMaterialVersionPublicationContract },
		{ "ConcurrentArenaPublications", TestConcurrentArenaPublications },
		{ "PagedArenaMetadataPublication", TestPagedArenaMetadataPublication },
		{ "PackedDrawSceneChanges", TestPackedDrawSceneChanges },
		{ "MaterialVersionsSurviveConcurrentWorldUpdates", TestMaterialVersionsSurviveConcurrentWorldUpdates },
		{ "PackedDrawEqualKeysPreserveBindingOrder", TestPackedDrawEqualKeysPreserveBindingOrder },
		{ "DynamicSpatialRootIsolation", TestDynamicSpatialRootIsolation },
		{ "ParallelSpatialIndexVisibility", TestParallelSpatialIndexVisibility },
		{ "InstancedViewLodAndDistanceContract", TestInstancedViewLodAndDistanceContract },
		{ "SnapshotCameraLodContract", TestSnapshotCameraLodContract },
		{ "BatchTextureBindingIdentityContract", TestBatchTextureBindingIdentityContract },
		{ "CustomShadowMaterialKey", TestCustomShadowMaterialKey },
		{ "TextureSamplerPublication", TestTextureSamplerPublication },
		{ "ShaderReadOnlyBarrierSynchronizesShaderSampling", TestShaderReadOnlyBarrierSynchronizesShaderSampling },
		{ "DepthSamplingBarrierScopes", TestDepthSamplingBarrierScopes },
		{ "ComputeWriteBarrierOverloadDirections", TestComputeWriteBarrierOverloadDirections },
		{ "BakedVolumeScalePerInstanceLayoutContract", TestBakedVolumeScalePerInstanceLayoutContract },
		{ "UnskinnedInstanceDefaults", TestUnskinnedInstanceDefaults },
		{ "PathTracerThicknessSlotContract", TestPathTracerThicknessSlotContract },
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
