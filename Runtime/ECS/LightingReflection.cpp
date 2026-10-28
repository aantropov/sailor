#include "ECS/LightingECS.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "FrameGraph/BlitNode.h"
#include "FrameGraph/ClearNode.h"
#include "FrameGraph/DepthPrepassNode.h"
#include "FrameGraph/EnvironmentNode.h"
#include "FrameGraph/GlobalIlluminationResolveNode.h"
#include "FrameGraph/LightCullingNode.h"
#include "FrameGraph/LinearizeDepthNode.h"
#include "FrameGraph/PostProcessNode.h"
#include "FrameGraph/RenderSceneNode.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "RHI/CommandList.h"
#include "RHI/RenderTarget.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"

#include <algorithm>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

namespace
{
	std::atomic_uint64_t g_nextReflectionGeneration{ 1u };

	class CaptureDrawNode final : public RenderSceneNode
	{
	public:
		bool IsPrepared(const RHISceneViewSnapshot& snapshot) const
		{
			return snapshot.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
				this, snapshot.m_cameraIndex, 0u)->m_bPacketComplete;
		}
		bool WasRecorded(const RHISceneViewSnapshot& snapshot) const
		{
			const auto resources = snapshot.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
				this, snapshot.m_cameraIndex, 0u);
			return GetDrawCallStats().m_numInstances == resources->m_packet.GetNumDrawInstances();
		}
	};

	class CaptureDepthNode final : public DepthPrepassNode
	{
	public:
		bool IsPrepared(const RHISceneViewSnapshot& snapshot) const
		{
			auto resources = snapshot.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
				this, snapshot.m_cameraIndex, 0u);
			return resources->m_bPacketComplete && resources->m_bAreMaterialsReady;
		}
		bool WasRecorded(const RHISceneViewSnapshot& snapshot) const
		{
			const auto resources = snapshot.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
				this, snapshot.m_cameraIndex, 0u);
			return GetDrawCallStats().m_numInstances == resources->m_packet.GetNumDrawInstances() +
				resources->m_customPacket.GetNumDrawInstances();
		}
	};

	class CaptureShadowNode final : public ShadowPrepassNode
	{
	public:
		bool IsPrepared(const RHISceneViewSnapshot& snapshot) const
		{
			auto resources = snapshot.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
				this, snapshot.m_cameraIndex, 0u);
			if (resources->m_shadowPayloadComplete.Num() != snapshot.m_shadowMapsToUpdate.Num())
			{
				return false;
			}
			for (const auto& pass : resources->m_shadowPayloadComplete)
			{
				for (bool complete : pass)
				{
					if (!complete)
					{
						return false;
					}
				}
			}
			return true;
		}
	};

	class CaptureLightCullingNode final : public LightCullingNode
	{
	public:
		bool WasRecorded(const RHISceneViewSnapshot& snapshot) const
		{
			return snapshot.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(
				this, snapshot.m_cameraIndex, 0u)->m_bComplete;
		}
	};

	class CaptureRestoreNode final : public BlitNode
	{
	public:
		bool WasRecorded(const RHIFrameGraphPtr& graph) const
		{
			const auto surface = GetRHIResource("dst"_h, graph.GetRawPtr()).DynamicCast<RHISurface>();
			return surface && (!surface->NeedsResolve() || GetDrawCallStats().m_numInstances == 1u);
		}
	};

	class CaptureTransmissionNode final : public BaseFrameGraphNode
	{
	public:
		void Clear() override {}
		void Process(RHIFrameGraphPtr graph, RHICommandListPtr, RHICommandListPtr command,
			const RHISceneViewSnapshot&) override
		{
			m_bComplete = false;
			auto commands = Renderer::GetDriverCommands();
			auto source = GetResolvedAttachment("src"_h, graph.GetRawPtr());
			auto target = GetResolvedAttachment("dst"_h, graph.GetRawPtr());
			if (!source || !target)
			{
				return;
			}
			commands->ImageMemoryBarrier(command, source, EImageLayout::TransferSrcOptimal);
			commands->ImageMemoryBarrier(command, target, EImageLayout::TransferDstOptimal);
			const glm::ivec4 area(0, 0, target->GetExtent().x, target->GetExtent().y);
			if (!commands->BlitImage(command, source, target, area, area))
			{
				return;
			}
			const uint32_t before = command->GetNumRecordedCommands();
			commands->GenerateMipMaps(command, target);
			m_bComplete = command->GetNumRecordedCommands() > before;
		}
		bool m_bComplete = false;
	};

	bool WarmCaptureShaders()
	{
		auto compiler = App::GetSubmodule<ShaderCompiler>();
		auto registry = App::GetSubmodule<AssetRegistry>();
		const auto load = [&](std::string_view path, const TVector<std::string>& defines = {})
		{
			auto info = registry->GetAssetInfoPtr(path);
			ShaderSetPtr shader;
			return info && compiler->LoadShader_Immediate(info->GetFileId(), shader, defines) &&
				shader && shader->IsReady();
		};
		for (const auto path : { "Shaders/LinearizeDepth.shader", "Shaders/ComputeLightCulling.shader",
			"Shaders/Blit.shader", "Shaders/GlobalIlluminationResolve.shader",
			"Shaders/ReflectionCaptureSky.shader", "Shaders/ReflectionCaptureFace.shader" })
		{
			if (!load(path))
			{
				return false;
			}
		}
		for (uint32_t variant = 0u; variant < 8u; ++variant)
		{
			TVector<std::string> defines;
			if (variant & 1u)
			{
				defines.Add("SKINNING");
			}
			if (variant & 2u)
			{
				defines.Add("MASKED");
			}
			if (variant < 4u && !load("Shaders/DepthOnly.shader", defines))
			{
				return false;
			}
			if (variant & 4u)
			{
				defines.Add("EVSM");
			}
			if (!load("Shaders/ShadowCaster.shader", defines))
			{
				return false;
			}
		}
		return load("Shaders/Blur.shader", { "VERTICAL", "EVSM" }) &&
			load("Shaders/Blur.shader", { "HORIZONTAL", "EVSM" });
	}

	bool IsPrepared(const LocalReflectionCaptureJob& job)
	{
		auto graph = job.m_graph;
		for (const auto& face : job.m_faces)
		{
			const auto& snapshot = face->m_snapshots[0];
			for (const auto& node : graph->GetGraph())
			{
				if (auto depth = node.DynamicCast<CaptureDepthNode>(); depth && !depth->IsPrepared(snapshot))
				{
					return false;
				}
				if (auto draw = node.DynamicCast<CaptureDrawNode>(); draw && !draw->IsPrepared(snapshot))
				{
					return false;
				}
				if (auto shadow = node.DynamicCast<CaptureShadowNode>(); shadow && !shadow->IsPrepared(snapshot))
				{
					return false;
				}
				if (auto post = node.DynamicCast<PostProcessNode>(); post && !post->IsShaderReady())
				{
					return false;
				}
			}
		}
		return true;
	}

	bool InitializeGraph(LocalReflectionCaptureJob& job, const RHIFrameGraphPtr& source)
	{
		const auto rawEnvironment = source->GetSampler("g_rawEnvCubemap"_h);
		const StringHash globalSamplers[] = {
			"g_envCubemap"_h, "g_irradianceCubemap"_h, "g_sheenEnvCubemap"_h, "g_brdfSampler"_h
		};
		if (!rawEnvironment || !rawEnvironment->IsReady())
		{
			job.m_bRetry = true;
			return false;
		}
		for (const auto name : globalSamplers)
		{
			auto sampler = source->GetSampler(name);
			if (!sampler || !sampler->IsReady())
			{
				job.m_bRetry = true;
				return false;
			}
		}

		auto& driver = Renderer::GetDriver();
		const glm::ivec2 extent(job.m_request.m_faceSize);
		uint32_t mipLevels = 1u;
		for (uint32_t size = job.m_request.m_faceSize; size > 1u; size >>= 1u)
		{
			++mipLevels;
		}
		job.m_cubemap = driver->CreateCubemap(extent, mipLevels, EFormat::R16G16B16A16_SFLOAT);
		auto color = driver->CreateSurface(extent, 1u, EFormat::R16G16B16A16_SFLOAT);
		auto depth = driver->CreateSurface(extent, 1u, EFormat::D32_SFLOAT_S8_UINT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit |
			ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::TextureTransferSrc_Bit);
		auto motion = driver->CreateSurface(extent, 1u, EFormat::R16G16B16A16_SFLOAT);
		auto linearDepth = driver->CreateRenderTarget(extent, 1u, EFormat::R32_SFLOAT);
		auto transmission = driver->CreateRenderTarget(extent, mipLevels, EFormat::R16G16B16A16_SFLOAT);
		auto probeCells = driver->CreateRenderTarget(extent / 2, 1u, EFormat::R32G32B32A32_SFLOAT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::Sampled_Bit | ETextureUsageBit::Storage_Bit | ETextureUsageBit::TextureTransferDst_Bit);
		if (!job.m_cubemap || !color || !depth || !motion || !linearDepth || !transmission || !probeCells)
		{
			return false;
		}
		driver->SetDebugName(job.m_cubemap, "Local reflection capture"_h);
		job.m_graph = RHIFrameGraphPtr::Make();
		auto graph = job.m_graph;
		graph->SetSurface("Main"_h, color);
		graph->SetSurface("CaptureDepth"_h, depth);
		graph->SetSurface("CaptureMotion"_h, motion);
		graph->SetRenderTarget("LinearDepth"_h, linearDepth);
		for (const auto name : globalSamplers)
		{
			graph->SetSampler(name, source->GetSampler(name));
		}
		// The private graph never receives local samplers or local box parameters.
		const auto clear = [&](RHIResourcePtr target)
		{
			auto node = TRefPtr<ClearNode>::Make();
			node->SetRHIResource("target"_h, target);
			node->SetVec4("clearColor"_h, glm::vec4(0));
			node->SetFloat("clearDepth"_h, 0);
			node->SetFloat("clearStencil"_h, 0);
			graph->GetGraph().Add(node);
		};
		clear(depth);
		clear(motion);
		for (const auto queue : { "Opaque", "Masked" })
		{
			auto node = TRefPtr<CaptureDepthNode>::Make();
			node->SetString("Tag"_h, queue);
			node->SetString("GPUCulling"_h, "false");
			node->SetString("VirtualizeInstancePayloads"_h, "false");
			node->SetRHIResource("depthStencil"_h, depth);
			graph->GetGraph().Add(node);
		}
		auto linearize = TRefPtr<LinearizeDepthNode>::Make();
		linearize->SetRHIResource("depthStencil"_h, depth);
		linearize->SetRHIResource("target"_h, linearDepth);
		graph->GetGraph().Add(linearize);
		auto lightCulling = TRefPtr<CaptureLightCullingNode>::Make();
		lightCulling->SetRHIResource("linearDepth"_h, linearDepth);
		graph->GetGraph().Add(lightCulling);
		auto shadows = TRefPtr<CaptureShadowNode>::Make();
		shadows->SetString("VirtualizeInstancePayloads"_h, "false");
		graph->GetGraph().Add(shadows);
		auto sky = TRefPtr<PostProcessNode>::Make();
		sky->SetString("shader"_h, "Shaders/ReflectionCaptureSky.shader");
		sky->SetRHIResource("environmentSampler"_h, rawEnvironment);
		sky->SetRHIResource("color"_h, color);
		sky->PreloadShader();
		graph->GetGraph().Add(sky);
		// Sky fills the resolved target; restore it before MSAA geometry resumes.
		auto restore = TRefPtr<CaptureRestoreNode>::Make();
		restore->SetRHIResource("src"_h, color);
		restore->SetRHIResource("dst"_h, color);
		graph->GetGraph().Add(restore);
		auto resolveGi = TRefPtr<GlobalIlluminationResolveNode>::Make();
		resolveGi->SetRHIResource("depthSampler"_h, depth);
		resolveGi->SetRHIResource("probeCellIndices"_h, probeCells);
		graph->GetGraph().Add(resolveGi);
		for (const auto queue : { "Opaque", "Masked", "Transparent" })
		{
			const bool transparent = std::string_view(queue) == "Transparent";
			if (transparent)
			{
				auto copy = TRefPtr<CaptureTransmissionNode>::Make();
				copy->SetRHIResource("src"_h, color);
				copy->SetRHIResource("dst"_h, transmission);
				graph->GetGraph().Add(copy);
			}
			auto draw = TRefPtr<CaptureDrawNode>::Make();
			draw->SetString("Tag"_h, queue);
			draw->SetString("GPUCulling"_h, "false");
			draw->SetString("VirtualizeInstancePayloads"_h, "false");
			draw->SetRHIResource("color"_h, color);
			draw->SetRHIResource("depthStencil"_h, depth);
			draw->SetRHIResource("motionVectors"_h, motion);
			draw->SetRHIResource("globalIlluminationProbeCellIndicesSampler"_h, probeCells);
			if (transparent)
			{
				draw->SetString("Sorting"_h, "BackToFront");
				draw->SetRHIResource("transmissionFramebuffer"_h, transmission);
			}
			graph->GetGraph().Add(draw);
		}
		auto copyFace = TRefPtr<PostProcessNode>::Make();
		copyFace->SetTag("CaptureFace"_h);
		copyFace->SetString("shader"_h, "Shaders/ReflectionCaptureFace.shader");
		copyFace->SetRHIResource("colorSampler"_h, color);
		copyFace->SetRHIResource("color"_h, job.m_cubemap->GetFace(0u));
		copyFace->PreloadShader();
		graph->GetGraph().Add(copyFace);
		return true;
	}
}

uint64_t LightingECS::RequestLocalReflection(const LocalReflectionCaptureRequest& request)
{
	return m_localReflection ? m_localReflection->Request(request) : 0u;
}

void LightingECS::ResetLocalReflection()
{
	if (!m_localReflection)
	{
		return;
	}
	if (!m_localReflection->Reset())
	{
		return;
	}
	auto renderer = App::GetSubmodule<Renderer>();
	if (renderer && renderer->GetFrameGraph())
	{
		auto graph = renderer->GetFrameGraph()->GetRHI();
		Tasks::CreateTask("Reset local reflection"_h, [state = m_localReflection, graph]()
			{
				state->Poll(graph);
			}, EThreadType::Render)->Run();
	}
}

LocalReflectionCaptureStatus LightingECS::GetLocalReflectionStatus() const
{
	return m_localReflection ? m_localReflection->GetStatus() : LocalReflectionCaptureStatus{};
}

LocalReflectionCubemap LightingECS::GetLocalReflectionCapture() const
{
	return m_localReflection ? m_localReflection->GetCapture() : LocalReflectionCubemap{};
}

uint64_t LocalReflectionCaptureState::Request(const LocalReflectionCaptureRequest& request)
{
	if (!request.IsValid())
	{
		return 0u;
	}
	m_lock.Lock();
	m_pending = request;
	m_pending->m_parameters.m_minEnabled.w = 1.0f;
	m_bHasWork.store(true, std::memory_order_release);
	m_nextGeneration = g_nextReflectionGeneration.fetch_add(1u, std::memory_order_relaxed);
	m_status.m_requestedGeneration = m_nextGeneration;
	m_status.m_state = ELocalReflectionCaptureState::Pending;
	if (!m_warmup)
	{
		m_warmup = Tasks::CreateTaskWithResult<bool>("Prepare reflection capture shaders"_h,
			[]() { return WarmCaptureShaders(); }, EThreadType::Worker);
		m_warmup->Run();
	}
	const uint64_t generation = m_status.m_requestedGeneration;
	m_lock.Unlock();
	return generation;
}

bool LocalReflectionCaptureState::Reset()
{
	m_lock.Lock();
	if (m_status.m_requestedGeneration == 0u && !m_active)
	{
		m_lock.Unlock();
		return false;
	}
	m_resetGeneration = m_nextGeneration;
	m_pending.reset();
	m_capture = {};
	m_status.m_requestedGeneration = 0u;
	m_status.m_publishedGeneration = 0u;
	m_status.m_state = ELocalReflectionCaptureState::Idle;
	m_bResetPending = true;
	m_bHasWork.store(true, std::memory_order_release);
	if (m_active)
	{
		m_active->m_bCancelled.store(true, std::memory_order_release);
	}
	m_lock.Unlock();
	return true;
}

LocalReflectionCaptureStatus LocalReflectionCaptureState::GetStatus() const
{
	m_lock.Lock();
	auto result = m_status;
	m_lock.Unlock();
	return result;
}

LocalReflectionCubemap LocalReflectionCaptureState::GetCapture() const
{
	m_lock.Lock();
	auto result = m_capture;
	m_lock.Unlock();
	return result;
}

TRefPtr<LocalReflectionCaptureJob> LocalReflectionCaptureState::Capture(const RHISceneViewPtr& source)
{
	if (!m_bHasWork.load(std::memory_order_acquire))
	{
		return {};
	}
	m_lock.Lock();
	if (!m_pending || m_active || !m_warmup || !m_warmup->IsFinished())
	{
		m_lock.Unlock();
		return {};
	}
	if (!m_warmup->GetResult())
	{
		m_status.m_state = ELocalReflectionCaptureState::Failed;
		m_pending.reset();
		m_warmup.Clear();
		m_lock.Unlock();
		return {};
	}
	auto job = TRefPtr<LocalReflectionCaptureJob>::Make();
	job->m_request = *m_pending;
	job->m_generation = m_status.m_requestedGeneration;
	job->m_firstCamera = static_cast<uint32_t>(source->m_cameras.Num());
	m_active = job;
	m_pending.reset();
	m_status.m_state = ELocalReflectionCaptureState::Capturing;
	m_lock.Unlock();

	const glm::vec3 directions[] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
		{ 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	const glm::vec3 up[] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 },
		{ 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };
	const glm::vec3 position(job->m_request.m_parameters.m_positionBlend);
	source->m_firstShadowCamera = job->m_firstCamera;
	source->m_cameraRenderExtents.Resize(source->m_cameras.Num());
	for (uint32_t face = 0u; face < 6u; ++face)
	{
		CameraData camera;
		camera.SetFov(90.0f);
		camera.SetAspect(1.0f);
		camera.SetZNear(job->m_request.m_nearPlane);
		camera.SetZFar(job->m_request.m_farPlane);
		camera.SetViewMatrix(glm::lookAtRH(position, position + directions[face], up[face]));
		camera.SetProjectionMatrix(Math::PerspectiveRH(glm::radians(90.0f), 1.0f,
			job->m_request.m_nearPlane, job->m_request.m_farPlane));
		source->m_cameras.Add(camera);
		source->m_cameraTransforms.Add(Math::Transform::FromMatrix(camera.GetInvViewMatrix()));
		source->m_cameraRenderExtents.Add(glm::ivec2(job->m_request.m_faceSize));
	}
	return job;
}

void LocalReflectionCaptureJob::ExtractSnapshots(const RHISceneViewPtr& source)
{
	m_submission = source->GetOrCreateSubmissionCompletionToken();
	for (uint32_t face = 0u; face < 6u; ++face)
	{
		auto view = RHISceneViewPtr::Make();
		view->m_snapshots.Add(std::move(source->m_snapshots[m_firstCamera + face]));
		auto& snapshot = view->m_snapshots[0];
		snapshot.m_renderMode = ESceneViewRenderMode::Lit;
		snapshot.m_debugDrawSecondaryCmdList.Clear();
		snapshot.m_drawImGui.Clear();
		snapshot.m_pathTracerScene.Clear();
		m_faces[face] = std::move(view);
	}
	source->RestoreCameraCount(m_firstCamera);
}

TVector<Tasks::ITaskPtr> LocalReflectionCaptureJob::Prepare(const RHIFrameGraphPtr& source,
	const RHIRenderSubmissionContextPtr& context)
{
	m_completion = context->GetOrCreateFrameCompletion();
	auto frameContext = context;
	frameContext->RetainResource(ToRefPtr<LocalReflectionCaptureJob>());
	TVector<Tasks::ITaskPtr> tasks;
	if (m_bCancelled.load(std::memory_order_acquire) || !InitializeGraph(*this, source))
	{
		return tasks;
	}
	// Private resource ownership avoids keeping obsolete per-request graph/node
	// keys in the main renderer's reusable flight context.
	m_context = RHIRenderSubmissionContextPtr::Make();
	m_context->BeginSubmission(context->GetSubmissionId(), context->GetFlightSlot(),
		context->GetMaterialRevision(), m_generation);
	for (auto& face : m_faces)
	{
		face->SetSubmissionContext(m_context);
		tasks.AddRange(m_graph->Prepare(face));
	}
	m_bPrepared = true;
	return tasks;
}

bool LocalReflectionCaptureJob::Submit(RHISemaphorePtr& chainSemaphore)
{
	if (!m_bPrepared || m_bCancelled.load(std::memory_order_acquire))
	{
		return true;
	}
	if (!IsPrepared(*this))
	{
		m_bRetry = true;
		return true;
	}
	auto& driver = Renderer::GetDriver();
	auto commands = Renderer::GetDriverCommands();
	for (uint32_t face = 0u; face < 6u; ++face)
	{
		m_graph->GetGraphNode("CaptureFace"_h)->SetRHIResource("color"_h, m_cubemap->GetFace(face));
		TVector<RHICommandListPtr> transfers, draws;
		RHISemaphorePtr ready = chainSemaphore;
		const bool recorded = m_graph->Process(m_faces[face], transfers, draws, chainSemaphore, ready, false);
		chainSemaphore = ready;
		if (!recorded)
		{
			return false;
		}
		const auto& snapshot = m_faces[face]->m_snapshots[0];
		for (const auto& node : m_graph->GetGraph())
		{
			const auto draw = node.DynamicCast<CaptureDrawNode>();
			const auto depth = node.DynamicCast<CaptureDepthNode>();
			const auto culling = node.DynamicCast<CaptureLightCullingNode>();
			const auto restore = node.DynamicCast<CaptureRestoreNode>();
			const auto transmission = node.DynamicCast<CaptureTransmissionNode>();
			const auto resolveGi = node.DynamicCast<GlobalIlluminationResolveNode>();
			const bool fullscreen = node.DynamicCast<PostProcessNode>() || node.DynamicCast<LinearizeDepthNode>();
			if ((draw && !draw->WasRecorded(snapshot)) || (depth && !depth->WasRecorded(snapshot)) ||
				(culling && !culling->WasRecorded(snapshot)) || (restore && !restore->WasRecorded(m_graph)) ||
				(transmission && !transmission->m_bComplete) ||
				(resolveGi && !resolveGi->WasRecorded()) ||
				(fullscreen && node->GetDrawCallStats().m_numInstances != 1u))
			{
				m_bRetry = true;
				return true;
			}
		}
		for (const auto& shadow : snapshot.m_shadowMapsToUpdate)
		{
			if (!shadow.m_payloadCompletionToken || !shadow.m_payloadCompletionToken->IsSuccessful())
			{
				m_bRetry = true;
				return true;
			}
		}
		// Finish this face's tail before the next face can reuse its attachments.
		for (size_t i = 0u; i < transfers.Num(); ++i)
		{
			for (const auto& command : { transfers[i], draws[i] })
			{
				auto next = driver->CreateWaitSemaphore();
				auto fence = RHIFencePtr::Make();
				if (!driver->SubmitCommandList(command, fence, next, chainSemaphore))
				{
					return false;
				}
				chainSemaphore = next;
			}
		}
	}
	auto finish = driver->CreateCommandList(false, ECommandListQueue::Graphics);
	commands->BeginCommandList(finish, true);
	const uint32_t beforeMips = finish->GetNumRecordedCommands();
	commands->GenerateMipMaps(finish, m_cubemap);
	const bool mipChainRecorded = finish->GetNumRecordedCommands() > beforeMips;
	commands->ImageMemoryBarrier(finish, m_cubemap, EImageLayout::ShaderReadOnlyOptimal);
	commands->EndCommandList(finish);
	if (!mipChainRecorded)
	{
		m_bRetry = true;
		return true;
	}
	auto next = driver->CreateWaitSemaphore();
	if (!driver->SubmitCommandList(finish, RHIFencePtr::Make(), next, chainSemaphore))
	{
		return false;
	}
	chainSemaphore = next;
	m_bSubmitted = true;
	return true;
}

void LocalReflectionCaptureState::Poll(const RHIFrameGraphPtr& graph)
{
	if (!graph || !m_bHasWork.load(std::memory_order_acquire))
	{
		return;
	}
	auto sourceGraph = graph;
	auto environment = sourceGraph->GetGraphNode("Environment"_h).DynamicCast<EnvironmentNode>();
	if (!environment)
	{
		return;
	}
	m_lock.Lock();
	if (m_bResetPending)
	{
		environment->ResetLocalReflection(m_resetGeneration);
		m_bResetPending = false;
	}
	if (m_active && m_active->m_submission && !m_active->m_submission->IsPending())
	{
		auto job = m_active;
		const EFenceStatus fence = job->m_completion ? job->m_completion->GetStatus() : EFenceStatus::Failed;
		if (fence != EFenceStatus::Pending)
		{
			if (job->m_bSubmitted)
			{
				++m_status.m_capturesSubmitted;
			}
			if (job->m_generation > m_resetGeneration)
			{
				if (job->m_bSubmitted && job->m_submission->IsSuccessful() && fence == EFenceStatus::Finished)
				{
					LocalReflectionCubemap capture{ job->m_request.m_parameters, job->m_cubemap, job->m_generation };
					if (environment->SetLocalReflection(capture))
					{
						m_capture = std::move(capture);
						m_status.m_state = ELocalReflectionCaptureState::Filtering;
					}
					else
					{
						m_status.m_state = ELocalReflectionCaptureState::Failed;
					}
				}
				else if (job->m_bRetry && fence == EFenceStatus::Finished && !m_pending)
				{
					m_pending = job->m_request;
					m_status.m_state = ELocalReflectionCaptureState::Pending;
				}
				else if (!m_pending)
				{
					m_status.m_state = ELocalReflectionCaptureState::Failed;
				}
			}
			m_active.Clear();
		}
	}
	const auto published = environment->GetLocalReflectionGeneration();
	if (published > m_resetGeneration && published <= m_status.m_requestedGeneration)
	{
		m_status.m_publishedGeneration = published;
		if (!m_active && !m_pending && published == m_status.m_requestedGeneration)
		{
			m_status.m_state = ELocalReflectionCaptureState::Ready;
		}
	}
	if (!m_active && !m_pending && !m_bResetPending &&
		m_status.m_state != ELocalReflectionCaptureState::Filtering)
	{
		m_bHasWork.store(false, std::memory_order_release);
	}
	m_lock.Unlock();
}

void LocalReflectionCaptureState::Cancel(const TRefPtr<LocalReflectionCaptureJob>& job)
{
	m_lock.Lock();
	if (job && m_active == job)
	{
		m_active.Clear();
		if (job->m_generation > m_resetGeneration && !m_pending)
		{
			// Flight acquisition can fail while a window is being resized. Keep
			// the explicit request until a frame can actually render its capture.
			m_pending = job->m_request;
			m_status.m_state = ELocalReflectionCaptureState::Pending;
			m_bHasWork.store(true, std::memory_order_release);
		}
	}
	m_lock.Unlock();
}
