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
#include "Core/Reflection.h"
#include "Math/Math.h"
#include "Math/Transform.h"
#if defined(_WIN32)
#include <libloaderapi.h>
#endif
#include <queue>
#include <string>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>
#include "Platform/Win32/Window.h"

using namespace Sailor;

namespace
{
	bool IsDescendantOf(GameObjectPtr object, GameObjectPtr possibleParent)
	{
		for (auto current = object; current.IsValid(); current = current->GetParent())
		{
			if (current == possibleParent)
			{
				return true;
			}
		}

		return false;
	}

	glm::mat4 CalculateCurrentWorldMatrix(GameObjectPtr gameObject)
	{
		glm::mat4 worldMatrix = glm::identity<glm::mat4>();
		for (auto current = gameObject; current.IsValid(); current = current->GetParent())
		{
			worldMatrix = current->GetTransformComponent().GetTransform().Matrix() * worldMatrix;
		}

		return worldMatrix;
	}

	bool IsFiniteMatrix(const glm::mat4& matrix)
	{
		for (glm::length_t column = 0; column < matrix.length(); ++column)
		{
			for (glm::length_t row = 0; row < matrix[column].length(); ++row)
			{
				if (!std::isfinite(matrix[column][row]))
				{
					return false;
				}
			}
		}

		return true;
	}

	bool AreMatricesNear(const glm::mat4& lhs, const glm::mat4& rhs, float tolerance = 0.0001f)
	{
		for (glm::length_t column = 0; column < lhs.length(); ++column)
		{
			for (glm::length_t row = 0; row < lhs[column].length(); ++row)
			{
				const float scale = std::max({ 1.0f, std::abs(lhs[column][row]), std::abs(rhs[column][row]) });
				if (std::abs(lhs[column][row] - rhs[column][row]) > tolerance * scale)
				{
					return false;
				}
			}
		}

		return true;
	}

	bool TryInvertTransformMatrix(const glm::mat4& matrix, glm::mat4& outInverse)
	{
		if (!IsFiniteMatrix(matrix))
		{
			return false;
		}

		const glm::vec3 axisX(matrix[0]);
		const glm::vec3 axisY(matrix[1]);
		const glm::vec3 axisZ(matrix[2]);
		const float axisLengthProduct = glm::length(axisX) * glm::length(axisY) * glm::length(axisZ);
		const float normalizedVolume = axisLengthProduct > 0.0f
			? std::abs(glm::dot(glm::cross(axisX, axisY), axisZ)) / axisLengthProduct
			: 0.0f;
		if (!std::isfinite(normalizedVolume) || normalizedVolume <= 0.000001f)
		{
			return false;
		}

		outInverse = glm::inverse(matrix);
		return IsFiniteMatrix(outInverse) &&
			AreMatricesNear(matrix * outInverse, glm::identity<glm::mat4>(), 0.001f);
	}

	bool TryMakeExactTransform(const glm::mat4& matrix, Math::Transform& outTransform)
	{
		if (!IsFiniteMatrix(matrix))
		{
			return false;
		}

		outTransform = Math::Transform::FromMatrix(matrix);
		const glm::vec4 rotation(
			outTransform.m_rotation.x,
			outTransform.m_rotation.y,
			outTransform.m_rotation.z,
			outTransform.m_rotation.w);
		if (!Math::AllFinite(outTransform.m_position) ||
			!Math::AllFinite(rotation) ||
			!Math::AllFinite(outTransform.m_scale))
		{
			return false;
		}

		return AreMatricesNear(outTransform.Matrix(), matrix);
	}

	bool ResolveParent(World* world, const InstanceId& parentInstanceId, GameObjectPtr& outParent)
	{
		outParent = {};
		if (!parentInstanceId)
		{
			return true;
		}

		if (!world)
		{
			return false;
		}
		if (!parentInstanceId.IsGameObjectId())
		{
			return false;
		}

		auto parentObject = world->GetObjectByInstanceId(parentInstanceId);
		outParent = parentObject.DynamicCast<GameObject>();
		return outParent.IsValid();
	}
}

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
	if (!m_world)
	{
		return false;
	}

	auto objPtr = m_world->GetObjectByInstanceId(instanceId.GameObjectId());

	if (instanceId.ComponentId() != Sailor::InstanceId::Invalid && objPtr.IsValid())
	{
		ReflectedData overrideData;
		YAML::Node objectYaml = YAML::Load(strYamlNode);
		overrideData.Deserialize(objectYaml);
		if (!overrideData.IsValid())
		{
			return false;
		}

		auto go = objPtr.DynamicCast<GameObject>();
		auto components = go->GetComponents();

		for (auto el : components)
		{
			if (el->GetInstanceId().ComponentId() == instanceId.ComponentId())
			{
				if (el->GetTypeInfo().Name() != overrideData.GetTypeInfo().Name())
				{
					return false;
				}

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

	if (!m_world || !instanceId.IsGameObjectId())
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
	if (!ResolveParent(m_world, parentInstanceId, parentGameObject))
	{
		return false;
	}

	if (parentGameObject)
	{
		if (parentGameObject == gameObject)
		{
			return false;
		}

		if (IsDescendantOf(parentGameObject, gameObject))
		{
			return false;
		}
	}

	if (gameObject->GetParent() == parentGameObject)
	{
		return true;
	}

	Math::Transform localTransform;
	if (bKeepWorldTransform)
	{
		const glm::mat4 oldWorldMatrix = CalculateCurrentWorldMatrix(gameObject);
		glm::mat4 localMatrix = oldWorldMatrix;
		if (parentGameObject)
		{
			glm::mat4 inverseParentMatrix;
			if (!TryInvertTransformMatrix(CalculateCurrentWorldMatrix(parentGameObject), inverseParentMatrix))
			{
				return false;
			}

			localMatrix = inverseParentMatrix * oldWorldMatrix;
		}

		if (!TryMakeExactTransform(localMatrix, localTransform))
		{
			return false;
		}
	}

	gameObject->SetParent(parentGameObject);

	if (bKeepWorldTransform)
	{
		auto& transform = gameObject->GetTransformComponent();
		transform.SetPosition(localTransform.m_position);
		transform.SetRotation(localTransform.m_rotation);
		transform.SetScale(localTransform.m_scale);
	}

	return true;
}

bool Editor::CreateGameObject(const InstanceId& parentInstanceId, const InstanceId& preferredInstanceId, InstanceId& outInstanceId)
{
	SAILOR_PROFILE_FUNCTION();
	outInstanceId = InstanceId::Invalid;

	if (!m_world)
	{
		return false;
	}

	GameObjectPtr parentGameObject;
	if (!ResolveParent(m_world, parentInstanceId, parentGameObject))
	{
		return false;
	}

	auto gameObject = preferredInstanceId
		? m_world->Instantiate("GameObject", preferredInstanceId)
		: m_world->Instantiate("GameObject");
	if (!gameObject)
	{
		return false;
	}

	if (parentGameObject)
	{
		gameObject->SetParent(parentGameObject);
	}

	outInstanceId = gameObject->GetInstanceId();
	return true;
}

bool Editor::DestroyObject(const InstanceId& instanceId)
{
	SAILOR_PROFILE_FUNCTION();

	if (!m_world || !instanceId.IsGameObjectId())
	{
		return false;
	}

	auto object = m_world->GetObjectByInstanceId(instanceId.GameObjectId());
	auto gameObject = object.DynamicCast<GameObject>();
	if (!gameObject)
	{
		return false;
	}

	m_world->DestroyImmediate(gameObject);
	return true;
}

bool Editor::ResetComponentToDefaults(const InstanceId& instanceId)
{
	SAILOR_PROFILE_FUNCTION();

	if (!m_world || !instanceId || instanceId.ComponentId() == InstanceId::Invalid)
	{
		return false;
	}

	auto object = m_world->GetObjectByInstanceId(instanceId.GameObjectId());
	auto gameObject = object.DynamicCast<GameObject>();
	if (!gameObject)
	{
		return false;
	}

	for (uint32_t i = 0; i < gameObject->GetComponents().Num(); i++)
	{
		auto component = gameObject->GetComponent(i);
		if (component->GetInstanceId().ComponentId() != instanceId.ComponentId())
		{
			continue;
		}

		const ReflectedData& defaults = Reflection::GetCDO(component->GetTypeInfo().Name());
		component->ApplyReflection(defaults);
		component->ResolveRefs(defaults, m_world->GetObjects(), true);
		return true;
	}

	return false;
}

bool Editor::AddComponent(
	const InstanceId& instanceId,
	const std::string& componentTypeName,
	const InstanceId& preferredInstanceId,
	InstanceId& outInstanceId)
{
	SAILOR_PROFILE_FUNCTION();
	outInstanceId = InstanceId::Invalid;

	if (!m_world || !instanceId.IsGameObjectId() || componentTypeName.empty())
	{
		return false;
	}

	auto object = m_world->GetObjectByInstanceId(instanceId.GameObjectId());
	auto gameObject = object.DynamicCast<GameObject>();
	if (!gameObject)
	{
		return false;
	}

	const TypeInfo* componentType = Reflection::TryGetTypeByName(componentTypeName);
	if (componentType == nullptr)
	{
		return false;
	}

	auto component = Reflection::CreateObject<Component>(*componentType, m_world->GetAllocator());
	if (!component)
	{
		return false;
	}

	component = gameObject->AddComponentRaw(component, preferredInstanceId);
	if (!component)
	{
		return false;
	}

	const ReflectedData& defaults = Reflection::GetCDO(componentTypeName);
	component->ApplyReflection(defaults);
	component->ResolveRefs(defaults, m_world->GetObjects(), true);
	outInstanceId = component->GetInstanceId();
	return true;
}

bool Editor::RemoveComponent(const InstanceId& instanceId)
{
	SAILOR_PROFILE_FUNCTION();

	if (!m_world || !instanceId || instanceId.ComponentId() == InstanceId::Invalid)
	{
		return false;
	}

	auto object = m_world->GetObjectByInstanceId(instanceId.GameObjectId());
	auto gameObject = object.DynamicCast<GameObject>();
	if (!gameObject)
	{
		return false;
	}

	for (uint32_t i = 0; i < gameObject->GetComponents().Num(); i++)
	{
		auto component = gameObject->GetComponent(i);
		if (component->GetInstanceId().ComponentId() == instanceId.ComponentId())
		{
			return gameObject->RemoveComponent(component);
		}
	}

	return false;
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

	return InstantiatePrefab(prefab, parentInstanceId);
}

bool Editor::InstantiatePrefab(const PrefabPtr& prefab, const InstanceId& parentInstanceId)
{
	SAILOR_PROFILE_FUNCTION();

	if (!m_world || !prefab)
	{
		return false;
	}

	GameObjectPtr parentGameObject;
	if (!ResolveParent(m_world, parentInstanceId, parentGameObject))
	{
		return false;
	}

	auto root = m_world->Instantiate(prefab);
	if (!root)
	{
		return false;
	}

	if (parentGameObject)
	{
		root->SetParent(parentGameObject);
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
