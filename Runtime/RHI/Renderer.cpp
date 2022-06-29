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
#include "JobSystem/JobSystem.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "Components/TestComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Engine/World.h"

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
}

Renderer::~Renderer()
{
	Renderer::GetDriver()->WaitIdle();
	m_driverInstance.Clear();
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
	m_driverInstance->FixLostDevice(m_pViewport);
}

RHISceneViewPtr& Renderer::GetOrCreateSceneView(WorldPtr worldPtr)
{
	return m_cachedSceneViews[worldPtr];
}

void Renderer::RemoveSceneView(WorldPtr worldPtr)
{
	m_cachedSceneViews.Remove(worldPtr);
}

bool Renderer::PushFrame(const Sailor::FrameState& frame)
{
	SAILOR_PROFILE_BLOCK("Wait for render thread");

	if (m_bForceStop || App::GetSubmodule<JobSystem::Scheduler>()->GetNumRenderingJobs() > MaxFramesInQueue)
	{
		return false;
	}

	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Push frame");

	TSharedPtr<class JobSystem::ITask> preRenderingJob = JobSystem::Scheduler::CreateTask("Trace command lists & Track RHI resources",
		[this]()
	{
		this->GetDriver()->TrackResources_ThreadSafe();
	}, Sailor::JobSystem::EThreadType::Rendering);

	TSharedPtr<class JobSystem::ITask> renderingJob = JobSystem::Scheduler::CreateTask("Render Frame",
		[this, frame]()
	{
		auto frameInstance = frame;
		static Utils::Timer timer;
		timer.Start();

		SAILOR_PROFILE_BLOCK("Submit & Wait frame command list");
		TVector<RHISemaphorePtr> waitFrameUpdate;
		for (uint32_t i = 0; i < frameInstance.NumCommandLists; i++)
		{
			if (auto pCommandList = frameInstance.GetCommandBuffer(i))
			{
				waitFrameUpdate.Add(GetDriver()->CreateWaitSemaphore());
				GetDriver()->SubmitCommandList(pCommandList, RHIFencePtr::Make(), waitFrameUpdate[i]);
			}
		}
		SAILOR_PROFILE_END_BLOCK();

		TVector<RHI::RHICommandListPtr> secondaryCommandLists;
		if (auto cmdList = DrawTestScene(frame))
		{
			secondaryCommandLists.Emplace(cmdList);
		}

		// Test Code
		if (TestComponentPtr testComponent = frame.GetWorld()->GetGameObjects()[0]->GetComponent<TestComponent>())
		{
			const auto& debugFrame = frame.GetDebugFrame();

			if (debugFrame.m_drawDebugMeshCmd)
			{
				if (debugFrame.m_signalSemaphore)
				{
					waitFrameUpdate.Add(debugFrame.m_signalSemaphore);
				}

				secondaryCommandLists.Add(debugFrame.m_drawDebugMeshCmd);
			}
		}

		do
		{
			static uint32_t totalFramesCount = 0U;

			SAILOR_PROFILE_BLOCK("Present Frame");

			if (m_driverInstance->PresentFrame(frame, nullptr, &secondaryCommandLists, waitFrameUpdate))
			{
				totalFramesCount++;
				timer.Stop();

				if (timer.ResultAccumulatedMs() > 1000)
				{
					m_pureFps = totalFramesCount;
					totalFramesCount = 0;
					timer.Clear();
				}
			}
			else
			{
				m_pureFps = 0;
			}

			SAILOR_PROFILE_END_BLOCK();

		} while (m_pViewport->IsIconic());

	}, Sailor::JobSystem::EThreadType::Rendering);

	renderingJob->Join(preRenderingJob);

	App::GetSubmodule<JobSystem::Scheduler>()->Run(preRenderingJob);
	App::GetSubmodule<JobSystem::Scheduler>()->Run(renderingJob);

	SAILOR_PROFILE_END_BLOCK();

	return true;
}

// Temporary
RHI::RHICommandListPtr Renderer::DrawTestScene(const Sailor::FrameState& frame)
{
	for (auto& gameObject : frame.GetWorld()->GetGameObjects())
	{
		TestComponentPtr testComponent = gameObject->GetComponent<TestComponent>();
		MeshRendererComponentPtr rendererComponent = gameObject->GetComponent<MeshRendererComponent>();

		if (testComponent && rendererComponent)
		{
			auto& perInstanceBinding = testComponent->GetPerInstanceBinding();
			auto pModel = rendererComponent->GetModel();

			if (!pModel || !pModel->IsReady())
			{
				continue;
			}

			SAILOR_PROFILE_BLOCK("Render meshes");
			auto cmdList = GetDriver()->CreateCommandList(true, false);
			GetDriverCommands()->BeginCommandList(cmdList, true);

			for (uint32_t index = 0; index < pModel->GetMeshes().Num(); index++)
			{
				if (index >= rendererComponent->GetMaterials().Num())
				{
					continue;
				}

				auto pMaterial = rendererComponent->GetMaterials()[index];

				if (!pMaterial || !pMaterial->IsReady())
				{
					continue;
				}

				SAILOR_PROFILE_BLOCK("Get data");
				auto& mesh = pModel->GetMeshes()[index];

				if (!testComponent->GetCulledMeshes().Contains(mesh))
				{
					//continue;
				}

				auto& material = pMaterial->GetOrAddRHI(mesh->m_vertexDescription);
				bool bIsMaterialReady = pMaterial && pMaterial->IsReady();
				SAILOR_PROFILE_END_BLOCK();

				if (bIsMaterialReady && pMaterial->IsReady() && material && material->m_vulkan.m_pipeline && perInstanceBinding && perInstanceBinding->m_vulkan.m_descriptorSet)
				{
					GetDriverCommands()->BindMaterial(cmdList, material);
					GetDriverCommands()->SetDefaultViewport(cmdList);

					GetDriverCommands()->BindVertexBuffers(cmdList, { mesh->m_vertexBuffer });
					GetDriverCommands()->BindIndexBuffer(cmdList, mesh->m_indexBuffer);

					// TODO: Parse missing descriptor sets
					TVector<RHIShaderBindingSetPtr> sets;
					if (testComponent->GetFrameBinding()->GetShaderBindings().Num())
					{
						sets.Add(testComponent->GetFrameBinding());
					}
					if (perInstanceBinding->GetShaderBindings().Num())
					{
						sets.Add(perInstanceBinding);
					}
					if (material->GetBindings()->GetShaderBindings().Num())
					{
						sets.Add(material->GetBindings());
					}

					GetDriverCommands()->BindShaderBindings(cmdList, material, sets);
					GetDriverCommands()->DrawIndexed(cmdList, mesh->m_indexBuffer, (uint32_t)mesh->m_indexBuffer->GetSize() / sizeof(uint32_t), 1, 0, 0, perInstanceBinding->GetOrCreateShaderBinding("data")->GetStorageInstanceIndex());
				}
				SAILOR_PROFILE_END_BLOCK();
			}

			GetDriverCommands()->EndCommandList(cmdList);
			return cmdList;
		}
	}

	return nullptr;
}