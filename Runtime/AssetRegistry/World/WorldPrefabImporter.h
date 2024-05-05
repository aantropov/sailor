#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "Engine/Types.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabAssetInfo.h"
#include "AssetRegistry/AssetFactory.h"
#include "WorldPrefabAssetInfo.h"
#include "Tasks/Scheduler.h"
#include "Engine/Object.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include "Core/Reflection.h"

using namespace Sailor::Memory;

namespace Sailor
{
	using WorldPrefabPtr = TObjectPtr<class WorldPrefab>;

	class WorldPrefab : public Object, public IYamlSerializable
	{
	public:
		SAILOR_API WorldPrefab(FileId uid) :
			Object(std::move(uid)) {}

		SAILOR_API virtual bool IsReady() const override { return m_bIsReady; }
		SAILOR_API virtual ~WorldPrefab() = default;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

		SAILOR_API bool SaveToFile(const std::string& path) const;

		static WorldPrefabPtr FromWorld(WorldPtr go);

	protected:

		std::atomic<bool> m_bIsReady{};

		std::string m_name{};
		TVector<PrefabPtr> m_gameObjects;

		friend class WorldImporter;
		friend class World;
	};

	class WorldPrefabImporter final : public TSubmodule<WorldPrefabImporter>, public IAssetInfoHandlerListener, public IAssetFactory
	{
	public:

		SAILOR_API WorldPrefabImporter(WorldPrefabAssetInfoHandler* infoHandler);
		SAILOR_API virtual ~WorldPrefabImporter() override;

		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override;

		SAILOR_API bool LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate = true) override;

		SAILOR_API Tasks::TaskPtr<WorldPrefabPtr> LoadWorld(FileId uid, WorldPrefabPtr& outModel);
		SAILOR_API bool LoadWorld_Immediate(FileId uid, WorldPrefabPtr& outModel);

		SAILOR_API virtual void CollectGarbage() override;

		SAILOR_API WorldPrefabPtr Create();

	protected:

		TConcurrentMap<FileId, Tasks::TaskPtr<WorldPrefabPtr>> m_promises;
		TConcurrentMap<FileId, WorldPrefabPtr> m_loadedWorldPrefabs;

		ObjectAllocatorPtr m_allocator;
	};
}
