#pragma once

#include "AssetRegistry/AssetFactory.h"
#include "AssetRegistry/GlobalIllumination/GIProbesAssetInfo.h"
#include "GlobalIllumination/GIProbesData.h"
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
	struct GIProbesAssetSnapshot final
	{
		GIProbesDataPtr m_data{};
		uint64_t m_revision = 0u;
		std::string m_diagnostic{};
	};

	class GIProbesAsset final : public Object
	{
	public:
		SAILOR_API explicit GIProbesAsset(FileId uid);
		SAILOR_API bool IsReady() const override;
		SAILOR_API uint64_t GetRevision() const noexcept;
		SAILOR_API GIProbesAssetSnapshot GetSnapshot() const;

	private:
		void ApplyLoadResult(
			GIProbesDataPtr data,
			std::string diagnostic);

		mutable SpinLock m_lock;
		GIProbesDataPtr m_data{};
		std::string m_diagnostic{};
		std::atomic<uint64_t> m_revision{ 0u };
		std::atomic<bool> m_bReady{ false };

		friend class GIProbesImporter;
	};

	using GIProbesAssetPtr = TObjectPtr<GIProbesAsset>;

	class GIProbesImporter final :
		public TSubmodule<GIProbesImporter>,
		public IAssetInfoHandlerListener,
		public IAssetFactory
	{
	public:
		SAILOR_API explicit GIProbesImporter(
			GIProbesAssetInfoHandler* infoHandler);
		SAILOR_API ~GIProbesImporter() override;

		SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override;
		SAILOR_API void OnUpdateAssetInfo(
			AssetInfoPtr assetInfo,
			bool bWasExpired) override;
		SAILOR_API bool LoadAsset(
			FileId uid,
			TObjectPtr<Object>& out,
			bool bImmediate = true) override;
		SAILOR_API Tasks::TaskPtr<GIProbesAssetPtr> LoadGIProbes(
			FileId uid,
			GIProbesAssetPtr& outAsset);
		SAILOR_API bool LoadGIProbes_Immediate(
			FileId uid,
			GIProbesAssetPtr& outAsset);
		SAILOR_API void RetainRuntimeGIProbes(FileId uid);
		SAILOR_API void ReleaseRuntimeGIProbes(FileId uid);
		SAILOR_API void CollectGarbage() override;

	private:
		bool ImportGIProbes(FileId uid, GIProbesAssetPtr& outAsset);
		void TryEvictReleasedGIProbes(FileId uid);

		TConcurrentMap<FileId, Tasks::TaskPtr<GIProbesAssetPtr>> m_promises{};
		TConcurrentMap<FileId, GIProbesAssetPtr> m_loadedAssets{};
		// Entries exist only for assets explicitly retained by runtime GI
		// bindings. A zero count is a pending eviction once loading completes.
		TConcurrentMap<FileId, uint32_t> m_runtimeRetentions{};
		Memory::ObjectAllocatorPtr m_allocator;
		GIProbesAssetInfoHandler* m_infoHandler = nullptr;
	};
}
