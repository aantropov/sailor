#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Engine/Types.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/AssetInfo.h"
#include "TextureAssetInfo.h"
#include "RHI/Types.h"
#include "Engine/Object.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"

namespace Sailor
{
	class Texture : public Object
	{
	public:

		SAILOR_API Texture(UID uid) : Object(uid) {}

		SAILOR_API virtual bool IsReady() const override;

		SAILOR_API const RHI::RHITexturePtr& GetRHI() const { return m_rhiTexture; }
		SAILOR_API RHI::RHITexturePtr& GetRHI() { return m_rhiTexture; }

	protected:

		RHI::RHITexturePtr m_rhiTexture;

		friend class TextureImporter;
	};

	using TexturePtr = TObjectPtr<Texture>;

	class TextureImporter final : public TSubmodule<TextureImporter>, public IAssetInfoHandlerListener
	{
	public:
		using ByteCode = TVector<uint8_t>;

		SAILOR_API TextureImporter(TextureAssetInfoHandler* infoHandler);
		SAILOR_API virtual ~TextureImporter() override;

		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override;
		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

		SAILOR_API bool LoadTexture_Immediate(UID uid, TexturePtr& outTexture);
		SAILOR_API Tasks::TaskPtr<TexturePtr> LoadTexture(UID uid, TexturePtr& outTexture);

		SAILOR_API TexturePtr GetLoadedTexture(UID uid);
		SAILOR_API Tasks::TaskPtr<TexturePtr> GetLoadPromise(UID uid);

	protected:

		TConcurrentMap<UID, Tasks::TaskPtr<TexturePtr>> m_promises;
		TConcurrentMap<UID, TexturePtr> m_loadedTextures;

		Memory::ObjectAllocatorPtr m_allocator;

		SAILOR_API bool IsTextureLoaded(UID uid) const;
		SAILOR_API static bool ImportTexture(UID uid, ByteCode& decodedData, int32_t& width, int32_t& height, uint32_t& mipLevels);
	};
}
