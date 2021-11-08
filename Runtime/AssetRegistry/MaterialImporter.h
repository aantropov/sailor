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

using namespace Sailor::RHI;

namespace Sailor
{
	class MaterialAsset
	{
	public:

		bool IsDepthTestEnabled() const { return m_bEnableDepthTest; }
		bool IsEnabledZWrite() const { return m_bEnableZWrite; }
		bool IsTransparent() const { return m_bIsTransparent; }
		ECullMode GetCullMode() const { return m_cullMode; }
		EBlendMode GetBlendMode() const { return m_blendMode; }
		float GetDepthBias() const { return m_depthBias; }
		const std::string& GetRenderQueue() const { return m_renderQueue; }

	protected:

		bool m_bEnableDepthTest = true;
		bool m_bEnableZWrite = true;
		float m_depthBias = 0.0f;
		ECullMode m_cullMode = ECullMode::Back;
		std::string m_renderQueue = "Opaque";
		bool m_bIsTransparent = false;
		EBlendMode m_blendMode = EBlendMode::None;

		UID m_vertexShader;
		UID m_fragmentShader;

		std::vector<std::string> m_shaderDefines;
	};

	class MaterialImporter final : public TSingleton<MaterialImporter>, public IAssetInfoHandlerListener
	{
	public:

		static SAILOR_API void Initialize();
		virtual SAILOR_API ~MaterialImporter() override;

		virtual SAILOR_API void OnAssetInfoUpdated(AssetInfoPtr assetInfo) override;

		bool SAILOR_API LoadMaterial(UID uid);

	private:

	};
}
