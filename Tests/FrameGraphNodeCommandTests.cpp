#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Components/CameraComponent.h"
#include "Components/SkyComponent.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "ECS/LightingECS.h"
#include "FrameGraph/AtmosphericFogNode.h"
#include "FrameGraph/BlitNode.h"
#include "FrameGraph/ClearNode.h"
#include "FrameGraph/DepthHighZNode.h"
#include "FrameGraph/DepthPrepassNode.h"
#include "FrameGraph/DebugDrawNode.h"
#include "FrameGraph/DebugViewNode.h"
#include "FrameGraph/EnvironmentNode.h"
#include "FrameGraph/LinearizeDepthNode.h"
#include "FrameGraph/LightCullingNode.h"
#include "FrameGraph/ParticlesNode.h"
#include "FrameGraph/PostProcessNode.h"
#include "FrameGraph/MotionBlurNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "FrameGraph/RenderImGuiNode.h"
#include "FrameGraph/RenderSceneNode.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "FrameGraph/SkyNode.h"
#include "Math/Noise.h"
#include "GraphicsDriver/Vulkan/VulkanCommandBuffer.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "GraphicsDriver/Vulkan/VulkanPipeline.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Cubemap.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/PackedDrawCommands.hpp"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/VertexDescription.h"
#if defined(__APPLE__)
#include "Support/VulkanCapabilityOverrides.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <latch>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <glm/gtc/packing.hpp>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace Sailor::Tests
{
	void RunShaderInterfaceCommandTests();
	void RunPostProcessingCommandTests();
	void RunGIResolveCommandTests();
	void RunEditorReadbackCommandTests();
	void RequireRejectedNativeSubmission(RHICommandListPtr command);
	void RequireAttachmentInitializationRefusal(const std::function<void()>& record,
		uint32_t precedingSubmits = 0, VkResult error = VK_ERROR_OUT_OF_HOST_MEMORY);
	void RequireImageInitializationRefusal(const std::function<void()>& record,
		uint32_t precedingSubmits, uint32_t refusals, VkResult error);
	void RequireWorkerUploadRefusal(const std::function<void()>& load, VkResult error, bool transfer = false);
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
		static void WithoutSamplerMinmax(VulkanDevice& device, const std::function<void()>& test)
		{
			const bool supported = device.m_bSupportsSamplerFilterMinmax;
			auto saved = std::move(device.m_samplers);
			device.m_bSupportsSamplerFilterMinmax = false;
			try
			{
				device.m_samplers = TUniquePtr<VulkanSamplerCache>::Make(VulkanDevicePtr(&device));
				test();
			}
			catch (...)
			{
				device.m_samplers = std::move(saved);
				device.m_bSupportsSamplerFilterMinmax = supported;
				throw;
			}
			device.m_samplers = std::move(saved);
			device.m_bSupportsSamplerFilterMinmax = supported;
		}

		static void ExchangeBeginRendering(VulkanDevice& device, PFN_vkCmdBeginRenderingKHR& dispatch)
		{
			std::swap(device.pVkCmdBeginRendering, dispatch);
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
		using ShadowPrepassNode::m_pBlurHorizontalShader;
		using ShadowPrepassNode::m_pBlurVerticalShader;
		using ShadowPrepassNode::m_pBlurHorizontalMaterial;
		using ShadowPrepassNode::m_pBlurVerticalMaterial;
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
	std::function<void()> renderingObserver;
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
		if (renderingObserver) renderingObserver();
	}

	struct CaptureAttachments
	{
		explicit CaptureAttachments(std::function<void()> observer = {})
		{
			// No App::Start; the scheduler queues are drained before installing the observer.
			auto device = VulkanApi::GetInstance()->GetMainDevice();
			renderingObserver = std::move(observer);
			originalBeginRendering = CaptureRendering;
			FrameGraphNodeTestAccess::ExchangeBeginRendering(*device, originalBeginRendering);
			Require(originalBeginRendering != nullptr, "native dynamic-rendering dispatch must be available");
		}
		~CaptureAttachments()
		{
			renderingObserver = {};
			FrameGraphNodeTestAccess::ExchangeBeginRendering(*VulkanApi::GetInstance()->GetMainDevice(), originalBeginRendering);
		}
	};

	class PassCommandRecorder : public VulkanGraphicsDriver
	{
	public:
		void BeginDebugRegion(RHICommandListPtr command, StringHash title, const glm::vec4& color) override
		{
			BeginDebugRegion(std::move(command), title.ToString(), color);
		}

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

		bool RenderSecondaryCommandBuffers(RHICommandListPtr, TVector<RHICommandListPtr> secondary,
			const TVector<RHITexturePtr>& color, RHITexturePtr depth, glm::ivec4, glm::ivec2,
			bool, glm::vec4, float, bool, bool) override
		{
			Require(m_regions.size() == 2 && m_regions.back() == RenderImGuiNode::GetName().ToString(),
				"ImGui commands must be nested inside their own debug region");
			Require(secondary.Num() == 1 && color.Num() == 1 && color[0] && depth,
				"ImGui must record one complete secondary with both attachments");
			m_secondary = secondary[0];
			if (m_acceptSecondary) ++m_draws;
			return m_acceptSecondary;
		}

		void ImageMemoryBarrier(RHICommandListPtr, RHITexturePtr, EImageLayout) override {}
		void BindMaterial(RHICommandListPtr, RHIMaterialPtr) override {}
		void SetViewport(RHICommandListPtr, float, float, float, float, glm::vec2, glm::vec2, float, float) override {}
		void BindVertexBuffer(RHICommandListPtr, RHIBufferPtr, uint32_t) override {}
		void BindIndexBuffer(RHICommandListPtr, RHIBufferPtr, uint32_t, bool) override {}

		bool BeginRenderPass(RHICommandListPtr, const TVector<RHITexturePtr>& colors, RHITexturePtr depth,
			glm::ivec4, glm::ivec2, bool, glm::vec4, float, bool, bool) override
		{
			Require(m_regions.size() == 2 && m_regions.back() == LinearizeDepthNode::GetName().ToString() && !m_inRenderPass,
				"linear depth must begin its render pass inside the matching debug region");
			Require(colors.Num() == 1 && colors[0] && !depth, "linear depth must use one color target and sample depth separately");
			m_inRenderPass = true;
			++m_renderPasses;
			return true;
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
		bool m_acceptSecondary = true;
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
			Tasks::Task<RHICommandListPtr>("Delayed ImGui producer"_h, std::move(function), EThreadType::RHI),
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
			if (!finished && App::GetSubmodule<Tasks::Scheduler>()->IsRendererThread() && ++m_renderChecks == 2)
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
			node->SetRHIResource("color"_h, color);
			node->SetRHIResource("depthStencil"_h, depth);
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
			auto record = Tasks::CreateTaskWithResult<std::string>("Record delayed ImGui pass"_h, [&]() -> std::string
				{
					try
					{
						precedingPassRecorded = true;
						recorder.m_commands->BeginDebugRegion({}, "Frame"_h, {});
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
			if (!empty)
			{
				recorder.m_commands->m_acceptSecondary = false;
				recorder.m_commands->BeginDebugRegion({}, "Frame"_h, {});
				node->Process(graph, {}, {}, scene);
				recorder.m_commands->EndDebugRegion({});
				Require(recorder.m_commands->m_draws == 1 && node->GetDrawCallStats().m_numBatches == 0 &&
					node->GetDrawCallStats().m_numInstances == 0 && !recorder.m_commands->m_unbalanced && recorder.m_commands->m_regions.empty(),
					"a refused secondary pass must not retain previous-frame ImGui draw statistics or debug regions");
			}
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
		scene.m_drawImGui = Tasks::CreateTaskWithResult<RHICommandListPtr>("Unused ImGui producer"_h, []() { return RHICommandListPtr{}; });
		for (uint32_t mask = 0; mask < 3; ++mask)
		{
			node->SetRHIResource("color"_h, mask & 1 ? attachment : RHIRenderTargetPtr{});
			node->SetRHIResource("depthStencil"_h, mask & 2 ? attachment : RHIRenderTargetPtr{});
			recorder.m_commands->BeginDebugRegion({}, "Frame"_h, {});
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
				node->SetRHIResource_Unresolved("depthStencil"_h, "Depth"_h);
				node->SetRHIResource_Unresolved("target"_h, "LinearDepth"_h);
			}
			struct Case { bool depth, target, ready, bind; };
			for (const auto test : { Case{ true, true, true, true }, Case{ false, true, true, true },
				Case{ true, false, true, true }, Case{ false, false, true, true },
				Case{ true, true, false, true }, Case{ true, true, true, false }, Case{ true, true, true, true } })
			{
				if (named)
				{
					graph->SetRenderTarget("Depth"_h, test.depth ? depth : RHIRenderTargetPtr{});
					graph->SetRenderTarget("LinearDepth"_h, test.target ? target : RHIRenderTargetPtr{});
				}
				else
				{
					node->SetRHIResource("depthStencil"_h, test.depth ? depth : RHIRenderTargetPtr{});
					node->SetRHIResource("target"_h, test.target ? target : RHIRenderTargetPtr{});
				}
				node->m_pLinearizeDepthShader = test.ready ? shader : pendingShader;
				auto& commands = *recorder.m_commands;
				commands.m_acceptBindings = test.bind;
				commands.BeginDebugRegion({}, "Frame"_h, {});
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

	void TestShaderSourceLifetime(const std::filesystem::path& workspace)
	{
		const auto path = workspace / "Content" / "RetainedShaderSource.shader";
		YAML::Node shader;
		shader["glslCommon"] = "#version 450\n";
		shader["glslCompute"] = "layout(local_size_x = 1) in; void main() {}";
		std::ofstream output(path);
		output << shader;
		output.close();
		Require(static_cast<bool>(output), "shader source lifetime fixture must be written");
		const auto id = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::Render, EThreadType::RHI });
		auto* compiler = App::GetSubmodule<ShaderCompiler>();
		const auto source = compiler->LoadShaderAsset(id);
		Require(bool(source), "a registered shader source must load");
		auto compilation = compiler->CompileAllPermutations(id);
		Require(bool(compilation), "shader compilation must return its completion task");
		compilation->Wait();
		Require(compilation->GetResult(), "the source lifetime fixture must compile successfully");
		Require(bool(source), "a loaded shader source must remain alive after compilation evicts its cache entry");
		Require(source->ContainsCompute() && source->GetGlslComputeCode() == shader["glslCompute"].as<std::string>(),
			"the retained source must preserve its parsed shader contents");
		const auto reloaded = compiler->LoadShaderAsset(id);
		Require(reloaded && reloaded != source && reloaded->GetGlslComputeCode() == source->GetGlslComputeCode(),
			"the evicted source must reload independently while its previous owner remains valid");
		std::cout << "Shader source lifetime: retained source survives real compilation and cache eviction passed\n";
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
    float: [{gain: 0.75}]
    vec4: [{tint: [0.125, 0.5, 2.0, 0.75]}]
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
		auto node = graph->GetGraphNode("Bindings"_h);
		const auto surface = graph->GetSurface("StaticSurface"_h);
		const auto texture = graph->GetRenderTarget("StaticTexture"_h);
		Require(node && surface && texture, "imported graph must contain native surfaces and targets");
		Require(node->GetFloat("gain"_h) == 0.75f && node->GetVec4("tint"_h) == glm::vec4(0.125f, 0.5f, 2.0f, 0.75f),
			"imported nodes must preserve authored scalar and vector parameters independently of global values");
		Require(node->GetRHIResource("color"_h) == surface && node->GetRHIResource("sourceSampler"_h) == texture &&
			node->GetResolvedAttachment("color"_h) == surface->GetResolved(),
			"static graph binding must retain the surface rather than discard its multisampled target");
		Require(!node->GetRHIResource("externalSampler"_h, graph.GetRawPtr()), "unpublished external input must remain unresolved");
		graph->SetSampler("DynamicInput"_h, texture);
		Require(node->GetSampledAttachment("externalSampler"_h, graph.GetRawPtr()) == texture,
			"external graph input must resolve when its producer publishes it");
		graph->SetSampler("DynamicInput"_h, surface->GetResolved());
		Require(node->GetSampledAttachment("externalSampler"_h, graph.GetRawPtr()) == surface->GetResolved() &&
			node->GetRHIResource("sourceSampler"_h) == texture,
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

	TVector<FileId> TestGraphLoadFailures(const std::filesystem::path& workspace, FileId validId)
	{
		TVector<FileId> repairedGraphs;
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
				Require(graph->GetGraph().Num() == 3 && graph->GetGraph()[0]->GetTag() == "Clear"_h &&
					graph->GetGraph()[1]->GetTag() == "Composite"_h && graph->GetGraph()[2]->GetTag() == "Particles"_h &&
					graph->GetSampler("ById"_h) && graph->GetSampler("ByPath"_h) && graph->GetSampler("Both"_h),
					"a repaired graph must retain every ordered pass and static sampler, including ExperimentalParticles");
			}
			FrameGraphImporterTestAccess::ReleaseInstance(*importer, instance);
			repairedGraphs.Add(id);
			std::cout << "FrameGraph load failure: " << failure << ", rejection, retained output and repair retry passed\n";
		}
		return repairedGraphs;
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
				const auto expression = std::string(" \t") + name + "\t\r\n/\v " + std::to_string(divisor) + "\f\r\n";
				const auto expected = (std::max)(1u, static_cast<uint32_t>(size / divisor));
				Require(FrameGraphAsset::RenderTarget::ParseUintValue(expression) == expected,
					"relative dimensions must support positive fractions, whitespace, truncation and minimum-one extents");
			}
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
		Require(graph->GetRenderTarget("Render"_h)->GetExtent() == renderSize &&
			graph->GetSurface("Viewport"_h)->GetTarget()->GetExtent() == viewportSize &&
			graph->GetSurface("Viewport"_h)->GetResolved()->GetExtent() == viewportSize,
			"the importer must allocate texture and Surface images using their resolved declaration sizes");
		Require(graph->GetRenderTarget("Render"_h)->GetMipLevels() == ((std::max)(renderSize.x, renderSize.y) > 1 ? 2u : 1u),
			"the authored positive mip limit must reach the native render target");
		FrameGraphImporterTestAccess::ReleaseInstance(*importer, instance);
		std::cout << "FrameGraph relative dimensions: variables, divisors and image extents passed\n";
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
  - {name: Particles, width: 8, height: 8, format: R32G32B32A32_SFLOAT}
frame:
  - {name: Clear, tag: ClearColor, renderTargets: [{target: Color}]}
  - {name: RenderScene, tag: First, renderTargets: [{color: Color}, {motionVectors: Motion}]}
  - {name: RenderScene, tag: Second, renderTargets: [{color: Other}]}
  - {name: RenderScene, tag: External, renderTargets: [{color: Remote}, {motionVectors: Motion}]}
  - {name: Clear, tag: ClearParticles, renderTargets: [{target: Particles}]}
  - {name: ExperimentalParticles, tag: Particles, renderTargets: [{color: Particles}]}
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
			auto first = graph->GetGraphNode("First"_h);
			auto color = first->GetTargetAttachment("color"_h, graph.GetRawPtr());
			auto motion = first->GetTargetAttachment("motionVectors"_h, graph.GetRawPtr());
			Require(color->GetMsaaSamples() == samples && motion->GetMsaaSamples() == samples,
				"static MSAA attachments must be ready when the importer returns, before the first Process");
			Require(color == graph->GetGraphNode("ClearColor"_h)->GetTargetAttachment("target"_h, graph.GetRawPtr()) && color != motion &&
				color != graph->GetGraphNode("Second"_h)->GetTargetAttachment("color"_h, graph.GetRawPtr()),
				"static binding must share declared aliases without sharing independent outputs");
			Require(!graph->GetGraphNode("External"_h)->GetRHIResource("color"_h, graph.GetRawPtr()),
				"binding static targets must not require an unpublished external color");
			auto particles = graph->GetGraphNode("Particles"_h)->GetTargetAttachment("color"_h, graph.GetRawPtr());
			Require(particles->GetMsaaSamples() == samples && particles != color && particles != motion &&
				particles == graph->GetGraphNode("ClearParticles"_h)->GetTargetAttachment("target"_h, graph.GetRawPtr()),
				"imported particle output and Clear must share their own live MSAA image before Process");
			auto original = first->GetResolvedAttachment("color"_h, graph.GetRawPtr());
			graph->SetRenderTarget("Color"_h, graph->GetRenderTarget("Other"_h));
			Require(first->GetTargetAttachment("color"_h, graph.GetRawPtr()) == color &&
				first->GetResolvedAttachment("color"_h, graph.GetRawPtr()) == original,
				"static bindings must retain their image when a publication name is replaced");
		}
		Require(firstInstance->GetRHI()->GetGraphNode("First"_h)->GetTargetAttachment("color"_h, firstInstance->GetRHI().GetRawPtr()) !=
			secondInstance->GetRHI()->GetGraphNode("First"_h)->GetTargetAttachment("color"_h, secondInstance->GetRHI().GetRawPtr()),
			"separate imported graphs must not share implicit MSAA images");
		Require(firstInstance->GetRHI()->GetGraphNode("Particles"_h)->GetTargetAttachment("color"_h, firstInstance->GetRHI().GetRawPtr()) !=
			secondInstance->GetRHI()->GetGraphNode("Particles"_h)->GetTargetAttachment("color"_h, secondInstance->GetRHI().GetRawPtr()),
			"separate imported particle graphs must not share implicit MSAA images");
		FrameGraphImporterTestAccess::ReleaseInstance(*importer, firstInstance);
		FrameGraphImporterTestAccess::ReleaseInstance(*importer, secondInstance);
		std::cout << "FrameGraph static MSAA binding: import-time attachments, aliases and independent instances passed\n";
	}

	class DeclaredOutputsNode final : public BaseFrameGraphNode
	{
	public:
		std::span<const StringHash> GetMsaaOutputs() const override
		{
			static const StringHash outputs[] = { "albedo"_h, "velocity"_h };
			return outputs;
		}

		void Process(RHIFrameGraphPtr, RHICommandListPtr, RHICommandListPtr, const RHISceneViewSnapshot&) override {}
		void Clear() override {}
	};

	void TestGraphMsaaTargets()
	{
		auto& driver = Renderer::GetDriver();
		auto graph = TRefPtr<TestGraph>::Make();
		auto first = TRefPtr<RenderSceneNode>::Make();
		auto second = TRefPtr<RenderSceneNode>::Make();
		auto firstOutput = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		auto secondOutput = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		auto unused = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		graph->SetRenderTarget("External"_h, firstOutput);
		graph->SetRenderTarget("Unused"_h, unused);
		first->SetRHIResource_Unresolved("color"_h, "External"_h);
		second->SetRHIResource("color"_h, secondOutput);
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
		auto firstLive = first->GetTargetAttachment("color"_h, graph.GetRawPtr());
		auto secondLive = second->GetTargetAttachment("color"_h, graph.GetRawPtr());
		const bool msaa = App::GetSubmodule<Renderer>()->GetMsaaSamples() != EMsaaSamples::Samples_1;
		Require(firstLive != secondLive && firstLive->GetMsaaSamples() == App::GetSubmodule<Renderer>()->GetMsaaSamples(),
			"same-format color outputs in separate passes need distinct native targets");
		Require(first->GetResolvedAttachment("color"_h, graph.GetRawPtr()) == firstOutput &&
			second->GetSampledAttachment("color"_h, graph.GetRawPtr()) == secondOutput && graph->GetResource("Unused"_h) == unused,
			"MSAA preparation must retain resolve identities and leave unrelated targets alone");
		prepare();
		Require(first->GetTargetAttachment("color"_h, graph.GetRawPtr()) == firstLive &&
			second->GetTargetAttachment("color"_h, graph.GetRawPtr()) == secondLive, "unchanged outputs must reuse their MSAA targets");

		auto previous = first->GetRHIResource("color"_h, graph.GetRawPtr());
		graph->SetRenderTarget("External"_h, unused);
		prepare();
		Require(first->GetResolvedAttachment("color"_h, graph.GetRawPtr()) == unused &&
			first->GetTargetAttachment("color"_h, graph.GetRawPtr()) != firstLive &&
			second->GetTargetAttachment("color"_h, graph.GetRawPtr()) == secondLive, "external replacement must change only its own target");
		if (msaa) Require(previous.NumRefs() == 1, "the graph must release a retired MSAA surface");
		previous = first->GetRHIResource("color"_h, graph.GetRawPtr());
		graph->SetRenderTarget("External"_h, {});
		prepare();
		Require(!first->GetRHIResource("color"_h, graph.GetRawPtr()), "a withdrawn input must not retain its former surface");
		if (msaa) Require(previous.NumRefs() == 1, "withdrawal must release the graph's MSAA surface");
		graph->SetRenderTarget("External"_h, firstOutput);
		prepare();
		Require(first->GetResolvedAttachment("color"_h, graph.GetRawPtr()) == firstOutput, "a restored input must be usable again");
		graph->SetRenderTarget("External"_h, secondOutput);
		prepare();
		Require(first->GetTargetAttachment("color"_h, graph.GetRawPtr()) == secondLive, "aliases of one output must share its live target");

		auto declared = driver->CreateSurface(secondOutput);
		auto clear = TRefPtr<ClearNode>::Make();
		clear->SetRHIResource("target"_h, declared);
		graph->Clear();
		graph->GetGraph().Add(clear);
		graph->GetGraph().Add(second);
		prepare();
		Require(second->GetTargetAttachment("color"_h, graph.GetRawPtr()) == declared->GetTarget(),
			"a resolved output must reuse the Surface supplied to its clear pass");
		clear->SetVec4("clearColor"_h, glm::vec4(0.5f));
		prepare();
		Require(second->GetTargetAttachment("color"_h, graph.GetRawPtr()) == declared->GetTarget(),
			"changing a clear value must retain the prepared static attachments");
		second->SetRHIResource("color"_h, unused);
		prepare();
		Require(second->GetResolvedAttachment("color"_h, graph.GetRawPtr()) == unused,
			"a static resource setter must update an already prepared graph");
		auto alias = driver->CreateSurface(unused);
		graph->SetSurface("Alias"_h, alias);
		prepare();
		Require(second->GetTargetAttachment("color"_h, graph.GetRawPtr()) == alias->GetTarget(),
			"publishing an explicit Surface must update its prepared resolved alias");
		second->SetRHIResource_Unresolved("color"_h, "Changed"_h);
		graph->SetRenderTarget("Changed"_h, firstOutput);
		prepare();
		Require(second->GetResolvedAttachment("color"_h, graph.GetRawPtr()) == firstOutput,
			"switching a static input to external must join external refresh");
		second->SetRHIResource("color"_h, unused);
		prepare();
		graph->SetRenderTarget("Changed"_h, secondOutput);
		prepare();
		Require(second->GetResolvedAttachment("color"_h, graph.GetRawPtr()) == unused,
			"switching an external input back to static must stop following its old name");
		auto replacement = TRefPtr<RenderSceneNode>::Make();
		replacement->SetRHIResource("color"_h, firstOutput);
		graph->GetGraph()[1] = replacement;
		prepare();
		Require(replacement->GetTargetAttachment("color"_h, graph.GetRawPtr())->GetMsaaSamples() ==
			App::GetSubmodule<Renderer>()->GetMsaaSamples(), "replacing a node at the same index must rebuild its bindings");
		graph->GetGraph().RemoveLast();
		prepare();
		Require(graph->ResolveResource(firstOutput) == firstOutput, "removing the last producer must retire its implicit Surface");
		graph->Clear();
		Require(graph->ResolveResource(secondOutput) == secondOutput, "clearing the graph must discard its MSAA associations");
		std::cout << "FrameGraph MSAA ownership: independent outputs, reuse, replacement, withdrawal, aliases and Clear passed\n";

		auto color = driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		auto motion = RHISurfacePtr::Make(unused, unused, false);
		first->SetRHIResource("color"_h, color);
		first->SetRHIResource("motionVectors"_h, motion);
		second->SetRHIResource("color"_h, motion);
		graph->GetGraph().Add(first);
		graph->GetGraph().Add(second);
		graph->SetSurface("Motion"_h, motion);
		prepare();
		auto preparedMotion = graph->ResolveResource(unused).DynamicCast<RHISurface>();
		Require(preparedMotion && preparedMotion->GetTarget()->GetMsaaSamples() == color->GetTarget()->GetMsaaSamples() &&
			preparedMotion->GetResolved() == unused && second->GetTargetAttachment("color"_h, graph.GetRawPtr()) == unused,
			"promoting a secondary MRT must preserve an explicitly single-sample primary use of the same output");
		prepare();
		Require(graph->ResolveResource(unused) == preparedMotion, "unchanged mixed-sample bindings must reuse their prepared image");
		first->SetRHIResource("motionVectors"_h, {});
		prepare();
		Require(graph->ResolveResource(unused) == motion,
			"removing the MSAA consumer must restore the authored single-sample alias");
		if (msaa) Require(preparedMotion.NumRefs() == 1, "removing the MSAA consumer must retire its promoted Surface");
		graph->SetSurface("Motion"_h, {});
		second->SetRHIResource("color"_h, {});
		prepare();
		Require(graph->ResolveResource(unused) == unused, "withdrawing the last Surface owner must remove its alias");
		graph->Clear();
		std::cout << "FrameGraph mixed sample ownership: primary policy, secondary promotion, reuse, consumer removal and alias withdrawal passed\n";

		auto declaredOutputs = TRefPtr<DeclaredOutputsNode>::Make();
		declaredOutputs->SetRHIResource("albedo"_h, firstOutput);
		declaredOutputs->SetRHIResource_Unresolved("velocity"_h, "External"_h);
		graph->SetRenderTarget("External"_h, secondOutput);
		graph->GetGraph().Add(declaredOutputs);
		prepare();
		const auto primary = declaredOutputs->GetTargetAttachment("albedo"_h, graph.GetRawPtr());
		const auto secondary = declaredOutputs->GetTargetAttachment("velocity"_h, graph.GetRawPtr());
		Require(primary != secondary && primary->GetMsaaSamples() == App::GetSubmodule<Renderer>()->GetMsaaSamples() &&
			secondary->GetMsaaSamples() == primary->GetMsaaSamples(),
			"a node's declared outputs must prepare MSAA without a built-in node type or attachment name");
		Require(declaredOutputs->GetResolvedAttachment("albedo"_h, graph.GetRawPtr()) == firstOutput &&
			declaredOutputs->GetResolvedAttachment("velocity"_h, graph.GetRawPtr()) == secondOutput,
			"declared outputs must preserve their original resolve images");
		graph->SetRenderTarget("External"_h, unused);
		prepare();
		Require(declaredOutputs->GetTargetAttachment("albedo"_h, graph.GetRawPtr()) == primary &&
			declaredOutputs->GetTargetAttachment("velocity"_h, graph.GetRawPtr()) != secondary &&
			declaredOutputs->GetResolvedAttachment("velocity"_h, graph.GetRawPtr()) == unused,
			"only the replaced external declaration must change its prepared output");
		std::cout << "FrameGraph declared MSAA outputs: custom node/names, native targets, resolves and external replacement passed\n";
	}

	RHIBufferPtr ReadColor(RHICommandListPtr command, RHITexturePtr texture);
	RHIBufferPtr ReadDepth(RHICommandListPtr command, RHIRenderTargetPtr texture, const std::array<ShaderSetPtr, 4>& shaders);

	void TestSceneMrt(ShaderSetPtr shader, bool colorIsSurface, bool motionIsSurface, bool late, bool forceSingleSample = false,
		bool clearThroughNode = false, bool largerDepth = false, bool independentMotionSamples = false)
	{
		auto driver = Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		auto commands = Renderer::GetDriverCommands();
		const bool msaa = !forceSingleSample && VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
		auto graph = TRefPtr<TestGraph>::Make();
		const bool surfaces[] = { colorIsSurface, motionIsSurface };
		const StringHash names[] = { "Color"_h, "Motion"_h };
		const StringHash inputs[] = { "color"_h, "motionVectors"_h };
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
					const bool singleSample = independentMotionSamples && i == 1 ? !forceSingleSample : forceSingleSample;
					if (singleSample) surface = RHISurfacePtr::Make(surface->GetResolved(), surface->GetResolved(), false);
					resources[i] = surface;
					outputs[i] = surface->GetResolved();
					targets[i] = surface->GetTarget();
					graph->SetSurface(names[i], surface);
					if (independentMotionSamples && i == 1)
						targets[i] = msaa ? driver->GetOrAddMsaaFramebufferRenderTarget(surface->GetResolved()->GetFormat(), glm::ivec2(Side), i) :
							RHITexturePtr(surface->GetResolved());
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
		auto depth = driver->CreateRenderTarget(glm::ivec2(largerDepth ? Side * 2 : Side, Side), 1, EFormat::D32_SFLOAT_S8_UINT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit);
		const RHITexturePtr depthTarget = msaa ?
			driver->GetOrAddMsaaFramebufferRenderTarget(depth->GetFormat(), depth->GetExtent()) : RHITexturePtr(depth);
		graph->SetRenderTarget("SceneDepth"_h, depth);
		auto view = RHISceneViewPtr::Make();
		view->m_snapshots.Resize(1);
		auto& scene = view->m_snapshots[0];
		scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		scene.m_submissionContext->BeginSubmission(162, 0);
		scene.m_camera = TUniquePtr<CameraData>::Make();
		scene.m_frameBindings = driver->CreateShaderBindings();
		scene.m_rhiLightsData = driver->CreateShaderBindings();
		auto node = TRefPtr<SceneNode>::Make();
		node->SetString("Tag"_h, "MotionMrt");
		node->SetString("GPUCulling"_h, "false");
		for (uint32_t i = 0; i < 2; ++i)
		{
			if (late) node->SetRHIResource_Unresolved(inputs[i], names[i]);
			else node->SetRHIResource(inputs[i], resources[i]);
			if (clearThroughNode)
			{
				clears[i] = TRefPtr<ClearNode>::Make();
				if (late) clears[i]->SetRHIResource_Unresolved("target"_h, names[i]);
				else clears[i]->SetRHIResource("target"_h, resources[i]);
			}
		}
		if (late) node->SetRHIResource_Unresolved("depthStencil"_h, "SceneDepth"_h);
		else node->SetRHIResource("depthStencil"_h, depth);
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
			graph->SetRenderTarget("Main"_h, outputs[0]);
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
					graph->SetRenderTarget("Main"_h, outputs[0]);
					if (!late)
						for (uint32_t i = 0; i < 2; ++i) node->SetRHIResource(inputs[i], resources[i]);
				}
				for (uint32_t i = 0; i < 2; ++i)
				{
					const auto clearName = StringHash::Runtime("Clear" + names[i].ToString());
					graph->SetRenderTarget(clearName, outputs[i]);
					if (late) clears[i]->SetRHIResource_Unresolved("target"_h, frame % 2 ? clearName : names[i]);
					else clears[i]->SetRHIResource("target"_h, frame % 2 ? RHIResourcePtr(outputs[i]) : resources[i]);
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
					clears[i]->SetVec4("clearColor"_h, background[i] + glm::vec4(0.125f * frame));
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
					if (independentMotionSamples && i == 1)
					{
						const auto prepared = graph->ResolveResource(outputs[i]).DynamicCast<RHISurface>();
						targets[i] = !msaa ? RHITexturePtr(outputs[i]) : prepared && prepared->NeedsResolve() ?
							RHITexturePtr(prepared->GetTarget()) : driver->GetOrAddMsaaFramebufferRenderTarget(outputs[i]->GetFormat(), glm::ivec2(Side), i);
					}
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
			valid = valid && recordedDepth.imageView == static_cast<VkImageView>(*depthTarget->m_vulkan.m_imageView) &&
				recordedDepth.resolveImageView == (msaa ? static_cast<VkImageView>(*depth->m_vulkan.m_imageView) : VK_NULL_HANDLE);
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
				throw std::runtime_error("RenderScene MRT dropped or replaced a native color/motion/depth attachment: count=" +
					std::to_string(recordedColorCount) + ", batches=" + std::to_string(node->GetDrawCallStats().m_numBatches) +
					", independentMotion=" + std::to_string(independentMotionSamples) + ", singleSample=" + std::to_string(forceSingleSample) +
					", late=" + std::to_string(late));
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
		std::cout << (independentMotionSamples ? "MixedSamplesMRT " : largerDepth ? "LargeDepthMRT " : clearThroughNode ? "ClearSceneMRT " : "RenderScene MRT ") << (msaa ? "2x" : "1x") << " colorSurface=" << colorIsSurface <<
			" motionSurface=" << motionIsSurface << " late=" << late << " clearNode=" << clearThroughNode << ": " << frames << " frames / live and resolved images and native descriptors passed\n";
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
		const auto extent = texture->GetExtent();
		auto buffer = driver->CreateBuffer(size_t(extent.x) * extent.y * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
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

	void TestCubemapMipViews()
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		for (uint32_t levels : { 1u, 2u, 4u })
		{
			auto cube = driver->CreateCubemap(glm::ivec2(Side), levels, EFormat::R32G32B32A32_SFLOAT,
				ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit);
			Require(cube && cube->GetMipLevels() == levels && cube->m_vulkan.m_image->m_mipLevels == levels &&
				cube->m_vulkan.m_imageView->m_subresourceRange.levelCount == levels,
				"cubemap mip count must include its base level and match the native image/view");
			Require(cube->GetMipLevel(0) == cube, "cubemap level zero must retain the base resource");
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			struct Readback { RHIBufferPtr m_buffer; glm::vec4 m_color; uint32_t m_side; };
			TVector<Readback> readbacks;
			for (uint32_t mip = 0; mip < cube->GetMipLevels(); ++mip)
			{
				auto level = cube->GetMipLevel(mip);
				Require(level && level->GetMipLevels() == (mip == 0 ? levels : 1u) && level->GetMipLevel(0) == level,
					"a cubemap mip view must expose its own base and visible mip count");
				for (uint32_t face = 0; face < 6; ++face)
				{
					auto texture = level->GetFace(face);
					const auto& range = texture->m_vulkan.m_imageView->m_subresourceRange;
					const uint32_t side = Side >> mip;
					Require(texture == cube->GetFace(face, mip) && texture->GetExtent() == glm::ivec2(side) &&
						range.baseMipLevel == mip && range.levelCount == 1 && range.baseArrayLayer == face && range.layerCount == 1,
						"cubemap faces must address the requested native mip/layer and extent");
					const glm::vec4 color(float(face + 1) / 8, float(mip + 1) / 8, 0.25f, 1);
					ClearColor(draw, texture, color);
					readbacks.Add(Readback{ ReadColor(draw, texture), color, side });
				}
			}
			CompleteCommands(upload, draw);
			for (const auto& readback : readbacks)
			{
				const auto pixels = static_cast<const glm::vec4*>(readback.m_buffer->GetPointer());
				for (uint32_t i = 0; i < readback.m_side * readback.m_side; ++i)
					Require(pixels[i] == readback.m_color, "each cubemap face/mip must retain its own GPU-written pixels");
			}
			std::cout << "Cubemap mip views: levels=" << levels << ", base identity, native ranges and all six faces per mip passed\n";
		}
	}

	class SkyCommandProbe : public SkyNode
	{
	public:
		void LoadShaders()
		{
			auto registry = App::GetSubmodule<AssetRegistry>();
			auto compiler = App::GetSubmodule<ShaderCompiler>();
			auto load = [&](const char* path, ShaderSetPtr& shader, const TVector<std::string>& defines = {})
			{
				auto info = registry->GetAssetInfoPtr(path);
				Require(info && compiler->LoadShader_Immediate(info->GetFileId(), shader, defines) && shader->IsReady(),
					"the production sky shaders must compile before recording");
			};
			load("Shaders/Sky.shader", m_pSkyShader, { "FILL" });
			load("Shaders/Sky.shader", m_pSkyEnvShader);
			load("Shaders/Sky.shader", m_pSunShader, { "SUN" });
			load("Shaders/Sky.shader", m_pComposeShader, { "COMPOSE" });
			load("Shaders/Stars.shader", m_pStarsShader);
			load("Shaders/SunShafts.shader", m_pSunShaftsShader);
			load("Shaders/Blit.shader", m_pBlitShader);
		}

		bool LoadStars()
		{
			m_bStarsRequested = true;
			m_loadMeshTask = CreateStarsMesh();
			m_loadMeshTask->Wait();
			return static_cast<bool>(m_loadMeshTask->GetResult());
		}

		bool HasNoStarsOrPendingLoad() const { return m_bStarsRequested && !m_starsMesh && !m_loadMeshTask; }
		RHIMeshPtr GetStarsMesh() const { return m_starsMesh; }
		RHIMeshPtr WaitForStars()
		{
			if (!m_loadMeshTask) return m_starsMesh;
			m_loadMeshTask->Wait();
			return m_loadMeshTask->GetResult();
		}

		using SkyNode::AreCloudsResourcesReady;
		using SkyNode::ParseStarsMesh;

		void LoadClouds()
		{
			auto registry = App::GetSubmodule<AssetRegistry>();
			auto info = registry->GetAssetInfoPtr("Shaders/Sky.shader");
			Require(info && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(info->GetFileId(), m_pCloudsShader, { "CLOUDS" }) &&
				m_pCloudsShader->IsReady(), "the production cloud shader must compile");
			auto weather = registry->GetAssetInfoPtr("Textures/CloudsMap.png");
			Require(weather && App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(weather->GetFileId(), m_clouds),
				"the production cloud weather map must load");
			m_pCloudsMapTexture = m_clouds->GetRHI();
			m_bStarsRequested = true;
		}

		void PrepareCloudReadback()
		{
			const auto& profile = App::GetActiveGraphicsSettings();
			const auto viewport = App::GetMainWindow()->GetRenderArea();
			const auto render = Settings::ResolveRenderDimensions(viewport.x, viewport.y, profile.m_resolutionFactor);
			float multiplier = 1.0f;
#if defined(__APPLE__)
			multiplier = 0.5f;
#endif
			const auto extent = Settings::ResolveCloudsExtent(render.m_width, render.m_height, profile, multiplier);
			m_pCloudsTexture = Renderer::GetDriver()->CreateRenderTarget(glm::ivec2(extent.m_width, extent.m_height), 1,
				EFormat::R16G16B16A16_SFLOAT, ETextureFiltration::Linear, ETextureClamping::Clamp,
				ETextureUsageBit::Sampled_Bit | ETextureUsageBit::ColorAttachment_Bit |
				ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::TextureTransferSrc_Bit);
		}

		RHITexturePtr GetCloudsTexture() const { return m_pCloudsTexture; }
		ShaderSetPtr GetBlitShader() const { return m_pBlitShader; }
		std::array<Tasks::TaskPtr<TVector<uint8_t>>, 2> GetNoiseTasks() const { return { m_createNoiseLow, m_createNoiseHigh }; }
		std::array<RHITexturePtr, 2> GetNoiseTextures() const { return { m_pCloudsNoiseLowTexture, m_pCloudsNoiseHighTexture }; }
		void SetNoiseTextures(const std::array<RHITexturePtr, 2>& textures)
		{
			m_pCloudsNoiseLowTexture = textures[0];
			m_pCloudsNoiseHighTexture = textures[1];
		}
		void ResetNoise() { m_pCloudsNoiseLowTexture.Clear(); m_pCloudsNoiseHighTexture.Clear(); }


		void SetEmptyClouds()
		{
			auto& driver = Renderer::GetDriver();
			const uint8_t empty = 0;
			m_pCloudsNoiseLowTexture = driver->CreateTexture(&empty, sizeof(empty), glm::ivec3(1), 1,
				ETextureType::Texture3D, EFormat::R8_UNORM, ETextureFiltration::Linear, ETextureClamping::Repeat,
				ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit);
			m_pCloudsNoiseHighTexture = m_pCloudsNoiseLowTexture;
			m_pCloudsMapTexture = driver->GetDefaultTexture();
			m_skyParams.m_cloudsCoverage = 0.0f;
		}
	};

	void TestSkyWithoutStars(const std::filesystem::path& workspace, VkResult uploadError = VK_SUCCESS)
	{
		auto registry = App::GetSubmodule<AssetRegistry>();
		std::string colors;
		TVector<uint8_t> catalogue;
		Require(registry->ReadContentText("StarsColor.yaml", colors) && registry->ReadContentBinary("BSC5", catalogue),
			"the mounted engine star assets must be readable");
		const auto colorPath = workspace / "Content" / "StarsColor.yaml";
		const auto cataloguePath = workspace / "Content" / "BSC5";
		auto restore = [&]()
		{
			AssetRegistry::WriteTextFile(colorPath, colors);
			AssetRegistry::WriteBinaryFile(cataloguePath, catalogue);
		};
		restore();
		Require(registry->GetOrLoadFile(colorPath.string()) && registry->GetOrLoadFile(cataloguePath.string()),
			"temporary workspace star overrides must register");
		AssetRegistry::AssetReadLocation location;
		Require(registry->ResolveContentFile("BSC5", location) && std::filesystem::equivalent(location.m_physicalPath, cataloguePath) &&
			registry->ResolveContentFile("StarsColor.yaml", location) && std::filesystem::equivalent(location.m_physicalPath, colorPath),
			"star fixtures must use the workspace overrides, not engine fallback files");
		TRefPtr<SkyCommandProbe> node;
		for (uint32_t input = 0; input < 3; ++input)
		{
			restore();
			switch (input)
			{
			case 0: std::filesystem::remove(colorPath); break;
			case 1: std::filesystem::remove(cataloguePath); break;
			case 2:
			{
				auto empty = catalogue;
				empty.Resize(28);
				const int32_t count = 0;
				std::memcpy(empty.GetData() + 8, &count, sizeof(count));
				AssetRegistry::WriteBinaryFile(cataloguePath, empty);
				break;
			}
			}
			node = TRefPtr<SkyCommandProbe>::Make();
			Require(!node->LoadStars(), "failed or empty star assets must not allocate an RHI mesh");
		}
		restore();
		Require(TRefPtr<SkyCommandProbe>::Make()->LoadStars(), "a new sky node must load a repaired catalogue");
		RHIMeshPtr rejectedMesh;
		if (uploadError != VK_SUCCESS)
		{
			node = TRefPtr<SkyCommandProbe>::Make();
			Tests::RequireWorkerUploadRefusal([&]() { node->LoadStars(); }, uploadError, true);
			rejectedMesh = node->WaitForStars();
			Require(rejectedMesh && rejectedMesh->HasInitializationFailed() && !rejectedMesh->IsReady(),
				"native star upload refusal must retain terminal failure, not publish readiness");
		}
		node->LoadShaders();
		auto task = Tasks::CreateTaskWithResult<std::string>("Sky without a star mesh"_h, [&]() -> std::string
		{
			try
			{
				auto& driver = Renderer::GetDriver();
				auto commands = Renderer::GetDriverCommands();
				auto graph = TRefPtr<TestGraph>::Make();
				node->SetEmptyClouds();
				graph->SetSampler("g_ditherPatternSampler"_h, driver->GetDefaultTexture());
				graph->SetSampler("g_noiseSampler"_h, driver->GetDefaultTexture());
				auto color = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R16G16B16A16_SFLOAT);
				auto depth = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::D32_SFLOAT,
					ETextureFiltration::Nearest, ETextureClamping::Clamp,
					ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit |
					ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit);
				auto linearDepth = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32_SFLOAT);
				graph->SetRenderTarget("DepthBuffer"_h, depth);
				node->SetRHIResource("color"_h, color);
				node->SetRHIResource("linearDepth"_h, linearDepth);
				RHIMeshPtr recovered;
				for (uint32_t frame = 0; frame < 8; ++frame)
				{
					auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(upload, true);
					commands->BeginCommandList(draw, true);
					ClearColor(draw, color, glm::vec4(-1));
					ClearColor(draw, linearDepth, glm::vec4(1000));
					commands->ImageMemoryBarrier(draw, linearDepth, EImageLayout::ShaderReadOnlyOptimal);
					commands->ImageMemoryBarrier(draw, depth, EImageLayout::TransferDstOptimal);
					commands->ClearDepthStencil(draw, depth, 1.0f, 0);
					RHISceneViewSnapshot scene;
					scene.m_frameBindings = driver->CreateShaderBindings();
					UboFrameData frameData{};
					frameData.m_view = frameData.m_projection = frameData.m_invProjection = glm::mat4(1);
					frameData.m_viewportSize = glm::ivec2(Side);
					frameData.m_cameraZNearZFar = glm::vec2(0.1f, 1000);
					auto binding = driver->AddBufferToShaderBindings(scene.m_frameBindings, "frameData"_h, sizeof(frameData), 0, EShaderBindingType::UniformBuffer);
					commands->UpdateShaderBinding(upload, binding, &frameData, sizeof(frameData));
					node->Process(graph, upload, draw, scene);
					auto pixels = ReadColor(draw, color);
					CompleteCommands(upload, draw);
					Require(node->GetDrawCallStats().m_numBatches >= 5, "an absent star mesh must not stop sky, sun or cloud composition");
					if (uploadError == VK_SUCCESS)
					{
						Require(node->HasNoStarsOrPendingLoad(), "an empty catalogue must not retry every frame after its input is repaired");
					}
					else if (frame == 0)
					{
						Require(!node->GetStarsMesh(), "Sky must release the rejected star mesh before requesting its replacement");
					}
					else if (frame == 1)
					{
						recovered = node->WaitForStars();
						Require(recovered && recovered != rejectedMesh && !recovered->HasInitializationFailed(),
							"the same Sky node must retry the unchanged catalogue with a new mesh owner");
						driver->WaitIdle();
						driver->TrackResources_ThreadSafe();
						Require(recovered->IsReady(), "the replacement star mesh must become ready after native completion");
						const auto expected = SkyCommandProbe::ParseStarsMesh(colors, catalogue);
						const size_t vertexBytes = expected.m_first.Num() * sizeof(VertexP3C4);
						const size_t indexBytes = expected.m_second.Num() * sizeof(uint32_t);
						auto vertices = driver->CreateBuffer(vertexBytes, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
						auto indices = driver->CreateBuffer(indexBytes, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
						Require(driver->CopyBuffer_Immediate(recovered->m_vertexBuffer, vertices, vertexBytes) &&
							driver->CopyBuffer_Immediate(recovered->m_indexBuffer, indices, indexBytes),
							"the recovered star mesh must support complete vertex/index readback");
						const auto actual = static_cast<const VertexP3C4*>(vertices->GetPointer());
						for (size_t i = 0; i < expected.m_first.Num(); ++i)
							Require(actual[i].m_position == expected.m_first[i].m_position && actual[i].m_color == expected.m_first[i].m_color,
								"every recovered star position and color must match the authored catalogue");
						Require(std::memcmp(indices->GetPointer(), expected.m_second.GetData(), indexBytes) == 0,
							"every recovered star index must match the authored catalogue");
					}
					else
					{
						Require(node->GetStarsMesh() == recovered && node->GetDrawCallStats().m_numBatches >= 6,
							"later Sky frames must draw and reuse the recovered star mesh");
					}
					const auto values = static_cast<const uint16_t*>(pixels->GetPointer());
					for (uint32_t pixel = 0; pixel < Side * Side; ++pixel)
						for (uint32_t component = 0; component < 3; ++component)
						{
							const float value = glm::unpackHalf1x16(values[pixel * 4 + component]);
							Require(std::isfinite(value) && value >= 0, "the starless sky must replace every cleared pixel with finite radiance");
						}
				}
				SkyParameters captured;
				Require(graph->GetSampler("g_skyCubemap"_h) && node->GetEnvironmentSkyParams(captured),
					"the starless sky must still publish its completed environment capture");
				return {};
			}
			catch (const std::exception& error) { return error.what(); }
		}, EThreadType::Render);
		task->Run();
		task->Wait();
		if (!task->GetResult().empty()) throw std::runtime_error(task->GetResult());
		if (uploadError == VK_SUCCESS)
			std::cout << "Starless sky: missing/empty mounted assets, repaired mesh, eight frames without retry, native pixels and environment capture passed\n";
		else
			std::cout << "Star upload refusal " << uploadError << ": same-node recovery, all vertex/index values, eight sky frames and warm mesh reuse passed\n";
	}



	ShaderSetPtr WriteCullingIndexReadbackShader(const std::filesystem::path& workspace)
	{
		const auto path = workspace / "Content" / "CullingIndexReadback.shader";
		YAML::Node shader;
		shader["glslCommon"] = "#version 450\n";
		shader["glslCompute"] = "layout(local_size_x = 1) in;\n"
			"layout(set = 0, binding = 1, std430) readonly buffer Indices { uint values[]; } indices;\n"
			"layout(set = 1, binding = 0, std430) writeonly buffer Output { uint values[]; } outputValues;\n"
			"layout(push_constant) uniform Range { uint first; } range;\n"
			"void main() { uint i = gl_GlobalInvocationID.x; outputValues.values[i] = indices.values[range.first + i]; }\n";
		std::ofstream output(path);
		output << shader;
		output.close();
		Require(static_cast<bool>(output), "culling index readback shader must be written");
		const auto id = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		ShaderSetPtr result;
		Require(App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(id, result) && result->IsReady(),
			"culling index readback shader must compile");
		return result;
	}

	ShaderSetPtr WriteIndexedStorageShader(const std::filesystem::path& workspace, size_t stride)
	{
		const auto path = workspace / "Content" / ("IndexedStorage" + std::to_string(stride) + ".shader");
		YAML::Node shader;
		shader["glslCommon"] = "#version 450\n";
		shader["glslCompute"] = "layout(local_size_x = 1) in;\n"
			"struct Element { uvec4 words[" + std::to_string(stride / 16) + "]; };\n" +
			"layout(set = 0, binding = 0, std430) readonly buffer Source { Element data[]; } source;\n"
			"layout(set = 1, binding = 0, std430) writeonly buffer Output { uint words[]; } outputValue;\n"
			"layout(push_constant) uniform ReadIndex { uint baseIndex; } readIndex;\n"
			"void main() { uint word = gl_GlobalInvocationID.x; uint stride = " + std::to_string(stride / 4) + ";\n"
			"outputValue.words[word] = source.data[readIndex.baseIndex + word / stride].words[(word % stride) / 4][word % 4]; }\n";
		std::ofstream output(path);
		output << shader;
		output.close();
		Require(static_cast<bool>(output), "indexed storage shader must be written");
		const auto id = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		ShaderSetPtr result;
		Require(App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(id, result) && result->IsReady(),
			"indexed storage shader must compile before recording");
		return result;
	}

	void TestIndexedStorageBindings(ShaderSetPtr shader, size_t elementSize, bool suballocated)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto* nativeDriver = driver.DynamicCast<VulkanGraphicsDriver>();
		auto pipeline = nativeDriver->GetOrAddComputePipeline(shader->GetComputeShaderRHI());
		Require(pipeline && pipeline->IsCompiled(), "indexed storage needs a compiled compute pipeline");
		const size_t stride = (elementSize + 15) / 16 * 16;
		using Allocation = Memory::TManagedMemory<VulkanBufferMemoryPtr, VulkanBufferAllocator>;
		TVector<TSharedPtr<Allocation>> zeroBasedReservations;
		uint32_t relocatedBuffers = 0, offsetRanges = 0;
		for (uint32_t method = 0; method < 3; ++method)
			for (bool projected : { false, true })
			{
				const bool withOffset = method == 2;
				const uint32_t count = method == 0 ? 1 : 3;
				const size_t size = stride * count;
				auto blocker = driver->CreateShaderBindings();
				Require(static_cast<bool>(driver->AddSsboToShaderBindings(blocker, "blocker"_h, stride, 5, 0, false)),
					"indexed storage fixture must keep a neighboring allocation live");
				auto inputs = driver->CreateShaderBindings();
				const auto allocate = [&]()
				{
					return method == 0 ?
						driver->AddBufferToShaderBindings(inputs, "indexed"_h, elementSize, 0, EShaderBindingType::StorageBuffer) :
						driver->AddSsboToShaderBindings(inputs, "indexed"_h, elementSize, count, 0, withOffset);
				};
				auto binding = allocate();
				Require(binding && binding->m_vulkan.m_valueBinding, "indexed storage must allocate a managed range");
				// Establish a nonzero VkDeviceMemory placement instead of relying on earlier tests' allocations.
				for (uint32_t attempt = 0; attempt < 8192; ++attempt)
				{
					auto range = *binding->m_vulkan.m_valueBinding->Get();
					if ((*range.m_buffer->GetMemoryPtr()).m_offset != 0) break;
					zeroBasedReservations.Add(binding->m_vulkan.m_valueBinding);
					binding = allocate();
				}
				if (projected)
					Require(static_cast<bool>(driver->AddShaderBinding(inputs, blocker->GetOrAddShaderBinding("blocker"_h), "unused"_h, 31)),
						"an extra binding must require descriptor projection");
				const auto prepare = [&]()
				{
					auto range = *binding->m_vulkan.m_valueBinding->Get();
					const auto backing = *range.m_buffer->GetMemoryPtr();
					if (backing.m_offset > 0) ++relocatedBuffers;
					if (range.m_offset > 0) ++offsetRanges;
					const size_t expectedOffset = withOffset ? 0 : range.m_offset;
					if (size_t(binding->GetStorageInstanceIndex()) * stride != expectedOffset)
						throw std::runtime_error("indexed storage address must be relative to VkBuffer: method=" + std::to_string(method) +
							", index=" + std::to_string(binding->GetStorageInstanceIndex()) + ", stride=" + std::to_string(stride) +
							", buffer offset=" + std::to_string(range.m_offset) + ", device-memory offset=" + std::to_string(backing.m_offset));
					Require(range.m_size == size && binding->GetLayout().m_paddedSize == stride &&
						binding->m_vulkan.m_bBindSsboWithOffset == withOffset,
						"indexed storage must keep its padded stride and suballocation size");
					Require(nativeDriver->IsCompatible(pipeline->m_layout, { inputs })[0] != projected,
						"indexed storage must use the requested direct or projected descriptor path");
					auto sets = nativeDriver->GetCompatibleDescriptorSets(pipeline->m_layout, { inputs });
					Require(sets.Num() == 1 && sets[0] && sets[0]->IsCompiled() &&
						(sets[0] == inputs->m_vulkan.m_descriptorSet) != projected,
						"indexed storage must retain a complete native set");
					Require(nativeDriver->GetCompatibleDescriptorSets(pipeline->m_layout, { inputs })[0] == sets[0],
						"an unchanged indexed binding must reuse its native descriptor set");
					Require(sets[0]->m_descriptors.Num() == 1, "the selected shader uses only the source buffer");
					VkWriteDescriptorSet write{};
					sets[0]->m_descriptors[0]->Apply(write);
					Require(write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && write.pBufferInfo &&
						write.pBufferInfo->buffer == static_cast<VkBuffer>(*range.m_buffer) &&
						write.pBufferInfo->offset == (withOffset ? range.m_offset : 0) &&
						write.pBufferInfo->range == VK_WHOLE_SIZE,
						"descriptor offset and shader index must describe the same live storage range");
					return sets[0];
				};
				auto nativeA = prepare();
				const auto indexA = binding->GetStorageInstanceIndex();
				const auto rangeA = *binding->m_vulkan.m_valueBinding->Get();
				TWeakPtr<Allocation> weakA(binding->m_vulkan.m_valueBinding);
				const auto revisionA = inputs->GetDescriptorRevision();
				Require(!driver->AddBufferToShaderBindings(inputs, "indexed"_h, size, 0, EShaderBindingType::UniformBuffer) &&
					inputs->GetDescriptorRevision() == revisionA && inputs->GetOrAddShaderBinding("indexed"_h) == binding &&
					prepare() == nativeA,
					"a rejected indexed-buffer replacement must retain the published range and revision");
				auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(draw, true);
				const auto write = [&](uint32_t seed)
				{
					std::vector<uint32_t> values(size / sizeof(uint32_t));
					for (uint32_t i = 0; i < values.size(); ++i) values[i] = seed + i * 17;
					commands->UpdateShaderBinding(upload, binding, values.data(), size);
					std::fill(values.begin(), values.end(), 0xdeadc0deu);
				};
				struct Readback { RHIBufferPtr m_buffer; uint32_t m_seed; };
				std::vector<Readback> readbacks;
				const auto record = [&](VulkanDescriptorSetPtr input, uint32_t index, uint32_t seed)
				{
					auto output = driver->CreateBuffer(size, EBufferUsageBit::StorageBuffer_Bit | EBufferUsageBit::BufferTransferSrc_Bit,
						EMemoryPropertyBit::DeviceLocal);
					auto outputBindings = driver->CreateShaderBindings();
					Require(static_cast<bool>(driver->AddBufferToShaderBindings(outputBindings, output, "outputValue"_h, 0)),
						"indexed readback output must bind");
					auto native = draw->m_vulkan.m_commandBuffer;
					native->BindPipeline(pipeline);
					native->AddDependency(shader->GetComputeShaderRHI());
					native->BindDescriptorSet(pipeline->m_layout, { input, outputBindings->m_vulkan.m_descriptorSet }, VK_PIPELINE_BIND_POINT_COMPUTE);
					native->PushConstants(pipeline->m_layout, 0, sizeof(index), &index);
					native->Dispatch(static_cast<uint32_t>(size / sizeof(uint32_t)), 1, 1);
					native->MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
					auto result = driver->CreateBuffer(size, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
					native->CopyBuffer(*output->m_vulkan.m_buffer->Get(), *result->m_vulkan.m_buffer->Get(), size);
					readbacks.push_back({ result, seed });
				};
				write(0x12340000u);
				record(nativeA, indexA, 0x12340000u);
				Require(allocate() == binding && inputs->GetDescriptorRevision() == revisionA + 1,
					"indexed storage replacement must preserve binding identity and publish once");
				auto nativeB = prepare();
				const auto rangeB = *binding->m_vulkan.m_valueBinding->Get();
				Require(nativeA != nativeB && (rangeA.m_buffer != rangeB.m_buffer || rangeA.m_offset != rangeB.m_offset),
					"pending storage generations must use distinct allocations");
				write(0x56780000u);
				record(nativeB, binding->GetStorageInstanceIndex(), 0x56780000u);
				record(nativeA, indexA, 0x12340000u);
				nativeA.Clear();
				nativeB.Clear();
				binding.Clear();
				inputs.Clear();
				blocker.Clear();
				nativeDriver->CollectGarbage_RenderThread();
				Require(static_cast<bool>(weakA.TryLock()), "recorded indexed descriptors must retain their original allocation");
				CompleteCommands(upload, draw);
				for (auto& result : readbacks)
				{
					const auto* words = static_cast<const uint32_t*>(result.m_buffer->GetPointer());
					for (uint32_t i = 0; i < size / sizeof(uint32_t); ++i)
						Require(words[i] == result.m_seed + i * 17, "indexed GPU reads must return every original/replacement/retained word");
				}
				upload->m_vulkan.m_commandBuffer->Reset();
				draw->m_vulkan.m_commandBuffer->Reset();
				driver->TrackResources_ThreadSafe();
				nativeDriver->CollectGarbage_RenderThread();
				Require(!weakA.TryLock(), "completed indexed commands must release their managed reservation");
			}
		Require(relocatedBuffers > 0 && (!suballocated || offsetRanges > 0),
			"the indexed fixture must exercise nonzero buffer placements and nonzero suballocations");
		std::cout << "Indexed storage " << (suballocated ? "suballocated" : "default") << " stride " << stride <<
			": generic/indexed/offset allocations, direct/projected sets, rejection, A/B/A GPU words and owner release passed\n";
	}

	struct DescriptorReadback
	{
		RHIBufferPtr m_buffer;
		uint32_t m_seed;

		void Check() const
		{
			const auto* words = static_cast<const uint32_t*>(m_buffer->GetPointer());
			for (uint32_t i = 0; i < 8; ++i)
				Require(words[i] == m_seed + i * 17, "retained descriptors must read every word of their own generation");
		}
	};

	DescriptorReadback RecordDescriptorReadback(RHICommandListPtr draw, ShaderSetPtr shader,
		VulkanComputePipelinePtr pipeline, VulkanDescriptorSetPtr input, uint32_t seed)
	{
		auto& driver = Renderer::GetDriver();
		constexpr size_t size = 8 * sizeof(uint32_t);
		auto output = driver->CreateBuffer(size, EBufferUsageBit::StorageBuffer_Bit | EBufferUsageBit::BufferTransferSrc_Bit,
			EMemoryPropertyBit::DeviceLocal);
		auto bindings = driver->CreateShaderBindings();
		Require(static_cast<bool>(driver->AddBufferToShaderBindings(bindings, output, "outputValue"_h, 0)),
			"descriptor readback output must bind");
		auto native = draw->m_vulkan.m_commandBuffer;
		native->BindPipeline(pipeline);
		native->AddDependency(shader->GetComputeShaderRHI());
		native->BindDescriptorSet(pipeline->m_layout, { input, bindings->m_vulkan.m_descriptorSet }, VK_PIPELINE_BIND_POINT_COMPUTE);
		const uint32_t index = 0;
		native->PushConstants(pipeline->m_layout, 0, sizeof(index), &index);
		native->Dispatch(8, 1, 1);
		native->MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		auto result = driver->CreateBuffer(size, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
		std::memset(result->GetPointer(), 0xcd, size);
		native->CopyBuffer(*output->m_vulkan.m_buffer->Get(), *result->m_vulkan.m_buffer->Get(), size);
		return { result, seed };
	}

	class DescriptorPoolProbe final : public VulkanDescriptorPool
	{
	public:
		DescriptorPoolProbe(VulkanDevicePtr device, bool& destroyed) :
			VulkanDescriptorPool(std::move(device), 128, { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256 } }),
			m_destroyed(destroyed) {}
		~DescriptorPoolProbe() override { m_destroyed = true; }

		using VulkanDescriptorPool::CreatePool;
		VulkanDescriptorPoolPagePtr GetPage() const { return m_currentPage; }

	private:
		bool& m_destroyed;
	};

	void TestDescriptorPoolLifetime(ShaderSetPtr shader)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto* nativeDriver = driver.DynamicCast<VulkanGraphicsDriver>();
		auto pipeline = nativeDriver->GetOrAddComputePipeline(shader->GetComputeShaderRHI());
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& context = device->GetCurrentThreadContext();
		auto savedPool = context.m_descriptorPool;
		for (bool observePages : { true, false })
			for (bool projected : { false, true })
			{
				bool ownerDestroyed = false;
				auto pool = TRefPtr<DescriptorPoolProbe>::Make(device, ownerDestroyed);
				context.m_descriptorPool = pool;
				try
				{
					auto firstPage = observePages ? pool->GetPage() : VulkanDescriptorPoolPagePtr{};
					const VkDescriptorPool firstHandle = *pool;
					auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(upload, true);
					commands->BeginCommandList(draw, true);
					auto inputs = driver->CreateShaderBindings();
					const auto prepare = [&](uint32_t seed)
					{
						auto binding = driver->AddSsboToShaderBindings(inputs, "source"_h, 16, 2, 0, true);
						Require(static_cast<bool>(binding), "pool lifetime input must prepare");
						std::array<uint32_t, 8> words;
						for (uint32_t i = 0; i < words.size(); ++i) words[i] = seed + i * 17;
						commands->UpdateShaderBinding(upload, binding, words.data(), sizeof(words));
						if (projected)
							Require(static_cast<bool>(driver->AddShaderBinding(inputs, binding, "unused"_h, 31)),
								"pool lifetime projection must have an extra binding");
						auto sets = nativeDriver->GetCompatibleDescriptorSets(pipeline->m_layout, { inputs });
						Require(sets.Num() == 1 && sets[0] && sets[0]->IsCompiled() &&
							(projected ? sets[0] != inputs->m_vulkan.m_descriptorSet : sets[0] == inputs->m_vulkan.m_descriptorSet),
							"pool lifetime must exercise the requested direct or projected path");
						const auto warm = nativeDriver->GetCompatibleDescriptorSets(pipeline->m_layout, { inputs });
						Require(warm.Num() == 1 && warm[0] == sets[0],
							"an unchanged descriptor request must reuse the native set");
						return sets[0];
					};
					auto first = prepare(0x12340000u);
					const auto firstRead = RecordDescriptorReadback(draw, shader, pipeline, first, 0x12340000u);
					Require(pool->CreatePool() == VK_SUCCESS && static_cast<VkDescriptorPool>(*pool) != firstHandle,
						"page replacement must create a different live native pool");
					auto secondPage = observePages ? pool->GetPage() : VulkanDescriptorPoolPagePtr{};
					auto second = prepare(0x56780000u);
					const auto secondRead = RecordDescriptorReadback(draw, shader, pipeline, second, 0x56780000u);
					const auto retainedRead = RecordDescriptorReadback(draw, shader, pipeline, first, 0x12340000u);
					first.Clear();
					second.Clear();
					inputs.Clear();
					nativeDriver->CollectGarbage_RenderThread();
					context.m_descriptorPool = savedPool;
					Require(pool.NumRefs() == 1, "compiled sets must retain their page, not the allocating pool owner");
					pool.Clear();
					Require(ownerDestroyed, "the allocating pool owner must be destroyed before submission");
					if (observePages)
						Require(firstPage.NumRefs() > 1 && secondPage.NumRefs() > 1,
							"recorded descriptors must retain both retired pool pages");
					CompleteCommands(upload, draw);
					for (const auto& result : { firstRead, secondRead, retainedRead }) result.Check();
					upload->m_vulkan.m_commandBuffer->Reset();
					draw->m_vulkan.m_commandBuffer->Reset();
					driver->TrackResources_ThreadSafe();
					nativeDriver->CollectGarbage_RenderThread();
					if (observePages)
						Require(firstPage.NumRefs() == 1 && secondPage.NumRefs() == 1,
							"completed commands must release both pages to the observation handles");
				}
				catch (...)
				{
					context.m_descriptorPool = savedPool;
					throw;
				}
			}
		std::cout << "Descriptor pool lifetime: direct/projected pages, owner release, command-only A/B/A reads and final page release passed\n";
	}

	void TestConcurrentDescriptorPublication(ShaderSetPtr shader)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto* nativeDriver = driver.DynamicCast<VulkanGraphicsDriver>();
		auto pipeline = nativeDriver->GetOrAddComputePipeline(shader->GetComputeShaderRHI());
		const uint32_t workers = App::GetSubmodule<Tasks::Scheduler>()->GetNumRHIThreads();
		constexpr uint32_t iterations = 32;
		std::latch ready(workers), start(1), done(workers);
		std::vector<DWORD> threadIds(workers);
		TVector<Tasks::TaskPtr<std::string>> tasks;
		for (uint32_t worker = 0; worker < workers; ++worker)
		{
			auto task = Tasks::CreateTaskWithResult<std::string>("Concurrent descriptor publication"_h, [&, worker]() -> std::string
			{
				threadIds[worker] = GetCurrentThreadId();
				ready.count_down();
				start.wait();
				std::string error;
				try
				{
					for (uint32_t iteration = 0; iteration < iterations; ++iteration)
					{
						auto inputs = driver->CreateShaderBindings();
						auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
						auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
						commands->BeginCommandList(upload, true);
						commands->BeginCommandList(draw, true);
						const auto prepare = [&](uint32_t seed)
						{
							auto binding = driver->AddSsboToShaderBindings(inputs, "source"_h, 16, 2, 0, true);
							Require(static_cast<bool>(binding), "concurrent storage preparation must succeed");
							std::array<uint32_t, 8> words;
							for (uint32_t i = 0; i < words.size(); ++i) words[i] = seed + i * 17;
							commands->UpdateShaderBinding(upload, binding, words.data(), sizeof(words));
							if (iteration % 2)
								Require(static_cast<bool>(driver->AddShaderBinding(inputs, binding, "unused"_h, 31)),
									"concurrent projection must have an extra binding");
							auto sets = nativeDriver->GetCompatibleDescriptorSets(pipeline->m_layout, { inputs });
							Require(sets.Num() == 1 && sets[0] && sets[0]->IsCompiled() &&
								(iteration % 2 ? sets[0] != inputs->m_vulkan.m_descriptorSet : sets[0] == inputs->m_vulkan.m_descriptorSet),
								"concurrent publication must return a complete native set");
							return sets[0];
						};
						const uint32_t seed = 0x10000000u + worker * 0x100000u + iteration * 0x100u;
						auto first = prepare(seed);
						const auto firstRead = RecordDescriptorReadback(draw, shader, pipeline, first, seed);
						const auto revision = inputs->GetDescriptorRevision();
						if (iteration % 8 == 0)
						{
							const auto published = inputs->m_vulkan.m_descriptorSet;
							Require(!driver->AddBufferToShaderBindings(inputs, "source"_h, 32, 0, EShaderBindingType::UniformBuffer) &&
								inputs->GetDescriptorRevision() == revision && inputs->m_vulkan.m_descriptorSet == published,
								"a rejected concurrent replacement must not publish a revision");
						}
						auto second = prepare(seed + 0x01000000u);
						Require(first != second && inputs->GetDescriptorRevision() == revision + (iteration % 2 ? 2 : 1),
							"replacement must publish a new set while the recorded generation remains alive");
						const auto secondRead = RecordDescriptorReadback(draw, shader, pipeline, second, seed + 0x01000000u);
						const auto retainedRead = RecordDescriptorReadback(draw, shader, pipeline, first, seed);
						first.Clear();
						second.Clear();
						inputs.Clear();
						CompleteCommands(upload, draw);
						for (const auto& result : { firstRead, secondRead, retainedRead }) result.Check();
						upload->m_vulkan.m_commandBuffer->Reset();
						draw->m_vulkan.m_commandBuffer->Reset();
					}
				}
				catch (const std::exception& failure) { error = failure.what(); }
				done.count_down();
				return error;
			}, EThreadType::RHI);
			tasks.Add(task);
			task->Run();
		}
		ready.wait();
		start.count_down();
		uint32_t collections = 0;
		while (!done.try_wait())
		{
			nativeDriver->CollectGarbage_RenderThread();
			++collections;
			std::this_thread::yield();
		}
		for (auto& task : tasks) task->Wait();
		for (auto& task : tasks) Require(task->GetResult().empty(), task->GetResult().c_str());
		Require(collections > 0, "Render cache collection must overlap active RHI tasks");
		for (uint32_t i = 0; i < workers; ++i)
			for (uint32_t j = 0; j < i; ++j)
				Require(threadIds[i] != threadIds[j], "publication stress must use distinct real RHI workers");
		driver->TrackResources_ThreadSafe();
		nativeDriver->CollectGarbage_RenderThread();
		std::cout << "Concurrent descriptors: " << workers << " RHI workers, 32 direct/projected replacements each, refusal, Render cache collection and complete A/B/A reads passed\n";
	}

	void TestSamplerReductionCache()
	{
		const auto device = VulkanApi::GetInstance()->GetMainDevice();
		for (auto filter : { ETextureFiltration::Nearest, ETextureFiltration::Linear })
			for (auto clamping : { ETextureClamping::Repeat, ETextureClamping::Clamp })
				for (bool mips : { false, true })
					for (auto reduction : { ESamplerReductionMode::Average, ESamplerReductionMode::Min, ESamplerReductionMode::Max })
					{
						const auto sampler = device->GetSamplers()->GetSampler(filter, clamping, mips, reduction);
						const bool supported = reduction == ESamplerReductionMode::Average || device->IsSamplerFilterMinmaxSupported();
						Require(bool(sampler) == supported,
							"the sampler cache must not create min/max variants without samplerFilterMinmax");
						if (sampler) Require(VkSampler(*sampler) != VK_NULL_HANDLE, "supported samplers must have native handles");
						Require(sampler == device->GetSamplers()->GetSampler(filter, clamping, mips, reduction),
							"repeated sampler requests must reuse the cached variant");
					}
		auto& driver = Renderer::GetDriver();
		for (auto reduction : { ESamplerReductionMode::Min, ESamplerReductionMode::Max })
		{
			auto texture = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false,
				EImageLayout::ShaderReadOnlyOptimal, reduction);
			texture->m_vulkan = driver->GetDefaultTexture()->m_vulkan;
			for (bool replacement : { false, true })
			{
				auto bindings = driver->CreateShaderBindings();
				if (replacement) Require(bool(driver->AddSamplerToShaderBindings(bindings, "source"_h, driver->GetDefaultTexture(), 0)),
					"average sampling must work before min/max replacement");
				const auto previous = bindings->m_vulkan.m_descriptorSet;
				const auto revision = bindings->GetDescriptorRevision();
				const bool prepared = bool(driver->AddSamplerToShaderBindings(bindings, "source"_h, texture, 0));
				Require(prepared == device->IsSamplerFilterMinmaxSupported(),
					"min/max preparation must fail when the sampler is unavailable, not use average filtering");
				if (!prepared)
				{
					Require(bindings->m_vulkan.m_descriptorSet == previous && bindings->GetDescriptorRevision() == revision,
						"rejected sampler preparation must preserve the published set and revision");
					Require(bool(driver->AddSamplerToShaderBindings(bindings, "source"_h, driver->GetDefaultTexture(), 0)),
						"a rejected min/max request must allow an ordinary sampling retry");
				}
			}
		}
		std::cout << "Sampler reduction cache: minmax=" << device->IsSamplerFilterMinmaxSupported() << ", all 24 variants checked\n";
		std::cout << "Sampler reduction publication: minmax=" << device->IsSamplerFilterMinmaxSupported() << ", cold/replacement Min/Max, revision and retry passed\n";
	}

	void TestSkyOverlayBlending(ShaderSetPtr shader, TRefPtr<TestGraph> graph, RHIShaderBindingSetPtr frame)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto mesh = graph->GetFullscreenNdcQuad();
		auto target = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		for (auto mode : { EBlendMode::Multiply, EBlendMode::AlphaBlendingPreserveAlpha })
			for (float sourceAlpha : { 0.0f, 0.35f, 1.0f })
				for (float destinationAlpha : { 0.0f, 0.4f, 1.0f })
				{
					const glm::vec4 destination(0.25f, 0.75f, 2.0f, destinationAlpha);
					const glm::vec3 tint(0.2f, 0.5f, 0.7f);
					const glm::vec4 source(mode == EBlendMode::Multiply ? tint * sourceAlpha : tint, sourceAlpha);
					auto texture = driver->CreateImage_Immediate(&source, sizeof(source), glm::ivec3(1), 1,
						ETextureType::Texture2D, EFormat::R32G32B32A32_SFLOAT);
					Require(static_cast<bool>(texture), "overlay test pixels must upload");
					auto bindings = driver->CreateShaderBindings();
					driver->AddSamplerToShaderBindings(bindings, "colorSampler"_h, texture, 0);
					const RenderState state{ false, false, 0, false, ECullMode::None, mode, EFillMode::Fill, 0, false };
					auto material = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shader, bindings);
					auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(upload, true);
					commands->BeginCommandList(draw, true);
					ClearColor(draw, target, destination);
					commands->ImageMemoryBarrier(draw, target, EImageLayout::ColorAttachmentOptimal);
					commands->BeginRenderPass(draw, { target }, nullptr, glm::vec4(0, 0, Side, Side),
						glm::ivec2(0), false, glm::vec4(0), 0, false);
					commands->SetViewport(draw, 0, 0, Side, Side, glm::vec2(0), glm::vec2(Side), 0, 1);
					commands->BindMaterial(draw, material);
					Require(commands->BindShaderBindings(draw, material, { frame, bindings }), "sky overlay bindings must bind");
					commands->BindVertexBuffer(draw, mesh->m_vertexBuffer, 0);
					commands->BindIndexBuffer(draw, mesh->m_indexBuffer, 0);
					commands->DrawIndexed(draw, 6, 1, 0, 0, 0);
					commands->EndRenderPass(draw);
					auto pixels = ReadColor(draw, target);
					CompleteCommands(upload, draw);
					const glm::vec3 rgb = mode == EBlendMode::Multiply ?
						glm::vec3(destination) * (glm::vec3(1 - sourceAlpha) + glm::vec3(source)) :
						glm::vec3(destination) * (1 - sourceAlpha) + tint * sourceAlpha;
					const glm::vec4 expected(rgb, destinationAlpha);
					const auto actual = static_cast<const glm::vec4*>(pixels->GetPointer());
					for (uint32_t pixel = 0; pixel < Side * Side; ++pixel)
						for (uint32_t c = 0; c < 4; ++c)
							Require(std::isfinite(actual[pixel][c]) && std::abs(actual[pixel][c] - expected[c]) < 0.00001f,
								"cloud and shaft overlays must blend HDR RGB while preserving destination alpha");
				}
		std::cout << "Sky overlay blending: transparent/partial/opaque sources, HDR RGB and three destination alpha masks passed\n";
	}

	struct BackgroundPause
	{
		std::latch entered{ 1 }, release{ 1 };
		Tasks::TaskPtr<void> task;
		bool resumed = false;

		BackgroundPause()
		{
			task = Tasks::CreateTask("Hold cloud generation"_h, [&]()
			{
				entered.count_down();
				release.wait();
			}, EThreadType::Background);
			task->Run();
			entered.wait();
		}

		void Resume()
		{
			if (!resumed)
			{
				resumed = true;
				release.count_down();
				task->Wait();
			}
		}

		~BackgroundPause() { Resume(); }
	};

	void TestGeneratedCloudNoise(const std::filesystem::path& workspace)
	{
		const auto depthReadback = WriteDepthReadbackShader(workspace);
		Require(std::filesystem::equivalent(AssetRegistry::GetCacheFolder(), workspace / "Cache"),
			"cloud noise must use the temporary workspace cache");
		const std::array names{ "PerlinWorleyCloudsNoiseLow.bin", "PerlinWorleyCloudsNoiseHigh.bin" };
		const std::array obsoleteNames{ "CloudsNoiseLow.bin", "CloudsNoiseHigh.bin" };
		const std::array<uint32_t, 2> sizes{ 128, 32 };
		for (auto name : names)
			Require(!std::filesystem::exists(workspace / "Cache" / name), "the cloud fixture must start cold");
		for (uint32_t n = 0; n < sizes.size(); ++n)
		{
			TVector<uint8_t> obsolete(size_t(sizes[n]) * sizes[n] * sizes[n]);
			std::fill_n(obsolete.GetData(), obsolete.Num(), uint8_t(0));
			const auto path = workspace / "Cache" / obsoleteNames[n];
			AssetRegistry::WriteBinaryFile(path, obsolete);
			Require(std::filesystem::file_size(path) == obsolete.Num(),
				"obsolete cloud cache fixtures must be written in the temporary workspace");
		}

		auto node = TRefPtr<SkyCommandProbe>::Make();
		node->LoadShaders();
		node->LoadClouds();
		const auto onRender = [](const std::function<void()>& action)
		{
			auto task = Tasks::CreateTaskWithResult<std::string>("Cloud noise GPU contracts"_h, [&]() -> std::string
			{
				try { action(); return {}; }
				catch (const std::exception& error) { return error.what(); }
			}, EThreadType::Render);
			task->Run();
			task->Wait();
			if (!task->GetResult().empty()) throw std::runtime_error(task->GetResult());
		};

		TRefPtr<TestGraph> graph;
		RHIRenderTargetPtr color, depth, linearDepth;
		RHISurfacePtr depthSurface;
		RHISceneViewSnapshot scene;
		UboFrameData frameData{};
		frameData.m_view = glm::rotate(glm::mat4(1), glm::radians(-45.0f), Math::vec3_Right);
		frameData.m_projection = Math::PerspectiveRH(glm::radians(80.0f), 1.0f, 0.1f, 100000.0f);
		frameData.m_invProjection = glm::inverse(frameData.m_projection);
		frameData.m_cameraPosition = glm::vec4(0, 100, 0, 1);
		frameData.m_viewportSize = glm::ivec2(Side);
		frameData.m_cameraZNearZFar = glm::vec2(0.1f, 100000);
		onRender([&]()
		{
			auto& driver = Renderer::GetDriver();
			graph = TRefPtr<TestGraph>::Make();
			graph->SetSampler("g_ditherPatternSampler"_h, driver->GetDefaultTexture());
			graph->SetSampler("g_noiseSampler"_h, driver->GetDefaultTexture());
			color = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R16G16B16A16_SFLOAT);
			depth = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::D32_SFLOAT,
				ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit |
				ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit);
			linearDepth = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32_SFLOAT);
			graph->SetRenderTarget("DepthBuffer"_h, depth);
			node->SetRHIResource("color"_h, color);
			node->SetRHIResource("linearDepth"_h, linearDepth);
			node->PrepareCloudReadback();
			scene.m_frameBindings = driver->CreateShaderBindings();
			driver->AddBufferToShaderBindings(scene.m_frameBindings, "frameData"_h, sizeof(frameData), 0, EShaderBindingType::UniformBuffer);
			auto params = node->GetSkyParams();
			params.m_cloudsCoverage = 0.8f;
			params.m_sunShaftsIntensity = 0;
			node->SetSkyParams(params);
			TestSkyOverlayBlending(node->GetBlitShader(), graph, scene.m_frameBindings);
		});

		std::vector<glm::vec4> clouds, compositePixels;
		const auto render = [&](bool expectClouds = false, VkResult uploadError = VK_SUCCESS, uint32_t refusedUploads = 2,
			float sceneDepth = 100000.0f, bool checkDepthIndependence = false)
		{
			onRender([&]()
			{
				auto& driver = Renderer::GetDriver();
				auto commands = Renderer::GetDriverCommands();
				CaptureAttachments capture;
				auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(draw, true);
				ClearColor(draw, color, glm::vec4(-1, -1, -1, 1));
				ClearColor(draw, linearDepth, glm::vec4(sceneDepth));
				commands->ImageMemoryBarrier(draw, linearDepth, EImageLayout::ShaderReadOnlyOptimal);
				commands->ImageMemoryBarrier(draw, depth, EImageLayout::TransferDstOptimal);
				commands->ClearDepthStencil(draw, depth, 1.0f, 0);
				if (checkDepthIndependence)
				{
					if (depthSurface && depthSurface->NeedsResolve())
					{
						commands->ImageMemoryBarrier(draw, depthSurface->GetTarget(), EImageLayout::TransferDstOptimal);
						commands->ClearDepthStencil(draw, depthSurface->GetTarget(), 0.375f, 0);
					}
					if (auto decoy = graph->GetRenderTarget("DepthBuffer"_h); decoy && decoy != depth)
					{
						commands->ImageMemoryBarrier(draw, decoy, EImageLayout::TransferDstOptimal);
						commands->ClearDepthStencil(draw, decoy, 0.625f, 0);
					}
				}
				commands->UpdateShaderBinding(upload, scene.m_frameBindings->GetOrAddShaderBinding("frameData"_h), &frameData, sizeof(frameData));
				Require(!expectClouds || node->AreCloudsResourcesReady(), "cloud comparison requires completed uploads before recording");
				if (uploadError != VK_SUCCESS)
					Tests::RequireImageInitializationRefusal([&]() { node->Process(graph, upload, draw, scene); }, 0, refusedUploads, uploadError);
				else node->Process(graph, upload, draw, scene);
				const bool bNoDepthAttachment = recordedDepth.imageView == VK_NULL_HANDLE && recordedDepth.resolveImageView == VK_NULL_HANDLE;
				const auto depthPixels = checkDepthIndependence ? ReadDepth(draw, depth, depthReadback) : RHIBufferPtr{};
				const auto liveDepthPixels = checkDepthIndependence && depthSurface && depthSurface->NeedsResolve() ?
					ReadDepth(draw, depthSurface->GetTarget(), depthReadback) : RHIBufferPtr{};
				auto output = ReadColor(draw, color);
				auto texture = node->GetCloudsTexture();
				const auto extent = texture->GetExtent();
				auto pixels = driver->CreateBuffer(size_t(extent.x) * extent.y * 8, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
				commands->ImageMemoryBarrier(draw, texture, EImageLayout::TransferSrcOptimal);
				commands->CopyImageToBuffer(draw, texture, pixels);
				commands->ImageMemoryBarrier(draw, texture, texture->GetDefaultLayout());
				CompleteCommands(upload, draw);
				if (checkDepthIndependence)
				{
					Require(bNoDepthAttachment, "Sky overlay must not bind unused hardware depth");
					const auto* values = static_cast<const glm::vec2*>(depthPixels->GetPointer());
					for (uint32_t i = 0; i < Side * Side; ++i)
						Require(values[i].x == 1.0f, "Sky overlay must preserve resolved depth");
					if (liveDepthPixels)
					{
						values = static_cast<const glm::vec2*>(liveDepthPixels->GetPointer());
						for (uint32_t i = 0; i < Side * Side * uint32_t(depthSurface->GetTarget()->GetMsaaSamples()); ++i)
							Require(values[i].x == 0.375f, "Sky overlay must preserve every unused MSAA depth sample");
					}
				}
				const auto composite = static_cast<const uint16_t*>(output->GetPointer());
				compositePixels.resize(Side * Side);
				for (uint32_t i = 0; i < Side * Side * 4; ++i)
				{
					const float value = glm::unpackHalf1x16(composite[i]);
					compositePixels[i / 4][i % 4] = value;
					if (!std::isfinite(value) || value < 0 || (i % 4 == 3 && value > 1))
						throw std::runtime_error("sky composition while noise loads: component=" + std::to_string(i) +
							", value=" + std::to_string(value) + ", draws=" + std::to_string(node->GetDrawCallStats().m_numBatches));
				}
				clouds.resize(size_t(extent.x) * extent.y);
				const auto values = static_cast<const uint16_t*>(pixels->GetPointer());
				for (size_t i = 0; i < clouds.size(); ++i)
					for (uint32_t c = 0; c < 4; ++c)
					{
						clouds[i][c] = glm::unpackHalf1x16(values[i * 4 + c]);
						Require(std::isfinite(clouds[i][c]) && clouds[i][c] >= 0,
							"generated clouds must contain finite nonnegative radiance and opacity");
					}
				driver->TrackResources_ThreadSafe();
			});
		};

		BackgroundPause pause;
		std::array<Tasks::TaskPtr<TVector<uint8_t>>, 2> noiseTasks;
		onRender([&]()
		{
			for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				auto& driver = Renderer::GetDriver();
				auto commands = Renderer::GetDriverCommands();
				auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(draw, true);
				Tests::RequireAttachmentInitializationRefusal([&]() { node->Process(graph, upload, draw, scene); }, 0, error);
				Require(draw->GetNumRecordedCommands() == 0 && !graph->GetSampler("g_skyCubemap"_h),
					"refused fallback noise must not draw the sky or publish an environment");
				CompleteCommands(upload, draw);
			}
		});
		for (uint32_t frame = 0; frame < 3; ++frame)
		{
			render();
			onRender([&]()
			{
				noiseTasks = node->GetNoiseTasks();
				Require(noiseTasks[0] && noiseTasks[1] && !noiseTasks[0]->IsFinished() && !noiseTasks[1]->IsFinished(),
					"held Background generation must remain pending after rendering");
				Require(!node->AreCloudsResourcesReady(), "pending generation must not enable clouds");
			});
			Require(std::all_of(clouds.begin(), clouds.end(), [](auto pixel) { return pixel == glm::vec4(0); }),
				"pending cloud frames must be cleared to transparent black");
		}
		std::cout << "Cloud noise pending: three completed GPU frames while Background is held passed\n";
		pause.Resume();
		const auto started = std::chrono::steady_clock::now();
		for (auto task : noiseTasks) task->Wait();
		std::cout << "Cloud noise cold generation: " <<
			std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s\n";
		for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			render(false, error);
			onRender([&]()
			{
				const auto textures = node->GetNoiseTextures();
				Require(node->GetNoiseTasks() == noiseTasks && !textures[0] && !textures[1] && !node->AreCloudsResourcesReady(),
					"refused noise uploads must retain both completed CPU tasks for the next frame");
			});
		}
		render();
		onRender([&]() { App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(EThreadType::RHI); });
		render(true);
		std::cout << "Cloud noise initialization: fallback refusal, retained generated tasks and same-node GPU retry passed\n";

		std::array<RHITexturePtr, 2> generated;
		std::array<std::filesystem::file_time_type, 2> written;
		onRender([&]()
		{
			Require(node->AreCloudsResourcesReady(), "both completed noise uploads must enable clouds");
			generated = node->GetNoiseTextures();
			auto& driver = Renderer::GetDriver();
			for (uint32_t n = 0; n < sizes.size(); ++n)
			{
				TVector<uint8_t> cached;
				const auto path = workspace / "Cache" / names[n];
				Require(AssetRegistry::ReadBinaryFile(path, cached) && cached.Num() == size_t(sizes[n]) * sizes[n] * sizes[n],
					"real cloud generation must save the complete derived volume");
				written[n] = std::filesystem::last_write_time(path);
				const auto& generatedBytes = noiseTasks[n]->GetResult();
				Require(cached.Num() == generatedBytes.Num() &&
					std::memcmp(cached.GetData(), generatedBytes.GetData(), cached.Num()) == 0,
					"the persisted noise must equal the actual Background task result");
				auto pixels = driver->CreateBuffer(cached.Num(), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
				auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				auto commands = Renderer::GetDriverCommands();
				commands->BeginCommandList(draw, true);
				commands->ImageMemoryBarrier(draw, generated[n], EImageLayout::TransferSrcOptimal);
				draw->m_vulkan.m_commandBuffer->CopyImageToBuffer(*pixels->m_vulkan.m_buffer->Get(),
					generated[n]->m_vulkan.m_image, sizes[n], sizes[n], sizes[n]);
				commands->ImageMemoryBarrier(draw, generated[n], generated[n]->GetDefaultLayout());
				commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
					static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
				commands->EndCommandList(draw);
				Require(driver->SubmitCommandList_Immediate(draw), "the 3D cloud readback must complete");
				Require(std::memcmp(cached.GetData(), pixels->GetPointer(), cached.Num()) == 0,
					"every uploaded cloud voxel must match the generated cache bytes");
				std::array<bool, 256> histogram{};
				for (auto value : cached) histogram[value] = true;
				const auto distinct = std::count(histogram.begin(), histogram.end(), true);
				Require(distinct > 32, "real cloud noise must retain a useful range of densities");
				std::cout << "Cloud noise " << sizes[n] << "^3: " << distinct << " distinct values; CPU/cache/GPU bytes identical\n";
			}
		});
		const auto reference = clouds;
		float minimum = 1, maximum = 0;
		for (auto pixel : reference)
		{
			minimum = std::min(minimum, pixel.a);
			maximum = std::max(maximum, pixel.a);
			Require(pixel.a <= 1, "cloud opacity must stay normalized");
		}
		std::cout << "Cloud opacity range [" << minimum << ", " << maximum << "]; " << reference.size() << " pixels\n";
		if (maximum <= 0.05f || maximum - minimum <= 0.05f)
			throw std::runtime_error("real cloud opacity range [" + std::to_string(minimum) + ", " + std::to_string(maximum) + "]");

		for (bool named : { false, true })
		for (bool surface : { false, true })
		{
			onRender([&]()
			{
				if (named)
				{
					node->SetRHIResource_Unresolved("color"_h, "SkyOutput"_h);
					node->SetRHIResource_Unresolved("linearDepth"_h, "SkyDepth"_h);
				}
			});
			VulkanDescriptorSetPtr previous;
			for (uint32_t frame = 0; frame < 6; ++frame)
			{
				onRender([&]()
				{
					auto& driver = Renderer::GetDriver();
					if (frame == 0 || frame == 3)
						color = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R16G16B16A16_SFLOAT);
					if (frame == 2 || frame == 4)
						linearDepth = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32_SFLOAT);
					const auto publish = [&](StringHash slot, StringHash name, RHIRenderTargetPtr texture)
					{
						const auto resource = surface ? RHIResourcePtr(driver->CreateSurface(texture)) : RHIResourcePtr(texture);
						if (!named) node->SetRHIResource(slot, resource);
						else if (surface) graph->SetSurface(name, resource.DynamicCast<RHISurface>());
						else graph->SetRenderTarget(name, texture);
					};
					publish("color"_h, "SkyOutput"_h, color);
					publish("linearDepth"_h, "SkyDepth"_h, linearDepth);
				});
				const bool bIsOccluded = frame == 2 || frame == 3;
				render(true, VK_SUCCESS, 2, bIsOccluded ? 10.0f : frameData.m_cameraZNearZFar.y);
				if (bIsOccluded)
					Require(std::all_of(clouds.begin(), clouds.end(), [](auto pixel) { return pixel == glm::vec4(0); }),
						"replaced near depth must occlude every cloud pixel");
				else Require(clouds == reference, "restored far depth must reproduce all cloud pixels");
				onRender([&]()
				{
					auto bindings = node->GetShaderBindings();
					Require(bindings->GetOrAddShaderBinding("linearDepth"_h)->GetTextureBinding() == linearDepth,
						"Sky must bind the current resolved depth input");
					if (frame % 2 == 1)
						Require(previous == bindings->m_vulkan.m_descriptorSet,
							"unchanged Sky samplers must reuse their native descriptor set");
					previous = bindings->m_vulkan.m_descriptorSet;
				});
			}
			std::cout << "Sky resources named=" << named << " surface=" << surface
				<< ": six frames, output/depth replacement, all cloud pixels and warm descriptors passed\n";
		}

		const auto compositeReference = compositePixels;
		enum class SkyDepth { Target, Surface, SurfaceOnly, SurfaceDecoy, Absent };
		for (auto input : { SkyDepth::Target, SkyDepth::Surface, SkyDepth::SurfaceOnly, SkyDepth::SurfaceDecoy, SkyDepth::Absent })
		{
			for (uint32_t frame = 0; frame < 6; ++frame)
			{
				onRender([&]()
				{
					if (frame % 2 == 0)
					{
						auto& driver = Renderer::GetDriver();
						const auto usage = ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit;
						depth = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::D32_SFLOAT,
							ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
						depthSurface = input != SkyDepth::Target && input != SkyDepth::Absent ? driver->CreateSurface(depth) : RHISurfacePtr{};
						graph->SetSurface("DepthBuffer"_h, depthSurface);
						const auto target = input == SkyDepth::SurfaceDecoy ? driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::D32_SFLOAT,
							ETextureFiltration::Nearest, ETextureClamping::Clamp, usage) : depth;
						graph->SetRenderTarget("DepthBuffer"_h, input == SkyDepth::SurfaceOnly || input == SkyDepth::Absent ? RHIRenderTargetPtr{} : target);
					}
				});
				render(true, VK_SUCCESS, 2, frameData.m_cameraZNearZFar.y, true);
				Require(clouds == reference && compositePixels == compositeReference,
					"hardware depth publication must not affect Sky's cloud or final HDR pixels");
			}
			std::cout << "Sky overlay depth " << magic_enum::enum_name(input)
				<< ": six frames, replacement/reuse, no native depth attachment, untouched depth and HDR/cloud parity passed\n";
		}
		onRender([&]() { graph->SetSurface("DepthBuffer"_h, {}); graph->SetRenderTarget("DepthBuffer"_h, depth); depthSurface.Clear(); });

		std::array<RHITexturePtr, 2> withoutWorley;
		std::array<TVector<uint8_t>, 2> perlinOnly;
		for (uint32_t n = 0; n < sizes.size(); ++n)
		{
			const auto size = sizes[n];
			auto& bytes = perlinOnly[n];
			bytes.Resize(size_t(size) * size * size);
			for (uint32_t z = 0; z < size; ++z)
				for (uint32_t y = 0; y < size; ++y)
					for (uint32_t x = 0; x < size; ++x)
					{
						const auto uv = (glm::vec3(x, y, z) + (n == 0 ? 0.5f : 0.0f)) / float(size);
						const float perlin = (Math::fBmTiledPerlin(uv * 5.0f, 4, 5) + 1) * 0.5f;
						// The original magnitude bug reduced the Worley contribution to zero.
						const float value = n == 0 ? (perlin + 1) * 0.5f : perlin * 0.625f;
						bytes[x + y * size + z * size * size] = uint8_t(value * 255);
					}
		}
		onRender([&]()
		{
			for (uint32_t n = 0; n < sizes.size(); ++n)
			{
				const auto& bytes = perlinOnly[n];
				withoutWorley[n] = Renderer::GetDriver()->CreateTexture(bytes.GetData(), bytes.Num(), glm::ivec3(sizes[n]), 1,
					ETextureType::Texture3D, EFormat::R8_UNORM, ETextureFiltration::Linear, ETextureClamping::Repeat,
					ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit);
			}
			node->SetNoiseTextures(withoutWorley);
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(EThreadType::RHI);
		});
		render();
		render(true);
		size_t changed = 0;
		for (size_t i = 0; i < reference.size(); ++i)
			if (std::abs(reference[i].a - clouds[i].a) > 0.005f) ++changed;
		Require(changed > reference.size() / 20, "Worley must change the rendered cloud silhouette, not only CPU bytes");
		onRender([&]() { node->SetNoiseTextures(generated); });
		render(true);
		Require(reference == clouds, "restoring the generated noise must restore the same cloud pixels");
		std::cout << "Cloud noise rendering: opacity [" << minimum << ", " << maximum << "], " << changed << "/" <<
			reference.size() << " pixels changed without Worley; restored output identical passed\n";

		onRender([&]() { node->ResetNoise(); });
		BackgroundPause warmPause;
		render();
		onRender([&]() { noiseTasks = node->GetNoiseTasks(); });
		warmPause.Resume();
		for (auto task : noiseTasks) { Require(static_cast<bool>(task), "warm reload must schedule the real cache task"); task->Wait(); }
		render();
		onRender([&]() { App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(EThreadType::RHI); });
		render(true);
		Require(reference == clouds, "warm cloud cache reuse must preserve the rendered output");
		for (uint32_t n = 0; n < sizes.size(); ++n)
			Require(std::filesystem::last_write_time(workspace / "Cache" / names[n]) == written[n],
				"warm noise loading must not regenerate or rewrite derived data");
		std::cout << "Cloud noise cache: cold generation, complete 3D upload and warm pixel parity passed\n";
		RHITexturePtr previousSky;
		SkyParameters captured, replacement;
		onRender([&]()
		{
			previousSky = graph->GetSampler("g_skyCubemap"_h);
			Require(previousSky && node->GetEnvironmentSkyParams(captured), "capture refusal needs a completed sky environment");
			replacement = node->GetSkyParams();
			replacement.m_sunIlluminance *= 0.5f;
			node->SetSkyParams(replacement);
		});
		for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			render(true, error, 1);
			onRender([&]()
			{
				SkyParameters visible;
				Require(graph->GetSampler("g_skyCubemap"_h) == previousSky && node->GetEnvironmentSkyParams(visible) &&
					visible.GetEnvironmentKey() == captured.GetEnvironmentKey(),
					"failed sky capture initialization must retain the old cubemap and its matching lighting parameters");
			});
		}
		for (uint32_t frame = 0; frame < 8; ++frame) render(true);
		onRender([&]()
		{
			SkyParameters visible;
			Require(graph->GetSampler("g_skyCubemap"_h) != previousSky && node->GetEnvironmentSkyParams(visible) &&
				visible.GetEnvironmentKey() == replacement.GetEnvironmentKey(),
				"the unchanged pending sky capture must finish and publish its matching lighting parameters");
		});
		std::cout << "Sky capture initialization: native refusal, retained cubemap/lighting pair and same-capture recovery passed\n";
	}

	class EnvironmentCommandProbe : public EnvironmentNode
	{
	public:
		uint32_t NumCachedEnvironments() const { return m_numCachedEnvironments; }
	};

	class SkyRendererAccess : public Renderer
	{
	public:
		using Renderer::UpdateSkyParameters;
		using Renderer::m_previousRenderFrame;
	};

	void TestSkyQueuedFrameOrder()
	{
		auto* renderer = App::GetSubmodule<Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
		World world("Queued sky frame", 0);
		auto sky = world.Instantiate("Sky")->AddComponent<SkyComponent>();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<SkyNode>::Make();
		node->SetTag("Sky"_h);
		graph->GetGraph().Add(node);
		(renderer->*&SkyRendererAccess::UpdateSkyParameters)(&world, graph);
		scheduler->WaitIdle({ EThreadType::Render });
		const auto first = sky->GetSkyParameters();
		std::latch releasePreparation(1);
		auto preparation = Tasks::CreateTask("Delayed sky frame preparation"_h,
			[&]() { releasePreparation.wait(); }, EThreadType::RHI);
		bool previousFrameMatched = false;
		auto previousFrame = Tasks::CreateTask("Observe previous sky frame"_h,
			[&]() { previousFrameMatched = node->GetSkyParams() == first; }, EThreadType::Render);
		previousFrame->Join(preparation);
		auto& previous = renderer->*&SkyRendererAccess::m_previousRenderFrame;
		auto saved = previous;
		previous = previousFrame;
		preparation->Run();
		previousFrame->Run();
		sky->SetCloudsDensity(first.m_cloudsDensity + 0.1f);
		(renderer->*&SkyRendererAccess::UpdateSkyParameters)(&world, graph);
		auto pending = Tasks::CreateTaskWithResult<bool>("Observe sky while RHI preparation is pending"_h,
			[&]() { return node->GetSkyParams() == first; }, EThreadType::Render);
		pending->Run();
		pending->Wait();
		releasePreparation.count_down();
		scheduler->WaitIdle({ EThreadType::RHI, EThreadType::Render });
		previous = saved;
		const bool currentMatched = node->GetSkyParams() == sky->GetSkyParameters();
		world.Clear();
		Require(pending->GetResult() && previousFrameMatched && currentMatched,
			"new sky parameters must not overtake the previous frame waiting for RHI preparation");
		std::cout << "Sky frame ordering: delayed RHI preparation retains the previous sky until its Render task completes passed\n";
	}

	void TestSkyWorldOwnership()
	{
		auto* renderer = App::GetSubmodule<Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
		World first("First sky world", 0), second("Second sky world", 0), empty("No sky world", 0);
		auto otherOwner = first.Instantiate("Other sky", InstanceId("00000000000000000003"));
		auto chosenOwner = first.Instantiate("Chosen sky", InstanceId("00000000000000000002"));
		auto other = otherOwner->AddComponent<SkyComponent>();
		auto chosen = chosenOwner->AddComponent<SkyComponent>();
		auto secondSky = second.Instantiate("Second sky")->AddComponent<SkyComponent>();
		chosen->SetCloudsDensity(0.25f);
		other->SetCloudsDensity(0.75f);
		secondSky->SetCloudsDensity(0.5f);
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<SkyNode>::Make();
		node->SetTag("Sky"_h);
		graph->GetGraph().Add(node);
		std::vector<Tasks::TaskPtr<bool>> observations;
		const auto observe = [&](TRefPtr<SkyNode> observed, SkyParameters expected)
		{
			auto task = Tasks::CreateTaskWithResult<bool>("Observe selected sky"_h,
				[observed, expected]() { return observed->GetSkyParams() == expected; }, EThreadType::Render);
			task->Run();
			observations.push_back(task);
		};
		const auto publish = [&](World& world, uint32_t expectedTasks)
		{
			const auto before = scheduler->GetNumTasks(EThreadType::Render);
			(renderer->*&SkyRendererAccess::UpdateSkyParameters)(&world, graph);
			Require(scheduler->GetNumTasks(EThreadType::Render) == before + expectedTasks,
				"sky publication must enqueue exactly one changed snapshot and no unchanged work");
		};
		std::latch entered(1), release(1);
		auto hold = Tasks::CreateTask("Hold Render during sky ownership changes"_h, [&]()
		{
			entered.count_down();
			release.wait();
		}, EThreadType::Render);
		hold->Run();
		entered.wait();
		try
		{
			publish(first, 1);
			observe(node, chosen->GetSkyParameters());
			const auto beforeTicks = scheduler->GetNumTasks(EThreadType::Render);
			for (uint32_t frame = 0; frame < 120; ++frame)
			{
				other->Tick(0);
				chosen->EditorTick(0);
				publish(first, 0);
			}
			Require(scheduler->GetNumTasks(EThreadType::Render) == beforeTicks,
				"unchanged component ticks must not queue render work");
			other->SetCloudsDensity(0.9f);
			publish(first, 0);
			chosen->SetCloudsDensity(0.3f);
			const auto captured = chosen->GetSkyParameters();
			publish(first, 1);
			chosen->SetCloudsDensity(0.4f);
			observe(node, captured);
			chosen->SetCloudsDensity(0.3f);
			const auto beforeRemoval = scheduler->GetNumTasks(EThreadType::Render);
			Require(otherOwner->RemoveComponent(other), "a nonselected sky must be removable");
			Require(scheduler->GetNumTasks(EThreadType::Render) == beforeRemoval,
				"nonselected EndPlay must not enqueue a global reset");
			publish(first, 0);
			observe(node, captured);
			publish(second, 1);
			const auto beforeTeardown = scheduler->GetNumTasks(EThreadType::Render);
			first.Clear();
			Require(first.GetECS<LightingECS>()->GetNumSkies() == 0 &&
				scheduler->GetNumTasks(EThreadType::Render) == beforeTeardown,
				"old-world teardown must unregister its skies without resetting the new world");
			publish(second, 0);
			observe(node, secondSky->GetSkyParameters());
			auto replacement = TRefPtr<SkyNode>::Make();
			replacement->SetTag("Sky"_h);
			graph->GetGraph().Clear();
			graph->GetGraph().Add(replacement);
			publish(second, 1);
			observe(replacement, secondSky->GetSkyParameters());
			observe(node, secondSky->GetSkyParameters());
			node.Clear();
			publish(empty, 1);
			publish(empty, 0);
			observe(replacement, SkyParameters{});
			publish(second, 1);
			observe(replacement, secondSky->GetSkyParameters());
			graph->GetGraph().Clear();
			publish(second, 0);
			graph->GetGraph().Add(replacement);
			publish(second, 1);
			observe(replacement, secondSky->GetSkyParameters());
		}
		catch (...)
		{
			release.count_down();
			hold->Wait();
			scheduler->WaitIdle({ EThreadType::Render });
			first.Clear();
			second.Clear();
			empty.Clear();
			throw;
		}
		release.count_down();
		hold->Wait();
		for (const auto& observation : observations)
		{
			observation->Wait();
			Require(observation->GetResult(), "queued sky snapshots must retain their owner, values and order across teardown");
		}
		second.Clear();
		empty.Clear();
		std::cout << "Sky ownership: selected world, 120 unchanged frames, copied values, nonselected removal, old-world teardown, graph replacement and default restoration passed\n";
	}

	std::vector<glm::vec3> ReadEnvironmentSamples(const std::array<RHITexturePtr, 4>& maps)
	{
		std::vector<glm::vec3> samples;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		for (const auto& texture : maps)
		{
			float peak = 0;
			auto cube = texture.DynamicCast<RHICubemap>();
			Require(cube && cube->GetFormat() == EFormat::R16G16B16A16_SFLOAT, "the completed sky bundle must contain HDR cubemaps");
			for (uint32_t mip = 0; mip < cube->m_vulkan.m_image->m_mipLevels; ++mip)
			{
				const auto size = glm::max(cube->GetExtent() >> static_cast<int32_t>(mip), glm::ivec2(1));
				for (uint32_t face = 0; face < 6; ++face)
				{
					auto buffer = driver->CreateBuffer(size_t(size.x) * size.y * 8, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
					auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(command, true);
					commands->ImageMemoryBarrier(command, cube, EImageLayout::TransferSrcOptimal);
					command->m_vulkan.m_commandBuffer->CopyImageToBuffer(*buffer->m_vulkan.m_buffer->Get(),
						cube->m_vulkan.m_image, size.x, size.y, 1, mip, face);
					commands->ImageMemoryBarrier(command, cube, EImageLayout::ShaderReadOnlyOptimal);
					commands->MemoryBarrier(command, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
						static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
					commands->EndCommandList(command);
					Require(driver->SubmitCommandList_Immediate(command), "sky subresource readback must complete");
					const auto pixels = static_cast<const uint16_t*>(buffer->GetPointer());
					for (uint32_t i = 0; i < uint32_t(size.x * size.y); ++i)
						for (uint32_t channel = 0; channel < 3; ++channel)
							peak = std::max(peak, glm::unpackHalf1x16(pixels[i * 4 + channel]));
					for (uint32_t index : { 0u, uint32_t(size.y / 2 * size.x + size.x / 2), uint32_t(size.x * size.y - 1) })
					{
						const glm::vec3 pixel(glm::unpackHalf1x16(pixels[index * 4]), glm::unpackHalf1x16(pixels[index * 4 + 1]),
							glm::unpackHalf1x16(pixels[index * 4 + 2]));
						Require(Math::AllFinite(pixel) && glm::all(glm::greaterThanEqual(pixel, glm::vec3(0))),
							"every sampled sky face and mip must contain finite nonnegative radiance");
						samples.push_back(pixel);
					}
				}
			}
			Require(peak > 0 && peak < 60000, "the linear radiance fixture must be nonblack and below half-float saturation");
			std::cout << "Sky environment readback peak: " << peak << '\n';
		}
		return samples;
	}

	void TestSkyEnvironmentPublication()
	{
		const auto onRender = [](const std::function<void()>& action)
		{
			auto task = Tasks::CreateTaskWithResult<std::string>("Sky environment publication"_h, [&]() -> std::string
			{
				try { action(); return {}; }
				catch (const std::exception& error) { return error.what(); }
			}, EThreadType::Render);
			task->Run();
			task->Wait();
			Require(task->GetResult().empty(), task->GetResult().c_str());
		};
		auto* renderer = App::GetSubmodule<Renderer>();
		Require(renderer->EnsureFrameGraph(), "the real SkyComponent handoff needs the renderer's frame graph");
		auto productionGraph = renderer->GetFrameGraph()->GetRHI();
		const auto originalNodes = productionGraph->GetGraph();
		auto sky = TRefPtr<SkyCommandProbe>::Make();
		auto environment = TRefPtr<EnvironmentCommandProbe>::Make();
		sky->LoadShaders();
		sky->SetTag("Sky"_h);
		environment->SetTag("Environment"_h);
		auto graph = TRefPtr<TestGraph>::Make();
		graph->GetGraph().Add(sky);
		graph->GetGraph().Add(environment);
		const StringHash names[] = { "g_rawEnvCubemap"_h, "g_envCubemap"_h, "g_irradianceCubemap"_h, "g_sheenEnvCubemap"_h };
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto color = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R16G16B16A16_SFLOAT);
		auto depth = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::D32_SFLOAT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit |
			ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit);
		auto linearDepth = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32_SFLOAT);
		RHISceneViewSnapshot scene;
		scene.m_frameBindings = driver->CreateShaderBindings();
		UboFrameData frame{};
		frame.m_view = frame.m_projection = frame.m_invProjection = glm::mat4(1);
		frame.m_viewportSize = glm::ivec2(Side);
		frame.m_cameraZNearZFar = glm::vec2(0.1f, 1000);
		auto frameBinding = driver->AddBufferToShaderBindings(scene.m_frameBindings, "frameData"_h, sizeof(frame), 0, EShaderBindingType::UniformBuffer);
		World skyWorld("Sky publication", 0);
		auto componentOwner = skyWorld.Instantiate("Sky");
		auto componentPtr = componentOwner->AddComponent<SkyComponent>();
		auto& component = *componentPtr;
		component.SetCloudsCoverage(0);
		const auto maps = [&]()
		{
			std::array<RHITexturePtr, 4> result;
			for (uint32_t i = 0; i < result.size(); ++i) result[i] = graph->GetSampler(names[i]);
			return result;
		};
		const auto process = [&](bool drawSky, bool filter, int refusedStage = -1, VkResult error = VK_SUCCESS)
		{
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			if (drawSky)
			{
				ClearColor(draw, color, glm::vec4(-1));
				ClearColor(draw, linearDepth, glm::vec4(1000));
				commands->ImageMemoryBarrier(draw, linearDepth, EImageLayout::ShaderReadOnlyOptimal);
				commands->ImageMemoryBarrier(draw, depth, EImageLayout::TransferDstOptimal);
				commands->ClearDepthStencil(draw, depth, 1.0f, 0);
				commands->UpdateShaderBinding(upload, frameBinding, &frame, sizeof(frame));
				sky->Process(graph, upload, draw, scene);
			}
			if (filter)
			{
				if (refusedStage < 0) environment->Process(graph, upload, draw, scene);
				else Tests::RequireAttachmentInitializationRefusal([&]() { environment->Process(graph, upload, draw, scene); }, refusedStage, error);
			}
			CompleteCommands(upload, draw);
			driver->TrackResources_ThreadSafe();
		};
		const auto handoff = [&](float intensity)
		{
			SkyParameters before;
			onRender([&]() { before = sky->GetSkyParams(); });
			std::latch entered(1), release(1);
			bool unchanged = false;
			auto hold = Tasks::CreateTask("Hold sky handoff"_h, [&]()
			{
				entered.count_down();
				release.wait();
				unchanged = sky->GetSkyParams() == before;
			}, EThreadType::Render);
			hold->Run();
			entered.wait();
			component.SetSunIlluminance(glm::vec3(intensity));
			component.Tick(0);
			(renderer->*&SkyRendererAccess::UpdateSkyParameters)(&skyWorld, productionGraph);
			const auto captured = component.GetSkyParameters();
			component.SetAmbient(captured.m_ambient + 17);
			release.count_down();
			hold->Wait();
			onRender([&]()
			{
				Require(unchanged && sky->GetSkyParams() == captured,
					"SkyComponent must enqueue a value snapshot without mutating render-owned state while its task is pending");
			});
			component.SetAmbient(captured.m_ambient);
			return captured;
		};
		onRender([&]()
		{
			productionGraph->GetGraph().Clear();
			productionGraph->GetGraph().Add(sky);
			sky->SetEmptyClouds();
			graph->SetSampler("g_ditherPatternSampler"_h, driver->GetDefaultTexture());
			graph->SetSampler("g_noiseSampler"_h, driver->GetDefaultTexture());
			graph->SetRenderTarget("DepthBuffer"_h, depth);
			sky->SetRHIResource("color"_h, color);
			sky->SetRHIResource("linearDepth"_h, linearDepth);
		});
		try
		{
			onRender([&]() { process(false, true); }); // Warm BRDF independently of a completed sky.
			const auto first = handoff(100);
			onRender([&]()
			{
				for (uint32_t i = 0; i < 7; ++i) process(true, false);
				SkyParameters captured;
				Require(sky->GetEnvironmentSkyParams(captured) && captured == first, "the first complete capture must keep its queued parameters");
				for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
				for (int stage = 0; stage < 3; ++stage)
				{
					process(false, true, stage, error);
					Require(!environment->GetEnvironmentSkyParams(captured) && !maps()[0],
						"failed first filtering must not publish sky parameters without their environment maps");
				}
				process(false, true);
			});
			std::array<RHITexturePtr, 4> retained;
			std::vector<glm::vec3> originalPixels;
			onRender([&]() { retained = maps(); originalPixels = ReadEnvironmentSamples(retained); });
			const auto second = handoff(200);
			onRender([&]()
			{
				for (uint32_t i = 0; i < 6; ++i)
				{
					process(true, true);
					SkyParameters captured, filtered;
					Require(sky->GetEnvironmentSkyParams(captured) && captured == first &&
						environment->GetEnvironmentSkyParams(filtered) && filtered == first && maps() == retained,
						"unfinished faces must retain the previous sky and filtered environment pair");
				}
				process(true, false);
				for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
				for (int stage = 0; stage < 3; ++stage)
				{
					process(false, true, stage, error);
					SkyParameters filtered;
					Require(environment->GetEnvironmentSkyParams(filtered) && filtered == first && maps() == retained,
						"failed replacement filtering must preserve the previous completed parameters and maps together");
				}
				process(false, true);
				SkyParameters filtered;
				Require(environment->GetEnvironmentSkyParams(filtered) && filtered == second && maps() != retained,
					"successful replacement must publish its sky parameters and complete map bundle together");
				const auto pixels = ReadEnvironmentSamples(maps());
				Require(pixels.size() == originalPixels.size(), "repeated sky captures must keep the same subresources");
				float energy = 0;
				for (size_t i = 0; i < pixels.size(); ++i)
				{
					energy += glm::length(originalPixels[i]);
					if (!glm::all(glm::lessThanEqual(glm::abs(pixels[i] - 2.0f * originalPixels[i]),
						glm::max(glm::vec3(0.01f), glm::abs(pixels[i]) * 0.01f))))
						throw std::runtime_error("sky radiance must double at sample " + std::to_string(i) +
							": previous=" + std::to_string(originalPixels[i].r) + ", current=" + std::to_string(pixels[i].r));
				}
				Require(energy > 1, "a black fallback cannot satisfy the sky radiance comparison");
				Require(ReadEnvironmentSamples(retained) == originalPixels, "later filtering must not overwrite retained GPU maps");
				const auto beforeInvalidation = maps();
				sky->MarkDirty();
				for (uint32_t i = 0; i < 7; ++i)
				{
					process(true, true);
					if (i < 6) Require(maps() == beforeInvalidation, "explicit sky invalidation must retain the previous pair while capturing faces");
				}
				const auto afterInvalidation = maps();
				Require(afterInvalidation[0] != beforeInvalidation[0] && environment->NumCachedEnvironments() == 2,
					"explicit sky invalidation must complete a new raw capture without growing the filtered cache");
				for (uint32_t i = 1; i < afterInvalidation.size(); ++i)
					Require(afterInvalidation[i] == beforeInvalidation[i], "an unchanged completed sky key must reuse all three filtered maps");
				Require(environment->GetEnvironmentSkyParams(filtered) && filtered == second &&
					ReadEnvironmentSamples(afterInvalidation) == pixels,
					"cache restoration must publish matching parameters and preserve all sampled radiance");
			});
			for (uint32_t state = 3; state <= 7; ++state)
			{
				const auto expected = handoff(100.0f * state);
				onRender([&]()
				{
					for (uint32_t i = 0; i < 7; ++i) process(true, true);
					SkyParameters filtered;
					Require(environment->GetEnvironmentSkyParams(filtered) && filtered == expected &&
						environment->NumCachedEnvironments() == std::min(state, 4u),
						"completed successive skies must publish the current parameters within a four-entry cache");
				});
			}
			onRender([&]()
			{
				for (const auto& map : retained) Require(map.NumRefs() == 1, "evicted maps must be owned only by the explicit retained consumer");
				Require(ReadEnvironmentSamples(retained) == originalPixels, "eviction must not invalidate the retained consumer's images");
				const auto beforeFallback = maps();
				SkyParameters previous;
				Require(environment->GetEnvironmentSkyParams(previous), "the fallback transition must start with a completed sky");
				graph->GetGraph().RemoveAt(0);
				environment->MarkDirty();
				for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
				for (int stage = 0; stage < 4; ++stage)
				{
					process(false, true, stage, error);
					SkyParameters filtered;
					Require(environment->GetEnvironmentSkyParams(filtered) && filtered == previous && maps() == beforeFallback,
						"a refused constant fallback must not withdraw the parameters of the still-published sky maps");
				}
				process(false, true);
				SkyParameters filtered;
				Require(!environment->GetEnvironmentSkyParams(filtered) && environment->NumCachedEnvironments() == 1,
					"a completed constant fallback must replace the sky bundle and withdraw analytic sun parameters");
				for (const auto& pixel : ReadEnvironmentSamples(maps()))
					Require(glm::all(glm::lessThan(glm::abs(pixel - glm::vec3(0.03f)), glm::vec3(0.0001f))),
						"the constant fallback must initialize all four maps, six faces and every mip");
				graph->GetGraph().Add(sky);
				environment->MarkDirty();
				process(false, true);
				Require(environment->GetEnvironmentSkyParams(filtered) && filtered == previous && maps()[0] == beforeFallback[0],
					"returning from constant fallback must restore the actual completed sky source and parameters");
			});
			std::cout << "Sky environment publication: real component RenderQueue handoff, pending faces, twenty native refusals, paired parameters/maps, HDR scaling, explicit invalidation, bounded retirement and constant fallback passed\n";
		}
		catch (...)
		{
			onRender([&]() { productionGraph->GetGraph() = originalNodes; });
			skyWorld.Clear();
			throw;
		}
		onRender([&]() { productionGraph->GetGraph() = originalNodes; });
		skyWorld.Clear();
	}

	void TestAuthoredEnvironmentReload(const std::filesystem::path& workspace)
	{
		const auto path = workspace / "Content" / "AuthoredEnvironment.hdr";
		const auto writeHdr = [&](std::array<uint8_t, 4> rgbe)
		{
			const auto previous = std::filesystem::exists(path) ? std::filesystem::last_write_time(path) :
				std::filesystem::file_time_type{};
			std::ofstream output(path, std::ios::binary);
			output << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n";
			for (uint32_t i = 0; i < 8; ++i) output.write(reinterpret_cast<const char*>(rgbe.data()), rgbe.size());
			output.close();
			Require(static_cast<bool>(output), "the authored HDR fixture must be written");
			if (std::filesystem::last_write_time(path) <= previous)
				std::filesystem::last_write_time(path, previous + std::chrono::seconds(1));
		};
		writeHdr({ 128, 16, 8, 131 }); // (4, 0.5, 0.25), including radiance above one.
		TextureAssetInfo defaults;
		auto metadata = defaults.Serialize();
		const FileId id = FileId::CreateNewFileId();
		metadata["fileId"] = id;
		metadata["filename"] = path.filename().string();
		metadata["format"] = ETextureFormat::R32G32B32A32_SFLOAT;
		metadata["bShouldGenerateMips"] = false;
		{
			std::ofstream output(path.string() + ".asset");
			output << metadata;
			Require(static_cast<bool>(output), "the HDR metadata must be written");
		}
		auto registry = App::GetSubmodule<AssetRegistry>();
		auto importer = App::GetSubmodule<TextureImporter>();
		Require(registry->GetOrLoadFile(path.string()) == id, "the authored HDR must register");
		auto node = TRefPtr<EnvironmentNode>::Make();
		node->SetString("EnvironmentMap"_h, path.filename().string());
		auto graph = RHIFrameGraphPtr::Make();
		const StringHash names[] = { "g_rawEnvCubemap"_h, "g_envCubemap"_h, "g_irradianceCubemap"_h, "g_sheenEnvCubemap"_h };
		auto retained = RHIFrameGraphPtr::Make();
		constexpr uint32_t levels[] = { EnvironmentNode::EnvMapLevels, EnvironmentNode::EnvMapLevels, 1, EnvironmentNode::SheenEnvMapLevels };
		const auto onRender = [](const std::function<void()>& action)
		{
			auto task = Tasks::CreateTaskWithResult<std::string>("Authored environment regression"_h, [&]() -> std::string
			{
				try { action(); return {}; }
				catch (const std::exception& error) { return error.what(); }
			}, EThreadType::Render);
			task->Run();
			task->Wait();
			Require(task->GetResult().empty(), task->GetResult().c_str());
		};
		const auto process = [&]()
		{
			auto& driver = Renderer::GetDriver();
			auto commands = Renderer::GetDriverCommands();
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			node->Process(graph, upload, draw, {});
			CompleteCommands(upload, draw);
			driver->TrackResources_ThreadSafe();
		};
		const auto checkPixels = [&](RHIFrameGraphPtr source, glm::vec3 expected)
		{
			auto& driver = Renderer::GetDriver();
			auto commands = Renderer::GetDriverCommands();
			for (uint32_t channel = 0; channel < std::size(names); ++channel)
			{
				auto cube = source->GetSampler(names[channel]);
				Require(cube && cube->GetFormat() == EFormat::R16G16B16A16_SFLOAT, "all four HDR outputs must be available");
				for (uint32_t mip = 0; mip < levels[channel]; ++mip)
				{
					if (mip != 0 && mip != levels[channel] / 2 && mip != levels[channel] - 1) continue;
					const auto size = glm::max(cube->GetExtent() >> static_cast<int32_t>(mip), glm::ivec2(1));
					for (uint32_t face = 0; face < 6; ++face)
					{
						auto buffer = driver->CreateBuffer(size.x * size.y * 8u, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
						auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
						commands->BeginCommandList(command, true);
						commands->ImageMemoryBarrier(command, cube, EImageLayout::TransferSrcOptimal);
						command->m_vulkan.m_commandBuffer->CopyImageToBuffer(*buffer->m_vulkan.m_buffer->Get(),
							cube->m_vulkan.m_image, size.x, size.y, 1, mip, face);
						commands->ImageMemoryBarrier(command, cube, EImageLayout::ShaderReadOnlyOptimal);
						commands->MemoryBarrier(command, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
							static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
						commands->EndCommandList(command);
						Require(driver->SubmitCommandList_Immediate(command), "HDR pixel readback must complete");
						const auto pixels = static_cast<const uint32_t*>(buffer->GetPointer());
						for (int32_t i = 0; i < size.x * size.y; ++i)
						{
							const glm::vec3 actual(glm::vec4(glm::unpackHalf2x16(pixels[2 * i]), glm::unpackHalf2x16(pixels[2 * i + 1])));
							if (!glm::all(glm::lessThan(glm::abs(actual - expected), glm::vec3(0.02f))))
								throw std::runtime_error(names[channel].ToString() + " must match the current authored HDR; actual red " +
									std::to_string(actual.r) + ", expected " + std::to_string(expected.r));
						}
					}
				}
			}
		};
		onRender([&]()
		{
			for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				auto& driver = Renderer::GetDriver();
				auto commands = Renderer::GetDriverCommands();
				auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(draw, true);
				Tests::RequireAttachmentInitializationRefusal([&]() { node->Process(graph, upload, draw, {}); }, 0, error);
				Require(!graph->GetSampler("g_brdfSampler"_h) && !graph->GetSampler(names[0]) &&
					draw->GetNumRecordedCommands() == 0 && upload->GetNumRecordedCommands() == 0,
					"a refused BRDF target must not publish a sampler or record dependent environment work");
				CompleteCommands(upload, draw);
			}
			node->SetString("EnvironmentMap"_h, "MissingEnvironmentFixture.hdr");
			process();
			Require(graph->GetSampler("g_brdfSampler"_h).IsValid() && !graph->GetSampler(names[0]),
				"the BRDF must be ready before isolating the cold source upload");
			node->SetString("EnvironmentMap"_h, path.filename().string());
		});
		for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			Tests::RequireWorkerUploadRefusal([&]() { onRender(process); }, error);
			onRender([&]()
			{
				for (const auto name : names) Require(!graph->GetSampler(name), "failed source upload must not publish dependent environment maps");
			});
		}
		onRender([&]()
		{
			for (uint32_t frame = 0; frame < 16 && !graph->GetSampler(names[0]); ++frame) process();
			checkPixels(graph, { 4, 0.5f, 0.25f });
			Require(graph->GetSampler("g_brdfSampler"_h).IsValid(), "the same Environment node must retry its BRDF target");
			std::cout << "Environment BRDF target: native refusal, no dependent publication and same-node HDR pixel recovery passed\n";
			for (const auto name : names) retained->SetSampler(name, graph->GetSampler(name));
		});
		const auto texture = importer->GetLoadedTexture(id);
		Require(texture && texture->GetRHI(), "the unchanged Environment node must retry its failed TextureImporter source");
		const auto originalSource = texture->GetRHI();
		std::cout << "Environment source initialization: cold RHI-task refusal, no partial maps and unchanged-node HDR pixel recovery passed\n";
		writeHdr({ 16, 128, 32, 130 }); // (0.25, 2, 0.5).
		Require(App::UpdateAsset(id.ToString().c_str()), "the real HDR reload must complete");
		Require(importer->GetLoadedTexture(id) == texture && texture->GetRHI() != originalSource,
			"hot reload must retain the Texture object while replacing its published GPU image");
		onRender([&]()
		{
			const auto previousRaw = graph->GetSampler(names[0]);
			node->MarkDirty();
			Renderer::GetDriver()->WaitIdle();
			Renderer::GetDriver()->TrackResources_ThreadSafe();
			for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			for (uint32_t stage = 0; stage < 4; ++stage)
			{
				auto& driver = Renderer::GetDriver();
				auto commands = Renderer::GetDriverCommands();
				auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(draw, true);
				Tests::RequireAttachmentInitializationRefusal([&]() { node->Process(graph, upload, draw, {}); }, stage, error);
				for (const auto name : names)
					Require(graph->GetSampler(name) == retained->GetSampler(name),
						"a refused raw, specular, irradiance or sheen replacement must retain the entire published bundle");
				CompleteCommands(upload, draw);
			}
			checkPixels(retained, { 4, 0.5f, 0.25f });
			for (uint32_t frame = 0; frame < 16 && graph->GetSampler(names[0]) == previousRaw; ++frame)
			{
				process();
				if (graph->GetSampler(names[0]) == previousRaw)
					for (const auto name : names)
						Require(graph->GetSampler(name) == retained->GetSampler(name), "pending upload must retain the entire previous environment");
			}
			Require(graph->GetSampler(names[0]) != previousRaw, "completed HDR upload must replace the environment");
			checkPixels(graph, { 0.25f, 2, 0.5f });
			checkPixels(retained, { 4, 0.5f, 0.25f });
			std::cout << "Environment image initialization: all four native refusals, retained HDR bundle and unchanged-source pixel recovery passed\n";
			std::array<RHITexturePtr, 4> stable;
			for (uint32_t channel = 0; channel < stable.size(); ++channel) stable[channel] = graph->GetSampler(names[channel]);
			const auto brdf = graph->GetSampler("g_brdfSampler"_h);
			for (uint32_t repeat = 0; repeat < 32; ++repeat)
			{
				node->MarkDirty();
				process();
				for (uint32_t channel = 0; channel < stable.size(); ++channel)
					Require(graph->GetSampler(names[channel]) == stable[channel], "unchanged HDR invalidation must reuse raw and filtered resources");
				Require(graph->GetSampler("g_brdfSampler"_h) == brdf, "HDR invalidation must not regenerate the independent BRDF LUT");
			}
			checkPixels(graph, { 0.25f, 2, 0.5f });
		});
		{
			std::ofstream invalid(path);
			invalid << "invalid HDR";
		}
		Require(!App::UpdateAsset(id.ToString().c_str()), "malformed HDR reload must fail");
		onRender([&]()
		{
			node->MarkDirty();
			process();
			checkPixels(graph, { 0.25f, 2, 0.5f });
		});
		writeHdr({ 128, 16, 8, 131 });
		Require(App::UpdateAsset(id.ToString().c_str()) && importer->GetLoadedTexture(id) == texture,
			"repair must reload the same Texture object");
		onRender([&]()
		{
			const auto previousRaw = graph->GetSampler(names[0]);
			node->MarkDirty();
			for (uint32_t frame = 0; frame < 16 && graph->GetSampler(names[0]) == previousRaw; ++frame) process();
			Require(graph->GetSampler(names[0]) != previousRaw, "repaired HDR must replace the last good environment");
			checkPixels(graph, { 4, 0.5f, 0.25f });
			checkPixels(retained, { 4, 0.5f, 0.25f });
			SkyParameters sky;
			Require(!node->GetEnvironmentSkyParams(sky), "authored HDR must not publish unrelated analytic Sky lighting");
			node->SetString("EnvironmentMap"_h, "");
			node->MarkDirty();
			std::array<RHITexturePtr, 4> authored;
			for (uint32_t i = 0; i < authored.size(); ++i) authored[i] = graph->GetSampler(names[i]);
			for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			for (uint32_t stage = 0; stage < 4; ++stage)
			{
				auto& driver = Renderer::GetDriver();
				auto commands = Renderer::GetDriverCommands();
				auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(draw, true);
				Tests::RequireAttachmentInitializationRefusal([&]() { node->Process(graph, upload, draw, {}); }, stage, error);
				for (uint32_t i = 0; i < authored.size(); ++i)
					Require(graph->GetSampler(names[i]) == authored[i], "failed constant fallback must retain the previous authored bundle");
				CompleteCommands(upload, draw);
			}
			process();
			checkPixels(graph, glm::vec3(0.03f));
			std::cout << "Constant environment initialization: all four refused stages retain authored maps and recover complete fallback pixels passed\n";
			const auto fallback = graph->GetSampler(names[0]);
			for (uint32_t repeat = 0u; repeat < 8u; ++repeat)
			{
				node->MarkDirty();
				process();
				Require(graph->GetSampler(names[0]) == fallback, "constant fallback must reuse its raw cubemap");
			}
			node->SetString("EnvironmentMap"_h, path.filename().string());
			node->MarkDirty();
			for (uint32_t frame = 0u; frame < 16u && graph->GetSampler(names[0]) == fallback; ++frame) process();
			checkPixels(graph, { 4, 0.5f, 0.25f });
		});
		const std::array localNames{ "g_localEnvCubemap"_h, "g_localSheenEnvCubemap"_h };
		auto lightingView = RHISceneViewPtr::Make();
		lightingView->m_snapshots.Resize(2);
		auto lightingSubmission = RHIRenderSubmissionContextPtr::Make();
		World cameraWorld("LocalReflectionCamera", 0);
		auto camera = cameraWorld.Instantiate("Camera")->AddComponent<CameraComponent>();
		cameraWorld.GetECS<CameraECS>()->Tick(0);
		auto cameraData = camera->GetData();
		cameraData.SetOwner({});
		cameraWorld.Clear();
		for (uint32_t i = 0; i < 2; ++i)
		{
			auto& scene = lightingView->m_snapshots[i];
			scene.m_submissionContext = lightingSubmission;
			scene.m_cameraIndex = i;
			scene.m_camera = TUniquePtr<CameraData>::Make(cameraData);
			scene.m_bGlobalIlluminationEnabled = false;
		}
		uint64_t lightingFrame = 0;
		const auto checkLighting = [&](bool bReplaceBindings = false)
		{
			auto& driver = Renderer::GetDriver();
			auto commands = Renderer::GetDriverCommands();
			const auto expected = graph->GetGraph().IsEmpty() ? LocalReflectionParameters{} : node->GetLocalReflectionParameters();
			lightingSubmission->BeginSubmission(++lightingFrame, 0);
			if (bReplaceBindings)
			{
				for (auto& scene : lightingView->m_snapshots)
				{
					scene.m_rhiLightsData = driver->CreateShaderBindings();
				}
			}
			TVector<RHICommandListPtr> uploads, draws;
			RHISemaphorePtr ready;
			Require(graph->Process(lightingView, uploads, draws, {}, ready), "the local reflection graph must record both cameras");
			for (size_t i = 0; i < uploads.Num(); ++i)
			{
				for (const auto& command : { uploads[i], draws[i] })
				{
					auto next = driver->CreateWaitSemaphore();
					Require(driver->SubmitCommandList(command, RHIFencePtr::Make(), next, ready), "local reflection view commands must submit");
					ready = next;
				}
			}
			std::array<Memory::VulkanBufferMemoryPtr, 2> uniforms;
			for (uint32_t i = 0; i < 2; ++i)
			{
				auto bindings = lightingView->m_snapshots[i].m_rhiLightsData;
				uniforms[i] = *bindings->GetOrAddShaderBinding("localReflection"_h)->m_vulkan.m_valueBinding->Get();
				auto buffer = driver->CreateBuffer(sizeof(expected), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
				auto read = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(read, true);
				read->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
				read->m_vulkan.m_commandBuffer->CopyBuffer(uniforms[i], *buffer->m_vulkan.m_buffer->Get(), sizeof(expected));
				read->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
				commands->EndCommandList(read);
				auto finished = RHIFencePtr::Make();
				auto next = driver->CreateWaitSemaphore();
				Require(driver->SubmitCommandList(read, finished, next, ready) && finished->Wait(5000000000ull) == EFenceStatus::Finished,
					"local reflection uniform readback must complete");
				ready = next;
				const auto& actual = *static_cast<const LocalReflectionParameters*>(buffer->GetPointer());
				if (actual.m_positionBlend != expected.m_positionBlend || actual.m_minEnabled != expected.m_minEnabled ||
					actual.m_max != expected.m_max)
				{
					throw std::runtime_error("Local reflection uniform mismatch: frame=" + std::to_string(lightingFrame) +
						", camera=" + std::to_string(i) + ", expected position/enabled=" + std::to_string(expected.m_positionBlend.x) +
						"/" + std::to_string(expected.m_minEnabled.w) + ", actual=" + std::to_string(actual.m_positionBlend.x) +
						"/" + std::to_string(actual.m_minEnabled.w));
				}
				if (expected.m_minEnabled.w != 0.0f)
				{
					for (auto name : localNames)
					{
						Require(bindings->GetOrAddShaderBinding(name)->GetTextureBinding() == graph->GetSampler(name),
							"the uniform and sampled local maps must belong to the same completed capture");
					}
				}
			}
			Require(uniforms[0] != uniforms[1], "pending cameras must not share a writable local reflection uniform range");
		};
		onRender([&]()
		{
			checkLighting(); // A graph without Environment must initialize a disabled local capture.
			graph->GetGraph().Add(node);
		});
		const auto publishLocal = [&](uint32_t samples, glm::vec3 radiance)
		{
			LocalReflectionImage image;
			image.m_extent = { 4, 2 };
			image.m_parameters.m_positionBlend = { float(samples), 0, 0, 1 + float(samples) };
			image.m_parameters.m_minEnabled = { -10, -10, -10, 1 };
			image.m_parameters.m_max = { 10, 10, 10, 0 };
			image.m_samplesPerPixel = samples;
			image.m_pixels.Resize(8);
			for (auto& pixel : image.m_pixels) pixel = glm::vec4(radiance, 1);
			Require(node->SetLocalReflection(std::move(image)), "the actual local capture must enter the Render queue");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(EThreadType::Render);
		};
		const auto checkLocalPixels = [&](glm::vec3 expected)
		{
			auto& driver = Renderer::GetDriver();
			auto commands = Renderer::GetDriverCommands();
			for (const auto name : localNames)
			{
				auto cube = graph->GetSampler(name);
				Require(cube.IsValid(), "both local reflection maps must publish together");
				for (uint32_t mip = 0; mip < 8; ++mip)
				for (uint32_t face = 0; face < 6; ++face)
				{
					const uint32_t size = 128u >> mip;
					auto buffer = driver->CreateBuffer(size * size * 8u, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
					auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(command, true);
					commands->ImageMemoryBarrier(command, cube, EImageLayout::TransferSrcOptimal);
					command->m_vulkan.m_commandBuffer->CopyImageToBuffer(*buffer->m_vulkan.m_buffer->Get(),
						cube->m_vulkan.m_image, size, size, 1, mip, face);
					commands->ImageMemoryBarrier(command, cube, cube->GetDefaultLayout());
					commands->MemoryBarrier(command, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
						static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
					commands->EndCommandList(command);
					Require(driver->SubmitCommandList_Immediate(command), "local reflection readback must complete");
					const auto pixels = static_cast<const uint16_t*>(buffer->GetPointer());
					for (uint32_t i = 0; i < size * size; ++i)
					for (uint32_t channel = 0; channel < 3; ++channel)
						Require(std::abs(glm::unpackHalf1x16(pixels[4 * i + channel]) - expected[channel]) < 0.02f,
							"every local reflection face/mip pixel must match the accepted capture");
				}
			}
		};
		publishLocal(1, { 1, 0.5f, 0.25f });
		onRender([&]()
		{
			for (uint32_t frame = 0; frame < 16 && !node->IsLocalReflectionReady(); ++frame) process();
			Require(node->GetLocalReflectionSamples() == 1, "the initial local capture must publish");
			checkLocalPixels({ 1, 0.5f, 0.25f });
			checkLighting();
		});
		uint32_t samples = 1;
		for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			++samples;
			const glm::vec3 radiance(0.25f, float(samples), 0.5f);
			std::array<RHITexturePtr, 2> previous;
			onRender([&]() { for (uint32_t i = 0; i < previous.size(); ++i) previous[i] = graph->GetSampler(localNames[i]); });
			publishLocal(samples, radiance);
			onRender([&]()
			{
				const auto reject = [&](uint32_t stage)
				{
					auto& driver = Renderer::GetDriver();
					auto commands = Renderer::GetDriverCommands();
					auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(upload, true);
					commands->BeginCommandList(draw, true);
					Tests::RequireAttachmentInitializationRefusal([&]() { node->Process(graph, upload, draw, {}); }, stage, error);
					Require(draw->GetNumRecordedCommands() == 0 && node->GetLocalReflectionSamples() == samples - 1 &&
						node->IsLocalReflectionReady(), "failed local replacement must retain the previous ready capture and record no filter work");
					for (uint32_t i = 0; i < previous.size(); ++i)
						Require(graph->GetSampler(localNames[i]) == previous[i], "failed local replacement must retain both published map owners");
					CompleteCommands(upload, draw);
				};
				reject(0); // Upload.
				process();
				for (uint32_t stage = 0; stage < 3; ++stage) reject(stage);
				for (uint32_t frame = 0; frame < 16 && node->GetLocalReflectionSamples() != samples; ++frame) process();
				Require(node->GetLocalReflectionSamples() == samples, "the pending local capture must retry without another SetLocalReflection");
				checkLocalPixels(radiance);
				checkLighting();
				const auto bindings = lightingView->m_snapshots[0].m_rhiLightsData;
				checkLighting();
				Require(lightingView->m_snapshots[0].m_rhiLightsData == bindings, "warm local parameters must reuse lighting bindings");
				checkLighting(true);
				std::array<RHITexturePtr, 2> accepted;
				for (uint32_t i = 0; i < accepted.size(); ++i) accepted[i] = graph->GetSampler(localNames[i]);
				for (uint32_t frame = 0; frame < 8; ++frame) process();
				for (uint32_t i = 0; i < accepted.size(); ++i)
					Require(graph->GetSampler(localNames[i]) == accepted[i], "warm local reflection frames must retain the accepted pair");
			});
		}
		onRender([&]()
		{
			graph->GetGraph().Clear(false);
			checkLighting();
			graph->GetGraph().Add(node);
			checkLighting();
		});
		node->ResetLocalReflection();
		App::GetSubmodule<Tasks::Scheduler>()->WaitIdle(EThreadType::Render);
		onRender([&]()
		{
			checkLighting();
			checkLighting();
			graph->Clear();
			checkLighting();
		});
		std::cout << "Local reflection initialization: upload/three-map refusal, retained capture, Render-queue retry and all face/mip pixels passed\n";
		std::cout << "Local reflection lighting: two camera GPU uniforms, matching maps, warm reuse, replaced bindings, reset and absent node passed\n";
		std::cout << "Authored HDR reload: four-map pixels, retained consumers, failed reload/repair and 32 warm invalidations passed\n";
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
			node->SetRHIResource("color"_h, driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT));
			graph->GetGraph().Add(node);
			TVector<RHICommandListPtr> transfers, graphics;
			RHISemaphorePtr ready;
			Require(graph->Process(RHISceneViewPtr::Make(), transfers, graphics, {}, ready), "lifetime fixture must prepare its graph");
			auto target = node->GetTargetAttachment("color"_h, graph.GetRawPtr());
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
		Require(driver->AddSamplerToShaderBindings(bindings, "depthSampler"_h, texture->GetDepthAspect(), 0).IsValid(),
			"readback must bind the real depth view");
		if (stencil)
		{
			auto stencilView = RHITexturePtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false);
			stencilView->m_vulkan = texture->GetStencilAspect()->m_vulkan;
			Require(driver->AddSamplerToShaderBindings(bindings, "stencilSampler"_h, stencilView, 1).IsValid(),
				"readback must bind the integer stencil view");
		}
		Require(driver->AddBufferToShaderBindings(bindings, buffer, "outputValues"_h, 2).IsValid(), "depth readback storage must bind");
		commands->MemoryBarrier(command, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit), static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit));
		commands->ImageMemoryBarrierForComputeSampling(command, texture);
		commands->Dispatch(command, shaders[(samples > 1 ? 1 : 0) | (stencil ? 2 : 0)]->GetComputeShaderRHI(),
			(size.x + 7) / 8, (size.y + 7) / 8, 1, { bindings });
		commands->MemoryBarrier(command, static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
		return buffer;
	}

	void TestDepthPrepassCulling(ShaderSetPtr sourceShader, ShaderSetPtr indexReadback,
		const std::array<ShaderSetPtr, 4>& depthReadback, bool named, bool surface, bool paged)
	{
		constexpr uint32_t Count = Renderer::GPUCullingGroupSize + 3;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto mesh = graph->GetFullscreenNdcQuad();
		mesh->m_bounds = Math::AABB(glm::vec3(0), glm::vec3(1, 1, 0));
		const RenderState state(true, true, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, "Opaque"_h.GetHash(), true);
		auto material = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, sourceShader);
		Require(material && material->GetVersion(), "culling fixture needs a complete source material");
		RHISceneViewProxy source;
		for (uint32_t i = 0; i < Count; ++i)
		{
			glm::mat4 model(1);
			model[3] = glm::vec4(i % 5 < 3 ? 0 : 1000, 0, -2, 1);
			source.m_meshes.Add(mesh);
			source.m_meshModelMatrices.Add(model);
			source.m_overrideMaterials.Add(material);
			source.m_renderQueueTags.Add("Opaque"_h.GetHash());
		}
		RHISceneInstanceRecord record;
		record.m_producerKey = 1;
		record.m_mobility = EMobilityType::Static;
		record.m_worldMatrix = glm::mat4(1);
		record.m_worldBounds = Math::AABB(glm::vec3(500, 0, -2), glm::vec3(502, 2, 2));
		record.m_topology = RHISceneProxyResourcePtr::Make(std::move(source));
		auto scene = RHIScenePtr::Make();
		scene->AddInstance(record);
		RHISceneViewSnapshot snapshot;
		snapshot.m_sceneVersions = TSharedPtr<const TVector<RHISceneVersionPtr>>::Make(TVector<RHISceneVersionPtr>{ scene->PublishVersion() });
		snapshot.ForEachSceneProxy(record.m_mobility, [&](const RHIVisibleSceneProxy& proxy) { snapshot.m_proxies.Add(proxy); });
		snapshot.m_camera = TUniquePtr<CameraData>::Make();
		snapshot.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		auto depth = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::D32_SFLOAT, ETextureFiltration::Nearest,
			ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit);
		auto depthSurface = driver->CreateSurface(depth);
		auto node = TRefPtr<DepthNode>::Make();
		node->SetString("Tag"_h, "Opaque");
		node->SetString("ClearDepth"_h, "true");
		node->SetString("VirtualizeInstancePayloads"_h, paged ? "true" : "false");
		node->SetRHIResource("depthStencil"_h, depthSurface);
		if (named) node->SetRHIResource_Unresolved("depthHighZ"_h, "SelectedHighZ"_h);
		RHITexturePtr highZ;
		struct Recorded
		{
			RHICommandListPtr m_upload, m_draw;
			RHIRenderSubmissionContextPtr m_context;
			RHIBufferPtr m_indices, m_indirect, m_depth;
			TVector<uint32_t> m_candidates, m_visible;
			uint32_t m_firstInstance = 0, m_samples = 0;
			float m_expectedDepth = 0;
		};
		const auto complete = [&](const Recorded& recorded)
		{
			CompleteCommands(recorded.m_upload, recorded.m_draw);
			const auto& indirect = *static_cast<const DrawIndexedIndirectData*>(recorded.m_indirect->GetPointer());
			if (indirect.m_instanceCount != recorded.m_visible.Num())
				throw std::runtime_error("depth culling indirect count=" + std::to_string(indirect.m_instanceCount) +
					", expected=" + std::to_string(recorded.m_visible.Num()));
			Require(indirect.m_firstInstance == recorded.m_firstInstance && indirect.m_indexCount == mesh->GetIndexCount() &&
				indirect.m_firstIndex == mesh->GetFirstIndex() && indirect.m_vertexOffset == mesh->GetVertexOffset(),
				"depth culling must preserve indirect geometry and select the correct index range");
			const auto* indices = static_cast<const uint32_t*>(recorded.m_indices->GetPointer());
			for (uint32_t i = 0; i < Count; ++i)
				Require(indices[i] == recorded.m_candidates[i], "depth culling must retain all immutable candidate IDs");
			const auto offset = recorded.m_visible.Num() == Count ? 0 : Count;
			for (uint32_t i = 0; i < recorded.m_visible.Num(); ++i)
				Require(indices[offset + i] == recorded.m_visible[i], "depth culling must compact exact visible IDs in order");
			const auto* pixels = static_cast<const glm::vec2*>(recorded.m_depth->GetPointer());
			for (uint32_t y = 0; y < Side; ++y)
			for (uint32_t x = 0; x < Side; ++x)
			for (uint32_t sample = 0; sample < recorded.m_samples; ++sample)
			{
				const float expected = x >= Side / 4 && x < 3 * Side / 4 && y >= Side / 4 && y < 3 * Side / 4 ? recorded.m_expectedDepth : 0;
				Require(std::abs(pixels[(y * Side + x) * recorded.m_samples + sample].x - expected) < 0.00001f,
					"culling on/off and camera changes must preserve every depth sample");
			}
		};
		Recorded pending;
		RHIShaderBindingSetPtr warmBindings;
		for (uint32_t frame = 0; frame < 8; ++frame)
		{
			if (frame == 2 || frame == 3) snapshot.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
			if (frame == 5) node->Clear();
			if (frame != 1 && frame != 6 && frame != 7)
			{
				RHIResourcePtr input;
				if (frame != 4)
				{
					auto target = driver->CreateRenderTarget({ 9 + int(frame), 7 + int(frame) }, 1, EFormat::R32_SFLOAT,
						ETextureFiltration::Nearest, ETextureClamping::Clamp);
					highZ = target;
					input = surface ? RHIResourcePtr(driver->CreateSurface(target)) : RHIResourcePtr(target);
				}
				else highZ.Clear();
				if (!named) node->SetRHIResource("depthHighZ"_h, input);
				else if (surface) graph->SetSurface("SelectedHighZ"_h, input.DynamicCast<RHISurface>());
				else graph->SetRenderTarget("SelectedHighZ"_h, input.DynamicCast<RHIRenderTarget>());
			}
			node->SetString("GPUCulling"_h, frame == 6 ? "false" : "true");
			const bool bGpuExpected = frame != 4 && frame != 6;
			const float cameraX = frame == 2 || frame == 3 || frame == 6 ? 1000.0f : 0.0f;
			UboFrameData frameData{};
			frameData.m_view = glm::mat4(1);
			frameData.m_view[3].x = -cameraX;
			frameData.m_projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
			frameData.m_invProjection = glm::inverse(frameData.m_projection);
			frameData.m_viewportSize = glm::ivec2(Side);
			frameData.m_cameraZNearZFar = { 0.1f, 100.0f };
			snapshot.m_frameBindings = driver->CreateShaderBindings();
			auto frameBuffer = driver->CreateBuffer(sizeof(frameData), EBufferUsageBit::UniformBuffer_Bit, HostMemory);
			std::memcpy(frameBuffer->GetPointer(), &frameData, sizeof(frameData));
			driver->AddBufferToShaderBindings(snapshot.m_frameBindings, frameBuffer, "frame"_h, 0);
			snapshot.PrepareLods(frameData.m_view, frameData.m_projection);
			snapshot.m_frame = frame + 1;
			const uint64_t submission = 302000 + frame;
			snapshot.m_submissionContext->BeginSubmission(submission, 0, RHIMaterial::BeginSubmissionVersionCapture(submission));
			auto prepare = node->Prepare(graph, snapshot);
			prepare->Run();
			prepare->Wait();
			RHIMaterial::EndSubmissionVersionCapture(submission);
			auto resources = node->GetResources(snapshot);
			Require(resources->m_packet.GetNumDrawInstances() == Count && resources->m_packet.GetGroups().Num() == 1 &&
				resources->m_customPacket.GetNumInstances() == 0, "all culling candidates must enter the real compact-depth packet");
			Recorded recorded;
			recorded.m_context = snapshot.m_submissionContext;
			recorded.m_upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			recorded.m_draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(recorded.m_upload, true);
			commands->BeginCommandList(recorded.m_draw, true);
			if (highZ)
			{
				commands->ImageMemoryBarrier(recorded.m_upload, highZ, EImageLayout::TransferDstOptimal);
				commands->ClearImage(recorded.m_upload, highZ, glm::vec4(frame % 2));
				commands->ImageMemoryBarrierForComputeSampling(recorded.m_upload, highZ);
			}
			node->Process(graph, recorded.m_upload, recorded.m_draw, snapshot);
			const auto binding = resources->m_perInstanceData->GetOrAddShaderBinding("indices"_h);
			const uint32_t first = binding->GetStorageInstanceIndex();
			recorded.m_firstInstance = first + (bGpuExpected ? Count : 0);
			recorded.m_indirect = resources->m_indirectBuffers[0];
			recorded.m_candidates = resources->m_packet.m_resolvedInstanceIndices;
			for (uint32_t i = 0; i < Count; ++i)
				if (!bGpuExpected || GetSingleMobilityInstance(resources->m_packet, record.m_mobility, i).model[3].x == cameraX)
					recorded.m_visible.Add(recorded.m_candidates[i]);
			recorded.m_indices = driver->CreateBuffer(2 * Count * sizeof(uint32_t), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
			auto outputBindings = driver->CreateShaderBindings();
			driver->AddBufferToShaderBindings(outputBindings, recorded.m_indices, "outputValues"_h, 0);
			commands->MemoryBarrier(recorded.m_draw, static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit) | static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit));
			commands->Dispatch(recorded.m_draw, indexReadback->GetComputeShaderRHI(), bGpuExpected ? 2 * Count : Count, 1, 1,
				{ resources->m_perInstanceData, outputBindings }, &first, sizeof(first));
			commands->MemoryBarrier(recorded.m_draw, static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit) | static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			recorded.m_depth = ReadDepth(recorded.m_draw, depthSurface->GetTarget(), depthReadback);
			recorded.m_samples = static_cast<uint32_t>(depthSurface->GetTarget()->GetMsaaSamples());
			const auto projected = frameData.m_projection * glm::vec4(0, 0, -2, 1);
			recorded.m_expectedDepth = projected.z / projected.w;
			if (frame == 2) pending = recorded;
			else
			{
				if (frame == 3) complete(pending);
				complete(recorded);
				if (bGpuExpected)
				{
					const RHIShaderBindingPtr* sampled = nullptr;
					Require(resources->m_computeMeshCullingBindings &&
						resources->m_computeMeshCullingBindings->GetShaderBindings().Find("depthHighZ"_h, sampled) &&
						(*sampled)->GetTextureBinding() == highZ, "culling bindings must retain the current resolved Hi-Z image");
					if (frame == 0) warmBindings = resources->m_computeMeshCullingBindings;
					if (frame == 1) Require(warmBindings == resources->m_computeMeshCullingBindings,
						"unchanged Hi-Z must reuse its culling bindings");
				}
			}
		}
		std::cout << "Depth prepass culling named=" << named << " surface=" << surface << " paged=" << paged <<
			": eight frames, 259 candidates, exact GPU indices/counts, depth samples, camera/input replacement and retained commands passed\n";
	}

	void TestDepthPrepassOcclusion(ShaderSetPtr sourceShader, ShaderSetPtr indexReadback,
		const std::array<ShaderSetPtr, 4>& depthReadback, bool named, bool paged)
	{
		constexpr uint32_t Count = Renderer::GPUCullingGroupSize + 3;
		constexpr uint32_t Extent = 32;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto mesh = graph->GetFullscreenNdcQuad();
		mesh->m_bounds = Math::AABB(glm::vec3(0), glm::vec3(1, 1, 0));
		auto* vertices = static_cast<VertexP3N3UV2C4*>(mesh->m_vertexBuffer->GetPointer());
		for (uint32_t i = 0; i < 4; ++i) vertices[i].m_color = glm::vec4(1);
		const auto makeMaterial = [&](StringHash tag)
		{
			const RenderState state(true, true, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, tag.GetHash(), true);
			return driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, sourceShader);
		};
		auto opaque = makeMaterial("Opaque"_h);
		auto masked = makeMaterial("Masked"_h);
		auto seed = TRefPtr<DepthNode>::Make();
		auto node = TRefPtr<DepthNode>::Make();
		auto pyramidNode = TRefPtr<DepthHighZNode>::Make();
		auto depth = driver->CreateSurface(glm::ivec2(Extent), 1, EFormat::D32_SFLOAT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit);
		for (auto pass : { seed, node })
		{
			pass->SetRHIResource("depthStencil"_h, depth);
			pass->SetString("VirtualizeInstancePayloads"_h, paged ? "true" : "false");
		}
		seed->SetString("Tag"_h, "Opaque");
		seed->SetString("ClearDepth"_h, "true");
		seed->SetString("GPUCulling"_h, "false");
		node->SetString("Tag"_h, "Masked");
		pyramidNode->SetRHIResource("src"_h, depth);
		if (named)
		{
			node->SetRHIResource_Unresolved("depthHighZ"_h, "Pyramid"_h);
			pyramidNode->SetRHIResource_Unresolved("dst"_h, "Pyramid"_h);
		}
		auto context = RHIRenderSubmissionContextPtr::Make();
		auto scene = RHIScenePtr::Make();
		RenderInstanceHandle handle;
		RHIRenderTargetPtr pyramid;
		for (uint32_t frame = 0; frame < 8; ++frame)
		{
			if (frame == 0 || frame == 5)
			{
				pyramid = driver->CreateRenderTarget(glm::ivec2(Extent / 2), 5, EFormat::R32_SFLOAT,
					ETextureFiltration::Nearest, ETextureClamping::Clamp);
				if (named) graph->SetRenderTarget("Pyramid"_h, pyramid);
				else
				{
					node->SetRHIResource("depthHighZ"_h, pyramid);
					pyramidNode->SetRHIResource("dst"_h, pyramid);
				}
			}
			if (frame == 6) { node->Clear(); pyramidNode->Clear(); }
			const float cameraX = frame == 3 ? 1000.0f : 0.0f;
			UboFrameData data{};
			data.m_view = glm::translate(glm::mat4(1), glm::vec3(-cameraX, 0, 0));
			data.m_projection = Math::PerspectiveRH(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
			data.m_invProjection = glm::inverse(data.m_projection);
			data.m_viewportSize = glm::ivec2(Extent);
			data.m_cameraZNearZFar = { 0.1f, 100.0f };
			auto frameBindings = driver->CreateShaderBindings();
			auto frameBuffer = driver->CreateBuffer(sizeof(data), EBufferUsageBit::UniformBuffer_Bit, HostMemory);
			std::memcpy(frameBuffer->GetPointer(), &data, sizeof(data));
			driver->AddBufferToShaderBindings(frameBindings, frameBuffer, "frame"_h, 0);
			RHISceneViewProxy source;
			for (uint32_t i = 0; i <= Count; ++i)
			{
				const bool bOccluder = i == Count;
				const glm::vec3 position = bOccluder ? glm::vec3(frame == 2 || frame == 4 ? 1000 : -2, 0, -4) :
					i % 4 == 0 ? glm::vec3(-2, 0, -6) : i % 4 == 1 ? glm::vec3(i % 8 == 1 ? 0 : 2, 0, -6) :
					i % 4 == 2 ? glm::vec3(-0.5f, 0, -2) : glm::vec3(1000, 0, -6);
				source.m_meshes.Add(mesh);
				source.m_meshModelMatrices.Add(glm::scale(glm::translate(glm::mat4(1), position),
					bOccluder ? glm::vec3(2, 4, 1) : glm::vec3(0.25f)));
				source.m_overrideMaterials.Add(bOccluder ? opaque : masked);
				source.m_renderQueueTags.Add((bOccluder ? "Opaque"_h : "Masked"_h).GetHash());
			}
			RHISceneInstanceRecord record;
			record.m_producerKey = 1;
			record.m_mobility = EMobilityType::Static;
			record.m_worldMatrix = glm::mat4(1);
			record.m_worldBounds = Math::AABB(glm::vec3(500, 0, -4), glm::vec3(510, 10, 10));
			record.m_topology = RHISceneProxyResourcePtr::Make(std::move(source));
			if (frame == 0) handle = scene->AddInstance(record);
			else Require(scene->UpdateInstance(handle, record, ToMask(ESceneChangeBit::MeshOrLodTopology)),
				"moving the occluder must publish the updated mesh transforms");
			RHISceneViewSnapshot snapshot;
			snapshot.m_sceneVersions = TSharedPtr<const TVector<RHISceneVersionPtr>>::Make(TVector<RHISceneVersionPtr>{ scene->PublishVersion() });
			snapshot.ForEachSceneProxy(record.m_mobility, [&](const RHIVisibleSceneProxy& proxy) { snapshot.m_proxies.Add(proxy); });
			snapshot.m_camera = TUniquePtr<CameraData>::Make();
			snapshot.m_frameBindings = frameBindings;
			snapshot.m_submissionContext = context;
			snapshot.m_frame = frame + 1;
			snapshot.PrepareLods(data.m_view, data.m_projection);
			const uint64_t submission = 302100 + frame;
			context->BeginSubmission(submission, 0, RHIMaterial::BeginSubmissionVersionCapture(submission));
			for (auto pass : { seed, node })
			{
				auto prepare = pass->Prepare(graph, snapshot);
				prepare->Run();
				prepare->Wait();
			}
			RHIMaterial::EndSubmissionVersionCapture(submission);
			auto resources = node->GetResources(snapshot);
			if (seed->GetResources(snapshot)->m_packet.GetNumDrawInstances() != 1 ||
				resources->m_packet.GetNumDrawInstances() != Count || resources->m_packet.GetGroups().Num() != 1)
				throw std::runtime_error("Hi-Z fixture frame=" + std::to_string(frame) + " seed=" +
					std::to_string(seed->GetResources(snapshot)->m_packet.GetNumDrawInstances()) + " masked=" +
					std::to_string(resources->m_packet.GetNumDrawInstances()) + " groups=" + std::to_string(resources->m_packet.GetGroups().Num()));
			std::array<RHIBufferPtr, 2> depths;
			for (uint32_t reference = 0; reference < 2; ++reference)
			{
				node->SetString("GPUCulling"_h, reference ? "false" : "true");
				node->SetString("OcclusionCulling"_h, frame == 7 ? "false" : "true");
				auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(draw, true);
				// Upload-list culling would see an empty pyramid and miss the occluder.
				if (frame != 2) ClearColor(upload, pyramid, glm::vec4(0));
				graph->ResetCurrentDepthPyramids();
				seed->Process(graph, upload, draw, snapshot);
				if (frame != 2) pyramidNode->Process(graph, upload, draw, snapshot);
				node->Process(graph, upload, draw, snapshot);
				const auto indexBinding = resources->m_perInstanceData->GetOrAddShaderBinding("indices"_h);
				const uint32_t first = indexBinding->GetStorageInstanceIndex() + (reference ? 0 : Count);
				auto indices = driver->CreateBuffer(Count * sizeof(uint32_t), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
				auto outputBindings = driver->CreateShaderBindings();
				driver->AddBufferToShaderBindings(outputBindings, indices, "outputValues"_h, 0);
				commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit) | static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
					static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit));
				commands->Dispatch(draw, indexReadback->GetComputeShaderRHI(), Count, 1, 1,
					{ resources->m_perInstanceData, outputBindings }, &first, sizeof(first));
				commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
				depths[reference] = ReadDepth(draw, depth->GetTarget(), depthReadback);
				CompleteCommands(upload, draw);
				TVector<uint32_t> expected;
				for (uint32_t i = 0; i < Count; ++i)
				{
					const auto center = GetSingleMobilityInstance(resources->m_packet, record.m_mobility, i).model[3];
					const bool bInFrustum = std::abs(center.x - cameraX) < 10;
					const bool bOccluded = frame != 2 && frame != 3 && frame != 4 && frame != 7 && center.x == -2;
					if (reference || (bInFrustum && !bOccluded)) expected.Add(resources->m_packet.m_resolvedInstanceIndices[i]);
				}
				const auto& indirect = *static_cast<const DrawIndexedIndirectData*>(resources->m_indirectBuffers[0]->GetPointer());
				if (indirect.m_instanceCount != expected.Num())
					throw std::runtime_error("Hi-Z prepass frame=" + std::to_string(frame) + " count=" +
						std::to_string(indirect.m_instanceCount) + ", expected=" + std::to_string(expected.Num()));
				const auto* actual = static_cast<const uint32_t*>(indices->GetPointer());
				for (uint32_t i = 0; i < expected.Num(); ++i)
					Require(actual[i] == expected[i], "Hi-Z must compact exact visible IDs, retaining foreground and uncovered objects");
			}
			const uint32_t samples = static_cast<uint32_t>(depth->GetTarget()->GetMsaaSamples());
			const auto* actual = static_cast<const glm::vec2*>(depths[0]->GetPointer());
			const auto* reference = static_cast<const glm::vec2*>(depths[1]->GetPointer());
			bool bHasDepth = false;
			for (uint32_t pixel = 0; pixel < Extent * Extent * samples; ++pixel)
			{
				Require(std::abs(actual[pixel].x - reference[pixel].x) < 0.00001f,
					"Hi-Z-culled depth must match unculled drawing in every MSAA sample");
				bHasDepth |= reference[pixel].x > 0;
			}
			Require(bHasDepth, "occlusion readback must contain rasterized geometry");
		}
		std::cout << "Depth prepass Hi-Z named=" << named << " paged=" << paged <<
			": eight frames, exact GPU counts/IDs, moving occluder/camera, stale pyramid, replacement and unculled depth parity passed\n";
	}

	enum class DefaultDepthPublication { Target, Surface, SurfaceOnly, SurfaceWithDecoy };

	void TestDefaultDepthAttachment(ShaderSetPtr shader, const std::array<ShaderSetPtr, 4>& depthReadback,
		bool prepass, uint32_t colorKind, DefaultDepthPublication publication, bool named)
	{
		CaptureAttachments capture;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto mesh = graph->GetFullscreenNdcQuad();
		auto* vertices = static_cast<VertexP3N3UV2C4*>(mesh->m_vertexBuffer->GetPointer());
		for (uint32_t i = 0; i < 4; ++i) vertices[i].m_position.z = 0.5f;
		const RenderState state(true, true, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, true);
		auto material = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shader);
		RHIBatch batch(material, mesh);
		batch.m_textureBindings = driver->CreateShaderBindings();
		auto depthNode = TRefPtr<DepthNode>::Make();
		auto sceneNode = TRefPtr<SceneNode>::Make();
		FrameGraphNodePtr node = prepass ? FrameGraphNodePtr(depthNode) : FrameGraphNodePtr(sceneNode);
		node->SetString("Tag"_h, "Opaque");
		node->SetString("GPUCulling"_h, "false");
		const auto name = named ? "SelectedDepth"_h : "DepthBuffer"_h;
		if (named) node->SetRHIResource_Unresolved("depthStencil"_h, name);
		const bool bSurface = publication != DefaultDepthPublication::Target;
		const bool bSingleSample = !prepass && colorKind == 2;
		const bool bMsaa = VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
		struct Recorded
		{
			RHICommandListPtr m_upload, m_draw;
			RHIRenderSubmissionContextPtr m_context;
			RHIBufferPtr m_depth, m_resolved, m_color, m_unusedDepth;
			glm::ivec2 m_extent;
			uint32_t m_samples = 1, m_unusedSamples = 1, m_frame = 0;
			float m_clearDepth = 0;
			bool m_bAttachmentsMatch = false;
		};
		const auto complete = [&](const Recorded& recorded)
		{
			CompleteCommands(recorded.m_upload, recorded.m_draw);
			const auto* live = static_cast<const glm::vec2*>(recorded.m_depth->GetPointer());
			const auto* resolved = static_cast<const glm::vec2*>(recorded.m_resolved->GetPointer());
			for (int y = 0; y < recorded.m_extent.y; ++y)
			for (int x = 0; x < recorded.m_extent.x; ++x)
			{
				const uint32_t pixel = y * recorded.m_extent.x + x;
				const bool bVisible = recorded.m_clearDepth < 0.5f && (prepass || x < 4);
				const float expected = bVisible ? 0.5f : recorded.m_clearDepth;
				if (!(std::abs(resolved[pixel].x - expected) < 0.00001f))
					throw std::runtime_error("default depth frame=" + std::to_string(recorded.m_frame) + " pixel=" +
						std::to_string(pixel) + " actual=" + std::to_string(resolved[pixel].x) + ", expected=" + std::to_string(expected));
				for (uint32_t sample = 0; sample < recorded.m_samples; ++sample)
					if (!(std::abs(live[pixel * recorded.m_samples + sample].x - expected) < 0.00001f))
						throw std::runtime_error("default live depth frame=" + std::to_string(recorded.m_frame) + " pixel=" +
							std::to_string(pixel) + " sample=" + std::to_string(sample) + " actual=" +
							std::to_string(live[pixel * recorded.m_samples + sample].x) + ", expected=" + std::to_string(expected));
				if (recorded.m_unusedDepth)
				{
					const auto* unused = static_cast<const glm::vec2*>(recorded.m_unusedDepth->GetPointer());
					for (uint32_t sample = 0; sample < recorded.m_unusedSamples; ++sample)
						Require(unused[pixel * recorded.m_unusedSamples + sample].x == 0.125f,
							"a forced single-sample pass must leave the live MSAA depth untouched");
				}
				if (recorded.m_color)
				{
					const auto actual = static_cast<const glm::vec4*>(recorded.m_color->GetPointer())[pixel];
					Require(actual == (bVisible ? glm::vec4(0.75f, 0.5f, 0.25f, 1) : glm::vec4(-1)),
						"scene color must obey the selected depth, including rejected geometry");
				}
			}
			Require(recorded.m_bAttachmentsMatch, "default depth must retain the explicit attachment's native target/resolve views");
		};
		RHIRenderTargetPtr resolved, target, decoy, color, colorTarget;
		RHISurfacePtr surface;
		Recorded pending;
		for (uint32_t frame = 0; frame < 6; ++frame)
		{
			const glm::ivec2 extent(frame < 2 ? 8 : 12, frame < 4 ? 8 : 12);
			if (frame != 1 && frame != 5)
			{
				const auto usage = ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit;
				resolved = driver->CreateRenderTarget(extent, 1, EFormat::D32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
				surface = bSurface ? driver->CreateSurface(resolved) : RHISurfacePtr{};
				decoy = driver->CreateRenderTarget(extent, 1, EFormat::D32_SFLOAT_S8_UINT, ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
				graph->SetSurface(name, surface);
				graph->SetRenderTarget(name, publication == DefaultDepthPublication::SurfaceOnly ? RHIRenderTargetPtr{} :
					publication == DefaultDepthPublication::SurfaceWithDecoy ? decoy : resolved);
				target = bSingleSample ? resolved : surface ? surface->GetTarget() : bMsaa ?
					driver->GetOrAddMsaaFramebufferRenderTarget(resolved->GetFormat(), extent).DynamicCast<RHIRenderTarget>() : resolved;
				color = driver->CreateRenderTarget(extent, 1, EFormat::R32G32B32A32_SFLOAT);
				const auto colorSurface = colorKind == 2 ? RHISurfacePtr::Make(color, color, false) :
					colorKind == 1 ? driver->CreateSurface(color) : RHISurfacePtr{};
				colorTarget = colorSurface ? colorSurface->GetTarget() : bMsaa ?
					driver->GetOrAddMsaaFramebufferRenderTarget(color->GetFormat(), extent).DynamicCast<RHIRenderTarget>() : color;
				node->SetRHIResource("color"_h, colorSurface ? RHIResourcePtr(colorSurface) : RHIResourcePtr(color));
			}
			RHISceneViewSnapshot scene;
			scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
			scene.m_submissionContext->BeginSubmission(305000 + frame, 0);
			scene.m_frameBindings = driver->CreateShaderBindings();
			scene.m_rhiLightsData = driver->CreateShaderBindings();
			UboFrameData data{};
			data.m_view = data.m_projection = data.m_invProjection = glm::mat4(1);
			data.m_viewportSize = extent;
			auto buffer = driver->CreateBuffer(sizeof(data), EBufferUsageBit::UniformBuffer_Bit, HostMemory);
			std::memcpy(buffer->GetPointer(), &data, sizeof(data));
			driver->AddBufferToShaderBindings(scene.m_frameBindings, buffer, "frame"_h, 0);
			if (prepass)
			{
				DepthPrepassNode::PerInstanceData instance{};
				instance.model = glm::mat4(1);
				auto& packet = depthNode->GetResources(scene)->m_packet;
				packet.Add(batch, mesh, instance);
				packet.Finalize();
			}
			else
			{
				RenderSceneNode::PerInstanceData instance{};
				instance.model = glm::mat4(1);
				auto& packet = sceneNode->GetResources(scene)->m_packet;
				packet.Add(batch, mesh, instance);
				packet.Finalize();
			}
			Recorded recorded;
			recorded.m_context = scene.m_submissionContext;
			recorded.m_extent = extent;
			recorded.m_frame = frame;
			recorded.m_clearDepth = frame % 2 ? 0.75f : 0.25f;
			recorded.m_samples = uint32_t(target->GetMsaaSamples());
			recorded.m_upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			recorded.m_draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(recorded.m_upload, true);
			commands->BeginCommandList(recorded.m_draw, true);
			const auto clearDepth = [&](RHITexturePtr image, float value)
			{
				commands->ImageMemoryBarrier(recorded.m_draw, image, EImageLayout::TransferDstOptimal);
				commands->ClearDepthStencil(recorded.m_draw, image, value, 0);
				commands->ImageMemoryBarrier(recorded.m_draw, image, image->GetDefaultLayout());
			};
			clearDepth(resolved, recorded.m_clearDepth);
			clearDepth(target, recorded.m_clearDepth);
			clearDepth(decoy, 1 - recorded.m_clearDepth);
			if (surface && bMsaa)
				clearDepth(driver->GetOrAddMsaaFramebufferRenderTarget(resolved->GetFormat(), extent), 1 - recorded.m_clearDepth);
			if (bMsaa) clearDepth(driver->GetOrAddMsaaFramebufferRenderTarget(decoy->GetFormat(), extent), 1 - recorded.m_clearDepth);
			if (bSingleSample && surface && surface->NeedsResolve()) clearDepth(surface->GetTarget(), 0.125f);
			for (auto image : { color, colorTarget }) ClearColor(recorded.m_draw, image, glm::vec4(-1));
			recordedDepth = {};
			node->Process(graph, recorded.m_upload, recorded.m_draw, scene);
			recorded.m_bAttachmentsMatch = node->GetDrawCallStats().m_numInstances == 1 &&
				recordedDepth.imageView == static_cast<VkImageView>(*target->m_vulkan.m_imageView) &&
				recordedDepth.resolveImageView == (target != resolved ? static_cast<VkImageView>(*resolved->m_vulkan.m_imageView) : VK_NULL_HANDLE);
			recorded.m_depth = ReadDepth(recorded.m_draw, target, depthReadback);
			recorded.m_resolved = ReadDepth(recorded.m_draw, resolved, depthReadback);
			if (!prepass) recorded.m_color = ReadColor(recorded.m_draw, color);
			if (bSingleSample && surface && surface->NeedsResolve())
			{
				recorded.m_unusedDepth = ReadDepth(recorded.m_draw, surface->GetTarget(), depthReadback);
				recorded.m_unusedSamples = uint32_t(surface->GetTarget()->GetMsaaSamples());
			}
			if (frame == 2) pending = recorded;
			else
			{
				if (frame == 3) complete(pending);
				complete(recorded);
			}
		}
		std::cout << "Default depth prepass=" << prepass << " colorKind=" << colorKind << " publication=" << magic_enum::enum_name(publication)
			<< " named=" << named << ": six frames, native views, live/resolved depth, color rejection, resize and retained commands passed\n";
	}

	void TestParticleHistory(ShaderSetPtr shader)
	{
		using Particle = Experimental::ParticlesNode;
		constexpr uint32_t Particles = 513, Traces = 4, Frames = 3, Instances = Particles * Traces;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		TVector<Particle::ParticleData> frames;
		for (uint32_t frame = 0; frame < Frames; ++frame)
		for (uint32_t particle = 0; particle < Particles; ++particle)
		{
			Particle::ParticleData value{};
			value.m_bIsEnabled = 1;
			value.m_x1 = value.m_x2 = particle * 0.25f;
			value.m_y1 = value.m_y2 = frame * 0.5f;
			value.m_z1 = 1;
			value.m_z2 = 3;
			value.m_size2 = 2.0f / 3;
			value.m_r2 = particle / 1024.0f;
			value.m_g2 = frame * 0.25f;
			value.m_b2 = 0.5f;
			value.m_a2 = 1;
			frames.Add(value);
		}
		auto input = driver->CreateBuffer(frames.Num() * sizeof(Particle::ParticleData), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
		std::memcpy(input->GetPointer(), frames.GetData(), input->GetSize());
		auto output = driver->CreateBuffer((Instances + 1) * sizeof(Particle::PerInstanceData), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
		auto bindings = driver->CreateShaderBindings();
		driver->AddBufferToShaderBindings(bindings, output, "data"_h, 0);
		driver->AddBufferToShaderBindings(bindings, input, "particlesData"_h, 1);
		auto frameBindings = driver->CreateShaderBindings();
		auto frameBuffer = driver->CreateBuffer(sizeof(UboFrameData), EBufferUsageBit::UniformBuffer_Bit, HostMemory);
		driver->AddBufferToShaderBindings(frameBindings, frameBuffer, "frame"_h, 0);
		const Particle::PushConstants constants{ Instances, Frames, 1, Traces, 0.5f };
		constexpr uint32_t Current[Frames][Traces] = { { 0, 0, 0, 0 }, { 1, 0, 0, 0 }, { 2, 1, 0, 0 } };
		constexpr uint32_t Previous[Frames][Traces] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 1, 0, 0, 0 } };
		for (uint32_t time = 0; time < 5; ++time)
		{
			auto* actual = static_cast<Particle::PerInstanceData*>(output->GetPointer());
			for (uint32_t i = 0; i <= Instances; ++i)
			{
				actual[i] = {};
				actual[i].model = glm::mat4(-17);
				actual[i].color = actual[i].colorOld = glm::vec4(-17);
				actual[i].materialInstance = 100 + i;
			}
			UboFrameData data{};
			data.m_currentTime = float(time);
			std::memcpy(frameBuffer->GetPointer(), &data, sizeof(data));
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit) | static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit));
			commands->Dispatch(draw, shader->GetComputeShaderRHI(), 256, 1, 1, { bindings, frameBindings }, &constants, sizeof(constants));
			commands->MemoryBarrier(draw, static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			CompleteCommands(upload, draw);
			for (uint32_t i = 0; i < Instances; ++i)
			{
				const uint32_t trace = i % Traces;
				const auto& current = frames[Current[time % Frames][trace] * Particles + i / Traces];
				const auto& previous = frames[Previous[time % Frames][trace] * Particles + i / Traces];
				const float decay = 1.0f / float(1u << trace);
				glm::mat4 model(1);
				model[0][0] = model[1][1] = decay;
				model[2][2] = 2;
				model[3] = glm::vec4(current.m_x2, current.m_y2, 2, 1);
				const glm::vec4 color(current.m_r2, current.m_g2, current.m_b2, decay);
				const glm::vec4 oldColor(previous.m_r2, previous.m_g2, previous.m_b2, decay);
				for (uint32_t column = 0; column < 4; ++column)
					Require(glm::all(glm::lessThan(glm::abs(actual[i].model[column] - model[column]), glm::vec4(0.00001f))),
						"particle history must clamp each trace frame before calculating its transform");
				Require(actual[i].color == color && actual[i].colorOld == oldColor && actual[i].materialInstance == 100 + i,
					"particle history must retain first-frame colors, clip wrap, trace decay and material identity");
			}
			Require(actual[Instances].model == glm::mat4(-17) && actual[Instances].color == glm::vec4(-17) &&
				actual[Instances].colorOld == glm::vec4(-17) && actual[Instances].materialInstance == 100 + Instances,
				"particle compute must not write past the last instance");
		}
		std::cout << "Particle history: 513 particles, four trace segments, five times, GPU transforms/colors/decay and trailing sentinel passed\n";
	}

	void TestPipelineSampleVariants(ShaderSetPtr shader)
	{
		auto& driver = *Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const RenderState state(true, true, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, "Opaque"_h.GetHash(), true);
		auto material = driver.CreateMaterial(driver.GetOrAddVertexDescription<VertexP3N3T3B3UV2C4>(),
			EPrimitiveTopology::TriangleList, state, shader, driver.CreateShaderBindings());
		const TVector<VkFormat> colors{ VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
		const auto deviceSamples = VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples();
		const auto basePipeline = material->m_vulkan.m_pipelines[0];
		const auto baseSamples = basePipeline->GetMsaaSamples();
		std::array<VulkanGraphicsPipelinePtr, 2> variants;
		size_t count = 0;
		for (uint32_t pass = 0; pass < 8; ++pass)
		{
			const auto samples = pass % 2 ? deviceSamples : VK_SAMPLE_COUNT_1_BIT;
			auto draw = driver.CreateCommandList(true, ECommandListQueue::Graphics);
			draw->m_vulkan.m_commandBuffer->BeginSecondaryCommandList(colors, VK_FORMAT_D32_SFLOAT,
				VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
				VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT, samples != VK_SAMPLE_COUNT_1_BIT);
			Require(draw->m_vulkan.m_commandBuffer->GetCurrentMsaaSamples() == samples,
				"secondary recording must inherit the pass sample count for material binding");
			commands->BindMaterial(draw, material);
			commands->EndCommandList(draw);
			const auto index = material->m_vulkan.m_pipelines.FindIf([&](const auto& pipeline)
			{
				return pipeline->GetMsaaSamples() == samples && pipeline->m_pipelineStates[0].template StaticCast<VulkanStateDynamicRendering>()->
					Fits(colors, VK_FORMAT_D32_SFLOAT, VK_FORMAT_UNDEFINED);
			});
			Require(index != decltype(material->m_vulkan.m_pipelines)::InvalidIndex,
				"material binding must publish a pipeline matching both formats and sample count");
			const auto pipeline = material->m_vulkan.m_pipelines[index];
			VkGraphicsPipelineCreateInfo createInfo{};
			for (const auto& pipelineState : pipeline->m_pipelineStates)
			{
				pipelineState->Apply(createInfo);
			}
			Require(createInfo.pMultisampleState->rasterizationSamples == samples,
				"the compiled variant's native state must agree with its sample count");
			if (pass < 2)
			{
				variants[pass] = pipeline;
				count = material->m_vulkan.m_pipelines.Num();
			}
			Require(pipeline == variants[pass % 2] && material->m_vulkan.m_pipelines.Num() == count && basePipeline->GetMsaaSamples() == baseSamples,
				"binding cached variants must not compile again or mutate the shared base pipeline");
		}
		Require(deviceSamples == VK_SAMPLE_COUNT_1_BIT || variants[0] != variants[1],
			"identical formats with different sample counts require separate pipelines");
		std::cout << "Pipeline MSAA: sample-specific variants, native state, warm reuse and secondary inheritance passed\n";
	}

	class ParticleDrawNode : public Experimental::ParticlesNode
	{
	public:
		void Process(RHIFrameGraphPtr graph, RHICommandListPtr upload, RHICommandListPtr draw,
			const RHISceneViewSnapshot& scene) override
		{
			Experimental::ParticlesNode::Process(graph, upload, draw, scene);
			m_colorAttachment = recordedColor;
		}

		VkRenderingAttachmentInfo m_colorAttachment{};

		void Initialize(const std::array<ShaderSetPtr, 3>& shaders)
		{
			auto& driver = Renderer::GetDriver();
			m_mesh = RHIMeshPtr::Make();
			m_mesh->m_vertexDescription = driver->GetOrAddVertexDescription<VertexP3N3T3B3UV2C4>();
			std::array<VertexP3N3T3B3UV2C4, 4> vertices{};
			for (uint32_t i = 0; i < vertices.size(); ++i)
			{
				vertices[i].m_position = glm::vec3(i % 2 ? 1 : -1, i / 2 ? 1 : -1, 0);
				vertices[i].m_texcoord = glm::vec2(i % 2, i / 2);
				vertices[i].m_normal = glm::vec3(0, 0, 1);
				vertices[i].m_tangent = glm::vec3(1, 0, 0);
				vertices[i].m_bitangent = glm::vec3(0, 1, 0);
				vertices[i].m_color = glm::vec4(1);
			}
			const uint32_t indices[] = { 0, 1, 2, 2, 1, 3 };
			m_mesh->m_vertexBuffer = driver->CreateBuffer(sizeof(vertices), EBufferUsageBit::VertexBuffer_Bit, HostMemory);
			m_mesh->m_indexBuffer = driver->CreateBuffer(sizeof(indices), EBufferUsageBit::IndexBuffer_Bit, HostMemory);
			std::memcpy(m_mesh->m_vertexBuffer->GetPointer(), vertices.data(), sizeof(vertices));
			std::memcpy(m_mesh->m_indexBuffer->GetPointer(), indices, sizeof(indices));
			auto bindings = driver->CreateShaderBindings();
			const glm::vec4 emission(0);
			auto materialBuffer = driver->CreateBuffer(sizeof(emission), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
			std::memcpy(materialBuffer->GetPointer(), &emission, sizeof(emission));
			driver->AddBufferToShaderBindings(bindings, materialBuffer, "material"_h, 0);
			const RenderState state(true, true, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, "Opaque"_h.GetHash(), true);
			m_material = driver->CreateMaterial(m_mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shaders[0], bindings);
			m_shadowMaterial = driver->CreateMaterial(m_mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shaders[1], bindings);
			m_pComputeShader = shaders[2];
			m_shadowMap = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32_SFLOAT);
			m_shadowMapBinding = driver->CreateShaderBindings();
			driver->AddSamplerToShaderBindings(m_shadowMapBinding, "shadowMapSampler"_h, m_shadowMap, 0);
			m_particlesHeader.m_bIsLoaded = true;
			m_particlesHeader.m_n = 2;
			m_particlesHeader.m_frames = 3;
			m_particlesHeader.m_fps = 1;
			m_particlesHeader.m_traceFrames = 1;
			m_particlesHeader.m_traceDecay = 1;
			m_particlesHeader.m_screenW = m_particlesHeader.m_screenH = Side;
			for (uint32_t frame = 0; frame < 3; ++frame)
			for (uint32_t particle = 0; particle < 2; ++particle)
			{
				ParticleData data{};
				data.m_bIsEnabled = 1;
				data.m_size1 = data.m_size2 = 1.0f / 3;
				data.m_x1 = data.m_x2 = particle ? 0.5f : -0.5f;
				data.m_z1 = 0.25f;
				data.m_z2 = 0.75f;
				const auto color = Color(frame, particle);
				data.m_r1 = data.m_r2 = color.r;
				data.m_g1 = data.m_g2 = color.g;
				data.m_b1 = data.m_b2 = color.b;
				data.m_a1 = data.m_a2 = color.a;
				m_particlesDataBinary.Add(data);
			}
			Require(InitializeBuffers({ PerInstanceData{}, PerInstanceData{} }), "particle frames and zeroed instances must upload");
		}

		static glm::vec4 Color(uint32_t frame, uint32_t particle)
		{
			return { (frame + 1) * 0.25f, (particle + 1) * 0.25f, 0.25f, 1 };
		}

		RHITexturePtr ShadowMap() const { return m_shadowMap; }
	};

	void TestParticleAttachments(const std::array<ShaderSetPtr, 3>& shaders,
		const std::array<ShaderSetPtr, 4>& depthReadback, bool colorSurface, bool namedColor, uint32_t depthKind, bool namedDepth,
		bool forceSingleSample = false)
	{
		CaptureAttachments capture;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = RHIFrameGraphPtr::Make();
		auto node = TRefPtr<ParticleDrawNode>::Make();
		node->Initialize(shaders);
		if (namedColor) node->SetRHIResource_Unresolved("color"_h, "ParticleColor"_h);
		if (namedDepth && depthKind < 2) node->SetRHIResource_Unresolved("depthStencil"_h, "ParticleDepth"_h);
		const bool bDepthSurface = depthKind == 1 || depthKind >= 3;
		RHISceneViewSnapshot scene;
		scene.m_rhiLightsData = driver->CreateShaderBindings();
		RHILightShaderData light{};
		light.m_direction = glm::vec3(0, 0, 1);
		auto lightBuffer = driver->CreateBuffer(sizeof(light), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
		std::memcpy(lightBuffer->GetPointer(), &light, sizeof(light));
		driver->AddBufferToShaderBindings(scene.m_rhiLightsData, lightBuffer, "light"_h, 0);
		const glm::mat4 identity(1);
		auto matrixBuffer = driver->CreateBuffer(sizeof(identity), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
		std::memcpy(matrixBuffer->GetPointer(), &identity, sizeof(identity));
		driver->AddBufferToShaderBindings(scene.m_rhiLightsData, matrixBuffer, "lightsMatrices"_h, 6);
		struct Recorded
		{
			RHICommandListPtr m_upload, m_draw;
			RHIBufferPtr m_color, m_depth, m_resolvedDepth, m_shadow, m_unusedDepth;
			glm::ivec2 m_extent;
			uint32_t m_frame = 0, m_samples = 1;
			float m_clearDepth = 0;
			uint32_t m_unusedSamples = 1;
			bool m_bAttachmentsMatch = false;
		};
		const auto complete = [&](const Recorded& recorded)
		{
			CompleteCommands(recorded.m_upload, recorded.m_draw);
			const auto* color = static_cast<const glm::vec4*>(recorded.m_color->GetPointer());
			const auto* depth = static_cast<const glm::vec2*>(recorded.m_depth->GetPointer());
			const auto* resolvedDepth = static_cast<const glm::vec2*>(recorded.m_resolvedDepth->GetPointer());
			for (int y = 0; y < recorded.m_extent.y; ++y)
			for (int x = 0; x < recorded.m_extent.x; ++x)
			{
				const bool covered = y >= recorded.m_extent.y / 4 && y < 3 * recorded.m_extent.y / 4;
				const bool visible = covered && recorded.m_clearDepth < 0.5f;
				const uint32_t particle = x < recorded.m_extent.x / 2 ? 0 : 1;
				const auto current = ParticleDrawNode::Color(recorded.m_frame, particle);
				const auto previous = ParticleDrawNode::Color(recorded.m_frame ? recorded.m_frame - 1 : 0, particle);
				const auto expected = visible ? glm::mix(current, previous, 0.5f) * glm::vec4(0.05f, 0.05f, 0.05f, 1) : glm::vec4(-1);
				const uint32_t pixel = y * recorded.m_extent.x + x;
				Require(std::abs(resolvedDepth[pixel].x - (visible ? 0.5f : recorded.m_clearDepth)) < 0.00001f,
					"particle depth resolve must preserve the selected attachment's pixels");
				if (!glm::all(glm::lessThan(glm::abs(color[pixel] - expected), glm::vec4(0.00001f))))
					throw std::runtime_error("particle color pixel=" + std::to_string(pixel) + " actual=" + std::to_string(color[pixel].r) +
						", expected=" + std::to_string(expected.r) + ", frame=" + std::to_string(recorded.m_frame));
				for (uint32_t sample = 0; sample < recorded.m_samples; ++sample)
					Require(std::abs(depth[pixel * recorded.m_samples + sample].x - (visible ? 0.5f : recorded.m_clearDepth)) < 0.00001f,
						"particle depth must use the selected attachment and preserve every occluded sample");
				if (recorded.m_unusedDepth)
				{
					const auto* unused = static_cast<const glm::vec2*>(recorded.m_unusedDepth->GetPointer());
					for (uint32_t sample = 0; sample < recorded.m_unusedSamples; ++sample)
						Require(unused[pixel * recorded.m_unusedSamples + sample].x == 0.125f,
							"single-sample particles must leave live MSAA depth untouched");
				}
			}
			Require(recorded.m_bAttachmentsMatch, "particle rendering must bind the exact selected native targets and resolve images");
			const auto* shadow = static_cast<const float*>(recorded.m_shadow->GetPointer());
			for (uint32_t y = 0; y < Side; ++y)
			for (uint32_t x = 0; x < Side; ++x)
				Require(std::abs(shadow[y * Side + x] - (y >= Side / 4 && y < 3 * Side / 4 ? 0.5f : 0.0f)) < 0.00001f,
					"particle shadow must rasterize the production compute transforms");
		};
		RHIResourcePtr colorResource, depthResource;
		RHIRenderTargetPtr color, depth, colorTarget, depthTarget, decoy;
		Recorded pending;
		for (uint32_t frame = 0; frame < 6; ++frame)
		{
			if (frame == 5) node->Clear();
			const glm::ivec2 extent(frame < 2 ? 8 : 12, frame < 4 ? 8 : 12);
			if (frame != 1)
			{
				color = driver->CreateRenderTarget(extent, 1, EFormat::R32G32B32A32_SFLOAT);
				colorResource = forceSingleSample ? RHIResourcePtr(RHISurfacePtr::Make(color, color, false)) :
					colorSurface ? RHIResourcePtr(driver->CreateSurface(color)) : RHIResourcePtr(color);
				const auto depthUsage = ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit;
				depth = driver->CreateRenderTarget(extent, 1, EFormat::D32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp, depthUsage);
				depthResource = bDepthSurface ? RHIResourcePtr(driver->CreateSurface(depth)) : RHIResourcePtr(depth);
				decoy = driver->CreateRenderTarget(extent, 1, EFormat::D32_SFLOAT_S8_UINT,
					ETextureFiltration::Nearest, ETextureClamping::Clamp, depthUsage);
				graph->SetRenderTarget("DepthBuffer"_h, depthKind == 4 ? RHIRenderTargetPtr{} : depthKind == 2 || depthKind == 3 ? depth : decoy);
				if (depthKind >= 3) graph->SetSurface("DepthBuffer"_h, depthResource.DynamicCast<RHISurface>());
				if (!namedColor) node->SetRHIResource("color"_h, colorResource);
				else if (colorSurface) graph->SetSurface("ParticleColor"_h, colorResource.DynamicCast<RHISurface>());
				else graph->SetRenderTarget("ParticleColor"_h, color);
				if (depthKind < 2)
				{
					if (!namedDepth) node->SetRHIResource("depthStencil"_h, depthResource);
					else if (depthKind == 1) graph->SetSurface("ParticleDepth"_h, depthResource.DynamicCast<RHISurface>());
					else graph->SetRenderTarget("ParticleDepth"_h, depth);
				}
				const bool msaa = VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
				colorTarget = colorSurface ? colorResource.DynamicCast<RHISurface>()->GetTarget() : msaa ?
					driver->GetOrAddMsaaFramebufferRenderTarget(color->GetFormat(), extent).StaticCast<RHIRenderTarget>() : color;
				depthTarget = forceSingleSample ? depth : bDepthSurface ? depthResource.DynamicCast<RHISurface>()->GetTarget() : msaa ?
					driver->GetOrAddMsaaFramebufferRenderTarget(depth->GetFormat(), extent).StaticCast<RHIRenderTarget>() : depth;
			}
			const std::array<uint32_t, 6> frames{ 1, 2, 0, 1, 0, 2 };
			UboFrameData frameData{};
			frameData.m_view = frameData.m_projection = frameData.m_invProjection = identity;
			frameData.m_viewportSize = extent;
			frameData.m_currentTime = float(frames[frame]);
			scene.m_frameBindings = driver->CreateShaderBindings();
			auto frameBuffer = driver->CreateBuffer(sizeof(frameData), EBufferUsageBit::UniformBuffer_Bit, HostMemory);
			std::memcpy(frameBuffer->GetPointer(), &frameData, sizeof(frameData));
			driver->AddBufferToShaderBindings(scene.m_frameBindings, frameBuffer, "frame"_h, 0);
			Recorded recorded;
			recorded.m_extent = extent;
			recorded.m_frame = frames[frame];
			recorded.m_clearDepth = frame % 2 ? 0.75f : 0.25f;
			recorded.m_samples = uint32_t(depthTarget->GetMsaaSamples());
			recorded.m_upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			recorded.m_draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(recorded.m_upload, true);
			commands->BeginCommandList(recorded.m_draw, true);
			for (auto target : { color, colorTarget }) ClearColor(recorded.m_draw, target, glm::vec4(-1));
			for (auto target : { depth, depthTarget, decoy })
			{
				commands->ImageMemoryBarrier(recorded.m_draw, target, EImageLayout::TransferDstOptimal);
				commands->ClearDepthStencil(recorded.m_draw, target, target == decoy ? 1.0f - recorded.m_clearDepth : recorded.m_clearDepth, 0);
			}
			if (VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT)
			{
				auto target = driver->GetOrAddMsaaFramebufferRenderTarget(decoy->GetFormat(), extent);
				commands->ImageMemoryBarrier(recorded.m_draw, target, EImageLayout::TransferDstOptimal);
				commands->ClearDepthStencil(recorded.m_draw, target, 1.0f - recorded.m_clearDepth, 0);
				if (bDepthSurface)
				{
					target = driver->GetOrAddMsaaFramebufferRenderTarget(depth->GetFormat(), extent);
					commands->ImageMemoryBarrier(recorded.m_draw, target, EImageLayout::TransferDstOptimal);
					commands->ClearDepthStencil(recorded.m_draw, target, 1.0f - recorded.m_clearDepth, 0);
					if (forceSingleSample)
					{
						target = depthResource.DynamicCast<RHISurface>()->GetTarget();
						commands->ImageMemoryBarrier(recorded.m_draw, target, EImageLayout::TransferDstOptimal);
						commands->ClearDepthStencil(recorded.m_draw, target, 0.125f, 0);
					}
				}
			}
			node->Process(graph, recorded.m_upload, recorded.m_draw, scene);
			const auto& layouts = recorded.m_draw->m_vulkan.m_commandBuffer->GetImageBarriers();
			Require(layouts[*colorTarget->m_vulkan.m_image].m_layout == EImageLayout::ColorAttachmentOptimal,
				"particle rendering must transition the actual color target, including cached MSAA attachments");
			Require(layouts[*depthTarget->m_vulkan.m_image].m_layout == EImageLayout::DepthAttachmentOptimal,
				"particle rendering must transition the actual depth target, including cached MSAA attachments");
			recorded.m_bAttachmentsMatch = node->GetDrawCallStats().m_numBatches == 2 && recordedColorCount == 1 &&
				recordedColor.imageView == static_cast<VkImageView>(*colorTarget->m_vulkan.m_imageView) &&
				recordedDepth.imageView == static_cast<VkImageView>(*depthTarget->m_vulkan.m_imageView) &&
				recordedColor.resolveImageView == (colorTarget != color ? static_cast<VkImageView>(*color->m_vulkan.m_imageView) : VK_NULL_HANDLE) &&
				recordedDepth.resolveImageView == (depthTarget != depth ? static_cast<VkImageView>(*depth->m_vulkan.m_imageView) : VK_NULL_HANDLE);
			recorded.m_color = ReadColor(recorded.m_draw, color);
			recorded.m_shadow = ReadColor(recorded.m_draw, node->ShadowMap());
			recorded.m_depth = ReadDepth(recorded.m_draw, depthTarget, depthReadback);
			recorded.m_resolvedDepth = ReadDepth(recorded.m_draw, depth, depthReadback);
			const auto surface = depthResource.DynamicCast<RHISurface>();
			if (forceSingleSample && surface && surface->NeedsResolve())
			{
				recorded.m_unusedDepth = ReadDepth(recorded.m_draw, surface->GetTarget(), depthReadback);
				recorded.m_unusedSamples = uint32_t(surface->GetTarget()->GetMsaaSamples());
			}
			if (frame == 2) pending = recorded;
			else
			{
				if (frame == 3) complete(pending);
				complete(recorded);
			}
		}
		std::cout << (depthKind >= 3 || forceSingleSample ? "Particles default depth " : "Particles ") <<
			"colorSurface=" << colorSurface << " namedColor=" << namedColor << " depthKind=" << depthKind <<
			" namedDepth=" << namedDepth << " singleSample=" << forceSingleSample <<
			": six frames, production compute/shadow/color, all pixels/depth samples, resize and retained commands passed\n";
	}

	void TestParticleGraph(const std::array<ShaderSetPtr, 3>& shaders, uint32_t colorKind, bool named, bool clearResolved)
	{
		CaptureAttachments capture;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const bool msaa = colorKind != 2 && VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
		auto graph = RHIFrameGraphPtr::Make();
		auto view = RHISceneViewPtr::Make();
		view->m_snapshots.Resize(1);
		auto& scene = view->m_snapshots[0];
		World cameraWorld("ParticleGraphCamera", 0);
		auto camera = cameraWorld.Instantiate("Camera")->AddComponent<CameraComponent>();
		cameraWorld.GetECS<CameraECS>()->Tick(0);
		auto cameraData = camera->GetData();
		cameraData.SetProjectionMatrix(cameraData.GetInvViewMatrix());
		cameraData.SetOwner({});
		cameraWorld.Clear();
		scene.m_camera = TUniquePtr<CameraData>::Make(cameraData);
		scene.m_bGlobalIlluminationEnabled = false;
		scene.m_shadowMatrices.Add(glm::mat4(1));
		auto lights = TSharedPtr<TVector<RHILightShaderData>>::Make();
		RHILightShaderData light{};
		light.m_direction = glm::vec3(0, 0, 1);
		lights->Add(light);
		scene.m_cpuLightsData = lights;
		const std::array<StringHash, 2> names{ "ParticleA"_h, "ParticleB"_h };
		const std::array<StringHash, 2> aliases{ "ClearParticleA"_h, "ClearParticleB"_h };
		std::array<TRefPtr<ParticleDrawNode>, 2> nodes;
		std::array<TRefPtr<ClearNode>, 2> clears;
		std::array<RHIRenderTargetPtr, 2> outputs;
		std::array<RHITexturePtr, 2> previousTargets;
		auto clearDepth = TRefPtr<ClearNode>::Make();
		clearDepth->SetFloat("clearDepth"_h, 0.25f);
		clearDepth->SetFloat("clearStencil"_h, 0);
		for (uint32_t i = 0; i < nodes.size(); ++i)
		{
			nodes[i] = TRefPtr<ParticleDrawNode>::Make();
			nodes[i]->Initialize(shaders);
			clears[i] = TRefPtr<ClearNode>::Make();
			if (named)
			{
				nodes[i]->SetRHIResource_Unresolved("color"_h, names[i]);
				clears[i]->SetRHIResource_Unresolved("target"_h, clearResolved ? aliases[i] : names[i]);
			}
		}
		const auto populateGraph = [&]
		{
			for (auto clear : clears) graph->GetGraph().Add(clear);
			for (auto node : nodes)
			{
				graph->GetGraph().Add(clearDepth);
				graph->GetGraph().Add(node);
			}
		};
		populateGraph();
		struct Recorded
		{
			TVector<RHICommandListPtr> m_transfers, m_graphics;
			RHICommandListPtr m_readback;
			RHISemaphorePtr m_ready;
			std::array<RHIBufferPtr, 4> m_colors;
			glm::ivec2 m_extent{};
			uint32_t m_frame = 0;
			bool m_bAttachmentsMatch = true;
			bool m_bComputeInputReady = true;
		};
		const auto background = [](uint32_t image, uint32_t frame)
		{
			return glm::vec4(-1.0f - image - 0.125f * frame);
		};
		const auto complete = [&](const Recorded& recorded)
		{
			auto ready = recorded.m_ready;
			for (size_t i = 0; i < recorded.m_transfers.Num(); ++i)
				for (auto command : { recorded.m_transfers[i], recorded.m_graphics[i] })
				{
					auto next = driver->CreateWaitSemaphore();
					Require(driver->SubmitCommandList(command, RHIFencePtr::Make(), next, ready), "particle graph commands must submit");
					ready = next;
				}
			auto finished = RHIFencePtr::Make();
			Require(driver->SubmitCommandList(recorded.m_readback, finished, nullptr, ready) &&
				finished->Wait(5000000000ull) == EFenceStatus::Finished, "particle graph readback must finish");
			for (uint32_t image = 0; image < recorded.m_colors.size(); ++image)
			{
				const auto* pixels = static_cast<const glm::vec4*>(recorded.m_colors[image]->GetPointer());
				for (int y = 0; y < recorded.m_extent.y; ++y)
				for (int x = 0; x < recorded.m_extent.x; ++x)
				{
					const uint32_t time = recorded.m_frame % 3;
					const uint32_t particle = x < recorded.m_extent.x / 2 ? 0 : 1;
					const auto current = ParticleDrawNode::Color(time, particle);
					const auto previous = ParticleDrawNode::Color(time ? time - 1 : 0, particle);
					const auto expected = y >= recorded.m_extent.y / 4 && y < 3 * recorded.m_extent.y / 4 ?
						glm::mix(current, previous, 0.5f) * glm::vec4(0.05f, 0.05f, 0.05f, 1) : background(image / 2, recorded.m_frame);
					const uint32_t pixel = y * recorded.m_extent.x + x;
					if (!glm::all(glm::lessThan(glm::abs(pixels[pixel] - expected), glm::vec4(0.00001f))))
						throw std::runtime_error("particle graph colorKind=" + std::to_string(colorKind) + " named=" + std::to_string(named) +
							" clearResolved=" + std::to_string(clearResolved) + " frame=" + std::to_string(recorded.m_frame) +
							" image=" + std::to_string(image) + " pixel=" + std::to_string(pixel) +
							" actual=" + std::to_string(pixels[pixel].r) + ", expected=" + std::to_string(expected.r));
				}
			}
			Require(recorded.m_bAttachmentsMatch, "particle draws must bind distinct graph-owned live/resolve pairs with LOAD/STORE");
			Require(recorded.m_bComputeInputReady, "particle compute must read frame uniforms after their native transfer-to-uniform dependency");
		};
		Recorded pending;
		for (uint32_t frame = 0; frame < 6; ++frame)
		{
			if (frame == 4)
			{
				graph->Clear();
				populateGraph();
			}
			const glm::ivec2 extent(frame < 2 ? 8 : 12, frame < 4 ? 8 : 12);
			const bool replace = frame != 1 && frame != 5;
			if (replace)
			{
				auto depth = driver->CreateRenderTarget(extent, 1, EFormat::D32_SFLOAT, ETextureFiltration::Nearest,
					ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::TextureTransferDst_Bit);
				auto depthSurface = msaa ? driver->CreateSurface(depth) : RHISurfacePtr::Make(depth, depth, false);
				clearDepth->SetRHIResource("target"_h, depthSurface);
				for (uint32_t i = 0; i < nodes.size(); ++i)
				{
					outputs[i] = driver->CreateRenderTarget(extent, 1, EFormat::R32G32B32A32_SFLOAT);
					RHIResourcePtr resource = outputs[i];
					if (colorKind)
					{
						auto surface = colorKind == 2 ? RHISurfacePtr::Make(outputs[i], outputs[i], false) : driver->CreateSurface(outputs[i]);
						resource = surface;
						graph->SetSurface(names[i], surface);
					}
					graph->SetRenderTarget(names[i], outputs[i]);
					graph->SetRenderTarget(aliases[i], outputs[i]);
					if (!named)
					{
						nodes[i]->SetRHIResource("color"_h, resource);
						clears[i]->SetRHIResource("target"_h, clearResolved ? RHIResourcePtr(outputs[i]) : resource);
					}
					nodes[i]->SetRHIResource("depthStencil"_h, depthSurface);
				}
				graph->SetRenderTarget("Main"_h, outputs[0]);
			}
			for (uint32_t i = 0; i < clears.size(); ++i) clears[i]->SetVec4("clearColor"_h, background(i, frame));
			auto poison = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(poison, true);
			ClearColor(poison, driver->GetOrAddMsaaFramebufferRenderTarget(EFormat::R32G32B32A32_SFLOAT, extent), glm::vec4(-16));
			commands->EndCommandList(poison);
			auto initialized = driver->CreateWaitSemaphore();
			Require(driver->SubmitCommandList(poison, RHIFencePtr::Make(), initialized), "particle graph poison must submit");
			scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
			scene.m_submissionContext->BeginSubmission(304 + frame, frame % 2);
			view->m_currentTime = float(frame % 3);
			scene.m_currentTime = view->m_currentTime;
			Recorded recorded;
			recorded.m_frame = frame;
			recorded.m_extent = extent;
			const auto process = [&]
			{
				Require(graph->Process(view, recorded.m_transfers, recorded.m_graphics, initialized, recorded.m_ready),
					"Clear and Particles must execute through the frame graph");
			};
#if defined(__APPLE__)
			const auto inputs = Tests::CaptureVulkanComputeInputs(process);
			const auto frameMemory = *scene.m_frameBindings->GetOrAddShaderBinding("frameData"_h)->m_vulkan.m_valueBinding->Get();
			VkCommandBuffer copiedOn = VK_NULL_HANDLE;
			bool uniformReady = false;
			uint32_t dispatches = 0;
			for (const auto& event : inputs)
			{
				using Kind = Tests::VulkanComputeInputEvent::Kind;
				if (event.m_kind == Kind::Copy && event.m_buffer == *frameMemory.m_buffer &&
					event.m_offset <= frameMemory.m_offset && event.m_offset + event.m_size >= frameMemory.m_offset + sizeof(UboFrameData))
				{
					copiedOn = event.m_command;
					uniformReady = false;
				}
				if (event.m_command != copiedOn) continue;
				const bool coversUniform = event.m_buffer == VK_NULL_HANDLE || (event.m_buffer == *frameMemory.m_buffer &&
					event.m_offset <= frameMemory.m_offset && (event.m_size == VK_WHOLE_SIZE ||
						event.m_offset + event.m_size >= frameMemory.m_offset + sizeof(UboFrameData)));
				if (event.m_kind == Kind::Barrier &&
					coversUniform &&
					(event.m_sourceStage & (VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)) &&
					(event.m_destinationStage & (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)) &&
					(event.m_sourceAccess & (VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT)) &&
					(event.m_destinationAccess & (VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_MEMORY_READ_BIT))) uniformReady = true;
				if (event.m_kind == Kind::Dispatch)
				{
					++dispatches;
					recorded.m_bComputeInputReady &= uniformReady;
				}
			}
			recorded.m_bComputeInputReady &= dispatches == nodes.size();
#else
			process();
#endif
			Require(recorded.m_graphics.Num() == 1 && recorded.m_transfers.Num() == 1,
				"particle fixture must retain its graph draw commands until explicit submission");
			recorded.m_readback = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(recorded.m_readback, true);
			for (uint32_t i = 0; i < nodes.size(); ++i)
			{
				auto target = nodes[i]->GetTargetAttachment("color"_h, graph.GetRawPtr());
				if (!replace) Require(target == previousTargets[i], "unchanged particle output must reuse its live image");
				else if (frame) Require(target != previousTargets[i], "replaced particle output must own a different live image");
				previousTargets[i] = target;
				const auto& attachment = nodes[i]->m_colorAttachment;
				recorded.m_bAttachmentsMatch &= nodes[i]->GetDrawCallStats().m_numBatches == 2 &&
					target->GetMsaaSamples() == (msaa ? App::GetSubmodule<Renderer>()->GetMsaaSamples() : EMsaaSamples::Samples_1) &&
					attachment.imageView == static_cast<VkImageView>(*target->m_vulkan.m_imageView) &&
					attachment.resolveImageView == (msaa ? static_cast<VkImageView>(*outputs[i]->m_vulkan.m_imageView) : VK_NULL_HANDLE) &&
					attachment.resolveMode == (msaa ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE) &&
					attachment.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && attachment.storeOp == VK_ATTACHMENT_STORE_OP_STORE;
				recorded.m_colors[i * 2] = ReadColor(recorded.m_readback, outputs[i]);
				recorded.m_colors[i * 2 + 1] = target == outputs[i] ? recorded.m_colors[i * 2] : ReadColor(recorded.m_readback, target);
			}
			recorded.m_bAttachmentsMatch &= previousTargets[0] != previousTargets[1];
			commands->MemoryBarrier(recorded.m_readback, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			commands->EndCommandList(recorded.m_readback);
			if (frame == 2) pending = recorded;
			else
			{
				if (frame == 3) complete(pending);
				complete(recorded);
			}
		}
		std::cout << "Particle graph colorKind=" << colorKind << " named=" << named << " clearResolved=" << clearResolved <<
			": six frames, two distinct outputs, live/resolved pixels, resize, graph Clear and retained commands passed\n";
	}

	template<typename Instance>
	void TestPagedArenaUploadFlights(std::string_view pass)
	{
		constexpr uint32_t NumRanges = 10000u;
		constexpr uint32_t InstancesPerRange = 4u;
		constexpr uint32_t NumInstances = NumRanges * InstancesPerRange;
		constexpr uint32_t PageSize = TPackedDrawArenaPage<Instance>::NumInstances;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto mesh = RHIMeshPtr::Make();
		mesh->m_vertexDescription = driver->GetOrAddVertexDescription<VertexP3N3UV2C4>();
		mesh->m_vertexBuffer = driver->CreateBuffer(sizeof(VertexP3N3UV2C4) * 3u, EBufferUsageBit::VertexBuffer_Bit, HostMemory);
		mesh->m_indexBuffer = driver->CreateBuffer(sizeof(uint32_t) * 3u, EBufferUsageBit::IndexBuffer_Bit, HostMemory);
		RHIBatch batch;
		batch.m_mesh = mesh;
		TPackedDrawPagedArenaCache<Instance> arena;
		TVector<Instance> instances;
		TVector<uint64_t> keys;
		instances.Resize(InstancesPerRange);
		keys.Resize(InstancesPerRange);
		auto replace = [&](uint32_t range, uint32_t revision)
		{
			for (uint32_t i = 0; i < InstancesPerRange; ++i)
			{
				instances[i] = {};
				instances[i].model = glm::translate(glm::mat4(1), glm::vec3(range, i, revision));
				instances[i].sphereBounds = glm::vec4(range, i, revision, 1);
				keys[i] = range * InstancesPerRange + i;
			}
			Require(arena.ReplaceRange(range, revision, instances, keys), "upload fixture range must publish");
		};
		arena.BeginUpdate(1, 1, 1);
		for (uint32_t range = 0; range < NumRanges; ++range) replace(range, 1);
		auto payload = arena.EndUpdate();
		const auto original = payload;

		struct Flight
		{
			TPackedDrawPacket<Instance> m_packet;
			RHIShaderBindingSetPtr m_bindings;
			RHIBufferPtr m_storage, m_indices, m_indirect;
		};
		std::array<Flight, 3> flights;
		for (auto& flight : flights)
		{
			const auto usage = EBufferUsageBit::StorageBuffer_Bit | EBufferUsageBit::BufferTransferDst_Bit;
			flight.m_storage = driver->CreateBuffer(sizeof(Instance) * NumInstances, usage, HostMemory);
			flight.m_indices = driver->CreateBuffer(sizeof(uint32_t) * NumInstances, usage, HostMemory);
			flight.m_bindings = driver->CreateShaderBindings();
			Require(driver->AddBufferToShaderBindings(flight.m_bindings, flight.m_storage, "data"_h, 0).IsValid() &&
				driver->AddBufferToShaderBindings(flight.m_bindings, flight.m_indices, "indices"_h, 1).IsValid(),
				"upload fixture buffers must bind");
		}
		auto verifyStorage = [&](const Flight& flight, const auto& expected)
		{
			const auto* actual = static_cast<const Instance*>(flight.m_storage->GetPointer());
			for (uint32_t i = 0; i < NumInstances; ++i)
				Require(actual[i] == expected->m_arenaPages[i / PageSize]->m_instances[i % PageSize],
					"completed GPU storage must match the retained payload, including untouched pages");
		};
		auto upload = [&](uint32_t flightIndex, std::string_view stage, uint32_t expectedPages)
		{
			auto& flight = flights[flightIndex];
			auto& packet = flight.m_packet;
			packet.Reset();
			packet.UseSharedArenaPayload(EMobilityType::Static, payload);
			for (uint32_t range = 0; range < NumRanges; ++range)
			{
				if (!payload->FindRange(range)) continue;
				for (uint32_t i = 0; i < InstancesPerRange; ++i)
					Require(packet.AddArenaView(batch, mesh, range, range * InstancesPerRange + i),
						"each live range must produce its view indices");
			}
			packet.Finalize();
			auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(command, true);
			const uint64_t expectedBytes = uint64_t(expectedPages) * PageSize * sizeof(Instance);
			const auto record = [&]
			{
				return RHIRecordPackedDrawPacketImpl(packet, command, command,
					[](const RHIBatch&, TVector<RHIShaderBindingSetPtr>&) {}, flight.m_bindings, flight.m_indirect,
					glm::ivec4(0, 0, 8, 8), glm::uvec4(0, 0, 8, 8), glm::vec2(0, 1), {}, nullptr, {}, {}, false,
					[] { return false; }); // Exercise production uploads without a framebuffer or shader draw.
			};
#if defined(__APPLE__)
			decltype(record()) stats;
			const auto writes = Tests::CaptureVulkanBufferWrites([&] { stats = record(); });
			const auto memory = *flight.m_storage->m_vulkan.m_buffer->Get();
			uint64_t writtenBytes = 0;
			for (const auto& write : writes)
			{
				if (write.m_buffer != *memory.m_buffer) continue;
				const auto begin = std::max(write.m_offset, VkDeviceSize(memory.m_offset));
				const auto end = std::min(write.m_offset + write.m_size, VkDeviceSize(memory.m_offset + memory.m_size));
				if (end > begin) writtenBytes += end - begin;
			}
			Require(writtenBytes == expectedBytes, "native instance transfers must contain only changed pages for this flight");
			std::cout << "ArenaNativeUpload " << pass << ' ' << stage << ": bytes=" << writtenBytes << '\n';
#else
			const auto stats = record();
#endif
			Require(stats.m_numBatches == 0, "upload-only fixture must not record draws");
			Require(packet.m_metrics.m_instanceUploadBytes == expectedBytes &&
				packet.m_metrics.m_dirtyInstanceRanges == expectedPages,
				"recorded instance uploads must contain exactly the changed pages for this flight");
			Require(packet.m_metrics.m_bReusedInstancePayload == (expectedPages == 0),
				"unchanged flight payload must reuse its uploaded storage");
			uint32_t lastEnd = 0;
			for (const auto& range : packet.m_instanceUploads)
			{
				Require(range.m_offset >= lastEnd && range.m_offset % PageSize == 0 && range.m_count == PageSize,
					"recorded page uploads must be ordered, non-overlapping and aligned");
				lastEnd = range.m_offset + range.m_count;
			}
			commands->MemoryBarrier(command, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			commands->EndCommandList(command);
			auto finished = RHIFencePtr::Make();
			Require(driver->SubmitCommandList(command, finished) && finished->Wait(5000000000ull) == EFenceStatus::Finished,
				"arena upload must complete before host inspection");
			verifyStorage(flight, payload);
			const auto* indices = static_cast<const uint32_t*>(flight.m_indices->GetPointer());
			for (uint32_t i = 0; i < packet.GetNumInstances(); ++i)
				Require(indices[i] == packet.GetInstanceIndices()[i], "GPU indices must follow the current view after deletion and re-add");
			std::cout << "ArenaUpload " << pass << ' ' << stage << ": bytes=" << expectedBytes
				<< " ranges=" << expectedPages << '\n';
		};
		for (uint32_t i = 0; i < flights.size(); ++i) upload(i, "initial", NumInstances / PageSize);
		arena.BeginUpdate(1, 2, 2);
		replace(17, 2);
		payload = arena.EndUpdate();
		upload(0, "sparse", 1);
		arena.BeginUpdate(1, 3, 3);
		replace(8001, 3);
		payload = arena.EndUpdate();
		upload(1, "skipped-flight", 2);
		upload(0, "next-page", 1);
		upload(1, "unchanged", 0);
		arena.BeginUpdate(1, 4, 4);
		arena.RemoveRange(1000);
		payload = arena.EndUpdate();
		upload(1, "delete", 0);
		arena.BeginUpdate(1, 5, 5);
		replace(1000, 5);
		payload = arena.EndUpdate();
		upload(1, "re-add", 1);
		arena.BeginUpdate(1, 6, 6);
		for (uint32_t range = 0; range < NumRanges; ++range) replace(range, 6);
		payload = arena.EndUpdate();
		upload(1, "dense", NumInstances / PageSize);
		Require(flights[2].m_packet.GetSharedPayload(EMobilityType::Static) == original,
			"unused flight must retain its own payload generation");
		verifyStorage(flights[2], original);
		upload(2, "retained-flight", NumInstances / PageSize);
		flights[1].m_packet.InvalidateUploadedState();
		upload(1, "invalidated-flight", NumInstances / PageSize);
	}

	enum class DepthAttachmentBinding { Target, NamedTarget, Surface, NamedSurface };

	void TestCustomDepthSilhouette(ShaderSetPtr shader, const std::array<ShaderSetPtr, 4>& depthReadback,
		bool paged, bool instanced, bool skinned, EMobilityType mobility,
		DepthAttachmentBinding depthBinding = DepthAttachmentBinding::Target)
	{
		CaptureAttachments capture;
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
			Require(driver->AddBufferToShaderBindings(result, buffer, "material"_h, 0).IsValid(), "custom material parameters must bind");
			return result;
		};
		const RenderState state(true, true, 0, true, ECullMode::None, EBlendMode::None, EFillMode::Fill, "Masked"_h.GetHash(), true);
		auto material = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shader, materialBindings(0.25f));
		Require(material && material->GetVersion(), "custom masked material must have a complete version");
		mesh->m_bakedVolumeScale = glm::vec3(2, 3, 4);
		RHISceneViewProxy source;
		auto shadowCaster = TSharedPtr<RHIShadowCasterProxy>::Make();
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
				caster.m_localMatrix = transform;
				caster.m_renderQueueTag = "Masked"_h.GetHash();
				caster.m_customDepthMaterial = material;
				caster.m_customDepthShader = shader;
				caster.m_alphaCutoff = 0.75f;
#if defined(__APPLE__)
				caster.m_materialTextureSamplers = { 0 };
#endif
				shadowCaster->m_meshes.Add(std::move(caster));
			}
		}
		source.m_shadowCaster = std::move(shadowCaster);
		auto topology = RHISceneProxyResourcePtr::Make(std::move(source));
		RHISceneInstanceRecord record;
		record.m_producerKey = 1;
		record.m_mobility = mobility;
		record.m_worldMatrix = glm::mat4(1);
		record.m_worldBounds = Math::AABB(glm::vec3(0), glm::vec3(3));
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
		previousSnapshot.m_sceneVersions = TSharedPtr<const TVector<RHISceneVersionPtr>>::Make(
			TVector<RHISceneVersionPtr>{ scene->PublishVersion() });
		auto previousFrame = TSharedPtr<RHIMotionHistoryFrame>::Make();
		previousFrame->m_sceneVersions = previousSnapshot.m_sceneVersions;
		previousFrame->m_mobilityRevisions[static_cast<size_t>(mobility)] = previousSnapshot.GetMobilityRevision(mobility);
		Require(scene->UpdateInstance(handle, record, ToMask(ESceneChangeBit::Transform)), "fixture must publish its current transform");
		RHISceneViewSnapshot snapshot;
		snapshot.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		snapshot.m_sceneVersions = TSharedPtr<const TVector<RHISceneVersionPtr>>::Make(
			TVector<RHISceneVersionPtr>{ scene->PublishVersion() });
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
			driver->AddBufferToShaderBindings(snapshot.m_frameBindings, buffer, binding ? "previousFrame"_h : "frame"_h, binding);
		}
		if (skinned)
		{
			glm::mat4 bone(1);
			bone[3].x = 0.25f;
			auto buffer = driver->CreateBuffer(sizeof(bone), EBufferUsageBit::StorageBuffer_Bit, HostMemory);
			std::memcpy(buffer->GetPointer(), &bone, sizeof(bone));
			snapshot.m_boneMatrices = driver->CreateShaderBindings();
			driver->AddBufferToShaderBindings(snapshot.m_boneMatrices, buffer, "bones"_h, 0);
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
			node->SetString("Tag"_h, "Masked");
			node->SetString("GPUCulling"_h, "false");
			node->SetString("VirtualizeInstancePayloads"_h, paged ? "true" : "false");
		}
		depth->SetString("ClearDepth"_h, "true");
		const bool bNamedDepth = depthBinding == DepthAttachmentBinding::NamedTarget || depthBinding == DepthAttachmentBinding::NamedSurface;
		const bool bDepthSurface = depthBinding == DepthAttachmentBinding::Surface || depthBinding == DepthAttachmentBinding::NamedSurface;
		const auto bindDepth = [&](FrameGraphNodePtr node, RHIRenderTargetPtr target, StringHash name)
		{
			RHIResourcePtr attachment = bDepthSurface ? RHIResourcePtr(driver->CreateSurface(target)) : RHIResourcePtr(target);
			Require(attachment.IsValid(), "custom-depth attachment must initialize");
			if (bNamedDepth)
			{
				if (bDepthSurface) graph->SetSurface(name, attachment.DynamicCast<RHISurface>());
				else graph->SetRenderTarget(name, target);
			}
			else node->SetRHIResource("depthStencil"_h, attachment);
		};
		if (bNamedDepth)
		{
			depth->SetRHIResource_Unresolved("depthStencil"_h, "SelectedDepth"_h);
			main->SetRHIResource_Unresolved("depthStencil"_h, "SelectedMainDepth"_h);
		}
		bindDepth(depth, prepassDepth, "SelectedDepth"_h);
		bindDepth(main, mainDepth, "SelectedMainDepth"_h);
		main->SetRHIResource("color"_h, color);
		for (uint32_t frame = 0; frame < 4; ++frame)
		{
			if (frame == 2) material->SetBindings(materialBindings(0));
			if (frame == 2 && depthBinding != DepthAttachmentBinding::Target)
			{
				prepassDepth = depthTarget();
				mainDepth = depthTarget();
				bindDepth(depth, prepassDepth, "SelectedDepth"_h);
				bindDepth(main, mainDepth, "SelectedMainDepth"_h);
			}
			snapshot.m_frame = frame + 1;
			if (frame == 2) snapshot.m_previousMotionFrame = previousFrame;
			const uint64_t submissionId = 177000 + frame;
			const uint64_t materialRevision = RHIMaterial::BeginSubmissionVersionCapture(submissionId);
			snapshot.m_submissionContext->BeginSubmission(submissionId, 0, materialRevision);
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
				Require(material->GetVersion()->GetBindingsRaw()->GetShaderBindings().Find("material"_h, materialBinding),
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
				if (frame == 0 && camera == 1 && !paged && !instanced && !skinned && mobility == EMobilityType::Static &&
					VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT)
				{
					const glm::ivec2 extent(19, 13);
					auto refusedDepth = driver->CreateRenderTarget(extent, 1, EFormat::D32_SFLOAT,
						ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit);
					auto refusedColor = driver->CreateRenderTarget(extent, 1, EFormat::R32G32B32A32_SFLOAT);
					depth->SetRHIResource("depthStencil"_h, refusedDepth);
					main->SetRHIResource("depthStencil"_h, refusedDepth);
					main->SetRHIResource("color"_h, refusedColor);
					for (FrameGraphNodePtr node : { FrameGraphNodePtr(depth), FrameGraphNodePtr(main) })
					{
						auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
						auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
						commands->BeginCommandList(upload, true);
						commands->BeginCommandList(draw, true);
						Tests::RequireAttachmentInitializationRefusal([&]() { node->Process(graph, upload, draw, snapshot); });
						Require(node->GetDrawCallStats().m_numBatches == 0 && node->GetDrawCallStats().m_numInstances == 0,
							"a refused depth or scene pass must not report packed draws");
						CompleteCommands(upload, draw);
					}
					depth->SetRHIResource("depthStencil"_h, prepassDepth);
					main->SetRHIResource("depthStencil"_h, mainDepth);
					main->SetRHIResource("color"_h, color);
				}
				auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(draw, true);
				depth->Process(graph, upload, draw, snapshot);
				RHITexturePtr liveDepth = prepassDepth;
				if (bDepthSurface)
				{
					liveDepth = depth->GetTargetAttachment("depthStencil"_h, graph.GetRawPtr());
					const bool bResolve = liveDepth != prepassDepth;
					Require(recordedDepth.imageView == static_cast<VkImageView>(*liveDepth->m_vulkan.m_imageView) &&
						recordedDepth.resolveImageView == (bResolve ? static_cast<VkImageView>(*prepassDepth->m_vulkan.m_imageView) : VK_NULL_HANDLE),
						"depth prepass must use both views of the authored Surface, not a cached replacement target");
				}
				auto depthPixels = ReadDepth(draw, prepassDepth, depthReadback);
				auto liveDepthPixels = liveDepth == prepassDepth ? depthPixels : ReadDepth(draw, liveDepth.DynamicCast<RHIRenderTarget>(), depthReadback);
				const auto liveMainDepth = main->GetTargetAttachment("depthStencil"_h, graph.GetRawPtr());
				const RHITexturePtr mainDepthResolve = liveMainDepth != mainDepth ? mainDepth : RHITexturePtr{};
				commands->BeginRenderPass(draw, TVector<RHITexturePtr>{ color }, TVector<RHITexturePtr>{ nullptr }, liveMainDepth, mainDepthResolve,
					glm::ivec4(0, 0, Side, Side), glm::ivec2(0), true, glm::vec4(0), 0, true, true);
				commands->EndRenderPass(draw);
				main->Process(graph, upload, draw, snapshot);
				if (bDepthSurface)
					Require(recordedDepth.imageView == static_cast<VkImageView>(*liveMainDepth->m_vulkan.m_imageView) &&
						recordedDepth.resolveImageView == (mainDepthResolve ? static_cast<VkImageView>(*mainDepth->m_vulkan.m_imageView) : VK_NULL_HANDLE),
						"main pass must use both views of the authored depth Surface");
				auto mainDepthPixels = ReadDepth(draw, mainDepth, depthReadback);
				auto liveMainDepthPixels = liveMainDepth == mainDepth ? mainDepthPixels :
					ReadDepth(draw, liveMainDepth.DynamicCast<RHIRenderTarget>(), depthReadback);
				auto mainPixels = ReadColor(draw, color);
				shadow->Process(graph, upload, draw, snapshot);
				auto shadowPixels = ReadColor(draw, snapshot.m_shadowMapsToUpdate[0].m_shadowMap);
				CompleteCommands(upload, draw);
				Require(depth->GetDrawCallStats().m_numInstances == 2 && main->GetDrawCallStats().m_numInstances == 2 &&
					shadow->GetDrawCallStats().m_numInstances == 2, "all three real passes must draw both fixture instances");
				const auto depths = static_cast<const glm::vec2*>(depthPixels->GetPointer());
				const auto liveDepths = static_cast<const glm::vec2*>(liveDepthPixels->GetPointer());
				const auto mainDepths = static_cast<const glm::vec2*>(mainDepthPixels->GetPointer());
				const auto liveMainDepths = static_cast<const glm::vec2*>(liveMainDepthPixels->GetPointer());
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
						Require((mainDepths[pixel].x > 0.1f) == covered, "main-pass resolved depth must match its color silhouette");
						for (uint32_t sample = 0; sample < static_cast<uint32_t>(liveDepth->GetMsaaSamples()); ++sample)
							Require((liveDepths[pixel * static_cast<uint32_t>(liveDepth->GetMsaaSamples()) + sample].x > 0.1f) == covered,
								"each sample of the authored live depth target must retain the rendered silhouette");
						for (uint32_t sample = 0; sample < static_cast<uint32_t>(liveMainDepth->GetMsaaSamples()); ++sample)
							Require((liveMainDepths[pixel * static_cast<uint32_t>(liveMainDepth->GetMsaaSamples()) + sample].x > 0.1f) == covered,
								"each sample of the main-pass depth target must retain the rendered silhouette");
						Require((shadows[pixel].r > 0.1f) == covered, "packed custom shadow pixels must match the main-pass silhouette");
					}
			}
			VerifyConcurrentPacketPreparation(graph, snapshot, *main, *depth, *shadow, mobility);
			RHIMaterial::EndSubmissionVersionCapture(submissionId);
		}
		std::cout << "Custom masked depth paged=" << paged << " instanced=" << instanced << " skinned=" << skinned
			<< " mobility=" << static_cast<uint32_t>(mobility)
			<< " depthBinding=" << magic_enum::enum_name(depthBinding)
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
		Require(driver->AddBufferToShaderBindings(bindings, buffer, "material"_h, 0).IsValid(), "transparent fixture parameters must bind");
		const RenderState state(false, false, 0, false, ECullMode::None, EBlendMode::AlphaBlending,
			EFillMode::Fill, "Translucent"_h.GetHash(), true);
		auto material = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shader, bindings);
		Require(material && material->GetVersion(), "transparent fixture must have a complete material");
		auto scene = RHIScenePtr::Make();
		uint32_t id = 0;
		for (auto mobility : { EMobilityType::Static, EMobilityType::Stationary, EMobilityType::Dynamic })
		{
			RHISceneViewProxy source;
			++id;
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
				source.m_meshModelMatrices = { glm::mat4(1) };
				source.m_overrideMaterials = { material };
			}
			RHISceneInstanceRecord record;
			record.m_producerKey = id;
			record.m_mobility = mobility;
			record.m_worldMatrix = glm::translate(glm::mat4(1), glm::vec3(0, 0, -static_cast<float>(id)));
			record.m_worldBounds = Math::AABB(glm::vec3(0, 0, -static_cast<float>(id)), glm::vec3(1));
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
		snapshot.m_sceneVersions = TSharedPtr<const TVector<RHISceneVersionPtr>>::Make(
			TVector<RHISceneVersionPtr>{ scene->PublishVersion() });
		snapshot.m_camera = TUniquePtr<CameraData>::Make(cameraData);
		auto node = TRefPtr<SceneNode>::Make();
		node->SetString("Tag"_h, "Translucent");
		node->SetString("Sorting"_h, "BackToFront");
		node->SetString("VirtualizeInstancePayloads"_h, paged ? "true" : "false");
		const auto firstResources = node->GetResources(snapshot);
		for (uint32_t cameraIndex = 0; cameraIndex < 2; ++cameraIndex)
		{
			snapshot.m_cameraIndex = cameraIndex;
			snapshot.m_proxies.Clear();
			for (auto mobility : { EMobilityType::Static, EMobilityType::Stationary, EMobilityType::Dynamic })
				snapshot.ForEachSceneProxy(mobility, [&](const RHIVisibleSceneProxy& proxy)
				{
					if (cameraIndex == 0 || proxy.m_record->m_producerKey != 2) snapshot.m_proxies.Add(proxy);
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

		const glm::vec4 settings(0, 0, 0, 1);
		std::memcpy(buffer->GetPointer(), &settings, sizeof(settings));
		UboFrameData frame{};
		frame.m_view = frame.m_projection = frame.m_invProjection = glm::mat4(1);
		frame.m_projection[2][2] = -0.2f;
		frame.m_viewportSize = glm::ivec2(Side);
		auto frameBuffer = driver->CreateBuffer(sizeof(frame), EBufferUsageBit::UniformBuffer_Bit, HostMemory);
		std::memcpy(frameBuffer->GetPointer(), &frame, sizeof(frame));
		snapshot.m_frameBindings = driver->CreateShaderBindings();
		driver->AddBufferToShaderBindings(snapshot.m_frameBindings, frameBuffer, "frame"_h, 0);
		snapshot.m_rhiLightsData = driver->CreateShaderBindings();
		snapshot.m_bGlobalIlluminationEnabled = false;
		auto color = driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
		auto depth = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::D32_SFLOAT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit |
			ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit);
		node->SetRHIResource("color"_h, color);
		node->SetRHIResource("depthStencil"_h, depth);
		auto commands = Renderer::GetDriverCommands();
		for (uint32_t cameraIndex : { 1u, 0u })
		{
			snapshot.m_cameraIndex = cameraIndex;
			const auto resources = node->GetResources(snapshot);
			const auto& packet = resources->m_packet;
			const auto count = packet.GetNumInstances();
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			ClearColor(draw, color->GetTarget(), glm::vec4(0));
			node->Process(graph, upload, draw, snapshot);
			Require(node->GetDrawCallStats().m_numInstances == count &&
				packet.m_metrics.m_instanceUploadBytes == count * sizeof(RenderSceneNode::PerInstanceData) &&
				packet.m_metrics.m_dirtyInstanceRanges == 1,
				"each transparent camera must record its own draws and one complete dynamic upload");
			draw->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			const auto readBuffer = [&](const auto& source, size_t size)
			{
				auto result = driver->CreateBuffer(size, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
				draw->m_vulkan.m_commandBuffer->CopyBuffer(source, *result->m_vulkan.m_buffer->Get(), size);
				return result;
			};
			const auto& instanceBindings = resources->m_perInstanceData->GetShaderBindings();
			const auto instances = readBuffer(*instanceBindings["data"_h]->m_vulkan.m_valueBinding->Get(),
				count * sizeof(RenderSceneNode::PerInstanceData));
			const auto indices = readBuffer(*instanceBindings["indices"_h]->m_vulkan.m_valueBinding->Get(),
				count * sizeof(uint32_t));
			const auto indirect = readBuffer(*resources->m_indirectBuffers[0]->m_vulkan.m_buffer->Get(),
				count * sizeof(DrawIndexedIndirectData));
			const auto pixels = ReadColor(draw, color->GetResolved());
			CompleteCommands(upload, draw);
			const auto* actualInstances = static_cast<const RenderSceneNode::PerInstanceData*>(instances->GetPointer());
			const auto* actualIndices = static_cast<const uint32_t*>(indices->GetPointer());
			const auto* actualDraws = static_cast<const DrawIndexedIndirectData*>(indirect->GetPointer());
			for (uint32_t i = 0; i < count; ++i)
			{
				Require(actualInstances[i] == packet.GetPayload(EMobilityType::Dynamic).m_instances[i] &&
					actualIndices[i] == i && actualDraws[i].m_firstInstance == i && actualDraws[i].m_instanceCount == 1 &&
					actualDraws[i].m_indexCount == mesh->GetIndexCount() && actualDraws[i].m_firstIndex == mesh->GetFirstIndex() &&
					actualDraws[i].m_vertexOffset == mesh->GetVertexOffset(),
					"submitted GPU instances, indices and indirect commands must preserve the prepared transparent order");
			}
			const auto* colors = static_cast<const glm::vec4*>(pixels->GetPointer());
			for (uint32_t pixel = 0; pixel < Side * Side; ++pixel)
			{
				Require(colors[pixel] == glm::vec4(1), "each camera's transparent draws must cover the target");
			}
		}
		std::cout << "Transparent packet paged=" << paged << " instanced=" << instanced
			<< ": two cameras, mixed mobility, material versions, GPU draw order, uploads and pixels passed\n";
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
		Require(driver->AddBufferToShaderBindings(bindings, buffer, "material"_h, 0).IsValid(), "depth fixture parameters must bind");
		const RenderState state(true, true, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill,
			masked ? "Masked"_h.GetHash() : "Opaque"_h.GetHash(), true);
		auto material = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shader, bindings);
		Require(material && material->GetVersion(), "depth fixture must have a complete source material");
		TVector<glm::mat4> models{ glm::mat4(1), glm::mat4(1), glm::mat4(1) };
		models[1][3].x = 1;
		models[2][3].x = 2;
		const TVector<glm::vec4> colors{ glm::vec4(0), glm::vec4(0.5f), glm::vec4(1) };
		RHISceneViewProxy source;
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
		record.m_worldBounds = Math::AABB(glm::vec3(0), glm::vec3(4));
		record.m_topology = RHISceneProxyResourcePtr::Make(std::move(source));
		auto scene = RHIScenePtr::Make();
		scene->AddInstance(record);
		RHISceneViewSnapshot snapshot;
		snapshot.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		snapshot.m_sceneVersions = TSharedPtr<const TVector<RHISceneVersionPtr>>::Make(
			TVector<RHISceneVersionPtr>{ scene->PublishVersion() });
		snapshot.ForEachSceneProxy(mobility, [&](const RHIVisibleSceneProxy& proxy) { snapshot.m_proxies.Add(proxy); });
		auto node = TRefPtr<DepthNode>::Make();
		node->SetString("Tag"_h, masked ? "Masked" : "Opaque");
		node->SetString("VirtualizeInstancePayloads"_h, paged ? "true" : "false");
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

	void TestShadowVerticalPublication(const std::array<ShaderSetPtr, 4>& shaders)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto* nativeDriver = driver.DynamicCast<VulkanGraphicsDriver>();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<ShadowCacheProbe>::Make();
		node->m_pBlurHorizontalShader = shaders[2];
		node->m_pBlurVerticalShader = shaders[3];
		RHISceneViewSnapshot scene;
		scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		scene.m_submissionContext->BeginSubmission(217000, 0);
		scene.m_camera = TUniquePtr<CameraData>::Make();
		scene.m_frameBindings = driver->CreateShaderBindings();
		UboFrameData frame{};
		frame.m_view = frame.m_projection = frame.m_invProjection = glm::mat4(1);
		auto frameBuffer = driver->CreateBuffer(sizeof(frame), EBufferUsageBit::UniformBuffer_Bit, HostMemory);
		std::memcpy(frameBuffer->GetPointer(), &frame, sizeof(frame));
		driver->AddBufferToShaderBindings(scene.m_frameBindings, frameBuffer, "frame"_h, 0);
		driver->AddBufferToShaderBindings(scene.m_frameBindings, frameBuffer, "previousFrame"_h, 1);
		RHIUpdateShadowMapCommand pass;
		pass.m_shadowType = EShadowType::EVSM;
		pass.m_lightMatrix = glm::mat4(1);
		pass.m_blurRadius = glm::vec2(1);
		pass.m_shadowMap = driver->CreateRenderTarget(glm::ivec2(2 * Side), 1, EFormat::R32G32B32A32_SFLOAT);
		pass.m_payloadCompletionToken = RHISubmissionCompletionTokenPtr::Make();
		scene.m_shadowMapsToUpdate.Add(std::move(pass));
		auto resources = node->GetResources(scene);
		RHIShaderBindingSetPtr bindings;
		RHIShaderBindingPtr sampler, data;
		VulkanDescriptorSetPtr nativeA;
		VulkanImageViewPtr viewA;
		RHITexturePtr temporaryA;
		const auto readPixels = [&](RHICommandListPtr draw, RHITexturePtr texture)
		{
			const auto size = texture->GetExtent();
			auto pixels = driver->CreateBuffer(size.x * size.y * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
			commands->ImageMemoryBarrier(draw, texture, EImageLayout::TransferSrcOptimal);
			commands->CopyImageToBuffer(draw, texture, pixels);
			return pixels;
		};
		const auto checkPixels = [](RHIBufferPtr pixels, size_t count, glm::vec4 expected)
		{
			const auto values = static_cast<const glm::vec4*>(pixels->GetPointer());
			for (size_t pixel = 0; pixel < count; ++pixel)
				for (uint32_t channel = 0; channel < 4; ++channel)
					Require(std::isfinite(values[pixel][channel]) && std::abs(values[pixel][channel] - expected[channel]) < 0.00001f,
						"rejected vertical blur must retain clear output, completed H pixels and the previous image; retry must apply both filters");
		};
		for (uint32_t phase = 0; phase < 5; ++phase)
		{
			const bool reject = phase == 2;
			auto& request = scene.m_shadowMapsToUpdate[0];
			if (phase == 1)
			{
				const RenderState state(false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, false);
				node->m_pBlurHorizontalMaterial = driver->CreateMaterial(graph->GetFullscreenNdcQuad()->m_vertexDescription,
					EPrimitiveTopology::TriangleList, state, shaders[0]);
				node->m_pBlurVerticalMaterial = driver->CreateMaterial(graph->GetFullscreenNdcQuad()->m_vertexDescription,
					EPrimitiveTopology::TriangleList, state, shaders[1]);
				Require(node->m_pBlurHorizontalMaterial && node->m_pBlurVerticalMaterial, "both blur observation materials must compile");
			}
			if (reject) request.m_shadowMap = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
			auto prepare = node->Prepare(graph, scene);
			if (prepare) { prepare->Run(); prepare->Wait(); }
			Require(request.m_payloadCompletionToken->IsSuccessful(), "blur publication must start with a complete empty packet");
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			auto& pool = VulkanApi::GetInstance()->GetMainDevice()->GetCurrentThreadContext().m_descriptorPool;
			auto savedPool = pool;
			Require(static_cast<bool>(savedPool), "vertical refusal needs the existing Render descriptor pool");
			VulkanDescriptorSetPtr nativeH;
			uint64_t revisionH = 0;
			uint32_t renderPasses = 0;
			try
			{
				CaptureAttachments capture([&]()
				{
					++renderPasses;
					if (!reject) return;
					if (renderPasses == 2)
					{
						Require(recordedColor.imageView != static_cast<VkImageView>(*request.m_shadowMap->m_vulkan.m_imageView),
							"horizontal blur must render into its temporary image");
						nativeH = bindings->m_vulkan.m_descriptorSet;
						revisionH = bindings->GetDescriptorRevision();
						Require(sampler->GetTextureBinding() == request.m_shadowMap && nativeH != nativeA,
							"horizontal publication must precede vertical producer refusal");
						// Warm the normal projection before taking the pool away from the next AddSampler.
						auto sets = nativeDriver->GetCompatibleDescriptorSets(node->m_pBlurHorizontalMaterial->m_vulkan.m_pipelines[0]->m_layout,
							{ scene.m_frameBindings, bindings });
						Require(sets.Num() == 2 && sets[1] && sets[1]->IsCompiled(), "horizontal blur projection must be available");
						pool.Clear();
					}
					else if (renderPasses == 3)
					{
						Require(!pool && bindings->m_vulkan.m_descriptorSet == nativeH && bindings->GetDescriptorRevision() == revisionH &&
							sampler->GetTextureBinding() == request.m_shadowMap && nativeH->ReferencesImageView(1, 0, request.m_shadowMap->m_vulkan.m_imageView),
							"vertical producer refusal must preserve the exact horizontal publication");
						pool = savedPool;
					}
				});
				node->Process(graph, upload, draw, scene);
			}
			catch (...)
			{
				pool = std::move(savedPool);
				throw;
			}
			const bool poolRestored = pool == savedPool;
			pool = std::move(savedPool);
			Require(poolRestored && renderPasses == 3 && request.m_payloadCompletionToken->IsSuccessful() == !reject &&
				node->GetDrawCallStats().m_numBatches == (reject ? 1u : 2u) &&
				draw->GetRecordedDrawCallStats().m_numBatches == (reject ? 1u : 2u),
				"vertical producer refusal must skip only V, fail the payload and restore the pool before submission");
			if (!phase)
			{
				bindings = resources->m_blurShaderBindings;
				sampler = bindings->GetOrAddShaderBinding("colorSampler"_h);
				data = bindings->GetOrAddShaderBinding("data"_h);
			}
			Require(resources->m_blurShaderBindings == bindings && bindings->GetOrAddShaderBinding("data"_h) == data &&
				bindings->GetOrAddShaderBinding("colorSampler"_h) == sampler, "blur retries must retain their flight binding identities");
			const auto size = request.m_shadowMap->GetExtent();
			const size_t pixelCount = size.x * size.y;
			auto pixels = readPixels(draw, request.m_shadowMap);
			RHIBufferPtr horizontalPixels, retainedPixels;
			if (reject)
			{
				auto temporary = driver->GetOrAddTemporaryRenderTarget(request.m_shadowMap->GetFormat(), size, 1);
				Require(temporary != request.m_shadowMap && temporary->m_vulkan.m_imageView != viewA,
					"failed V must leave a distinct current horizontal image");
				horizontalPixels = readPixels(draw, temporary);
				driver->ReleaseTemporaryRenderTarget(temporary);
			}
			if (phase == 3) retainedPixels = readPixels(draw, temporaryA);
			CompleteCommands(upload, draw);
			const glm::vec4 expected = !phase || reject ? glm::vec4(1, 1, -1, 1) : glm::vec4(1.25f, 1.5f, -1, 1);
			checkPixels(pixels, pixelCount, expected);
			if (horizontalPixels) checkPixels(horizontalPixels, pixelCount, { 1.25f, 1, -1, 1 });
			if (retainedPixels) checkPixels(retainedPixels, 4 * Side * Side, { 1.25f, 1, -1, 1 });
			if (phase == 1)
			{
				nativeA = bindings->m_vulkan.m_descriptorSet;
				temporaryA = sampler->GetTextureBinding();
				viewA = temporaryA->m_vulkan.m_imageView;
			}
			if (phase >= 2) Require(nativeA->IsCompiled() && nativeA->ReferencesImageView(1, 0, viewA),
				"replacing the shadow map must retain the previous native sampler image");
			if (phase >= 3) Require(sampler->GetTextureBinding() != request.m_shadowMap &&
				sampler->GetTextureBinding()->GetExtent() == glm::ivec2(Side), "retry must publish the new horizontal temporary for V");
		}
		std::cout << "Shadow vertical producer: completed H, refused V, retained publication, same-target retry and full pixels passed\n";
	}

	void TestShadowAttachmentRefusal(const std::array<ShaderSetPtr, 4>& shaders)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		uint32_t caseIndex = 0;
		for (uint32_t failedAttachment : { 0u, 1u })
		{
			for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				const glm::ivec2 size(37 + 2 * caseIndex++, 19);
				auto graph = TRefPtr<TestGraph>::Make();
				auto node = TRefPtr<ShadowCacheProbe>::Make();
				node->m_pBlurHorizontalShader = shaders[2];
				node->m_pBlurVerticalShader = shaders[3];
				RHISceneViewSnapshot scene;
				scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
				scene.m_submissionContext->BeginSubmission(224000 + caseIndex, 0);
				scene.m_camera = TUniquePtr<CameraData>::Make();
				scene.m_frameBindings = driver->CreateShaderBindings();
				UboFrameData frame{};
				frame.m_view = frame.m_projection = frame.m_invProjection = glm::mat4(1);
				auto frameBuffer = driver->CreateBuffer(sizeof(frame), EBufferUsageBit::UniformBuffer_Bit, HostMemory);
				std::memcpy(frameBuffer->GetPointer(), &frame, sizeof(frame));
				driver->AddBufferToShaderBindings(scene.m_frameBindings, frameBuffer, "frame"_h, 0);
				driver->AddBufferToShaderBindings(scene.m_frameBindings, frameBuffer, "previousFrame"_h, 1);
				RHIUpdateShadowMapCommand pass;
				pass.m_shadowType = EShadowType::EVSM;
				pass.m_lightMatrix = glm::mat4(1);
				pass.m_blurRadius = glm::vec2(1);
				pass.m_shadowMap = driver->CreateRenderTarget(size, 1, EFormat::R32G32B32A32_SFLOAT);
				pass.m_payloadCompletionToken = RHISubmissionCompletionTokenPtr::Make();
				scene.m_shadowMapsToUpdate.Add(std::move(pass));
				auto& request = scene.m_shadowMapsToUpdate[0];
				for (bool reject : { true, false })
				{
					auto prepare = node->Prepare(graph, scene);
					if (prepare) { prepare->Run(); prepare->Wait(); }
					Require(request.m_payloadCompletionToken->IsSuccessful(), "the prepared empty shadow packet must be complete");
					auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
					commands->BeginCommandList(upload, true);
					commands->BeginCommandList(draw, true);
					ClearColor(draw, request.m_shadowMap, glm::vec4(-8));
					if (reject)
						Tests::RequireAttachmentInitializationRefusal([&]() { node->Process(graph, upload, draw, scene); }, failedAttachment, error);
					else node->Process(graph, upload, draw, scene);
					Require(request.m_payloadCompletionToken->IsSuccessful() == !reject &&
						node->GetDrawCallStats().m_numBatches == (reject ? 0u : 2u) &&
						draw->GetRecordedDrawCallStats().m_numBatches == (reject ? 0u : 2u),
						"a missing depth or blur attachment must fail the shadow payload and record no draws; retry must record both filters");
					auto pixels = driver->CreateBuffer(size.x * size.y * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
					commands->ImageMemoryBarrier(draw, request.m_shadowMap, EImageLayout::TransferSrcOptimal);
					commands->CopyImageToBuffer(draw, request.m_shadowMap, pixels);
					CompleteCommands(upload, draw);
					const auto values = static_cast<const glm::vec4*>(pixels->GetPointer());
					const glm::vec4 expected = reject ? glm::vec4(-8) : glm::vec4(1, 1, -1, 1);
					for (int32_t pixel = 0; pixel < size.x * size.y; ++pixel)
						for (uint32_t channel = 0; channel < 4; ++channel)
							Require(std::isfinite(values[pixel][channel]) && std::abs(values[pixel][channel] - expected[channel]) < 0.00001f,
								"rejected shadow attachments must preserve every sentinel pixel; retry must produce filtered EVSM output");
				}
			}
		}
		std::cout << "Shadow attachments: depth/blur native refusal, failed payload, no draws and same-target filtered pixel recovery passed\n";
	}

	void TestShadowBlitInitialization()
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		int32_t halfWidth = 13;
		for (auto error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			const glm::ivec2 size(2 * halfWidth, 11);
			auto graph = RHIFrameGraphPtr::Make();
			auto node = TRefPtr<ShadowPrepassNode>::Make();
			auto target = driver->CreateRenderTarget(size, 1, EFormat::R32G32B32A32_SFLOAT);
			RHISceneViewSnapshot scene;
			scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
			scene.m_submissionContext->BeginSubmission(224100 + halfWidth, 0);
			scene.m_shadowMapsToBlit.Add({ target, target, { 0, 0, halfWidth, size.y }, { halfWidth, 0, halfWidth, size.y } });
			for (bool reject : { true, false })
			{
				auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(draw, true);
				commands->ImageMemoryBarrier(draw, target, EImageLayout::ColorAttachmentOptimal);
				Require(commands->BeginRenderPass(draw, TVector<RHITexturePtr>{ target }, {}, { 0, 0, size.x, size.y }, {},
					true, glm::vec4(0.25f), 0, false), "the self-blit fixture must initialize its two distinct regions");
				commands->ClearAttachments(draw, { halfWidth, 0, halfWidth, size.y }, glm::vec4(0.75f), 0);
				commands->EndRenderPass(draw);
				if (reject) Tests::RequireAttachmentInitializationRefusal([&]() { node->Process(graph, upload, draw, scene); }, 0, error);
				else node->Process(graph, upload, draw, scene);
				auto pixels = driver->CreateBuffer(size.x * size.y * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
				commands->ImageMemoryBarrier(draw, target, EImageLayout::TransferSrcOptimal);
				commands->CopyImageToBuffer(draw, target, pixels);
				CompleteCommands(upload, draw);
				const auto values = static_cast<const glm::vec4*>(pixels->GetPointer());
				for (int32_t y = 0; y < size.y; ++y)
					for (int32_t x = 0; x < size.x; ++x)
						Require(values[y * size.x + x] == glm::vec4(reject && x >= halfWidth ? 0.75f : 0.25f),
							"a refused self-blit scratch image must preserve both regions; retry must copy the source into the destination");
			}
			halfWidth += 2;
		}
		std::cout << "Shadow self-blit: native scratch refusal, unchanged regions and same-command pixel recovery passed\n";
	}

	void TestCustomShadowCache(ShaderSetPtr shader, bool paged)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto mesh = graph->GetFullscreenNdcQuad();
		mesh->m_bounds = Math::AABB(glm::vec3(0), glm::vec3(2));
		auto node = TRefPtr<ShadowCacheProbe>::Make();
		node->SetString("VirtualizeInstancePayloads"_h, paged ? "true" : "false");
		node->SetString("GPUCulling"_h, "false");
		auto liveBindings = TSharedPtr<uint32_t>::Make(0u);
		TVector<RHIMaterialPtr> retiredSources;
		RHIRenderSubmissionContextPtr heldContext;
		auto reusedContext = RHIRenderSubmissionContextPtr::Make();
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
			Require(driver->AddBufferToShaderBindings(bindings, buffer, "material"_h, 0).IsValid(),
				"counted source bindings must use real native descriptors");
			const RenderState state(true, true, 0, true, ECullMode::None, EBlendMode::None, EFillMode::Fill, "Masked"_h.GetHash(), true);
			auto source = driver->CreateMaterial(mesh->m_vertexDescription, EPrimitiveTopology::TriangleList, state, shader, bindings);
			RHISceneViewSnapshot snapshot;
			snapshot.m_frame = frame;
			snapshot.m_submissionContext = frame == 1 ? RHIRenderSubmissionContextPtr::Make() : reusedContext;
			snapshot.m_camera = TUniquePtr<CameraData>::Make();
			snapshot.m_frameBindings = driver->CreateShaderBindings();
			snapshot.m_rhiLightsData = driver->CreateShaderBindings();
			UboFrameData frameData{};
			frameData.m_view = frameData.m_projection = frameData.m_invProjection = glm::mat4(1);
			auto frameBuffer = driver->CreateBuffer(sizeof(frameData), EBufferUsageBit::UniformBuffer_Bit, HostMemory);
			std::memcpy(frameBuffer->GetPointer(), &frameData, sizeof(frameData));
			driver->AddBufferToShaderBindings(snapshot.m_frameBindings, frameBuffer, "frame"_h, 0);
			RHISceneViewProxy proxy;
			proxy.m_meshes = { mesh };
			proxy.m_overrideMaterials = { source };
			auto shadowCaster = TSharedPtr<RHIShadowCasterProxy>::Make();
			RHIShadowMeshProxy caster;
			caster.m_mesh = mesh;
			caster.m_localMatrix = glm::mat4(1);
			caster.m_renderQueueTag = "Masked"_h.GetHash();
			caster.m_customDepthMaterial = source;
			caster.m_customDepthShader = shader;
#if defined(__APPLE__)
			caster.m_materialTextureSamplers = { 0 };
#endif
			shadowCaster->m_meshes.Add(std::move(caster));
			proxy.m_shadowCaster = std::move(shadowCaster);
			RHISceneInstanceRecord record;
			record.m_producerKey = frame;
			record.m_mobility = EMobilityType::Static;
			record.m_worldMatrix = glm::mat4(1);
			record.m_worldBounds = mesh->m_bounds;
			record.m_topology = RHISceneProxyResourcePtr::Make(std::move(proxy));
			record.m_renderFlags = 1;
			auto scene = RHIScenePtr::Make();
			scene->AddInstance(record);
			const uint32_t instanceCount = frame < 3 ? 1u : 2u;
			if (instanceCount == 2)
			{
				record.m_producerKey += 100;
				scene->AddInstance(record);
			}
			snapshot.m_sceneVersions = TSharedPtr<const TVector<RHISceneVersionPtr>>::Make(
				TVector<RHISceneVersionPtr>{ scene->PublishVersion() });
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
			snapshot.m_submissionContext->BeginSubmission(submissionId, 0, revision);
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
#if defined(__APPLE__)
			const bool bRetryStorage = frame == 1 || frame == 3;
			if (bRetryStorage)
			{
				auto view = node->GetResources(snapshot)->m_activeShadowViews[0];
				const auto previousSet = view->m_perInstanceData;
				const auto previousNative = previousSet ? previousSet->m_vulkan.m_descriptorSet : VulkanDescriptorSetPtr{};
				const auto previousRevision = previousSet ? previousSet->GetDescriptorRevision() : 0u;
				const auto previousBytes = view->m_sizePerInstanceData;
				const auto previousIndices = view->m_sizeInstanceIndices;
				Require(bool(previousSet) == (frame == 3) && view->m_packet.GetNumStorageInstances() == instanceCount &&
					view->m_packet.GetNumDrawInstances() == instanceCount && previousIndices < sizeof(uint32_t) * instanceCount,
					"the fixture must exercise cold publication and real same-view storage growth");
				const auto failure = Tests::RefuseSecondVulkanDescriptorAllocation([&]()
					{
						node->Process(graph, upload, draw, snapshot);
					});
				Require(failure.m_bFirstStorageWritten && failure.m_failedLayout,
					"the first SSBO must reach Vulkan before the second descriptor allocation is refused");
				Require(view->m_perInstanceData == previousSet && view->m_sizePerInstanceData == previousBytes &&
					view->m_sizeInstanceIndices == previousIndices && view->m_packet.m_metrics.m_instanceUploadBytes == 0 &&
					(!previousSet || (previousSet->m_vulkan.m_descriptorSet == previousNative && previousSet->GetDescriptorRevision() == previousRevision)),
					"second-SSBO refusal must preserve the published pair, capacities and revision without uploading a partial payload");
				Require(!snapshot.m_shadowMapsToUpdate[0].m_payloadCompletionToken->IsSuccessful() &&
					node->GetDrawCallStats().m_numInstances == 2 * instanceCount,
					"only the incomplete cascade must fail; the other two cascades must draw");
				auto empty = ReadColor(draw, snapshot.m_shadowMapsToUpdate[0].m_shadowMap);
				CompleteCommands(upload, draw);
				const auto values = static_cast<const glm::vec4*>(empty->GetPointer());
				Require(std::all_of(values, values + Side * Side, [](glm::vec4 value) { return value == glm::vec4(0); }),
					"the refused cascade must contain only clear pixels, not draws from absent or undersized storage");
				auto retry = node->Prepare(graph, snapshot);
				retry->Run();
				retry->Wait();
				Require(node->GetResources(snapshot)->m_activeShadowViews[0] == view,
					"publication retry must use the same shadow view");
				upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(draw, true);
			}
#endif
			node->Process(graph, upload, draw, snapshot);
			Require(node->GetDrawCallStats().m_numInstances == 3 * instanceCount,
				"all three cascades must draw the full packet after publication");
#if defined(__APPLE__)
			if (bRetryStorage)
			{
				const auto view = node->GetResources(snapshot)->m_activeShadowViews[0];
				const auto bindings = view->m_perInstanceData;
				Require(bindings && bindings->HasBinding("data"_h) && bindings->HasBinding("indices"_h) &&
					view->m_sizePerInstanceData == sizeof(ShadowPrepassNode::PerInstanceData) * instanceCount &&
					view->m_sizeInstanceIndices == sizeof(uint32_t) * instanceCount &&
					view->m_packet.m_metrics.m_instanceUploadBytes == view->m_sizePerInstanceData &&
					snapshot.m_shadowMapsToUpdate[0].m_payloadCompletionToken->IsSuccessful(),
					"same-view retry must publish and upload both SSBOs");
			}
#endif
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
		reusedContext.Clear();
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
#if defined(__APPLE__)
		std::cout << "Shadow storage paged=" << paged << ": second-SSBO cold/growth refusal and same-packet pixel recovery passed\n";
#endif
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
		commands->BeginRenderPass(command, TVector<RHITexturePtr>{}, TVector<RHITexturePtr>{}, depth, nullptr,
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

	void TestImportedRendering(const std::array<FileId, 2>& ids, const TVector<FileId>& repairedGraphs)
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
		TVector<FrameGraphPtr> instances(2);
		Require(importer->LoadFrameGraph_Immediate(ids[0], instances[0]) &&
			importer->Instantiate_Immediate(ids[0], instances[1]), "both graph instances must load");
		Require(instances[0]->GetRHI() != instances[1]->GetRHI() &&
			instances[0]->GetRHI()->GetSurface("Main"_h) != instances[1]->GetRHI()->GetSurface("Main"_h),
			"instantiating the same asset must not share mutable targets or nodes");
		for (const auto id : repairedGraphs)
		{
			FrameGraphPtr repaired;
			Require(importer->LoadFrameGraph_Immediate(id, repaired) && repaired,
				"every repaired graph must remain available for actual rendering");
			instances.Add(repaired);
		}
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
		for (uint32_t instanceIndex = 0; instanceIndex < instances.Num(); ++instanceIndex)
		{
			auto graph = instances[instanceIndex]->GetRHI();
			auto node = graph->GetGraphNode("Composite"_h).DynamicCast<PostProcessNode>();
			const auto output = graph->GetSurface("Main"_h);
			const auto source = graph->GetSampler("ById"_h);
			Require(source && node->GetSampledAttachment("sourceSampler"_h) == source,
				"the imported node must retain its declared source sampler");
			if (instanceIndex < 2)
				Require(source == graph->GetSampler("ByPath"_h) && source == graph->GetSampler("Both"_h),
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
				graph->SetSurface("DynamicInput"_h, published ? externalSurface : RHISurfacePtr{});
				graph->SetSampler("DynamicInput"_h, published && !externalSurface ? RHITexturePtr(external) : RHITexturePtr{});
				if (frame >= 2) graph->SetSampler("ById"_h, external);
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
					Require(bindings->GetOrAddShaderBinding("sourceSampler"_h)->GetTextureBinding() == source &&
						bindings->GetOrAddShaderBinding("externalSampler"_h)->GetTextureBinding() == (published ? RHITexturePtr(external) : fallback),
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
		std::cout << "FrameGraph imported rendering: two original instances and " << repairedGraphs.Num()
			<< " repaired graphs, six frames each, native attachments, exact pixels and external replacement passed\n";
	}

	void TestMotionBlurNode()
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		graph->SetRenderTarget("Main"_h, driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT));
		FrameGraphBuilder builder;
		auto node = builder.CreateNode("MotionBlur"_h);
		Require(node.IsValid(), "motion history must be available through the data-driven node factory");
		World world("MotionHistoryCameras", 0);
		auto camera = world.Instantiate("Camera")->AddComponent<CameraComponent>();
		world.GetECS<CameraECS>()->Tick(0);
		auto context = RHIRenderSubmissionContextPtr::Make();
		std::array<RHISceneViewSnapshot, 2> views;
		std::array<UboFrameData, 2> previousFrames;
		std::array<TSharedPtr<const TVector<glm::mat4>>, 2> previousBones;
		std::array<RHIShaderBindingPtr, 2> frameAllocations, boneAllocations;
		for (uint32_t i = 0; i < views.size(); ++i)
		{
			auto& view = views[i];
			view.m_cameraIndex = i;
			view.m_camera = TUniquePtr<CameraData>::Make(camera->GetData());
			view.m_submissionContext = context;
			view.m_frameBindings = driver->CreateShaderBindings();
		}
		const uint32_t boneCounts[] = { 1, 5, 5, 5, 0, 0 };
		for (uint32_t frame = 0; frame < std::size(boneCounts); ++frame)
		{
			context->BeginSubmission(4800 + frame, 0);
			if (frame == 4) node->Clear();
			auto completion = RHISubmissionCompletionTokenPtr::Make();
			for (uint32_t i = 0; i < views.size(); ++i)
			{
				auto& view = views[i];
				view.m_submissionCompletionToken = completion;
				view.m_currentTime = 0.016f * frame;
				view.m_cameraTransform.m_position = glm::vec4(float(i * 20 + frame), 0, 0, 1);
				if (frame == 2 && i == 1) view.m_camera->ResetMotionHistory();
				auto bones = TSharedPtr<TVector<glm::mat4>>::Make();
				for (uint32_t bone = 0; bone < boneCounts[frame]; ++bone)
					bones->Add(glm::translate(glm::mat4(1), glm::vec3(float(100 * i + 10 * frame + bone), 0, 0)));
				view.m_cpuBoneMatrices = bones;
				node->Prepare(graph, view);
				const bool hasHistory = frame != 0 && frame != 4 && !(frame == 2 && i == 1);
				Require(view.m_previousMotionFrame.IsValid() == hasHistory,
					"each camera must retain its submitted history and reset on a cut or node Clear");
				const auto expectedFrame = hasHistory ? previousFrames[i] : view.GetFrameData(glm::ivec2(Side));
				const auto expectedBones = hasHistory ? previousBones[i] : view.m_cpuBoneMatrices;
				if (hasHistory)
				{
					Require(view.m_previousMotionFrame->m_frameData.m_cameraPosition == expectedFrame.m_cameraPosition &&
						view.m_previousMotionFrame->m_frameData.m_currentTime == expectedFrame.m_currentTime &&
						view.m_previousMotionFrame->m_bones == expectedBones,
						"camera and skeleton history must come from the same submitted frame");
				}
				auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				node->Process(graph, upload, {}, view);
				auto frameBinding = view.m_frameBindings->GetOrAddShaderBinding("previousFrameData"_h);
				auto boneBinding = view.m_frameBindings->GetOrAddShaderBinding("previousBones"_h);
				if (frame > 0) Require(frameBinding == frameAllocations[i], "camera history must reuse its uniform allocation");
				if (frame > 2) Require(boneBinding == boneAllocations[i], "grown bone storage must not be reallocated every frame");
				frameAllocations[i] = frameBinding;
				boneAllocations[i] = boneBinding;
				const size_t count = (std::max)(size_t{ 1 }, expectedBones->Num());
				Require(boneBinding->m_vulkan.m_valueBinding->Get().m_size >= count * sizeof(glm::mat4),
					"bone storage must accommodate the retained pose, including the empty-pose identity");
				commands->EndCommandList(upload);
				auto fence = RHIFencePtr::Make();
				Require(driver->SubmitCommandList(upload, fence) && fence->Wait(5000000000ull) == EFenceStatus::Finished,
					"motion history uploads must complete on the GPU");
				previousFrames[i] = view.GetFrameData(glm::ivec2(Side));
				previousBones[i] = view.m_cpuBoneMatrices;
			}
			completion->Complete(true);
		}
		std::cout << "MotionBlur history: two cameras, cuts, Clear, GPU uploads and allocation reuse passed\n";
	}

	void TestSceneRenderExtent()
	{
		auto& driver = Renderer::GetDriver();
		for (bool named : { false, true })
		for (bool surface : { false, true })
		{
			auto graph = RHIFrameGraphPtr::Make();
			auto node = TRefPtr<DebugDrawNode>::Make();
			node->SetTag("DebugDraw"_h);
			graph->GetGraph().Add(node);
			graph->SetRenderTarget("Main"_h, driver->CreateRenderTarget(glm::ivec2(17, 9), 1, EFormat::R16G16B16A16_SFLOAT));
			if (named) node->SetRHIResource_Unresolved("color"_h, "SceneOutput"_h);
			for (const auto extent : { glm::ivec2(31, 17), glm::ivec2(9, 43), glm::ivec2(65, 7) })
			{
				auto target = driver->CreateRenderTarget(extent, 1, EFormat::R16G16B16A16_SFLOAT);
				const auto resource = surface ? RHIResourcePtr(driver->CreateSurface(target)) : RHIResourcePtr(target);
				if (!named) node->SetRHIResource("color"_h, resource);
				else if (surface) graph->SetSurface("SceneOutput"_h, resource.DynamicCast<RHISurface>());
				else graph->SetRenderTarget("SceneOutput"_h, target);
				const auto actual = graph->GetSceneRenderExtent();
				if (actual != extent)
					throw std::runtime_error("Scene extent named=" + std::to_string(named) + " surface=" + std::to_string(surface) +
						": actual=" + std::to_string(actual.x) + "x" + std::to_string(actual.y) +
						", expected=" + std::to_string(extent.x) + "x" + std::to_string(extent.y));
			}
			graph->GetGraph().Clear();
			Require(graph->GetSceneRenderExtent() == glm::ivec2(17, 9), "an absent scene pass must retain Main target fallback");
			graph->SetSurface("Main"_h, driver->CreateSurface(glm::ivec2(19, 11), 1, EFormat::R16G16B16A16_SFLOAT));
			Require(graph->GetSceneRenderExtent() == glm::ivec2(19, 11), "Main Surface must retain priority over its target alias");
			std::cout << "Scene extent named=" << named << " surface=" << surface
				<< ": three replacements, selected output and Main fallback priority passed\n";
		}
	}

	enum class DebugDepthInput { Default, Texture, Surface, DefaultSurface };
	enum class DepthDrawPath { DebugNode, SurfacePass, ImGuiNode };

	void TestDepthTestedDraw(ShaderSetPtr shader, ShaderSetPtr depthPattern, const std::array<ShaderSetPtr, 4>& depthReadback,
		EFormat format, uint32_t colorKind, DebugDepthInput depthInput, bool namedColor, bool namedDepth,
		DepthDrawPath path = DepthDrawPath::DebugNode, bool bFullDepthSurface = false)
	{
		auto driver = Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		auto commands = Renderer::GetDriverCommands();
		const bool colorSurface = colorKind != 0;
		const bool msaa = path != DepthDrawPath::ImGuiNode && colorKind == 1 &&
			VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
		const bool defaultDepth = depthInput == DebugDepthInput::Default || depthInput == DebugDepthInput::DefaultSurface;
		const bool depthSurface = depthInput == DebugDepthInput::Surface || depthInput == DebugDepthInput::DefaultSurface;
		auto graph = TRefPtr<TestGraph>::Make();
		FrameGraphNodePtr node = path == DepthDrawPath::ImGuiNode ?
			FrameGraphNodePtr(TRefPtr<RenderImGuiNode>::Make()) : TRefPtr<DebugDrawNode>::Make();
		if (namedColor) node->SetRHIResource_Unresolved("color"_h, "DebugColor"_h);
		if (namedDepth) node->SetRHIResource_Unresolved("depthStencil"_h, defaultDepth ? "DepthBuffer"_h : "DebugDepth"_h);
		CaptureAttachments capture;
		for (uint32_t frame = 0; frame < 3; ++frame)
		{
			if (frame == 2) node->Clear();
			auto color = colorSurface ? driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT) : RHISurfacePtr{};
			if (colorKind == 2) color = RHISurfacePtr::Make(color->GetResolved(), color->GetResolved(), false);
			auto resolved = color ? color->GetResolved() : driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
			auto target = color && path != DepthDrawPath::ImGuiNode ? color->GetTarget() : resolved;
			const auto usage = ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit;
			auto depth = driver->CreateRenderTarget(glm::ivec2(Side), 1, format, ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
			auto surface = depthSurface ? driver->CreateSurface(glm::ivec2(Side), 1, format,
				ETextureFiltration::Nearest, ETextureClamping::Clamp, usage) : RHISurfacePtr{};
			if (surface && !msaa && path != DepthDrawPath::ImGuiNode && !bFullDepthSurface)
				surface = RHISurfacePtr::Make(surface->GetResolved(), surface->GetResolved(), false);
			auto cachedDepth = msaa ? driver->GetOrAddMsaaFramebufferRenderTarget(format, glm::ivec2(Side)).StaticCast<RHIRenderTarget>() : depth;
			auto depthTarget = surface ? (msaa ? surface->GetTarget() : surface->GetResolved()) : cachedDepth;
			if (color) graph->SetSurface("DebugColor"_h, color);
			else graph->SetRenderTarget("DebugColor"_h, resolved);
			if (surface) graph->SetSurface(defaultDepth ? "DepthBuffer"_h : "DebugDepth"_h, surface);
			else graph->SetRenderTarget(defaultDepth ? "DepthBuffer"_h : "DebugDepth"_h, depth);
			if (!namedColor) node->SetRHIResource("color"_h, color ? RHIResourcePtr(color) : resolved);
			if (!namedDepth && !defaultDepth) node->SetRHIResource("depthStencil"_h, surface ? RHIResourcePtr(surface) : depth);
			if (!defaultDepth) graph->SetRenderTarget("DepthBuffer"_h, depth);
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
				auto secondaryTask = Tasks::CreateTaskWithResult<RHICommandListPtr>("Record debug attachment fixture"_h,
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
			for (auto image : { depth, cachedDepth, surface ? surface->GetResolved() : depth, surface ? surface->GetTarget() : depth })
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
			auto unusedDepthPixels = bFullDepthSurface && surface && surface->NeedsResolve() && !msaa ?
				ReadDepth(draw, surface->GetTarget(), depthReadback) : RHIBufferPtr{};
			CompleteCommands(upload, draw);
			if (unusedDepthPixels)
			{
				const auto* values = static_cast<const glm::vec2*>(unusedDepthPixels->GetPointer());
				for (uint32_t i = 0; i < Side * Side * uint32_t(surface->GetTarget()->GetMsaaSamples()); ++i)
					Require(values[i] == glm::vec2(0.9375f, 0), "single-sample debug drawing must preserve unused MSAA depth");
			}
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
		std::cout << (bFullDepthSurface ? "DebugResolvedDepth" : path == DepthDrawPath::SurfacePass ? "PrimarySurfaceDepth" : path == DepthDrawPath::ImGuiNode ? "ImGuiDepth" : "DebugDraw") << " colorSurface=" << colorSurface << " depth=" << uint32_t(depthInput) <<
			" namedColor=" << namedColor << " namedDepth=" << namedDepth << " format=" << uint32_t(format) <<
			": three replacements, native attachments and per-sample occlusion passed\n";
	}

	enum class DepthInput { Default, Texture, Surface, DefaultSurface, DefaultSurfaceOnly, DefaultSurfaceDecoy, DefaultSingleSampleSurface };

	void TestDepthHighZ(ShaderSetPtr patternShader, const std::array<ShaderSetPtr, 4>& readbackShaders,
		EFormat format, DepthInput input, bool namedInput, bool namedOutput, bool outputSurface)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<DepthHighZNode>::Make();
		const bool bDefaultSurface = input >= DepthInput::DefaultSurface;
		const bool bDefault = input == DepthInput::Default || bDefaultSurface;
		if (namedInput) node->SetRHIResource_Unresolved("src"_h, bDefault ? "DepthBuffer"_h : "CustomDepth"_h);
		if (namedOutput) node->SetRHIResource_Unresolved("dst"_h, "Pyramid"_h);
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
			graph->SetRenderTarget("DepthBuffer"_h, defaultDepth);
			auto depthSurface = input == DepthInput::Surface || bDefaultSurface ? driver->CreateSurface(inputSize, 1, format,
				ETextureFiltration::Nearest, ETextureClamping::Clamp, usage) : RHISurfacePtr{};
			if (input == DepthInput::DefaultSingleSampleSurface)
				depthSurface = RHISurfacePtr::Make(depthSurface->GetResolved(), depthSurface->GetResolved(), false);
			auto resolved = depthSurface ? depthSurface->GetResolved() : input == DepthInput::Default ? defaultDepth :
				driver->CreateRenderTarget(inputSize, 1, format, ETextureFiltration::Nearest, ETextureClamping::Clamp, usage);
			auto depth = depthSurface ? depthSurface->GetTarget() : input == DepthInput::Default ? defaultTarget : resolved;
			if (bDefaultSurface)
			{
				graph->SetSurface("DepthBuffer"_h, depthSurface);
				graph->SetRenderTarget("DepthBuffer"_h, input == DepthInput::DefaultSurfaceOnly ? RHIRenderTargetPtr{} :
					input == DepthInput::DefaultSurfaceDecoy ? defaultDepth : resolved);
			}
			auto surface = outputSurface ? driver->CreateSurface(outputSize, mipCount, EFormat::R32_SFLOAT) : RHISurfacePtr{};
			auto pyramid = surface ? surface->GetResolved() : driver->CreateRenderTarget(outputSize, mipCount, EFormat::R32_SFLOAT);
			if (depthSurface) graph->SetSurface("CustomDepth"_h, depthSurface);
			else graph->SetRenderTarget("CustomDepth"_h, resolved);
			if (surface) graph->SetSurface("Pyramid"_h, surface);
			else graph->SetRenderTarget("Pyramid"_h, pyramid);
			if (!namedInput && !bDefault) node->SetRHIResource("src"_h, depthSurface ? RHIResourcePtr(depthSurface) : depth);
			if (!namedOutput) node->SetRHIResource("dst"_h, surface ? RHIResourcePtr(surface) : pyramid);
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
		std::cout << (bDefaultSurface ? "DepthHighZ default " : "DepthHighZ ") << "format=" << uint32_t(format) << " input=" << uint32_t(input) << " namedInput=" << namedInput
			<< " namedOutput=" << namedOutput << " outputSurface=" << outputSurface << ": all samples, even/odd mips, replacement and Clear passed\n";
	}

	void TestDepthHighZIsolatedClearSample(const std::array<ShaderSetPtr, 4>& readbackShaders, std::string_view mode)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const std::array<glm::ivec2, 3> mipSizes{ glm::ivec2(4, 3), glm::ivec2(2, 1), glm::ivec2(1, 1) };
		for (bool trailingCorner : { false, true })
		{
			auto graph = TRefPtr<TestGraph>::Make();
			auto node = TRefPtr<DepthHighZNode>::Make();
			auto depth = driver->CreateRenderTarget({ 9, 7 }, 1, EFormat::D32_SFLOAT,
				ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit);
			auto pyramid = driver->CreateRenderTarget(mipSizes[0], 3, EFormat::R32_SFLOAT);
			node->SetRHIResource("src"_h, depth);
			node->SetRHIResource("dst"_h, pyramid);
			auto source = driver->CreateBuffer(9 * 7 * sizeof(float), EBufferUsageBit::BufferTransferSrc_Bit, HostMemory);
			auto pixels = static_cast<float*>(source->GetPointer());
			std::fill_n(pixels, 9 * 7, 0.75f);
			pixels[trailingCorner ? 62 : 0] = 0;
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			draw->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			commands->ImageMemoryBarrier(draw, depth, EImageLayout::TransferDstOptimal);
			VkBufferImageCopy region{};
			region.bufferOffset = source->GetOffset();
			region.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
			region.imageExtent = { 9, 7, 1 };
			const auto memory = *source->m_vulkan.m_buffer->Get();
			vkCmdCopyBufferToImage(*draw->m_vulkan.m_commandBuffer, *memory.m_buffer,
				*depth->m_vulkan.m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			auto input = ReadDepth(draw, depth, readbackShaders);
			ClearColor(draw, pyramid, glm::vec4(-8));
			node->Process(graph, upload, draw, RHISceneViewSnapshot{});
			Require(graph->HasCurrentDepthPyramid(pyramid), "isolated-clear depth reduction must publish its result");
			std::array<RHIBufferPtr, 3> readbacks;
			for (uint32_t mip = 0; mip < 3; ++mip)
			{
				auto image = pyramid->GetMipLayer(mip);
				Require(image->GetExtent() == mipSizes[mip], "the odd depth fixture must retain its expected mip extents");
				readbacks[mip] = driver->CreateBuffer(mipSizes[mip].x * mipSizes[mip].y * sizeof(float),
					EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
				std::fill_n(static_cast<float*>(readbacks[mip]->GetPointer()), mipSizes[mip].x * mipSizes[mip].y, -2.0f);
				commands->ImageMemoryBarrier(draw, image, EImageLayout::TransferSrcOptimal);
				commands->CopyImageToBuffer(draw, image, readbacks[mip]);
			}
			CompleteCommands(upload, draw);
			const auto inputPixels = static_cast<const glm::vec2*>(input->GetPointer());
			for (uint32_t i = 0; i < 63; ++i)
				Require(inputPixels[i].x == pixels[i], "GPU input must contain exactly one clear sample");
			// Nearest 9x7 -> 4x3 sampling misses both corners. Conservative reduction
			// must retain the sole zero in the same corner of every mip, down to 1x1.
			for (uint32_t mip = 0; mip < 3; ++mip)
			{
				const uint32_t count = mipSizes[mip].x * mipSizes[mip].y;
				const auto actual = static_cast<const float*>(readbacks[mip]->GetPointer());
				for (uint32_t i = 0; i < count; ++i)
					Require(actual[i] == (i == (trailingCorner ? count - 1 : 0) ? 0.0f : 0.75f),
						"every mip must preserve the isolated clear corner without changing the other depths");
			}
			std::cout << "DepthHighZ isolated clear: mode=" << mode << " corner=" << (trailingCorner ? "last" : "first")
				<< " 9x7 -> 4x3 -> 2x1 -> 1x1 passed\n";
		}
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
		if (namedDepth) node->SetRHIResource_Unresolved("depthStencil"_h, "Depth"_h);
		if (namedTarget) node->SetRHIResource_Unresolved("target"_h, "LinearDepth"_h);
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
			auto frameBinding = driver->AddBufferToShaderBindings(scene.m_frameBindings, "frameData"_h, sizeof(frameData), 0, EShaderBindingType::UniformBuffer);
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
			if (depthSurface) graph->SetSurface("Depth"_h, depthSurface);
			else graph->SetRenderTarget("Depth"_h, depth);
			if (colorSurface) graph->SetSurface("LinearDepth"_h, colorSurface);
			else graph->SetRenderTarget("LinearDepth"_h, color);
			if (!namedDepth) node->SetRHIResource("depthStencil"_h, depthSurface ? RHIResourcePtr(depthSurface) : depth);
			if (!namedTarget) node->SetRHIResource("target"_h, colorSurface ? RHIResourcePtr(colorSurface) : color);
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

	void TestClearDepth(const std::array<ShaderSetPtr, 4>& shaders, EFormat format, bool surfaceInput, bool late, bool implicitDepth = false,
		uint32_t resolvedAlias = 0)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<ClearNode>::Make();
		RHISceneViewSnapshot scene;
		const auto name = implicitDepth ? "DepthBuffer"_h : "Output"_h;
		if (late) node->SetRHIResource_Unresolved("target"_h, resolvedAlias ? "ResolvedDepth"_h : name);
		auto owner = TRefPtr<DepthPrepassNode>::Make();
		if (resolvedAlias)
		{
			graph->GetGraph().Add(node);
			graph->GetGraph().Add(owner);
		}
		for (uint32_t frame = 0; frame < 2; ++frame)
		{
			auto resolved = driver->CreateRenderTarget(glm::ivec2(Side), 1, format, ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit);
			const bool msaa = VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples() != VK_SAMPLE_COUNT_1_BIT;
			auto target = (surfaceInput || implicitDepth) && msaa ?
				driver->GetOrAddMsaaFramebufferRenderTarget(format, glm::ivec2(Side), implicitDepth && !resolvedAlias ? 0 : frame + 1).StaticCast<RHIRenderTarget>() : resolved;
			auto surface = surfaceInput ? RHISurfacePtr::Make(target, resolved, target != resolved) : RHISurfacePtr{};
			graph->SetRenderTarget(name, surface && frame == 1 && !implicitDepth ? RHIRenderTargetPtr{} : resolved);
			if (surface && resolvedAlias != 2) graph->SetSurface(name, surface);
			if (!late) node->SetRHIResource("target"_h, surface && !resolvedAlias ? RHIResourcePtr(surface) : resolved);
			if (resolvedAlias)
			{
				graph->SetRenderTarget("ResolvedDepth"_h, resolved);
				if (resolvedAlias == 2) owner->SetRHIResource("depthStencil"_h, surface);
				TVector<RHICommandListPtr> transfers, graphics;
				RHISemaphorePtr ready;
				Require(graph->Process(RHISceneViewPtr::Make(), transfers, graphics, {}, ready) &&
					transfers.IsEmpty() && graphics.IsEmpty(), "depth aliases must prepare before any view is recorded");
			}
			auto unusedDepth = resolvedAlias && msaa ? driver->GetOrAddMsaaFramebufferRenderTarget(format, glm::ivec2(Side)).StaticCast<RHIRenderTarget>() : RHIRenderTargetPtr{};
			const glm::vec2 expected(frame == 0 ? 0.375f : 0.0f, IsDepthStencilFormat(format) ? float(17 + frame) : 0.0f);
			node->SetFloat("clearDepth"_h, expected.x);
			node->SetFloat("clearStencil"_h, expected.y);
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(draw, true);
			for (auto texture : { target, resolved })
			{
				commands->ImageMemoryBarrier(draw, texture, EImageLayout::TransferDstOptimal);
				commands->ClearDepthStencil(draw, texture, 0.875f, 91);
			}
			if (unusedDepth)
			{
				commands->ImageMemoryBarrier(draw, unusedDepth, EImageLayout::TransferDstOptimal);
				commands->ClearDepthStencil(draw, unusedDepth, 0.8125f, 53);
			}
			node->Process(graph, upload, draw, scene);
			auto resolvedReadback = ReadDepth(draw, resolved, shaders);
			auto targetReadback = target != resolved ? ReadDepth(draw, target, shaders) : resolvedReadback;
			auto unusedReadback = unusedDepth ? ReadDepth(draw, unusedDepth, shaders) : RHIBufferPtr{};
			if (unusedDepth) commands->ImageMemoryBarrier(draw, unusedDepth, unusedDepth->GetDefaultLayout());
			CompleteCommands(upload, draw);
			if (unusedReadback)
			{
				const auto* values = static_cast<const glm::vec2*>(unusedReadback->GetPointer());
				for (uint32_t i = 0; i < Side * Side * uint32_t(unusedDepth->GetMsaaSamples()); ++i)
					Require(values[i] == glm::vec2(0.8125f, IsDepthStencilFormat(format) ? 53.0f : 0.0f),
						"clearing an authored depth Surface must not alter the unrelated implicit MSAA cache");
			}
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
		std::cout << (resolvedAlias ? "ClearDepthAlias " : "Clear depth ") << "format=" << uint32_t(format) << " surface=" << surfaceInput << " late=" << late << " implicit=" << implicitDepth
			<< (resolvedAlias == 2 ? " owner=bound" : resolvedAlias ? " owner=graph" : "")
			<< ": two frames and all live/resolved depth/stencil samples passed\n";
	}

	void TestClearColor(bool surfaceInput, bool late)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<ClearNode>::Make();
		RHISceneViewSnapshot scene;
		if (late) node->SetRHIResource_Unresolved("target"_h, "Output"_h);
		for (uint32_t frame = 0; frame < 2; ++frame)
		{
			auto surface = surfaceInput ? driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT) : RHISurfacePtr{};
			auto resolved = surface ? surface->GetResolved() : driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
			auto target = surface ? surface->GetTarget() : resolved;
			// A named Surface must work without a second entry for its resolved texture.
			graph->SetRenderTarget("Output"_h, surface && frame == 1 ? RHIRenderTargetPtr{} : resolved);
			if (surface) graph->SetSurface("Output"_h, surface);
			if (!late) node->SetRHIResource("target"_h, surface ? RHIResourcePtr(surface) : resolved);
			const glm::vec4 expected(0.125f * (frame + 1), 0.25f, 0.75f, 0.5f);
			node->SetVec4("clearColor"_h, expected);
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

	void TestBlit(bool sourceIsSurface, bool destinationIsSurface, bool late, bool scaled = false, bool debugView = false)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		FrameGraphNodePtr node = debugView ? FrameGraphNodePtr(TRefPtr<DebugViewNode>::Make()) : TRefPtr<BlitNode>::Make();
		if (debugView)
		{
			node->SetString("shader"_h, "Shaders/Blit.shader");
		}
		RHISceneViewSnapshot scene;
		scene.m_frameBindings = driver->CreateShaderBindings();
		if (late)
		{
			node->SetRHIResource_Unresolved("src"_h, "Source"_h);
			node->SetRHIResource_Unresolved("dst"_h, "Destination"_h);
		}
		CaptureAttachments capture;
		for (uint32_t frame = 0; frame < 2; ++frame)
		{
			const glm::ivec2 sourceExtent(scaled ? Side / 2 : Side);
			auto sourceSurface = sourceIsSurface ? driver->CreateSurface(sourceExtent, 1, EFormat::R32G32B32A32_SFLOAT) : RHISurfacePtr{};
			auto destinationSurface = destinationIsSurface ? driver->CreateSurface(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT) : RHISurfacePtr{};
			auto source = sourceSurface ? sourceSurface->GetResolved() : driver->CreateRenderTarget(sourceExtent, 1, EFormat::R32G32B32A32_SFLOAT);
			auto destination = destinationSurface ? destinationSurface->GetResolved() : driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
			graph->SetRenderTarget("Source"_h, source);
			graph->SetRenderTarget("Destination"_h, destination);
			if (sourceSurface) graph->SetSurface("Source"_h, sourceSurface);
			if (destinationSurface) graph->SetSurface("Destination"_h, destinationSurface);
			if (!late)
			{
				node->SetRHIResource("src"_h, sourceSurface ? RHIResourcePtr(sourceSurface) : source);
				node->SetRHIResource("dst"_h, destinationSurface ? RHIResourcePtr(destinationSurface) : destination);
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
								", destinationSurface=" + std::to_string(destinationIsSurface) + ", late=" + std::to_string(late) +
								", scaled=" + std::to_string(scaled) + ", debugView=" + std::to_string(debugView));
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
		std::cout << (debugView ? "DebugView" : "Blit") << " sourceSurface=" << sourceIsSurface << " destinationSurface=" << destinationIsSurface << " late=" << late <<
			" scaled=" << scaled << ": replaced inputs, both images and native descriptors passed\n";
	}

	void TestFog(bool colorIsSurface, bool late, EFormat depthFormat)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<AtmosphericFogNode>::Make();
		node->SetVec4("scattering"_h, glm::vec4(0.75f, 0, 0.95f, 0));
		if (late)
		{
			node->SetRHIResource_Unresolved("color"_h, "Color"_h);
			node->SetRHIResource_Unresolved("depthSampler"_h, "Depth"_h);
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
			graph->SetRenderTarget("Color"_h, color);
			if (surface) graph->SetSurface("Color"_h, surface);
			graph->SetRenderTarget("Depth"_h, depth);
			graph->SetSampler("g_irradianceCubemap"_h, environment);
			if (!late)
			{
				node->SetRHIResource("color"_h, surface ? RHIResourcePtr(surface) : color);
				node->SetRHIResource("depthSampler"_h, depth);
			}
			node->SetVec4("fog"_h, glm::vec4(density, 0, 0, 0));
			RHISceneViewSnapshot scene;
			scene.m_frameBindings = driver->CreateShaderBindings();
			UboFrameData frameData{};
			frameData.m_view = frameData.m_projection = frameData.m_invProjection = glm::mat4(1);
			frameData.m_viewportSize = glm::ivec2(Side);
			auto frameBinding = driver->AddBufferToShaderBindings(scene.m_frameBindings, "frameData"_h, sizeof(frameData), 0, EShaderBindingType::UniformBuffer);
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

	class LightCullingProbe : public LightCullingNode
	{
	public:
		RHIShaderBindingSetPtr GetBindings(const RHISceneViewSnapshot& scene) const
		{
			return scene.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
				this, scene.m_cameraIndex, 0u)->m_bindings;
		}
	};

	void TestLightCulling(LightCullingInput inputMode, bool surfaceInput, bool sameFlight)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<LightCullingProbe>::Make();
		graph->GetGraph().Add(node);
		if (inputMode == LightCullingInput::Named || inputMode == LightCullingInput::NamedWithoutDefault)
			node->SetRHIResource_Unresolved("linearDepth"_h, "SelectedDepth"_h);
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
		// Initial, unchanged, replaced, larger, smaller, Clear, no lights, restored lights,
		// replaced lighting set, then unchanged again.
		const std::array extents{ glm::ivec2(8), glm::ivec2(8), glm::ivec2(8), glm::ivec2(35, 19),
			glm::ivec2(11, 5), glm::ivec2(17, 33), glm::ivec2(17, 33), glm::ivec2(17, 33),
			glm::ivec2(17, 33), glm::ivec2(17, 33) };
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
				if (round == 8)
				{
					scene.m_rhiLightsData = driver->CreateShaderBindings();
				}
				scene.m_camera->SetProjectionMatrix(Math::PerspectiveRH(glm::radians(90.0f), aspect, 0.1f, 200.0f));
				if (round != 1 && round < 6)
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
				graph->SetRenderTarget("Main"_h, recording.depth);
				const auto depthName = inputMode == LightCullingInput::Default ? "LinearDepth"_h : "SelectedDepth"_h;
				graph->SetRenderTarget(depthName, surfaceInput ? RHIRenderTargetPtr{} : recording.depth);
				graph->SetSurface(depthName, recording.input.DynamicCast<RHISurface>());
				if (inputMode == LightCullingInput::Bound) node->SetRHIResource("linearDepth"_h, recording.input);
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
					graph->SetRenderTarget("LinearDepth"_h, poison);
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
				auto bindings = node->GetBindings(scene);
				Require(bindings.IsValid(), "light-culling bindings must belong to the node's flight/camera resources");
				const auto indices = *bindings->GetOrAddShaderBinding("culledLights"_h)->m_vulkan.m_valueBinding->Get();
				const auto grid = *bindings->GetOrAddShaderBinding("lightsGrid"_h)->m_vulkan.m_valueBinding->Get();
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
				Require(bindings->GetOrAddShaderBinding("linearDepth"_h)->GetTextureBinding() == recording.depth,
					"light-culling sampler must use the selected resolved input");
				Require(*scene.m_rhiLightsData->GetOrAddShaderBinding("culledLights"_h)->m_vulkan.m_valueBinding->Get() == indices &&
					*scene.m_rhiLightsData->GetOrAddShaderBinding("lightsGrid"_h)->m_vulkan.m_valueBinding->Get() == grid,
					"downstream lighting must consume exactly the culling output allocations");
				if (round == 1 || round == 2 || round >= 4)
				{
					Require(bindings == recording.bindings && indices == recording.indices && grid == recording.grid,
						"completed flight/camera must reuse sufficient tile storage, including replaced inputs and smaller extents");
					Require((scene.m_rhiLightsData->m_vulkan.m_descriptorSet == recording.lightingDescriptor) == (round != 8),
						"unchanged output allocations must not rebuild downstream lighting descriptors");
				}
				if (round == 1 || round >= 6)
				{
					Require(bindings->m_vulkan.m_descriptorSet == recording.descriptor, "unchanged culling inputs must not rebuild descriptors");
				}
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
			": ten frames, two pending views, reused descriptors and exact GPU tile lists passed\n";
	}

	void TestPostProcessFlights(const std::string& shader, bool sameFlight)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = TRefPtr<TestGraph>::Make();
		auto node = TRefPtr<PostProcessNode>::Make();
		node->SetString("shader"_h, shader);
		node->SetRHIResource_Unresolved("color"_h, "Output"_h);
		node->SetFloat("data.gain"_h, 0.5f);
		const glm::vec4 texel(0.125f, 0.25f, 0.5f, 1);
		auto texture = driver->CreateTexture(&texel, sizeof(texel), glm::ivec3(1), 1, ETextureType::Texture2D,
			EFormat::R32G32B32A32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp);
		node->SetRHIResource("sourceSampler"_h, texture);
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
				node->SetVec4("data.tint"_h, tint);
				recording.expected = tint * 0.5f + texel;
				auto target = driver->CreateRenderTarget(glm::ivec2(Side), 1, EFormat::R32G32B32A32_SFLOAT);
				graph->SetRenderTarget("Output"_h, target);
				recording.upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				recording.draw = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(recording.upload, true);
				commands->BeginCommandList(recording.draw, true);
				commands->MemoryBarrier(recording.draw, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
					static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
				node->Process(graph, recording.upload, recording.draw, recording.scene);
				Require(node->GetDrawCallStats().m_numBatches == 1, "each pending post-process recording must contain a draw");
				auto bindings = PostProcessNodeTestAccess::GetBindings(*node, recording.scene);
				const auto uniform = *bindings->GetOrAddShaderBinding("data"_h)->m_vulkan.m_valueBinding->Get();
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
		node->SetString("shader"_h, smallShader);
		node->SetString("defines"_h, "");
		node->SetRHIResource("color"_h, output);
		node->SetRHIResource("sourceSampler"_h, textureA);
		glm::vec4 tint(0.25f, 0.5f, 0.75f, 1);
		float gain = 0.5f;
		bool inverted = false;
		node->SetVec4("data.tint"_h, tint);
		node->SetFloat("data.gain"_h, gain);
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
			const auto binding = currentBindings->GetOrAddShaderBinding("data"_h);
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
		node->SetVec4("data.tint"_h, tint);
		node->SetFloat("data.gain"_h, gain);
		draw("mutated parameters", output, {}, firstTexel);
		node->SetRHIResource("sourceSampler"_h, textureB);
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
			node->SetVec4("data.tint"_h, tint);
			node->SetFloat("data.gain"_h, gain);
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
		node->SetRHIResource("sourceSampler"_h, pendingTexture);
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
		node->SetRHIResource("sourceSampler"_h, textureB);
		node->SetRHIResource("color"_h, single);
		draw("bound 1x surface", output, single, secondTexel);
		node->SetRHIResource("color"_h, multisampled);
		draw("bound 2x surface", multisampled->GetResolved(), multisampled, secondTexel);
		graph->SetSurface("LateOutput"_h, multisampled);
		graph->SetRenderTarget("LateOutput"_h, multisampled->GetResolved());
		node->SetRHIResource_Unresolved("color"_h, "LateOutput"_h);
		draw("late 2x surface", multisampled->GetResolved(), multisampled, secondTexel);
		graph->SetSurface("LateOutput"_h, single);
		graph->SetRenderTarget("LateOutput"_h, output);
		draw("late 1x surface replacement", output, single, secondTexel);
		graph->SetRenderTarget("PlainOutput"_h, output);
		node->SetRHIResource_Unresolved("color"_h, "PlainOutput"_h);
		draw("late plain target", output, {}, secondTexel);
		graph->SetSampler("LateSampler"_h, textureA);
		node->SetRHIResource_Unresolved("sourceSampler"_h, "LateSampler"_h);
		draw("late sampled texture", output, {}, firstTexel);
		graph->SetSampler("LateSampler"_h, textureB);
		draw("replaced late sampler", output, {}, secondTexel);
		node->SetRHIResource("sourceSampler"_h, textureA);
		draw("bound overrides late sampler", output, {}, firstTexel);
		node->SetRHIResource("sourceSampler"_h, multisampled);
		draw("sample resolved 2x surface", output, {}, tint * gain + secondTexel);
		graph->SetSurface("SurfaceSampler"_h, multisampled);
		node->SetRHIResource_Unresolved("sourceSampler"_h, "SurfaceSampler"_h);
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
			node->SetRHIResource("sourceSampler"_h, depth);
			draw("bound sampled depth", output, {}, glm::vec4(0.375f, 0, 0, 1));
			graph->SetRenderTarget("SceneDepth"_h, depth);
			node->SetRHIResource_Unresolved("sourceSampler"_h, "SceneDepth"_h);
			draw("late sampled depth", output, {}, glm::vec4(0.375f, 0, 0, 1));
			const auto sampler = PostProcessNodeTestAccess::GetBindings(*node, scene)->GetOrAddShaderBinding("sourceSampler"_h);
			const RHITexturePtr expectedDepth = depth->GetDepthAspect() ? depth->GetDepthAspect() : RHITexturePtr(depth);
			Require(sampler->GetTextureBinding() == expectedDepth &&
				expectedDepth->m_vulkan.m_imageView->m_subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT,
				"depth-stencil sampling must select the actual depth-only view");
		}
		node->SetRHIResource("sourceSampler"_h, textureA);
		node->SetString("shader"_h, largeShader);
		draw("large reflected block and changed shader", output, {}, firstTexel);
		const auto largeBinding = PostProcessNodeTestAccess::GetBindings(*node, scene)->GetOrAddShaderBinding("data"_h);
		Require(largeBinding->GetLayout().m_binding == 3 && largeBinding->GetLayout().m_size > 512,
			"large-block test must use the new shader with a nonzero uniform binding");
		inverted = true;
		node->SetString("defines"_h, "INVERT");
		draw("changed shader defines", output, {}, firstTexel);
		inverted = false;
		node->SetString("defines"_h, "");
		draw("restored shader defines", output, {}, firstTexel);
		node->Clear();
		draw("recreated material", output, {}, firstTexel);
	}
}

namespace Sailor::Tests
{
	void RunCloudNoiseCommandTests(const std::filesystem::path& workspace)
	{
		TestShaderSourceLifetime(workspace);
		TestGeneratedCloudNoise(workspace);
	}

	void RunSkyStarsCommandTests(const std::filesystem::path& workspace)
	{
		TestSkyWithoutStars(workspace);
		TestSkyWithoutStars(workspace, VK_ERROR_OUT_OF_HOST_MEMORY);
		TestSkyWithoutStars(workspace, VK_ERROR_OUT_OF_DEVICE_MEMORY);
	}

	void RunFrameGraphNodeCommandTests(const std::filesystem::path& workspace)
	{
		RunShaderInterfaceCommandTests();
		RunPostProcessingCommandTests();
		RunGIResolveCommandTests();
		RunEditorReadbackCommandTests();
		RunSkyStarsCommandTests(workspace);
		TestSkyWorldOwnership();
		TestSkyQueuedFrameOrder();
		TestSkyEnvironmentPublication();
		TestAuthoredEnvironmentReload(workspace);
		const auto smallShader = WriteShader(workspace, false);
		const auto largeShader = WriteShader(workspace, true);
		const auto depthReadback = WriteDepthReadbackShader(workspace);
		const auto cullingIndexReadback = WriteCullingIndexReadbackShader(workspace);
		ShaderSetPtr compactDepthShader, depthCullingShader;
		const auto compactDepthInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/DepthOnly.shader");
		const auto depthCullingInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeMeshCulling.shader");
		Require(compactDepthInfo && depthCullingInfo &&
			App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(compactDepthInfo->GetFileId(), compactDepthShader) && compactDepthShader->IsReady() &&
			App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(depthCullingInfo->GetFileId(), depthCullingShader, { "DEPTH_INSTANCE_LAYOUT", "OCCLUSION_CULLING" }) && depthCullingShader->IsReady(),
			"production compact-depth and occlusion-culling shaders must be ready before recording");
		const std::array depthPatterns{ WriteDepthPatternShader(workspace, EFormat::D32_SFLOAT), WriteDepthPatternShader(workspace, EFormat::D32_SFLOAT_S8_UINT) };
		const std::array debugShaders{ WriteDebugDrawShader(workspace, EFormat::D32_SFLOAT), WriteDebugDrawShader(workspace, EFormat::D32_SFLOAT_S8_UINT) };
		const auto highZInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeDepthHighZ.shader");
		std::array<ShaderSetPtr, 3> particleShaders;
		for (uint32_t i = 0; i < particleShaders.size(); ++i)
		{
			const auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(i == 2 ?
				"Experimental/MeshParticles/ComputeParticles.shader" : "Experimental/MeshParticles/Particle.shader");
			const TVector<std::string> defines = i == 1 ? TVector<std::string>{ "SHADOW_CASTER" } : TVector<std::string>{};
			Require(info && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(info->GetFileId(), particleShaders[i], defines) && particleShaders[i]->IsReady(),
				"production particle compute, color and shadow shaders must compile");
		}
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
		std::array<ShaderSetPtr, 4> shadowBlurShaders;
		for (uint32_t i = 0; i < shadowBlurShaders.size(); ++i)
		{
			const auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(i < 2 ? "Tests/Shaders/ShadowDrawCompletion.shader" : "Shaders/Blur.shader");
			TVector<std::string> defines;
			if (i % 2 == 0) defines.Add("HORIZONTAL");
			else if (i >= 2) defines.Add("VERTICAL");
			if (i >= 2) defines.Add("EVSM");
			Require(info && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(info->GetFileId(), shadowBlurShaders[i], defines) && shadowBlurShaders[i]->IsReady(),
				"shadow publication shaders must compile before recording");
		}
		const std::array<size_t, 4> storageSizes{ 16, 100, 140, 292 };
		std::array<ShaderSetPtr, 4> storageShaders;
		for (size_t i = 0; i < storageSizes.size(); ++i)
			storageShaders[i] = WriteIndexedStorageShader(workspace, (storageSizes[i] + 15) / 16 * 16);
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
		auto task = Tasks::CreateTaskWithResult<std::string>("Post-process attachment and parameter contracts"_h, [&]() -> std::string
			{
				try
				{
					TestCubemapMipViews();
					TestPipelineSampleVariants(mrtShader);
					TestParticleHistory(particleShaders[2]);
					std::string particleGraphFailures;
					for (uint32_t kind : { 1u, 2u, 0u })
					for (bool named : { false, true })
					for (bool clearResolved : { false, true })
					{
						try { TestParticleGraph(particleShaders, kind, named, clearResolved); }
						catch (const std::exception& error) { particleGraphFailures += std::string(error.what()) + "\n"; }
					}
					if (!particleGraphFailures.empty()) throw std::runtime_error(particleGraphFailures);
					for (bool colorSurface : { true, false })
					for (bool namedColor : { false, true })
					for (uint32_t depthKind : { 0u, 1u, 2u })
					for (bool namedDepth : { false, true })
					{
						if (depthKind == 2 && namedDepth) continue;
						TestParticleAttachments(particleShaders, depthReadback, colorSurface, namedColor, depthKind, namedDepth);
					}
					std::string defaultDepthFailures;
					for (auto publication : { DefaultDepthPublication::Target, DefaultDepthPublication::Surface,
						DefaultDepthPublication::SurfaceOnly, DefaultDepthPublication::SurfaceWithDecoy })
					for (bool named : { true, false })
					for (bool prepass : { true, false })
					for (uint32_t colorKind = 0; colorKind < (prepass ? 1u : 3u); ++colorKind)
					{
						try { TestDefaultDepthAttachment(prepass ? compactDepthShader : mrtShader, depthReadback, prepass, colorKind, publication, named); }
						catch (const std::exception& error)
						{
							defaultDepthFailures += "Default depth publication=" + std::string(magic_enum::enum_name(publication)) +
								" named=" + std::to_string(named) + " prepass=" + std::to_string(prepass) +
								" colorKind=" + std::to_string(colorKind) + ": " + error.what() + "\n";
						}
					}
					for (uint32_t depthKind : { 1u, 3u, 4u, 5u })
					for (bool singleSample : { false, true })
					for (bool namedColor : { false, true })
					{
						if (depthKind == 1 && !singleSample) continue;
						try { TestParticleAttachments(particleShaders, depthReadback, true, namedColor, depthKind, false, singleSample); }
						catch (const std::exception& error)
						{
							defaultDepthFailures += "Particles default depth depthKind=" + std::to_string(depthKind) +
								" singleSample=" + std::to_string(singleSample) + " namedColor=" + std::to_string(namedColor) + ": " + error.what() + "\n";
						}
					}
					if (!defaultDepthFailures.empty()) throw std::runtime_error(defaultDepthFailures);
					std::string cullingFailures;
					for (bool named : { false, true })
					for (bool surface : { false, true })
					for (bool paged : { false, true })
					{
						try { TestDepthPrepassCulling(compactDepthShader, cullingIndexReadback, depthReadback, named, surface, paged); }
						catch (const std::exception& error)
						{
							cullingFailures += "Depth prepass named=" + std::to_string(named) + " surface=" + std::to_string(surface) +
								" paged=" + std::to_string(paged) + ": " + error.what() + "\n";
						}
					}
					if (!cullingFailures.empty()) throw std::runtime_error(cullingFailures);
					for (bool named : { false, true })
					for (bool paged : { false, true })
						TestDepthPrepassOcclusion(compactDepthShader, cullingIndexReadback, depthReadback, named, paged);
					TestSamplerReductionCache();
					FrameGraphNodeTestAccess::WithoutSamplerMinmax(*VulkanApi::GetInstance()->GetMainDevice(), [&]()
						{
							TestSamplerReductionCache();
							TestDepthHighZ(depthPatterns[0], depthReadback, EFormat::D32_SFLOAT, DepthInput::Default, true, true, false);
							TestDepthHighZIsolatedClearSample(depthReadback, "minmax-disabled");
							std::cout << "DepthHighZ: complete odd/even/MSAA mip readbacks with minmax=false passed\n";
						});
					for (size_t i = 0; i < storageShaders.size(); ++i) TestIndexedStorageBindings(storageShaders[i], storageSizes[i], false);
					// The production pool skips a block after its first small allocation.
					// Also exercise nonzero buffer offsets using the same allocator with a smaller average element size.
					auto* driver = Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
					auto& storageAllocator = driver->GetGeneralSsboAllocator();
					auto savedAllocator = storageAllocator;
					storageAllocator = TSharedPtr<VulkanBufferAllocator>::Make(65536, 256, 65536);
					storageAllocator->GetGlobalAllocator().SetUsage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
					storageAllocator->GetGlobalAllocator().SetMemoryProperties(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
					try
					{
						for (size_t i = 0; i < storageShaders.size(); ++i) TestIndexedStorageBindings(storageShaders[i], storageSizes[i], true);
					}
					catch (...)
					{
						storageAllocator = std::move(savedAllocator);
						throw;
					}
					storageAllocator = std::move(savedAllocator);
					TestDescriptorPoolLifetime(storageShaders[0]);
					TestPagedArenaUploadFlights<RenderSceneNode::PerInstanceData>("main");
					TestPagedArenaUploadFlights<DepthPrepassNode::PerInstanceData>("depth");
					TestPagedArenaUploadFlights<ShadowPrepassNode::PerInstanceData>("shadow");
					TestConcurrentDescriptorPublication(storageShaders[0]);
					TestImGuiSkippedAttachments();
					TestLinearizeDepthRegions(linearDepthShader);
					TestFullscreenUploadRetry();
					const auto repairedGraphs = TestGraphLoadFailures(workspace, importedGraphIds[0]);
					TestImportedRendering(importedGraphIds, repairedGraphs);
					TestRelativeAttachmentDimensions(workspace);
					for (bool paged : { false, true })
						for (bool instanced : { false, true })
							for (bool skinned : { false, true })
								for (auto mobility : { EMobilityType::Static, EMobilityType::Stationary, EMobilityType::Dynamic })
									TestCustomDepthSilhouette(customDepthShaders[skinned ? 1 : 0], depthReadback, paged, instanced, skinned, mobility);
					for (auto binding : { DepthAttachmentBinding::NamedTarget, DepthAttachmentBinding::Surface, DepthAttachmentBinding::NamedSurface })
						for (bool paged : { false, true })
							for (bool skinned : { false, true })
								TestCustomDepthSilhouette(customDepthShaders[skinned ? 1 : 0], depthReadback, paged, true, skinned, EMobilityType::Stationary, binding);
					TestShadowVerticalPublication(shadowBlurShaders);
					TestShadowAttachmentRefusal(shadowBlurShaders);
					TestShadowBlitInitialization();
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
					TestSceneRenderExtent();
					TestMotionBlurNode();
					TestSceneMrt(mrtShader, true, false, false, true, true);
					for (bool late : { false, true })
						for (bool colorSurface : { true, false })
							for (bool motionSurface : { true, false }) TestSceneMrt(mrtShader, colorSurface, motionSurface, late, false, true);
					for (auto format : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT })
						for (bool namedColor : { false, true })
							for (bool namedDepth : { false, true })
								for (bool colorSurface : { false, true })
									for (auto input : { DebugDepthInput::Texture, DebugDepthInput::Surface })
										TestDepthTestedDraw(debugShaders[format == EFormat::D32_SFLOAT ? 0 : 1], depthPatterns[format == EFormat::D32_SFLOAT ? 0 : 1],
											depthReadback, format, colorSurface, input, namedColor, namedDepth, DepthDrawPath::ImGuiNode);
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
					std::string depthConsumerFailures;
					for (uint32_t colorKind : { 0u, 2u })
					for (bool namedColor : { false, true })
					for (bool namedDepth : { false, true })
					for (auto input : { DebugDepthInput::Surface, DebugDepthInput::DefaultSurface })
					for (auto format : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT })
					{
						try { TestDepthTestedDraw(debugShaders[format == EFormat::D32_SFLOAT ? 0 : 1], depthPatterns[format == EFormat::D32_SFLOAT ? 0 : 1],
							depthReadback, format, colorKind, input, namedColor, namedDepth, DepthDrawPath::DebugNode, true); }
						catch (const std::exception& error) { depthConsumerFailures += std::string(error.what()) + "\n"; }
					}
					for (auto input : { DepthInput::DefaultSurface, DepthInput::DefaultSurfaceOnly,
						DepthInput::DefaultSurfaceDecoy, DepthInput::DefaultSingleSampleSurface })
					for (bool namedInput : { false, true })
					for (bool outputSurface : { false, true })
					for (auto format : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT })
					{
						try { TestDepthHighZ(depthPatterns[format == EFormat::D32_SFLOAT ? 0 : 1], depthReadback,
							format, input, namedInput, true, outputSurface); }
						catch (const std::exception& error) { depthConsumerFailures += std::string(error.what()) + "\n"; }
					}
					if (!depthConsumerFailures.empty()) throw std::runtime_error(depthConsumerFailures);
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
					TestDepthHighZIsolatedClearSample(depthReadback, "native");
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
						{
							for (bool destinationSurface : { false, true })
							{
								TestBlit(sourceSurface, destinationSurface, late);
								TestBlit(sourceSurface, destinationSurface, late, false, true);
							}
						}
						TestBlit(false, false, late, true);
						TestBlit(true, true, late, true);
						for (bool colorSurface : { false, true })
							for (auto depthFormat : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT }) TestFog(colorSurface, late, depthFormat);
					}
					std::string depthAliasFailures;
					for (bool late : { false, true })
						for (bool defaultName : { false, true })
							for (uint32_t alias : { 1u, 2u })
								for (auto format : { EFormat::D32_SFLOAT, EFormat::D32_SFLOAT_S8_UINT })
								{
									try { TestClearDepth(depthReadback, format, true, late, defaultName, alias); }
									catch (const std::exception& error) { depthAliasFailures += std::string(error.what()) + "\n"; }
								}
					if (!depthAliasFailures.empty()) throw std::runtime_error(depthAliasFailures);
					std::string mixedSampleFailures;
					for (bool late : { false, true })
						for (bool singleSample : { false, true })
							for (bool clear : { false, true })
							{
								try { TestSceneMrt(mrtShader, true, true, late, singleSample, clear, false, true); }
								catch (const std::exception& error) { mixedSampleFailures += std::string(error.what()) + "\n"; }
							}
					if (!mixedSampleFailures.empty()) throw std::runtime_error(mixedSampleFailures);
					TestSceneMrt(mrtShader, true, false, false, true);
					for (bool late : { false, true })
						for (bool singleSample : { false, true }) TestSceneMrt(mrtShader, true, false, late, singleSample, false, true);
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
