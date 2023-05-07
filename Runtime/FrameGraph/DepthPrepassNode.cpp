#include "DepthPrepassNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/Types.h"
#include "RHI/Batch.hpp"
#include "RHI/VertexDescription.h"

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* DepthPrepassNode::m_name = "DepthPrepass";
#endif

RHI::RHIMaterialPtr DepthPrepassNode::GetOrAddDepthMaterial(RHI::RHIVertexDescriptionPtr vertexDescription)
{
	auto& material = m_depthOnlyMaterials[vertexDescription->GetVertexAttributeBits()];

	if (!material)
	{
		auto shaderUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/DepthOnly.shader");
		ShaderSetPtr pShader;

		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderUID->GetUID(), pShader))
		{
			RenderState renderState = RHI::RenderState(true, true, 0.0f, false, ECullMode::Back, EBlendMode::None, EFillMode::Fill, GetHash(std::string("DepthOnly")), true);
			material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, renderState, pShader);
		}
	}

	return material;
}

RHI::ESortingOrder DepthPrepassNode::GetSortingOrder() const
{
	const std::string& sortOrder = GetString("Sorting");

	if (!sortOrder.empty())
	{
		return magic_enum::enum_cast<RHI::ESortingOrder>(sortOrder).value_or(RHI::ESortingOrder::FrontToBack);
	}

	return RHI::ESortingOrder::FrontToBack;
}

void DepthPrepassNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	const std::string QueueTag = GetString("Tag");
	const size_t QueueTagHash = GetHash(QueueTag);

	auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	TDrawCalls<DepthPrepassNode::PerInstanceData> drawCalls;
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
			auto depthMaterial = GetOrAddDepthMaterial(mesh->m_vertexDescription);

			if (proxy.GetMaterials()[i]->GetRenderState().IsRequiredCustomDepthShader())
			{
				// TODO: Fix custom depth shader
				// We don't support that yet
				//depthMaterial = proxy.GetMaterials()[i];
				continue;
			}

			const bool bIsDepthMaterialReady = depthMaterial &&
				depthMaterial->GetVertexShader() &&
				depthMaterial->GetFragmentShader() &&
				depthMaterial->GetRenderState().IsEnabledZWrite();

			if (!bIsDepthMaterialReady)
			{
				continue;
			}

			if (proxy.GetMaterials()[i]->GetRenderState().GetTag() == QueueTagHash)
			{
				DepthPrepassNode::PerInstanceData data;
				data.model = proxy.m_worldMatrix;

				RHIBatch batch(depthMaterial, mesh);

				drawCalls[batch][mesh].Add(data);
				batches.Insert(batch);

				numMeshes++;
			}
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	auto depthAttachment = GetRHIResource("depthStencil").StaticCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	if (numMeshes == 0)
	{
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::TransferDstOptimal);
		commands->ClearDepthStencil(commandList, depthAttachment, 0.0f);
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::TransferDstOptimal, depthAttachment->GetDefaultLayout());
		return;
	}

	SAILOR_PROFILE_BLOCK("Create storage for matrices");

	if (!m_perInstanceData || m_sizePerInstanceData < sizeof(DepthPrepassNode::PerInstanceData) * numMeshes)
	{
		m_perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(m_perInstanceData, "data", sizeof(DepthPrepassNode::PerInstanceData), numMeshes, 0);
		m_sizePerInstanceData = sizeof(DepthPrepassNode::PerInstanceData) * numMeshes;
	}

	RHI::RHIShaderBindingPtr storageBinding = m_perInstanceData->GetOrAddShaderBinding("data");
	SAILOR_PROFILE_END_BLOCK();

	TVector<DepthPrepassNode::PerInstanceData> gpuMatricesData;
	gpuMatricesData.AddDefault(numMeshes);
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

			memcpy(&gpuMatricesData[ssboIndex], matrices.GetData(), sizeof(DepthPrepassNode::PerInstanceData) * matrices.Num());

			if (!bIsInited)
			{
				storageIndex[j] = storageBinding->GetStorageInstanceIndex() + (uint32_t)ssboIndex;
				bIsInited = true;
			}
			ssboIndex += matrices.Num();
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Fill transfer command list with matrices data");
	if (gpuMatricesData.Num() > 0)
	{
		commands->UpdateShaderBinding(transferCommandList, storageBinding,
			gpuMatricesData.GetData(),
			sizeof(DepthPrepassNode::PerInstanceData) * gpuMatricesData.Num(),
			0);
	}
	SAILOR_PROFILE_END_BLOCK();

	const size_t numThreads = scheduler->GetNumRHIThreads() + 1;
	const size_t materialsPerThread = (batches.Num()) / numThreads;

	if (m_indirectBuffers.Num() < numThreads)
	{
		m_indirectBuffers.Resize(numThreads);
	}

	auto shaderBindingsByMaterial = [&](RHIMaterialPtr material)
	{
		TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, m_perInstanceData });

		if (material->GetRenderState().IsRequiredCustomDepthShader())
		{
			sets = TVector<RHIShaderBindingSetPtr>({ sceneView.m_frameBindings, sceneView.m_rhiLightsData, m_perInstanceData , material->GetBindings() });
		}
		return sets;
	};

	commands->BeginDebugRegion(commandList, std::string(GetName()) + " QueueTag:" + QueueTag, DebugContext::Color_CmdGraphics);

	commands->BeginRenderPass(commandList,
		TVector<RHI::RHITexturePtr>{},
		depthAttachment,
		glm::vec4(0, 0, depthAttachment->GetExtent().x, depthAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		true,
		glm::vec4(0.0f),
		0.0f,
		true,
		true);

	RHIRecordDrawCall(0, (uint32_t)vecBatches.Num(), vecBatches, commandList, shaderBindingsByMaterial, drawCalls, storageIndex, m_indirectBuffers[0],
		glm::ivec4(0, depthAttachment->GetExtent().y, depthAttachment->GetExtent().x, -depthAttachment->GetExtent().y),
		glm::uvec4(0, 0, depthAttachment->GetExtent().x, depthAttachment->GetExtent().y));

	commands->EndRenderPass(commandList);

	commands->EndDebugRegion(commandList);
}

void DepthPrepassNode::Clear()
{
	m_perInstanceData.Clear();
}