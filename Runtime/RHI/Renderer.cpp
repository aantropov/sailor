#include "Types.h"
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

using namespace Sailor;
using namespace Sailor::RHI;

void IDelayedInitialization::TraceVisit(class TRefPtr<RHIResource> visitor, bool& bShouldRemoveFromList)
{
	bShouldRemoveFromList = false;

	if (auto fence = TRefPtr<RHI::RHIFence>(visitor.GetRawPtr()))
	{
		if (fence->IsFinished())
		{
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
		}
	}
}

bool IDelayedInitialization::IsReady() const
{
	return m_dependencies.Num() == 0;
}

Renderer::Renderer(Win32::Window const* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
{
	m_pViewport = pViewport;
	m_msaaSamples = msaaSamples;

#if defined(SAILOR_BUILD_WITH_VULKAN)
	m_driverInstance = TUniquePtr<Sailor::GraphicsDriver::Vulkan::VulkanGraphicsDriver>::Make();
	m_driverInstance->Initialize(pViewport, msaaSamples, bIsDebug);
#endif

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
}

Renderer::~Renderer()
{
	m_cachedSceneViews.Clear();
	Renderer::GetDriver()->WaitIdle();
	m_driverInstance.Clear();
}

void Renderer::MemoryStats()
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	auto driverInstance = App::GetSubmodule<Renderer>()->GetDriver().DynamicCast<Sailor::GraphicsDriver::Vulkan::VulkanGraphicsDriver>();

	const auto& internalMemoryAllocators = VulkanApi::GetInstance()->GetMainDevice()->GetMemoryAllocators();

	float texturesOccupiedSpace = 0.0f;
	for (const auto& allocator : internalMemoryAllocators)
	{
		texturesOccupiedSpace += allocator.m_second->GetOccupiedSpace() / (1024.0f * 1024.0f);
	}

	const auto& uniformBuffersMemoryAllocators = driverInstance->GetUniformBufferAllocators();

	float uniformBuffersOccupiedSpace = 0.0f;
	for (const auto& allocator : uniformBuffersMemoryAllocators)
	{
		uniformBuffersOccupiedSpace += allocator.m_second->GetOccupiedSpace() / (1024.0f * 1024.0f);
	}

	SAILOR_LOG("Memory consumption (GPU):");
	SAILOR_LOG("Materials: % 2.fmb", driverInstance->GetMaterialSsboAllocator()->GetOccupiedSpace() / (1024.0f * 1024.0f));
	SAILOR_LOG("General: % 2.fmb", driverInstance->GetGeneralSsboAllocator()->GetOccupiedSpace() / (1024.0f * 1024.0f));
	SAILOR_LOG("Meshes: % 2.fmb", driverInstance->GetMeshSsboAllocator()->GetOccupiedSpace() / (1024.0f * 1024.0f));
	SAILOR_LOG("Textures: % 2.fmb", texturesOccupiedSpace);
	SAILOR_LOG("UniformBuffers: % 2.fmb", uniformBuffersOccupiedSpace);

#endif
}

RHI::EFormat Renderer::GetColorFormat() const
{
	return Renderer::GetDriver()->GetBackBuffer()->GetFormat();
}

RHI::EFormat Renderer::GetDepthFormat() const
{
	return Renderer::GetDriver()->GetDepthBuffer()->GetFormat();
}

void Renderer::Clear()
{
	m_frameGraph.Clear();
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
	if (m_driverInstance->FixLostDevice(m_pViewport))
	{
		m_bFrameGraphOutdated = true;
	}
}

RHISceneViewPtr Renderer::GetOrAddSceneView(WorldPtr worldPtr)
{
	auto& res = m_cachedSceneViews.At_Lock(worldPtr);
	if (!res)
	{
		res = RHISceneViewPtr::Make();
	}
	m_cachedSceneViews.Unlock(worldPtr);

	return res;
}

void Renderer::RemoveSceneView(WorldPtr worldPtr)
{
	m_cachedSceneViews.Remove(worldPtr);
}

bool Renderer::PushFrame(const Sailor::FrameState& frame)
{
	SAILOR_PROFILE_BLOCK("Wait for render thread");

	if (m_bForceStop || App::GetSubmodule<Tasks::Scheduler>()->GetNumRenderingJobs() > MaxFramesInQueue)
	{
		return false;
	}

	SAILOR_PROFILE_END_BLOCK();
	if ((!m_frameGraph || m_bFrameGraphOutdated) && !m_pViewport->IsIconic())
	{
		m_frameGraph.Clear();

		if (auto frameGraphUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<AssetInfoPtr>("DefaultRenderer.renderer"))
		{
			App::GetSubmodule<FrameGraphImporter>()->Instantiate_Immediate(frameGraphUID->GetUID(), m_frameGraph);
		}

		m_bFrameGraphOutdated = false;
	}

	SAILOR_PROFILE_BLOCK("Copy scene view to render thread");
	auto world = frame.GetWorld();
	auto rhiSceneView = RHISceneViewPtr::Make();
	rhiSceneView->m_world = world;
	world->GetECS<StaticMeshRendererECS>()->CopySceneView(rhiSceneView);
	world->GetECS<CameraECS>()->CopyCameraData(rhiSceneView);
	world->GetECS<LightingECS>()->FillLightsData(rhiSceneView);

	rhiSceneView->m_deltaTime = frame.GetDeltaTime();
	rhiSceneView->m_currentTime = frame.GetWorld()->GetTime();

	rhiSceneView->m_drawImGui = frame.GetDrawImGuiTask();
	rhiSceneView->PrepareDebugDrawCommandLists(world);
	rhiSceneView->PrepareSnapshots();

	for (auto& snapshot : rhiSceneView->m_snapshots)
	{
		snapshot.m_sceneView = rhiSceneView;
	}

	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Push frame");

	auto preRenderingJob = Tasks::Scheduler::CreateTask("Trace command lists & Track RHI resources",
		[this]()
		{
			this->GetDriver()->TrackResources_ThreadSafe();
		}, Sailor::Tasks::EThreadType::Render);

	auto renderingJob = Tasks::Scheduler::CreateTask("Render Frame",
		[this, frame, rhiSceneView]() mutable
		{
			auto frameInstance = frame;
	static Utils::Timer timer;
	timer.Start();

	bool bRunCommandLists = false;
	TVector<RHI::RHICommandListPtr> primaryCommandLists;
	TVector<RHI::RHICommandListPtr> transferCommandLists;
	TVector<RHISemaphorePtr> waitFrameUpdate;

	static uint32_t totalFramesCount = 0U;

	SAILOR_PROFILE_BLOCK("Present Frame");

	if (m_driverInstance->AcquireNextImage())
	{
		if (!bRunCommandLists && !m_bFrameGraphOutdated && !m_pViewport->IsIconic())
		{
			auto rhiFrameGraph = m_frameGraph->GetRHI();

			rhiFrameGraph->SetRenderTarget("BackBuffer", m_driverInstance->GetBackBuffer());
			rhiFrameGraph->SetRenderTarget("DepthBuffer", m_driverInstance->GetDepthBuffer());
			rhiFrameGraph->Process(rhiSceneView, transferCommandLists, primaryCommandLists);

			SAILOR_PROFILE_BLOCK("Submit & Wait frame command list");
			for (uint32_t i = 0; i < frameInstance.NumCommandLists; i++)
			{
				if (auto pCommandList = frameInstance.GetCommandBuffer(i))
				{
					waitFrameUpdate.Add(GetDriver()->CreateWaitSemaphore());
					GetDriver()->SubmitCommandList(pCommandList, RHIFencePtr::Make(), *(waitFrameUpdate.end() - 1));
				}
			}

			for (auto& cmdList : transferCommandLists)
			{
				waitFrameUpdate.Add(GetDriver()->CreateWaitSemaphore());
				GetDriver()->SubmitCommandList(cmdList, RHIFencePtr::Make(), *(waitFrameUpdate.end() - 1));
			}

			SAILOR_PROFILE_END_BLOCK();

			bRunCommandLists = true;
		}

		if (m_driverInstance->PresentFrame(frame, primaryCommandLists, waitFrameUpdate))
		{
			totalFramesCount++;
			timer.Stop();

			if (timer.ResultAccumulatedMs() > 1000)
			{
				m_stats.m_gpuFps = totalFramesCount;
				totalFramesCount = 0;
				timer.Clear();
#if defined(SAILOR_BUILD_WITH_VULKAN)
				size_t heapUsage = 0;
				size_t heapBudget = 0;

				VulkanApi::GetInstance()->GetMainDevice()->GetOccupiedVideoMemory(VkMemoryHeapFlagBits::VK_MEMORY_HEAP_DEVICE_LOCAL_BIT, heapBudget, heapUsage);

				m_stats.m_gpuHeapUsage = heapUsage;
				m_stats.m_gpuHeapBudget = heapBudget;
				m_stats.m_numSubmittedCommandBuffers = m_driverInstance->GetNumSubmittedCommandBuffers();
#endif // SAILOR_BUILD_WITH_VULKAN
			}
		}
		else
		{
			m_stats.m_gpuFps = 0;
		}
	}

	SAILOR_PROFILE_END_BLOCK();

	rhiSceneView->Clear();
	GetDriver()->CollectGarbage_RenderThread();

		}, Sailor::Tasks::EThreadType::Render);

	renderingJob->Join(preRenderingJob);

	App::GetSubmodule<Tasks::Scheduler>()->Run(preRenderingJob);
	App::GetSubmodule<Tasks::Scheduler>()->Run(renderingJob);

	SAILOR_PROFILE_END_BLOCK();

	return true;
}
