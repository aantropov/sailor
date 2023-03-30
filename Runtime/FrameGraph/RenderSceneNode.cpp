#include "RenderSceneNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include "RHI/Types.h"
#include "RHI/Batch.hpp"
#include "RHI/VertexDescription.h"
#include "RHI/CommandList.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* RenderSceneNode::m_name = "RenderScene";
#endif

RHI::ESortingOrder RenderSceneNode::GetSortingOrder() const
{
	const std::string& sortOrder = GetString("Sorting");

	if (!sortOrder.empty())
	{
		return magic_enum::enum_cast<RHI::ESortingOrder>(sortOrder).value_or(RHI::ESortingOrder::FrontToBack);
	}

	return RHI::ESortingOrder::FrontToBack;
}

/*
https://developer.nvidia.com/vulkan-shader-resource-binding
*/
void RenderSceneNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	const std::string QueueTag = GetString("Tag");
	const size_t QueueTagHash = GetHash(QueueTag);

	auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, std::string(GetName()) + " QueueTag:" + QueueTag, DebugContext::Color_CmdGraphics);

	TDrawCalls<PerInstanceData> drawCalls;
	TSet<RHIBatch> batches;

	uint32_t numMeshes = 0;

	SAILOR_PROFILE_BLOCK("Filter sceneView by tag");
	for (auto& proxy : sceneView.m_proxies)
	{
		for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
		{
			const bool bHasMaterial = proxy.GetMaterials().Num() > i;
			if (!bHasMaterial)
			{
				break;
			}

			const auto& mesh = proxy.m_meshes[i];
			const auto& material = proxy.GetMaterials()[i];

			const bool bIsMaterialReady = material &&
				material->GetVertexShader() &&
				material->GetFragmentShader() &&
				material->GetBindings() &&
				material->GetBindings()->GetShaderBindings().Num() > 0;

			if (!bIsMaterialReady)
			{
				continue;
			}

			if (material->GetRenderState().GetTag() == QueueTagHash)
			{
				RHIShaderBindingPtr shaderBinding;
				if (material->GetBindings()->GetShaderBindings().ContainsKey("material"))
				{
					shaderBinding = material->GetBindings()->GetShaderBindings()["material"];
				}

				PerInstanceData data;
				data.model = proxy.m_worldMatrix;
				data.materialInstance = shaderBinding.IsValid() ? shaderBinding->GetStorageInstanceIndex() : 0;

				RHIBatch batch(material, mesh);

				drawCalls[batch][mesh].Add(data);
				batches.Insert(batch);

				numMeshes++;
			}
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	if (numMeshes == 0)
	{
		return;
	}

	SAILOR_PROFILE_BLOCK("Create storage for matrices");
	if (!m_perInstanceData || m_sizePerInstanceData < sizeof(RenderSceneNode::PerInstanceData) * numMeshes)
	{
		m_perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(m_perInstanceData, "data", sizeof(RenderSceneNode::PerInstanceData), numMeshes, 0);
		m_sizePerInstanceData = sizeof(RenderSceneNode::PerInstanceData) * numMeshes;
	}

	RHI::RHIShaderBindingPtr storageBinding = m_perInstanceData->GetOrAddShaderBinding("data");
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Prepare command list");
	TVector<PerInstanceData> gpuMatricesData;
	gpuMatricesData.AddDefault(numMeshes);

	RHI::RHISurfacePtr colorAttachment = GetRHIResource("color").DynamicCast<RHI::RHISurface>();
	RHI::RHITexturePtr depthAttachment = GetRHIResource("depthStencil").DynamicCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	if (!colorAttachment)
	{
		return;
	}

	auto shaderBindingsByMaterial = [&](RHIMaterialPtr material)
	{
		TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, sceneView.m_rhiLightsData, m_perInstanceData, material->GetBindings() });
		return sets;
	};

	const size_t numThreads = scheduler->GetNumRHIThreads() + 1;
	const size_t materialsPerThread = (batches.Num()) / numThreads;

	if (m_indirectBuffers.Num() < numThreads)
	{
		m_indirectBuffers.Resize(numThreads);
	}

	TVector<RHICommandListPtr> secondaryCommandLists(batches.Num() > numThreads ? (numThreads - 1) : 0);
	TVector<Tasks::ITaskPtr> tasks;

	auto vecBatches = batches.ToVector();

	SAILOR_PROFILE_BLOCK("Calculate SSBO offsets");
	size_t ssboIndex = 0;
	TVector<uint32_t> storageIndex(vecBatches.Num());
	for (uint32_t j = 0; j < vecBatches.Num(); j++)
	{
		bool bIsInited = false;
		for (auto& instancedDrawCall : drawCalls[vecBatches[j]])
		{
			auto& mesh = instancedDrawCall.First();
			auto& matrices = instancedDrawCall.Second();

			memcpy(&gpuMatricesData[ssboIndex], matrices.GetData(), sizeof(PerInstanceData) * matrices.Num());

			if (!bIsInited)
			{
				storageIndex[j] = storageBinding->GetStorageInstanceIndex() + (uint32_t)ssboIndex;
				bIsInited = true;
			}
			ssboIndex += matrices.Num();
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Create secondary command lists");
	for (uint32_t i = 0; i < secondaryCommandLists.Num(); i++)
	{
		const uint32_t start = (uint32_t)materialsPerThread * i;
		const uint32_t end = (uint32_t)materialsPerThread * (i + 1);

		auto task = Tasks::Scheduler::CreateTask("Record draw calls in secondary command list",
			[&, i = i, start = start, end = end]()
		{
			RHICommandListPtr cmdList = driver->CreateCommandList(true, false);
			RHI::Renderer::GetDriver()->SetDebugName(cmdList, "Record draw calls in secondary command list");
			commands->BeginSecondaryCommandList(cmdList, true, true);
			RHIRecordDrawCall(start, end, vecBatches, cmdList, shaderBindingsByMaterial, drawCalls, storageIndex, m_indirectBuffers[i + 1]);
			commands->EndCommandList(cmdList);
			secondaryCommandLists[i] = std::move(cmdList);
		}, Tasks::EThreadType::RHI);

		task->Run();
		tasks.Add(task);
	}
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Fill transfer command list with matrices data");
	if (gpuMatricesData.Num() > 0)
	{
		commands->UpdateShaderBinding(transferCommandList, storageBinding,
			gpuMatricesData.GetData(),
			sizeof(PerInstanceData) * gpuMatricesData.Num(),
			0);
	}
	SAILOR_PROFILE_END_BLOCK();

	commands->ImageMemoryBarrier(commandList, colorAttachment->GetTarget(), colorAttachment->GetTarget()->GetFormat(), colorAttachment->GetTarget()->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

	if(batches.Num() > 0)
	{
		SAILOR_PROFILE_BLOCK("Record draw calls in primary command list");
		commands->BeginRenderPass(commandList,
			TVector<RHI::RHISurfacePtr>{ colorAttachment },
			depthAttachment,
			glm::vec4(0, 0, colorAttachment->GetTarget()->GetExtent().x, colorAttachment->GetTarget()->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			true);

		RHIRecordDrawCall((uint32_t)secondaryCommandLists.Num() * (uint32_t)materialsPerThread, 
			(uint32_t)vecBatches.Num(), 
			vecBatches, 
			commandList, 
			shaderBindingsByMaterial,
			drawCalls, 
			storageIndex, 
			m_indirectBuffers[0]);

		commands->EndRenderPass(commandList);
		SAILOR_PROFILE_END_BLOCK();
	}

	SAILOR_PROFILE_BLOCK("Wait for secondary command lists");
	for (auto& task : tasks)
	{
		task->Wait();
	}
	SAILOR_PROFILE_END_BLOCK();

	if (secondaryCommandLists.Num() > 0)
	{
		commands->RenderSecondaryCommandBuffers(commandList,
			secondaryCommandLists,
			TVector<RHI::RHISurfacePtr>{ colorAttachment },
			depthAttachment,
			glm::vec4(0, 0, colorAttachment->GetTarget()->GetExtent().x, colorAttachment->GetTarget()->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			true);
	}

	commands->ImageMemoryBarrier(commandList,
		colorAttachment->GetTarget(),
		colorAttachment->GetTarget()->GetFormat(),
		EImageLayout::ColorAttachmentOptimal,
		colorAttachment->GetTarget()->GetDefaultLayout());

	SAILOR_PROFILE_END_BLOCK();

	commands->EndDebugRegion(commandList);
}

void RenderSceneNode::Clear()
{
	m_indirectBuffers.Clear();
	m_perInstanceData.Clear();
}