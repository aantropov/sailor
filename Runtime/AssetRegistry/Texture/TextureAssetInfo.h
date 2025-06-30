#pragma once
#include "AssetRegistry/AssetInfo.h"
#include "RHI/Types.h"
#include "Core/Singleton.hpp"

using namespace std;

namespace Sailor
{
	class TextureAssetInfo final : public AssetInfo
	{
	public:
		SAILOR_API virtual ~TextureAssetInfo() = default;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

		SAILOR_API RHI::ESamplerReductionMode GetSamplerReduction() const { return m_reduction; }
		SAILOR_API RHI::ETextureFiltration GetFiltration() const { return m_filtration; }
		SAILOR_API RHI::ETextureClamping GetClamping() const { return m_clamping; }
		SAILOR_API RHI::ETextureFormat GetFormat() const { return m_format; }
		SAILOR_API bool ShouldGenerateMips() const { return m_bShouldGenerateMips; }
		SAILOR_API bool ShouldSupportStorageBinding() const { return m_bShouldSupportStorageBinding; }
		SAILOR_API bool StoredInGlb() const { return m_glbTextureIndex != -1; }
		SAILOR_API int32_t GetGlbTextureIndex() const { return m_glbTextureIndex; }

		SAILOR_API virtual IAssetInfoHandler* GetHandler() override;

	private:

		RHI::ETextureFiltration m_filtration = RHI::ETextureFiltration::Linear;
		RHI::ETextureClamping m_clamping = RHI::ETextureClamping::Repeat;
		RHI::ETextureFormat m_format = RHI::ETextureFormat::R8G8B8A8_SRGB;
		RHI::ESamplerReductionMode m_reduction = RHI::ESamplerReductionMode::Average;

		bool m_bShouldGenerateMips = true;
		bool m_bShouldSupportStorageBinding = false;

		// We support the loading of textures from glb
		int32_t m_glbTextureIndex = -1;
	};

	using TextureAssetInfoPtr = TextureAssetInfo*;

	class TextureAssetInfoHandler final : public TSubmodule<TextureAssetInfoHandler>, public IAssetInfoHandler
	{

	public:

		IAssetFactory* GetFactory() override;

		SAILOR_API TextureAssetInfoHandler(AssetRegistry* assetRegistry);

		SAILOR_API void GetDefaultMeta(YAML::Node& outDefaultYaml) const override;
		SAILOR_API AssetInfoPtr CreateAssetInfo() const override;

		SAILOR_API virtual ~TextureAssetInfoHandler() = default;
	};
}
