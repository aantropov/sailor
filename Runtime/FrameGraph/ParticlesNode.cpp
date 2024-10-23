#include "ParticlesNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include "RHI/VertexDescription.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Math/Noise.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;
using namespace Sailor::Framegraph::Experimental;

#ifndef _SAILOR_IMPORT_
const char* ParticlesNode::m_name = "ExperimentalParticles";
#endif

void ParticlesNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	auto assetRegistry = App::GetSubmodule<AssetRegistry>();
	auto modelImporter = App::GetSubmodule<ModelImporter>();

	std::string m_particlesPath;
	if (!m_particlesHeader.m_bIsLoaded && TryGetString("particlesData", m_particlesPath))
	{
		std::string yamlParticlesData;
		std::string dataPath = assetRegistry->GetContentFolder() + m_particlesPath;

		if (assetRegistry->ReadAllTextFile(dataPath, yamlParticlesData))
		{
			YAML::Node yamlNode = YAML::Load(yamlParticlesData);
			m_particlesHeader.Deserialize(yamlNode);
		}

		dataPath = Utils::RemoveFileExtension(dataPath) + "dat";

		if (assetRegistry->ReadBinaryFile(dataPath, m_particlesDataBinary))
		{
			m_particlesHeader.m_bIsLoaded = true;
		}
	}

	if (!m_particlesHeader.m_bIsLoaded)
	{
		return;
	}

	if (m_shadowMap == nullptr)
	{
		const auto usage = RHI::ETextureUsageBit::ColorAttachment_Bit |
			RHI::ETextureUsageBit::TextureTransferSrc_Bit |
			RHI::ETextureUsageBit::TextureTransferDst_Bit |
			RHI::ETextureUsageBit::Sampled_Bit;

		m_shadowMap = driver->CreateRenderTarget(glm::ivec2(4096, 4096), 1, RHI::EFormat::R32_SFLOAT, RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, usage);

		m_shadowMapBinding = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(m_shadowMapBinding, "shadowMapSampler", m_shadowMap, 0);
	}

	if (m_mesh == nullptr || m_material == nullptr || m_shadowMaterial == nullptr)
	{
		ModelPtr model;
		TVector<MaterialPtr> materials;

		std::string particleModel;
		if (!TryGetString("particleModel", particleModel))
		{
			return;
		}

		std::string particleShadowMaterial;
		if (!TryGetString("particleShadowMaterial", particleShadowMaterial))
		{
			return;
		}

		if (auto modelFileId = assetRegistry->GetAssetInfoPtr<ModelAssetInfoPtr>(particleModel))
		{
			modelImporter->LoadModel(modelFileId->GetFileId(), model);
			modelImporter->LoadDefaultMaterials(modelFileId->GetFileId(), materials);
		}

		MaterialPtr shadowCasterMaterial;
		if (auto materialFileId = assetRegistry->GetAssetInfoPtr<MaterialAssetInfoPtr>(particleShadowMaterial))
		{
			App::GetSubmodule<MaterialImporter>()->LoadMaterial(materialFileId->GetFileId(), shadowCasterMaterial);
		}

		m_mesh = model->IsReady() ? *model->GetMeshes().First() : nullptr;

		m_material = materials[0]->GetOrAddRHI(m_mesh->m_vertexDescription);
		m_shadowMaterial = shadowCasterMaterial->GetOrAddRHI(m_mesh->m_vertexDescription);
	}

	if (!m_pComputeShader)
	{
		if (auto shaderInfo = assetRegistry->GetAssetInfoPtr("Experimental/MeshParticles/ComputeParticles.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderInfo->GetFileId(), m_pComputeShader);
		}
	}

	if (m_mesh == nullptr || !m_mesh->IsReady() || !m_material.IsValid() || !m_material->IsReady() || !m_shadowMaterial.IsValid() || !m_shadowMaterial->IsReady())
	{
		return;
	}

	if (m_instances == nullptr)
	{
		TVector<PerInstanceData> instances;

		RHIShaderBindingPtr shaderBinding;
		if (m_material->GetBindings()->GetShaderBindings().ContainsKey("material"))
		{
			shaderBinding = m_material->GetBindings()->GetShaderBindings()["material"];
		}

		instances.Reserve(m_particlesHeader.m_n * m_particlesHeader.m_traceFrames);

		for (uint32_t i = 0; i < m_particlesHeader.m_n * m_particlesHeader.m_traceFrames; i++)
		{
			const uint32_t j = i / m_particlesHeader.m_traceFrames;

			PerInstanceData newInstance{};

			Math::Transform transform{};
			transform.m_position = vec4(m_particlesDataBinary[j].m_x2,
				m_particlesDataBinary[j].m_y2,
				m_particlesDataBinary[j].m_z2,
				1.0f);

			transform.m_scale = vec4((float)m_particlesDataBinary[j].m_size2);
			transform.m_scale.w = 0.0f;

			newInstance.model = transform.Matrix();
			newInstance.materialInstance = shaderBinding.IsValid() ? shaderBinding->GetStorageInstanceIndex() : 0;
			newInstance.color = vec4(1.0f);

			instances.Emplace(std::move(newInstance));
		}

		m_numInstances = (uint32_t)instances.Num();
		m_instances = driver->CreateBuffer_Immediate(instances.GetData(), instances.Num() * sizeof(PerInstanceData), EBufferUsageBit::StorageBuffer_Bit);
		m_particlesFrames = driver->CreateBuffer_Immediate(m_particlesDataBinary.GetData(), m_particlesDataBinary.Num() * sizeof(ParticleData), EBufferUsageBit::StorageBuffer_Bit);

		m_perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();

		Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(m_perInstanceData, m_instances, "data", 0);
		Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(m_perInstanceData, m_particlesFrames, "particlesData", 1);

		m_particlesDataBinary.Clear();
	}

	RHI::RHISurfacePtr colorAttachment = GetRHIResource("color").DynamicCast<RHI::RHISurface>();
	RHI::RHITexturePtr depthAttachment = GetRHIResource("depthStencil").DynamicCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	auto textureSamplers = App::GetSubmodule<TextureImporter>()->GetTextureSamplersBindingSet();
	TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, sceneView.m_rhiLightsData, m_perInstanceData, m_material->GetBindings(), m_shadowMapBinding, textureSamplers });
	TVector<RHIShaderBindingSetPtr> computeSets({ m_perInstanceData, sceneView.m_frameBindings });

	uvec4 numInstances = uvec4(m_numInstances, 0, 0, 0);

	RHI::RHIShaderPtr cmptShader = m_pComputeShader->GetComputeShaderRHI();
#ifdef _DEBUG
	cmptShader = m_pComputeShader->GetDebugComputeShaderRHI();
#endif

	ParticlesNode::PushConstants constants{};
	constants.m_numInstances = m_numInstances;
	constants.m_numFrames = m_particlesHeader.m_frames;
	constants.m_fps = m_particlesHeader.m_fps;
	constants.m_traceFrames = m_particlesHeader.m_traceFrames;
	constants.m_traceDecay = m_particlesHeader.m_traceDecay;

	commands->BeginDebugRegion(transferCommandList, GetName(), DebugContext::Color_CmdCompute);
	commands->Dispatch(transferCommandList, cmptShader, 256, 1, 1, computeSets, &constants, sizeof(constants));
	commands->EndDebugRegion(transferCommandList);

	// Shadows
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdGraphics);
	{
		commands->ImageMemoryBarrier(commandList, m_shadowMap, EImageLayout::ColorAttachmentOptimal);

		const auto viewport = glm::ivec4(0, m_shadowMap->GetExtent().y, m_shadowMap->GetExtent().x, -m_shadowMap->GetExtent().y);
		const auto scissors = glm::uvec4(0, 0, m_shadowMap->GetExtent().x, m_shadowMap->GetExtent().y);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{ m_shadowMap },
			nullptr,
			glm::vec4(0, 0, m_shadowMap->GetExtent().x, m_shadowMap->GetExtent().y),
			glm::ivec2(0, 0),
			true,
			glm::vec4(0.0f),
			0.0f,
			true,
			true);

		commands->BindMaterial(commandList, m_shadowMaterial);
		commands->SetViewport(commandList, (float)viewport.x, (float)viewport.y,
			(float)viewport.z,
			(float)viewport.w,
			glm::vec2(scissors.x, scissors.y),
			glm::vec2(scissors.z, scissors.w),
			0.0f, 1.0f);

		commands->BindShaderBindings(commandList, m_material, sets);
		commands->BindVertexBuffer(commandList, m_mesh->m_vertexBuffer, 0);
		commands->BindIndexBuffer(commandList, m_mesh->m_indexBuffer, 0);
		commands->DrawIndexed(commandList, m_mesh->GetIndexCount(), m_numInstances, m_mesh->GetFirstIndex(), m_mesh->GetVertexOffset(), 0);

		commands->EndRenderPass(commandList);
		commands->ImageMemoryBarrier(commandList, m_shadowMap, EImageLayout::ShaderReadOnlyOptimal);
	}
	//

	commands->BeginRenderPass(commandList,
		TVector<RHI::RHISurfacePtr>{ colorAttachment },
		depthAttachment,
		glm::vec4(0, 0, colorAttachment->GetTarget()->GetExtent().x, colorAttachment->GetTarget()->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		0.0f,
		true);

	const auto viewport = glm::ivec4(0, colorAttachment->GetTarget()->GetExtent().y, colorAttachment->GetTarget()->GetExtent().x, -colorAttachment->GetTarget()->GetExtent().y);
	const auto scissors = glm::uvec4(0, 0, colorAttachment->GetTarget()->GetExtent().x, colorAttachment->GetTarget()->GetExtent().y);

	commands->BindMaterial(commandList, m_material);
	commands->SetViewport(commandList, (float)viewport.x, (float)viewport.y,
		(float)viewport.z,
		(float)viewport.w,
		glm::vec2(scissors.x, scissors.y),
		glm::vec2(scissors.z, scissors.w),
		0.0f, 1.0f);

	commands->BindShaderBindings(commandList, m_material, sets);
	commands->BindVertexBuffer(commandList, m_mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, m_mesh->m_indexBuffer, 0);
	commands->DrawIndexed(commandList, m_mesh->GetIndexCount(), m_numInstances, m_mesh->GetFirstIndex(), m_mesh->GetVertexOffset(), 0);
	commands->EndRenderPass(commandList);

	commands->EndDebugRegion(commandList);
}

void ParticlesNode::Clear()
{
}
