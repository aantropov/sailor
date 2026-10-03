#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Components/CameraComponent.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "FrameGraph/AtmosphericFogNode.h"
#include "FrameGraph/BlitNode.h"
#include "FrameGraph/ClearNode.h"
#include "FrameGraph/DepthHighZNode.h"
#include "FrameGraph/DepthPrepassNode.h"
#include "FrameGraph/DebugDrawNode.h"
#include "FrameGraph/LinearizeDepthNode.h"
#include "FrameGraph/LightCullingNode.h"
#include "FrameGraph/PostProcessNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "FrameGraph/RenderImGuiNode.h"
#include "FrameGraph/RenderSceneNode.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Cubemap.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/VertexDescription.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <latch>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace Sailor::Tests
{
	void RequireRejectedNativeSubmission(RHICommandListPtr command);
	void RequireRejectedGraphicsSubmission(const std::function<bool()>& submit);
}

namespace Sailor
{
	class FrameGraphImporterTestAccess
	{
	public:
		static void ReleaseInstance(FrameGraphImporter& importer, FrameGraphPtr instance)
		{
			instance.DestroyObject(importer.m_allocator);
		}
	};
}

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
		static RHIShaderBindingSetPtr GetBindings(const PostProcessNode& node, const RHISceneViewSnapshot& scene)
		{
			return scene.m_submissionContext->GetOrAddFrameGraphResources<PostProcessNode::SubmissionResources>(
				&node, scene.m_cameraIndex, 0)->m_shaderBindings;
		}
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
			{
				vertices[i].m_position = glm::vec3(i % 2 ? 1 : -1, i / 2 ? 1 : -1, 0);
				vertices[i].m_texcoord = glm::vec2(i % 2, i / 2);
			}
			const uint32_t indices[] = { 0, 1, 2, 2, 1, 3 };
			m_postEffectPlane->m_vertexBuffer = driver->CreateBuffer(sizeof(vertices), EBufferUsageBit::VertexBuffer_Bit, HostMemory);
			m_postEffectPlane->m_indexBuffer = driver->CreateBuffer(sizeof(indices), EBufferUsageBit::IndexBuffer_Bit, HostMemory);
			std::memcpy(m_postEffectPlane->m_vertexBuffer->GetPointer(), vertices.data(), sizeof(vertices));
			std::memcpy(m_postEffectPlane->m_indexBuffer->GetPointer(), indices, sizeof(indices));
		}
	};

	class ShadowCacheProbe : public ShadowPrepassNode
	{
	public:
		size_t GetCachedMaterialCount() const { return m_customShadowMaterials.Num(); }

		TRefPtr<SubmissionResources> GetResources(const RHISceneViewSnapshot& scene)
		{
			return scene.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(this, scene.m_cameraIndex, 0);
		}

		RHIMaterialPtr GetCascadeMaterial(const RHIRenderSubmissionContextPtr& context, uint32_t cascade) const
		{
			const auto resources = context->GetOrAddFrameGraphResources<SubmissionResources>(this, 0, 0);
			const auto& groups = resources->m_activeShadowViews[cascade]->m_packet.GetGroups();
			Require(groups.Num() == 1, "each cache fixture cascade must contain one real draw group");
			return groups[0].m_batch.m_material;
		}
	};

	class CountedMaterialBindings : public RHIShaderBindingSet
	{
	public:
		explicit CountedMaterialBindings(TSharedPtr<uint32_t> live) : m_live(std::move(live)) { ++*m_live; }
		~CountedMaterialBindings() override { --*m_live; }

	private:
		TSharedPtr<uint32_t> m_live;
	};

	PFN_vkCmdBeginRenderingKHR originalBeginRendering = nullptr;
	VkRenderingAttachmentInfo recordedColor{};
	VkRenderingAttachmentInfo recordedMotion{};
	VkRenderingAttachmentInfo recordedDepth{};
	VkRenderingFlags recordedRenderingFlags = 0;
	uint32_t recordedColorCount = 0;

	VKAPI_ATTR void VKAPI_CALL CaptureRendering(VkCommandBuffer command, const VkRenderingInfo* info)
	{
		recordedColorCount = info->colorAttachmentCount;
		recordedDepth = info->pDepthAttachment ? *info->pDepthAttachment : VkRenderingAttachmentInfo{};
		recordedRenderingFlags = info->flags;
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

	class PassCommandRecorder : public VulkanGraphicsDriver
	{
	public:
		void BeginDebugRegion(RHICommandListPtr, const std::string& title, const glm::vec4&) override
		{
			m_regions.push_back(title);
			++m_regionStarts;
		}

		void EndDebugRegion(RHICommandListPtr) override
		{
			if (m_regions.empty()) m_unbalanced = true;
			else m_regions.pop_back();
		}

		void RenderSecondaryCommandBuffers(RHICommandListPtr, TVector<RHICommandListPtr> secondary,
			const TVector<RHITexturePtr>& color, RHITexturePtr depth, glm::ivec4, glm::ivec2,
			bool, glm::vec4, float, bool, bool) override
		{
			Require(m_regions.size() == 2 && m_regions.back() == RenderImGuiNode::GetName(),
				"ImGui commands must be nested inside their own debug region");
			Require(secondary.Num() == 1 && color.Num() == 1 && color[0] && depth,
				"ImGui must record one complete secondary with both attachments");
			m_secondary = secondary[0];
			++m_draws;
		}

		void ImageMemoryBarrier(RHICommandListPtr, RHITexturePtr, EImageLayout) override {}
		void BindMaterial(RHICommandListPtr, RHIMaterialPtr) override {}
		void SetViewport(RHICommandListPtr, float, float, float, float, glm::vec2, glm::vec2, float, float) override {}
		void BindVertexBuffer(RHICommandListPtr, RHIBufferPtr, uint32_t) override {}
		void BindIndexBuffer(RHICommandListPtr, RHIBufferPtr, uint32_t, bool) override {}

		void BeginRenderPass(RHICommandListPtr, const TVector<RHITexturePtr>& colors, RHITexturePtr depth,
			glm::ivec4, glm::ivec2, bool, glm::vec4, float, bool, bool) override
		{
			Require(m_regions.size() == 2 && m_regions.back() == LinearizeDepthNode::GetName() && !m_inRenderPass,
				"linear depth must begin its render pass inside the matching debug region");
			Require(colors.Num() == 1 && colors[0] && !depth, "linear depth must use one color target and sample depth separately");
			m_inRenderPass = true;
			++m_renderPasses;
		}

		void EndRenderPass(RHICommandListPtr) override
		{
			Require(m_inRenderPass, "every render-pass end must have a matching begin");
			m_inRenderPass = false;
		}

		bool BindShaderBindings(RHICommandListPtr, RHIMaterialPtr material, const TVector<RHIShaderBindingSetPtr>& bindings) override
		{
			Require(m_inRenderPass && material && bindings.Num() == 2 && bindings[0] && bindings[1],
				"linear depth must bind its frame and sampled-depth sets inside the render pass");
			return m_acceptBindings;
		}

		void DrawIndexed(RHICommandListPtr, uint32_t indices, uint32_t instances, uint32_t, uint32_t, uint32_t) override
		{
			Require(m_inRenderPass && m_acceptBindings && indices == 6 && instances == 1,
				"linear depth must record one fullscreen draw only after successful binding");
			++m_draws;
		}

		std::vector<std::string> m_regions;
		RHICommandListPtr m_secondary;
		uint32_t m_draws = 0;
		uint32_t m_regionStarts = 0;
		uint32_t m_renderPasses = 0;
		bool m_inRenderPass = false;
		bool m_acceptBindings = true;
		bool m_unbalanced = false;
	};

	struct ScopedPassRecorder
	{
		ScopedPassRecorder()
		{
			auto recorder = TUniquePtr<PassCommandRecorder>::Make();
			m_commands = recorder.GetRawPtr();
			m_driver = std::move(Renderer::GetDriver());
			Renderer::GetDriver() = std::move(recorder);
		}

		~ScopedPassRecorder() { Renderer::GetDriver() = std::move(m_driver); }

		PassCommandRecorder* m_commands = nullptr;
		TUniquePtr<IGraphicsDriver> m_driver;
	};

	class ObservedImGuiTask : public Tasks::Task<RHICommandListPtr>
	{
	public:
		ObservedImGuiTask(Function function, std::latch& observed, std::latch& resume) :
			Tasks::Task<RHICommandListPtr>("Delayed ImGui producer", std::move(function), EThreadType::RHI),
			m_observed(observed), m_resume(resume)
		{
		}

		static TSharedPtr<ObservedImGuiTask> Create(Function function, std::latch& observed, std::latch& resume)
		{
			auto task = TSharedPtr<ObservedImGuiTask>::Make(std::move(function), observed, resume);
			task->m_self = task;
			return task;
		}

		bool IsFinished() const override
		{
			const bool finished = Tasks::ITask::IsFinished();
			if (!finished && m_pScheduler->IsRendererThread() && ++m_renderChecks == 2)
			{
				m_observed.count_down();
				m_resume.wait();
			}
			return finished;
		}

	private:
		std::latch& m_observed;
		std::latch& m_resume;
		mutable uint32_t m_renderChecks = 0;
	};

	void TestDelayedImGuiProducer()
	{
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		for (bool empty : { false, true })
		{
			ScopedPassRecorder recorder;
			auto graph = RHIFrameGraphPtr::Make();
			auto node = TRefPtr<RenderImGuiNode>::Make();
			auto color = RHIRenderTargetPtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false);
			auto depth = RHIRenderTargetPtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false);
			node->SetRHIResource("color", color);
			node->SetRHIResource("depthStencil", depth);
			auto secondary = RHICommandListPtr::Make(ECommandListQueue::Graphics);
			secondary->RecordDrawCallStats(3);
			secondary->RecordDrawCallStats(5);
			std::latch producerStarted(1), releaseProducer(1), observed(1), resume(1);
			uint32_t producerCalls = 0;
			bool precedingPassRecorded = false;
			auto producer = ObservedImGuiTask::Create([&]()
				{
					++producerCalls;
					producerStarted.count_down();
					releaseProducer.wait();
					return empty ? RHICommandListPtr{} : secondary;
				}, observed, resume);
			producer->Run();
			producerStarted.wait();
			RHISceneViewSnapshot scene;
			scene.m_drawImGui = producer;
			auto record = Tasks::CreateTaskWithResult<std::string>("Record delayed ImGui pass", [&]() -> std::string
				{
					try
					{
						precedingPassRecorded = true;
						recorder.m_commands->BeginDebugRegion({}, "Frame", {});
						node->Process(graph, {}, {}, scene);
						Require(recorder.m_commands->m_regions == std::vector<std::string>{ "Frame" },
							"ImGui must leave its enclosing debug region open");
						recorder.m_commands->EndDebugRegion({});
						return {};
					}
					catch (const std::exception& error) { return error.what(); }
				}, EThreadType::Render);
			record->Run();
			observed.wait();
			// Inspect from another thread while the producer is held. Task::Wait
			// rechecks completion under its existing sync lock; a polling loop does not.
			auto& block = scheduler->GetTaskSyncBlock(*producer);
			const bool polling = block.m_mutex.try_lock();
			if (polling) block.m_mutex.unlock();
			const bool reachedPass = precedingPassRecorded && recorder.m_commands->m_draws == 0;
			resume.count_down();
			releaseProducer.count_down();
			record->Wait();
			producer->Wait();
			Require(!polling, "ImGui must use the task completion wait instead of polling the producer");
			Require(reachedPass, "earlier recording must proceed before the ImGui producer completes");
			Require(record->GetResult().empty(), record->GetResult().c_str());
			Require(producerCalls == 1 && recorder.m_commands->m_draws == (empty ? 0u : 1u),
				"an already-running ImGui producer must finish once and draw only a nonempty result");
			Require(node->GetDrawCallStats().m_numBatches == (empty ? 0u : 2u) &&
				node->GetDrawCallStats().m_numInstances == (empty ? 0u : 8u),
				"ImGui statistics must come from the completed secondary");
			Require(!recorder.m_commands->m_unbalanced && recorder.m_commands->m_regions.empty(),
				"ready and empty ImGui results must balance debug regions");
			if (!empty) Require(recorder.m_commands->m_secondary == secondary,
				"ImGui must consume the exact RHIPtr produced on the RHI queue");
		}
		std::cout << "ImGui delayed producer: task completion wait, pass overlap, single execution and empty result passed\n";
	}

	void TestImGuiSkippedAttachments()
	{
		ScopedPassRecorder recorder;
		auto graph = RHIFrameGraphPtr::Make();
		auto node = TRefPtr<RenderImGuiNode>::Make();
		auto attachment = RHIRenderTargetPtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false);
		RHISceneViewSnapshot scene;
		scene.m_drawImGui = Tasks::CreateTaskWithResult<RHICommandListPtr>("Unused ImGui producer", []() { return RHICommandListPtr{}; });
		for (uint32_t mask = 0; mask < 3; ++mask)
		{
			node->SetRHIResource("color", mask & 1 ? attachment : RHIRenderTargetPtr{});
			node->SetRHIResource("depthStencil", mask & 2 ? attachment : RHIRenderTargetPtr{});
			recorder.m_commands->BeginDebugRegion({}, "Frame", {});
			node->Process(graph, {}, {}, scene);
			Require(recorder.m_commands->m_regions == std::vector<std::string>{ "Frame" },
				"a skipped ImGui pass must not leave an extra debug region open");
			recorder.m_commands->EndDebugRegion({});
		}
		Require(!scene.m_drawImGui->IsStarted() && recorder.m_commands->m_draws == 0 &&
			node->GetDrawCallStats().m_numBatches == 0 && !recorder.m_commands->m_unbalanced,
			"missing ImGui attachments must skip recording without starting or waiting for the producer");
		std::cout << "ImGui missing attachments: no producer wait or unmatched debug region passed\n";
	}

	class LinearizeDepthProbe : public LinearizeDepthNode
	{
	public:
		using LinearizeDepthNode::m_pLinearizeDepthShader;

		LinearizeDepthProbe(ShaderSetPtr shader, RHITexturePtr depth)
		{
			m_pLinearizeDepthShader = shader;
			m_boundDepthAttachment = depth;
			m_linearizeDepth = RHIShaderBindingSetPtr::Make();
			m_postEffectMaterial = RHIMaterialPtr::Make(RenderState{}, shader->GetVertexShaderRHI(), shader->GetFragmentShaderRHI());
		}
	};

	void TestLinearizeDepthRegions(ShaderSetPtr shader)
	{
		// Only control flow is intercepted here. Native pixel tests below exercise
		// resource creation, binding and depth reconstruction through the real driver.
		auto graph = TRefPtr<TestGraph>::Make();
		auto depth = RHIRenderTargetPtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false);
		auto target = RHIRenderTargetPtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false);
		auto pendingShader = ShaderSetPtr::Make(Memory::ObjectAllocatorPtr::Make(),
			FileId::CreateNewFileId(), TVector<std::string>{});
		Require(shader->IsReady() && !pendingShader->IsReady(), "the fixture must distinguish ready and pending shaders");
		RHISceneViewSnapshot scene;
		scene.m_frameBindings = RHIShaderBindingSetPtr::Make();
		ScopedPassRecorder recorder;
		for (bool named : { false, true })
		{
			auto node = TRefPtr<LinearizeDepthProbe>::Make(shader, depth);
			if (named)
			{
				node->SetRHIResource_Unresolved("depthStencil", "Depth");
				node->SetRHIResource_Unresolved("target", "LinearDepth");
			}
			struct Case { bool depth, target, ready, bind; };
			for (const auto test : { Case{ true, true, true, true }, Case{ false, true, true, true },
				Case{ true, false, true, true }, Case{ false, false, true, true },
				Case{ true, true, false, true }, Case{ true, true, true, false }, Case{ true, true, true, true } })
			{
				if (named)
				{
					graph->SetRenderTarget("Depth", test.depth ? depth : RHIRenderTargetPtr{});
					graph->SetRenderTarget("LinearDepth", test.target ? target : RHIRenderTargetPtr{});
				}
				else
				{
					node->SetRHIResource("depthStencil", test.depth ? depth : RHIRenderTargetPtr{});
					node->SetRHIResource("target", test.target ? target : RHIRenderTargetPtr{});
				}
				node->m_pLinearizeDepthShader = test.ready ? shader : pendingShader;
				auto& commands = *recorder.m_commands;
				commands.m_acceptBindings = test.bind;
				commands.BeginDebugRegion({}, "Frame", {});
				const auto regionsBefore = commands.m_regionStarts;
				const auto passesBefore = commands.m_renderPasses;
				const auto drawsBefore = commands.m_draws;
				node->Process(graph, {}, {}, scene);
				const uint32_t passes = test.depth && test.target && test.ready ? 1u : 0u;
				const uint32_t draws = passes && test.bind ? 1u : 0u;
				Require(commands.m_regions == std::vector<std::string>{ "Frame" } && !commands.m_inRenderPass,
					"linear depth must close its render/debug regions on ready and skipped paths");
				Require(commands.m_regionStarts == regionsBefore + passes && commands.m_renderPasses == passesBefore + passes &&
					commands.m_draws == drawsBefore + draws && node->GetDrawCallStats().m_numBatches == draws,
					"pending/missing inputs must emit no pass; binding refusal must emit no draw or stale statistics");
				commands.EndDebugRegion({});
				Require(commands.m_regions.empty() && !commands.m_unbalanced, "linear depth must preserve the enclosing graph label");
			}
		}
		std::cout << "LinearizeDepth debug regions: ready, pending shader, missing attachments, binding refusal and retry passed\n";
	}

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

	std::array<ShaderSetPtr, 4> WriteDepthReadbackShader(const std::filesystem::path& workspace)
	{
		const auto path = workspace / "Content" / "ClearDepthReadback.shader";
		YAML::Node shader;
		shader["defines"].push_back("MSAA");
		shader["defines"].push_back("STENCIL");
		shader["glslCommon"] = "#version 450\n";
		shader["glslCompute"] = R"glsl(
layout(local_size_x = 8, local_size_y = 8) in;
#ifdef MSAA
layout(set = 0, binding = 0) uniform sampler2DMS depthSampler;
#ifdef STENCIL
layout(set = 0, binding = 1) uniform usampler2DMS stencilSampler;
#endif
#else
layout(set = 0, binding = 0) uniform sampler2D depthSampler;
#ifdef STENCIL
layout(set = 0, binding = 1) uniform usampler2D stencilSampler;
#endif
#endif
layout(set = 0, binding = 2, std430) writeonly buffer OutputValues { vec2 values[]; } outputValues;
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	int samples = 1;
#ifdef MSAA
	samples = textureSamples(depthSampler);
	ivec2 size = textureSize(depthSampler);
#else
	ivec2 size = textureSize(depthSampler, 0);
#endif
	if (any(greaterThanEqual(pixel, size))) return;
	for (int sampleIndex = 0; sampleIndex < samples; ++sampleIndex) {
		float depth = texelFetch(depthSampler, pixel, sampleIndex).r;
		float stencil = 0;
#ifdef STENCIL
		stencil = float(texelFetch(stencilSampler, pixel, sampleIndex).r);
#endif
		outputValues.values[(pixel.y * size.x + pixel.x) * samples + sampleIndex] = vec2(depth, stencil);
	}
}
)glsl";
		std::ofstream output(path);
		output << shader;
		output.close();
		Require(static_cast<bool>(output), "depth readback shader fixture must be written");
		const auto id = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		std::array<ShaderSetPtr, 4> result;
		for (uint32_t variant = 0; variant < result.size(); ++variant)
		{
			TVector<std::string> defines;
			if (variant & 1u) defines.Add("MSAA");
			if (variant & 2u) defines.Add("STENCIL");
			Require(App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(id, result[variant], defines) && result[variant]->IsReady(),
				"all depth/stencil readback permutations must compile");
		}
		return result;
	}

	ShaderSetPtr WriteDepthPatternShader(const std::filesystem::path& workspace, EFormat format)
	{
		const auto path = workspace / "Content" / ("DepthPattern" + std::to_string(uint32_t(format)) + ".shader");
		YAML::Node shader;
		shader["glslCommon"] = "#version 450\n";
		shader["depthStencilAttachment"] = format == EFormat::D32_SFLOAT ? "D32_SFLOAT" : "D32_SFLOAT_S8_UINT";
		shader["glslVertex"] = "layout(location = 0) in vec3 position; void main() { gl_Position = vec4(position, 1); }";
		shader["glslFragment"] = R"glsl(
layout(push_constant) uniform Pattern { uint sampleIndex; uint frame; } pattern;
void main() {
	uvec2 pixel = uvec2(gl_FragCoord.xy);
	if (pattern.sampleIndex > 0 && (pixel.x + pixel.y + pattern.frame) % 5 == 0) discard;
	gl_SampleMask[0] = 1 << pattern.sampleIndex;
	gl_FragDepth = float(1 + (pixel.x * 3 + pixel.y * 5 + pattern.frame * 7) % 13) / 16.0 - float(pattern.sampleIndex) / 32.0;
}
)glsl";
		std::ofstream output(path);
		output << shader;
		output.close();
		Require(static_cast<bool>(output), "depth pattern shader must be written");
		const auto id = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		ShaderSetPtr result;
		Require(App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(id, result) && result->IsReady(),
			"depth pattern shader must compile before recording");
		return result;
	}

	ShaderSetPtr WriteDebugDrawShader(const std::filesystem::path& workspace, EFormat format)
	{
		const auto path = workspace / "Content" / ("DebugDrawTest" + std::to_string(uint32_t(format)) + ".shader");
		YAML::Node shader;
		shader["glslCommon"] = "#version 450\n";
		shader["colorAttachments"].push_back("R32G32B32A32_SFLOAT");
		shader["depthStencilAttachment"] = format == EFormat::D32_SFLOAT ? "D32_SFLOAT" : "D32_SFLOAT_S8_UINT";
		shader["glslVertex"] = R"glsl(
layout(location = 0) in vec3 position;
layout(push_constant) uniform Camera { mat4 viewProjection; } camera;
void main() { gl_Position = camera.viewProjection * vec4(position, 1); }
)glsl";
		shader["glslFragment"] = R"glsl(
layout(location = 0) out vec4 outColor;
void main() {
	if (gl_FragCoord.x >= 4) discard;
	outColor = vec4(0.75, 0.5, 0.25, 1);
}
)glsl";
		std::ofstream output(path);
		output << shader;
		output.close();
		Require(static_cast<bool>(output), "debug draw shader fixture must be written");
		const auto id = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		ShaderSetPtr result;
		Require(App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(id, result) && result->IsReady(),
			"debug draw shader must compile before secondary recording");
		return result;
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

	std::array<FileId, 2> WriteImportedGraph(const std::filesystem::path& workspace)
	{
		const auto texturePath = workspace / "Content" / "GraphSample.tga";
		const uint8_t tga[] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 32, 8, 0, 0, 255, 255 };
		std::ofstream textureFile(texturePath, std::ios::binary);
		textureFile.write(reinterpret_cast<const char*>(tga), sizeof(tga));
		textureFile.close();
		Require(static_cast<bool>(textureFile), "the one-pixel red TGA must be written");
		auto registry = App::GetSubmodule<AssetRegistry>();
		const auto textureId = registry->GetOrLoadFile(texturePath.string());
		TexturePtr texture;
		Require(App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(textureId, texture) && texture,
			"the static texture must load before recording the imported graph");

		const auto shaderPath = workspace / "Content" / "ImportedBindings.shader";
		YAML::Node shader;
		shader["glslCommon"] = "#version 450\n";
		shader["colorAttachments"].push_back("R32G32B32A32_SFLOAT");
		shader["glslVertex"] = "layout(location = 0) in vec3 position; void main() { gl_Position = vec4(position, 1); }";
		shader["glslFragment"] = R"glsl(
layout(set = 1, binding = 0) uniform sampler2D sourceSampler;
layout(set = 1, binding = 1) uniform sampler2D externalSampler;
layout(location = 0) out vec4 outColor;
void main() {
	outColor = texelFetch(sourceSampler, ivec2(0), 0) * 0.25 + texelFetch(externalSampler, ivec2(0), 0);
}
)glsl";
		std::ofstream shaderFile(shaderPath);
		shaderFile << shader;
		shaderFile.close();
		Require(static_cast<bool>(shaderFile), "the imported graph shader must be written");
		ShaderSetPtr compiled;
		Require(App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(registry->GetOrLoadFile(shaderPath.string()), compiled) &&
			compiled && compiled->IsReady(), "the imported graph shader must compile before recording");

		auto graph = YAML::Load(R"yaml(
samplers:
  - {name: ById}
  - {name: ByPath, path: GraphSample.tga}
  - {name: Both, path: DoesNotExist.tga}
renderTargets:
  - {name: Main, width: 8, height: 8, format: R32G32B32A32_SFLOAT, bIsSurface: true}
frame:
  - name: Clear
    renderTargets: [{target: Main}]
    vec4: [{clearColor: [0, 0, 0, 0]}]
  - name: PostProcess
    tag: Composite
    string: [{shader: ImportedBindings.shader}]
    renderTargets: [{color: Main}, {sourceSampler: ById}, {externalSampler: DynamicInput}]
)yaml");
		graph["samplers"][0]["fileId"] = textureId.ToString();
		graph["samplers"][2]["fileId"] = textureId.ToString();
		const auto graphPath = workspace / "Content" / "ImportedBindings.renderer";
		std::ofstream graphFile(graphPath);
		graphFile << graph;
		graphFile.close();
		Require(static_cast<bool>(graphFile), "the imported graph must be written");

		const auto invalidPath = workspace / "Content" / "AmbiguousBindings.renderer";
		std::ofstream invalidFile(invalidPath);
		invalidFile << "renderTargets: [{name: Shared, width: 8}, {name: Shared, width: 16}]\n";
		invalidFile.close();
		Require(static_cast<bool>(invalidFile), "the ambiguous graph must be written");
		return { registry->GetOrLoadFile(graphPath.string()), registry->GetOrLoadFile(invalidPath.string()) };
	}

	void TestGraphLoadFailures(const std::filesystem::path& workspace, FileId validId)
	{
		auto importer = App::GetSubmodule<FrameGraphImporter>();
		auto registry = App::GetSubmodule<AssetRegistry>();
		FrameGraphPtr previous;
		Require(importer->LoadFrameGraph_Immediate(validId, previous) && previous,
			"the valid graph must be available before testing failed replacements");
		auto valid = YAML::LoadFile(registry->GetAssetInfoPtr(validId)->GetAssetFilepath());
		valid["frame"].push_back(YAML::Load("{name: ExperimentalParticles, tag: Particles}"));

		for (const std::string failure : { "missing-file", "unknown-node", "texture-decode", "sampler-path", "sampler-id",
			"zero-width", "invalid-divisor", "invalid-mips" })
		{
			auto description = YAML::Clone(valid);
			const auto path = workspace / "Content" / (failure + ".renderer");
			const auto unavailable = workspace / "Content" / (failure + ".unavailable");
			const auto brokenTexture = workspace / "Content" / "BrokenGraphSample.tga";
			if (failure == "unknown-node")
			{
				description["frame"][1]["name"] = "UnregisteredTestNode";
			}
			else if (failure == "texture-decode")
			{
				std::ofstream output(brokenTexture, std::ios::binary);
				output << "not a TGA image";
				output.close();
				Require(static_cast<bool>(output), "the invalid texture fixture must be written");
				description["samplers"][0]["fileId"] = registry->GetOrLoadFile(brokenTexture.string()).ToString();
			}
			else if (failure == "sampler-path")
			{
				description["samplers"][0].remove("fileId");
				description["samplers"][0]["path"] = "MissingGraphSample.tga";
			}
			else if (failure == "sampler-id")
			{
				description["samplers"][0]["fileId"] = FileId::CreateNewFileId().ToString();
				description["samplers"][0]["path"] = "GraphSample.tga";
			}
			else if (failure == "zero-width") description["renderTargets"][0]["width"] = 0;
			else if (failure == "invalid-divisor") description["renderTargets"][0]["height"] = "RenderHeight/0";
			else if (failure == "invalid-mips") description["renderTargets"][0]["maxMipLevel"] = -1;
			auto write = [&](const YAML::Node& data)
				{
					std::ofstream output(path);
					output << data;
					output.close();
					Require(static_cast<bool>(output), "the failed-load graph fixture must be written");
				};
			write(description);
			const auto id = registry->GetOrLoadFile(path.string());
			if (failure == "missing-file") std::filesystem::rename(path, unavailable);
			auto parsed = importer->LoadFrameGraphAsset(id);
			const bool invalidDescription = failure == "missing-file" || failure == "zero-width" ||
				failure == "invalid-divisor" || failure == "invalid-mips";
			Require(invalidDescription ? !parsed : static_cast<bool>(parsed),
				"invalid descriptions must fail to load; invalid build dependencies must still parse");
			for (uint32_t attempt = 0; attempt < 2; ++attempt)
			{
				FrameGraphPtr rejected;
				Require(!importer->LoadFrameGraph_Immediate(id, rejected) && !rejected &&
					!importer->Instantiate_Immediate(id, rejected) && !rejected,
					"a failed build must not return or cache a partial graph");
				auto retained = previous;
				Require(!importer->LoadFrameGraph_Immediate(id, retained) && retained == previous &&
					!importer->Instantiate_Immediate(id, retained) && retained == previous,
					"a failed build must leave the caller's existing graph unchanged");
			}
			if (failure == "missing-file")
			{
				std::filesystem::rename(unavailable, path);
			}
			else if (failure == "texture-decode")
			{
				std::filesystem::copy_file(workspace / "Content" / "GraphSample.tga", brokenTexture,
					std::filesystem::copy_options::overwrite_existing);
			}
			else
			{
				write(valid);
			}
			FrameGraphPtr repaired, instance, cached;
			Require(importer->LoadFrameGraph_Immediate(id, repaired) && repaired &&
				importer->Instantiate_Immediate(id, instance) && instance &&
				importer->LoadFrameGraph_Immediate(id, cached) && cached == repaired,
				"repairing the file or its dependency must allow retry and normal cache reuse");
			Require(instance->GetRHI() != repaired->GetRHI(), "instantiation must still create an independent graph");
			for (auto graph : { repaired->GetRHI(), instance->GetRHI() })
			{
				Require(graph->GetGraph().Num() == 3 && graph->GetGraph()[0]->GetTag() == "Clear" &&
					graph->GetGraph()[1]->GetTag() == "Composite" && graph->GetGraph()[2]->GetTag() == "Particles" &&
					graph->GetSampler("ById") && graph->GetSampler("ByPath") && graph->GetSampler("Both"),
					"a repaired graph must retain every ordered pass and static sampler, including ExperimentalParticles");
			}
			FrameGraphImporterTestAccess::ReleaseInstance(*importer, instance);
			std::cout << "FrameGraph load failure: " << failure << ", rejection, retained output and repair retry passed\n";
		}
	}

	void TestRelativeAttachmentDimensions(const std::filesystem::path& workspace)
	{
		const glm::ivec2 viewport = App::GetMainWindow()->GetRenderArea();
		const auto render = Settings::ResolveRenderDimensions(
			static_cast<uint32_t>((std::max)(viewport.x, 1)), static_cast<uint32_t>((std::max)(viewport.y, 1)),
			App::GetActiveGraphicsSettings().m_resolutionFactor);
		const std::array<std::pair<const char*, uint32_t>, 4> dimensions = {{
			{ "RenderWidth", render.m_width }, { "RenderHeight", render.m_height },
			{ "ViewportWidth", static_cast<uint32_t>((std::max)(viewport.x, 1)) },
			{ "ViewportHeight", static_cast<uint32_t>((std::max)(viewport.y, 1)) } }};
		for (const auto& [name, size] : dimensions)
		{
			Require(FrameGraphAsset::RenderTarget::ParseUintValue(name) == size,
				"an exact dimension variable must select the corresponding viewport/render axis");
			for (const double divisor : { 0.5, 2.0, 2.5, 1000000.0 })
			{
				const auto expression = std::string(" ") + name + " / " + std::to_string(divisor) + " ";
				const auto expected = (std::max)(1u, static_cast<uint32_t>(size / divisor));
				Require(FrameGraphAsset::RenderTarget::ParseUintValue(expression) == expected,
					"relative dimensions must support positive fractions, whitespace, truncation and minimum-one extents");
			}
			bool rejected = false;
			try { FrameGraphAsset::RenderTarget::ParseUintValue(std::string(name) + "/1e-100"); }
			catch (const YAML::Exception&) { rejected = true; }
			Require(rejected, "a scaled dimension must not overflow the image extent");
		}

		const auto path = workspace / "Content" / "RelativeDimensions.renderer";
		std::ofstream output(path);
		output << R"yaml(
renderTargets:
  - {name: Render, width: RenderWidth/4, height: RenderHeight/4, format: R32G32B32A32_SFLOAT, bGenerateMips: true, maxMipLevel: 2}
  - {name: Viewport, width: ViewportWidth/4, height: ViewportHeight/4, format: R32G32B32A32_SFLOAT, bIsSurface: true}
)yaml";
		output.close();
		Require(static_cast<bool>(output), "the relative-dimension fixture must be written");
		const auto id = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		auto importer = App::GetSubmodule<FrameGraphImporter>();
		FrameGraphPtr instance;
		Require(importer->Instantiate_Immediate(id, instance), "a valid relative-dimension graph must instantiate");
		auto graph = instance->GetRHI();
		const glm::ivec2 renderSize((std::max)(1u, render.m_width / 4), (std::max)(1u, render.m_height / 4));
		const glm::ivec2 viewportSize((std::max)(1, viewport.x / 4), (std::max)(1, viewport.y / 4));
		Require(graph->GetRenderTarget("Render")->GetExtent() == renderSize &&
			graph->GetSurface("Viewport")->GetTarget()->GetExtent() == viewportSize &&
			graph->GetSurface("Viewport")->GetResolved()->GetExtent() == viewportSize,
			"the importer must allocate texture and Surface images using their resolved declaration sizes");
		Require(graph->GetRenderTarget("Render")->GetMipLevels() == ((std::max)(renderSize.x, renderSize.y) > 1 ? 2u : 1u),
			"the authored positive mip limit must reach the native render target");
		FrameGraphImporterTestAccess::ReleaseInstance(*importer, instance);
		std::cout << "FrameGraph relative dimensions: variables, divisors, image extents and overflow passed\n";
	}

	class SceneNode : public RenderSceneNode
	{
	public:
		TRefPtr<SubmissionResources> GetResources(const RHISceneViewSnapshot& scene)
		{
			return scene.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(this, scene.m_cameraIndex, 0);
		}
	};

	class DepthNode : public DepthPrepassNode
	{
	public:
		TRefPtr<SubmissionResources> GetResources(const RHISceneViewSnapshot& scene)
		{
			return scene.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(this, scene.m_cameraIndex, 0);
		}
	};

	template<typename T>
	const T& GetSingleMobilityInstance(const TPackedDrawPacket<T>& packet, EMobilityType mobility, uint32_t drawIndex)
	{
		const auto& payload = packet.GetPayload(mobility);
		Require(packet.GetNumStorageInstances() == payload.GetNumStorageInstances(), "fixture packet must contain only one mobility");
		const uint32_t index = packet.GetInstanceIndices()[drawIndex];
		constexpr uint32_t pageSize = TPackedDrawArenaPage<T>::NumInstances;
		return payload.IsPagedArena() ? payload.m_arenaPages[index / pageSize]->m_instances[index % pageSize] : payload.m_instances[index];
	}

	void VerifyConcurrentPacketPreparation(const RHIFrameGraphPtr& graph, const RHISceneViewSnapshot& source,
		SceneNode& main, DepthNode& depth, ShadowCacheProbe& shadow, EMobilityType mobility)
	{
		std::array<RHISceneViewSnapshot, 4> views;
		TVector<Tasks::TaskPtr<void, void>> tasks;
		std::array<BaseFrameGraphNode*, 3> nodes{ &main, &depth, &shadow };
		for (uint32_t i = 0; i < views.size(); ++i)
		{
			auto& view = views[i];
			view.m_cameraIndex = 3 + i;
			view.m_frame = source.m_frame;
			view.m_submissionContext = source.m_submissionContext;
			view.m_sceneVersions = source.m_sceneVersions;
			view.m_previousMotionFrame = source.m_previousMotionFrame;
			view.m_cameraTransform = source.m_cameraTransform;
			view.m_shadowMapsToUpdate = source.m_shadowMapsToUpdate;
			view.m_shadowMapsToUpdate[0].m_payloadCompletionToken = RHISubmissionCompletionTokenPtr::Make();
			if (i % 2) view.m_proxies = source.m_proxies;
			else view.m_shadowMapsToUpdate[0].m_meshList.Clear(false);
			view.PrepareLods(glm::mat4(1), glm::mat4(1));
			for (auto* node : nodes) tasks.Add(node->Prepare(graph, view));
		}
		for (auto& task : tasks) task->Run();
		for (auto& task : tasks) task->Wait();
		for (uint32_t i = 0; i < views.size(); ++i)
		{
			const auto& view = views[i];
			const auto compare = [&](const auto& actual, const auto& expected)
			{
				const uint32_t count = i % 2 ? expected.GetNumInstances() : 0;
				Require(actual.GetNumInstances() == count, "concurrent cameras must retain their own draw visibility");
				for (uint32_t instance = 0; instance < count; ++instance)
				{
					Require(GetSingleMobilityInstance(actual, mobility, instance) == GetSingleMobilityInstance(expected, mobility, instance),
						"concurrent preparation must preserve complete finalized instance records");
				}
				const auto& payload = expected.GetPayload(mobility);
				if (payload.IsPagedArena())
				{
					const auto& pages = actual.GetPayload(mobility).m_arenaPages;
					Require(!pages.IsEmpty() && pages[0] == payload.m_arenaPages[0],
						"concurrent cameras must share complete-scene storage, not rebuild it per view");
				}
			};
			compare(main.GetResources(view)->m_packet, main.GetResources(source)->m_packet);
			compare(depth.GetResources(view)->m_customPacket, depth.GetResources(source)->m_customPacket);
			compare(shadow.GetResources(view)->m_activeShadowViews[0]->m_packet,
				shadow.GetResources(source)->m_activeShadowViews[0]->m_packet);
		}
	}

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

	std::array<ShaderSetPtr, 2> WriteCustomDepthShader(const std::filesystem::path& workspace)
	{
		const auto path = workspace / "Content" / "CustomMaskedDepth.shader";
		YAML::Node shader;
		shader["includes"].push_back("Shaders/Constants.glsl");
		for (const char* define : { "SKINNING", "ALPHA_CUTOUT", "PACKED_SHADOW_CASTER" }) shader["defines"].push_back(define);
		shader["glslCommon"] = R"glsl(
#version 450
layout(std430, set=3, binding=0) readonly buffer MaterialData { vec4 instance[]; } material;
)glsl";
		shader["glslVertex"] = R"glsl(
layout(location=DefaultPositionBinding) in vec3 position;
layout(location=DefaultTexcoordBinding) in vec2 texcoord;
layout(location=0) out vec2 uv;
layout(location=1) flat out uint materialIndex;
struct Instance {
	mat4 model; vec4 sphereBounds; uvec4 indices; vec4 bakedVolumeScale;
#ifdef PACKED_SHADOW_CASTER
	vec4 masked;
#else
	mat4 previousModel; uvec4 motionState;
#endif
};
layout(std430, set=2, binding=0) readonly buffer InstanceData { Instance instance[]; } data;
layout(std430, set=2, binding=1) readonly buffer InstanceIndices { uint instance[]; } instanceIndices;
layout(set=0, binding=0) uniform FrameData {
	mat4 view; mat4 projection; mat4 invProjection; vec4 cameraPosition;
	ivec2 viewportSize; vec2 cameraZNearZFar; float currentTime; float deltaTime;
} frame;
#ifdef PACKED_SHADOW_CASTER
layout(push_constant) uniform Shadow { mat4 lightMatrix; } shadow;
#endif
#ifdef SKINNING
layout(location=DefaultBoneIdsBinding) in uvec4 boneIds;
layout(location=DefaultBoneWeightsBinding) in vec4 weights;
layout(std430, set=5, binding=0) readonly buffer Bones { mat4 matrix[]; } bones;
#endif
void main() {
	Instance instance = data.instance[instanceIndices.instance[gl_InstanceIndex]];
	materialIndex = instance.indices.x;
	vec4 settings = material.instance[materialIndex];
	vec4 vertex = vec4(position.xy * settings.w + vec2(settings.x, 0), settings.z, 1);
#ifdef SKINNING
	vertex = (bones.matrix[instance.indices.y + boneIds.x] * weights.x +
		bones.matrix[instance.indices.y + boneIds.y] * weights.y +
		bones.matrix[instance.indices.y + boneIds.z] * weights.z +
		bones.matrix[instance.indices.y + boneIds.w] * weights.w) * vertex;
#endif
#ifdef PACKED_SHADOW_CASTER
	gl_Position = shadow.lightMatrix * instance.model * vertex;
#else
	gl_Position = frame.projection * frame.view * instance.model * vertex;
#endif
	uv = texcoord;
}
)glsl";
		shader["glslFragment"] = R"glsl(
layout(location=0) in vec2 uv;
layout(location=1) flat in uint materialIndex;
layout(location=0) out vec4 color;
void main() {
	if (uv.x < material.instance[materialIndex].y) discard;
#ifdef PACKED_SHADOW_CASTER
	color = vec4(gl_FragCoord.z);
#else
	color = vec4(1);
#endif
}
)glsl";
		std::ofstream output(path);
		output << shader;
		output.close();
		Require(static_cast<bool>(output), "custom masked shader fixture must be written");
		const auto id = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		auto* compiler = App::GetSubmodule<ShaderCompiler>();
		std::array<ShaderSetPtr, 2> result;
		for (uint32_t skinned = 0; skinned < result.size(); ++skinned)
		{
			TVector<std::string> defines{ "ALPHA_CUTOUT" };
			if (skinned) defines.Add("SKINNING");
			Require(compiler->LoadShader_Immediate(id, result[skinned], defines) && result[skinned]->IsReady(),
				"the custom main/depth shader must compile");
			defines.Add("PACKED_SHADOW_CASTER");
			ShaderSetPtr shadow;
			Require(compiler->LoadShader_Immediate(id, shadow, defines) && shadow->IsReady(),
				"the compatible packed shadow variant must compile");
		}
		return result;
	}

	void TestStaticMsaaBindings(const std::filesystem::path& workspace)
	{
		const auto path = workspace / "Content" / "StaticMsaaBindings.renderer";
		std::ofstream output(path);
		output << R"yaml(
renderTargets:
  - {name: Color, width: 8, height: 8, format: R32G32B32A32_SFLOAT}
  - {name: Motion, width: 8, height: 8, format: R32G32B32A32_SFLOAT}
  - {name: Other, width: 8, height: 8, format: R32G32B32A32_SFLOAT}
frame:
  - {name: Clear, tag: ClearColor, renderTargets: [{target: Color}]}
  - {name: RenderScene, tag: First, renderTargets: [{color: Color}, {motionVectors: Motion}]}
  - {name: RenderScene, tag: Second, renderTargets: [{color: Other}]}
  - {name: RenderScene, tag: External, renderTargets: [{color: Remote}, {motionVectors: Motion}]}
)yaml";
		output.close();
		Require(static_cast<bool>(output), "the static MSAA graph fixture must be written");
		const auto id = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		auto importer = App::GetSubmodule<FrameGraphImporter>();
		FrameGraphPtr firstInstance, secondInstance;
		Require(importer->Instantiate_Immediate(id, firstInstance) && importer->Instantiate_Immediate(id, secondInstance),
			"both static MSAA graph instances must load");
		const auto samples = App::GetSubmodule<Renderer>()->GetMsaaSamples();
		for (auto instance : { firstInstance, secondInstance })
		{
			auto graph = instance->GetRHI();
			auto first = graph->GetGraphNode("First");
			auto color = first->GetTargetAttachment("color", graph.GetRawPtr());
			auto motion = first->GetTargetAttachment("motionVectors", graph.GetRawPtr());
			Require(color->GetMsaaSamples() == samples && motion->GetMsaaSamples() == samples,
				"static MSAA attachments must be ready when the importer returns, before the first Process");
			Require(color == graph->GetGraphNode("ClearColor")->GetTargetAttachment("target", graph.GetRawPtr()) && color != motion &&
				color != graph->GetGraphNode("Second")->GetTargetAttachment("color", graph.GetRawPtr()),
				"static binding must share declared aliases without sharing independent outputs");
			Require(!graph->GetGraphNode("External")->GetRHIResource("color", graph.GetRawPtr()),
				"binding static targets must not require an unpublished external color");
			auto original = first->GetResolvedAttachment("color", graph.GetRawPtr());
			graph->SetRenderTarget("Color", graph->GetRenderTarget("Other"));
			Require(first->GetTargetAttachment("color", graph.GetRawPtr()) == color &&
				first->GetResolvedAttachment("color", graph.GetRawPtr()) == original,
				"static bindings must retain their image when a publication name is replaced");
		}
		Require(firstInstance->GetRHI()->GetGraphNode("First")->GetTargetAttachment("color", firstInstance->GetRHI().GetRawPtr()) !=
			secondInstance->GetRHI()->GetGraphNode("First")->GetTargetAttachment("color", secondInstance->GetRHI().GetRawPtr()),
			"separate imported graphs must not share implicit MSAA images");
		FrameGraphImporterTestAccess::ReleaseInstance(*importer, firstInstance);
		FrameGraphImporterTestAccess::ReleaseInstance(*importer, secondInstance);
		std::cout << "FrameGraph static MSAA binding: import-time attachments, aliases and independent instances passed\n";
	}

	void TestGraphMsaaTargets()
	{
		auto& driver = Renderer::GetDriver();
		auto graph = TRefPtr<TestGraph>::Make();
		auto first = TRefPtr<RenderSceneNode>::Make();
		auto second = TRefPtr<RenderSceneNode>::Make();
		auto firstOutput = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		auto secondOutput = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		auto unused = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		graph->SetRenderTarget("External", firstOutput);
		graph->SetRenderTarget("Unused", unused);
		first->SetRHIResource_Unresolved("color", "External");
		second->SetRHIResource("color", secondOutput);
		graph->GetGraph().Add(first);
		graph->GetGraph().Add(second);
		const auto prepare = [&]()
		{
			TVector<RHICommandListPtr> transfers, graphics;
			RHISemaphorePtr ready;
			Require(graph->Process(RHISceneViewPtr::Make(), transfers, graphics, {}, ready), "graph target preparation must succeed");
			Require(transfers.IsEmpty() && graphics.IsEmpty() && !ready, "an empty view must not record a camera");
		};
		prepare();
		auto firstLive = first->GetTargetAttachment("color", graph.GetRawPtr());
		auto secondLive = second->GetTargetAttachment("color", graph.GetRawPtr());
		const bool msaa = App::GetSubmodule<Renderer>()->GetMsaaSamples() != EMsaaSamples::Samples_1;
		Require(firstLive != secondLive && firstLive->GetMsaaSamples() == App::GetSubmodule<Renderer>()->GetMsaaSamples(),
			"same-format color outputs in separate passes need distinct native targets");
		Require(first->GetResolvedAttachment("color", graph.GetRawPtr()) == firstOutput &&
			second->GetSampledAttachment("color", graph.GetRawPtr()) == secondOutput && graph->GetResource("Unused") == unused,
			"MSAA preparation must retain resolve identities and leave unrelated targets alone");
		prepare();
		Require(first->GetTargetAttachment("color", graph.GetRawPtr()) == firstLive &&
			second->GetTargetAttachment("color", graph.GetRawPtr()) == secondLive, "unchanged outputs must reuse their MSAA targets");

		auto previous = first->GetRHIResource("color", graph.GetRawPtr());
		graph->SetRenderTarget("External", unused);
		prepare();
		Require(first->GetResolvedAttachment("color", graph.GetRawPtr()) == unused &&
			first->GetTargetAttachment("color", graph.GetRawPtr()) != firstLive &&
			second->GetTargetAttachment("color", graph.GetRawPtr()) == secondLive, "external replacement must change only its own target");
		if (msaa) Require(previous.NumRefs() == 1, "the graph must release a retired MSAA surface");
		previous = first->GetRHIResource("color", graph.GetRawPtr());
		graph->SetRenderTarget("External", {});
		prepare();
		Require(!first->GetRHIResource("color", graph.GetRawPtr()), "a withdrawn input must not retain its former surface");
		if (msaa) Require(previous.NumRefs() == 1, "withdrawal must release the graph's MSAA surface");
		graph->SetRenderTarget("External", firstOutput);
		prepare();
		Require(first->GetResolvedAttachment("color", graph.GetRawPtr()) == firstOutput, "a restored input must be usable again");
		graph->SetRenderTarget("External", secondOutput);
		prepare();
		Require(first->GetTargetAttachment("color", graph.GetRawPtr()) == secondLive, "aliases of one output must share its live target");

		auto declared = driver->CreateSurface(secondOutput);
		auto clear = TRefPtr<ClearNode>::Make();
		clear->SetRHIResource("target", declared);
		graph->Clear();
		graph->GetGraph().Add(clear);
		graph->GetGraph().Add(second);
		prepare();
		Require(second->GetTargetAttachment("color", graph.GetRawPtr()) == declared->GetTarget(),
			"a resolved output must reuse the Surface supplied to its clear pass");
		clear->SetVec4("clearColor", glm::vec4(0.5f));
		prepare();
		Require(second->GetTargetAttachment("color", graph.GetRawPtr()) == declared->GetTarget(),
			"changing a clear value must retain the prepared static attachments");
		second->SetRHIResource("color", unused);
		prepare();
		Require(second->GetResolvedAttachment("color", graph.GetRawPtr()) == unused,
			"a static resource setter must update an already prepared graph");
		auto alias = driver->CreateSurface(unused);
		graph->SetSurface("Alias", alias);
		prepare();
		Require(second->GetTargetAttachment("color", graph.GetRawPtr()) == alias->GetTarget(),
			"publishing an explicit Surface must update its prepared resolved alias");
		second->SetRHIResource_Unresolved("color", "Changed");
		graph->SetRenderTarget("Changed", firstOutput);
		prepare();
		Require(second->GetResolvedAttachment("color", graph.GetRawPtr()) == firstOutput,
			"switching a static input to external must join external refresh");
		second->SetRHIResource("color", unused);
		prepare();
		graph->SetRenderTarget("Changed", secondOutput);
		prepare();
		Require(second->GetResolvedAttachment("color", graph.GetRawPtr()) == unused,
			"switching an external input back to static must stop following its old name");
		auto replacement = TRefPtr<RenderSceneNode>::Make();
		replacement->SetRHIResource("color", firstOutput);
		graph->GetGraph()[1] = replacement;
		prepare();
		Require(replacement->GetTargetAttachment("color", graph.GetRawPtr())->GetMsaaSamples() ==
			App::GetSubmodule<Renderer>()->GetMsaaSamples(), "replacing a node at the same index must rebuild its bindings");
		graph->GetGraph().RemoveLast();
		prepare();
		Require(graph->ResolveResource(firstOutput) == firstOutput, "removing the last producer must retire its implicit Surface");
		graph->Clear();
		Require(graph->ResolveResource(secondOutput) == secondOutput, "clearing the graph must discard its MSAA associations");
		std::cout << "FrameGraph MSAA ownership: independent outputs, reuse, replacement, withdrawal, aliases and Clear passed\n";
	}

	RHIBufferPtr ReadColor(RHICommandListPtr command, RHITexturePtr texture);

	void TestSceneMrt(ShaderSetPtr shader, bool colorIsSurface, bool motionIsSurface, bool late, bool forceSingleSample = false,
		bool clearThroughNode = false)
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
		std::array<TRefPtr<ClearNode>, 2> clears;
		const auto publishOutputs = [&]()
		{
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
		};
		publishOutputs();
		auto depth = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::D32_SFLOAT_S8_UINT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit);
		graph->SetRenderTarget("SceneDepth", depth);
		auto view = RHISceneViewPtr::Make();
		view->m_snapshots.Resize(1);
		auto& scene = view->m_snapshots[0];
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
			if (clearThroughNode)
			{
				clears[i] = TRefPtr<ClearNode>::Make();
				if (late) clears[i]->SetRHIResource_Unresolved("target", names[i]);
				else clears[i]->SetRHIResource("target", resources[i]);
			}
		}
		if (late) node->SetRHIResource_Unresolved("depthStencil", "SceneDepth");
		else node->SetRHIResource("depthStencil", depth);
		if (clearThroughNode)
		{
			World cameraWorld("ClearSceneCamera", 0);
			auto camera = cameraWorld.Instantiate("Camera")->AddComponent<CameraComponent>();
			cameraWorld.GetECS<CameraECS>()->Tick(0);
			auto data = camera->GetData();
			data.SetOwner({});
			cameraWorld.Clear();
			scene.m_camera = TUniquePtr<CameraData>::Make(data);
			scene.m_bGlobalIlluminationEnabled = false;
			graph->SetRenderTarget("Main", outputs[0]);
			for (auto clear : clears) graph->GetGraph().Add(clear);
			graph->GetGraph().Add(node);
		}
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
		std::array<RHITexturePtr, 2> previousTargets;
		const uint32_t frames = clearThroughNode ? 4 : 2;
		for (uint32_t frame = 0; frame < frames; ++frame)
		{
			if (clearThroughNode)
			{
				if (frame == 2)
				{
					publishOutputs();
					graph->SetRenderTarget("Main", outputs[0]);
					if (!late)
						for (uint32_t i = 0; i < 2; ++i) node->SetRHIResource(inputs[i], resources[i]);
				}
				for (uint32_t i = 0; i < 2; ++i)
				{
					const std::string clearName = std::string("Clear") + names[i];
					graph->SetRenderTarget(clearName, outputs[i]);
					if (late) clears[i]->SetRHIResource_Unresolved("target", frame % 2 ? clearName : names[i]);
					else clears[i]->SetRHIResource("target", frame % 2 ? RHIResourcePtr(outputs[i]) : resources[i]);
				}
			}
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
				if (clearThroughNode)
				{
					if (frame == 0)
					{
						commands->ImageMemoryBarrier(draw, targets[i], EImageLayout::TransferDstOptimal);
						commands->ClearImage(draw, targets[i], glm::vec4(-16));
						commands->ImageMemoryBarrier(draw, targets[i], EImageLayout::ColorAttachmentOptimal);
					}
					clears[i]->SetVec4("clearColor", background[i] + glm::vec4(0.125f * frame));
					continue;
				}
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
			RHISemaphorePtr graphReady;
			if (clearThroughNode)
			{
				commands->EndCommandList(upload);
				commands->EndCommandList(draw);
				auto initialized = driver->CreateWaitSemaphore();
				Require(driver->SubmitCommandList(draw, RHIFencePtr::Make(), initialized), "MRT poison setup must submit");
				TVector<RHICommandListPtr> transfers, graphics;
				Require(graph->Process(view, transfers, graphics, initialized, graphReady), "Clear and RenderScene must execute through the frame graph");
				for (size_t i = 0; i < transfers.Num(); ++i)
					for (auto command : { transfers[i], graphics[i] })
					{
						auto next = driver->CreateWaitSemaphore();
						Require(driver->SubmitCommandList(command, RHIFencePtr::Make(), next, graphReady), "the composed graph must submit");
						graphReady = next;
					}
				for (uint32_t i = 0; i < 2; ++i)
				{
					targets[i] = node->GetTargetAttachment(inputs[i], graph.GetRawPtr());
					if (frame % 2) Require(targets[i] == previousTargets[i], "an unchanged MRT output must reuse its live image");
					else if (frame) Require(targets[i] != previousTargets[i], "a replaced MRT output needs its own live image");
					previousTargets[i] = targets[i];
				}
				upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(draw, true);
			}
			else
			{
				node->Process(graph, upload, draw, scene);
			}
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
			std::array<RHIBufferPtr, 4> readback;
			for (uint32_t i = 0; i < 2; ++i)
			{
				readback[i * 2] = ReadColor(draw, outputs[i]);
				readback[i * 2 + 1] = targets[i] == outputs[i] ? readback[i * 2] : ReadColor(draw, targets[i]);
			}
			commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			commands->EndCommandList(upload);
			commands->EndCommandList(draw);
			auto ready = driver->CreateWaitSemaphore();
			auto uploaded = RHIFencePtr::Make();
			auto finished = RHIFencePtr::Make();
			Require(driver->SubmitCommandList(upload, uploaded, ready, graphReady) && driver->SubmitCommandList(draw, finished, nullptr, ready),
				"MRT upload and draw must submit");
			Require(finished->Wait(5000000000ull) == EFenceStatus::Finished && uploaded->Wait(5000000000ull) == EFenceStatus::Finished,
				"MRT GPU readbacks must finish");
			for (uint32_t image = 0; image < readback.size(); ++image)
			{
				const auto pixels = static_cast<const glm::vec4*>(readback[image]->GetPointer());
				for (uint32_t i = 0; i < Side * Side; ++i)
					for (uint32_t component = 0; component < 4; ++component)
					{
						const auto expected = i % Side < Side / 2 ? drawn[image / 2] : background[image / 2] + glm::vec4(clearThroughNode ? 0.125f * frame : 0);
						if (!std::isfinite(pixels[i][component]) || std::abs(pixels[i][component] - expected[component]) >= 0.00001f)
							throw std::runtime_error("MRT uncovered pixel mismatch: clearNode=" + std::to_string(clearThroughNode) +
								", colorSurface=" + std::to_string(colorIsSurface) + ", motionSurface=" + std::to_string(motionIsSurface) +
								", frame=" + std::to_string(frame) + ", image=" + std::to_string(image) + ", pixel=" + std::to_string(i) +
								", actual=" + std::to_string(pixels[i][component]) + ", expected=" + std::to_string(expected[component]));
					}
			}
		}
		std::cout << (clearThroughNode ? "ClearSceneMRT " : "RenderScene MRT ") << (msaa ? "2x" : "1x") << " colorSurface=" << colorIsSurface <<
			" motionSurface=" << motionIsSurface << " late=" << late << ": " << frames << " frames / live and resolved images and native descriptors passed\n";
	}

	void ClearColor(RHICommandListPtr command, RHITexturePtr texture, glm::vec4 color)
	{
		auto commands = Renderer::GetDriverCommands();
		commands->ImageMemoryBarrier(command, texture, EImageLayout::TransferDstOptimal);
		commands->ClearImage(command, texture, color);
	}

	RHIBufferPtr ReadColor(RHICommandListPtr command, RHITexturePtr texture)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		commands->ImageMemoryBarrier(command, texture, EImageLayout::TransferSrcOptimal);
		if (texture->GetMsaaSamples() != EMsaaSamples::Samples_1)
		{
			auto resolved = driver->CreateRenderTarget(texture->GetExtent(), 1, texture->GetFormat());
			commands->ImageMemoryBarrier(command, resolved, EImageLayout::TransferDstOptimal);
			const glm::ivec4 area(0, 0, texture->GetExtent().x, texture->GetExtent().y);
			Require(commands->BlitImage(command, texture, resolved, area, area), "live MSAA readback must resolve");
			texture = resolved;
			commands->ImageMemoryBarrier(command, texture, EImageLayout::TransferSrcOptimal);
		}
		auto buffer = driver->CreateBuffer(Side * Side * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
		commands->CopyImageToBuffer(command, texture, buffer);
		return buffer;
	}

	void CompleteCommands(RHICommandListPtr upload, RHICommandListPtr draw)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
		commands->EndCommandList(upload);
		commands->EndCommandList(draw);
		auto ready = driver->CreateWaitSemaphore();
		auto uploaded = RHIFencePtr::Make();
		auto finished = RHIFencePtr::Make();
		Require(driver->SubmitCommandList(upload, uploaded, ready) && driver->SubmitCommandList(draw, finished, nullptr, ready),
			"fullscreen upload and draw must submit");
		Require(finished->Wait(5000000000ull) == EFenceStatus::Finished && uploaded->Wait(5000000000ull) == EFenceStatus::Finished,
			"fullscreen pixel readback must finish");
	}

	void TestGraphTargetLifetime()
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(upload, true);
		commands->BeginCommandList(draw, true);
		RHIBufferPtr pixels;
		{
			auto graph = TRefPtr<TestGraph>::Make();
			auto node = TRefPtr<RenderSceneNode>::Make();
			node->SetRHIResource("color", driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT));
			graph->GetGraph().Add(node);
			TVector<RHICommandListPtr> transfers, graphics;
			RHISemaphorePtr ready;
			Require(graph->Process(RHISceneViewPtr::Make(), transfers, graphics, {}, ready), "lifetime fixture must prepare its graph");
			auto target = node->GetTargetAttachment("color", graph.GetRawPtr());
			ClearColor(draw, target, glm::vec4(0.375f));
			pixels = ReadColor(draw, target);
			graph->Clear();
		}
		CompleteCommands(upload, draw);
		const auto values = static_cast<const glm::vec4*>(pixels->GetPointer());
		for (uint32_t pixel = 0; pixel < Side * Side; ++pixel)
			for (uint32_t component = 0; component < 4; ++component)
				Require(values[pixel][component] == 0.375f, "recorded commands must retain images after the graph and its nodes are destroyed");
		std::cout << "FrameGraph target lifetime: clear, graph destruction, submission and complete readback passed\n";
	}

	RHIBufferPtr ReadDepth(RHICommandListPtr command, RHIRenderTargetPtr texture, const std::array<ShaderSetPtr, 4>& shaders)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const uint32_t samples = static_cast<uint32_t>(texture->GetMsaaSamples());
		const bool stencil = IsDepthStencilFormat(texture->GetFormat());
		const auto size = texture->GetExtent();
		const uint32_t count = size.x * size.y * samples;
		auto buffer = driver->CreateBuffer(count * sizeof(glm::vec2), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
		std::fill_n(static_cast<glm::vec2*>(buffer->GetPointer()), count, glm::vec2(std::numeric_limits<float>::quiet_NaN()));
		auto bindings = driver->CreateShaderBindings();
		Require(driver->AddSamplerToShaderBindings(bindings, "depthSampler", texture->GetDepthAspect(), 0).IsValid(),
			"readback must bind the real depth view");
		if (stencil)
		{
			auto stencilView = RHITexturePtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false);
			stencilView->m_vulkan = texture->GetStencilAspect()->m_vulkan;
			Require(driver->AddSamplerToShaderBindings(bindings, "stencilSampler", stencilView, 1).IsValid(),
				"readback must bind the integer stencil view");
		}
		Require(driver->AddBufferToShaderBindings(bindings, buffer, "outputValues", 2).IsValid(), "depth readback storage must bind");
		commands->MemoryBarrier(command, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit), static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit));
		commands->ImageMemoryBarrierForComputeSampling(command, texture);
		commands->Dispatch(command, shaders[(samples > 1 ? 1 : 0) | (stencil ? 2 : 0)]->GetComputeShaderRHI(),
			(size.x + 7) / 8, (size.y + 7) / 8, 1, { bindings });
		commands->MemoryBarrier(command, static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
		return buffer;
	}

	void TestCustomDepthSilhouette(ShaderSetPtr shader, const std::array<ShaderSetPtr, 4>& depthReadback,
		bool paged, bool instanced, bool skinned, EMobilityType mobility)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto mesh = RHIMeshPtr::Make();
		mesh->m_vertexDescription = driver->GetOrAddVertexDescription<VertexP3N3T3B3UV2C4I4W4>();
		std::array<VertexP3N3T3B3UV2C4I4W4, 4> vertices{};
		for (uint32_t i = 0; i < vertices.size(); ++i)
		{
			vertices[i].m_position = glm::vec3(i % 2 ? 1 : -1, i / 2 ? 1 : -1, 0.5f);
			vertices[i].m_texcoord = glm::vec2(i % 2, i / 2);
			vertices[i].m_color = glm::vec4(1);
			vertices[i].m_boneWeights = glm::vec4(1, 0, 0, 0);
		}
		const uint32_t indices[] = { 0, 1, 2, 2, 1, 3 };
		mesh->m_vertexBuffer = driver->CreateBuffer(sizeof(vertices), EBufferUsageBit::VertexBuffer_Bit, HostMemory);
		mesh->m_indexBuffer = driver->CreateBuffer(sizeof(indices), EBufferUsageBit::IndexBuffer_Bit, HostMemory);
		std::memcpy(mesh->m_vertexBuffer->GetPointer(), vertices.data(), sizeof(vertices));
		std::memcpy(mesh->m_indexBuffer->GetPointer(), indices, sizeof(indices));
		mesh->m_bounds = Math::AABB(glm::vec3(0), glm::vec3(2));
		const auto materialBindings = [&](float shift)
		{
			const glm::vec4 settings(shift, 0.75f, 0.5f, 0.5f);
			auto buffer = driver->CreateBuffer(sizeof(settings), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
			std::memcpy(buffer->GetPointer(), &settings, sizeof(settings));
			auto result = driver->CreateShaderBindings();
			Require(driver->AddBufferToShaderBindings(result, buffer, "material", 0).IsValid(), "custom material parameters must bind");
			return result;
		};
		const RenderState state(true, true, 0, true, ECullMode::None, EBlendMode::None, EFillMode::Fill, "Masked"_h.GetHash(), true);
		auto material = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shader, materialBindings(0.25f));
		Require(material && material->GetVersion(), "custom masked material must have a complete version");
		mesh->m_bakedVolumeScale = glm::vec3(2, 3, 4);
		RHISceneViewProxy source;
		source.m_staticMeshEcs = 1;
		source.m_mobility = mobility;
		source.m_worldMatrix = glm::mat4(1);
		source.m_worldAabb = Math::AABB(glm::vec3(0), glm::vec3(3));
		source.m_shadowCaster = RHIShadowCasterProxyPtr::Make();
		source.m_shadowCaster->m_worldAabb = source.m_worldAabb;
		glm::mat4 second(1);
		second[3].x = -1;
		if (instanced)
		{
			RHIInstancedMeshGroup group;
			group.m_instanceTransforms = { glm::mat4(1), second };
			group.m_meshes = { mesh };
			group.m_materials = { material };
			group.m_sourceMaterialShaders = { shader };
			group.m_renderQueueTags = { "Masked"_h.GetHash() };
			group.m_baseColorFactors = { glm::vec4(1) };
			group.m_baseColorSamplers = { 0 };
			group.m_alphaCutoffs = { 0.75f };
			group.m_bCastShadows = true;
#if defined(__APPLE__)
			group.m_materialTextureSamplers = { { 0 } };
#endif
			source.m_instancedGroups.Add(std::move(group));
		}
		else
		{
			source.m_meshes = { mesh, mesh };
			source.m_meshModelMatrices = { glm::mat4(1), second };
			source.m_overrideMaterials = { material, material };
			source.m_renderQueueTags = { "Masked"_h.GetHash(), "Masked"_h.GetHash() };
			source.m_baseColorFactors = { glm::vec4(1), glm::vec4(1) };
			source.m_baseColorSamplers = { 0, 0 };
			source.m_alphaCutoffs = { 0.75f, 0.75f };
#if defined(__APPLE__)
			source.m_materialTextureSamplers = { { 0 }, { 0 } };
#endif
			for (const auto& transform : source.m_meshModelMatrices)
			{
				RHIShadowMeshProxy caster;
				caster.m_mesh = mesh;
				caster.m_worldMatrix = transform;
				caster.m_renderQueueTag = "Masked"_h.GetHash();
				caster.m_customDepthMaterial = material;
				caster.m_customDepthShader = shader;
				caster.m_alphaCutoff = 0.75f;
#if defined(__APPLE__)
				caster.m_materialTextureSamplers = { 0 };
#endif
				source.m_shadowCaster->m_meshes.Add(std::move(caster));
			}
		}
		auto topology = RHISceneProxyResourcePtr::Make(std::move(source));
		RHISceneInstanceRecord record;
		record.m_producerKey = 1;
		record.m_mobility = mobility;
		record.m_worldMatrix = glm::mat4(1);
		record.m_worldBounds = topology->m_proxy.m_worldAabb;
		record.m_topology = topology;
		record.m_topologyRevision = topology->m_mainRevision;
		record.m_shadowRevision = topology->m_shadowRevision;
		record.m_renderFlags = 1;
		record.m_skeletonOffset = skinned ? 0 : std::numeric_limits<uint32_t>::max();
		auto scene = RHIScenePtr::Make();
		auto previousRecord = record;
		previousRecord.m_worldMatrix[3].y = -0.125f;
		const auto handle = scene->AddInstance(previousRecord);
		RHISceneViewSnapshot previousSnapshot;
		previousSnapshot.m_sceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make();
		previousSnapshot.m_sceneVersions->Add(scene->PublishVersion());
		auto previousFrame = TSharedPtr<RHIMotionHistoryFrame>::Make();
		previousFrame->m_sceneVersions = previousSnapshot.m_sceneVersions;
		previousFrame->m_mobilityRevisions[static_cast<size_t>(mobility)] = previousSnapshot.GetMobilityRevision(mobility);
		Require(scene->UpdateInstance(handle, record, ToMask(ESceneChangeBit::Transform)), "fixture must publish its current transform");
		RHISceneViewSnapshot snapshot;
		snapshot.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		snapshot.m_sceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make();
		snapshot.m_sceneVersions->Add(scene->PublishVersion());
		snapshot.m_camera = TUniquePtr<CameraData>::Make();
		snapshot.m_frameBindings = driver->CreateShaderBindings();
		snapshot.m_rhiLightsData = driver->CreateShaderBindings();
		snapshot.m_bGlobalIlluminationEnabled = false;
		UboFrameData frameData{};
		frameData.m_view = frameData.m_projection = frameData.m_invProjection = glm::mat4(1);
		frameData.m_viewportSize = glm::ivec2(Side);
		for (uint32_t binding = 0; binding < 2; ++binding)
		{
			auto buffer = driver->CreateBuffer(sizeof(frameData), EBufferUsageBit::UniformBuffer_Bit, HostMemory);
			std::memcpy(buffer->GetPointer(), &frameData, sizeof(frameData));
			driver->AddBufferToShaderBindings(snapshot.m_frameBindings, buffer, binding ? "previousFrame" : "frame", binding);
		}
		if (skinned)
		{
			glm::mat4 bone(1);
			bone[3].x = 0.25f;
			auto buffer = driver->CreateBuffer(sizeof(bone), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
			std::memcpy(buffer->GetPointer(), &bone, sizeof(bone));
			snapshot.m_boneMatrices = driver->CreateShaderBindings();
			driver->AddBufferToShaderBindings(snapshot.m_boneMatrices, buffer, "bones", 0);
		}
		snapshot.ForEachSceneProxy(mobility, [&](const RHIVisibleSceneProxy& proxy) { snapshot.m_proxies.Add(proxy); });
		RHIUpdateShadowMapCommand shadowPass;
		shadowPass.m_shadowType = EShadowType::PCF;
		shadowPass.m_lightMatrix = glm::mat4(1);
		shadowPass.m_shadowMap = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		snapshot.ForEachShadowCaster(mobility, [&](const RHIVisibleShadowCaster& caster) { shadowPass.m_meshList.Add(caster); });
		snapshot.m_shadowMapsToUpdate.Add(std::move(shadowPass));
		snapshot.PrepareLods(glm::mat4(1), glm::mat4(1));
		auto color = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		const auto depthTarget = [&]()
		{
			return driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::D32_SFLOAT,
				ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit);
		};
		auto prepassDepth = depthTarget();
		auto mainDepth = depthTarget();
		auto depth = TRefPtr<DepthNode>::Make();
		auto main = TRefPtr<SceneNode>::Make();
		auto shadow = TRefPtr<ShadowCacheProbe>::Make();
		for (FrameGraphNodePtr node : { FrameGraphNodePtr(depth), FrameGraphNodePtr(main), FrameGraphNodePtr(shadow) })
		{
			node->SetString("Tag", "Masked");
			node->SetString("GPUCulling", "false");
			node->SetString("VirtualizeInstancePayloads", paged ? "true" : "false");
		}
		depth->SetString("ClearDepth", "true");
		depth->SetRHIResource("depthStencil", prepassDepth);
		main->SetRHIResource("color", color);
		main->SetRHIResource("depthStencil", mainDepth);
		for (uint32_t frame = 0; frame < 4; ++frame)
		{
			if (frame == 2) material->SetBindings(materialBindings(0));
			snapshot.m_frame = frame + 1;
			if (frame == 2) snapshot.m_previousMotionFrame = previousFrame;
			const uint64_t submissionId = 177000 + frame;
			const uint64_t materialRevision = RHIMaterial::BeginSubmissionVersionCapture(submissionId);
			snapshot.m_submissionContext->BeginSubmission(submissionId, 0, 0, materialRevision);
			std::array<const void*, 3> arenaPages{};
			for (uint32_t camera = 0; camera < 3; ++camera)
			{
				snapshot.m_cameraIndex = camera;
				snapshot.m_proxies.Clear(false);
				snapshot.m_shadowMapsToUpdate[0].m_meshList.Clear(false);
				if (camera != 0)
				{
					snapshot.ForEachSceneProxy(mobility, [&](const RHIVisibleSceneProxy& proxy) { snapshot.m_proxies.Add(proxy); });
					snapshot.ForEachShadowCaster(mobility, [&](const RHIVisibleShadowCaster& caster)
						{ snapshot.m_shadowMapsToUpdate[0].m_meshList.Add(caster); });
				}
				snapshot.m_shadowMapsToUpdate[0].m_payloadCompletionToken = RHISubmissionCompletionTokenPtr::Make();
				for (FrameGraphNodePtr node : { FrameGraphNodePtr(depth), FrameGraphNodePtr(main), FrameGraphNodePtr(shadow) })
				{
					auto prepare = node->Prepare(graph, snapshot);
					if (prepare) { prepare->Run(); prepare->Wait(); }
				}
				const auto& mainPacket = main->GetResources(snapshot)->m_packet;
				const auto& depthPacket = depth->GetResources(snapshot)->m_customPacket;
				const auto& shadowPacket = shadow->GetResources(snapshot)->m_activeShadowViews[0]->m_packet;
				if (paged && mobility != EMobilityType::Dynamic)
				{
					const auto verifyArena = [&](const auto& packet, size_t pass)
					{
						const auto& payload = packet.GetPayload(mobility);
						Require(payload.IsPagedArena() && !payload.m_arenaPages.IsEmpty() && payload.m_arenaPages[0],
							"an empty camera must still prepare complete-scene arena storage");
						if (camera == 0) arenaPages[pass] = payload.m_arenaPages[0].GetRawPtr();
						else Require(arenaPages[pass] == payload.m_arenaPages[0].GetRawPtr(),
							"visible cameras must reuse the arena built with no visible objects");
					};
					verifyArena(mainPacket, 0);
					verifyArena(depthPacket, 1);
					verifyArena(shadowPacket, 2);
				}
				if (camera == 0)
				{
					Require(mainPacket.GetNumInstances() == 0 && depthPacket.GetNumInstances() == 0 && shadowPacket.GetNumInstances() == 0,
						"complete-scene storage must not add draws to an empty camera");
					continue;
				}
				Require(mainPacket.GetNumInstances() == 2 && depthPacket.GetNumInstances() == 2 && shadowPacket.GetNumInstances() == 2,
					"each prepared packet must retain both authored instances");
				const RHIShaderBindingPtr* materialBinding = nullptr;
				Require(material->GetVersion()->GetBindingsRaw()->GetShaderBindings().Find("material", materialBinding),
					"fixture material must retain its storage binding");
				RenderSceneNode::PerInstanceData expectedMain;
				expectedMain.model = glm::mat4(1);
				expectedMain.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();
				expectedMain.materialInstance = (*materialBinding)->GetStorageInstanceIndex();
				expectedMain.skeletonOffset = record.m_skeletonOffset;
				expectedMain.bakedVolumeScale = glm::vec4(2, 3, 4, 1);
				const auto expectedDepth = expectedMain;
				ShadowPrepassNode::PerInstanceData expectedShadow;
				expectedShadow.model = glm::mat4(1);
				expectedShadow.sphereBounds = expectedMain.sphereBounds;
				expectedShadow.materialInstance = expectedMain.materialInstance;
				expectedShadow.skeletonOffset = expectedMain.skeletonOffset;
				expectedShadow.bakedVolumeScale = expectedMain.bakedVolumeScale;
				expectedShadow.alphaCutoff = 0.75f;
				auto verifyPacket = [&](const auto& packet, auto expected, bool motion)
				{
					bool sawFirst = false, sawSecond = false;
					for (uint32_t i = 0; i < 2; ++i)
					{
						const auto& actual = GetSingleMobilityInstance(packet, mobility, i);
						sawFirst |= actual.model == glm::mat4(1);
						sawSecond |= actual.model == second;
						expected.model = actual.model;
						if constexpr (std::is_same_v<decltype(expected), RenderSceneNode::PerInstanceData>)
						{
							if (motion)
							{
								expected.motion.m_previousModel = actual.model;
								if (frame >= 2)
								{
									expected.motion.m_previousModel[3].y -= 0.125f;
									expected.motion.m_state = glm::uvec4(record.m_skeletonOffset, 1, 0, 0);
								}
							}
						}
						Require(actual == expected, "prepared instance fields, reserved fields and material indices must match the authored fixture");
					}
					Require(sawFirst && sawSecond, "packet transforms must retain both distinct authored models");
				};
				verifyPacket(mainPacket, expectedMain, true);
				verifyPacket(depthPacket, expectedDepth, false);
				verifyPacket(shadowPacket, expectedShadow, false);
				auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(draw, true);
				depth->Process(graph, upload, draw, snapshot);
				auto depthPixels = ReadDepth(draw, prepassDepth, depthReadback);
				commands->BeginRenderPass(draw, TVector<RHITexturePtr>{ color }, mainDepth,
					glm::ivec4(0, 0, Side, Side), glm::ivec2(0), true, glm::vec4(0), 0, true, true);
				commands->EndRenderPass(draw);
				main->Process(graph, upload, draw, snapshot);
				auto mainPixels = ReadColor(draw, color);
				shadow->Process(graph, upload, draw, snapshot);
				auto shadowPixels = ReadColor(draw, snapshot.m_shadowMapsToUpdate[0].m_shadowMap);
				CompleteCommands(upload, draw);
				Require(depth->GetDrawCallStats().m_numInstances == 2 && main->GetDrawCallStats().m_numInstances == 2 &&
					shadow->GetDrawCallStats().m_numInstances == 2, "all three real passes must draw both fixture instances");
				const auto depths = static_cast<const glm::vec2*>(depthPixels->GetPointer());
				const auto colors = static_cast<const glm::vec4*>(mainPixels->GetPointer());
				const auto shadows = static_cast<const glm::vec4*>(shadowPixels->GetPointer());
				const uint32_t column = (frame < 2 ? 6 : 5) + (skinned ? 1 : 0);
				for (uint32_t y = 0; y < Side; ++y)
					for (uint32_t x = 0; x < Side; ++x)
					{
						const uint32_t pixel = y * Side + x;
						const bool covered = y >= 2 && y < 6 && (x == column || x == column - 4);
						if ((colors[pixel].r > 0.1f) != covered || (depths[pixel].x > 0.1f) != covered ||
							(shadows[pixel].r > 0.1f) != covered)
						{
							std::cerr << "Custom depth paged=" << paged << " instanced=" << instanced << " skinned=" << skinned
								<< " frame=" << frame << " pixel=" << x << ',' << y << " covered=" << covered
								<< " main=" << colors[pixel].r << " depth=" << depths[pixel].x << " shadow=" << shadows[pixel].r << '\n';
						}
						Require((colors[pixel].r > 0.1f) == covered, "main-pass pixels must show the authored displacement, cutoff and skinning");
						Require((depths[pixel].x > 0.1f) == covered, "masked custom depth must match the main-pass silhouette");
						Require((shadows[pixel].r > 0.1f) == covered, "packed custom shadow pixels must match the main-pass silhouette");
					}
			}
			VerifyConcurrentPacketPreparation(graph, snapshot, *main, *depth, *shadow, mobility);
			RHIMaterial::EndSubmissionVersionCapture(submissionId);
		}
		std::cout << "Custom masked depth paged=" << paged << " instanced=" << instanced << " skinned=" << skinned
			<< " mobility=" << static_cast<uint32_t>(mobility)
			<< ": main/depth/shadow silhouettes, concurrent empty/visible cameras, shared arenas and material replacement passed\n";
	}

	void TestTransparentPacketOrder(ShaderSetPtr shader, bool paged, bool instanced)
	{
		auto& driver = Renderer::GetDriver();
		auto graph = TRefPtr<TestGraph>::Make();
		auto mesh = graph->GetFullscreenNdcQuad();
		mesh->m_bounds = Math::AABB(glm::vec3(0), glm::vec3(1));
		auto bindings = driver->CreateShaderBindings();
		auto buffer = driver->CreateBuffer(sizeof(glm::vec4), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
		std::memset(buffer->GetPointer(), 0, sizeof(glm::vec4));
		Require(driver->AddBufferToShaderBindings(bindings, buffer, "material", 0).IsValid(), "transparent fixture parameters must bind");
		const RenderState state(false, false, 0, false, ECullMode::None, EBlendMode::AlphaBlending,
			EFillMode::Fill, "Translucent"_h.GetHash(), false);
		auto material = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shader, bindings);
		Require(material && material->GetVersion(), "transparent fixture must have a complete material");
		auto scene = RHIScenePtr::Make();
		uint32_t id = 0;
		for (auto mobility : { EMobilityType::Static, EMobilityType::Stationary, EMobilityType::Dynamic })
		{
			RHISceneViewProxy source;
			source.m_staticMeshEcs = ++id;
			source.m_mobility = mobility;
			source.m_worldMatrix = glm::translate(glm::mat4(1), glm::vec3(0, 0, -static_cast<float>(id)));
			source.m_worldAabb = Math::AABB(glm::vec3(0, 0, -static_cast<float>(id)), glm::vec3(1));
			if (instanced)
			{
				RHIInstancedMeshGroup group;
				group.m_instanceTransforms = { glm::mat4(1) };
				group.m_meshes = { mesh };
				group.m_materials = { material };
				source.m_instancedGroups.Add(std::move(group));
			}
			else
			{
				source.m_meshes = { mesh };
				source.m_meshModelMatrices = { source.m_worldMatrix };
				source.m_overrideMaterials = { material };
			}
			RHISceneInstanceRecord record;
			record.m_producerKey = id;
			record.m_mobility = mobility;
			record.m_worldMatrix = source.m_worldMatrix;
			record.m_worldBounds = source.m_worldAabb;
			record.m_topology = RHISceneProxyResourcePtr::Make(std::move(source));
			scene->AddInstance(record);
		}
		World cameraWorld("TransparentPacketCamera", 0);
		auto camera = cameraWorld.Instantiate("Camera")->AddComponent<CameraComponent>();
		cameraWorld.GetECS<CameraECS>()->Tick(0);
		auto cameraData = camera->GetData();
		cameraData.SetOwner({});
		cameraWorld.Clear();
		RHISceneViewSnapshot snapshot;
		snapshot.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		snapshot.m_sceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make();
		snapshot.m_sceneVersions->Add(scene->PublishVersion());
		snapshot.m_camera = TUniquePtr<CameraData>::Make(cameraData);
		auto node = TRefPtr<SceneNode>::Make();
		node->SetString("Tag", "Translucent");
		node->SetString("Sorting", "BackToFront");
		node->SetString("VirtualizeInstancePayloads", paged ? "true" : "false");
		const auto firstResources = node->GetResources(snapshot);
		for (uint32_t cameraIndex = 0; cameraIndex < 2; ++cameraIndex)
		{
			snapshot.m_cameraIndex = cameraIndex;
			snapshot.m_proxies.Clear();
			for (auto mobility : { EMobilityType::Static, EMobilityType::Stationary, EMobilityType::Dynamic })
				snapshot.ForEachSceneProxy(mobility, [&](const RHIVisibleSceneProxy& proxy)
				{
					if (cameraIndex == 0 || proxy.GetSource()->m_staticMeshEcs != 2) snapshot.m_proxies.Add(proxy);
				});
			auto prepare = node->Prepare(graph, snapshot);
			prepare->Run();
			prepare->Wait();
			const auto& packet = node->GetResources(snapshot)->m_packet;
			const TVector<float> expected = cameraIndex == 0 ? TVector<float>{ -3, -2, -1 } : TVector<float>{ -3, -1 };
			Require(packet.GetNumInstances() == expected.Num() && packet.GetNumStorageInstances() == expected.Num() &&
				packet.GetGroups().Num() == expected.Num() && !packet.HasSharedImmutablePayload(),
				"transparent draws from every mobility must stay camera-local even with virtualization enabled");
			for (uint32_t i = 0; i < expected.Num(); ++i)
				Require(packet.GetPayload(EMobilityType::Dynamic).m_instances[i].model[3].z == expected[i] &&
					packet.GetInstanceIndices()[i] == i && packet.GetGroups()[i].m_firstInstance == i &&
					packet.GetGroups()[i].m_numInstances == 1 && packet.GetGroups()[i].m_batch.m_materialVersion == material->GetVersion(),
					"back-to-front packet order, indirect offsets and material versions must match each camera's visible set");
		}
		Require(firstResources->m_packet.GetNumInstances() == 3 &&
			firstResources->m_packet.GetPayload(EMobilityType::Dynamic).m_instances[1].model[3].z == -2,
			"preparing the second camera must not rewrite the first camera's packet");
		std::cout << "Transparent packet paged=" << paged << " instanced=" << instanced
			<< ": two cameras, mixed mobility, depth order, indices and material versions passed\n";
	}

	void TestDepthPacketParameters(ShaderSetPtr shader, bool paged, bool instanced, bool masked, EMobilityType mobility)
	{
		auto& driver = Renderer::GetDriver();
		auto graph = TRefPtr<TestGraph>::Make();
		auto mesh = graph->GetFullscreenNdcQuad();
		mesh->m_bounds = Math::AABB(glm::vec3(0), glm::vec3(1));
		auto bindings = driver->CreateShaderBindings();
		auto buffer = driver->CreateBuffer(sizeof(glm::vec4), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
		std::memset(buffer->GetPointer(), 0, sizeof(glm::vec4));
		Require(driver->AddBufferToShaderBindings(bindings, buffer, "material", 0).IsValid(), "depth fixture parameters must bind");
		const RenderState state(true, true, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill,
			masked ? "Masked"_h.GetHash() : "Opaque"_h.GetHash(), true);
		auto material = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shader, bindings);
		Require(material && material->GetVersion(), "depth fixture must have a complete source material");
		TVector<glm::mat4> models{ glm::mat4(1), glm::mat4(1), glm::mat4(1) };
		models[1][3].x = 1;
		models[2][3].x = 2;
		const TVector<glm::vec4> colors{ glm::vec4(0), glm::vec4(0.5f), glm::vec4(1) };
		RHISceneViewProxy source;
		source.m_staticMeshEcs = 1;
		source.m_mobility = mobility;
		source.m_worldMatrix = glm::mat4(1);
		source.m_worldAabb = Math::AABB(glm::vec3(0), glm::vec3(4));
		if (instanced)
		{
			RHIInstancedMeshGroup group;
			group.m_instanceTransforms = { glm::mat4(1) };
			group.m_meshes = { mesh, mesh, mesh };
			group.m_meshTransforms = models;
			group.m_materials = { material, material, material };
			group.m_baseColorFactors = colors;
			group.m_alphaCutoffs = { 0.4f, 0.4f, 0.4f };
			source.m_instancedGroups.Add(std::move(group));
		}
		else
		{
			source.m_meshes = { mesh, mesh, mesh };
			source.m_meshModelMatrices = models;
			source.m_overrideMaterials = { material, material, material };
			source.m_baseColorFactors = colors;
			source.m_alphaCutoffs = { 0.4f, 0.4f, 0.4f };
		}
		RHISceneInstanceRecord record;
		record.m_producerKey = 1;
		record.m_mobility = mobility;
		record.m_worldMatrix = glm::mat4(1);
		record.m_worldBounds = source.m_worldAabb;
		record.m_topology = RHISceneProxyResourcePtr::Make(std::move(source));
		auto scene = RHIScenePtr::Make();
		scene->AddInstance(record);
		RHISceneViewSnapshot snapshot;
		snapshot.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		snapshot.m_sceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make();
		snapshot.m_sceneVersions->Add(scene->PublishVersion());
		snapshot.ForEachSceneProxy(mobility, [&](const RHIVisibleSceneProxy& proxy) { snapshot.m_proxies.Add(proxy); });
		auto node = TRefPtr<DepthNode>::Make();
		node->SetString("Tag", masked ? "Masked" : "Opaque");
		node->SetString("VirtualizeInstancePayloads", paged ? "true" : "false");
		auto visibleProxies = std::move(snapshot.m_proxies);
		auto prepare = node->Prepare(graph, snapshot);
		prepare->Run();
		prepare->Wait();
		const auto emptyResources = node->GetResources(snapshot);
		Require(emptyResources->m_packet.GetNumInstances() == 0 && emptyResources->m_customPacket.GetNumInstances() == 0,
			"an empty camera must not draw complete-scene depth instances");
		snapshot.m_cameraIndex = 1;
		snapshot.m_proxies = std::move(visibleProxies);
		prepare = node->Prepare(graph, snapshot);
		prepare->Run();
		prepare->Wait();
		const auto resources = node->GetResources(snapshot);
		if (paged && mobility != EMobilityType::Dynamic)
		{
			const auto& emptyPayload = emptyResources->m_packet.GetPayload(mobility);
			const auto& visiblePayload = resources->m_packet.GetPayload(mobility);
			Require(emptyPayload.IsPagedArena() && visiblePayload.IsPagedArena() &&
				!emptyPayload.m_arenaPages.IsEmpty() && !visiblePayload.m_arenaPages.IsEmpty() &&
				emptyPayload.m_arenaPages[0] == visiblePayload.m_arenaPages[0],
				"compact depth must reuse the complete-scene page prepared by the empty camera");
		}
		Require(resources->m_customPacket.GetNumInstances() == 0 && resources->m_packet.GetNumInstances() == 3,
			"generic depth must keep all records in its compact packet");
		const float cutoffs[] = { 2, 0.8f, 0.4f };
		bool seen[3]{};
		for (uint32_t i = 0; i < 3; ++i)
		{
			const auto& actual = GetSingleMobilityInstance(resources->m_packet, mobility, i);
			const uint32_t index = actual.model == models[0] ? 0 : actual.model == models[1] ? 1 : 2;
			DepthPrepassNode::PerInstanceData expected;
			expected.model = models[index];
			expected.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();
			expected.skeletonOffset = std::numeric_limits<uint32_t>::max();
			expected.padding = masked ? glm::floatBitsToUint(cutoffs[index]) : 0;
			Require(actual == expected && !seen[index], "compact depth fields must preserve zero-alpha rejection, scaled cutoff and opaque defaults");
			seen[index] = true;
		}
		std::cout << "Depth packet paged=" << paged << " instanced=" << instanced << " masked=" << masked
			<< " mobility=" << static_cast<uint32_t>(mobility) << ": empty/visible cameras, exact compact records and alpha policy passed\n";
	}

	void TestCustomShadowCache(ShaderSetPtr shader, bool paged)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto mesh = graph->GetFullscreenNdcQuad();
		mesh->m_bounds = Math::AABB(glm::vec3(0), glm::vec3(2));
		auto node = TRefPtr<ShadowCacheProbe>::Make();
		node->SetString("VirtualizeInstancePayloads", paged ? "true" : "false");
		node->SetString("GPUCulling", "false");
		auto liveBindings = TSharedPtr<uint32_t>::Make(0u);
		TVector<RHIMaterialPtr> retiredSources;
		RHIRenderSubmissionContextPtr heldContext;
		RHICommandListPtr heldUpload, heldDraw;
		std::array<RHIBufferPtr, 3> heldPixels;
		const RHIShaderBindingSet* heldBindings = nullptr;
		const auto checkPixels = [](const std::array<RHIBufferPtr, 3>& pixels, uint32_t column)
		{
			for (const auto& buffer : pixels)
			{
				const auto values = static_cast<const glm::vec4*>(buffer->GetPointer());
				for (uint32_t y = 0; y < Side; ++y)
					for (uint32_t x = 0; x < Side; ++x)
						Require((values[y * Side + x].r > 0.1f) == (y >= 2 && y < 6 && x == column),
							"every cascade must render the retained custom material parameters");
			}
		};
		for (uint32_t frame = 1; frame <= 24; ++frame)
		{
			const glm::vec4 settings(frame % 2 ? 0.25f : 0.0f, 0.75f, 0.5f, 0.5f);
			auto buffer = driver->CreateBuffer(sizeof(settings), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
			std::memcpy(buffer->GetPointer(), &settings, sizeof(settings));
			RHIShaderBindingSetPtr bindings = TRefPtr<CountedMaterialBindings>::Make(liveBindings);
			Require(driver->AddBufferToShaderBindings(bindings, buffer, "material", 0).IsValid(),
				"counted source bindings must use real native descriptors");
			const RenderState state(true, true, 0, true, ECullMode::None, EBlendMode::None, EFillMode::Fill, "Masked"_h.GetHash(), true);
			auto source = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shader, bindings);
			RHISceneViewSnapshot snapshot;
			snapshot.m_frame = frame;
			snapshot.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
			snapshot.m_camera = TUniquePtr<CameraData>::Make();
			snapshot.m_frameBindings = driver->CreateShaderBindings();
			snapshot.m_rhiLightsData = driver->CreateShaderBindings();
			UboFrameData frameData{};
			frameData.m_view = frameData.m_projection = frameData.m_invProjection = glm::mat4(1);
			auto frameBuffer = driver->CreateBuffer(sizeof(frameData), EBufferUsageBit::UniformBuffer_Bit, HostMemory);
			std::memcpy(frameBuffer->GetPointer(), &frameData, sizeof(frameData));
			driver->AddBufferToShaderBindings(snapshot.m_frameBindings, frameBuffer, "frame", 0);
			RHISceneViewProxy proxy;
			proxy.m_staticMeshEcs = frame;
			proxy.m_mobility = EMobilityType::Static;
			proxy.m_worldMatrix = glm::mat4(1);
			proxy.m_worldAabb = mesh->m_bounds;
			proxy.m_meshes = { mesh };
			proxy.m_overrideMaterials = { source };
			proxy.m_shadowCaster = RHIShadowCasterProxyPtr::Make();
			proxy.m_shadowCaster->m_worldAabb = mesh->m_bounds;
			RHIShadowMeshProxy caster;
			caster.m_mesh = mesh;
			caster.m_worldMatrix = glm::mat4(1);
			caster.m_renderQueueTag = "Masked"_h.GetHash();
			caster.m_customDepthMaterial = source;
			caster.m_customDepthShader = shader;
#if defined(__APPLE__)
			caster.m_materialTextureSamplers = { 0 };
#endif
			proxy.m_shadowCaster->m_meshes.Add(std::move(caster));
			RHISceneInstanceRecord record;
			record.m_producerKey = frame;
			record.m_mobility = EMobilityType::Static;
			record.m_worldMatrix = glm::mat4(1);
			record.m_worldBounds = mesh->m_bounds;
			record.m_topology = RHISceneProxyResourcePtr::Make(std::move(proxy));
			record.m_renderFlags = 1;
			auto scene = RHIScenePtr::Make();
			scene->AddInstance(record);
			snapshot.m_sceneVersions = TSharedPtr<TVector<RHISceneVersionPtr>>::Make();
			snapshot.m_sceneVersions->Add(scene->PublishVersion());
			for (uint32_t cascade = 0; cascade < 3; ++cascade)
			{
				RHIUpdateShadowMapCommand pass;
				pass.m_shadowType = EShadowType::PCF;
				pass.m_lighMatrixIndex = cascade;
				pass.m_lightMatrix = glm::mat4(1);
				pass.m_shadowMap = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
				pass.m_payloadCompletionToken = RHISubmissionCompletionTokenPtr::Make();
				snapshot.ForEachShadowCaster(EMobilityType::Static, [&](const RHIVisibleShadowCaster& item) { pass.m_meshList.Add(item); });
				snapshot.m_shadowMapsToUpdate.Add(std::move(pass));
			}
			snapshot.PrepareLods(glm::mat4(1), glm::mat4(1));
			const uint64_t submissionId = 178000 + frame;
			const uint64_t revision = RHIMaterial::BeginSubmissionVersionCapture(submissionId);
			snapshot.m_submissionContext->BeginSubmission(submissionId, 0, 0, revision);
			auto prepare = node->Prepare(graph, snapshot);
			Require(prepare.IsValid(), "shadow cache fixture must prepare its cascades");
			prepare->Run();
			prepare->Wait();
			Require(node->GetCascadeMaterial(snapshot.m_submissionContext, 0) == node->GetCascadeMaterial(snapshot.m_submissionContext, 1) &&
				node->GetCascadeMaterial(snapshot.m_submissionContext, 0) == node->GetCascadeMaterial(snapshot.m_submissionContext, 2),
				"three cascades must reuse one derivative for the same source generation");
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			node->Process(graph, upload, draw, snapshot);
			std::array<RHIBufferPtr, 3> pixels;
			for (uint32_t cascade = 0; cascade < 3; ++cascade)
				pixels[cascade] = ReadColor(draw, snapshot.m_shadowMapsToUpdate[cascade].m_shadowMap);
			if (frame == 1)
			{
				heldContext = snapshot.m_submissionContext;
				heldUpload = upload;
				heldDraw = draw;
				heldPixels = pixels;
				heldBindings = bindings.GetRawPtr();
			}
			else
			{
				CompleteCommands(upload, draw);
				checkPixels(pixels, frame % 2 ? 6 : 5);
			}
			RHIMaterial::EndSubmissionVersionCapture(submissionId);
			// Empty shells prevent address reuse without retaining their old bindings.
			source->SetBindings({});
			retiredSources.Add(source);
			driver->TrackResources_ThreadSafe();
			driver->CollectGarbage_RenderThread();
			Require(node->GetCachedMaterialCount() <= 6, "unused custom shadow derivatives must retire during preparation");
			Require(*liveBindings <= 7, "source binding resources must settle while one recorded submission remains held");
		}
		retiredSources.Clear();
		RHISceneViewSnapshot idle;
		idle.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		for (uint32_t frame = 25; frame <= 30; ++frame)
		{
			idle.m_frame = frame;
			idle.m_submissionContext->BeginSubmission(178000 + frame, 0);
			auto prepare = node->Prepare(graph, idle);
			if (prepare) { prepare->Run(); prepare->Wait(); }
			driver->TrackResources_ThreadSafe();
			driver->CollectGarbage_RenderThread();
			if (frame == 29) Require(node->GetCachedMaterialCount() == 1, "the latest derivative must survive the five-frame reuse window");
		}
		Require(node->GetCachedMaterialCount() == 0, "empty shadow frames must retire the remaining abandoned derivatives");
		Require(*liveBindings == 1, "only the held packet may retain a retired source binding after eviction");
		Require(node->GetCascadeMaterial(heldContext, 0)->GetVersion()->GetBindingsRaw() == heldBindings,
			"eviction must not replace the exact binding generation in an already prepared packet");
		CompleteCommands(heldUpload, heldDraw);
		checkPixels(heldPixels, 6);
		driver->TrackResources_ThreadSafe();
		heldUpload.Clear();
		heldDraw.Clear();
		heldContext.Clear();
		Require(*liveBindings == 0, "retired source bindings must release after the held submission finishes");
		std::cout << "Custom shadow cache paged=" << paged
			<< ": 24 generations, three-cascade reuse, bounded bindings, empty-frame eviction and retained pixels passed\n";
	}

	void DrawDepthPattern(RHICommandListPtr command, RHIFrameGraphPtr graph, RHIRenderTargetPtr depth, ShaderSetPtr shader, uint32_t frame)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const auto size = depth->GetExtent();
		const RenderState state{ true, true, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0,
			depth->GetMsaaSamples() != EMsaaSamples::Samples_1, EDepthCompare::Always };
		auto material = driver->CreateMaterial(driver->GetOrAddVertexDescription<VertexP3N3UV2C4>(), EPrimitiveTopology::TriangleList, state, shader);
		commands->ImageMemoryBarrier(command, depth, depth->GetDefaultLayout());
		commands->BeginRenderPass(command, TVector<RHITexturePtr>{}, TVector<RHITexturePtr>{}, depth,
			glm::ivec4(0, 0, size.x, size.y), glm::ivec2(0), true, glm::vec4(0), 0.0f, false, true);
		commands->BindMaterial(command, material);
		const auto mesh = graph->GetFullscreenNdcQuad();
		commands->BindVertexBuffer(command, mesh->m_vertexBuffer, 0);
		commands->BindIndexBuffer(command, mesh->m_indexBuffer, 0);
		commands->SetViewport(command, 0, 0, float(size.x), float(size.y), glm::vec2(0), glm::vec2(size), 0, 1);
		for (uint32_t sample = 0; sample < uint32_t(depth->GetMsaaSamples()); ++sample)
		{
			const glm::uvec2 pattern(sample, frame);
			commands->PushConstants(command, material, sizeof(pattern), &pattern);
			commands->DrawIndexed(command, 6, 1, uint32_t(mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t)),
				uint32_t(mesh->m_vertexBuffer->GetOffset() / mesh->m_vertexDescription->GetVertexStride()), 0);
		}
		commands->EndRenderPass(command);
	}

	void TestFullscreenUploadRetry()
	{
		auto graph = RHIFrameGraphPtr::Make();
		auto view = RHISceneViewPtr::Make();
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto before = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(before, true);
		commands->EndCommandList(before);
		auto input = driver->CreateWaitSemaphore();
		Require(driver->SubmitCommandList(before, RHIFencePtr::Make(), input), "the incoming graph dependency must submit");
		TVector<RHICommandListPtr> transfers, graphics;
		RHISemaphorePtr ready;
		Tests::RequireRejectedGraphicsSubmission([&]() { return graph->Process(view, transfers, graphics, input, ready); });
		Require(!graph->GetFullscreenNdcQuad() && ready == input && transfers.IsEmpty() && graphics.IsEmpty(),
			"a rejected quad upload must retain the incoming dependency without publishing an uninitialized mesh");
		Require(graph->Process(view, transfers, graphics, input, ready) && ready && ready != input,
			"the graph must retry its quad upload and publish the accepted dependency");
		auto after = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(after, true);
		commands->EndCommandList(after);
		auto finished = RHIFencePtr::Make();
		Require(driver->SubmitCommandList(after, finished, {}, ready) && finished->Wait(5000000000ull) == EFenceStatus::Finished,
			"the quad upload dependency must complete on the GPU");
		driver->TrackResources_ThreadSafe();
		const auto plane = graph->GetFullscreenNdcQuad();
		Require(plane && plane->IsReady() && plane->GetIndexCount() == 6, "the accepted quad must finish initialization");
		const auto submissions = driver->GetNumSubmittedCommandBuffers();
		Require(graph->Process(view, transfers, graphics, {}, ready) && !ready &&
			graph->GetFullscreenNdcQuad() == plane && driver->GetNumSubmittedCommandBuffers() == submissions,
			"later frames must reuse the quad without another upload or submission");
		std::cout << "FrameGraph fullscreen upload: native rejection, dependency-preserving retry and stable reuse passed\n";
	}

	void TestImportedRendering(const std::array<FileId, 2>& ids)
	{
		auto importer = App::GetSubmodule<FrameGraphImporter>();
		FrameGraphPtr rejected;
		Require(!importer->LoadFrameGraphAsset(ids[1]) &&
			!importer->LoadFrameGraph_Immediate(ids[1], rejected) && !rejected &&
			!importer->Instantiate_Immediate(ids[1], rejected) && !rejected,
			"invalid static declarations must report a failed load, not escape as an exception or partial graph");
		auto registry = App::GetSubmodule<AssetRegistry>();
		std::ifstream validFile(registry->GetAssetInfoPtr(ids[0])->GetAssetFilepath());
		std::ofstream repairedFile(registry->GetAssetInfoPtr(ids[1])->GetAssetFilepath());
		repairedFile << validFile.rdbuf();
		repairedFile.close();
		Require(static_cast<bool>(validFile) && static_cast<bool>(repairedFile) &&
			importer->LoadFrameGraph_Immediate(ids[1], rejected) && rejected,
			"a failed graph load must retry successfully after its temporary fixture is repaired");
		std::array<FrameGraphPtr, 2> instances;
		Require(importer->LoadFrameGraph_Immediate(ids[0], instances[0]) &&
			importer->Instantiate_Immediate(ids[0], instances[1]), "both graph instances must load");
		Require(instances[0]->GetRHI() != instances[1]->GetRHI() &&
			instances[0]->GetRHI()->GetSurface("Main") != instances[1]->GetRHI()->GetSurface("Main"),
			"instantiating the same asset must not share mutable targets or nodes");
		World cameraWorld("ImportedGraphCamera", 0);
		auto camera = cameraWorld.Instantiate("Camera")->AddComponent<CameraComponent>();
		cameraWorld.GetECS<CameraECS>()->Tick(0);
		auto cameraData = camera->GetData();
		cameraData.SetOwner({});
		cameraWorld.Clear();
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const auto fallback = driver->GetDefaultTexture();
		Require(fallback && fallback->GetFormat() == EFormat::R8G8B8A8_SRGB,
			"the Vulkan fallback fixture must expose its RGBA8 sRGB texel");
		auto fallbackRead = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		auto fallbackUpload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(fallbackRead, true);
		commands->BeginCommandList(fallbackUpload, true);
		auto fallbackPixels = ReadColor(fallbackRead, fallback);
		CompleteCommands(fallbackUpload, fallbackRead);
		const auto bytes = static_cast<const uint8_t*>(fallbackPixels->GetPointer());
		glm::vec4 fallbackValue(bytes[0] / 255.0f, bytes[1] / 255.0f, bytes[2] / 255.0f, bytes[3] / 255.0f);
		for (uint32_t i = 0; i < 3; ++i)
			fallbackValue[i] = fallbackValue[i] <= 0.04045f ? fallbackValue[i] / 12.92f :
				std::pow((fallbackValue[i] + 0.055f) / 1.055f, 2.4f);
		CaptureAttachments capture;
		for (uint32_t instanceIndex = 0; instanceIndex < instances.size(); ++instanceIndex)
		{
			auto graph = instances[instanceIndex]->GetRHI();
			auto node = graph->GetGraphNode("Composite").DynamicCast<PostProcessNode>();
			const auto output = graph->GetSurface("Main");
			const auto source = graph->GetSampler("ById");
			Require(source && source == graph->GetSampler("ByPath") && source == graph->GetSampler("Both") &&
				node->GetSampledAttachment("sourceSampler") == source,
				"path-only/id-only samplers must bind the same asset; fileId takes precedence over path");
			auto view = RHISceneViewPtr::Make();
			view->m_snapshots.Resize(1);
			auto& scene = view->m_snapshots[0];
			scene.m_camera = TUniquePtr<CameraData>::Make(cameraData);
			scene.m_bGlobalIlluminationEnabled = false;
			scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
			scene.m_rhiLightsData = driver->CreateShaderBindings();
			RHIRenderTargetPtr external;
			RHISurfacePtr externalSurface;
			VulkanDescriptorSetPtr previousDescriptor;
			for (uint32_t frame = 0; frame < 6; ++frame)
			{
				const bool published = frame != 0 && frame != 4;
				const float externalValue = float(instanceIndex + 1) * (frame >= 2 ? 0.25f : 0.125f);
				if (frame == 1 || frame == 2 || frame == 5)
				{
					externalSurface = frame == 2 ? driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT) : RHISurfacePtr{};
					external = externalSurface ? externalSurface->GetResolved() :
						driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
					auto setup = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(upload, true);
					commands->BeginCommandList(setup, true);
					ClearColor(setup, external, glm::vec4(externalValue));
					if (externalSurface && externalSurface->NeedsResolve())
						ClearColor(setup, externalSurface->GetTarget(), glm::vec4(8));
					CompleteCommands(upload, setup);
				}
				graph->SetSurface("DynamicInput", published ? externalSurface : RHISurfacePtr{});
				graph->SetSampler("DynamicInput", published && !externalSurface ? RHITexturePtr(external) : RHITexturePtr{});
				if (frame >= 2) graph->SetSampler("ById", external);
				scene.m_submissionContext->BeginSubmission(frame + 1, 0);
				TVector<RHICommandListPtr> transfers, graphics;
				RHISemaphorePtr ready;
				recordedColorCount = 0;
				Require(graph->Process(view, transfers, graphics, {}, ready),
					"the imported graph must prepare and record through its actual pass sequence");
				if (node->GetDrawCallStats().m_numBatches != 1)
					throw std::runtime_error("Imported graph draw mismatch: instance=" + std::to_string(instanceIndex) +
						" frame=" + std::to_string(frame) + " published=" + std::to_string(published) +
						" shaderReady=" + std::to_string(node->IsShaderReady()) +
						" batches=" + std::to_string(node->GetDrawCallStats().m_numBatches));
				{
					Require(recordedColorCount == 1 &&
						recordedColor.imageView == static_cast<VkImageView>(*output->GetTarget()->m_vulkan.m_imageView) &&
						recordedColor.resolveImageView == (output->NeedsResolve() ? static_cast<VkImageView>(*output->GetResolved()->m_vulkan.m_imageView) : VK_NULL_HANDLE),
						"imported rendering must retain the actual static Surface attachment");
					auto bindings = PostProcessNodeTestAccess::GetBindings(*node, scene);
					Require(bindings->GetOrAddShaderBinding("sourceSampler")->GetTextureBinding() == source &&
						bindings->GetOrAddShaderBinding("externalSampler")->GetTextureBinding() == (published ? RHITexturePtr(external) : fallback),
						"only external inputs may refresh, retaining the driver fallback when unpublished");
					if (frame == 3) Require(bindings->m_vulkan.m_descriptorSet == previousDescriptor,
						"unchanged external input must reuse its descriptor set");
					previousDescriptor = bindings->m_vulkan.m_descriptorSet;
				}
				for (size_t i = 0; i < transfers.Num(); ++i)
					for (const auto& command : { transfers[i], graphics[i] })
					{
						auto next = driver->CreateWaitSemaphore();
						Require(driver->SubmitCommandList(command, RHIFencePtr::Make(), next, ready), "imported graph commands must submit");
						ready = next;
					}
				auto readback = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(readback, true);
				const std::array images{ ReadColor(readback, output->GetResolved()), ReadColor(readback, output->GetTarget()) };
				commands->MemoryBarrier(readback, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
				commands->EndCommandList(readback);
				auto finished = RHIFencePtr::Make();
				Require(driver->SubmitCommandList(readback, finished, {}, ready) && finished->Wait(5000000000ull) == EFenceStatus::Finished,
					"imported graph readback must complete");
				const auto expected = glm::vec4(0.25f, 0, 0, 0.25f) + (published ? glm::vec4(externalValue) : fallbackValue);
				for (const auto& image : images)
					for (uint32_t pixel = 0; pixel < Side * Side; ++pixel)
					{
						const auto actual = static_cast<const glm::vec4*>(image->GetPointer())[pixel];
						for (uint32_t component = 0; component < 4; ++component)
							if (!std::isfinite(actual[component]) || std::abs(actual[component] - expected[component]) >= (published ? 0.00001f : 0.002f))
								throw std::runtime_error("Imported graph pixel mismatch: instance=" + std::to_string(instanceIndex) +
									" frame=" + std::to_string(frame) + " pixel=" + std::to_string(pixel) +
									" component=" + std::to_string(component) + " actual=" + std::to_string(actual[component]) +
									" expected=" + std::to_string(expected[component]));
					}
			}
		}
		FrameGraphImporterTestAccess::ReleaseInstance(*importer, instances[1]);
		std::cout << "FrameGraph imported rendering: two instances, six frames, static samplers and external replacement passed\n";
	}

	enum class DebugDepthInput { Default, Texture, Surface, DefaultSurface };
	enum class DepthDrawPath { DebugNode, SurfacePass, ImGuiNode };

	void TestDepthTestedDraw(ShaderSetPtr shader, ShaderSetPtr depthPattern, const std::array<ShaderSetPtr, 4>& depthReadback,
		EFormat format, bool colorSurface, DebugDepthInput depthInput, bool namedColor, bool namedDepth,
		DepthDrawPath path = DepthDrawPath::DebugNode)
	{
		auto driver = Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		auto commands = Renderer::GetDriverCommands();
		const bool msaa = colorSurface && VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
		const bool defaultDepth = depthInput == DebugDepthInput::Default || depthInput == DebugDepthInput::DefaultSurface;
		const bool depthSurface = depthInput == DebugDepthInput::Surface || depthInput == DebugDepthInput::DefaultSurface;
		auto graph = TRefPtr<TestGraph>::Make();
		FrameGraphNodePtr node = path == DepthDrawPath::ImGuiNode ?
			FrameGraphNodePtr(TRefPtr<RenderImGuiNode>::Make()) : TRefPtr<DebugDrawNode>::Make();
		if (namedColor) node->SetRHIResource_Unresolved("color", "DebugColor");
		if (namedDepth) node->SetRHIResource_Unresolved("depthStencil", defaultDepth ? "DepthBuffer" : "DebugDepth");
		CaptureAttachments capture;
		for (uint32_t frame = 0; frame < 3; ++frame)
		{
			if (frame == 2) node->Clear();
			auto color = colorSurface ? driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT) : RHISurfacePtr{};
			auto resolved = color ? color->GetResolved() : driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
			auto target = color ? color->GetTarget() : resolved;
			const auto usage = ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit;
			auto depth = driver->CreateRenderTarget(glm::ivec2(Side), 1, format, ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
			auto surface = depthSurface ? driver->CreateSurface(glm::ivec2(Side), 1, format,
				ETextureFiltration::Nearest, ETextureClamping::Clamp, usage) : RHISurfacePtr{};
			if (surface && !msaa) surface = RHISurfacePtr::Make(surface->GetResolved(), surface->GetResolved(), false);
			auto cachedDepth = msaa ? driver->GetOrAddMsaaFramebufferRenderTarget(format, glm::ivec2(Side)).StaticCast<RHIRenderTarget>() : depth;
			auto depthTarget = surface ? surface->GetTarget() : cachedDepth;
			if (color) graph->SetSurface("DebugColor", color);
			else graph->SetRenderTarget("DebugColor", resolved);
			if (surface) graph->SetSurface(defaultDepth ? "DepthBuffer" : "DebugDepth", surface);
			else graph->SetRenderTarget(defaultDepth ? "DepthBuffer" : "DebugDepth", depth);
			if (!namedColor) node->SetRHIResource("color", color ? RHIResourcePtr(color) : resolved);
			if (!namedDepth && !defaultDepth) node->SetRHIResource("depthStencil", surface ? RHIResourcePtr(surface) : depth);
			if (!defaultDepth) graph->SetRenderTarget("DepthBuffer", depth);
			const auto mesh = graph->GetFullscreenNdcQuad();
			const RenderState state(true, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, msaa, EDepthCompare::Greater);
			auto material = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shader);
			DebugContext::DrawSnapshot snapshot;
			snapshot.m_vertexBuffer = mesh->m_vertexBuffer;
			snapshot.m_indexBuffer = mesh->m_indexBuffer;
			snapshot.m_material = material;
			snapshot.m_numVertices = 6;
			RHISceneViewSnapshot scene;
			if (path != DepthDrawPath::SurfacePass)
			{
				auto secondaryTask = Tasks::CreateTaskWithResult<RHICommandListPtr>("Record debug attachment fixture",
					[snapshot, format, msaa]()
					{
						auto secondary = Renderer::GetDriver()->CreateCommandList(true, ECommandListQueue::Graphics);
						secondary->m_vulkan.m_commandBuffer->BeginSecondaryCommandList({ VK_FORMAT_R32G32B32A32_SFLOAT },
							static_cast<VkFormat>(format), VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
							VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT, msaa);
						DebugContext::DrawDebugMesh(secondary, glm::translate(glm::mat4(1), glm::vec3(0, 0, 0.5f)), snapshot, glm::ivec2(Side));
						Renderer::GetDriverCommands()->EndCommandList(secondary);
						return secondary;
					}, EThreadType::RHI);
				secondaryTask->Run();
				secondaryTask->Wait();
				Require(secondaryTask->IsFinished() && secondaryTask->GetResult()->GetRecordedDrawCallStats().m_numBatches == 1,
					"debug fixture must deliver a completed, nonempty secondary before Process");
				if (path == DepthDrawPath::ImGuiNode) scene.m_drawImGui = secondaryTask;
				else scene.m_debugDrawSecondaryCmdList = secondaryTask;
			}
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
			const glm::vec4 background(0.125f * (frame + 1));
			ClearColor(draw, target, background);
			if (msaa) ClearColor(draw, resolved, glm::vec4(-8));
			for (auto image : { depth, cachedDepth, surface ? surface->GetResolved() : depth })
			{
				commands->ImageMemoryBarrier(draw, image, EImageLayout::TransferDstOptimal);
				commands->ClearDepthStencil(draw, image, 0.9375f, 0);
				commands->ImageMemoryBarrier(draw, image, image->GetDefaultLayout());
			}
			DrawDepthPattern(draw, graph, depthTarget, depthPattern, frame);
			recordedColorCount = 0;
			recordedDepth = {};
			recordedRenderingFlags = 0;
			const auto batchesBefore = draw->GetRecordedDrawCallStats().m_numBatches;
			if (path == DepthDrawPath::SurfacePass)
			{
				commands->BeginRenderPass(draw, TVector<RHISurfacePtr>{ color }, surface ? surface->GetTarget() : depth,
					glm::ivec4(0, 0, Side, Side), glm::ivec2(0), false, glm::vec4(0), 0.0f, true);
				DebugContext::DrawDebugMesh(draw, glm::translate(glm::mat4(1), glm::vec3(0, 0, 0.5f)), snapshot, glm::ivec2(Side));
				commands->EndRenderPass(draw);
			}
			else
			{
				node->Process(graph, upload, draw, scene);
			}
			const bool oneDraw = path == DepthDrawPath::SurfacePass ?
				draw->GetRecordedDrawCallStats().m_numBatches == batchesBefore + 1 : node->GetDrawCallStats().m_numBatches == 1;
			const bool valid = recordedColorCount == 1 && oneDraw &&
				recordedRenderingFlags == (path == DepthDrawPath::SurfacePass ? 0 : VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) &&
				recordedColor.imageView == static_cast<VkImageView>(*target->m_vulkan.m_imageView) &&
				recordedColor.resolveImageView == (msaa ? static_cast<VkImageView>(*resolved->m_vulkan.m_imageView) : VK_NULL_HANDLE) &&
				recordedColor.resolveMode == (msaa ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE) &&
				recordedColor.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && recordedColor.storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
				recordedDepth.imageView == static_cast<VkImageView>(*depthTarget->m_vulkan.m_imageView) &&
				recordedDepth.resolveImageView == (msaa && !surface ? static_cast<VkImageView>(*depth->m_vulkan.m_imageView) : VK_NULL_HANDLE) &&
				recordedDepth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && recordedDepth.storeOp == VK_ATTACHMENT_STORE_OP_STORE;
			if (!valid)
			{
				commands->EndCommandList(upload);
				commands->EndCommandList(draw);
				upload->m_vulkan.m_commandBuffer->Reset();
				draw->m_vulkan.m_commandBuffer->Reset();
				throw std::runtime_error("Depth draw attachment mismatch: path=" + std::to_string(uint32_t(path)) + ", colorSurface=" + std::to_string(colorSurface) +
					", depth=" + std::to_string(uint32_t(depthInput)) + ", namedColor=" + std::to_string(namedColor) +
					", namedDepth=" + std::to_string(namedDepth) + ", colors=" + std::to_string(recordedColorCount) +
					", singleDraw=" + std::to_string(oneDraw));
			}
			auto resolvedPixels = ReadColor(draw, resolved);
			auto targetPixels = msaa ? ReadColor(draw, target) : resolvedPixels;
			auto depthPixels = ReadDepth(draw, depthTarget, depthReadback);
			const auto depthResolved = surface ? surface->GetResolved() : depth;
			auto depthResolvedPixels = depthResolved != depthTarget ? ReadDepth(draw, depthResolved, depthReadback) : depthPixels;
			CompleteCommands(upload, draw);
			const uint32_t samples = uint32_t(depthTarget->GetMsaaSamples());
			const auto actualDepth = static_cast<const glm::vec2*>(depthPixels->GetPointer());
			for (uint32_t y = 0; y < Side; ++y)
				for (uint32_t x = 0; x < Side; ++x)
				{
					uint32_t visibleSamples = 0;
					float resolvedDepth = 1;
					for (uint32_t sample = 0; sample < samples; ++sample)
					{
						const float expectedDepth = sample > 0 && (x + y + frame) % 5 == 0 ? 0.0f :
							float(1 + (x * 3 + y * 5 + frame * 7) % 13) / 16.0f - float(sample) / 32.0f;
						Require(actualDepth[(y * Side + x) * samples + sample] == glm::vec2(expectedDepth, 0),
							"the depth-tested draw must preserve every depth/stencil sample");
						if (sample == 0 || recordedDepth.resolveMode == VK_RESOLVE_MODE_MIN_BIT)
							resolvedDepth = glm::min(resolvedDepth, expectedDepth);
						if (x < Side / 2 && 0.5f > expectedDepth) ++visibleSamples;
					}
					if (surface && msaa) resolvedDepth = 0.9375f;
					Require(static_cast<const glm::vec2*>(depthResolvedPixels->GetPointer())[y * Side + x] == glm::vec2(resolvedDepth, 0),
						"implicit depth must resolve; an explicit Surface's unused resolve must remain untouched");
					const auto expected = glm::mix(background, glm::vec4(0.75f, 0.5f, 0.25f, 1), float(visibleSamples) / samples);
					for (auto image : { resolvedPixels, targetPixels })
					{
						const auto actual = static_cast<const glm::vec4*>(image->GetPointer())[y * Side + x];
						for (uint32_t component = 0; component < 4; ++component)
							Require(std::isfinite(actual[component]) && std::abs(actual[component] - expected[component]) < 0.00001f,
								"the draw must respect per-sample depth and preserve uncovered live color");
					}
				}
		}
		std::cout << (path == DepthDrawPath::SurfacePass ? "PrimarySurfaceDepth" : "DebugDraw") << " colorSurface=" << colorSurface << " depth=" << uint32_t(depthInput) <<
			" namedColor=" << namedColor << " namedDepth=" << namedDepth << " format=" << uint32_t(format) <<
			": three replacements, native attachments and per-sample occlusion passed\n";
	}

	enum class DepthInput { Default, Texture, Surface };

	void TestDepthHighZ(ShaderSetPtr patternShader, const std::array<ShaderSetPtr, 4>& readbackShaders,
		EFormat format, DepthInput input, bool namedInput, bool namedOutput, bool outputSurface)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<DepthHighZNode>::Make();
		if (namedInput) node->SetRHIResource_Unresolved("src", input == DepthInput::Default ? "DepthBuffer" : "CustomDepth");
		if (namedOutput) node->SetRHIResource_Unresolved("dst", "Pyramid");
		RHIRenderTargetPtr previousPyramid;
		for (uint32_t frame = 0; frame < 3; ++frame)
		{
			if (frame == 2) node->Clear();
			const glm::ivec2 inputSize = frame == 0 ? glm::ivec2(8) : frame == 1 ? glm::ivec2(9, 7) : glm::ivec2(5, 9);
			const glm::ivec2 outputSize = frame == 0 ? glm::ivec2(8) : frame == 1 ? glm::ivec2(7, 5) : glm::ivec2(3, 7);
			const uint32_t mipCount = frame == 0 ? 4 : 3;
			const auto usage = ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit;
			auto defaultDepth = driver->CreateRenderTarget(inputSize, 1, format, ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
			auto defaultTarget = VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT ?
				driver->GetOrAddMsaaFramebufferRenderTarget(format, inputSize).StaticCast<RHIRenderTarget>() : defaultDepth;
			graph->SetRenderTarget("DepthBuffer", defaultDepth);
			auto depthSurface = input == DepthInput::Surface ? driver->CreateSurface(inputSize, 1, format,
				ETextureFiltration::Nearest, ETextureClamping::Clamp, usage) : RHISurfacePtr{};
			auto resolved = depthSurface ? depthSurface->GetResolved() : input == DepthInput::Default ? defaultDepth :
				driver->CreateRenderTarget(inputSize, 1, format, ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
			auto depth = depthSurface ? depthSurface->GetTarget() : input == DepthInput::Default ? defaultTarget : resolved;
			auto surface = outputSurface ? driver->CreateSurface(outputSize, mipCount, EFormat::R32_SFLOAT) : RHISurfacePtr{};
			auto pyramid = surface ? surface->GetResolved() : driver->CreateRenderTarget(outputSize, mipCount, EFormat::R32_SFLOAT);
			if (depthSurface) graph->SetSurface("CustomDepth", depthSurface);
			else graph->SetRenderTarget("CustomDepth", resolved);
			if (surface) graph->SetSurface("Pyramid", surface);
			else graph->SetRenderTarget("Pyramid", pyramid);
			if (!namedInput && input != DepthInput::Default) node->SetRHIResource("src", depthSurface ? RHIResourcePtr(depthSurface) : depth);
			if (!namedOutput) node->SetRHIResource("dst", surface ? RHIResourcePtr(surface) : pyramid);
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
			for (auto texture : { defaultDepth, defaultTarget, resolved })
			{
				commands->ImageMemoryBarrier(draw, texture, EImageLayout::TransferDstOptimal);
				commands->ClearDepthStencil(draw, texture, 0.9375f, 0);
			}
			DrawDepthPattern(draw, graph, depth, patternShader, frame);
			auto inputReadback = ReadDepth(draw, depth, readbackShaders);
			ClearColor(draw, pyramid, glm::vec4(-8));
			if (surface && surface->NeedsResolve()) ClearColor(draw, surface->GetTarget(), glm::vec4(-4));
			graph->ResetCurrentDepthPyramids();
			Require(!graph->HasCurrentDepthPyramid(pyramid) && !graph->HasCurrentDepthPyramid(previousPyramid),
				"reset must invalidate the previous camera/frame depth pyramid");
			RHISceneViewSnapshot scene;
			node->Process(graph, upload, draw, scene);
			const bool published = graph->HasCurrentDepthPyramid(pyramid);
			TVector<RHIBufferPtr> mipReadbacks(mipCount);
			for (uint32_t mip = 0; mip < mipCount; ++mip)
			{
				auto texture = pyramid->GetMipLayer(mip);
				const auto size = texture->GetExtent();
				mipReadbacks[mip] = driver->CreateBuffer(size.x * size.y * sizeof(float), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
				std::fill_n(static_cast<float*>(mipReadbacks[mip]->GetPointer()), size.x * size.y, std::numeric_limits<float>::quiet_NaN());
				commands->ImageMemoryBarrier(draw, texture, EImageLayout::TransferSrcOptimal);
				commands->CopyImageToBuffer(draw, texture, mipReadbacks[mip]);
			}
			auto unusedTarget = surface && surface->NeedsResolve() ? ReadColor(draw, surface->GetTarget()) : RHIBufferPtr{};
			CompleteCommands(upload, draw);
			if (unusedTarget)
			{
				const auto values = static_cast<const float*>(unusedTarget->GetPointer());
				for (int32_t i = 0; i < outputSize.x * outputSize.y; ++i)
					Require(values[i] == -4.0f, "depth pyramid compute must not modify an unused MSAA output target");
			}
			const auto inputValues = static_cast<const glm::vec2*>(inputReadback->GetPointer());
			const uint32_t samples = uint32_t(depth->GetMsaaSamples());
			TVector<float> previous(inputSize.x * inputSize.y);
			for (int32_t y = 0; y < inputSize.y; ++y)
				for (int32_t x = 0; x < inputSize.x; ++x)
				{
					float minDepth = 1;
					for (uint32_t sample = 0; sample < samples; ++sample)
					{
						const float expected = sample > 0 && (x + y + frame) % 5 == 0 ? 0.0f :
							float(1 + (x * 3 + y * 5 + frame * 7) % 13) / 16.0f - float(sample) / 32.0f;
						const auto value = inputValues[(y * inputSize.x + x) * samples + sample].x;
						Require(std::isfinite(value) && std::abs(value - expected) < 0.00001f,
							"depth fixture must write each distinct sample, including uncovered zero samples");
						minDepth = glm::min(minDepth, expected);
					}
					previous[y * inputSize.x + x] = minDepth;
				}
			if (!published) throw std::runtime_error("DepthHighZ did not publish: input=" + std::to_string(uint32_t(input)) +
				" namedInput=" + std::to_string(namedInput) + " namedOutput=" + std::to_string(namedOutput));
			auto previousSize = inputSize;
			for (uint32_t mip = 0; mip < mipCount; ++mip)
			{
				const auto size = pyramid->GetMipLayer(mip)->GetExtent();
				TVector<float> expected(size.x * size.y);
				const auto actual = static_cast<const float*>(mipReadbacks[mip]->GetPointer());
				for (int32_t y = 0; y < size.y; ++y)
					for (int32_t x = 0; x < size.x; ++x)
					{
						float value = 1;
						const glm::ivec2 begin = glm::ivec2(x, y) * previousSize / size;
						const glm::ivec2 end = (glm::ivec2(x + 1, y + 1) * previousSize + size - 1) / size;
						for (int32_t sourceY = begin.y; sourceY < end.y; ++sourceY)
							for (int32_t sourceX = begin.x; sourceX < end.x; ++sourceX)
								value = glm::min(value, previous[sourceY * previousSize.x + sourceX]);
						expected[y * size.x + x] = value;
						if (!std::isfinite(actual[y * size.x + x]) || std::abs(actual[y * size.x + x] - value) > 0.00001f)
							throw std::runtime_error("DepthHighZ mip differs from conservative reduction: input=" + std::to_string(uint32_t(input)) +
								" namedInput=" + std::to_string(namedInput) + " namedOutput=" + std::to_string(namedOutput) + " mip=" + std::to_string(mip));
					}
				previous = std::move(expected);
				previousSize = size;
			}
			previousPyramid = pyramid;
		}
		std::cout << "DepthHighZ format=" << uint32_t(format) << " input=" << uint32_t(input) << " namedInput=" << namedInput
			<< " namedOutput=" << namedOutput << " outputSurface=" << outputSurface << ": all samples, even/odd mips, replacement and Clear passed\n";
	}

	void TestDepthSurfaceFactory(const std::array<ShaderSetPtr, 4>& shaders, EFormat format)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const uint32_t samples = uint32_t(VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples());
		const bool stencil = IsDepthStencilFormat(format);
		for (uint32_t frame = 0; frame < 2; ++frame)
		{
			auto surface = driver->CreateSurface(glm::ivec2(Side), 1, format, ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit);
			const auto target = surface->GetTarget();
			const auto resolved = surface->GetResolved();
			Require(surface->NeedsResolve() == (samples > 1) && (target != resolved) == (samples > 1),
				"depth surface target must alias its resolve only without MSAA");
			Require(uint32_t(target->GetMsaaSamples()) == samples && resolved->GetMsaaSamples() == EMsaaSamples::Samples_1,
				"depth surface must use the requested target and resolve sample counts");
			for (auto texture : { target, resolved })
			{
				const auto depthView = texture->GetDepthAspect();
				const auto stencilView = texture->GetStencilAspect();
				if (texture->GetDefaultLayout() != EImageLayout::DepthStencilAttachmentOptimal || !depthView || bool(stencilView) != stencil)
					throw std::runtime_error("DepthSurface factory format=" + std::to_string(uint32_t(format)) +
						" samples=" + std::to_string(uint32_t(texture->GetMsaaSamples())) +
						" layout=" + std::to_string(uint32_t(texture->GetDefaultLayout())) +
						" depthView=" + std::to_string(bool(depthView)) + " stencilView=" + std::to_string(bool(stencilView)));
				Require(texture->GetFormat() == format && texture->GetExtent() == glm::ivec2(Side) &&
					texture->m_vulkan.m_image->m_defaultLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
					"native depth image must retain its format, extent and attachment layout");
				for (const auto& aspect : { std::pair{ depthView, VK_IMAGE_ASPECT_DEPTH_BIT }, std::pair{ stencilView, VK_IMAGE_ASPECT_STENCIL_BIT } })
				{
					if (!aspect.first) continue;
					Require(aspect.first->m_vulkan.m_image == texture->m_vulkan.m_image &&
						aspect.first->m_vulkan.m_imageView->m_image == texture->m_vulkan.m_image &&
						aspect.first->m_vulkan.m_imageView->m_subresourceRange.aspectMask == aspect.second &&
						aspect.first->GetDefaultLayout() == texture->GetDefaultLayout(),
						"sampled depth/stencil views must reference the correct image and individual aspect");
				}
			}

			const glm::vec2 targetValue(0.25f + 0.125f * frame, stencil ? float(17 + frame) : 0.0f);
			const glm::vec2 resolveValue(0.75f + 0.125f * frame, stencil ? float(41 + frame) : 0.0f);
			const std::array textures{ target, resolved };
			const std::array expected{ targetValue, target == resolved ? targetValue : resolveValue };
			std::array<RHIBufferPtr, 2> readbacks;
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			for (uint32_t image = 0; image < textures.size(); ++image)
			{
				if (image == 1 && target == resolved)
				{
					readbacks[image] = readbacks[0];
					continue;
				}
				commands->ImageMemoryBarrier(draw, textures[image], EImageLayout::TransferDstOptimal);
				commands->ClearDepthStencil(draw, textures[image], expected[image].x, uint32_t(expected[image].y));
				readbacks[image] = ReadDepth(draw, textures[image], shaders);
			}
			CompleteCommands(upload, draw);
			for (uint32_t image = 0; image < textures.size(); ++image)
			{
				const auto values = static_cast<const glm::vec2*>(readbacks[image]->GetPointer());
				const uint32_t count = Side * Side * uint32_t(textures[image]->GetMsaaSamples());
				for (uint32_t i = 0; i < count; ++i)
					for (uint32_t component = 0; component < 2; ++component)
						Require(std::isfinite(values[i][component]) && std::abs(values[i][component] - expected[image][component]) < 0.00001f,
							"factory-created depth surfaces must retain independent target/resolve values in every depth/stencil sample");
			}
		}
		std::cout << "DepthSurface factory format=" << uint32_t(format) << " samples=" << samples
			<< ": layouts, aspect identity and all independent target/resolve samples passed\n";
	}

	void TestLinearizeDepth(EFormat format, bool depthIsSurface, bool targetIsSurface, bool namedDepth, bool namedTarget)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<LinearizeDepthNode>::Make();
		if (namedDepth) node->SetRHIResource_Unresolved("depthStencil", "Depth");
		if (namedTarget) node->SetRHIResource_Unresolved("target", "LinearDepth");
		CaptureAttachments capture;
		for (uint32_t frame = 0; frame < 3; ++frame)
		{
			if (frame == 2) node->Clear();
			const auto depthUsage = ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit;
			auto depthSurface = depthIsSurface ? driver->CreateSurface(glm::ivec2(Side), 1, format,
				ETextureFiltration::Nearest, ETextureClamping::Clamp, depthUsage) : RHISurfacePtr{};
			auto depth = depthSurface ? depthSurface->GetResolved() : driver->CreateRenderTarget(glm::ivec2(Side), 1, format,
				ETextureFiltration::Nearest, ETextureClamping::Clamp, depthUsage);
			auto colorSurface = targetIsSurface ? driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32_SFLOAT) : RHISurfacePtr{};
			auto color = colorSurface ? colorSurface->GetResolved() : driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32_SFLOAT);
			auto liveColor = colorSurface ? colorSurface->GetTarget() : color;
			const float nearPlane = 0.25f * (frame + 1);
			const float distance = 2.0f + 3.0f * frame;
			UboFrameData frameData{};
			frameData.m_view = glm::mat4(1);
			frameData.m_projection = glm::mat4(0);
			frameData.m_projection[0][0] = frameData.m_projection[1][1] = 1;
			frameData.m_projection[2][3] = -1;
			frameData.m_projection[3][2] = nearPlane;
			frameData.m_invProjection = glm::inverse(frameData.m_projection);
			frameData.m_cameraZNearZFar = glm::vec2(nearPlane, 1000);
			frameData.m_viewportSize = glm::ivec2(Side);
			RHISceneViewSnapshot scene;
			scene.m_frameBindings = driver->CreateShaderBindings();
			auto frameBinding = driver->AddBufferToShaderBindings(scene.m_frameBindings, "frameData", sizeof(frameData), 0, EShaderBindingType::UniformBuffer);
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			commands->UpdateShaderBinding(upload, frameBinding, &frameData, sizeof(frameData));
			commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
			ClearColor(draw, color, glm::vec4(-8));
			if (liveColor != color) ClearColor(draw, liveColor, glm::vec4(-4));
			commands->ImageMemoryBarrier(draw, depth, EImageLayout::TransferDstOptimal);
			commands->ClearDepthStencil(draw, depth, nearPlane / distance, 23);
			if (depthSurface && depthSurface->NeedsResolve())
			{
				commands->ImageMemoryBarrier(draw, depthSurface->GetTarget(), EImageLayout::TransferDstOptimal);
				commands->ClearDepthStencil(draw, depthSurface->GetTarget(), 0.75f, 41);
			}
			if (frame == 0)
			{
				recordedColorCount = 0;
				node->Process(graph, upload, draw, scene);
				Require(recordedColorCount == 0 && node->GetDrawCallStats().m_numBatches == 0,
					"linear depth must defer drawing until its resources are available");
			}
			if (depthSurface) graph->SetSurface("Depth", depthSurface);
			else graph->SetRenderTarget("Depth", depth);
			if (colorSurface) graph->SetSurface("LinearDepth", colorSurface);
			else graph->SetRenderTarget("LinearDepth", color);
			if (!namedDepth) node->SetRHIResource("depthStencil", depthSurface ? RHIResourcePtr(depthSurface) : depth);
			if (!namedTarget) node->SetRHIResource("target", colorSurface ? RHIResourcePtr(colorSurface) : color);
			recordedColorCount = 0;
			node->Process(graph, upload, draw, scene);
			auto readback = ReadColor(draw, color);
			auto liveReadback = liveColor != color ? ReadColor(draw, liveColor) : readback;
			CompleteCommands(upload, draw);
			if (node->GetDrawCallStats().m_numBatches != 1 || recordedColorCount != 1)
				throw std::runtime_error("LinearizeDepth did not draw: depthSurface=" + std::to_string(depthIsSurface) +
					" targetSurface=" + std::to_string(targetIsSurface) + " namedDepth=" + std::to_string(namedDepth) +
					" namedTarget=" + std::to_string(namedTarget));
			Require(recordedColor.imageView == static_cast<VkImageView>(*color->m_vulkan.m_imageView) &&
				recordedColor.resolveMode == VK_RESOLVE_MODE_NONE && recordedColor.resolveImageView == VK_NULL_HANDLE &&
				recordedColor.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && recordedColor.storeOp == VK_ATTACHMENT_STORE_OP_STORE,
				"linear depth must write the single-sample output directly without an implicit MSAA target or resolve");
			const auto values = static_cast<const float*>(readback->GetPointer());
			const auto liveValues = static_cast<const float*>(liveReadback->GetPointer());
			for (uint32_t i = 0; i < Side * Side; ++i)
			{
				Require(std::isfinite(values[i]) && std::abs(values[i] - distance) < 0.0001f,
					"linear depth pixels must reconstruct the known view-space distance from resolved depth");
				if (liveColor != color) Require(liveValues[i] == -4.0f, "linear depth must leave an unused multisampled output unchanged");
			}
		}
		std::cout << "LinearizeDepth format=" << uint32_t(format) << " depthSurface=" << depthIsSurface << " targetSurface=" << targetIsSurface
			<< " namedDepth=" << namedDepth << " namedTarget=" << namedTarget << ": resource replacement, Clear and reconstructed distances passed\n";
	}

	void TestClearDepth(const std::array<ShaderSetPtr, 4>& shaders, EFormat format, bool surfaceInput, bool late, bool implicitDepth = false)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<ClearNode>::Make();
		RHISceneViewSnapshot scene;
		const char* name = implicitDepth ? "DepthBuffer" : "Output";
		if (late) node->SetRHIResource_Unresolved("target", name);
		for (uint32_t frame = 0; frame < 2; ++frame)
		{
			auto resolved = driver->CreateRenderTarget(glm::ivec2(Side), 1, format, ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit);
			const bool msaa = VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
			auto target = (surfaceInput || implicitDepth) && msaa ?
				driver->GetOrAddMsaaFramebufferRenderTarget(format, glm::ivec2(Side), implicitDepth ? 0 : frame + 1).StaticCast<RHIRenderTarget>() : resolved;
			auto surface = surfaceInput ? RHISurfacePtr::Make(target, resolved, target != resolved) : RHISurfacePtr{};
			graph->SetRenderTarget(name, surface && frame == 1 && !implicitDepth ? RHIRenderTargetPtr{} : resolved);
			if (surface) graph->SetSurface(name, surface);
			if (!late) node->SetRHIResource("target", surface ? RHIResourcePtr(surface) : resolved);
			const glm::vec2 expected(frame == 0 ? 0.375f : 0.0f, IsDepthStencilFormat(format) ? float(17 + frame) : 0.0f);
			node->SetFloat("clearDepth", expected.x);
			node->SetFloat("clearStencil", expected.y);
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			for (auto texture : { target, resolved })
			{
				commands->ImageMemoryBarrier(draw, texture, EImageLayout::TransferDstOptimal);
				commands->ClearDepthStencil(draw, texture, 0.875f, 91);
			}
			node->Process(graph, upload, draw, scene);
			auto resolvedReadback = ReadDepth(draw, resolved, shaders);
			auto targetReadback = target != resolved ? ReadDepth(draw, target, shaders) : resolvedReadback;
			CompleteCommands(upload, draw);
			for (const auto& image : { std::pair{ resolved, resolvedReadback }, std::pair{ target, targetReadback } })
			{
				const auto values = static_cast<const glm::vec2*>(image.second->GetPointer());
				const uint32_t count = Side * Side * static_cast<uint32_t>(image.first->GetMsaaSamples());
				for (uint32_t i = 0; i < count; ++i)
					for (uint32_t component = 0; component < 2; ++component)
						if (!std::isfinite(values[i][component]) || std::abs(values[i][component] - expected[component]) > 0.00001f)
							throw std::runtime_error("Clear depth surface=" + std::to_string(surfaceInput) + " late=" + std::to_string(late) +
								" implicit=" + std::to_string(implicitDepth) + ": every depth/stencil sample must contain the current clear value");
			}
		}
		std::cout << "Clear depth format=" << uint32_t(format) << " surface=" << surfaceInput << " late=" << late << " implicit=" << implicitDepth
			<< ": two frames and all live/resolved depth/stencil samples passed\n";
	}

	void TestClearColor(bool surfaceInput, bool late)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<ClearNode>::Make();
		RHISceneViewSnapshot scene;
		if (late) node->SetRHIResource_Unresolved("target", "Output");
		for (uint32_t frame = 0; frame < 2; ++frame)
		{
			auto surface = surfaceInput ? driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT) : RHISurfacePtr{};
			auto resolved = surface ? surface->GetResolved() : driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
			auto target = surface ? surface->GetTarget() : resolved;
			// A named Surface must work without a second entry for its resolved texture.
			graph->SetRenderTarget("Output", surface && frame == 1 ? RHIRenderTargetPtr{} : resolved);
			if (surface) graph->SetSurface("Output", surface);
			if (!late) node->SetRHIResource("target", surface ? RHIResourcePtr(surface) : resolved);
			const glm::vec4 expected(0.125f * (frame + 1), 0.25f, 0.75f, 0.5f);
			node->SetVec4("clearColor", expected);
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			ClearColor(draw, resolved, glm::vec4(-16));
			if (target != resolved) ClearColor(draw, target, glm::vec4(-8));
			node->Process(graph, upload, draw, scene);
			auto resolvedReadback = ReadColor(draw, resolved);
			auto targetReadback = target != resolved ? ReadColor(draw, target) : resolvedReadback;
			CompleteCommands(upload, draw);
			for (const auto& readback : { resolvedReadback, targetReadback })
			{
				const auto pixels = static_cast<const glm::vec4*>(readback->GetPointer());
				for (uint32_t i = 0; i < Side * Side; ++i)
					for (uint32_t component = 0; component < 4; ++component)
						if (!std::isfinite(pixels[i][component]) || std::abs(pixels[i][component] - expected[component]) > 0.00001f)
							throw std::runtime_error("Clear color surface=" + std::to_string(surfaceInput) + " late=" + std::to_string(late) +
								": every live/resolved pixel must contain the current clear color");
			}
		}
		std::cout << "Clear color surface=" << surfaceInput << " late=" << late << ": two frames and both images passed\n";
	}

	void TestBlit(bool sourceIsSurface, bool destinationIsSurface, bool late, bool scaled = false)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<BlitNode>::Make();
		RHISceneViewSnapshot scene;
		scene.m_frameBindings = driver->CreateShaderBindings();
		if (late)
		{
			node->SetRHIResource_Unresolved("src", "Source");
			node->SetRHIResource_Unresolved("dst", "Destination");
		}
		CaptureAttachments capture;
		for (uint32_t frame = 0; frame < 2; ++frame)
		{
			const glm::ivec2 sourceExtent(scaled ? Side / 2 : Side);
			auto sourceSurface = sourceIsSurface ? driver->CreateSurface(sourceExtent, 1, EFormat::R32G32B32A32_SFLOAT) : RHISurfacePtr{};
			auto destinationSurface = destinationIsSurface ? driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT) : RHISurfacePtr{};
			auto source = sourceSurface ? sourceSurface->GetResolved() : driver->CreateRenderTarget(sourceExtent, 1, EFormat::R32G32B32A32_SFLOAT);
			auto destination = destinationSurface ? destinationSurface->GetResolved() : driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
			graph->SetRenderTarget("Source", source);
			graph->SetRenderTarget("Destination", destination);
			if (sourceSurface) graph->SetSurface("Source", sourceSurface);
			if (destinationSurface) graph->SetSurface("Destination", destinationSurface);
			if (!late)
			{
				node->SetRHIResource("src", sourceSurface ? RHIResourcePtr(sourceSurface) : source);
				node->SetRHIResource("dst", destinationSurface ? RHIResourcePtr(destinationSurface) : destination);
			}
			const bool sourceMsaa = sourceSurface && sourceSurface->NeedsResolve();
			const bool destinationMsaa = destinationSurface && destinationSurface->NeedsResolve();
			const bool copyLiveTarget = sourceMsaa && destinationMsaa && !scaled;
			const glm::vec4 sourceColor = frame == 0 ? glm::vec4(0.125f, 0.5f, 0.75f, 0.625f) : glm::vec4(0.5f, 0.25f, 0.125f, 1);
			const glm::vec4 liveColor = sourceColor + glm::vec4(0.25f, 0.5f, 0.75f, 0);
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
			ClearColor(draw, source, sourceColor);
			ClearColor(draw, destination, glm::vec4(-8));
			if (sourceMsaa) ClearColor(draw, sourceSurface->GetTarget(), liveColor);
			if (destinationMsaa) ClearColor(draw, destinationSurface->GetTarget(), glm::vec4(-16));
			recordedColorCount = 0;
			node->Process(graph, upload, draw, scene);
			auto resolvedReadback = ReadColor(draw, destination);
			auto targetReadback = destinationMsaa ? ReadColor(draw, destinationSurface->GetTarget()) : resolvedReadback;
			CompleteCommands(upload, draw);
			for (const auto& image : { std::pair{ resolvedReadback, sourceColor }, std::pair{ targetReadback, copyLiveTarget ? liveColor : sourceColor } })
			{
				const auto pixels = static_cast<const glm::vec4*>(image.first->GetPointer());
				for (uint32_t i = 0; i < Side * Side; ++i)
					for (uint32_t component = 0; component < 4; ++component)
						if (!std::isfinite(pixels[i][component]) || std::abs(pixels[i][component] - image.second[component]) > 0.00001f)
							throw std::runtime_error("Blit changed resolved/live target contents: sourceSurface=" + std::to_string(sourceIsSurface) +
								", destinationSurface=" + std::to_string(destinationIsSurface) + ", late=" + std::to_string(late) + ", scaled=" + std::to_string(scaled));
			}
			const bool shaderDraw = (destinationMsaa && !copyLiveTarget) || (!destinationSurface && scaled);
			Require(node->GetDrawCallStats().m_numBatches == (shaderDraw ? 1u : 0u), "Blit must retain direct-copy versus shader paths");
			if (shaderDraw)
			{
				auto target = destinationMsaa ? destinationSurface->GetTarget() : destination;
				Require(recordedColorCount == 1 && recordedColor.imageView == static_cast<VkImageView>(*target->m_vulkan.m_imageView) &&
					recordedColor.resolveMode == VK_RESOLVE_MODE_NONE && recordedColor.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
					recordedColor.storeOp == VK_ATTACHMENT_STORE_OP_STORE, "Blit fullscreen draw must write the selected target directly");
			}
		}
		std::cout << "Blit sourceSurface=" << sourceIsSurface << " destinationSurface=" << destinationIsSurface << " late=" << late <<
			" scaled=" << scaled << ": replaced inputs, both images and native descriptors passed\n";
	}

	void TestFog(bool colorIsSurface, bool late, EFormat depthFormat)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<AtmosphericFogNode>::Make();
		node->SetVec4("scattering", glm::vec4(0.75f, 0, 0.95f, 0));
		if (late)
		{
			node->SetRHIResource_Unresolved("color", "Color");
			node->SetRHIResource_Unresolved("depthSampler", "Depth");
		}
		CaptureAttachments capture;
		for (uint32_t frame = 0; frame < 3; ++frame)
		{
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
			auto surface = colorIsSurface ? driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT) : RHISurfacePtr{};
			auto color = surface ? surface->GetResolved() : driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
			auto target = surface ? surface->GetTarget() : color;
			const bool msaa = surface && surface->NeedsResolve();
			auto depth = driver->CreateRenderTarget(glm::ivec2(Side), 1, depthFormat,
				ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit);
			auto environment = driver->CreateCubemap(glm::ivec2(1), 1, EFormat::R32G32B32A32_SFLOAT);
			const glm::vec4 background(0.25f, 0.5f, 0.75f, 0.625f);
			const glm::vec4 radiance = frame == 0 ? glm::vec4(0.125f, 1, 0.25f, 1) : glm::vec4(1, 0.125f, 0.5f, 1);
			const float depthValue = frame == 2 ? 0 : frame == 0 ? 0.5f : 0.25f;
			const float density = frame == 0 ? 0.3f : 0.65f;
			ClearColor(draw, target, background);
			if (msaa) ClearColor(draw, color, glm::vec4(-8));
			ClearColor(draw, environment, radiance);
			commands->ImageMemoryBarrier(draw, depth, EImageLayout::TransferDstOptimal);
			commands->ClearDepthStencil(draw, depth, depthValue, 0);
			graph->SetRenderTarget("Color", color);
			if (surface) graph->SetSurface("Color", surface);
			graph->SetRenderTarget("Depth", depth);
			graph->SetSampler("g_irradianceCubemap", environment);
			if (!late)
			{
				node->SetRHIResource("color", surface ? RHIResourcePtr(surface) : color);
				node->SetRHIResource("depthSampler", depth);
			}
			node->SetVec4("fog", glm::vec4(density, 0, 0, 0));
			RHISceneViewSnapshot scene;
			scene.m_frameBindings = driver->CreateShaderBindings();
			UboFrameData frameData{};
			frameData.m_view = frameData.m_projection = frameData.m_invProjection = glm::mat4(1);
			frameData.m_viewportSize = glm::ivec2(Side);
			auto frameBinding = driver->AddBufferToShaderBindings(scene.m_frameBindings, "frameData", sizeof(frameData), 0, EShaderBindingType::UniformBuffer);
			commands->UpdateShaderBinding(upload, frameBinding, &frameData, sizeof(frameData));
			recordedColorCount = 0;
			node->Process(graph, upload, draw, scene);
			auto resolvedReadback = ReadColor(draw, color);
			auto targetReadback = msaa ? ReadColor(draw, target) : resolvedReadback;
			CompleteCommands(upload, draw);
			Require(node->GetDrawCallStats().m_numBatches == 1 && recordedColorCount == 1 &&
				recordedColor.imageView == static_cast<VkImageView>(*target->m_vulkan.m_imageView) &&
				recordedColor.resolveImageView == (msaa ? static_cast<VkImageView>(*color->m_vulkan.m_imageView) : VK_NULL_HANDLE) &&
				recordedColor.resolveMode == (msaa ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE) &&
				recordedColor.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && recordedColor.storeOp == VK_ATTACHMENT_STORE_OP_STORE,
				"Fog must blend into the selected live target and resolve it");
			for (auto readback : { resolvedReadback, targetReadback })
			{
				const auto pixels = static_cast<const glm::vec4*>(readback->GetPointer());
				for (uint32_t i = 0; i < Side * Side; ++i)
				{
					const glm::vec2 clip = (glm::vec2(i % Side, i / Side) + 0.5f) * (2.0f / Side) - 1.0f;
					const float distance = glm::length(glm::vec3(clip, depthValue));
					const float opacity = depthValue > 0 ? (std::min)(1.0f - std::exp(-density * distance), 0.95f) : 0;
					const glm::vec4 expected(glm::mix(glm::vec3(background), 0.75f * glm::vec3(radiance), opacity), background.a);
					for (uint32_t component = 0; component < 4; ++component)
						if (!std::isfinite(pixels[i][component]) || std::abs(pixels[i][component] - expected[component]) > 0.0001f)
							throw std::runtime_error("Fog pixel differs from homogeneous-medium source-over: frame=" + std::to_string(frame) +
								", colorSurface=" + std::to_string(colorIsSurface) + ", late=" + std::to_string(late) +
								", actual=" + std::to_string(pixels[i][component]) + ", expected=" + std::to_string(expected[component]));
				}
			}
		}
		std::cout << "Fog colorSurface=" << colorIsSurface << " late=" << late << " depth=" << static_cast<uint32_t>(depthFormat) <<
			": three frames, replaced inputs, sky/alpha and both images passed\n";
	}

	enum class LightCullingInput { Default, Bound, Named, NamedWithoutDefault };

	void TestLightCulling(LightCullingInput inputMode, bool surfaceInput, bool sameFlight)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<LightCullingNode>::Make();
		graph->GetGraph().Add(node);
		if (inputMode == LightCullingInput::Named || inputMode == LightCullingInput::NamedWithoutDefault)
			node->SetRHIResource_Unresolved("linearDepth", "SelectedDepth");
		World cameraWorld("LightCullingTestCamera", 0);
		auto cameraObject = cameraWorld.Instantiate("Camera");
		auto camera = cameraObject->AddComponent<CameraComponent>();
		cameraWorld.GetECS<CameraECS>()->Tick(0);
		auto cameraData = camera->GetData();
		cameraData.SetOwner({});
		cameraWorld.Clear();
		Require(cameraData.GetViewMatrix() == glm::mat4(1), "the culling fixture must use an identity camera view");
		struct Recording
		{
			RHISceneViewPtr view;
			RHIResourcePtr input;
			RHIRenderTargetPtr depth;
			RHIShaderBindingSetPtr bindings;
			VulkanDescriptorSetPtr descriptor, lightingDescriptor;
			Memory::VulkanBufferMemoryPtr indices, grid;
			TVector<RHICommandListPtr> transfers, graphics;
			RHISemaphorePtr ready;
		};
		std::array<Recording, 2> recordings;
		for (uint32_t i = 0; i < recordings.size(); ++i)
		{
			auto& recording = recordings[i];
			recording.view = RHISceneViewPtr::Make();
			recording.view->m_snapshots.Resize(1);
			auto& scene = recording.view->m_snapshots[0];
			scene.m_submissionContext = sameFlight && i ? recordings[0].view->m_snapshots[0].m_submissionContext : RHIRenderSubmissionContextPtr::Make();
			scene.m_cameraIndex = sameFlight ? i : 0;
			scene.m_rhiLightsData = driver->CreateShaderBindings();
			scene.m_camera = TUniquePtr<CameraData>::Make(cameraData);
			scene.m_bGlobalIlluminationEnabled = false;
		}
		// Initial, unchanged, replaced, larger, smaller, Clear, no lights, restored lights.
		const std::array extents{ glm::ivec2(8), glm::ivec2(8), glm::ivec2(8), glm::ivec2(35, 19),
			glm::ivec2(11, 5), glm::ivec2(17, 33), glm::ivec2(17, 33), glm::ivec2(17, 33) };
		for (uint32_t round = 0; round < extents.size(); ++round)
		{
			const auto extent = extents[round];
			const glm::ivec2 tiles = (extent + 15) / 16;
			const uint32_t tileCount = tiles.x * tiles.y;
			const float aspect = static_cast<float>(extent.x) / extent.y;
			auto lights = TSharedPtr<TVector<RHILightShaderData>>::Make();
			lights->Resize(2 + 2 * tileCount);
			(*lights)[0].m_type = 0;
			(*lights)[1].m_type = 1;
			(*lights)[1].m_bounds.x = 2;
			for (uint32_t tile = 0; tile < tileCount; ++tile)
			{
				const glm::ivec2 first = glm::ivec2(tile % tiles.x, tile / tiles.x) * 16;
				const glm::vec2 center = (glm::vec2(first) + glm::vec2(glm::min(first + 16, extent))) * 0.5f;
				const glm::vec2 ndc = center / glm::vec2(extent) * 2.0f - 1.0f;
				for (uint32_t i = 0; i < 2; ++i)
				{
					auto& light = (*lights)[2 + tile * 2 + i];
					const float depth = 12.0f * (i + 1);
					light.m_type = 1;
					light.m_worldPosition = glm::vec3(ndc.x * aspect * depth, -ndc.y * depth, -depth);
					light.m_bounds.x = 0.001f;
				}
			}
			if (round == 5) node->Clear();
			for (uint32_t i = 0; i < recordings.size(); ++i)
			{
				auto& recording = recordings[i];
				auto& scene = recording.view->m_snapshots[0];
				if (!sameFlight || i == 0) scene.m_submissionContext->BeginSubmission(round + 1, i);
				scene.m_cpuLightsData = lights;
				scene.m_totalNumLights = round == 6 ? 0 : static_cast<uint32_t>(lights->Num());
				scene.m_lightingRevision = round + 1;
				scene.m_camera->SetProjectionMatrix(Math::PerspectiveRH(glm::radians(90.0f), aspect, 0.1f, 200.0f));
				if (round != 1 && round != 6 && round != 7)
				{
					auto surface = surfaceInput ? driver->CreateSurface(extent, 1, EFormat::R32_SFLOAT) : RHISurfacePtr{};
					recording.depth = surface ? surface->GetResolved() : driver->CreateRenderTarget(extent, 1, EFormat::R32_SFLOAT);
					recording.input = surface ? RHIResourcePtr(surface) : recording.depth;
					auto setup = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(setup, true);
					ClearColor(setup, recording.depth, glm::vec4(12.0f * (i + 1)));
					if (surface && surface->NeedsResolve()) ClearColor(setup, surface->GetTarget(), glm::vec4(100));
					commands->EndCommandList(setup);
					auto initialized = RHIFencePtr::Make();
					Require(driver->SubmitCommandList(setup, initialized) && initialized->Wait(5000000000ull) == EFenceStatus::Finished,
						"light-culling depth fixture must initialize");
				}
				graph->SetRenderTarget("Main", recording.depth);
				const char* depthName = inputMode == LightCullingInput::Default ? "LinearDepth" : "SelectedDepth";
				graph->SetRenderTarget(depthName, surfaceInput ? RHIRenderTargetPtr{} : recording.depth);
				graph->SetSurface(depthName, recording.input.DynamicCast<RHISurface>());
				if (inputMode == LightCullingInput::Bound) node->SetRHIResource("linearDepth", recording.input);
				if (inputMode == LightCullingInput::Bound || inputMode == LightCullingInput::Named)
				{
					// Large enough for every tested dispatch, but deliberately wrong depth and dimensions.
					auto poison = driver->CreateRenderTarget(glm::ivec2(48), 1, EFormat::R32_SFLOAT);
					auto setup = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(setup, true);
					ClearColor(setup, poison, glm::vec4(100));
					commands->EndCommandList(setup);
					auto initialized = RHIFencePtr::Make();
					Require(driver->SubmitCommandList(setup, initialized) && initialized->Wait(5000000000ull) == EFenceStatus::Finished,
						"unrelated default depth must initialize");
					graph->SetRenderTarget("LinearDepth", poison);
				}
				recording.transfers.Clear();
				recording.graphics.Clear();
				Require(graph->Process(recording.view, recording.transfers, recording.graphics, {}, recording.ready),
					"light-culling test must run actual framegraph resource preparation");
			}
			for (uint32_t i = 0; i < recordings.size(); ++i)
			{
				auto& recording = recordings[i];
				auto& scene = recording.view->m_snapshots[0];
				for (size_t j = 0; j < recording.transfers.Num(); ++j)
					for (const auto& command : { recording.transfers[j], recording.graphics[j] })
					{
						auto next = driver->CreateWaitSemaphore();
						Require(driver->SubmitCommandList(command, RHIFencePtr::Make(), next, recording.ready), "light-culling commands must submit");
						recording.ready = next;
					}
				auto bindings = scene.m_rhiLightCullingData;
				Require(bindings.IsValid(), "light-culling bindings must be published to the view");
				const auto indices = *bindings->GetOrAddShaderBinding("culledLights")->m_vulkan.m_valueBinding->Get();
				const auto grid = *bindings->GetOrAddShaderBinding("lightsGrid")->m_vulkan.m_valueBinding->Get();
				auto indexReadback = driver->CreateBuffer(indices.m_size, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
				auto gridReadback = driver->CreateBuffer(grid.m_size, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
				auto read = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(read, true);
				read->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
				read->m_vulkan.m_commandBuffer->CopyBuffer(indices, *indexReadback->m_vulkan.m_buffer->Get(), indices.m_size);
				read->m_vulkan.m_commandBuffer->CopyBuffer(grid, *gridReadback->m_vulkan.m_buffer->Get(), grid.m_size);
				read->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
				commands->EndCommandList(read);
				auto finished = RHIFencePtr::Make();
				Require(driver->SubmitCommandList(read, finished, {}, recording.ready) && finished->Wait(5000000000ull) == EFenceStatus::Finished,
					"light-culling readback must complete");
				Require(indices.m_size >= tileCount * 128 * sizeof(uint32_t) && grid.m_size >= tileCount * 2 * sizeof(uint32_t),
					"light-culling storage must cover every tile of the selected input");
				const auto gpuGrid = static_cast<const glm::uvec2*>(gridReadback->GetPointer());
				const auto gpuIndices = static_cast<const uint32_t*>(indexReadback->GetPointer());
				for (uint32_t tile = 0; tile < tileCount; ++tile)
				{
					if (gpuGrid[tile] != glm::uvec2(tile * 128, round == 6 ? 0 : 3))
						throw std::runtime_error("LightCulling GPU grid mismatch: input=" + std::to_string(static_cast<uint32_t>(inputMode)) +
							", surface=" + std::to_string(surfaceInput) + ", sameFlight=" + std::to_string(sameFlight) +
							", round=" + std::to_string(round) + ", view=" + std::to_string(i) + ", tile=" + std::to_string(tile) +
							", count=" + std::to_string(gpuGrid[tile].y) + ", offset=" + std::to_string(gpuGrid[tile].x));
					if (round == 6) continue;
					std::array actual{ gpuIndices[tile * 128], gpuIndices[tile * 128 + 1], gpuIndices[tile * 128 + 2] };
					std::sort(actual.begin(), actual.end());
					Require(actual == std::array<uint32_t, 3>{ 0, 1, 2 + tile * 2 + i }, "GPU tile must contain only the directional, enclosing and matching-depth light");
				}
				Require(bindings->GetOrAddShaderBinding("linearDepth")->GetTextureBinding() == recording.depth,
					"light-culling sampler must use the selected resolved input");
				Require(*scene.m_rhiLightsData->GetOrAddShaderBinding("culledLights")->m_vulkan.m_valueBinding->Get() == indices &&
					*scene.m_rhiLightsData->GetOrAddShaderBinding("lightsGrid")->m_vulkan.m_valueBinding->Get() == grid,
					"downstream lighting must consume exactly the culling output allocations");
				if (round == 1 || round == 2 || round >= 4)
				{
					Require(bindings == recording.bindings && indices == recording.indices && grid == recording.grid,
						"completed flight/camera must reuse sufficient tile storage, including replaced inputs and smaller extents");
					Require(scene.m_rhiLightsData->m_vulkan.m_descriptorSet == recording.lightingDescriptor,
						"unchanged output allocations must not rebuild downstream lighting descriptors");
				}
				if (round == 1 || round == 6 || round == 7)
					Require(bindings->m_vulkan.m_descriptorSet == recording.descriptor, "unchanged culling inputs must not rebuild descriptors");
				recording.bindings = bindings;
				recording.descriptor = bindings->m_vulkan.m_descriptorSet;
				recording.lightingDescriptor = scene.m_rhiLightsData->m_vulkan.m_descriptorSet;
				recording.indices = indices;
				recording.grid = grid;
			}
			Require(recordings[0].indices != recordings[1].indices && recordings[0].grid != recordings[1].grid,
				"pending cameras/flights must own separate culling output ranges");
		}
		std::cout << "LightCulling input=" << static_cast<uint32_t>(inputMode) << " surface=" << surfaceInput << " sameFlight=" << sameFlight <<
			": eight frames, two pending views, native bindings and exact GPU tile lists passed\n";
	}

	void TestPostProcessFlights(const std::string& shader, bool sameFlight)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<PostProcessNode>::Make();
		node->SetString("shader", shader);
		node->SetRHIResource_Unresolved("color", "Output");
		node->SetFloat("data.gain", 0.5f);
		const glm::vec4 texel(0.125f, 0.25f, 0.5f, 1);
		auto texture = driver->CreateTexture(&texel, sizeof(texel), glm::ivec3(1), 1, ETextureType::Texture2D,
			EFormat::R32G32B32A32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp);
		node->SetRHIResource("sourceSampler", texture);
		struct Recording
		{
			RHISceneViewSnapshot scene;
			RHIShaderBindingSetPtr bindings;
			Memory::VulkanBufferMemoryPtr uniform;
			RHICommandListPtr upload, draw;
			RHIBufferPtr readback;
			RHISemaphorePtr ready;
			glm::vec4 expected;
		};
		std::array<Recording, 2> recordings;
		for (uint32_t i = 0; i < recordings.size(); ++i)
		{
			auto& scene = recordings[i].scene;
			scene.m_submissionContext = sameFlight && i ? recordings[0].scene.m_submissionContext : RHIRenderSubmissionContextPtr::Make();
			scene.m_cameraIndex = sameFlight ? i : 0;
			scene.m_frameBindings = driver->CreateShaderBindings();
			scene.m_rhiLightsData = driver->CreateShaderBindings();
		}
		for (uint32_t round = 0; round < 3; ++round)
		{
			if (round == 2) node->Clear();
			for (uint32_t i = 0; i < recordings.size(); ++i)
			{
				auto& recording = recordings[i];
				if (!sameFlight || i == 0) recording.scene.m_submissionContext->BeginSubmission(round + 1, i);
				const glm::vec4 tint(0.25f * (i + 1), 0.125f * (round + 1), 0.75f, 1);
				node->SetVec4("data.tint", tint);
				recording.expected = tint * 0.5f + texel;
				auto target = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
				graph->SetRenderTarget("Output", target);
				recording.upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				recording.draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(recording.upload, true);
				commands->BeginCommandList(recording.draw, true);
				commands->MemoryBarrier(recording.draw, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
					static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
				node->Process(graph, recording.upload, recording.draw, recording.scene);
				Require(node->GetDrawCallStats().m_numBatches == 1, "each pending post-process recording must contain a draw");
				auto bindings = PostProcessNodeTestAccess::GetBindings(*node, recording.scene);
				const auto uniform = *bindings->GetOrAddShaderBinding("data")->m_vulkan.m_valueBinding->Get();
				Require(round != 1 || (bindings == recording.bindings && uniform == recording.uniform),
					"completed post-process slots must reuse their bindings and uniform allocations");
				Require(round != 2 || (bindings != recording.bindings && uniform != recording.uniform),
					"clearing the node must recreate bindings for every retained frame/camera");
				recording.bindings = bindings;
				recording.uniform = uniform;
				recording.readback = ReadColor(recording.draw, target);
				commands->MemoryBarrier(recording.draw, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
				commands->EndCommandList(recording.upload);
				commands->EndCommandList(recording.draw);
			}
			// Both uploads finish before either draw: a shared uniform range would expose the last camera/frame's values.
			for (auto& recording : recordings)
			{
				recording.ready = driver->CreateWaitSemaphore();
				auto uploaded = RHIFencePtr::Make();
				Require(driver->SubmitCommandList(recording.upload, uploaded, recording.ready) && uploaded->Wait(5000000000ull) == EFenceStatus::Finished,
					"all pending parameter uploads must finish before either draw");
			}
			for (auto& recording : recordings)
			{
				auto finished = RHIFencePtr::Make();
				Require(driver->SubmitCommandList(recording.draw, finished, nullptr, recording.ready) && finished->Wait(5000000000ull) == EFenceStatus::Finished,
					"pending post-process draws must finish");
				const auto pixels = static_cast<const glm::vec4*>(recording.readback->GetPointer());
				for (uint32_t i = 0; i < Side * Side; ++i)
					for (uint32_t component = 0; component < 4; ++component)
						Require(std::isfinite(pixels[i][component]) && std::abs(pixels[i][component] - recording.expected[component]) <= 0.00001f,
							"pending post-process frames/cameras must keep their recorded parameters");
			}
			Require(recordings[0].bindings != recordings[1].bindings && recordings[0].uniform != recordings[1].uniform,
				"active frames/cameras must not share mutable bindings or uniform ranges");
		}
		std::cout << "PostProcess " << (sameFlight ? "two cameras" : "two flights") << ": six complete images, reused allocations and Clear passed\n";
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
		scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		uint64_t submissionId = 0;
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
			scene.m_submissionContext->BeginSubmission(++submissionId, 0);
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto graphics = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(graphics, true);
			commands->MemoryBarrier(graphics, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
			recordedColorCount = 0;
			const auto beforeUpload = upload->GetNumRecordedCommands();
			node->Process(graph, upload, graphics, scene);
			const auto uploadCommands = upload->GetNumRecordedCommands() - beforeUpload;
			const bool msaa = surface && surface->NeedsResolve();
			auto currentBindings = PostProcessNodeTestAccess::GetBindings(*node, scene);
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
			return uploadCommands;
		};

		Require(draw("first draw", output, {}, firstTexel) > 0, "the first draw must upload its parameters");
		tint = glm::vec4(0.75f, 0.25f, 0.5f, 1);
		gain = 0.25f;
		node->SetVec4("data.tint", tint);
		node->SetFloat("data.gain", gain);
		draw("mutated parameters", output, {}, firstTexel);
		node->SetRHIResource("sourceSampler", textureB);
		draw("replaced bound sampler", output, {}, secondTexel);
		const auto bindings = PostProcessNodeTestAccess::GetBindings(*node, scene);
		const auto descriptor = bindings->m_vulkan.m_descriptorSet;
		const auto revision = bindings->GetDescriptorRevision();
		Require(draw("unchanged frame", output, {}, secondTexel) == 0, "unchanged parameters must not record another upload");
		Require(PostProcessNodeTestAccess::GetBindings(*node, scene) == bindings && bindings->m_vulkan.m_descriptorSet == descriptor &&
			bindings->GetDescriptorRevision() == revision, "unchanged post-process inputs must not rebuild descriptors");
		const char* retries[] = { "retry discarded parameter upload", "retry rejected upload", "retry rejected draw" };
		for (uint32_t failure = 0; failure < 3; ++failure)
		{
			tint = glm::vec4(0.125f * (failure + 1), 0.5f, 0.75f, 1);
			gain = 0.75f;
			node->SetVec4("data.tint", tint);
			node->SetFloat("data.gain", gain);
			scene.m_submissionContext->BeginSubmission(++submissionId, 0);
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto graphics = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(graphics, true);
			const auto beforeUpload = upload->GetNumRecordedCommands();
			node->Process(graph, upload, graphics, scene);
			Require(node->GetDrawCallStats().m_numBatches == 1 && upload->GetNumRecordedCommands() > beforeUpload,
				"failure fixture must record a real draw and parameter upload");
			commands->EndCommandList(upload);
			commands->EndCommandList(graphics);
			if (failure == 1)
			{
				Tests::RequireRejectedNativeSubmission(upload);
			}
			else if (failure == 2)
			{
				auto uploaded = RHIFencePtr::Make();
				Require(driver->SubmitCommandList(upload, uploaded) && uploaded->Wait(5000000000ull) == EFenceStatus::Finished,
					"draw failure fixture must first finish its parameter upload");
				Tests::RequireRejectedNativeSubmission(graphics);
			}
			upload->m_vulkan.m_commandBuffer->Reset();
			graphics->m_vulkan.m_commandBuffer->Reset();
			scene.m_submissionContext->InvalidateSubmissionResources();
			Require(draw(retries[failure], output, {}, secondTexel) > 0, "invalidated parameters must be uploaded on retry");
		}
		auto pendingTexture = RHITexturePtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false);
		node->SetRHIResource("sourceSampler", pendingTexture);
		{
			scene.m_submissionContext->BeginSubmission(++submissionId, 0);
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto graphics = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(graphics, true);
			const auto beforeUpload = upload->GetNumRecordedCommands();
			node->Process(graph, upload, graphics, scene);
			Require(node->GetDrawCallStats().m_numBatches == 0 && upload->GetNumRecordedCommands() == beforeUpload,
				"an unavailable sampler must defer the draw and parameter upload");
			CompleteCommands(upload, graphics);
		}
		pendingTexture->m_vulkan = textureA->m_vulkan;
		draw("retry ready bound sampler", output, {}, firstTexel);
		node->SetRHIResource("sourceSampler", textureB);
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
			const auto sampler = PostProcessNodeTestAccess::GetBindings(*node, scene)->GetOrAddShaderBinding("sourceSampler");
			const RHITexturePtr expectedDepth = depth->GetDepthAspect() ? depth->GetDepthAspect() : RHITexturePtr(depth);
			Require(sampler->GetTextureBinding() == expectedDepth &&
				expectedDepth->m_vulkan.m_imageView->m_subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT,
				"depth-stencil sampling must select the actual depth-only view");
		}
		node->SetRHIResource("sourceSampler", textureA);
		node->SetString("shader", largeShader);
		draw("large reflected block and changed shader", output, {}, firstTexel);
		const auto largeBinding = PostProcessNodeTestAccess::GetBindings(*node, scene)->GetOrAddShaderBinding("data");
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
		const auto depthReadback = WriteDepthReadbackShader(workspace);
		const std::array depthPatterns{ WriteDepthPatternShader(workspace, EFormat::D32_SFLOAT), WriteDepthPatternShader(workspace, EFormat::D32_SFLOAT_S8_UINT) };
		const std::array debugShaders{ WriteDebugDrawShader(workspace, EFormat::D32_SFLOAT), WriteDebugDrawShader(workspace, EFormat::D32_SFLOAT_S8_UINT) };
		const auto highZInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeDepthHighZ.shader");
		Require(highZInfo != nullptr, "the production depth pyramid shader must exist");
		std::array<ShaderSetPtr, 3> highZShaders;
		for (uint32_t i = 0; i < highZShaders.size(); ++i)
		{
			const TVector<std::string> defines = i == 0 ? TVector<std::string>{} : TVector<std::string>{ i == 1 ? "DEPTH_INPUT" : "MSAA_DEPTH_INPUT" };
			Require(App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(highZInfo->GetFileId(), highZShaders[i], defines) && highZShaders[i]->IsReady(),
				"all production depth pyramid permutations must compile before recording");
		}
		const auto graphId = WriteGraph(workspace);
		const auto importedGraphIds = WriteImportedGraph(workspace);
		const auto mrtShader = WriteMrtShader(workspace);
		const auto customDepthShaders = WriteCustomDepthShader(workspace);
		ShaderSetPtr blitShader;
		const auto blitInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/Blit.shader");
		Require(blitInfo && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(blitInfo->GetFileId(), blitShader) && blitShader->IsReady(),
			"the production Blit shader must compile before recording");
		ShaderSetPtr fogShader;
		const auto fogInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/AtmosphericFog.shader");
		Require(fogInfo && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(fogInfo->GetFileId(), fogShader) && fogShader->IsReady(),
			"the production fog shader must compile before recording");
		ShaderSetPtr linearDepthShader;
		const auto linearDepthInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/LinearizeDepth.shader");
		Require(linearDepthInfo && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(linearDepthInfo->GetFileId(), linearDepthShader) && linearDepthShader->IsReady(),
			"the production linear-depth shader must compile before recording");
		ShaderSetPtr lightCullingShader;
		const auto lightCullingInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeLightCulling.shader");
		Require(lightCullingInfo && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(lightCullingInfo->GetFileId(), lightCullingShader) && lightCullingShader->IsReady(),
			"the production light-culling shader must compile before recording");
		App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
		TestDelayedImGuiProducer();
		auto task = Tasks::CreateTaskWithResult<std::string>("Post-process attachment and parameter contracts", [&]() -> std::string
			{
				try
				{
					TestImGuiSkippedAttachments();
					TestLinearizeDepthRegions(linearDepthShader);
					TestFullscreenUploadRetry();
					TestImportedRendering(importedGraphIds);
					TestGraphLoadFailures(workspace, importedGraphIds[0]);
					TestRelativeAttachmentDimensions(workspace);
					for (bool paged : { false, true })
						for (bool instanced : { false, true })
							for (bool skinned : { false, true })
								for (auto mobility : { EMobilityType::Static, EMobilityType::Stationary, EMobilityType::Dynamic })
									TestCustomDepthSilhouette(customDepthShaders[skinned ? 1 : 0], depthReadback, paged, instanced, skinned, mobility);
					for (bool paged : { false, true }) TestCustomShadowCache(customDepthShaders[0], paged);
					for (bool paged : { false, true })
						for (bool instanced : { false, true }) TestTransparentPacketOrder(customDepthShaders[0], paged, instanced);
					for (bool paged : { false, true })
						for (bool instanced : { false, true })
							for (bool masked : { false, true })
								for (auto mobility : { EMobilityType::Static, EMobilityType::Stationary, EMobilityType::Dynamic })
									TestDepthPacketParameters(customDepthShaders[0], paged, instanced, masked, mobility);
					TestStaticMsaaBindings(workspace);
					TestGraphMsaaTargets();
					TestGraphTargetLifetime();
					TestSceneMrt(mrtShader, true, false, false, true, true);
					for (bool late : { false, true })
						for (bool colorSurface : { true, false })
							for (bool motionSurface : { true, false }) TestSceneMrt(mrtShader, colorSurface, motionSurface, late, false, true);
					for (auto format : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT })
						for (bool named : { false, true })
							TestDepthTestedDraw(debugShaders[format == EFormat::D32_SFLOAT ? 0 : 1], depthPatterns[format == EFormat::D32_SFLOAT ? 0 : 1],
								depthReadback, format, false, DebugDepthInput::Texture, named, named, DepthDrawPath::ImGuiNode);
					for (auto format : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT })
						for (auto input : { DebugDepthInput::Texture, DebugDepthInput::Surface })
							TestDepthTestedDraw(debugShaders[format == EFormat::D32_SFLOAT ? 0 : 1], depthPatterns[format == EFormat::D32_SFLOAT ? 0 : 1],
								depthReadback, format, true, input, false, false, DepthDrawPath::SurfacePass);
					for (bool namedColor : { true, false })
						for (bool namedDepth : { false, true })
							for (bool colorSurface : { false, true })
								for (auto depthInput : { DebugDepthInput::Default, DebugDepthInput::Texture, DebugDepthInput::Surface, DebugDepthInput::DefaultSurface })
									for (auto format : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT })
										TestDepthTestedDraw(debugShaders[format == EFormat::D32_SFLOAT ? 0 : 1], depthPatterns[format == EFormat::D32_SFLOAT ? 0 : 1],
											depthReadback, format, colorSurface, depthInput, namedColor, namedDepth);
					for (auto inputMode : { LightCullingInput::Bound, LightCullingInput::Named,
						LightCullingInput::Default, LightCullingInput::NamedWithoutDefault })
						for (bool surfaceInput : { false, true })
							for (bool sameFlight : { false, true }) TestLightCulling(inputMode, surfaceInput, sameFlight);
					for (auto format : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT }) TestDepthSurfaceFactory(depthReadback, format);
					for (bool namedInput : { false, true })
						for (bool namedOutput : { false, true })
							for (auto input : { DepthInput::Default, DepthInput::Texture, DepthInput::Surface })
								for (bool outputSurface : { false, true })
									for (auto format : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT })
										TestDepthHighZ(depthPatterns[format == EFormat::D32_SFLOAT ? 0 : 1], depthReadback, format, input, namedInput, namedOutput, outputSurface);
					for (bool namedDepth : { false, true })
						for (bool namedTarget : { false, true })
							for (bool depthSurface : { false, true })
								for (bool targetSurface : { false, true })
									for (auto format : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT })
										TestLinearizeDepth(format, depthSurface, targetSurface, namedDepth, namedTarget);
					for (bool late : { false, true })
					{
						for (auto format : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT })
						{
							TestClearDepth(depthReadback, format, false, late);
							TestClearDepth(depthReadback, format, false, late, true);
							TestClearDepth(depthReadback, format, true, late);
						}
						for (bool surface : { false, true }) TestClearColor(surface, late);
						for (bool sourceSurface : { false, true })
							for (bool destinationSurface : { false, true }) TestBlit(sourceSurface, destinationSurface, late);
						TestBlit(false, false, late, true);
						TestBlit(true, true, late, true);
						for (bool colorSurface : { false, true })
							for (auto depthFormat : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT }) TestFog(colorSurface, late, depthFormat);
					}
					TestSceneMrt(mrtShader, true, false, false, true);
					for (bool late : { false, true })
						for (bool colorSurface : { false, true })
							for (bool motionSurface : { false, true }) TestSceneMrt(mrtShader, colorSurface, motionSurface, late);
					TestImportedBindings(graphId);
					TestPostProcessFlights(smallShader, false);
					TestPostProcessFlights(smallShader, true);
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
