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

		const RHI::RenderState& GetRenderState() const { return m_renderState; }
		bool IsTransparent() const { return m_bIsTransparent; }
		const std::string& GetRenderQueue() const { return m_renderQueue; }

	protected:

		RHI::RenderState m_renderState;

		std::string m_renderQueue = "Opaque";
		bool m_bIsTransparent = false;

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
