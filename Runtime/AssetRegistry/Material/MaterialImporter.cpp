#include "AssetRegistry/Material/MaterialImporter.h"

#include "AssetRegistry/UID.h"
#include "AssetRegistry/AssetRegistry.h"
#include "MaterialAssetInfo.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "Math/Math.h"
#include "Core/Utils.h"
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

void Material::Flush()
{
	m_bIsReady = m_rhiMaterial;
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

JobSystem::TaskPtr<bool> MaterialImporter::LoadMaterial(UID uid, MaterialPtr& outMaterial)
{
	SAILOR_PROFILE_FUNCTION();

	std::scoped_lock<std::mutex> guard(m_mutex);

	JobSystem::TaskPtr<bool> promise;
	outMaterial = nullptr;

	// Check promises first
	auto it = m_promises.Find(uid);
	if (it != m_promises.end())
	{
		promise = (*it).m_second;
	}

	// Check loaded materials
	auto materialIt = m_loadedMaterials.Find(uid);
	if (materialIt != m_loadedMaterials.end())
	{
		outMaterial = (*materialIt).m_second;

		if (!promise)
		{
			return JobSystem::TaskPtr<bool>::Make(true);
		}

		return promise;
	}

	if (auto pMaterialAsset = LoadMaterialAsset(uid))
	{
		auto pMaterial = TSharedPtr<Material>::Make(uid);

		ShaderSetPtr pShader;
		auto pLoadShader = App::GetSubmodule<ShaderCompiler>()->LoadShader(pMaterialAsset->GetShader(), pShader, pMaterialAsset->GetShaderDefines());

		pShader.Lock()->AddHotReloadDependentObject(pMaterial);

		promise = JobSystem::Scheduler::CreateTaskWithResult<bool>("Load material",
			[pMaterial, pShader, pMaterialAsset]()
			{
				RHI::MaterialPtr pMaterialPtr = RHI::Renderer::GetDriver()->CreateMaterial(pMaterialAsset->GetRenderState(), pShader);

				auto flushMaterial = JobSystem::Scheduler::CreateTask("Flush material", [=]()
					{
						pMaterial.GetRawPtr()->Flush();
					});

				// Preload textures
				auto bindings = pMaterialPtr->GetBindings();
				for (auto& sampler : pMaterialAsset->GetSamplers())
				{
					if (bindings->HasBinding(sampler.m_name))
					{
						TexturePtr texture;
						flushMaterial->Join(
							App::GetSubmodule<TextureImporter>()->LoadTexture(sampler.m_uid, texture)->Then<void, bool>(
								[=](bool bRes)
								{
									if (bRes)
									{
										for (auto& sampler : pMaterialAsset.GetRawPtr()->GetSamplers())
										{
											if (bindings->HasBinding(sampler.m_name))
											{
												RHI::Renderer::GetDriver()->UpdateShaderBinding(pMaterialPtr->GetBindings(), sampler.m_name, texture.Lock()->GetRHI());
											}
										}
									}
								}, "Set material texture binding", JobSystem::EThreadType::Rendering));

						if (texture)
						{
							texture.Lock()->AddHotReloadDependentObject(pMaterial);
						}
					}
				}

				for (auto& uniform : pMaterialAsset.GetRawPtr()->GetUniformValues())
				{
					if (bindings->HasParameter(uniform.m_first))
					{
						std::string outBinding;
						std::string outVariable;

						RHI::ShaderBindingSet::ParseParameter(uniform.m_first, outBinding, outVariable);
						RHI::ShaderBindingPtr& binding = bindings->GetOrCreateShaderBinding(outBinding);
						auto value = uniform.m_second;

						SAILOR_ENQUEUE_JOB_RENDER_THREAD_TRANSFER_CMD("Set material parameter", ([&binding, outVariable, value](RHI::CommandListPtr& cmdList)
							{
								RHI::Renderer::GetDriverCommands()->UpdateShaderBingingVariable(cmdList, binding, outVariable, &value, sizeof(value));
							}));
					}
				}

				pMaterial.GetRawPtr()->m_rhiMaterial = pMaterialPtr;
				flushMaterial->Run();

				return true;
			});

		promise->Join(pLoadShader);

		App::GetSubmodule<JobSystem::Scheduler>()->Run(promise);

		outMaterial = m_loadedMaterials[uid] = pMaterial;
		m_promises[uid] = promise;

		return promise;
	}
	return JobSystem::TaskPtr<bool>::Make(false);
}


