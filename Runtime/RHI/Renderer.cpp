#include "Types.h"
#include "RHI/RenderTarget.h"
#include "Renderer.h"
#include "Mesh.h"
#include "CommandList.h"
#include "GraphicsDriver.h"
#include "VertexDescription.h"
#include "Engine/EngineLoop.h"
#include "Engine/GameObject.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "Tasks/Scheduler.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "Components/TestComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Engine/World.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "ECS/CameraECS.h"
#include "ECS/LightingECS.h"
#include "ECS/LandscapeECS.h"
#include "ECS/AnimationECS.h"
#include "ECS/PathTracerECS.h"
#include "Settings/GraphicsSettings.h"

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	struct RenderSubmissionBeginState
	{
		~RenderSubmissionBeginState()
		{
			if (m_bMaterialCaptureActive)
			{
				RHIMaterial::EndSubmissionVersionCapture(m_submissionId);
			}
		}

		bool m_bLifecycleReady = false;
		bool m_bHasSwapchainImage = false;
		bool m_bMaterialCaptureActive = false;
		uint64_t m_submissionId = 0ull;
		uint64_t m_materialRevision = 0ull;
		uint32_t m_flightSlot = 0u;
		RHIRenderSubmissionContextPtr m_context{};
	};
}

void IDelayedInitialization::TraceVisit(class TRefPtr<RHIResource> visitor, bool& bShouldRemoveFromList)
{
	bShouldRemoveFromList = false;

	if (auto fence = TRefPtr<RHI::RHIFence>(visitor.GetRawPtr()))
	{
		if (fence->IsFinished())
		{
			m_dependenciesLock.Lock();
			auto it = std::find_if(m_dependencies.begin(), m_dependencies.end(),
				[&fence](const auto& lhs)
				{
					return fence.GetRawPtr() == lhs.GetRawPtr();
				});

			if (it != std::end(m_dependencies))
			{
				std::iter_swap(it, m_dependencies.end() - 1);
				m_dependencies.RemoveLast();
				bShouldRemoveFromList = true;
			}
			m_dependenciesLock.Unlock();
		}
	}
}

bool IDelayedInitialization::IsReady() const
{
	m_dependenciesLock.Lock();
	const bool bIsReady = m_dependencies.IsEmpty();
	m_dependenciesLock.Unlock();
	return bIsReady;
}

Renderer::Renderer(Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
{
	m_pViewport = pViewport;
	m_msaaSamples = msaaSamples;
	m_bIsInitialized = false;

#if defined(SAILOR_BUILD_WITH_VULKAN)
	m_driverInstance = TUniquePtr<Sailor::GraphicsDriver::Vulkan::VulkanGraphicsDriver>::Make();
	m_driverInstance->Initialize(pViewport, msaaSamples, bIsDebug);
	auto* vulkanDriver = dynamic_cast<Sailor::GraphicsDriver::Vulkan::VulkanGraphicsDriver*>(m_driverInstance.GetRawPtr());
	if (!vulkanDriver || !vulkanDriver->IsInitialized())
	{
		SAILOR_LOG_ERROR("Renderer initialization failed: Vulkan backend is unavailable.");
		return;
	}
#endif
	m_bIsInitialized = true;

	const uint32_t numFlightSlots = (std::max)(1u, m_driverInstance->GetMaxFramesInFlight());
	m_submissionContexts.Resize(numFlightSlots);
	for (auto& context : m_submissionContexts)
	{
		context = RHIRenderSubmissionContextPtr::Make();
	}

	// Create default Vertices descriptions cache 
	auto& vertexP3N3UV2C4 = m_driverInstance->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
	vertexP3N3UV2C4->SetVertexStride(sizeof(RHI::VertexP3N3UV2C4));
	vertexP3N3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultPositionBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3UV2C4::m_position));
	vertexP3N3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultNormalBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3UV2C4::m_normal));
	vertexP3N3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultTexcoordBinding, 0, RHI::EFormat::R32G32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3UV2C4::m_texcoord));
	vertexP3N3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultColorBinding, 0, RHI::EFormat::R32G32B32A32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3UV2C4::m_color));

	auto& vertexP3C4 = m_driverInstance->GetOrAddVertexDescription<RHI::VertexP3C4>();
	vertexP3C4->SetVertexStride(sizeof(RHI::VertexP3C4));
	vertexP3C4->AddAttribute(RHIVertexDescription::DefaultPositionBinding, 0, EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3C4::m_position));
	vertexP3C4->AddAttribute(RHIVertexDescription::DefaultColorBinding, 0, EFormat::R32G32B32A32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3C4::m_color));

	auto& vertexP2UV2C1 = m_driverInstance->GetOrAddVertexDescription<RHI::VertexP2UV2C1>();
	vertexP2UV2C1->SetVertexStride(sizeof(RHI::VertexP2UV2C1));
	vertexP2UV2C1->AddAttribute(RHIVertexDescription::DefaultPositionBinding, 0, EFormat::R32G32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP2UV2C1::m_position));
	vertexP2UV2C1->AddAttribute(RHIVertexDescription::DefaultTexcoordBinding, 0, EFormat::R32G32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP2UV2C1::m_uv));
	vertexP2UV2C1->AddAttribute(RHIVertexDescription::DefaultColorBinding, 0, EFormat::R8G8B8A8_UNORM, (uint32_t)Sailor::OffsetOf(&RHI::VertexP2UV2C1::m_color));

	auto& vertexP3N3T3B3UV2C4 = m_driverInstance->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4>();
	vertexP3N3T3B3UV2C4->SetVertexStride(sizeof(RHI::VertexP3N3T3B3UV2C4));
	vertexP3N3T3B3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultPositionBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4::m_position));
	vertexP3N3T3B3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultNormalBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4::m_normal));
	vertexP3N3T3B3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultTexcoordBinding, 0, RHI::EFormat::R32G32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4::m_texcoord));
	vertexP3N3T3B3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultColorBinding, 0, RHI::EFormat::R32G32B32A32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4::m_color));
	vertexP3N3T3B3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultTangentBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4::m_tangent));
	vertexP3N3T3B3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultBitangentBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4::m_bitangent));

	auto& vertexSkinned = m_driverInstance->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4I4W4>();
	vertexSkinned->SetVertexStride(sizeof(RHI::VertexP3N3T3B3UV2C4I4W4));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultPositionBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_position));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultNormalBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_normal));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultTexcoordBinding, 0, RHI::EFormat::R32G32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_texcoord));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultColorBinding, 0, RHI::EFormat::R32G32B32A32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_color));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultTangentBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_tangent));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultBitangentBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_bitangent));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding, 0, RHI::EFormat::R32G32B32A32_UINT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_boneIds));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding, 0, RHI::EFormat::R32G32B32A32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_boneWeights));
}

Renderer::~Renderer()
{
	if (m_bIsInitialized && m_driverInstance)
	{
		if (App::GetSubmodule<Tasks::Scheduler>())
		{
			Renderer::GetDriver()->WaitIdle();
		}
	}

	// Submission contexts retain materials, pipelines, shader modules, and the
	// Vulkan device that created them. Release every renderer-owned GPU resource
	// before destroying the driver/Vulkan instance. Otherwise the last device
	// reference can be released from a late shader-module destructor after the
	// instance has already been destroyed (MoltenVK crashes in that ordering).
	m_previousRenderFrame.Clear();
	m_frameGraph.Clear();
	m_cachedSceneViews.Clear();
	m_submissionContexts.Clear();
	m_driverInstance.Clear();
}

void Renderer::MemoryStats()
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	auto renderer = App::GetSubmodule<Renderer>();
	if (!renderer)
	{
		return;
	}
	renderer->UpdateMemoryStats();
	const auto& stats = renderer->GetStats();
	constexpr float BytesToMb = 1.0f / (1024.0f * 1024.0f);

	SAILOR_LOG("Memory consumption (GPU):");
	SAILOR_LOG("Materials: %.2fmb", stats.m_materialsMemoryUsage.load(std::memory_order_relaxed) * BytesToMb);
	SAILOR_LOG("General: %.2fmb", stats.m_generalMemoryUsage.load(std::memory_order_relaxed) * BytesToMb);
	SAILOR_LOG("Meshes: %.2fmb", stats.m_meshesMemoryUsage.load(std::memory_order_relaxed) * BytesToMb);
	SAILOR_LOG("Textures: %.2fmb", stats.m_texturesMemoryUsage.load(std::memory_order_relaxed) * BytesToMb);

#endif
}

void Renderer::UpdateMemoryStats()
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	auto driverInstance = GetDriver().DynamicCast<Sailor::GraphicsDriver::Vulkan::VulkanGraphicsDriver>();
	if (!driverInstance)
	{
		return;
	}

	size_t texturesOccupiedSpace = 0u;
	const auto& internalMemoryAllocators = VulkanApi::GetInstance()->GetMainDevice()->GetMemoryAllocators();
	for (const auto& allocator : internalMemoryAllocators)
	{
		texturesOccupiedSpace += allocator.m_second->GetOccupiedSpace();
	}

	const auto& materialAllocator = driverInstance->GetMaterialSsboAllocatorIfInitialized();
	const auto& generalAllocator = driverInstance->GetGeneralSsboAllocatorIfInitialized();
	const auto& meshAllocator = driverInstance->GetMeshSsboAllocatorIfInitialized();
	m_stats.m_materialsMemoryUsage.store(
		materialAllocator ? materialAllocator->GetOccupiedSpace() : 0u,
		std::memory_order_relaxed);
	m_stats.m_generalMemoryUsage.store(
		generalAllocator ? generalAllocator->GetOccupiedSpace() : 0u,
		std::memory_order_relaxed);
	m_stats.m_meshesMemoryUsage.store(
		meshAllocator ? meshAllocator->GetOccupiedSpace() : 0u,
		std::memory_order_relaxed);
	m_stats.m_texturesMemoryUsage.store(texturesOccupiedSpace, std::memory_order_relaxed);
#endif
}

RHI::EFormat Renderer::GetColorFormat() const
{
	if (!m_bIsInitialized || !m_driverInstance || !Renderer::GetDriver()->GetBackBuffer())
	{
		return RHI::EFormat::UNDEFINED;
	}

	return Renderer::GetDriver()->GetBackBuffer()->GetFormat();
}

RHI::EFormat Renderer::GetDepthFormat() const
{
	if (!m_bIsInitialized || !m_driverInstance || !Renderer::GetDriver()->GetDepthBuffer())
	{
		return RHI::EFormat::UNDEFINED;
	}

	return Renderer::GetDriver()->GetDepthBuffer()->GetFormat();
}

void Renderer::BeginConditionalDestroy()
{
	m_bForceStop = true;

	if (!m_driverInstance)
	{
		return;
	}

	if (!m_bIsInitialized)
	{
		m_previousRenderFrame.Clear();
		m_frameGraph.Clear();
		m_cachedSceneViews.Clear();
		m_submissionContexts.Clear();
		return;
	}

	if (m_previousRenderFrame.IsValid())
	{
		m_previousRenderFrame->Wait();
		m_previousRenderFrame.Clear();
	}

	Renderer::GetDriver()->WaitIdle();

	m_frameGraph.Clear();
	m_cachedSceneViews.Clear();
	m_submissionContexts.Clear();
	m_driverInstance->BeginConditionalDestroy();
}

TUniquePtr<IGraphicsDriver>& Renderer::GetDriver()
{
	return App::GetSubmodule<Renderer>()->m_driverInstance;
}

IGraphicsDriverCommands* Renderer::GetDriverCommands()
{

#if defined(SAILOR_BUILD_WITH_VULKAN)
	return dynamic_cast<IGraphicsDriverCommands*>(App::GetSubmodule<Renderer>()->m_driverInstance.GetRawPtr());
#endif

	return nullptr;
}

void Renderer::FixLostDevice()
{
	if (!m_bIsInitialized || !m_driverInstance)
	{
		return;
	}

	if (m_driverInstance->FixLostDevice(m_pViewport))
	{
		m_bFrameGraphOutdated = true;
	}
}

RHISceneViewPtr Renderer::GetOrAddSceneView(WorldPtr worldPtr)
{
	SAILOR_PROFILE_FUNCTION();

	RHISceneViewPtr res;
	auto& list = m_cachedSceneViews.At_Lock(worldPtr);
	auto it = list.FindIf([](const auto& el) { return el.m_second == true; });
	if (it != list.end())
	{
		res = (*it).m_first;
		(*it).m_second = false;

		m_cachedSceneViews.Unlock(worldPtr);

		return res;
	}

	auto newEl = TPair(RHISceneViewPtr::Make(), false);
	list.EmplaceBack(newEl);
	res = newEl.m_first;

	m_cachedSceneViews.Unlock(worldPtr);

	return res;
}

void Renderer::RemoveSceneView(WorldPtr worldPtr)
{
	m_cachedSceneViews.Remove(worldPtr);
}

bool Renderer::EnsureFrameGraph()
{
	if (m_frameGraph && !m_bFrameGraphOutdated)
	{
		return true;
	}

	if (!m_bIsInitialized || !m_driverInstance || !m_pViewport || m_pViewport->IsIconic())
	{
		return false;
	}

	m_frameGraph.Clear();

	const char* frameGraphAssetPath = App::HasEditor() ? "EditorRenderer.renderer" : "DefaultRenderer.renderer";
	if (auto frameGraphFileId = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<AssetInfoPtr>(frameGraphAssetPath))
	{
		App::GetSubmodule<FrameGraphImporter>()->Instantiate_Immediate(frameGraphFileId->GetFileId(), m_frameGraph);
	}
	else
	{
		SAILOR_LOG_ERROR("Renderer::EnsureFrameGraph failed: %s asset info was not found.", frameGraphAssetPath);
	}

	m_bFrameGraphOutdated = false;
	if (m_frameGraph)
	{
		++m_frameGraphResourceGeneration;
	}
	return m_frameGraph.IsValid();
}

bool Renderer::PushFrame(const Sailor::FrameState& frame)
{
	SAILOR_PROFILE_FUNCTION();
	if (!m_bIsInitialized || !m_driverInstance)
	{
		return false;
	}

	if (m_bForceStop ||
		(!App::HasEditor() && m_driverInstance->ShouldFixLostDevice(m_pViewport)) ||
		App::GetSubmodule<Tasks::Scheduler>()->GetNumTasks(EThreadType::Render) > MaxFramesInQueue)
	{
		return false;
	}

	if (m_previousRenderFrame.IsValid() && m_previousRenderFrame->IsFinished())
	{
		m_previousRenderFrame.Clear();
	}

	if (!EnsureFrameGraph() || !m_frameGraph)
	{
		return false;
	}

	WorldPtr world = frame.GetWorld();
	RHISceneViewPtr rhiSceneView;
	const uint64_t currentFrame = world->GetCurrentFrame();
	const uint64_t submissionId = m_nextSubmissionId.fetch_add(1ull, std::memory_order_relaxed);
	auto rhiFrameGraph = m_frameGraph->GetRHI();
	const uint64_t frameGraphResourceGeneration = m_frameGraphResourceGeneration;
	auto submissionBeginState = TSharedPtr<RenderSubmissionBeginState>::Make();
	submissionBeginState->m_submissionId = submissionId;
	submissionBeginState->m_materialRevision =
		RHIMaterial::BeginSubmissionVersionCapture(submissionId);
	submissionBeginState->m_bMaterialCaptureActive = true;

	{
		SAILOR_PROFILE_SCOPE("Copy scene view to render thread");

		rhiSceneView = GetOrAddSceneView(world);

		rhiSceneView->m_world = world;
		world->GetECS<StaticMeshRendererECS>()->CopySceneView(rhiSceneView);
		world->GetECS<LandscapeECS>()->AppendSceneView(rhiSceneView);
		if (auto* pathTracerEcs = world->GetECS<PathTracerECS>())
		{
			pathTracerEcs->CopySceneView(rhiSceneView);
		}
		world->GetECS<CameraECS>()->CopyCameraData(rhiSceneView);

		rhiSceneView->m_deltaTime = frame.GetDeltaTime();
		rhiSceneView->m_currentTime = frame.GetWorld()->GetTime();
		rhiSceneView->m_renderMode = App::GetEditorRenderMode();
	}

	const uint64_t sceneRevision = rhiSceneView->m_sceneRevision;
	auto acquireRenderSubmission = Tasks::CreateTask(
		"Acquire render submission flight " + std::to_string(currentFrame),
		[this, rhiSceneView, submissionId, sceneRevision,
			frameGraphResourceGeneration, submissionBeginState]()
		{
			uint32_t flightSlot = 0u;
			bool bHasSwapchainImage = false;
			submissionBeginState->m_bLifecycleReady =
				m_driverInstance->BeginRenderSubmission(
					flightSlot,
					bHasSwapchainImage);
			submissionBeginState->m_flightSlot = flightSlot;
			submissionBeginState->m_bHasSwapchainImage = bHasSwapchainImage;

			if (submissionBeginState->m_bLifecycleReady &&
				flightSlot < m_submissionContexts.Num())
			{
				auto context = m_submissionContexts[flightSlot];
				context->BeginSubmission(
					submissionId,
					flightSlot,
					sceneRevision,
					submissionBeginState->m_materialRevision,
					frameGraphResourceGeneration);
				for (const auto& spatialVersion : rhiSceneView->m_sceneVersions)
				{
					if (!spatialVersion || !spatialVersion->m_scene ||
						!spatialVersion->m_sceneVersion)
					{
						continue;
					}

					auto flightState = spatialVersion->m_scene->PrepareFlight(
						flightSlot,
						spatialVersion->m_sceneVersion);
					context->RetainResource(spatialVersion->m_scene);
					context->RetainResource(spatialVersion->m_sceneVersion);
					context->RetainResource(flightState);
					spatialVersion->m_scene->CollectGarbage();
				}
				rhiSceneView->SetSubmissionContext(context);
				submissionBeginState->m_context = std::move(context);
			}
			else
			{
				submissionBeginState->m_bLifecycleReady = false;
				SAILOR_LOG_ERROR(
					"Renderer::PushFrame: failed to acquire render submission flight slot.");
			}

			this->GetDriver()->TrackResources_ThreadSafe();
		},
		Sailor::EThreadType::Render);
	if (m_previousRenderFrame.IsValid())
	{
		acquireRenderSubmission->Join(m_previousRenderFrame);
	}
	acquireRenderSubmission->Run();
	acquireRenderSubmission->Wait();

	if (!submissionBeginState->m_bLifecycleReady ||
		!submissionBeginState->m_context)
	{
		if (submissionBeginState->m_bMaterialCaptureActive)
		{
			RHIMaterial::EndSubmissionVersionCapture(submissionId);
			submissionBeginState->m_bMaterialCaptureActive = false;
		}
		rhiSceneView->Clear();
		auto& list = m_cachedSceneViews.At_Lock(world);
		auto it = list.FindIf([&](const auto& el)
			{
				return el.m_first == rhiSceneView;
			});
		if (it != list.end())
		{
			(*it).m_second = true;
		}
		m_cachedSceneViews.Unlock(world);
		return false;
	}

	{
		SAILOR_PROFILE_SCOPE("Prepare flight-local scene view");
		world->GetECS<AnimationECS>()->FillAnimationData(rhiSceneView);
		world->GetECS<LightingECS>()->FillLightingData(rhiSceneView);
		rhiSceneView->m_drawImGui = frame.GetDrawImGuiTask();
		rhiSceneView->PrepareDebugDrawCommandLists(
			world,
			rhiFrameGraph->GetSceneRenderExtent());
		rhiSceneView->PrepareSnapshots();
	}

	{
		SAILOR_PROFILE_SCOPE("Push frame");

		auto renderFrame1 = Tasks::CreateTask("Render Frame " + std::to_string(currentFrame),
			[this, rhiFrameGraph = rhiFrameGraph, frame, rhiSceneView, submissionId, submissionBeginState]() mutable
			{
				SAILOR_PROFILE_SCOPE("Render Frame");
				bool bSubmissionResourcesSucceeded = false;
				auto frameInstance = frame;
				static Utils::Timer timer;
				timer.Start();

				TVector<RHI::RHICommandListPtr> primaryCommandLists;
				TVector<RHI::RHICommandListPtr> transferCommandLists;

				static uint32_t totalFramesCount = 0U;

				auto updateFrameRHI = [&frameInstance = frameInstance](RHISemaphorePtr& inOutChainSemaphore)
					{
						SAILOR_PROFILE_SCOPE("Submit & Wait frame command lists");
						for (uint32_t i = 0; i < frameInstance.NumCommandLists; i++)
						{
							if (auto pCommandList = frameInstance.GetCommandBuffer(i))
							{
								auto signalSemaphore = GetDriver()->CreateWaitSemaphore();
								auto fence = RHIFencePtr::Make();
								GetDriver()->SetDebugName(signalSemaphore, std::format("frameInstance CommandBuffer {}", i));
								GetDriver()->SetDebugName(fence, std::format("frameInstance CommandBuffer {}", i));

								if (!GetDriver()->SubmitCommandList(pCommandList, fence, signalSemaphore, inOutChainSemaphore))
								{
									SAILOR_LOG_ERROR("Renderer::PushFrame: failed to submit frame command buffer %u.", i);
									return false;
								}

								inOutChainSemaphore = signalSemaphore;
							}
						}

						return true;
					};

				const bool bHasSwapchainImage = submissionBeginState->m_bHasSwapchainImage;
				if (submissionBeginState->m_bLifecycleReady)
				{
					bool bFrameGraphProcessed = false;
					RHISemaphorePtr chainSemaphore{};
					const bool bCanRenderFrame = !m_bForceStop &&
						(bHasSwapchainImage || App::HasEditor());
					bool bFrameSubmitsSucceeded = true;
					bool bGpuFrameTimeQueryStarted = false;
					if (bCanRenderFrame &&
						App::GetRenderStatsMode() ==
						Settings::ERenderStatsMode::RenderStatsAndQueries &&
						m_driverInstance->SupportsGpuFrameTimeQueries())
					{
						auto queryBeginCommandList = m_driverInstance->CreateCommandList(
							false,
							RHI::ECommandListQueue::Graphics);
						m_driverInstance->SetDebugName(
							queryBeginCommandList,
							"GpuFrameTime:Begin");
						GetDriverCommands()->BeginCommandList(
							queryBeginCommandList,
							true);
						bGpuFrameTimeQueryStarted =
							m_driverInstance->BeginGpuFrameTimeQuery(
								queryBeginCommandList);
						GetDriverCommands()->EndCommandList(queryBeginCommandList);

						if (bGpuFrameTimeQueryStarted)
						{
							auto signalSemaphore = GetDriver()->CreateWaitSemaphore();
							auto fence = RHIFencePtr::Make();
							GetDriver()->SetDebugName(
								signalSemaphore,
								"GpuFrameTime:Begin");
							GetDriver()->SetDebugName(fence, "GpuFrameTime:Begin");
							if (!GetDriver()->SubmitCommandList(
								queryBeginCommandList,
								fence,
								signalSemaphore,
								chainSemaphore))
							{
								GetDriver()->CancelGpuFrameTimeQuery();
								bGpuFrameTimeQueryStarted = false;
								bFrameSubmitsSucceeded = false;
								SAILOR_LOG_ERROR("Renderer::PushFrame: failed to submit the GPU frame-time begin boundary.");
							}
							else
							{
								chainSemaphore = signalSemaphore;
							}
						}
					}

					if (bFrameSubmitsSucceeded && !m_bForceStop)
					{
						bFrameSubmitsSucceeded = updateFrameRHI(chainSemaphore);
					}
					DrawCallStats drawCallStats;

					if (bFrameSubmitsSucceeded && bCanRenderFrame &&
						!m_bFrameGraphOutdated && !m_pViewport->IsIconic())
					{
						if (!App::HasEditor())
						{
							rhiFrameGraph->SetRenderTarget("BackBuffer", m_driverInstance->GetBackBuffer());
						}

						RHISemaphorePtr frameGraphChainSemaphore = chainSemaphore;
						const bool bFrameGraphSucceeded = rhiFrameGraph->Process(
							rhiSceneView,
							transferCommandLists,
							primaryCommandLists,
							chainSemaphore,
							frameGraphChainSemaphore);
						chainSemaphore = frameGraphChainSemaphore;
						if (!bFrameGraphSucceeded)
						{
							SAILOR_LOG_ERROR("Renderer::PushFrame: FrameGraph command buffer submission failed.");
							bFrameSubmitsSucceeded = false;
						}
						else
						{
							bFrameGraphProcessed = true;
							drawCallStats = rhiFrameGraph->GetDrawCallStats();
						}
					}

					if (bFrameSubmitsSucceeded)
					{
						SAILOR_PROFILE_SCOPE("Submit transfer command lists");
						uint32_t i = 0;
						for (auto& cmdList : transferCommandLists)
						{
							auto signalSemaphore = GetDriver()->CreateWaitSemaphore();
							auto fence = RHIFencePtr::Make();
							GetDriver()->SetDebugName(signalSemaphore, std::format("rhiFrameGraph TransferCommandList {}", i));
							GetDriver()->SetDebugName(fence, std::format("rhiFrameGraph TransferCommandList {}", i));

							if (!GetDriver()->SubmitCommandList(cmdList, fence, signalSemaphore, chainSemaphore))
							{
								SAILOR_LOG_ERROR("Renderer::PushFrame: failed to submit FrameGraph transfer command buffer %u.", i);
								bFrameSubmitsSucceeded = false;
								break;
							}

							chainSemaphore = signalSemaphore;
							i++;
						}
					}

					if (bGpuFrameTimeQueryStarted)
					{
						auto queryEndCommandList = m_driverInstance->CreateCommandList(
							false,
							RHI::ECommandListQueue::Graphics);
						m_driverInstance->SetDebugName(
							queryEndCommandList,
							"GpuFrameTime:End");
						GetDriverCommands()->BeginCommandList(
							queryEndCommandList,
							true);
						m_driverInstance->EndGpuFrameTimeQuery(queryEndCommandList);
						GetDriverCommands()->EndCommandList(queryEndCommandList);

						if (bFrameSubmitsSucceeded)
						{
							primaryCommandLists.Add(queryEndCommandList);
						}
						else
						{
							auto signalSemaphore = GetDriver()->CreateWaitSemaphore();
							auto fence = RHIFencePtr::Make();
							GetDriver()->SetDebugName(
								signalSemaphore,
								"GpuFrameTime:EndAfterFailure");
							GetDriver()->SetDebugName(
								fence,
								"GpuFrameTime:EndAfterFailure");
							if (GetDriver()->SubmitCommandList(
								queryEndCommandList,
								fence,
								signalSemaphore,
								chainSemaphore))
							{
								GetDriver()->CommitGpuFrameTimeQuery();
								chainSemaphore = signalSemaphore;
							}
							else
							{
								GetDriver()->CancelGpuFrameTimeQuery();
								SAILOR_LOG_ERROR("Renderer::PushFrame: failed to submit the GPU frame-time end boundary after a frame failure.");
							}
						}
					}

					TVector<RHISemaphorePtr> waitFrameUpdate;
					if (chainSemaphore)
					{
						waitFrameUpdate.Add(chainSemaphore);
						if (bFrameSubmitsSucceeded && submissionBeginState->m_context)
						{
							submissionBeginState->m_context->SetResourceReadySemaphore(chainSemaphore);
						}
					}

					if (!bFrameSubmitsSucceeded)
					{
						TVector<RHICommandListPtr> noCommandLists;
						const bool bFlightReleased = bHasSwapchainImage ?
							m_driverInstance->PresentFrame(frame, noCommandLists, waitFrameUpdate) :
							m_driverInstance->SubmitFrameWithoutPresent(
								noCommandLists,
								waitFrameUpdate);
						if (!bFlightReleased)
						{
							SAILOR_LOG_ERROR("Renderer::PushFrame: failed to fence an acquired flight slot after a submit failure.");
						}
					}

					bool bFrameCompleted = false;
					if (bFrameSubmitsSucceeded)
					{
						bFrameCompleted = bHasSwapchainImage
							? m_driverInstance->PresentFrame(frame, primaryCommandLists, waitFrameUpdate)
							: m_driverInstance->SubmitFrameWithoutPresent(primaryCommandLists, waitFrameUpdate);
					}

					if (bFrameCompleted)
					{
						bSubmissionResourcesSucceeded = bFrameGraphProcessed;
						m_stats.m_numBatches.store(drawCallStats.m_numBatches, std::memory_order_relaxed);
						m_stats.m_numInstances.store(drawCallStats.m_numInstances, std::memory_order_relaxed);
						totalFramesCount++;
						timer.Stop();

						if (timer.ResultAccumulatedMs() > 1000)
						{
							m_stats.m_gpuFps.store(totalFramesCount, std::memory_order_relaxed);
							totalFramesCount = 0;
							timer.Clear();
#if defined(SAILOR_BUILD_WITH_VULKAN)
							size_t heapUsage = 0;
							size_t heapBudget = 0;

							VulkanApi::GetInstance()->GetMainDevice()->GetOccupiedVideoMemory(VkMemoryHeapFlagBits::VK_MEMORY_HEAP_DEVICE_LOCAL_BIT, heapBudget, heapUsage);

							m_stats.m_gpuHeapUsage = heapUsage;
							m_stats.m_gpuHeapBudget = heapBudget;
							m_stats.m_numSubmittedCommandBuffers = m_driverInstance->GetNumSubmittedCommandBuffers();
							UpdateMemoryStats();
#endif // SAILOR_BUILD_WITH_VULKAN
						}
					}
					else
					{
						if (submissionBeginState->m_context)
						{
							submissionBeginState->m_context->InvalidateSubmissionResources();
						}
						m_stats.m_gpuFps.store(0u, std::memory_order_relaxed);
						m_stats.m_numBatches.store(0u, std::memory_order_relaxed);
						m_stats.m_numInstances.store(0u, std::memory_order_relaxed);
					}
				}
				if (submissionBeginState->m_bMaterialCaptureActive)
				{
					RHIMaterial::EndSubmissionVersionCapture(submissionId);
					submissionBeginState->m_bMaterialCaptureActive = false;
				}

				rhiSceneView->CompleteSubmissionResources(
					bSubmissionResourcesSucceeded);

				{
					SAILOR_PROFILE_SCOPE("Clear after Present");

					rhiSceneView->Clear();

					auto& list = m_cachedSceneViews.At_Lock(rhiSceneView->m_world);
					auto it = list.FindIf([&](const auto& el) { return el.m_first == rhiSceneView; });
					if (it != list.end())
					{
						(*it).m_second = true;
					}

					m_cachedSceneViews.Unlock(rhiSceneView->m_world);

					GetDriver()->CollectGarbage_RenderThread();
				}
			}, Sailor::EThreadType::Render);

		auto prepareRenderFrame = rhiFrameGraph->Prepare(rhiSceneView);

		for (auto& t : prepareRenderFrame)
		{
			renderFrame1->Join(t);
		}

		renderFrame1->Run();
		for (auto& t : prepareRenderFrame)
		{
			t->Run();
		}

		m_previousRenderFrame = renderFrame1;
	}

	return true;
}

void Renderer::WaitIdle()
{
	if (m_previousRenderFrame.IsValid())
	{
		m_previousRenderFrame->Wait();
		m_previousRenderFrame.Clear();
	}

	if (m_driverInstance)
	{
		m_driverInstance->WaitIdle();
	}
}
