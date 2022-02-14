#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/AssetInfo.h"
#include "TextureAssetInfo.h"
#include "RHI/Renderer.h"
#include "Engine/Object.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"

namespace Sailor
{
	class Texture : public Object
	{
	public:

		Texture(UID uid) : Object(uid) {}

		virtual bool IsReady() const override;

		const RHI::TexturePtr& GetRHI() const { return m_rhiTexture; }
		RHI::TexturePtr& GetRHI() { return m_rhiTexture; }

	protected:

		RHI::TexturePtr m_rhiTexture;

		friend class TextureImporter;
	};

	using TexturePtr = TObjectPtr<Texture>;

	class TextureImporter final : public TSubmodule<TextureImporter>, public IAssetInfoHandlerListener
	{
	public:
		using ByteCode = TVector<uint8_t>;

		SAILOR_API TextureImporter(TextureAssetInfoHandler* infoHandler);
		virtual SAILOR_API ~TextureImporter() override;

		virtual SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override;
		virtual SAILOR_API void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

		SAILOR_API bool LoadTexture_Immediate(UID uid, TexturePtr& outTexture);
		SAILOR_API JobSystem::TaskPtr<bool> LoadTexture(UID uid, TexturePtr& outTexture);

		SAILOR_API TexturePtr GetLoadedTexture(UID uid);
		SAILOR_API JobSystem::TaskPtr<bool> GetLoadPromise(UID uid);

	protected:

		TConcurrentMap<UID, JobSystem::TaskPtr<bool>> m_promises;
		TConcurrentMap<UID, TexturePtr> m_loadedTextures;

		Memory::ObjectAllocatorPtr m_allocator;

		SAILOR_API bool IsTextureLoaded(UID uid) const;
		SAILOR_API static bool ImportTexture(UID uid, ByteCode& decodedData, int32_t& width, int32_t& height, uint32_t& mipLevels);
	};
}
