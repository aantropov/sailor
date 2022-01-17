#include "AssetRegistry/Material/MaterialImporter.h"

#include "AssetRegistry/UID.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "MaterialAssetInfo.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "Math/Math.h"
#include "Core/Utils.h"
#include "Memory/WeakPtr.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "RHI/Renderer.h"
#include "RHI/Material.h"
#include "RHI/Shader.h"
#include "RHI/Fence.h"
#include "RHI/CommandList.h"
#include "AssetRegistry/Texture/TextureImporter.h"

using namespace Sailor;

bool Material::IsReady() const
{
	return m_bIsReady;
}

JobSystem::ITaskPtr Material::OnHotReload()
{
	UpdateRHIResource();
	return nullptr;
}

void Material::ClearSamplers()
{
	for (auto& sampler : m_samplers)
	{
		sampler.m_second.Lock()->RemoveHotReloadDependentObject(sampler.m_second);
	}
	m_samplers.Clear();
}

void Material::ClearUniforms()
{
	m_uniforms.Clear();
}

void Material::SetSampler(const std::string& name, TexturePtr value)
{
	if (value)
	{
		m_samplers[name] = value;
	}
}

void Material::SetUniform(const std::string& name, glm::vec4 value)
{
	m_uniforms[name] = value;
}

void Material::UpdateRHIResource()
{
	SAILOR_LOG("Update material RHI resource: %s", GetUID().ToString().c_str());

	m_bIsReady = false;
	m_rhiMaterial = RHI::Renderer::GetDriver()->CreateMaterial(m_renderState, m_shader);

	auto bindings = m_rhiMaterial->GetBindings();

	for (auto& sampler : m_samplers)
	{
		if (bindings->HasBinding(sampler.m_first))
		{
			RHI::Renderer::GetDriver()->UpdateShaderBinding(bindings, sampler.m_first, sampler.m_second.Lock()->GetRHI());
		}
	}

	for (auto& uniform : m_uniforms)
	{
		if (bindings->HasParameter(uniform.m_first))
		{
			std::string outBinding;
			std::string outVariable;

			RHI::ShaderBindingSet::ParseParameter(uniform.m_first, outBinding, outVariable);
			RHI::ShaderBindingPtr& binding = bindings->GetOrCreateShaderBinding(outBinding);

			const glm::vec4 value = uniform.m_second;
			SAILOR_ENQUEUE_JOB_RENDER_THREAD_TRANSFER_CMD("Set material parameter", ([&binding, outVariable, value](RHI::CommandListPtr& cmdList)
				{
					RHI::Renderer::GetDriverCommands()->UpdateShaderBindingVariable(cmdList, binding, outVariable, &value, sizeof(value));
				}));
		}
	}

	m_bIsReady = true;
}

void MaterialAsset::Serialize(nlohmann::json& outData) const
{
	outData["enable_depth_test"] = m_pData->m_renderState.IsDepthTestEnabled();
	outData["enable_z_write"] = m_pData->m_renderState.IsEnabledZWrite();
	outData["depth_bias"] = m_pData->m_renderState.GetDepthBias();
	outData["cull_mode"] = m_pData->m_renderState.GetCullMode();
	outData["render_queue"] = GetRenderQueue();
	outData["is_transparent"] = IsTransparent();
	outData["blend_mode"] = m_pData->m_renderState.GetBlendMode();
	outData["fill_mode"] = m_pData->m_renderState.GetFillMode();
	outData["defines"] = m_pData->m_shaderDefines;

	SerializeArray(m_pData->m_samplers, outData["samplers"]);
	outData["uniforms"] = m_pData->m_uniformsVec4;

	m_pData->m_shader.Serialize(outData["shader"]);
}

void MaterialAsset::Deserialize(const nlohmann::json& outData)
{
	m_pData = TUniquePtr<GetData>::Make();

	bool bEnableDepthTest = true;
	bool bEnableZWrite = true;
	float depthBias = 0.0f;
	RHI::ECullMode cullMode = RHI::ECullMode::Back;
	RHI::EBlendMode blendMode = RHI::EBlendMode::None;
	RHI::EFillMode fillMode = RHI::EFillMode::Fill;
	std::string renderQueue = "Opaque";

	m_pData->m_shaderDefines.Clear();
	m_pData->m_uniformsVec4.Clear();

	if (outData.contains("enable_depth_test"))
	{
		bEnableDepthTest = outData["enable_depth_test"].get<bool>();
	}

	if (outData.contains("enable_z_write"))
	{
		bEnableZWrite = outData["enable_z_write"].get<bool>();
	}

	if (outData.contains("depth_bias"))
	{
		depthBias = outData["depth_bias"].get<float>();
	}

	if (outData.contains("cull_mode"))
	{
		cullMode = (RHI::ECullMode)outData["cull_mode"].get<uint8_t>();
	}

	if (outData.contains("fill_mode"))
	{
		fillMode = (RHI::EFillMode)outData["fill_mode"].get<uint8_t>();
	}

	if (outData.contains("render_queue"))
	{
		renderQueue = outData["render_queue"].get<std::string>();
	}

	if (outData.contains("is_transparent"))
	{
		m_pData->m_bIsTransparent = outData["is_transparent"].get<bool>();
	}

	if (outData.contains("blend_mode"))
	{
		blendMode = (RHI::EBlendMode)outData["blend_mode"].get<uint8_t>();
	}

	if (outData.contains("defines"))
	{
		for (auto& elem : outData["defines"])
		{
			m_pData->m_shaderDefines.Add(elem.get<std::string>());
		}
	}

	if (outData.contains("samplers"))
	{
		Sailor::DeserializeArray(m_pData->m_samplers, outData["samplers"]);
	}

	if (outData.contains("uniforms"))
	{
		for (auto& elem : outData["uniforms"])
		{
			auto first = elem[0].get<std::string>();
			auto second = elem[1].get<glm::vec4>();

			m_pData->m_uniformsVec4.Add({ first, second });
		}
	}

	if (outData.contains("shader"))
	{
		m_pData->m_shader.Deserialize(outData["shader"]);
	}

	m_pData->m_renderState = RHI::RenderState(bEnableDepthTest, bEnableZWrite, depthBias, cullMode, blendMode, fillMode);
}

MaterialImporter::MaterialImporter(MaterialAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	infoHandler->Subscribe(this);
}

MaterialImporter::~MaterialImporter()
{
	m_loadedMaterials.Clear();
}

void MaterialImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
}

void MaterialImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
	MaterialPtr material = GetLoadedMaterial(assetInfo->GetUID());
	if (bWasExpired && material)
	{
		// We need to start load the material
		if (auto pMaterialAsset = LoadMaterialAsset(assetInfo->GetUID()))
		{
			auto updateMaterial = JobSystem::Scheduler::CreateTask("Update Material", [=]() {

				auto pMaterial = material.Lock();

				pMaterial->GetShader().Lock()->RemoveHotReloadDependentObject(material);
				pMaterial->ClearSamplers();
				pMaterial->ClearUniforms();

				ShaderSetPtr pShader;
				auto pLoadShader = App::GetSubmodule<ShaderCompiler>()->LoadShader(pMaterialAsset->GetShader(), pShader, pMaterialAsset->GetShaderDefines());

				pMaterial->SetRenderState(pMaterialAsset->GetRenderState());

				pMaterial->SetShader(pShader);
				pShader.Lock()->AddHotReloadDependentObject(material);

				auto updateRHI = JobSystem::Scheduler::CreateTask("Update material RHI resource", [=]()
					{
						pMaterial.GetRawPtr()->UpdateRHIResource();
						pMaterial.GetRawPtr()->TraceHotReload(nullptr);
					});

				// Preload textures
				for (auto& sampler : pMaterialAsset->GetSamplers())
				{
					TexturePtr texture;
					updateRHI->Join(
						App::GetSubmodule<TextureImporter>()->LoadTexture(sampler.m_uid, texture)->Then<void, bool>(
							[=](bool bRes)
							{
								if (bRes)
								{
									texture.Lock()->AddHotReloadDependentObject(material);
									pMaterial.GetRawPtr()->SetSampler(sampler.m_name, texture);
								}
							}, "Set material texture binding", JobSystem::EThreadType::Rendering));
				}

				for (auto& uniform : pMaterialAsset.GetRawPtr()->GetUniformValues())
				{
					pMaterial.GetRawPtr()->SetUniform(uniform.m_first, uniform.m_second);
				}

				updateRHI->Run();
				});

			if (auto promise = GetLoadPromise(assetInfo->GetUID()))
			{
				updateMaterial->Join(promise);
			}

			updateMaterial->Run();
		}
	}
}

bool MaterialImporter::IsMaterialLoaded(UID uid) const
{
	return m_loadedMaterials.ContainsKey(uid);
}

TSharedPtr<MaterialAsset> MaterialImporter::LoadMaterialAsset(UID uid)
{
	SAILOR_PROFILE_FUNCTION();

	if (MaterialAssetInfoPtr materialAssetInfo = dynamic_cast<MaterialAssetInfoPtr>(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid)))
	{
		const std::string& filepath = materialAssetInfo->GetAssetFilepath();

		std::string materialJson;

		AssetRegistry::ReadAllTextFile(filepath, materialJson);

		json j_material;
		if (j_material.parse(materialJson.c_str()) == nlohmann::detail::value_t::discarded)
		{
			SAILOR_LOG("Cannot parse material asset file: %s", filepath.c_str());
			return TSharedPtr<MaterialAsset>();
		}

		j_material = json::parse(materialJson);

		MaterialAsset* material = new MaterialAsset();
		material->Deserialize(j_material);

		return TSharedPtr<MaterialAsset>(material);
	}

	SAILOR_LOG("Cannot find material asset info with UID: %s", uid.ToString().c_str());
	return TSharedPtr<MaterialAsset>();
}

const UID& MaterialImporter::CreateMaterialAsset(const std::string& assetFilepath, MaterialAsset::GetData data)
{
	json newMaterial;

	MaterialAsset asset;
	asset.m_pData = TUniquePtr<MaterialAsset::GetData>::Make(std::move(data));
	asset.Serialize(newMaterial);

	std::ofstream assetFile(assetFilepath);
	assetFile << newMaterial.dump();
	assetFile.close();

	return App::GetSubmodule<AssetRegistry>()->LoadAsset(assetFilepath);
}

bool MaterialImporter::LoadMaterial_Immediate(UID uid, MaterialPtr& outMaterial)
{
	SAILOR_PROFILE_FUNCTION();
	auto task = LoadMaterial(uid, outMaterial);
	task->Wait();

	return task->GetResult();
}

MaterialPtr MaterialImporter::GetLoadedMaterial(UID uid)
{
	// Check loaded materials
	auto materialIt = m_loadedMaterials.Find(uid);
	if (materialIt != m_loadedMaterials.end())
	{
		return (*materialIt).m_second;
	}
	return TWeakPtr<Material>(nullptr);
}

JobSystem::TaskPtr<bool> MaterialImporter::GetLoadPromise(UID uid)
{
	auto it = m_promises.Find(uid);
	if (it != m_promises.end())
	{
		return (*it).m_second;
	}

	return JobSystem::TaskPtr<bool>(nullptr);
}

JobSystem::TaskPtr<bool> MaterialImporter::LoadMaterial(UID uid, MaterialPtr& outMaterial)
{
	SAILOR_PROFILE_FUNCTION();

	JobSystem::TaskPtr<bool> newPromise;
	outMaterial = nullptr;

	// Check promises first
	auto it = m_promises.Find(uid);
	if (it != m_promises.end())
	{
		newPromise = (*it).m_second;
	}

	// Check loaded materials
	auto materialIt = m_loadedMaterials.Find(uid);
	if (materialIt != m_loadedMaterials.end())
	{
		outMaterial = (*materialIt).m_second;

		if (newPromise != nullptr)
		{
			if (!newPromise)
			{
				return JobSystem::TaskPtr<bool>::Make(true);
			}

			return newPromise;
		}
	}

	auto& promise = m_promises.At_Lock(uid, nullptr);

	// We have promise
	if (promise)
	{
		m_promises.Unlock(uid);
		outMaterial = m_loadedMaterials[uid];
		return promise;
	}

	// We need to start load the material
	if (auto pMaterialAsset = LoadMaterialAsset(uid))
	{
		auto pMaterial = TSharedPtr<Material>::Make(uid);

		ShaderSetPtr pShader;
		auto pLoadShader = App::GetSubmodule<ShaderCompiler>()->LoadShader(pMaterialAsset->GetShader(), pShader, pMaterialAsset->GetShaderDefines());

		pMaterial->SetRenderState(pMaterialAsset->GetRenderState());

		pMaterial->SetShader(pShader);
		pShader.Lock()->AddHotReloadDependentObject(pMaterial);

		newPromise = JobSystem::Scheduler::CreateTaskWithResult<bool>("Load material",
			[pMaterial, pMaterialAsset]()
			{
				auto updateRHI = JobSystem::Scheduler::CreateTask("Update material RHI resource", [=]()
					{
						pMaterial.GetRawPtr()->UpdateRHIResource();
					});

				// Preload textures
				for (auto& sampler : pMaterialAsset->GetSamplers())
				{
					TexturePtr texture;
					updateRHI->Join(
						App::GetSubmodule<TextureImporter>()->LoadTexture(sampler.m_uid, texture)->Then<void, bool>(
							[=](bool bRes)
							{
								if (bRes)
								{
									texture.Lock()->AddHotReloadDependentObject(pMaterial);
									pMaterial.GetRawPtr()->SetSampler(sampler.m_name, texture);
								}
							}, "Set material texture binding", JobSystem::EThreadType::Rendering));
				}

				for (auto& uniform : pMaterialAsset.GetRawPtr()->GetUniformValues())
				{
					pMaterial.GetRawPtr()->SetUniform(uniform.m_first, uniform.m_second);
				}

				updateRHI->Run();

				return true;
			});

		newPromise->Join(pLoadShader);

		App::GetSubmodule<JobSystem::Scheduler>()->Run(newPromise);

		outMaterial = m_loadedMaterials[uid] = pMaterial;
		promise = newPromise;
		m_promises.Unlock(uid);

		return promise;
	}

	m_promises.Unlock(uid);

	return JobSystem::TaskPtr<bool>::Make(false);
}

