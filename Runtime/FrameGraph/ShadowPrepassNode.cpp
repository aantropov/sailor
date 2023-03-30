#include "ShadowPrepassNode.h"
#include "RHI/Batch.hpp"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* ShadowPrepassNode::m_name = "ShadowPrepass";
#endif

RHI::RHIMaterialPtr ShadowPrepassNode::GetOrAddShadowMaterial(RHI::RHIVertexDescriptionPtr vertexDescription)
{
	auto& material = m_shadowMaterials.At_Lock(vertexDescription->GetVertexAttributeBits());

	if (!material)
	{
		auto shaderUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/Shadow.shader");
		ShaderSetPtr pShader;

		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderUID->GetUID(), pShader))
		{
			RenderState renderState = RHI::RenderState(true, true, 0.0f, false, ECullMode::Back, EBlendMode::None, EFillMode::Fill, GetHash(std::string("Shadow")), false);
			material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, renderState, pShader);
		}
	}
	m_shadowMaterials.Unlock(vertexDescription->GetVertexAttributeBits());

	return material;
}

void ShadowPrepassNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	if (sceneView.m_directionalLights.Num() == 0)
	{
		return;
	}

	auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	if (!m_shadowMap)
	{
		m_shadowMap = driver->CreateRenderTarget(glm::ivec2(1024, 1024), 1, RHI::EFormat::D32_SFLOAT, ETextureFiltration::Linear, ETextureClamping::Clamp,
			RHI::ETextureUsageBit::DepthStencilAttachment_Bit |
			RHI::ETextureUsageBit::TextureTransferSrc_Bit |
			RHI::ETextureUsageBit::TextureTransferDst_Bit |
			RHI::ETextureUsageBit::Sampled_Bit);

		driver->SetDebugName(m_shadowMap, "Shadow Map");
	}

	TDrawCalls<ShadowPrepassNode::PerInstanceData> drawCalls;
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
			auto depthMaterial = GetOrAddShadowMaterial(mesh->m_vertexDescription);

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

			ShadowPrepassNode::PerInstanceData data;
			data.model = proxy.m_worldMatrix;

			RHIBatch batch(depthMaterial, mesh);

			drawCalls[batch][mesh].Add(data);
			batches.Insert(batch);

			numMeshes++;
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	if (numMeshes == 0)
	{
		return;
	}

	SAILOR_PROFILE_BLOCK("Create storage for matrices");

	if (!m_perInstanceData || m_sizePerInstanceData < sizeof(ShadowPrepassNode::PerInstanceData) * numMeshes)
	{
		m_perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(m_perInstanceData, "data", sizeof(ShadowPrepassNode::PerInstanceData), numMeshes, 0);
		m_sizePerInstanceData = sizeof(ShadowPrepassNode::PerInstanceData) * numMeshes;
	}

	RHI::RHIShaderBindingPtr storageBinding = m_perInstanceData->GetOrAddShaderBinding("data");
	SAILOR_PROFILE_END_BLOCK();

	TVector<ShadowPrepassNode::PerInstanceData> gpuMatricesData;
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

			memcpy(&gpuMatricesData[ssboIndex], matrices.GetData(), sizeof(ShadowPrepassNode::PerInstanceData) * matrices.Num());

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
			sizeof(ShadowPrepassNode::PerInstanceData) * gpuMatricesData.Num(),
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
		return sets;
	};

	SAILOR_PROFILE_BLOCK("Record draw calls in primary command list");
	commands->BeginRenderPass(commandList,
		TVector<RHI::RHITexturePtr>{},
		m_shadowMap,
		glm::vec4(0, 0, m_shadowMap->GetExtent().x, m_shadowMap->GetExtent().y),
		glm::ivec2(0, 0),
		true,
		glm::vec4(0.0f),
		false,
		true);

	auto& defaultDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4>();

	const mat4 proj = glm::ortho(-1024.0f, 1024.0f, -1024.0f, 1024.0f, 50000.0f, 0.0f);
	const mat4 lightMatrix = proj * sceneView.m_directionalLights[0].m_lightMatrix;
	commands->PushConstants(commandList, GetOrAddShadowMaterial(defaultDescription), 64, &lightMatrix);
	RHIRecordDrawCall(0, (uint32_t)vecBatches.Num(), vecBatches, commandList, shaderBindingsByMaterial, drawCalls, storageIndex, m_indirectBuffers[0],
		glm::ivec4(0, m_shadowMap->GetExtent().y, m_shadowMap->GetExtent().x, -m_shadowMap->GetExtent().y),
		glm::uvec4(0,0, m_shadowMap->GetExtent().x, m_shadowMap->GetExtent().y));

	commands->EndRenderPass(commandList);
	SAILOR_PROFILE_END_BLOCK();
}

void ShadowPrepassNode::Clear()
{
	m_perInstanceData.Clear();
}
