#include "RenderSceneNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* RenderSceneNode::m_name = "RenderScene";
#endif

RHI::ESortingOrder RenderSceneNode::GetSortingOrder() const
{
	const std::string& sortOrder = GetStringParam("Sorting");

	if (!sortOrder.empty())
	{
		return magic_enum::enum_cast<RHI::ESortingOrder>(sortOrder).value_or(RHI::ESortingOrder::FrontToBack);
	}

	return RHI::ESortingOrder::FrontToBack;
}

class PerInstanceData
{
public:
	alignas(16) glm::mat4 model;
	alignas(16) uint32_t materialInstance = 0;

	bool operator==(const PerInstanceData& rhs) const { return this->materialInstance == rhs.materialInstance && this->model == rhs.model; }

	size_t GetHash() const
	{
		hash<glm::mat4> p;
		return p(model);
	}
};

class Batch
{
public:

	RHIMaterialPtr m_material;
	RHIMeshPtr m_mesh;

	Batch() = default;
	Batch(const RHIMaterialPtr& material, const RHIMeshPtr& mesh) : m_material(material), m_mesh(mesh) {}
	
	bool operator==(const Batch& rhs) const
	{
		return
			m_material->GetBindings()->GetCompatibilityHashCode() == rhs.m_material->GetBindings()->GetCompatibilityHashCode() &&
			m_material->GetVertexShader() == rhs.m_material->GetVertexShader() &&
			m_material->GetFragmentShader() == rhs.m_material->GetFragmentShader() &&
			m_material->GetRenderState() == rhs.m_material->GetRenderState() &&
			m_mesh->m_vertexBuffer->GetCompatibilityHashCode() == rhs.m_mesh->m_vertexBuffer->GetCompatibilityHashCode() &&
			m_mesh->m_indexBuffer->GetCompatibilityHashCode() == rhs.m_mesh->m_indexBuffer->GetCompatibilityHashCode();
	}

	size_t GetHash() const
	{
		size_t hash = m_material->GetBindings()->GetCompatibilityHashCode();
		HashCombine(hash, m_mesh->m_vertexBuffer->GetCompatibilityHashCode(), m_mesh->m_indexBuffer->GetCompatibilityHashCode());
		return hash;
	}
};

void RecordDrawCall(uint32_t start,
	uint32_t end,
	const TVector<Batch>& vecBatches,
	RHI::RHICommandListPtr cmdList,
	const RHI::RHISceneViewSnapshot& sceneView,
	RHI::RHIShaderBindingSetPtr perInstanceData,
	const TMap<Batch, TMap<RHI::RHIMeshPtr, TVector<PerInstanceData>>>& drawCalls,
	const TVector<uint32_t>& storageIndex)
{
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	for (uint32_t j = start; j < end; j++)
	{
		auto& material = vecBatches[j].m_material;
		auto& mesh = vecBatches[j].m_mesh;

		TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, perInstanceData, material->GetBindings() });

		commands->BindMaterial(cmdList, material);
		commands->BindShaderBindings(cmdList, material, sets);

		commands->BindVertexBuffer(cmdList, mesh->m_vertexBuffer, 0);
		commands->BindIndexBuffer(cmdList, mesh->m_indexBuffer, 0);

		TVector<RHI::DrawIndexedIndirectData> drawIndirect;
		drawIndirect.Reserve(drawCalls[vecBatches[j]].Num());

		uint32_t ssboOffset = 0;
		for (auto& instancedDrawCall : drawCalls[vecBatches[j]])
		{
			auto& mesh = instancedDrawCall.First();
			auto& matrices = instancedDrawCall.Second();

			RHI::DrawIndexedIndirectData data{};
			data.m_indexCount = (uint32_t)mesh->m_indexBuffer->GetSize() / sizeof(uint32_t);
			data.m_instanceCount = (uint32_t)matrices.Num();
			data.m_firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
			data.m_vertexOffset = mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();
			data.m_firstInstance = storageIndex[j] + ssboOffset;

			drawIndirect.Emplace(std::move(data));

			ssboOffset += (uint32_t)matrices.Num();
		}

		RHIBufferPtr indirectCommandBuffer = driver->CreateIndirectBuffer(drawIndirect.GetData(), sizeof(RHI::DrawIndexedIndirectData) * drawIndirect.Num());
		commands->DrawIndexedIndirect(cmdList, indirectCommandBuffer, 0, (uint32_t)drawIndirect.Num(), sizeof(RHI::DrawIndexedIndirectData));
	}
}
/*
https://developer.nvidia.com/vulkan-shader-resource-binding
*/
void RenderSceneNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	TMap<Batch, TMap<RHI::RHIMeshPtr, TVector<PerInstanceData>>> drawCalls;
	TSet<Batch> batches;

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

			if (material->GetRenderState().GetTag() == GetHash(GetStringParam("Tag")))
			{
				RHIShaderBindingPtr shaderBinding;
				if (material->GetBindings()->GetShaderBindings().ContainsKey("material"))
				{
					shaderBinding = material->GetBindings()->GetShaderBindings()["material"];
				}

				PerInstanceData data;
				data.model = proxy.m_worldMatrix;
				data.materialInstance = shaderBinding.IsValid() ? shaderBinding->GetStorageInstanceIndex() : 0;

				Batch batch(material, mesh);

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
	RHI::RHIShaderBindingSetPtr perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
	RHI::RHIShaderBindingPtr storageBinding = Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(perInstanceData, "data", sizeof(PerInstanceData), numMeshes, 0);
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Prepare command list");
	TVector<PerInstanceData> gpuMatricesData;
	gpuMatricesData.AddDefault(numMeshes);

	auto colorAttachment = GetResourceParam("color").StaticCast<RHI::RHITexture>();
	if (!colorAttachment)
	{
		colorAttachment = frameGraph->GetRenderTarget("BackBuffer");
	}

	auto depthAttachment = GetResourceParam("depthStencil").StaticCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	const size_t numThreads = scheduler->GetNumRHIThreads() + 1;
	const size_t materialsPerThread = (batches.Num()) / numThreads;

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
			commands->BeginSecondaryCommandList(cmdList, true, true);
			commands->SetDefaultViewport(cmdList);
			RecordDrawCall(start, end, vecBatches, cmdList, sceneView, perInstanceData, drawCalls, storageIndex);
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

	commands->ImageMemoryBarrier(commandList, colorAttachment, colorAttachment->GetFormat(), colorAttachment->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

	SAILOR_PROFILE_BLOCK("Record draw calls in primary command list");
	commands->BeginRenderPass(commandList,
		TVector<RHI::RHITexturePtr>{ colorAttachment },
		depthAttachment,
		glm::vec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		true,
		false);

	RecordDrawCall((uint32_t)secondaryCommandLists.Num() * (uint32_t)materialsPerThread, (uint32_t)vecBatches.Num(), vecBatches, commandList, sceneView, perInstanceData, drawCalls, storageIndex);
	commands->EndRenderPass(commandList);
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Wait for secondary command lists");
	for (auto& task : tasks)
	{
		task->Wait();
	}
	SAILOR_PROFILE_END_BLOCK();

	commands->RenderSecondaryCommandBuffers(commandList,
		secondaryCommandLists,
		TVector<RHI::RHITexturePtr>{ colorAttachment },
		depthAttachment,
		glm::vec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		true,
		false);

	commands->ImageMemoryBarrier(commandList, colorAttachment, colorAttachment->GetFormat(), EImageLayout::ColorAttachmentOptimal, colorAttachment->GetDefaultLayout());

	SAILOR_PROFILE_END_BLOCK();
}

void RenderSceneNode::Clear()
{
}