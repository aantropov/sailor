#include "Core/LogMacros.h"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "ECS/TransformECS.h"
#include "ECS/PathTracerECS.h"
#include "Components/PathTracerProxyComponent.h"
#include "Raytracing/PathTracer.h"
#include "Editor.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/FileId.h"
#include "Math/Transform.h"
#if defined(_WIN32)
#include <libloaderapi.h>
#endif
#include <queue>
#include <string>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cmath>
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
				el->ResolveRefs(overrideData, m_world->GetObjects(), true);

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

bool Editor::ReparentObject(const InstanceId& instanceId, const InstanceId& parentInstanceId, bool bKeepWorldTransform)
{
	SAILOR_PROFILE_FUNCTION();

	if (!m_world)
	{
		return false;
	}

	auto object = m_world->GetObjectByInstanceId(instanceId.GameObjectId());
	if (!object)
	{
		return false;
	}

	auto gameObject = object.DynamicCast<GameObject>();
	if (!gameObject)
	{
		return false;
	}

	GameObjectPtr parentGameObject;
	if (parentInstanceId)
	{
		auto parentObject = m_world->GetObjectByInstanceId(parentInstanceId.GameObjectId());
		parentGameObject = parentObject.DynamicCast<GameObject>();
		if (!parentGameObject || parentGameObject == gameObject)
		{
			return false;
		}
	}

	const glm::mat4 oldWorldMatrix = gameObject->GetTransformComponent().GetCachedWorldMatrix();
	glm::mat4 parentWorldMatrix = glm::identity<glm::mat4>();
	if (parentGameObject)
	{
		parentWorldMatrix = parentGameObject->GetTransformComponent().GetCachedWorldMatrix();
	}

	gameObject->SetParent(parentGameObject);

	if (bKeepWorldTransform)
	{
		const glm::mat4 localMatrix = parentGameObject ? glm::inverse(parentWorldMatrix) * oldWorldMatrix : oldWorldMatrix;
		const auto localTransform = Math::Transform::FromMatrix(localMatrix);
		auto& transform = gameObject->GetTransformComponent();
		transform.SetPosition(localTransform.m_position);
		transform.SetRotation(localTransform.m_rotation);
		transform.SetScale(localTransform.m_scale);
	}

	return true;
}

bool Editor::InstantiatePrefab(const FileId& prefabId, const InstanceId& parentInstanceId)
{
	SAILOR_PROFILE_FUNCTION();

	if (!m_world)
	{
		return false;
	}

	auto prefabImporter = App::GetSubmodule<PrefabImporter>();
	if (!prefabImporter)
	{
		return false;
	}

	PrefabPtr prefab;
	if (!prefabImporter->LoadPrefab_Immediate(prefabId, prefab) || !prefab || !prefab->IsReady())
	{
		return false;
	}

	auto root = m_world->Instantiate(prefab);
	if (!root)
	{
		return false;
	}

	if (parentInstanceId)
	{
		auto parentObject = m_world->GetObjectByInstanceId(parentInstanceId.GameObjectId());
		if (auto parentGameObject = parentObject.DynamicCast<GameObject>())
		{
			root->SetParent(parentGameObject);
		}
	}

	return true;
}

bool Editor::RenderPathTracedImage(const InstanceId& instanceId, const std::string& outputPath, uint32_t height, uint32_t samplesPerPixel, uint32_t maxBounces)
{
	SAILOR_PROFILE_FUNCTION();

	if (!m_world || outputPath.empty())
	{
		return false;
	}

	Raytracing::PathTracer::Params params{};
	params.m_output = outputPath;
	params.m_height = height;
	params.m_maxBounces = maxBounces;

	if (samplesPerPixel > 0)
	{
		params.m_msaa = samplesPerPixel <= 32 ? std::min(4u, samplesPerPixel) : 8u;
		params.m_numSamples = std::max(1u, (uint32_t)std::lround(samplesPerPixel / (float)params.m_msaa));
	}

	Raytracing::PathTracer::ParseCommandLineArgs(params, nullptr, 0);

	return false;
}

void Editor::PushMessage(const std::string& msg)
{
	if (NumMessages() < 1024)
	{
		std::time_t now = std::time(nullptr);
		std::tm localTime;

#if defined(_WIN32)
		errno_t err = localtime_s(&localTime, &now);
		if (err != 0)
		{
			return;
		}
#else
		if (!localtime_r(&now, &localTime))
		{
			return;
		}
#endif

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
