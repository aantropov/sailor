#include "DepthPrepassNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/Types.h"
#include "RHI/Batch.hpp"
#include "RHI/VertexDescription.h"
#include "AssetRegistry/Texture/TextureImporter.h"

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
		auto shaderFileId = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/DepthOnly.shader");
		ShaderSetPtr pShader;

		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderFileId->GetFileId(), pShader) && pShader->IsReady())
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

Tasks::TaskPtr<void, void> DepthPrepassNode::Prepare(RHI::RHIFrameGraph* frameGraph, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	const std::string QueueTag = GetString("Tag");
	const size_t QueueTagHash = GetHash(QueueTag);

	Tasks::TaskPtr res = Tasks::CreateTask("Prepare DepthPrepassNode " + std::to_string(sceneView.m_frame),
		[=, &syncSharedResources = m_syncSharedResources, &sceneViewSnapshot = sceneView]() mutable {

			syncSharedResources.Lock();

			m_numMeshes = 0;
			m_drawCalls.Clear();
			m_batches.Clear();

			SAILOR_PROFILE_BLOCK("Filter sceneView by tag");
			for (auto& proxy : sceneViewSnapshot.m_proxies)
			{
				for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
				{
					const bool bHasMaterial = proxy.GetMaterials().Num() > i;
					if (!bHasMaterial)
					{
						break;
					}

					if (proxy.GetMaterials()[i]->GetRenderState().GetTag() != QueueTagHash)
					{
						continue;
					}

					const auto& mesh = proxy.m_meshes[i];

					auto depthMaterial = GetOrAddDepthMaterial(mesh->m_vertexDescription);

					const bool bRequiredCustomDepth = proxy.GetMaterials()[i]->GetRenderState().IsRequiredCustomDepthShader();
					if (bRequiredCustomDepth)
					{
						depthMaterial = proxy.GetMaterials()[i];
					}

					const bool bIsDepthMaterialReady = depthMaterial &&
						depthMaterial->GetVertexShader() &&
						depthMaterial->GetFragmentShader() &&
						depthMaterial->GetRenderState().IsEnabledZWrite();

					if (!bIsDepthMaterialReady)
					{
						continue;
					}

					DepthPrepassNode::PerInstanceData data;
					data.model = proxy.m_worldMatrix;
					data.bIsCulled = 0;
					data.sphereBounds = mesh->m_bounds.ToSphere().GetVec4();

					if (bRequiredCustomDepth)
					{
						RHIShaderBindingPtr shaderBinding;
						if (depthMaterial->GetBindings()->GetShaderBindings().ContainsKey("material"))
						{
							shaderBinding = depthMaterial->GetBindings()->GetShaderBindings()["material"];
						}
						data.materialInstance = shaderBinding.IsValid() ? shaderBinding->GetStorageInstanceIndex() : 0;
					}
					else
					{
						data.materialInstance = 0;
					}

					RHIBatch batch(depthMaterial, mesh);

					m_drawCalls[batch][mesh].Add(data);
					m_batches.Insert(batch);

					m_numMeshes++;
				}
			}
			SAILOR_PROFILE_END_BLOCK();

			syncSharedResources.Unlock();
		}, Tasks::EThreadType::RHI);

	return res;
}

void DepthPrepassNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	m_syncSharedResources.Lock();

	std::string temp;
	TryGetString("ClearDepth", temp);
	const bool bShouldClearDepth = temp == "true";

	temp.clear();
	TryGetString("GPUCulling", temp);
	const bool bGpuCullingEnabled = temp == "true";

	const std::string QueueTag = GetString("Tag");
	const size_t QueueTagHash = GetHash(QueueTag);

	auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	auto depthAttachment = GetRHIResource("depthStencil").StaticCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	if (bGpuCullingEnabled)
	{
		if (!m_pComputeMeshCullingShader)
		{
			if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeMeshCulling.shader"))
			{
				App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pComputeMeshCullingShader);
			}
		}

		if (!m_computeMeshCullingBindings.IsValid())
		{
			auto depthHighZ = GetResolvedAttachment("depthHighZ").StaticCast<RHI::RHITexture>();
			m_computeMeshCullingBindings = driver->CreateShaderBindings();
			driver->AddSamplerToShaderBindings(m_computeMeshCullingBindings, "depthHighZ", depthHighZ, 0);
		}
	}

	if (m_numMeshes == 0 || (bGpuCullingEnabled && !m_pComputeMeshCullingShader.IsValid()))
	{
		m_syncSharedResources.Unlock();
		return;
	}

	SAILOR_PROFILE_BLOCK("Create storage for matrices");

	if (!m_perInstanceData || m_sizePerInstanceData < sizeof(DepthPrepassNode::PerInstanceData) * m_numMeshes)
	{
		m_perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(m_perInstanceData, "data", sizeof(DepthPrepassNode::PerInstanceData), m_numMeshes, 0);
		m_sizePerInstanceData = sizeof(DepthPrepassNode::PerInstanceData) * m_numMeshes;
	}

	RHI::RHIShaderBindingPtr storageBinding = m_perInstanceData->GetOrAddShaderBinding("data");
	SAILOR_PROFILE_END_BLOCK();

	TVector<DepthPrepassNode::PerInstanceData> gpuMatricesData;
	gpuMatricesData.AddDefault(m_numMeshes);
	auto vecBatches = m_batches.ToVector();

	SAILOR_PROFILE_BLOCK("Calculate SSBO offsets");
	size_t ssboIndex = 0;
	TVector<uint32_t> storageIndex(vecBatches.Num());
	for (uint32_t j = 0; j < vecBatches.Num(); j++)
	{
		bool bIsInited = false;
		for (const auto& instancedDrawCall : m_drawCalls[vecBatches[j]])
		{
			auto& mesh = instancedDrawCall.First();
			auto& matrices = *instancedDrawCall.Second();

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
	const size_t materialsPerThread = (m_batches.Num()) / numThreads;

	if (m_indirectBuffers.Num() < numThreads)
	{
		m_indirectBuffers.Resize(numThreads);
		m_cullingIndirectBufferBinding.Resize(numThreads);

		for (uint32_t i = 0; i < numThreads; i++)
		{
			m_cullingIndirectBufferBinding[i] = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		}
	}

	auto textureSamplers = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
	auto shaderBindingsByMaterial = [&](RHIMaterialPtr material)
		{
			TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, m_perInstanceData });

			if (material->GetRenderState().IsRequiredCustomDepthShader())
			{
				sets = TVector<RHIShaderBindingSetPtr>({ sceneView.m_frameBindings, sceneView.m_rhiLightsData, m_perInstanceData , material->GetBindings(), textureSamplers });
			}
			return sets;
		};

	commands->BeginDebugRegion(commandList, std::string(GetName()) + " QueueTag:" + QueueTag, DebugContext::Color_CmdGraphics);

	commands->BeginRenderPass(commandList,
		TVector<RHI::RHITexturePtr>{},
		depthAttachment,
		glm::vec4(0, 0, depthAttachment->GetExtent().x, depthAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		bShouldClearDepth,
		glm::vec4(0.0f),
		0.0f,
		true,
		true);

	if (bGpuCullingEnabled)
	{
		auto cullingComputeShader = m_pComputeMeshCullingShader->GetComputeShaderRHI();

#ifdef _DEBUG
		cullingComputeShader = m_pComputeMeshCullingShader->GetDebugComputeShaderRHI();
#endif
		RHIRecordDrawCallGPUCulling(0, 
			(uint32_t)vecBatches.Num(), vecBatches, 
			commandList, transferCommandList, 
			shaderBindingsByMaterial, 
			m_drawCalls, 
			storageIndex, 
			m_indirectBuffers[0],
			glm::ivec4(0, depthAttachment->GetExtent().y, depthAttachment->GetExtent().x, -depthAttachment->GetExtent().y),
			glm::uvec4(0, 0, depthAttachment->GetExtent().x, depthAttachment->GetExtent().y),
			glm::vec2(0.0f, 1.0f), cullingComputeShader, m_cullingIndirectBufferBinding[0],
			{ m_computeMeshCullingBindings , m_perInstanceData, m_cullingIndirectBufferBinding[0], sceneView.m_frameBindings });
	}
	else
	{
		RHIRecordDrawCall(0, (uint32_t)vecBatches.Num(), vecBatches, commandList, transferCommandList, shaderBindingsByMaterial, m_drawCalls, storageIndex, m_indirectBuffers[0],
			glm::ivec4(0, depthAttachment->GetExtent().y, depthAttachment->GetExtent().x, -depthAttachment->GetExtent().y),
			glm::uvec4(0, 0, depthAttachment->GetExtent().x, depthAttachment->GetExtent().y));
	}
	commands->EndRenderPass(commandList);

	commands->EndDebugRegion(commandList);

	m_syncSharedResources.Unlock();
}

void DepthPrepassNode::Clear()
{
	m_perInstanceData.Clear();
}