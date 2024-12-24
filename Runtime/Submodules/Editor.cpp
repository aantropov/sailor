#include "Core/LogMacros.h"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "ECS/TransformECS.h"
#include "Editor.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include <libloaderapi.h>
#include <queue>
#include <string>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "Platform/Win32/Window.h"

using namespace Sailor;

Editor::Editor(HWND editorHwnd, uint32_t editorPort, Sailor::Win32::Window* pMainWindow) :
	m_editorPort(editorPort),
	m_editorHwnd(editorHwnd),
	m_pMainWindow(pMainWindow)
{

}

void Editor::ShowMainWindow(bool bShow)
{
	m_pMainWindow->Show(bShow);
}

bool Editor::UpdateObject(const InstanceId& instanceId, const std::string& strYamlNode)
{
	SAILOR_PROFILE_FUNCTION();

	auto objPtr = m_world->GetObjectByInstanceId(instanceId.GameObjectId());

	if (instanceId.ComponentId() != Sailor::InstanceId::Invalid && objPtr.IsValid())
	{
		ReflectedData overrideData;
		YAML::Node objectYaml = YAML::Load(strYamlNode);
		overrideData.Deserialize(objectYaml);

		auto go = objPtr.DynamicCast<GameObject>();
		auto components = go->GetComponents();

		for (auto el : components)
		{
			if (el->GetInstanceId().ComponentId() == instanceId.ComponentId())
			{
				el->ApplyReflection(overrideData);
				return true;
			}
		}
	}
	else if (instanceId.GameObjectId() != Sailor::InstanceId::Invalid)
	{
		if (!objPtr.IsValid())
		{
			// TODO: Create new gameobject
		}
		else if (auto go = objPtr.DynamicCast<GameObject>())
		{
			Prefab::ReflectedGameObject reflected;
			YAML::Node inData = YAML::Load(strYamlNode);
			reflected.Deserialize(inData);

			go->SetName(reflected.m_name);
			//go->SetMobilityType(reflected.m_mobilityType);

			auto& transform = go->GetTransformComponent();
			transform.SetPosition(reflected.m_position);
			transform.SetRotation(reflected.m_rotation);
			transform.SetScale(reflected.m_scale);

			return true;

			//TVector<ReflectedData> components{};
			//TVector<Prefab::ReflectedGameObject> gameObjects{};
			//TMap<InstanceId, uint32_t> gameObjectMapping;
			//TVector<bool> bUpdated{};

			//YAML::Node inData = YAML::Load(strYamlNode);
			//DESERIALIZE_PROPERTY(inData, gameObjects);
			//DESERIALIZE_PROPERTY(inData, components);

			//for (uint32_t i = 0; i < gameObjects.Num(); i++)
			//{
			//	bUpdated.Add(false);

			//	gameObjectMapping[gameObjects[i].m_instanceId.GameObjectId()] = i;
			//}

			//TVector<GameObjectPtr> stack;
			//do
			//{
			//	auto go = *stack.Last();
			//	stack.RemoveLast();

			//	auto goId = go->GetInstanceId().GameObjectId();

			//	if (gameObjectMapping.ContainsKey(goId))
			//	{
			//		const auto& reflectedData = gameObjects[gameObjectMapping[goId]];

			//		go->SetName(reflectedData.m_name);
			//		go->SetMobilityType(reflectedData.m_mobilityType);

			//		auto& transform = go->GetTransformComponent();
			//		transform.SetPosition(reflectedData.m_position);
			//		transform.SetRotation(reflectedData.m_rotation);
			//		transform.SetScale(reflectedData.m_scale);

			//		// TODO: Resolve parent index
			//		// TODO: Resolve Components
			//		for (uint32_t i = 0; i < go->GetComponents().Num(); i++)
			//		{
			//			go->GetComponent(i)->ApplyReflection(components[i]);
			//		}

			//		bUpdated[gameObjectMapping[goId]] = true;
			//	}
			//	else
			//	{
			//		// TODO: Remove GameObject
			//	}

			//	stack.AddRange(go->GetChildren());
			//} while (stack.Num() > 0);

			//for (uint32_t i = 0; i < gameObjects.Num(); i++)
			//{
			//	if (bUpdated[i] != true)
			//	{
			//		//TODO: new GameObject which is not updated
			//	}
			//}
		}
	}

	return false;
}

void Editor::PushMessage(const std::string& msg)
{
	if (NumMessages() < 1024)
	{
		std::time_t now = std::time(nullptr);
		std::tm localTime;

		errno_t err = localtime_s(&localTime, &now);

		if (err != 0)
		{
			// TODO: Handle error
			//throw std::runtime_error("Failed to get local time");
		}

		std::ostringstream oss;
		oss << '[' << std::put_time(&localTime, "%H:%M:%S") << "] " << msg;

		m_messagesQueue.push(oss.str());
	}
}

bool Editor::PullMessage(std::string& msg)
{
	if (m_messagesQueue.try_pop(msg))
	{
		return true;
	}

	return false;
}

YAML::Node Editor::SerializeWorld() const
{
	SAILOR_PROFILE_FUNCTION();

	if (m_world == nullptr)
	{
		return YAML::Node();
	}

	auto prefab = WorldPrefab::FromWorld(m_world);
	auto node = prefab->Serialize();
	return node;
}
