#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include "Containers/Set.h"
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
#include "Engine/GlobalIlluminationSettings.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include "Core/Reflection.h"
#include "Tasks/Tasks.h"

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

		SAILOR_API const std::string& GetName() const { return m_name; }
		SAILOR_API const TVector<PrefabPtr>& GetGameObjects() const { return m_gameObjects; }
		SAILOR_API const GlobalIlluminationWorldSettings& GetGlobalIlluminationSettings() const
		{
			return m_globalIllumination;
		}
		SAILOR_API const std::string& GetLoadDiagnostic() const { return m_loadDiagnostic; }

		static WorldPrefabPtr FromWorld(WorldPtr world);

	protected:

		struct PendingPrefabLinkUpdate final
		{
			InstanceId m_rootInstanceId{};
			TMap<InstanceId, InstanceId> m_sourceToInstanceIds{};
			PrefabPtr m_effectiveBaseline{};
		};

		SAILOR_API static bool ReconcileLinkedInstanceIds(
			const PrefabPtr& expandedPrefab,
			const PrefabPtr& sourcePrefab,
			const TMap<InstanceId, InstanceId>& savedSourceToInstanceIds,
			TSet<InstanceId>& reservedInstanceIds,
			TMap<InstanceId, InstanceId>& outSourceToInstanceIds,
			std::string& outDiagnostic);

		static bool BuildLinkedOverrides(
			const PrefabPtr& expandedPrefab,
			const PrefabPtr& sourcePrefab,
			const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
			TMap<InstanceId, YAML::Node>& outGameObjectOverrides,
			TMap<InstanceId, ReflectedData>& outComponentOverrides,
			std::string& outDiagnostic);

		SAILOR_API static bool BuildUpdatedLinkedOverrides(
			const PrefabPtr& expandedPrefab,
			const PrefabPtr& sourcePrefab,
			const PrefabPtr& effectiveBaseline,
			const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
			TMap<InstanceId, YAML::Node>& outGameObjectOverrides,
			TMap<InstanceId, ReflectedData>& outComponentOverrides,
			std::string& outDiagnostic);

		SAILOR_API static bool CommitLinkedInstanceUpdates(
			WorldPtr world,
			TVector<PendingPrefabLinkUpdate>& pendingUpdates,
			std::string& outDiagnostic);

		std::atomic<bool> m_bIsReady{};

		std::string m_name{};
		std::string m_loadDiagnostic{};
		GlobalIlluminationWorldSettings m_globalIllumination{};
		TVector<PrefabPtr> m_gameObjects;

		friend class WorldPrefabImporter;
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
		WorldPrefabAssetInfoHandler* m_worldInfoHandler{};
		PrefabAssetInfoHandler* m_prefabInfoHandler{};
	};
}
