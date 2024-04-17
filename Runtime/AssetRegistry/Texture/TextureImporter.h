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
#include "AssetRegistry/AssetFactory.h"
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

		SAILOR_API Texture(FileId uid) : Object(uid) {}

		SAILOR_API virtual bool IsReady() const override;

		SAILOR_API const RHI::RHITexturePtr& GetRHI() const { return m_rhiTexture; }
		SAILOR_API RHI::RHITexturePtr& GetRHI() { return m_rhiTexture; }

	protected:

		RHI::RHITexturePtr m_rhiTexture;

		friend class TextureImporter;
	};

	using TexturePtr = TObjectPtr<Texture>;

	class TextureImporter final : public TSubmodule<TextureImporter>, public IAssetInfoHandlerListener, public IAssetFactory
	{
	public:

		static const size_t MaxTexturesInScene = 1 << 18;

		using ByteCode = TVector<uint8_t>;

		static constexpr RHI::ETextureUsageFlags DefaultTextureUsage =
			RHI::ETextureUsageBit::TextureTransferSrc_Bit |
			RHI::ETextureUsageBit::TextureTransferDst_Bit |
			RHI::ETextureUsageBit::Sampled_Bit;

		SAILOR_API TextureImporter(TextureAssetInfoHandler* infoHandler);
		SAILOR_API virtual ~TextureImporter() override;

		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override;
		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

		SAILOR_API bool LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate = true) override;
		SAILOR_API bool LoadTexture_Immediate(FileId uid, TexturePtr& outTexture);
		SAILOR_API Tasks::TaskPtr<TexturePtr> LoadTexture(FileId uid, TexturePtr& outTexture);

		SAILOR_API TexturePtr GetLoadedTexture(FileId uid);
		SAILOR_API Tasks::TaskPtr<TexturePtr> GetLoadPromise(FileId uid);
		SAILOR_API virtual void CollectGarbage() override;

		SAILOR_API RHI::RHIShaderBindingSetPtr GetTextureSamplersBindingSet() { return m_textureSamplersBindings; }
		SAILOR_API size_t GetTextureIndex(FileId uid);

	protected:

		// Bindless texture bindings
		std::atomic<size_t> m_textureSamplersCurrentIndex = 0;
		RHI::RHIShaderBindingSetPtr m_textureSamplersBindings{};
		TConcurrentMap<FileId, size_t> m_textureSamplersIndices{};

		TConcurrentMap<FileId, Tasks::TaskPtr<TexturePtr>> m_promises{};
		TConcurrentMap<FileId, TexturePtr> m_loadedTextures{};

		Memory::ObjectAllocatorPtr m_allocator;

		SAILOR_API bool IsTextureLoaded(FileId uid) const;
		SAILOR_API static bool ImportTexture(FileId uid, ByteCode& decodedData, int32_t& width, int32_t& height, uint32_t& mipLevels);
	};
}
