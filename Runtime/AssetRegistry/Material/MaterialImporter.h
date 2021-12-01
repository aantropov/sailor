#pragma once
#include "Core/Defines.h"
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/UID.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/Material/MaterialAssetInfo.h"
#include "RHI/Types.h"
#include "RHI/Renderer.h"
#include "Framework/Object.h"

namespace Sailor
{
	class Material : public Object
	{
	public:

		Material(UID uid) : Object(uid) {}

		virtual bool IsReady() const override;

		const RHI::MaterialPtr& GetRHI() const { return m_rhiMaterial; }
		RHI::MaterialPtr& GetRHI() { return m_rhiMaterial; }

	protected:

		RHI::MaterialPtr m_rhiMaterial;

		friend class MaterialImporter;
	};

	using MaterialPtr = TWeakPtr<Material>;

	class MaterialAsset : public IJsonSerializable
	{
	public:

		class SamplerEntry final : IJsonSerializable
		{
		public:

			SamplerEntry() = default;
			SamplerEntry(std::string name, const UID& uid) : m_name(std::move(name)), m_uid(uid) {}

			std::string m_name;
			UID m_uid;

			virtual SAILOR_API void Serialize(nlohmann::json& outData) const
			{
				outData["name"] = m_name;
				m_uid.Serialize(outData["uid"]);
			}

			virtual SAILOR_API void Deserialize(const nlohmann::json& inData)
			{
				m_name = inData["name"].get<std::string>();
				m_uid.Deserialize(inData["uid"]);
			}
		};

		struct Data
		{
			RHI::RenderState m_renderState;

			std::string m_renderQueue = "Opaque";
			bool m_bIsTransparent = false;

			std::vector<std::string> m_shaderDefines;
			std::vector<SamplerEntry> m_samplers;
			std::vector<std::pair<std::string, glm::vec4>> m_uniformsVec4;

			UID m_shader;
		};

		virtual SAILOR_API ~MaterialAsset() = default;

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const override;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData) override;

		SAILOR_API const RHI::RenderState& GetRenderState() const { return m_pData->m_renderState; }
		SAILOR_API bool IsTransparent() const { return m_pData->m_bIsTransparent; }
		SAILOR_API const std::string& GetRenderQueue() const { return m_pData->m_renderQueue; }
		SAILOR_API const UID& GetShader() const { return m_pData->m_shader; }
		SAILOR_API const std::vector<std::string>& GetShaderDefines() const { return  m_pData->m_shaderDefines; }
		SAILOR_API const std::vector<SamplerEntry>& GetSamplers() const { return m_pData->m_samplers; }
		SAILOR_API const std::vector<std::pair<std::string, glm::vec4>>& GetUniformValues() const { return m_pData->m_uniformsVec4; }

	protected:

		TUniquePtr<Data> m_pData;
		friend class MaterialImporter;
	};

	class MaterialImporter final : public TSubmodule<MaterialImporter>, public IAssetInfoHandlerListener
	{
	public:

		SAILOR_API MaterialImporter(MaterialAssetInfoHandler* infoHandler);
		virtual SAILOR_API ~MaterialImporter() override;

		virtual SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override;
		virtual SAILOR_API void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

		SAILOR_API TSharedPtr<MaterialAsset> LoadMaterialAsset(UID uid);

		SAILOR_API bool LoadMaterial_Immediate(UID uid, MaterialPtr& outMaterial);
		SAILOR_API bool LoadMaterial(UID uid, MaterialPtr& outMaterial, JobSystem::TaskPtr& outLoadingTask);

		SAILOR_API const UID& CreateMaterialAsset(const std::string& assetpath, MaterialAsset::Data data);

	protected:

		SAILOR_API bool IsMaterialLoaded(UID uid) const;
		std::unordered_map<UID, TSharedPtr<Material>> m_loadedMaterials;
	};
}
