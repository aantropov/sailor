#pragma once
#include "PrefabImporter.h"

namespace Sailor::PrefabInstance
{
	// Built once at the input boundary; the indexed prefab is read-only for this operation.
	struct Snapshot
	{
		PrefabPtr m_prefab;
		TMap<InstanceId, uint32_t> m_gameObjects;
		TMap<InstanceId, uint32_t> m_components;
		TVector<InstanceId> m_componentIds;

		bool Build(PrefabPtr prefab, std::string& outDiagnostic);
		const Prefab::ReflectedGameObject* FindGameObject(const InstanceId& id) const;
		const ReflectedData* FindComponent(const InstanceId& id) const;
	};

	YAML::Node NormalizeReferences(const YAML::Node& node,
		const TMap<InstanceId, InstanceId>& instanceToSourceIds);
}
