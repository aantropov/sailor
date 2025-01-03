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
			glm::quat m_rotation{};
			glm::vec4 m_scale{};

			// We store indices
			uint32_t m_parentIndex;
			TVector<uint32_t> m_components;
			InstanceId m_instanceId;

			SAILOR_API virtual YAML::Node Serialize() const override;
			SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;
		};

		SAILOR_API Prefab(FileId uid) :
			Object(std::move(uid)) {}

		SAILOR_API virtual bool IsReady() const override { return m_bIsReady; }
		SAILOR_API virtual ~Prefab() = default;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

		SAILOR_API bool SaveToFile(const std::string& path) const;

		static PrefabPtr FromGameObject(GameObjectPtr go);

		SAILOR_API bool GetOverridePrefab(const PrefabPtr base, PrefabPtr outOverride) const;

	protected:

		static void SerializeGameObject(GameObjectPtr root, uint32_t parentIndex, TVector<ReflectedData>& components, TVector<Prefab::ReflectedGameObject>& gameObjects);

		std::atomic<bool> m_bIsReady{};

		TVector<ReflectedData> m_components{};
		TVector<ReflectedGameObject> m_gameObjects{};

		friend class PrefabImporter;

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

	protected:

		TConcurrentMap<FileId, Tasks::TaskPtr<PrefabPtr>> m_promises;
		TConcurrentMap<FileId, PrefabPtr> m_loadedPrefabs;

		ObjectAllocatorPtr m_allocator;
	};
}
