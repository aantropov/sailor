#include "PrefabImporter.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "PrefabAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include "Memory/ObjectAllocator.hpp"
#include "Tasks/Scheduler.h"
#include "Engine/GameObject.h"
#include "ECS/TransformECS.h"
#include "Containers/Set.h"
#include "YamlExceptionBoundary.h"

using namespace Sailor;

YAML::Node Prefab::ReflectedGameObject::Serialize() const
{
	YAML::Node outData;

	SERIALIZE_PROPERTY(outData, m_name);
	SERIALIZE_PROPERTY(outData, m_position);
	SERIALIZE_PROPERTY(outData, m_rotation);
	SERIALIZE_PROPERTY(outData, m_scale);
	SERIALIZE_PROPERTY(outData, m_parentIndex);
	SERIALIZE_PROPERTY(outData, m_instanceId);
	SERIALIZE_PROPERTY(outData, m_components);

	return outData;
}

void Prefab::ReflectedGameObject::Deserialize(const YAML::Node& inData)
{
	DESERIALIZE_PROPERTY(inData, m_name);
	DESERIALIZE_PROPERTY(inData, m_position);
	DESERIALIZE_PROPERTY(inData, m_rotation);
	DESERIALIZE_PROPERTY(inData, m_scale);
	m_bHasParentIndex = Sailor::Deserialize(inData, "parentIndex", m_parentIndex);
	DESERIALIZE_PROPERTY(inData, m_instanceId);
	DESERIALIZE_PROPERTY(inData, m_components);
}

YAML::Node Prefab::Serialize() const
{
	YAML::Node outData;

	SERIALIZE_PROPERTY(outData, m_gameObjects);
	SERIALIZE_PROPERTY(outData, m_components);

	return outData;
}

void Prefab::Deserialize(const YAML::Node& inData)
{
	DESERIALIZE_PROPERTY(inData, m_gameObjects);
	DESERIALIZE_PROPERTY(inData, m_components);
}

bool Prefab::ValidateForInstantiation(std::string& outDiagnostic) const
{
	outDiagnostic.clear();
	if (m_gameObjects.IsEmpty())
	{
		outDiagnostic = "the prefab has no game objects";
		return false;
	}

	TSet<InstanceId> componentInstanceIds;
	TVector<InstanceId> componentInstanceIdsByIndex;
	componentInstanceIdsByIndex.Reserve(m_components.Num());
	for (uint32_t componentIndex = 0; componentIndex < m_components.Num(); ++componentIndex)
	{
		const ReflectedData& reflection = m_components[componentIndex];
		if (!reflection.IsValid())
		{
			outDiagnostic = "reflected component " + std::to_string(componentIndex) +
				" has an unknown type; load or rebuild the workspace logic module";
			return false;
		}

		const auto& properties = reflection.GetProperties();
		if (!properties.ContainsKey("instanceId"))
		{
			outDiagnostic = "reflected component " + std::to_string(componentIndex) +
				" has no instanceId";
			return false;
		}

		InstanceId componentInstanceId;
		std::string conversionDiagnostic;
		if (!External::TryConvertYaml(properties["instanceId"], componentInstanceId, conversionDiagnostic) ||
			componentInstanceId.ComponentId() == InstanceId::Invalid ||
			componentInstanceId.GameObjectId() == InstanceId::Invalid)
		{
			outDiagnostic = "reflected component " + std::to_string(componentIndex) +
				" has an invalid instanceId";
			if (!conversionDiagnostic.empty())
			{
				outDiagnostic += ": " + conversionDiagnostic;
			}
			return false;
		}

		if (!componentInstanceIds.Insert(componentInstanceId))
		{
			outDiagnostic = "reflected component " + std::to_string(componentIndex) +
				" has a duplicate instanceId";
			return false;
		}

		componentInstanceIdsByIndex.Add(componentInstanceId);
	}

	const uint32_t invalidParentIndex = static_cast<uint32_t>(-1);
	uint32_t numRoots = 0;
	TSet<InstanceId> gameObjectInstanceIds;
	TSet<uint32_t> referencedComponentIndices;
	for (uint32_t gameObjectIndex = 0; gameObjectIndex < m_gameObjects.Num(); ++gameObjectIndex)
	{
		const auto& gameObject = m_gameObjects[gameObjectIndex];
		if (!gameObject.m_bHasParentIndex)
		{
			outDiagnostic = "game object " + std::to_string(gameObjectIndex) +
				" has no parentIndex";
			return false;
		}

		if (!gameObject.m_instanceId.IsGameObjectId())
		{
			outDiagnostic = "game object " + std::to_string(gameObjectIndex) +
				" has an invalid instanceId";
			return false;
		}

		if (!gameObjectInstanceIds.Insert(gameObject.m_instanceId))
		{
			outDiagnostic = "game object " + std::to_string(gameObjectIndex) +
				" has a duplicate instanceId";
			return false;
		}

		if (gameObject.m_parentIndex == invalidParentIndex)
		{
			++numRoots;
		}
		else if (gameObject.m_parentIndex >= m_gameObjects.Num())
		{
			outDiagnostic = "game object " + std::to_string(gameObjectIndex) +
				" has invalid parent index " + std::to_string(gameObject.m_parentIndex);
			return false;
		}

		for (const uint32_t componentIndex : gameObject.m_components)
		{
			if (componentIndex >= m_components.Num())
			{
				outDiagnostic = "game object " + std::to_string(gameObjectIndex) +
					" references invalid component index " + std::to_string(componentIndex);
				return false;
			}

			if (!referencedComponentIndices.Insert(componentIndex))
			{
				outDiagnostic = "component index " + std::to_string(componentIndex) +
					" is referenced more than once";
				return false;
			}

			if (componentInstanceIdsByIndex[componentIndex].GameObjectId() != gameObject.m_instanceId)
			{
				outDiagnostic = "reflected component " + std::to_string(componentIndex) +
					" belongs to a different game object";
				return false;
			}
		}
	}

	if (referencedComponentIndices.Num() != m_components.Num())
	{
		outDiagnostic = "the prefab contains an unreferenced reflected component";
		return false;
	}

	if (numRoots != 1)
	{
		outDiagnostic = "the prefab must contain exactly one root game object";
		return false;
	}

	for (uint32_t gameObjectIndex = 0; gameObjectIndex < m_gameObjects.Num(); ++gameObjectIndex)
	{
		uint32_t ancestorIndex = gameObjectIndex;
		bool bReachedRoot = false;
		for (uint32_t depth = 0; depth < m_gameObjects.Num(); ++depth)
		{
			const uint32_t parentIndex = m_gameObjects[ancestorIndex].m_parentIndex;
			if (parentIndex == invalidParentIndex)
			{
				bReachedRoot = true;
				break;
			}

			ancestorIndex = parentIndex;
		}

		if (!bReachedRoot)
		{
			outDiagnostic = "the prefab hierarchy contains a parent cycle at game object " +
				std::to_string(gameObjectIndex);
			return false;
		}
	}

	return true;
}

bool Prefab::SaveToFile(const std::string& path) const
{
	AssetRegistry::WriteTextFile(path, Serialize());
	return true;
}

void Prefab::SerializeGameObject(GameObjectPtr root, uint32_t parentIndex, TVector<ReflectedData>& components, TVector<Prefab::ReflectedGameObject>& gameObjects)
{
	Prefab::ReflectedGameObject rootData{};

	auto& transform = root->GetTransformComponent();
	rootData.m_position = transform.GetPosition();
	rootData.m_rotation = transform.GetRotation();
	rootData.m_scale = transform.GetScale();

	rootData.m_name = root->GetName();
	rootData.m_parentIndex = parentIndex;
	rootData.m_instanceId = root->GetInstanceId();

	for (auto& component : root->GetComponents())
	{
		auto reflection = component->GetReflectedData();

		rootData.m_components.Add((uint32_t)components.Num());
		components.Add(reflection);
	}

	parentIndex = (uint32_t)gameObjects.Num();
	gameObjects.Add(rootData);

	for (auto& child : root->GetChildren())
	{
		SerializeGameObject(child, parentIndex, components, gameObjects);
	}
}

bool Prefab::GetOverridePrefab(const PrefabPtr base, PrefabPtr outOverride) const
{
	if (base->GetFileId() != GetFileId())
	{
		return false;
	}

	if (base->m_gameObjects.Num() != m_gameObjects.Num() ||
		base->m_components.Num() != m_components.Num())
	{
		return false;
	}

	PrefabPtr res = App::GetSubmodule<PrefabImporter>()->Create();

	res->m_components.Reserve(m_components.Num());
	res->m_gameObjects = m_gameObjects;

	for (uint32_t i = 0; i < m_components.Num(); i++)
	{
		res->m_components.Add(m_components[i].DiffTo(base->m_components[i]));
	}

	return true;
}

PrefabPtr Prefab::FromGameObject(GameObjectPtr root)
{
	PrefabPtr res = App::GetSubmodule<PrefabImporter>()->Create();

	SerializeGameObject(root, -1, res->m_components, res->m_gameObjects);

	return res;
}

PrefabImporter::PrefabImporter(PrefabAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	infoHandler->Subscribe(this);
}

PrefabImporter::~PrefabImporter()
{
	for (auto& model : m_loadedPrefabs)
	{
		model.m_second.DestroyObject(m_allocator);
	}
}

PrefabPtr PrefabImporter::Create()
{
	return PrefabPtr::Make(m_allocator, FileId());
}

void PrefabImporter::OnUpdateAssetInfo(AssetInfoPtr, bool) {}

void PrefabImporter::OnImportAsset(AssetInfoPtr assetInfo) {}

bool PrefabImporter::LoadPrefab_Immediate(FileId uid, PrefabPtr& outPrefab)
{
	SAILOR_PROFILE_FUNCTION();

	auto task = LoadPrefab(uid, outPrefab);
	task->Wait();
	return task->GetResult().IsValid();
}

Tasks::TaskPtr<PrefabPtr> PrefabImporter::LoadPrefab(FileId uid, PrefabPtr& outPrefab)
{
	SAILOR_PROFILE_FUNCTION();

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedPrefab = m_loadedPrefabs.At_Lock(uid, PrefabPtr());

	// Check loaded assets
	if (loadedPrefab)
	{
		outPrefab = loadedPrefab;
		auto res = promise ? promise : Tasks::TaskPtr<PrefabPtr>::Make(outPrefab);

		m_loadedPrefabs.Unlock(uid);
		m_promises.Unlock(uid);

		return res;
	}

	// There is no promise, we need to load prefab
	if (PrefabAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<PrefabAssetInfoPtr>(uid))
	{
		SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath().c_str());

		PrefabPtr prefab = PrefabPtr::Make(m_allocator, uid);

		struct Data {};
		promise = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load prefab",
			[prefab, assetInfo]() mutable
			{
				TSharedPtr<Data> res = TSharedPtr<Data>::Make();

				std::string text;
				AssetRegistry::ReadTextFile(assetInfo->GetAssetFilepath(), text);

				YAML::Node node = YAML::Load(text);
				prefab->Deserialize(node);

				return res;

			}, EThreadType::Worker)->Then<PrefabPtr>([prefab](TSharedPtr<Data> data) mutable
				{
					return prefab;
				}, "Preload resources", EThreadType::RHI)->ToTaskWithResult();

				outPrefab = loadedPrefab = prefab;
				promise->Run();

				m_loadedPrefabs.Unlock(uid);
				m_promises.Unlock(uid);

				return promise;
	}

	outPrefab = nullptr;
	m_loadedPrefabs.Unlock(uid);
	m_promises.Unlock(uid);

	return Tasks::TaskPtr<PrefabPtr>();
}

bool PrefabImporter::LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate)
{
	PrefabPtr outAsset;
	if (bImmediate)
	{
		bool bRes = LoadPrefab_Immediate(uid, outAsset);
		out = outAsset;
		return bRes;
	}

	LoadPrefab(uid, outAsset);
	out = outAsset;
	return true;
}

void PrefabImporter::CollectGarbage()
{
	TVector<FileId> uidsToRemove;

	m_promises.LockAll();
	auto ids = m_promises.GetKeys();
	m_promises.UnlockAll();

	for (const auto& id : ids)
	{
		auto& promise = m_promises.At_Lock(id);

		if (!promise.IsValid() || (promise.IsValid() && promise->IsFinished()))
		{
			FileId uid = id;
			uidsToRemove.Emplace(uid);
		}

		m_promises.Unlock(id);
	}

	for (auto& uid : uidsToRemove)
	{
		m_promises.Remove(uid);
	}
}
