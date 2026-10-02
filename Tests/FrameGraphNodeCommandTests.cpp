#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "FrameGraph/PostProcessNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "FrameGraph/RenderSceneNode.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/VertexDescription.h"

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace Sailor::GraphicsDriver::Vulkan
{
	class FrameGraphNodeTestAccess
	{
	public:
		static void ExchangeBeginRendering(VulkanDevice& device, PFN_vkCmdBeginRenderingKHR& dispatch)
		{
			auto& active = device.pVkCmdBeginRenderingKHR ? device.pVkCmdBeginRenderingKHR : device.pVkCmdBeginRendering;
			std::swap(active, dispatch);
		}
	};
}

namespace Sailor::Framegraph
{
	class PostProcessNodeTestAccess
	{
	public:
		static RHIShaderBindingSetPtr GetBindings(const PostProcessNode& node) { return node.m_shaderBindings; }
	};
}

namespace
{
	constexpr uint32_t Side = 8;
	const auto HostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;

	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	class TestGraph : public RHIFrameGraph
	{
	public:
		TestGraph()
		{
			auto& driver = Renderer::GetDriver();
			m_postEffectPlane = RHIMeshPtr::Make();
			m_postEffectPlane->m_vertexDescription = driver->GetOrAddVertexDescription<VertexP3N3UV2C4>();
			std::array<VertexP3N3UV2C4, 4> vertices{};
			for (uint32_t i = 0; i < vertices.size(); ++i)
				vertices[i].m_position = glm::vec3(i % 2 ? 1 : -1, i / 2 ? 1 : -1, 0);
			const uint32_t indices[] = { 0, 1, 2, 2, 1, 3 };
			m_postEffectPlane->m_vertexBuffer = driver->CreateBuffer(sizeof(vertices), EBufferUsageBit::VertexBuffer_Bit, HostMemory);
			m_postEffectPlane->m_indexBuffer = driver->CreateBuffer(sizeof(indices), EBufferUsageBit::IndexBuffer_Bit, HostMemory);
			std::memcpy(m_postEffectPlane->m_vertexBuffer->GetPointer(), vertices.data(), sizeof(vertices));
			std::memcpy(m_postEffectPlane->m_indexBuffer->GetPointer(), indices, sizeof(indices));
		}
	};

	PFN_vkCmdBeginRenderingKHR originalBeginRendering = nullptr;
	VkRenderingAttachmentInfo recordedColor{};
	VkRenderingAttachmentInfo recordedMotion{};
	uint32_t recordedColorCount = 0;

	VKAPI_ATTR void VKAPI_CALL CaptureRendering(VkCommandBuffer command, const VkRenderingInfo* info)
	{
		recordedColorCount = info->colorAttachmentCount;
		if (recordedColorCount) recordedColor = info->pColorAttachments[0];
		if (recordedColorCount > 1) recordedMotion = info->pColorAttachments[1];
		originalBeginRendering(command, info);
	}

	struct CaptureAttachments
	{
		CaptureAttachments()
		{
			// No App::Start; the scheduler queues are drained before installing the observer.
			auto device = VulkanApi::GetInstance()->GetMainDevice();
			originalBeginRendering = CaptureRendering;
			FrameGraphNodeTestAccess::ExchangeBeginRendering(*device, originalBeginRendering);
			Require(originalBeginRendering != nullptr, "native dynamic-rendering dispatch must be available");
		}
		~CaptureAttachments() { FrameGraphNodeTestAccess::ExchangeBeginRendering(*VulkanApi::GetInstance()->GetMainDevice(), originalBeginRendering); }
	};

	std::string WriteShader(const std::filesystem::path& workspace, bool large)
	{
		const std::string name = large ? "LargePostProcess.shader" : "MutablePostProcess.shader";
		const auto path = workspace / "Content" / name;
		YAML::Node shader;
		shader["glslCommon"] = "#version 450\n";
		shader["defines"].push_back("INVERT");
		shader["colorAttachments"].push_back("R32G32B32A32_SFLOAT");
		shader["glslVertex"] = "layout(location = 0) in vec3 position; void main() { gl_Position = vec4(position, 1); }";
		shader["glslFragment"] = std::string("layout(std140, set = 1, binding = ") + (large ? "3" : "0") +
			") uniform Data { " + (large ? "vec4 padding[32]; " : "") + R"glsl(
	vec4 tint; float gain;
} data;
layout(set = 1, binding = 1) uniform sampler2D sourceSampler;
layout(location = 0) out vec4 outColor;
void main() {
	outColor = data.tint * data.gain + texelFetch(sourceSampler, ivec2(0), 0);
#ifdef INVERT
	outColor = vec4(1) - outColor;
#endif
}
)glsl";
		std::ofstream output(path);
		output << shader;
		output.close();
		Require(static_cast<bool>(output), "post-process shader fixture must be written");
		const auto id = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		ShaderSetPtr compiled;
		Require(App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(id, compiled) && compiled && compiled->IsReady(),
			"post-process shader must compile before recording");
		Require(App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(id, compiled, { "INVERT" }) && compiled && compiled->IsReady(),
			"post-process shader permutation must compile before recording");
		return name;
	}

	FileId WriteGraph(const std::filesystem::path& workspace)
	{
		const auto path = workspace / "Content" / "AttachmentBindings.renderer";
		std::ofstream output(path);
		output << R"yaml(
renderTargets:
  - name: StaticSurface
    format: R32G32B32A32_SFLOAT
    width: 8
    height: 8
    bIsSurface: true
  - name: StaticTexture
    format: R32G32B32A32_SFLOAT
    width: 8
    height: 8
frame:
  - name: PostProcess
    tag: Bindings
    renderTargets:
      - color: StaticSurface
      - sourceSampler: StaticTexture
      - externalSampler: DynamicInput
)yaml";
		output.close();
		Require(static_cast<bool>(output), "frame-graph resource fixture must be written");
		return App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
	}

	void TestImportedBindings(FileId id)
	{
		FrameGraphPtr instance;
		Require(App::GetSubmodule<FrameGraphImporter>()->LoadFrameGraph_Immediate(id, instance) && instance,
			"the real importer must build the attachment graph");
		auto graph = instance->GetRHI();
		auto node = graph->GetGraphNode("Bindings");
		const auto surface = graph->GetSurface("StaticSurface");
		const auto texture = graph->GetRenderTarget("StaticTexture");
		Require(node && surface && texture, "imported graph must contain native surfaces and targets");
		Require(node->GetRHIResource("color") == surface && node->GetRHIResource("sourceSampler") == texture &&
			node->GetResolvedAttachment("color") == surface->GetResolved(),
			"static graph binding must retain the surface rather than discard its multisampled target");
		Require(!node->GetRHIResource("externalSampler", graph.GetRawPtr()), "unpublished external input must remain unresolved");
		graph->SetSampler("DynamicInput", texture);
		Require(node->GetSampledAttachment("externalSampler", graph.GetRawPtr()) == texture,
			"external graph input must resolve when its producer publishes it");
		graph->SetSampler("DynamicInput", surface->GetResolved());
		Require(node->GetSampledAttachment("externalSampler", graph.GetRawPtr()) == surface->GetResolved() &&
			node->GetRHIResource("sourceSampler") == texture,
			"external replacement must refresh independently of fixed graph bindings");
		std::cout << "FrameGraph static surface/target binding and external publication passed\n";
	}

	class SceneNode : public RenderSceneNode
	{
	public:
		TRefPtr<SubmissionResources> GetResources(const RHISceneViewSnapshot& scene)
		{
			return scene.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(this, scene.m_cameraIndex, 0);
		}
	};

	ShaderSetPtr WriteMrtShader(const std::filesystem::path& workspace)
	{
		const auto path = workspace / "Content" / "MotionMrt.shader";
		YAML::Node shader;
		shader["glslCommon"] = "#version 450\n";
		shader["colorAttachments"].push_back("R32G32B32A32_SFLOAT");
		shader["colorAttachments"].push_back("R32G32B32A32_SFLOAT");
		shader["glslVertex"] = "layout(location = 0) in vec3 position; void main() { gl_Position = vec4(position, 1); }";
		shader["glslFragment"] = R"glsl(
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outMotion;
void main() {
	if (gl_FragCoord.x >= 4) discard;
	outColor = vec4(0.75, 0.5, 0.25, 1);
	outMotion = vec4(-0.5, 0.25, 0.125, 0);
}
)glsl";
		std::ofstream output(path);
		output << shader;
		output.close();
		Require(static_cast<bool>(output), "MRT shader fixture must be written");
		const auto id = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		ShaderSetPtr compiled;
		Require(App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(id, compiled) && compiled && compiled->IsReady(),
			"MRT shader must compile before recording");
		return compiled;
	}

	void TestSceneMrt(ShaderSetPtr shader, bool colorIsSurface, bool motionIsSurface, bool late, bool forceSingleSample = false)
	{
		auto driver = Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		auto commands = Renderer::GetDriverCommands();
		const bool msaa = !forceSingleSample && VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
		auto graph = TRefPtr<TestGraph>::Make();
		const bool surfaces[] = { colorIsSurface, motionIsSurface };
		const char* names[] = { "Color", "Motion" };
		const char* inputs[] = { "color", "motionVectors" };
		std::array<RHITexturePtr, 2> targets;
		std::array<RHIRenderTargetPtr, 2> outputs;
		std::array<RHIResourcePtr, 2> resources;
		for (uint32_t i = 0; i < 2; ++i)
		{
			if (surfaces[i])
			{
				auto surface = driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
				if (forceSingleSample) surface = RHISurfacePtr::Make(surface->GetResolved(), surface->GetResolved(), false);
				resources[i] = surface;
				outputs[i] = surface->GetResolved();
				targets[i] = surface->GetTarget();
				graph->SetSurface(names[i], surface);
			}
			else
			{
				outputs[i] = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
				resources[i] = outputs[i];
				targets[i] = msaa ? driver->GetOrAddMsaaFramebufferRenderTarget(EFormat::R32G32B32A32_SFLOAT, glm::ivec2(Side), i) : RHITexturePtr(outputs[i]);
			}
			graph->SetRenderTarget(names[i], outputs[i]);
		}
		auto depth = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::D32_SFLOAT_S8_UINT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit);
		graph->SetRenderTarget("SceneDepth", depth);
		RHISceneViewSnapshot scene;
		scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		scene.m_submissionContext->BeginSubmission(162, 0);
		scene.m_frameBindings = driver->CreateShaderBindings();
		scene.m_rhiLightsData = driver->CreateShaderBindings();
		auto node = TRefPtr<SceneNode>::Make();
		node->SetString("Tag", "MotionMrt");
		node->SetString("GPUCulling", "false");
		for (uint32_t i = 0; i < 2; ++i)
		{
			if (late) node->SetRHIResource_Unresolved(inputs[i], names[i]);
			else node->SetRHIResource(inputs[i], resources[i]);
		}
		if (late) node->SetRHIResource_Unresolved("depthStencil", "SceneDepth");
		else node->SetRHIResource("depthStencil", depth);
		auto mesh = graph->GetFullscreenNdcQuad();
		const RenderState state(false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, msaa);
		auto material = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shader);
		Require(material && material->GetVersion(), "MRT fixture needs a real material version");
		RHIBatch batch(material, mesh);
		batch.m_textureBindings = driver->CreateShaderBindings();
		SceneNode::PerInstanceData instance{};
		instance.model = glm::mat4(1);
		instance.sphereBounds = glm::vec4(0, 0, 0, 1);
		auto submission = node->GetResources(scene);
		submission->m_packet.Add(batch, mesh, instance);
		submission->m_packet.Finalize();
		CaptureAttachments capture;
		const glm::vec4 background[] = { glm::vec4(0.125f), glm::vec4(-0.25f) };
		const glm::vec4 drawn[] = { glm::vec4(0.75f, 0.5f, 0.25f, 1), glm::vec4(-0.5f, 0.25f, 0.125f, 0) };
		for (uint32_t frame = 0; frame < 2; ++frame)
		{
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			upload->m_vulkan.m_commandBuffer->AddDependency(scene.m_submissionContext);
			draw->m_vulkan.m_commandBuffer->AddDependency(scene.m_submissionContext);
			commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
			for (uint32_t i = 0; i < 2; ++i)
			{
				if (frame == 0)
				{
					commands->ImageMemoryBarrier(draw, targets[i], EImageLayout::TransferDstOptimal);
					commands->ClearImage(draw, targets[i], background[i]);
					commands->ImageMemoryBarrier(draw, targets[i], EImageLayout::ColorAttachmentOptimal);
				}
				if (msaa)
				{
					commands->ImageMemoryBarrier(draw, outputs[i], EImageLayout::TransferDstOptimal);
					commands->ClearImage(draw, outputs[i], glm::vec4(-8));
				}
			}
			recordedColorCount = 0;
			node->Process(graph, upload, draw, scene);
			const auto descriptors = std::array{ recordedColor, recordedMotion };
			bool valid = recordedColorCount == 2 && node->GetDrawCallStats().m_numBatches == 1;
			for (uint32_t i = 0; i < 2 && valid; ++i)
			{
				const auto& descriptor = descriptors[i];
				valid = descriptor.imageView == static_cast<VkImageView>(*targets[i]->m_vulkan.m_imageView) &&
					descriptor.resolveImageView == (msaa ? static_cast<VkImageView>(*outputs[i]->m_vulkan.m_imageView) : VK_NULL_HANDLE) &&
					descriptor.resolveMode == (msaa ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE) &&
					descriptor.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && descriptor.storeOp == VK_ATTACHMENT_STORE_OP_STORE;
			}
			if (!valid)
			{
				commands->EndCommandList(upload);
				commands->EndCommandList(draw);
				upload->m_vulkan.m_commandBuffer->Reset();
				draw->m_vulkan.m_commandBuffer->Reset();
				throw std::runtime_error("RenderScene MRT dropped or replaced a native color/motion attachment: count=" +
					std::to_string(recordedColorCount) + ", batches=" + std::to_string(node->GetDrawCallStats().m_numBatches));
			}
			std::array<RHIBufferPtr, 2> readback;
			for (uint32_t i = 0; i < 2; ++i)
			{
				readback[i] = driver->CreateBuffer(Side * Side * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
				commands->ImageMemoryBarrier(draw, outputs[i], EImageLayout::TransferSrcOptimal);
				commands->CopyImageToBuffer(draw, outputs[i], readback[i]);
			}
			commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			commands->EndCommandList(upload);
			commands->EndCommandList(draw);
			auto ready = driver->CreateWaitSemaphore();
			auto uploaded = RHIFencePtr::Make();
			auto finished = RHIFencePtr::Make();
			Require(driver->SubmitCommandList(upload, uploaded, ready) && driver->SubmitCommandList(draw, finished, nullptr, ready),
				"MRT upload and draw must submit");
			Require(finished->Wait(5000000000ull) == EFenceStatus::Finished && uploaded->Wait(5000000000ull) == EFenceStatus::Finished,
				"MRT GPU readbacks must finish");
			for (uint32_t image = 0; image < 2; ++image)
			{
				const auto pixels = static_cast<const glm::vec4*>(readback[image]->GetPointer());
				for (uint32_t i = 0; i < Side * Side; ++i)
					for (uint32_t component = 0; component < 4; ++component)
					{
						const auto expected = i % Side < Side / 2 ? drawn[image] : background[image];
						Require(std::isfinite(pixels[i][component]) && std::abs(pixels[i][component] - expected[component]) < 0.00001f,
							"MRT must draw both outputs and preserve every uncovered pixel in the live target");
					}
			}
		}
		std::cout << "RenderScene MRT " << (msaa ? "2x" : "1x") << " colorSurface=" << colorIsSurface <<
			" motionSurface=" << motionIsSurface << " late=" << late << ": two frames / both images and native descriptors passed\n";
	}

	void TestPostProcess(const std::string& smallShader, const std::string& largeShader)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		Require(VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() == VK_SAMPLE_COUNT_2_BIT,
			"the private command-test workspace must exercise real 2x MSAA");
		auto graph = TRefPtr<TestGraph>::Make();
		auto output = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		auto single = RHISurfacePtr::Make(output, output, false);
		auto multisampled = driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		const glm::vec4 firstTexel(0.125f, 0.25f, 0.5f, 1);
		const glm::vec4 secondTexel(0.75f, 0.5f, 0.25f, 0);
		auto textureA = driver->CreateTexture(&firstTexel, sizeof(firstTexel), glm::ivec3(1), 1, ETextureType::Texture2D,
			EFormat::R32G32B32A32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp);
		auto textureB = driver->CreateTexture(&secondTexel, sizeof(secondTexel), glm::ivec3(1), 1, ETextureType::Texture2D,
			EFormat::R32G32B32A32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp);
		RHISceneViewSnapshot scene;
		scene.m_frameBindings = driver->CreateShaderBindings();
		scene.m_rhiLightsData = driver->CreateShaderBindings();
		auto node = TRefPtr<PostProcessNode>::Make();
		node->SetString("shader", smallShader);
		node->SetString("defines", "");
		node->SetRHIResource("color", output);
		node->SetRHIResource("sourceSampler", textureA);
		glm::vec4 tint(0.25f, 0.5f, 0.75f, 1);
		float gain = 0.5f;
		bool inverted = false;
		node->SetVec4("data.tint", tint);
		node->SetFloat("data.gain", gain);
		CaptureAttachments capture;

		const auto draw = [&](const char* label, RHITexturePtr target, RHISurfacePtr surface, glm::vec4 texel)
		{
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto graphics = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(graphics, true);
			commands->MemoryBarrier(graphics, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
			recordedColorCount = 0;
			node->Process(graph, upload, graphics, scene);
			const bool msaa = surface && surface->NeedsResolve();
			auto currentBindings = PostProcessNodeTestAccess::GetBindings(*node);
			Require(currentBindings.IsValid(), "post-process shader must be ready and create its bindings");
			const auto binding = currentBindings->GetOrAddShaderBinding("data");
			const auto reflectedSize = (std::max)(binding->GetLayout().m_size, binding->GetLayout().m_paddedSize);
			const bool validAttachments = recordedColorCount == 1 &&
				recordedColor.imageView == static_cast<VkImageView>(*(msaa ? surface->GetTarget() : target)->m_vulkan.m_imageView) &&
				recordedColor.resolveImageView == (msaa ? static_cast<VkImageView>(*target->m_vulkan.m_imageView) : VK_NULL_HANDLE) &&
				recordedColor.resolveMode == (msaa ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE);
			if (!validAttachments || binding->m_vulkan.m_valueBinding->Get().m_size < reflectedSize)
			{
				commands->EndCommandList(upload);
				commands->EndCommandList(graphics);
				upload->m_vulkan.m_commandBuffer->Reset();
				graphics->m_vulkan.m_commandBuffer->Reset();
				throw std::runtime_error(std::string(label) + ": wrong native attachment/resolve or undersized reflected UBO");
			}
			Require(node->GetDrawCallStats().m_numBatches == 1, "post-process must record one draw");
			auto readback = driver->CreateBuffer(Side * Side * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
			commands->ImageMemoryBarrier(graphics, target, EImageLayout::TransferSrcOptimal);
			commands->CopyImageToBuffer(graphics, target, readback);
			commands->MemoryBarrier(graphics, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			commands->EndCommandList(upload);
			commands->EndCommandList(graphics);
			auto ready = driver->CreateWaitSemaphore();
			auto uploaded = RHIFencePtr::Make();
			auto finished = RHIFencePtr::Make();
			Require(driver->SubmitCommandList(upload, uploaded, ready) && driver->SubmitCommandList(graphics, finished, nullptr, ready),
				"post-process upload and draw must submit");
			Require(finished->Wait(5000000000ull) == EFenceStatus::Finished && uploaded->Wait(5000000000ull) == EFenceStatus::Finished,
				"post-process readback must complete");
			const auto pixels = static_cast<const glm::vec4*>(readback->GetPointer());
			const auto expected = inverted ? glm::vec4(1) - (tint * gain + texel) : tint * gain + texel;
			for (uint32_t i = 0; i < Side * Side; ++i)
				for (uint32_t component = 0; component < 4; ++component)
					if (!std::isfinite(pixels[i][component]) || std::abs(pixels[i][component] - expected[component]) > 0.00001f)
						throw std::runtime_error(std::string(label) + ": GPU pixel does not match current parameters and texture");
			std::cout << "PostProcess " << label << ": native attachments and 64 pixels passed\n";
		};

		draw("first draw", output, {}, firstTexel);
		tint = glm::vec4(0.75f, 0.25f, 0.5f, 1);
		gain = 0.25f;
		node->SetVec4("data.tint", tint);
		node->SetFloat("data.gain", gain);
		draw("mutated parameters", output, {}, firstTexel);
		node->SetRHIResource("sourceSampler", textureB);
		draw("replaced bound sampler", output, {}, secondTexel);
		const auto bindings = PostProcessNodeTestAccess::GetBindings(*node);
		const auto descriptor = bindings->m_vulkan.m_descriptorSet;
		const auto revision = bindings->GetDescriptorRevision();
		draw("unchanged frame", output, {}, secondTexel);
		Require(PostProcessNodeTestAccess::GetBindings(*node) == bindings && bindings->m_vulkan.m_descriptorSet == descriptor &&
			bindings->GetDescriptorRevision() == revision, "unchanged post-process inputs must not rebuild descriptors");
		node->SetRHIResource("color", single);
		draw("bound 1x surface", output, single, secondTexel);
		node->SetRHIResource("color", multisampled);
		draw("bound 2x surface", multisampled->GetResolved(), multisampled, secondTexel);
		graph->SetSurface("LateOutput", multisampled);
		graph->SetRenderTarget("LateOutput", multisampled->GetResolved());
		node->SetRHIResource_Unresolved("color", "LateOutput");
		draw("late 2x surface", multisampled->GetResolved(), multisampled, secondTexel);
		graph->SetSurface("LateOutput", single);
		graph->SetRenderTarget("LateOutput", output);
		draw("late 1x surface replacement", output, single, secondTexel);
		graph->SetRenderTarget("PlainOutput", output);
		node->SetRHIResource_Unresolved("color", "PlainOutput");
		draw("late plain target", output, {}, secondTexel);
		graph->SetSampler("LateSampler", textureA);
		node->SetRHIResource_Unresolved("sourceSampler", "LateSampler");
		draw("late sampled texture", output, {}, firstTexel);
		graph->SetSampler("LateSampler", textureB);
		draw("replaced late sampler", output, {}, secondTexel);
		node->SetRHIResource("sourceSampler", textureA);
		draw("bound overrides late sampler", output, {}, firstTexel);
		node->SetRHIResource("sourceSampler", multisampled);
		draw("sample resolved 2x surface", output, {}, tint * gain + secondTexel);
		graph->SetSurface("SurfaceSampler", multisampled);
		node->SetRHIResource_Unresolved("sourceSampler", "SurfaceSampler");
		draw("sample late 2x surface", output, {}, tint * gain + secondTexel);

		for (const auto format : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT })
		{
			auto setup = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(setup, true);
			auto depth = driver->CreateRenderTarget(setup, glm::ivec2(Side), 1, format,
				ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit);
			commands->ImageMemoryBarrier(setup, depth, EImageLayout::TransferDstOptimal);
			commands->ClearDepthStencil(setup, depth, 0.375f, 0);
			commands->ImageMemoryBarrier(setup, depth, EImageLayout::ShaderReadOnlyOptimal);
			commands->EndCommandList(setup);
			auto initialized = RHIFencePtr::Make();
			Require(driver->SubmitCommandList(setup, initialized) && initialized->Wait(5000000000ull) == EFenceStatus::Finished,
				"sampled depth fixture must initialize");
			node->SetRHIResource("sourceSampler", depth);
			draw("bound sampled depth", output, {}, glm::vec4(0.375f, 0, 0, 1));
			graph->SetRenderTarget("SceneDepth", depth);
			node->SetRHIResource_Unresolved("sourceSampler", "SceneDepth");
			draw("late sampled depth", output, {}, glm::vec4(0.375f, 0, 0, 1));
			const auto sampler = PostProcessNodeTestAccess::GetBindings(*node)->GetOrAddShaderBinding("sourceSampler");
			const RHITexturePtr expectedDepth = depth->GetDepthAspect() ? depth->GetDepthAspect() : RHITexturePtr(depth);
			Require(sampler->GetTextureBinding() == expectedDepth &&
				expectedDepth->m_vulkan.m_imageView->m_subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT,
				"depth-stencil sampling must select the actual depth-only view");
		}
		node->SetRHIResource("sourceSampler", textureA);
		node->SetString("shader", largeShader);
		draw("large reflected block and changed shader", output, {}, firstTexel);
		const auto largeBinding = PostProcessNodeTestAccess::GetBindings(*node)->GetOrAddShaderBinding("data");
		Require(largeBinding->GetLayout().m_binding == 3 && largeBinding->GetLayout().m_size > 512,
			"large-block test must use the new shader with a nonzero uniform binding");
		inverted = true;
		node->SetString("defines", "INVERT");
		draw("changed shader defines", output, {}, firstTexel);
		inverted = false;
		node->SetString("defines", "");
		draw("restored shader defines", output, {}, firstTexel);
		node->Clear();
		draw("recreated material", output, {}, firstTexel);
	}
}

namespace Sailor::Tests
{
	void RunFrameGraphNodeCommandTests(const std::filesystem::path& workspace)
	{
		const auto smallShader = WriteShader(workspace, false);
		const auto largeShader = WriteShader(workspace, true);
		const auto graphId = WriteGraph(workspace);
		const auto mrtShader = WriteMrtShader(workspace);
		App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
		auto task = Tasks::CreateTaskWithResult<std::string>("Post-process attachment and parameter contracts", [&]() -> std::string
			{
				try
				{
					TestSceneMrt(mrtShader, true, false, false, true);
					for (bool late : { false, true })
						for (bool colorSurface : { false, true })
							for (bool motionSurface : { false, true }) TestSceneMrt(mrtShader, colorSurface, motionSurface, late);
					TestImportedBindings(graphId);
					if (VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() == VK_SAMPLE_COUNT_2_BIT)
						TestPostProcess(smallShader, largeShader);
					return {};
				}
				catch (const std::exception& error) { return error.what(); }
			}, EThreadType::Render);
		task->Run();
		task->Wait();
		if (!task->GetResult().empty()) throw std::runtime_error(task->GetResult());
	}
}
