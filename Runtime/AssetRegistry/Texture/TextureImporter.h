#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
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
#include "Tasks/Tasks.h"

namespace Sailor
{
	class Texture : public Object
	{
	public:

		SAILOR_API Texture(FileId uid) : Object(uid) {}

		SAILOR_API virtual bool IsReady() const override;

		SAILOR_API const RHI::RHITexturePtr& GetRHI() const { return m_rhiTexture; }
		SAILOR_API RHI::RHITexturePtr& GetRHI() { return m_rhiTexture; }
		SAILOR_API const TVector<uint8_t>& GetDecodedData() const { return m_decodedData; }
		SAILOR_API int32_t GetWidth() const { return m_width; }
		SAILOR_API int32_t GetHeight() const { return m_height; }
		SAILOR_API uint32_t GetMipLevels() const { return m_mipLevels; }
		SAILOR_API bool HasCpuData() const { return m_decodedData.Num() > 0; }

	protected:

		RHI::RHITexturePtr m_rhiTexture;
		TVector<uint8_t> m_decodedData;
		int32_t m_width = 0;
		int32_t m_height = 0;
		uint32_t m_mipLevels = 1;

		friend class TextureImporter;
	};

	using TexturePtr = TObjectPtr<Texture>;

	class TextureImporter final : public TSubmodule<TextureImporter>, public IAssetInfoHandlerListener, public IAssetFactory
	{
	public:

		// Keep this in sync with runtime descriptor allocation on macOS/MoltenVK.
		// 262144 overflows Metal argument-buffer validation in current path.
		static const size_t MaxTexturesInScene = 8192;

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
		SAILOR_API size_t GetTextureSamplersCount() const { return m_textureSamplersCurrentIndex.load(); }

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
