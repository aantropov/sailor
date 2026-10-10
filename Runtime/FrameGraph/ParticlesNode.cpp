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
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "Math/Noise.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;
using namespace Sailor::Framegraph::Experimental;

std::span<const StringHash> ParticlesNode::GetMsaaOutputs() const
{
	static const StringHash outputs[] = { "color"_h };
	return outputs;
}

bool ParticlesNode::InitializeBuffers(const TVector<PerInstanceData>& instances)
{
	auto& driver = Renderer::GetDriver();
	auto instanceBuffer = driver->CreateBuffer_Immediate(instances.GetData(), instances.Num() * sizeof(PerInstanceData), EBufferUsageBit::StorageBuffer_Bit);
	if (!instanceBuffer) return false;
	auto framesBuffer = driver->CreateBuffer_Immediate(m_particlesDataBinary.GetData(), m_particlesDataBinary.Num() * sizeof(ParticleData), EBufferUsageBit::StorageBuffer_Bit);
	if (!framesBuffer) return false;

	auto bindings = driver->CreateShaderBindings();
	driver->AddBufferToShaderBindings(bindings, instanceBuffer, "data"_h, 0);
	driver->AddBufferToShaderBindings(bindings, framesBuffer, "particlesData"_h, 1);

	m_instances = std::move(instanceBuffer);
	m_particlesFrames = std::move(framesBuffer);
	m_perInstanceData = std::move(bindings);
	m_numInstances = (uint32_t)instances.Num();
	m_particlesDataBinary.Clear();
	return true;
}

void ParticlesNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	auto assetRegistry = App::GetSubmodule<AssetRegistry>();
	auto modelImporter = App::GetSubmodule<ModelImporter>();

	std::string m_particlesPath;
	if (!m_particlesHeader.m_bIsLoaded && TryGetString("particlesData"_h, m_particlesPath))
	{
		std::string yamlParticlesData;
		if (assetRegistry->ReadContentText(m_particlesPath, yamlParticlesData))
		{
			YAML::Node yamlNode = YAML::Load(yamlParticlesData);
			m_particlesHeader.Deserialize(yamlNode);
		}

		const std::string dataPath = Utils::RemoveFileExtension(m_particlesPath) + ".dat";
		if (assetRegistry->ReadContentBinary(dataPath, m_particlesDataBinary))
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
		if (!m_shadowMap) return;

		m_shadowMapBinding = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(m_shadowMapBinding, "shadowMapSampler"_h, m_shadowMap, 0);
	}

	if (m_mesh == nullptr || m_material == nullptr || m_shadowMaterial == nullptr)
	{
		ModelPtr model;
		TVector<MaterialPtr> materials;

		std::string particleModel;
		if (!TryGetString("particleModel"_h, particleModel))
		{
			return;
		}

		std::string particleShadowMaterial;
		if (!TryGetString("particleShadowMaterial"_h, particleShadowMaterial))
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

		if (!model || !model->IsReady())
		{
			return;
		}

		const auto& meshes = model->GetMeshes();
		if (meshes.IsEmpty() || !meshes[0] ||
			materials.IsEmpty() || !materials[0] ||
			!shadowCasterMaterial)
		{
			return;
		}

		m_mesh = meshes[0];

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
		if (m_material->GetBindings()->GetShaderBindings().ContainsKey("material"_h))
		{
			shaderBinding = m_material->GetBindings()->GetShaderBindings()["material"_h];
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

		if (!InitializeBuffers(instances)) return;
	}

	const auto colorSurface = GetRHIResource("color"_h, frameGraph.GetRawPtr()).DynamicCast<RHISurface>();
	const auto colorAttachment = GetTargetAttachment("color"_h, frameGraph.GetRawPtr());
	const auto colorResolve = colorSurface && colorSurface->NeedsResolve() ? colorSurface->GetResolved() : RHITexturePtr{};
	auto depthResource = GetRHIResource("depthStencil"_h, frameGraph.GetRawPtr());
	if (!depthResource) depthResource = frameGraph->GetResource("DepthBuffer"_h);
	const auto depthSurface = depthResource.DynamicCast<RHISurface>();
	RHITexturePtr depthAttachment = depthSurface ? depthSurface->GetResolved() : depthResource.DynamicCast<RHITexture>();
	RHITexturePtr depthResolve;
	if (depthSurface && depthSurface->NeedsResolve() && (!colorSurface || colorSurface->NeedsResolve()))
	{
		depthResolve = depthAttachment;
		depthAttachment = depthSurface->GetTarget();
	}
	if (!colorAttachment) return;

	TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, sceneView.m_rhiLightsData, m_perInstanceData, m_material->GetBindings(), m_shadowMapBinding });
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

		if (!commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{ m_shadowMap },
			nullptr,
			glm::vec4(0, 0, m_shadowMap->GetExtent().x, m_shadowMap->GetExtent().y),
			glm::ivec2(0, 0),
			true,
			glm::vec4(0.0f),
			0.0f,
			true,
			true))
		{
			commands->EndDebugRegion(commandList);
			return;
		}

		commands->BindMaterial(commandList, m_shadowMaterial);
		commands->SetViewport(commandList, (float)viewport.x, (float)viewport.y,
			(float)viewport.z,
			(float)viewport.w,
			glm::vec2(scissors.x, scissors.y),
			glm::vec2(scissors.z, scissors.w),
			0.0f, 1.0f);

		if (commands->BindShaderBindings(commandList, m_shadowMaterial, sets))
		{
			commands->BindVertexBuffer(commandList, m_mesh->m_vertexBuffer, 0);
			commands->BindIndexBuffer(commandList, m_mesh->m_indexBuffer, 0);
			commands->DrawIndexed(commandList, m_mesh->GetIndexCount(), m_numInstances, m_mesh->GetFirstIndex(), m_mesh->GetVertexOffset(), 0);
			RecordDrawCallStats(m_numInstances);
		}

		commands->EndRenderPass(commandList);
		commands->ImageMemoryBarrier(commandList, m_shadowMap, EImageLayout::ShaderReadOnlyOptimal);
	}
	//

	commands->ImageMemoryBarrier(commandList, colorAttachment, EImageLayout::ColorAttachmentOptimal);
	if (colorResolve) commands->ImageMemoryBarrier(commandList, colorResolve, EImageLayout::ColorAttachmentOptimal);
	if (depthAttachment)
	{
		const auto depthLayout = IsDepthStencilFormat(depthAttachment->GetFormat()) ?
			EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal;
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthLayout);
		if (depthResolve) commands->ImageMemoryBarrier(commandList, depthResolve, depthLayout);
	}
	if (!commands->BeginRenderPass(commandList,
		TVector<RHITexturePtr>{ colorAttachment },
		TVector<RHITexturePtr>{ colorResolve },
		depthAttachment,
		depthResolve,
		glm::vec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		0.0f,
		!colorSurface || colorSurface->NeedsResolve(),
		true))
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	const auto viewport = glm::ivec4(0, colorAttachment->GetExtent().y, colorAttachment->GetExtent().x, -colorAttachment->GetExtent().y);
	const auto scissors = glm::uvec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y);

	commands->BindMaterial(commandList, m_material);
	commands->SetViewport(commandList, (float)viewport.x, (float)viewport.y,
		(float)viewport.z,
		(float)viewport.w,
		glm::vec2(scissors.x, scissors.y),
		glm::vec2(scissors.z, scissors.w),
		0.0f, 1.0f);

	if (commands->BindShaderBindings(commandList, m_material, sets))
	{
		commands->BindVertexBuffer(commandList, m_mesh->m_vertexBuffer, 0);
		commands->BindIndexBuffer(commandList, m_mesh->m_indexBuffer, 0);
		commands->DrawIndexed(commandList, m_mesh->GetIndexCount(), m_numInstances, m_mesh->GetFirstIndex(), m_mesh->GetVertexOffset(), 0);
		RecordDrawCallStats(m_numInstances);
	}
	commands->EndRenderPass(commandList);

	commands->EndDebugRegion(commandList);
}

void ParticlesNode::Clear()
{
}
