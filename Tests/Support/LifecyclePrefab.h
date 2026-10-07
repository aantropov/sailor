#pragma once

#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "LifecycleTestComponent.h"
#include <array>

namespace Sailor::Tests
{
	inline YAML::Node MakeLifecyclePrefabDocument()
	{
		const std::array<InstanceId, 2> objects{
			InstanceId::GenerateNewInstanceId(), InstanceId::GenerateNewInstanceId() };
		const std::array<InstanceId, 2> components{
			InstanceId::GenerateNewComponentId(objects[0]), InstanceId::GenerateNewComponentId(objects[1]) };
		YAML::Node document;
		for (uint32_t i = 0; i < objects.size(); ++i)
		{
			Prefab::ReflectedGameObject object;
			object.m_name = i ? "Lifecycle child" : "Lifecycle root";
			object.m_instanceId = objects[i];
			object.m_parentIndex = i ? 0 : static_cast<uint32_t>(-1);
			object.m_position = glm::vec4(10.0f + i, 2, 3, 1);
			object.m_components.Add(i);
			document["gameObjects"].push_back(object.Serialize());
			YAML::Node component;
			component["typename"] = LifecycleTestComponent::GetStaticTypeInfo().Name();
			auto properties = component["overrideProperties"];
			properties["instanceId"] = components[i].ToString();
			properties["value"] = i ? 47.0f : 31.0f;
			properties["m_dependency"]["fileId"] = "NullFileId";
			properties["m_dependency"]["instanceId"] = components[1 - i].ToString();
			document["components"].push_back(component);
		}
		return document;
	}
}
