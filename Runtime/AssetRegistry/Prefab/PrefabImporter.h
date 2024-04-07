#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "Engine/Types.h"
#include "AssetRegistry/AssetInfo.h"
#include "PrefabAssetInfo.h"
#include "Tasks/Scheduler.h"
#include "PrefabAssetInfo.h"
#include "Engine/Object.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include "Core/Reflection.h"

using namespace Sailor::Memory;

namespace Sailor
{
	using PrefabPtr = TObjectPtr<class Prefab>;

	class Prefab : public Object
	{
	public:

		SAILOR_API Prefab(FileId uid, ReflectionInfo reflection = {}) :
			Object(std::move(uid)),
			m_reflectionInfo(std::move(reflection)) {}

		SAILOR_API virtual bool IsReady() const override { return m_bIsReady; }
		SAILOR_API virtual ~Prefab() = default;

	protected:

		std::atomic<bool> m_bIsReady{};
		ReflectionInfo m_reflectionInfo{};

		friend class PrefabImporter;
	};

	class PrefabImporter final : public TSubmodule<PrefabImporter>, public IAssetInfoHandlerListener
	{
	public:

		SAILOR_API PrefabImporter(PrefabAssetInfoHandler* infoHandler);
		SAILOR_API virtual ~PrefabImporter() override;

		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override;

		SAILOR_API Tasks::TaskPtr<PrefabPtr> LoadPrefab(FileId uid, PrefabPtr& outModel);
		SAILOR_API bool LoadPrefab_Immediate(FileId uid, PrefabPtr& outModel);

		SAILOR_API virtual void CollectGarbage() override;

	protected:

		TConcurrentMap<FileId, Tasks::TaskPtr<PrefabPtr>> m_promises;
		TConcurrentMap<FileId, PrefabPtr> m_loadedPrefabs;

		ObjectAllocatorPtr m_allocator;
	};
}
