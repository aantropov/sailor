#pragma once
#include "Core/Defines.h"
#include <atomic>
#include <string>
#include "Containers/Vector.h"
#include "Containers/Map.h"
#include "Containers/ConcurrentMap.h"
#include "Core/SpinLock.h"
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
	class TextureImporter final : public TSubmodule<TextureImporter>, public IAssetInfoHandlerListener, public IAssetFactory
	{
	public:
		struct TextureSamplerSlotSnapshot
		{
			uint32_t m_index = 0;
			uint64_t m_contentRevision = 0;
			RHI::RHITexturePtr m_texture;
		};

		struct TextureSamplersSnapshot
		{
			uint64_t m_descriptorRevision = 0;
			TVector<TextureSamplerSlotSnapshot> m_slots;
		};

		// Keep this in sync with runtime descriptor allocation on macOS/MoltenVK.
		// 262144 overflows Metal argument-buffer validation in current path.
		// Slot zero is reserved for the default texture.
		static constexpr size_t MaxTexturesInScene = 8192;
		static constexpr size_t MaxUserTexturesInScene = MaxTexturesInScene - 1;

		static constexpr bool IsUserTextureSamplerIndexValid(size_t index) noexcept
		{
			return index > 0 && index < MaxTexturesInScene;
		}

		using ByteCode = TVector<uint8_t>;

		struct CpuDecodeRequest
		{
			FileId m_fileId{};
			std::string m_filepath;
			int32_t m_glbTextureIndex = -1;
			bool m_bDecodeAsFloat = false;
			bool m_bGenerateMips = false;
			TMap<std::string, FileRevision> m_sourceRevisions;

			bool operator==(const CpuDecodeRequest&) const = default;
		};

		struct CpuTextureSnapshot
		{
			TSharedPtr<const ByteCode> m_pixels;
			int32_t m_width = 0, m_height = 0;
			RHI::ETextureClamping m_clamping = RHI::ETextureClamping::Repeat;
			CpuDecodeRequest m_source;
		};

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
		// Shared CPU-only decode for the captured source revision; no GPU texture or sampler slot.
		SAILOR_API Tasks::TaskPtr<CpuTextureSnapshot> LoadCpuTexture(FileId uid);
		SAILOR_API static bool DecodeTextureCpu(FileId uid, ByteCode& decodedData,
			int32_t& width, int32_t& height, uint32_t& mipLevels);
		SAILOR_API static bool CaptureCpuDecodeRequest(const TextureAssetInfo& assetInfo,
			CpuDecodeRequest& outRequest);
		SAILOR_API static bool DecodeTextureCpu(const CpuDecodeRequest& request, ByteCode& decodedData,
			int32_t& width, int32_t& height, uint32_t& mipLevels);
		SAILOR_API TexturePtr GetLoadedTexture(FileId uid);
		SAILOR_API Tasks::TaskPtr<TexturePtr> GetLoadPromise(FileId uid);
		SAILOR_API Tasks::TaskPtr<TVector<CpuTextureSnapshot>> CaptureCpuTextures(const TVector<TexturePtr>& textures);
		SAILOR_API virtual void CollectGarbage() override;

		SAILOR_API RHI::RHIShaderBindingSetPtr GetTextureSamplersBindingSet() { return m_textureSamplersBindings; }
		SAILOR_API TextureSamplersSnapshot GetTextureSamplersSnapshot(const TVector<uint32_t>& requestedIndices) const;
		SAILOR_API uint64_t CalculateTextureSamplersRevision(
			const TVector<uint32_t>& requestedIndices) const;
		SAILOR_API size_t GetTextureIndex(FileId uid);
		SAILOR_API size_t GetTextureSamplersCount() const { return m_textureSamplersCurrentIndex.load(); }

	protected:

		// Bindless texture bindings
		std::atomic<size_t> m_textureSamplersCurrentIndex = 0;
		RHI::RHIShaderBindingSetPtr m_textureSamplersBindings{};
		TConcurrentMap<FileId, size_t> m_textureSamplersIndices{};
		TVector<uint64_t> m_textureSamplerSlotRevisions{};
		mutable SpinLock m_textureSamplersLock;

		struct TextureEntry
		{
			TexturePtr m_texture;
			Tasks::TaskPtr<TexturePtr> m_load;
			Tasks::ITaskPtr m_lastAccess;
			bool m_bCpuBuffersRequested = false;
			bool operator==(const TextureEntry&) const = default;
		};
		TConcurrentMap<FileId, TextureEntry> m_textures;

		struct CpuTextureEntry
		{
			CpuDecodeRequest m_source;
			RHI::ETextureClamping m_clamping = RHI::ETextureClamping::Repeat;
			Tasks::TaskPtr<CpuTextureSnapshot> m_load;
			bool operator==(const CpuTextureEntry&) const = default;
		};
		TConcurrentMap<FileId, CpuTextureEntry> m_cpuTextures;

		Memory::ObjectAllocatorPtr m_allocator;

		using DecodeTextureFunction = bool (*)(const CpuDecodeRequest&, ByteCode&, int32_t&, int32_t&, uint32_t&);
		DecodeTextureFunction m_decodeTexture = &DecodeTextureCpu;

		Tasks::TaskPtr<TexturePtr> CreateTextureTask(TexturePtr texture, const TextureAssetInfo& assetInfo,
			bool bCpuOnly, bool bHotReload, const Tasks::ITaskPtr& previous);

		bool RegisterTextureSamplerBinding(RHI::RHITexturePtr texture, size_t& outIndex);
		bool UpdateTextureSamplerBinding(RHI::RHITexturePtr texture, uint32_t index);
		bool UpdateTextureSamplerBindingLocked(RHI::RHITexturePtr texture, uint32_t index);
		SAILOR_API bool IsTextureLoaded(FileId uid) const;
		SAILOR_API static bool ImportTexture(FileId uid, ByteCode& decodedData, int32_t& width, int32_t& height, uint32_t& mipLevels);
		static bool ImportTexture(const CpuDecodeRequest& request, ByteCode& decodedData,
			int32_t& width, int32_t& height, uint32_t& mipLevels);

		friend class TextureImporterTestAccess;
	};

	class Texture : public Object
	{
	public:

		SAILOR_API Texture(FileId uid) : Object(uid) {}

		SAILOR_API virtual bool IsReady() const override;

		SAILOR_API const RHI::RHITexturePtr& GetRHI() const { return m_rhiTexture; }
		SAILOR_API RHI::RHITexturePtr& GetRHI() { return m_rhiTexture; }
		SAILOR_API const TVector<uint8_t>& GetDecodedData() const
		{
			static const TVector<uint8_t> empty;
			return m_decodedData ? *m_decodedData : empty;
		}
		SAILOR_API int32_t GetWidth() const { return m_width; }
		SAILOR_API int32_t GetHeight() const { return m_height; }
		SAILOR_API uint32_t GetMipLevels() const { return m_mipLevels; }
		SAILOR_API bool HasCpuData() const { return m_bCpuDataReady.load(std::memory_order_acquire); }

	protected:

		void SetDecodedData(TVector<uint8_t>&& data)
		{
			m_decodedData = data.IsEmpty() ? TSharedPtr<const TVector<uint8_t>>{} :
				TSharedPtr<TVector<uint8_t>>::Make(std::move(data));
			m_bCpuDataReady.store(static_cast<bool>(m_decodedData), std::memory_order_release);
		}

		RHI::RHITexturePtr m_rhiTexture;
		TSharedPtr<const TVector<uint8_t>> m_decodedData;
		TextureImporter::CpuDecodeRequest m_cpuSource;
		std::atomic<bool> m_bCpuDataReady{ false };
		int32_t m_width = 0;
		int32_t m_height = 0;
		uint32_t m_mipLevels = 1;

		friend class ModelImporter;
		friend class TextureImporter;
	};

	using TexturePtr = TObjectPtr<Texture>;
}
