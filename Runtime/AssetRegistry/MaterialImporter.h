#pragma once
#include "Defines.h"
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Singleton.hpp"
#include "Core/SharedPtr.hpp"
#include "Core/WeakPtr.hpp"
#include "AssetInfo.h"
#include "RHI/Types.h"
#include "RHI/Renderer.h"

using namespace Sailor::RHI;

namespace Sailor
{
	class MaterialAsset : public IJsonSerializable
	{
	public:

		virtual SAILOR_API ~MaterialAsset() = default;

		virtual SAILOR_API void Serialize(nlohmann::json & outData) const override;
		virtual SAILOR_API void Deserialize(const nlohmann::json & inData) override;

		SAILOR_API const RHI::RenderState& GetRenderState() const { return m_renderState; }
		SAILOR_API bool IsTransparent() const { return m_bIsTransparent; }
		SAILOR_API const std::string& GetRenderQueue() const { return m_renderQueue; }
		SAILOR_API const UID& GetShader() const { return m_shader; }
		SAILOR_API const std::vector<std::string>& GetShaderDefines() const { return  m_shaderDefines; }
		SAILOR_API const std::vector<std::pair<std::string, UID>>& GetSamplers() const { return m_samplers; }
		SAILOR_API const std::vector<std::pair<std::string, float>>& GetUniforms() const { return m_uniforms; }

	protected:

		RHI::RenderState m_renderState;

		std::string m_renderQueue = "Opaque";
		bool m_bIsTransparent = false;

		std::vector<std::string> m_shaderDefines;
		std::vector<std::pair<std::string, UID>> m_samplers;
		std::vector<std::pair<std::string, float>> m_uniforms;

		UID m_shader;
	};

	class MaterialImporter final : public TSingleton<MaterialImporter>, public IAssetInfoHandlerListener
	{
	public:

		static SAILOR_API void Initialize();
		virtual SAILOR_API ~MaterialImporter() override;

		virtual SAILOR_API void OnAssetInfoUpdated(AssetInfoPtr assetInfo) override;

		static SAILOR_API TWeakPtr<MaterialAsset> LoadMaterialAsset(UID uid);
		static SAILOR_API RHI::MaterialPtr LoadMaterial(UID uid);

	private:

		std::unordered_map<UID, TSharedPtr<MaterialAsset>> m_loadedMaterials;
	};
}
