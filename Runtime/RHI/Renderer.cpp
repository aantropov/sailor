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
#include "ECS/AnimationECS.h"

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

Renderer::Renderer(Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
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

void Renderer::BeginConditionalDestroy()
{
	m_previousRenderFrame.Clear();
	m_frameGraph.Clear();
	m_cachedSceneViews.Clear();
	Renderer::GetDriver()->WaitIdle();

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

bool Renderer::PushFrame(const Sailor::FrameState& frame)
{
	SAILOR_PROFILE_FUNCTION();

	if (m_bForceStop ||
		m_driverInstance->ShouldFixLostDevice(m_pViewport) ||
		App::GetSubmodule<Tasks::Scheduler>()->GetNumTasks(EThreadType::Render) > MaxFramesInQueue)
	{
		return false;
	}

	if (m_previousRenderFrame.IsValid() && m_previousRenderFrame->IsFinished())
	{
		m_previousRenderFrame.Clear();
	}

	if ((!m_frameGraph || m_bFrameGraphOutdated) && !m_pViewport->IsIconic())
	{
		m_frameGraph.Clear();

		if (auto frameGraphFileId = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<AssetInfoPtr>("DefaultRenderer.renderer"))
		{
			App::GetSubmodule<FrameGraphImporter>()->Instantiate_Immediate(frameGraphFileId->GetFileId(), m_frameGraph);
		}

		m_bFrameGraphOutdated = false;
	}

	WorldPtr world = frame.GetWorld();
	RHISceneViewPtr rhiSceneView;

	{
		SAILOR_PROFILE_SCOPE("Copy scene view to render thread");

		rhiSceneView = GetOrAddSceneView(world);

		rhiSceneView->m_world = world;
		world->GetECS<StaticMeshRendererECS>()->CopySceneView(rhiSceneView);
		world->GetECS<CameraECS>()->CopyCameraData(rhiSceneView);
		world->GetECS<LightingECS>()->FillLightingData(rhiSceneView);
		world->GetECS<AnimationECS>()->FillAnimationData(rhiSceneView);

		rhiSceneView->m_deltaTime = frame.GetDeltaTime();
		rhiSceneView->m_currentTime = frame.GetWorld()->GetTime();

		rhiSceneView->m_drawImGui = frame.GetDrawImGuiTask();
		rhiSceneView->PrepareDebugDrawCommandLists(world);
		rhiSceneView->PrepareSnapshots();
	}

	{
		SAILOR_PROFILE_SCOPE("Push frame");

		uint64_t currentFrame = world->GetCurrentFrame();
		auto rhiFrameGraph = m_frameGraph->GetRHI();

		auto renderFrame = Tasks::CreateTask("Trace command lists & Track RHI resources " + std::to_string(currentFrame),
			[this]() { this->GetDriver()->TrackResources_ThreadSafe(); }, Sailor::EThreadType::Render);

		auto renderFrame1 = Tasks::CreateTask("Render Frame " + std::to_string(currentFrame),
			[this, rhiFrameGraph = rhiFrameGraph, frame, rhiSceneView]() mutable
			{
				SAILOR_PROFILE_SCOPE("Render Frame");

				auto frameInstance = frame;
				static Utils::Timer timer;
				timer.Start();

				TVector<RHI::RHICommandListPtr> primaryCommandLists;
				TVector<RHI::RHICommandListPtr> transferCommandLists;
				TVector<RHISemaphorePtr> waitFrameUpdate;

				static uint32_t totalFramesCount = 0U;

				auto updateFrameRHI = [&frameInstance = frameInstance, &waitFrameUpdate = waitFrameUpdate]()
					{
						SAILOR_PROFILE_SCOPE("Submit & Wait frame command lists");
						for (uint32_t i = 0; i < frameInstance.NumCommandLists; i++)
						{
							if (auto pCommandList = frameInstance.GetCommandBuffer(i))
							{
								auto newWaitSemaphore = GetDriver()->CreateWaitSemaphore();
								GetDriver()->SetDebugName(newWaitSemaphore, std::format("frameInstance CommandBuffer {}", i));

								waitFrameUpdate.Add(newWaitSemaphore);
								GetDriver()->SubmitCommandList(pCommandList, RHIFencePtr::Make(), *(waitFrameUpdate.end() - 1));
							}
						}
					};

				if (m_driverInstance->AcquireNextImage())
				{
					RHISemaphorePtr chainSemaphore{};

					updateFrameRHI();

					if (!m_bFrameGraphOutdated && !m_pViewport->IsIconic())
					{
						rhiFrameGraph->SetRenderTarget("BackBuffer", m_driverInstance->GetBackBuffer());
						rhiFrameGraph->SetRenderTarget("DepthBuffer", m_driverInstance->GetDepthBuffer());

						RHISemaphorePtr signalSemaphore{};

						if (waitFrameUpdate.Num() > 0)
						{
							signalSemaphore = *(waitFrameUpdate.end() - 1);
							waitFrameUpdate.RemoveLast();
						}
						rhiFrameGraph->Process(rhiSceneView, transferCommandLists, primaryCommandLists, signalSemaphore, chainSemaphore);
					}

					{
						SAILOR_PROFILE_SCOPE("Submit transfer command lists");

						uint32_t i = 0;
						for (auto& cmdList : transferCommandLists)
						{
							auto newWaitSemaphore = GetDriver()->CreateWaitSemaphore();
							GetDriver()->SetDebugName(newWaitSemaphore, std::format("rhiFrameGraph TransferCommandList {}", i++));

							waitFrameUpdate.AddUnique(newWaitSemaphore);
							GetDriver()->SubmitCommandList(cmdList, RHIFencePtr::Make(), *(waitFrameUpdate.end() - 1), chainSemaphore);
						}
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
				else
				{
					updateFrameRHI();
				}

				{
					SAILOR_PROFILE_SCOPE("Clear after Present");

					rhiSceneView->Clear();

					auto& list = m_cachedSceneViews.At_Lock(rhiSceneView->m_world);
					auto it = list.FindIf([&](const auto& el) { return el.m_first == rhiSceneView; });
					if (it != list.end())
					{
						(*it).m_second = true;
					}
					else
					{
						check(false);
					}

					m_cachedSceneViews.Unlock(rhiSceneView->m_world);

					GetDriver()->CollectGarbage_RenderThread();
				}
			}, Sailor::EThreadType::Render);

		auto prepareRenderFrame = rhiFrameGraph->Prepare(rhiSceneView);

		for (auto& t : prepareRenderFrame)
		{
			t->Join(renderFrame);

			if (m_previousRenderFrame.IsValid())
			{
				renderFrame->Join(m_previousRenderFrame);
			}

			renderFrame1->Join(t);

		}
		renderFrame1->Join(renderFrame);

		renderFrame1->Run();
		renderFrame->Run();
		for (auto& t : prepareRenderFrame)
		{
			t->Run();
		}

		m_previousRenderFrame = renderFrame1;
	}

	return true;
}
