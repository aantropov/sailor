#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "Engine/Types.h"
#include "AssetRegistry/Prefab/PrefabAssetInfo.h"
#include "AssetRegistry/AssetFactory.h"
#include "PrefabAssetInfo.h"
#include "Tasks/Scheduler.h"
#include "PrefabAssetInfo.h"
#include "Engine/Object.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include "Core/Reflection.h"
#include "Tasks/Tasks.h"
#include "Containers/Set.h"

using namespace Sailor::Memory;

namespace Sailor
{
	using PrefabPtr = TObjectPtr<class Prefab>;

	class Prefab : public Object, public IYamlSerializable
	{
	public:

		class ReflectedGameObject final : public IYamlSerializable
		{
		public:

			std::string m_name{};
			EMobilityType m_mobilityType{};

			glm::vec4 m_position{};
			glm::quat m_rotation = glm::identity<glm::quat>();
			glm::vec4 m_scale{ 1.0f };

			// We store indices
			uint32_t m_parentIndex = static_cast<uint32_t>(-1);
			TVector<uint32_t> m_components;
			InstanceId m_instanceId;

			SAILOR_API virtual YAML::Node Serialize() const override;
			SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

		private:

			bool m_bHasParentIndex = true;

			friend class Prefab;
		};

		SAILOR_API Prefab(FileId uid) :
			Object(std::move(uid)) {}

		SAILOR_API virtual bool IsReady() const override { return m_bIsReady; }
		SAILOR_API virtual ~Prefab() = default;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;
		SAILOR_API bool ValidateForInstantiation(std::string& outDiagnostic) const;

		SAILOR_API bool SaveToFile(const std::string& path) const;

		SAILOR_API bool GetOverridePrefab(
			const PrefabPtr base,
			PrefabPtr outOverride) const;

		static PrefabPtr FromGameObject(
			GameObjectPtr go,
			const FileId& sourcePrefabId = FileId::Invalid,
			const TSet<InstanceId>* excludedRoots = nullptr);

		SAILOR_API bool ConfigureLinkedInstance(
			const PrefabPtr& basePrefab,
			const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
			const InstanceId& parentInstanceId,
			const TMap<InstanceId, YAML::Node>& gameObjectOverrides,
			const TMap<InstanceId, ReflectedData>& componentOverrides,
			std::string& outDiagnostic);

		SAILOR_API bool AppendDetachedSupplementalHierarchy(
			const PrefabPtr& expandedPrefab,
			std::string& outDiagnostic);

		SAILOR_API bool IsLinkedInstanceRecord() const { return m_bLinkedInstanceRecord; }
		SAILOR_API const InstanceId& GetLinkedParentInstanceId() const { return m_linkedParentInstanceId; }
		SAILOR_API const TMap<InstanceId, InstanceId>& GetLinkedInstanceIds() const { return m_linkedInstanceIds; }
		SAILOR_API const TMap<InstanceId, YAML::Node>& GetLinkedGameObjectOverrides() const { return m_gameObjectOverrides; }
		SAILOR_API const TMap<InstanceId, ReflectedData>& GetLinkedComponentOverrides() const { return m_componentOverrides; }
		SAILOR_API bool IsDetachedFromPrefabRecord() const { return m_bDetachedFromPrefabRecord; }
		SAILOR_API const InstanceId& GetDetachedParentInstanceId() const { return m_detachedParentInstanceId; }
		SAILOR_API bool IsLinkedPrefabSnapshotRecord() const { return m_bLinkedPrefabSnapshotRecord; }
		SAILOR_API const FileId& GetLinkedSnapshotSourceFileId() const { return m_linkedSnapshotSourceFileId; }

	protected:

		static void SerializeGameObject(
			GameObjectPtr root,
			uint32_t parentIndex,
			TVector<ReflectedData>& components,
			TVector<Prefab::ReflectedGameObject>& gameObjects,
			const TSet<InstanceId>* excludedRoots);

		std::atomic<bool> m_bIsReady{};

		TVector<ReflectedData> m_components{};
		TVector<ReflectedGameObject> m_gameObjects{};
		TMap<InstanceId, InstanceId> m_linkedInstanceIds{};
		TMap<InstanceId, YAML::Node> m_gameObjectOverrides{};
		TMap<InstanceId, ReflectedData> m_componentOverrides{};
		TSet<InstanceId> m_detachedSupplementalInstanceIds{};
		FileId m_linkedSnapshotSourceFileId{};
		InstanceId m_linkedParentInstanceId{};
		InstanceId m_detachedParentInstanceId{};
		bool m_bLinkedInstanceRecord = false;
		bool m_bExpandedLinkedInstanceRecord = false;
		bool m_bDetachedFromPrefabRecord = false;
		bool m_bLinkedPrefabSnapshotRecord = false;

		friend class PrefabImporter;
		friend class WorldPrefab;

		// We need that for object instantiation
		friend class World;
	};

	class PrefabImporter final : public TSubmodule<PrefabImporter>, public IAssetInfoHandlerListener, public IAssetFactory
	{
	public:

		SAILOR_API PrefabImporter(PrefabAssetInfoHandler* infoHandler);
		SAILOR_API virtual ~PrefabImporter() override;

		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override;

		SAILOR_API bool LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate = true) override;
		SAILOR_API Tasks::TaskPtr<PrefabPtr> LoadPrefab(FileId uid, PrefabPtr& outPrefab);
		SAILOR_API bool LoadPrefab_Immediate(FileId uid, PrefabPtr& outPrefab);

		SAILOR_API virtual void CollectGarbage() override;

		SAILOR_API PrefabPtr Create();
		SAILOR_API PrefabPtr Create(const FileId& uid);

	protected:

		TConcurrentMap<FileId, Tasks::TaskPtr<PrefabPtr>> m_promises;
		TConcurrentMap<FileId, PrefabPtr> m_loadedPrefabs;

		ObjectAllocatorPtr m_allocator;
	};
}
