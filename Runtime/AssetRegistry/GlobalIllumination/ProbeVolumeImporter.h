#pragma once

#include "AssetRegistry/AssetFactory.h"
#include "AssetRegistry/GlobalIllumination/ProbeVolumeAssetInfo.h"
#include "AssetRegistry/GlobalIllumination/ProbeVolumeData.h"
#include "Containers/ConcurrentMap.h"
#include "Core/SpinLock.h"
#include "Core/Submodule.h"
#include "Engine/Object.h"
#include "Memory/ObjectAllocator.hpp"
#include "Tasks/Tasks.h"

#include <atomic>
#include <string>

namespace Sailor
{
	struct ProbeVolumeAssetSnapshot final
	{
		ProbeVolumeDataPtr m_data{};
		uint64_t m_revision = 0u;
		std::string m_diagnostic{};
	};

	class ProbeVolumeAsset final : public Object
	{
	public:
		SAILOR_API explicit ProbeVolumeAsset(FileId uid);
		SAILOR_API bool IsReady() const override;
		SAILOR_API uint64_t GetRevision() const noexcept;
		SAILOR_API ProbeVolumeAssetSnapshot GetSnapshot() const;

	private:
		void ApplyLoadResult(
			ProbeVolumeDataPtr data,
			std::string diagnostic);

		mutable SpinLock m_lock;
		ProbeVolumeDataPtr m_data{};
		std::string m_diagnostic{};
		std::atomic<uint64_t> m_revision{ 0u };
		std::atomic<bool> m_bReady{ false };

		friend class ProbeVolumeImporter;
	};

	using ProbeVolumeAssetPtr = TObjectPtr<ProbeVolumeAsset>;

	class ProbeVolumeImporter final :
		public TSubmodule<ProbeVolumeImporter>,
		public IAssetInfoHandlerListener,
		public IAssetFactory
	{
	public:
		SAILOR_API explicit ProbeVolumeImporter(
			ProbeVolumeAssetInfoHandler* infoHandler);
		SAILOR_API ~ProbeVolumeImporter() override;

		SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override;
		SAILOR_API void OnUpdateAssetInfo(
			AssetInfoPtr assetInfo,
			bool bWasExpired) override;
		SAILOR_API bool LoadAsset(
			FileId uid,
			TObjectPtr<Object>& out,
			bool bImmediate = true) override;
		SAILOR_API Tasks::TaskPtr<ProbeVolumeAssetPtr> LoadProbeVolume(
			FileId uid,
			ProbeVolumeAssetPtr& outAsset);
		SAILOR_API bool LoadProbeVolume_Immediate(
			FileId uid,
			ProbeVolumeAssetPtr& outAsset);
		SAILOR_API void RetainRuntimeProbeVolume(FileId uid);
		SAILOR_API void ReleaseRuntimeProbeVolume(FileId uid);
		SAILOR_API void CollectGarbage() override;

	private:
		bool ImportProbeVolume(FileId uid, ProbeVolumeAssetPtr& outAsset);
		void TryEvictReleasedProbeVolume(FileId uid);

		TConcurrentMap<FileId, Tasks::TaskPtr<ProbeVolumeAssetPtr>> m_promises{};
		TConcurrentMap<FileId, ProbeVolumeAssetPtr> m_loadedAssets{};
		// Entries exist only for assets explicitly retained by runtime GI
		// bindings. A zero count is a pending eviction once loading completes.
		TConcurrentMap<FileId, uint32_t> m_runtimeRetentions{};
		Memory::ObjectAllocatorPtr m_allocator;
		ProbeVolumeAssetInfoHandler* m_infoHandler = nullptr;
	};
}
