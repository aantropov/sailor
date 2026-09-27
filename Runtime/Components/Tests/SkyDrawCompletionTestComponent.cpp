#include "Components/Tests/SkyDrawCompletionTestComponent.h"
#include "FrameGraph/SkyNode.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Cubemap.h"
#include "RHI/Fence.h"
#include "RHI/Material.h"
#include "RHI/Mesh.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include "RHI/VertexDescription.h"
#include <atomic>
#include <cstring>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	constexpr uint32_t Side = 32u;
	class SkyProbe final : public Framegraph::SkyNode
	{
	public:
		using SkyNode::m_environmentCaptureStep;
		using SkyNode::m_pEnvironmentBindings;
		using SkyNode::m_pEnvironmentCapture;
		using SkyNode::m_pSkyEnvMaterial;
	};

	class CaptureGraph final : public RHIFrameGraph
	{
	public:
		CaptureGraph()
		{
			auto& driver = Renderer::GetDriver();
			VertexP3N3UV2C4 vertices[4]{};
			for (uint32_t i = 0u; i < 4u; ++i)
			{
				vertices[i].m_texcoord = glm::vec2(i % 2u, i / 2u);
				vertices[i].m_position = glm::vec3(vertices[i].m_texcoord * 2.0f - 1.0f, 0.0f);
			}
			const uint32_t indices[] = { 0u, 1u, 2u, 2u, 1u, 3u };
			const auto memory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
			m_postEffectPlane = RHIMeshPtr::Make();
			m_postEffectPlane->m_vertexDescription = driver->GetOrAddVertexDescription<VertexP3N3UV2C4>();
			m_postEffectPlane->m_vertexBuffer = driver->CreateBuffer(sizeof(vertices), EBufferUsageBit::VertexBuffer_Bit, memory);
			m_postEffectPlane->m_indexBuffer = driver->CreateBuffer(sizeof(indices), EBufferUsageBit::IndexBuffer_Bit, memory);
			std::memcpy(m_postEffectPlane->m_vertexBuffer->GetPointer(), vertices, sizeof(vertices));
			std::memcpy(m_postEffectPlane->m_indexBuffer->GetPointer(), indices, sizeof(indices));
		}
	};

	struct RestoreBuffer
	{
		RHIShaderBindingPtr m_binding;
		decltype(m_binding->m_vulkan.m_valueBinding) m_value;
		void Restore()
		{
			if (m_binding)
			{
				m_binding->m_vulkan.m_valueBinding = m_value;
				m_binding.Clear();
			}
		}
		~RestoreBuffer() { Restore(); }
	};
}

struct SkyDrawCompletionTestComponent::CaptureState
{
	enum class Stage { WarmA, FirstFaceB, RejectFaceB, RetryFaceB, FinishB };
	std::atomic_bool m_bCancelled = false;
	TRefPtr<CaptureGraph> m_graph;
	TRefPtr<SkyProbe> m_sky;
	RHISceneViewSnapshot m_scene;
	SkyParameters m_a, m_b;
	RHITexturePtr m_publishedA;
	RHICubemapPtr m_candidateB;
	RHIShaderBindingSetPtr m_request;
	uint64_t m_requestRevision = 0u;
	size_t m_requestHash = 0u;
	DrawCallStats m_firstFaceStats{};
	Stage m_stage = Stage::WarmA;

	std::string Initialize(RHICommandListPtr upload, RHICommandListPtr graphics)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		m_graph = TRefPtr<CaptureGraph>::Make();
		m_sky = TRefPtr<SkyProbe>::Make();
		m_graph->GetGraph().Add(m_sky);
		m_a.m_sunIlluminance = glm::vec4(30000.0f, 30000.0f, 30000.0f, 0.0f);
		m_a.m_cloudsDensity = m_a.m_cloudsCoverage = m_a.m_sunShaftsIntensity = 0.0f;
		m_b = m_a;
		m_b.m_sunIlluminance = glm::vec4(75000.0f, 75000.0f, 75000.0f, 0.0f);
		m_sky->SetSkyParams(m_a);
		auto color = driver->CreateRenderTarget(graphics, glm::ivec2(Side), 1u, EFormat::R16G16B16A16_SFLOAT);
		auto depth = driver->CreateRenderTarget(graphics, glm::ivec2(Side), 1u, driver->GetDepthBuffer()->GetFormat(),
			ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit);
		m_graph->SetRenderTarget("DepthBuffer", depth);
		m_graph->SetSampler("g_noiseSampler", driver->GetDefaultTexture());
		m_graph->SetSampler("g_ditherPatternSampler", driver->GetDefaultTexture());
		m_sky->SetRHIResource("color", color);
		m_sky->SetRHIResource("linearDepth", driver->GetDefaultTexture());
		m_scene.m_frameBindings = driver->CreateShaderBindings();
		UboFrameData frame{};
		frame.m_view = glm::mat4(1.0f);
		frame.m_projection = Math::PerspectiveRH(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
		frame.m_invProjection = glm::inverse(frame.m_projection);
		frame.m_viewportSize = glm::ivec2(Side);
		frame.m_cameraZNearZFar = glm::vec2(0.1f, 1000.0f);
		for (uint32_t binding = 0u; binding < 2u; ++binding)
		{
			auto buffer = driver->AddBufferToShaderBindings(m_scene.m_frameBindings,
				binding ? "previousFrameData" : "frameData", sizeof(frame), binding, EShaderBindingType::UniformBuffer);
			if (!buffer) { return "Sky frame uniform creation failed"; }
			commands->UpdateShaderBinding(upload, buffer, &frame, sizeof(frame));
		}
		commands->ImageMemoryBarrier(graphics, color, EImageLayout::ColorAttachmentOptimal);
		commands->ImageMemoryBarrier(graphics, depth, IsDepthStencilFormat(depth->GetFormat()) ?
			EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal);
		commands->BeginRenderPass(graphics, TVector<RHITexturePtr>{color}, depth,
			glm::ivec4(0, 0, Side, Side), glm::ivec2(0), true, glm::vec4(0), 0.0f, false, true);
		commands->EndRenderPass(graphics);
		return {};
	}

	StepResult Step()
	{
		if (m_bCancelled) { return {}; }
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		auto graphics = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(upload, true);
		commands->BeginCommandList(graphics, true);
		std::string error = m_graph ? std::string{} : Initialize(upload, graphics);
		RestoreBuffer restore;
		if (error.empty() && m_stage == Stage::RejectFaceB)
		{
			if (!m_request->GetShaderBindings().TryGet("data", restore.m_binding))
			{
				error = "Frozen environment has no captured data binding";
			}
			else
			{
				restore.m_value = restore.m_binding->m_vulkan.m_valueBinding;
				auto range = restore.m_value->Get();
				range.m_ptr.m_buffer = VulkanBufferPtr::Make(VulkanApi::GetInstance()->GetMainDevice(),
					sizeof(SkyParameters), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE);
				restore.m_binding->m_vulkan.m_valueBinding = decltype(restore.m_value)::Make(range,
					TWeakPtr<RHIShaderBinding::VulkanBufferAllocator>{});
			}
		}
		const uint32_t previousStep = m_sky->m_environmentCaptureStep;
		if (error.empty())
		{
			for (auto cmd : { upload, graphics })
			{
				cmd->m_vulkan.m_commandBuffer->AddDependency(m_graph);
				cmd->m_vulkan.m_commandBuffer->AddDependency(m_scene.m_frameBindings);
			}
			m_sky->Process(m_graph, upload, graphics, m_scene);
		}
		restore.Restore();
		commands->EndCommandList(upload);
		commands->EndCommandList(graphics);
		if (m_stage == Stage::RejectFaceB && m_sky->m_environmentCaptureStep != previousStep)
		{
			error = "Rejected environment face advanced its capture step";
		}
		if (!error.empty())
		{
			upload->m_vulkan.m_commandBuffer->Reset();
			graphics->m_vulkan.m_commandBuffer->Reset();
			return { false, error };
		}
		auto ready = driver->CreateWaitSemaphore();
		auto uploadFence = RHIFencePtr::Make();
		auto graphicsFence = RHIFencePtr::Make();
		if (!driver->SubmitCommandList(upload, uploadFence, ready) ||
			!driver->SubmitCommandList(graphics, graphicsFence, nullptr, ready))
		{
			return { false, "Sky validation submission failed" };
		}
		graphicsFence->Wait(5000000000ull);
		uploadFence->Wait(5000000000ull);
		if (!graphicsFence->IsFinished() || !uploadFence->IsFinished())
		{
			return { false, "Sky validation submission exceeded five seconds per fence" };
		}
		upload->m_vulkan.m_commandBuffer->Reset();
		graphics->m_vulkan.m_commandBuffer->Reset();

		SkyParameters published;
		const bool bReady = m_sky->GetEnvironmentSkyParams(published);
		auto texture = m_graph->GetSampler("g_skyCubemap");
		const uint32_t step = m_sky->m_environmentCaptureStep;
		if (m_stage == Stage::WarmA)
		{
			if (!bReady) { return {}; }
			if (!texture || !(published == m_a) || step != 7u)
			{
				return { false, "Initial Sky A publication is incomplete" };
			}
			m_publishedA = texture;
			m_sky->SetSkyParams(m_b);
			m_stage = Stage::FirstFaceB;
			return {};
		}
		if (m_stage != Stage::FirstFaceB &&
			(m_request != m_sky->m_pEnvironmentBindings || m_requestRevision != m_request->GetDescriptorRevision() ||
				m_requestHash != m_request->GetCompatibilityHashCode() || m_candidateB != m_sky->m_pEnvironmentCapture))
		{
			return { false, "Failure/retry changed the frozen B request or its candidate" };
		}
		if (m_stage == Stage::FinishB && previousStep == 6u)
		{
			const bool bComplete = bReady && step == 7u && published == m_b &&
				texture == m_candidateB && texture != m_publishedA;
			return { bComplete, bComplete ? std::string{} : "Sky B did not publish coherently after its mip step" };
		}
		if (!bReady || !(published == m_a) || texture != m_publishedA)
		{
			return { false, "Pending or rejected B replaced the published A cube/parameters" };
		}
		if (m_stage == Stage::FirstFaceB)
		{
			if (step != 1u || m_sky->m_pEnvironmentCapture == m_publishedA)
			{
				return { false, "B did not record its first face into a new candidate" };
			}
			m_candidateB = m_sky->m_pEnvironmentCapture;
			m_request = m_sky->m_pEnvironmentBindings;
			m_firstFaceStats = m_sky->GetDrawCallStats();
			if (!driver->AddBufferToShaderBindings(m_request, "unused", sizeof(glm::vec4), 31u, EShaderBindingType::UniformBuffer) ||
				VulkanApi::IsCompatible(m_sky->m_pSkyEnvMaterial->m_vulkan.m_pipelines[0]->m_layout, m_request->m_vulkan.m_descriptorSet, 1u))
			{
				return { false, "Private captured set did not require a fresh descriptor projection" };
			}
			m_requestRevision = m_request->GetDescriptorRevision();
			m_requestHash = m_request->GetCompatibilityHashCode();
			m_stage = Stage::RejectFaceB;
			return {};
		}
		const auto stats = m_sky->GetDrawCallStats();
		if (m_stage == Stage::RejectFaceB)
		{
			if (step != 1u || stats.m_numBatches + 1u != m_firstFaceStats.m_numBatches ||
				stats.m_numInstances + 1u != m_firstFaceStats.m_numInstances)
			{
				return { false, "Rejected face was counted as a draw or suppressed another Sky draw" };
			}
			m_stage = Stage::RetryFaceB;
		}
		else
		{
			if (step != previousStep + 1u || stats.m_numBatches != m_firstFaceStats.m_numBatches ||
				stats.m_numInstances != m_firstFaceStats.m_numInstances)
			{
				return { false, "Restored B did not record exactly its next face" };
			}
			m_stage = Stage::FinishB;
		}
		return {};
	}
};

SkyDrawCompletionTestComponent::~SkyDrawCompletionTestComponent() = default;

void SkyDrawCompletionTestComponent::Tick(float)
{
	if (IsFinished()) { return; }
	if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 60000)
	{
		if (m_capture) { m_capture->m_bCancelled = true; }
		MarkFailed("Sky draw completion did not finish within 60 seconds");
		return;
	}
	if (m_step)
	{
		if (!m_step->IsFinished()) { return; }
		const auto result = m_step->GetResult();
		m_step.Clear();
		if (!result.m_error.empty()) { MarkFailed(result.m_error); return; }
		if (result.m_bFinished)
		{
			AddJournalEvent("SkyDrawCompletionEvidence", "Real Sky Process retained published A through rejected B face1; "
				"restoring the same captured binding request retried face1, then completed all six faces and mip publication of B. "
				"Native submissions and exact draw counts passed; cubemap pixels were not read back.");
			MarkPassed();
			return;
		}
	}
	if (!m_capture) { m_capture = TSharedPtr<CaptureState>::Make(); }
	m_step = Tasks::CreateTaskWithResult<StepResult>("Sky draw completion step",
		[capture = m_capture]() { return capture->Step(); }, EThreadType::Render);
	m_step->Run();
}

void SkyDrawCompletionTestComponent::EndPlay()
{
	if (m_capture) { m_capture->m_bCancelled = true; }
	m_step.Clear();
	m_capture.Clear();
	TestCaseComponent::EndPlay();
}
