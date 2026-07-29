#include "Core/LogMacros.h"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "ECS/TransformECS.h"
#include "ECS/PathTracerECS.h"
#include "Components/PathTracerProxyComponent.h"
#include "Components/EditorComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Raytracing/PathTracer.h"
#include "Editor.h"
#include "Containers/Map.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/FileId.h"
#include "Core/Reflection.h"
#include "Math/Math.h"
#include "Math/Transform.h"
#include "Editor/EditorViewportController.h"
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
#include <limits>
#include "Platform/Win32/Window.h"

using namespace Sailor;

namespace Sailor
{
	struct EditorManagedMutationState
	{
		TMap<InstanceId, uint64_t> m_objectRevisions{};
	};
}

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

	bool TrySetWorldPosition(
		GameObjectPtr gameObject,
		const glm::vec3& worldPosition)
	{
		if (!gameObject || !Math::AllFinite(worldPosition))
		{
			return false;
		}

		glm::vec3 localPosition = worldPosition;
		if (const auto& parent = gameObject->GetParent())
		{
			glm::mat4 inverseParentMatrix{};
			if (!TryInvertTransformMatrix(
					CalculateCurrentWorldMatrix(parent),
					inverseParentMatrix))
			{
				return false;
			}

			glm::vec4 localHomogeneous =
				inverseParentMatrix * glm::vec4(worldPosition, 1.0f);
			if (!Math::AllFinite(localHomogeneous) ||
				std::abs(localHomogeneous.w) <=
					std::numeric_limits<float>::epsilon())
			{
				return false;
			}

			localHomogeneous /= localHomogeneous.w;
			localPosition = glm::vec3(localHomogeneous);
		}

		gameObject->GetTransformComponent().SetPosition(localPosition);
		return true;
	}
}

Editor::Editor(HWND editorHwnd, uint32_t editorPort, Sailor::Win32::Window* pMainWindow) :
	m_editorPort(editorPort),
	m_editorHwnd(editorHwnd),
	m_pMainWindow(pMainWindow),
	m_viewportController(TUniquePtr<EditorViewport::EditorViewportController>::Make()),
	m_managedMutationState(TUniquePtr<EditorManagedMutationState>::Make())
{

}

Editor::~Editor() = default;

void Editor::SetWorld(World* world)
{
	m_world = world;
	m_managedSelectionMutationRevision = 0;
	m_managedMutationState->m_objectRevisions.Clear();
	m_viewportController->Reset();
	m_viewportController->SetManagedMutationRevisions(0, 0);
}

void Editor::TickViewportTools()
{
	if (m_world)
	{
		uint64_t selectedObjectRevision = 0;
		for (const auto& gameObject : m_world->GetGameObjects())
		{
			if (gameObject &&
				m_world->IsEditorSelected(gameObject->GetInstanceId()) &&
				!gameObject->GetComponent<EditorComponent>())
			{
				selectedObjectRevision = GetManagedObjectMutationRevision(gameObject->GetInstanceId());
				break;
			}
		}

		m_viewportController->SetManagedMutationRevisions(
			m_managedSelectionMutationRevision,
			selectedObjectRevision);
#if defined(_WIN32)
		if (m_pMainWindow)
		{
			std::string fileId{};
			float normalizedX = 0.0f;
			float normalizedY = 0.0f;
			while (m_pMainWindow->PullEditorViewportAssetDrop(
				fileId,
				normalizedX,
				normalizedY))
			{
				m_viewportController->QueueAssetDropEvent(
					fileId,
					normalizedX,
					normalizedY);
			}

			uint32_t shortcutKeyCode = 0;
			while (m_pMainWindow->PullEditorViewportToolShortcut(
				shortcutKeyCode))
			{
				m_viewportController->QueueToolShortcutEvent(
					shortcutKeyCode);
			}
		}
#endif
		m_viewportController->Tick(*m_world);
	}
}

void Editor::NotifyManagedObjectMutation(const InstanceId& instanceId)
{
	const InstanceId gameObjectId = instanceId.GameObjectId();
	if (gameObjectId)
	{
		++m_managedMutationState->m_objectRevisions[gameObjectId];
	}
}

uint64_t Editor::GetManagedObjectMutationRevision(const InstanceId& instanceId) const
{
	const InstanceId gameObjectId = instanceId.GameObjectId();
	const uint64_t* revision = nullptr;
	return gameObjectId && m_managedMutationState->m_objectRevisions.Find(gameObjectId, revision)
		? *revision
		: 0;
}

void Editor::CancelViewportInteraction()
{
	if (m_world)
	{
		m_viewportController->CancelInteraction(*m_world);
	}
	else
	{
		m_viewportController->CancelPointerInteraction();
	}
}

bool Editor::PullViewportEvent(std::string& outEvent)
{
	return m_viewportController->PullEvent(outEvent);
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

				m_world->ApplyComponentReflection(el, overrideData, true);
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
			NotifyManagedObjectMutation(instanceId);
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
	if (gameObject->GetParent() != parentGameObject)
	{
		return false;
	}

	if (bKeepWorldTransform)
	{
		auto& transform = gameObject->GetTransformComponent();
		transform.SetPosition(localTransform.m_position);
		transform.SetRotation(localTransform.m_rotation);
		transform.SetScale(localTransform.m_scale);
	}

	NotifyManagedObjectMutation(instanceId);
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
		if (gameObject->GetParent() != parentGameObject)
		{
			m_world->DestroyImmediate(gameObject);
			return false;
		}
	}

	outInstanceId = gameObject->GetInstanceId();
	NotifyManagedObjectMutation(outInstanceId);
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

	if (m_world->IsPrefabLinked(instanceId) &&
		!m_world->IsPrefabInstanceRoot(instanceId))
	{
		return false;
	}

	NotifyManagedObjectMutation(instanceId);
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
		m_world->ApplyComponentReflection(component, defaults, true);
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
	m_world->ApplyComponentReflection(component, defaults, true);
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
	InstanceId instanceId{};
	return InstantiatePrefab(
		prefabId,
		parentInstanceId,
		nullptr,
		instanceId);
}

bool Editor::InstantiatePrefab(
	const PrefabPtr& prefab,
	const InstanceId& parentInstanceId)
{
	return InstantiatePrefab(prefab, parentInstanceId, false);
}

bool Editor::InstantiatePrefab(
	const PrefabPtr& prefab,
	const InstanceId& parentInstanceId,
	bool bStrictInstanceIds)
{
	InstanceId instanceId{};
	return InstantiatePrefab(
		prefab,
		parentInstanceId,
		nullptr,
		instanceId,
		bStrictInstanceIds);
}

bool Editor::InstantiatePrefab(
	const FileId& prefabId,
	const InstanceId& parentInstanceId,
	const glm::vec3* worldPosition,
	InstanceId& outInstanceId)
{
	SAILOR_PROFILE_FUNCTION();
	outInstanceId = InstanceId::Invalid;

	if (!m_world || !prefabId)
	{
		return false;
	}

	auto prefabImporter = App::GetSubmodule<PrefabImporter>();
	if (!prefabImporter)
	{
		return false;
	}

	PrefabPtr prefab;
	if (!prefabImporter->LoadPrefab_Immediate(prefabId, prefab) ||
		!prefab ||
		!prefab->IsReady())
	{
		return false;
	}

	return InstantiatePrefab(
		prefab,
		parentInstanceId,
		worldPosition,
		outInstanceId);
}

bool Editor::InstantiatePrefab(
	const PrefabPtr& prefab,
	const InstanceId& parentInstanceId,
	const glm::vec3* worldPosition,
	InstanceId& outInstanceId,
	bool bStrictInstanceIds)
{
	SAILOR_PROFILE_FUNCTION();
	outInstanceId = InstanceId::Invalid;

	if (!m_world || !prefab)
	{
		return false;
	}

	GameObjectPtr parentGameObject;
	if (!ResolveParent(m_world, parentInstanceId, parentGameObject))
	{
		return false;
	}

	const bool bDetachedFromPrefabRestore =
		prefab->IsDetachedFromPrefabRecord();
	if (bDetachedFromPrefabRestore)
	{
		if (!bStrictInstanceIds ||
			!parentGameObject ||
			prefab->GetFileId() ||
			prefab->GetDetachedParentInstanceId() !=
				parentInstanceId ||
			!m_world->IsPrefabLinked(parentInstanceId))
		{
			SAILOR_LOG_ERROR(
				"Cannot restore detached prefab snapshot: strict ids, an invalid source FileId, and the exact linked parent are required.");
			return false;
		}
	}
	else if (parentGameObject)
	{
		std::string reparentDiagnostic;
		if (!m_world->CanReparentPrefabObject(
				InstanceId::Invalid,
				parentInstanceId,
				&reparentDiagnostic))
		{
			SAILOR_LOG_ERROR(
				"Cannot instantiate prefab under parent '%s': %s.",
				parentInstanceId.ToString().c_str(),
				reparentDiagnostic.c_str());
			return false;
		}
	}

	auto root = m_world->Instantiate(prefab, bStrictInstanceIds);
	if (!root)
	{
		return false;
	}

	if (parentGameObject &&
		!bDetachedFromPrefabRestore)
	{
		root->SetParent(parentGameObject);
		if (root->GetParent() != parentGameObject)
		{
			m_world->DestroyImmediate(root);
			return false;
		}
	}
	else if (bDetachedFromPrefabRestore &&
		root->GetParent() != parentGameObject)
	{
		m_world->DestroyImmediate(root);
		return false;
	}

	if (worldPosition && !TrySetWorldPosition(root, *worldPosition))
	{
		m_world->DestroyImmediate(root);
		return false;
	}

	outInstanceId = root->GetInstanceId();
	NotifyManagedObjectMutation(root->GetInstanceId());
	return true;
}

bool Editor::CreateModelGameObject(
	const FileId& modelId,
	const std::string& name,
	const InstanceId& parentInstanceId,
	const glm::vec3* worldPosition,
	InstanceId& outInstanceId)
{
	SAILOR_PROFILE_FUNCTION();
	outInstanceId = InstanceId::Invalid;

	if (!m_world || !modelId || name.empty())
	{
		return false;
	}

	GameObjectPtr parentGameObject{};
	if (!ResolveParent(m_world, parentInstanceId, parentGameObject))
	{
		return false;
	}

	auto modelImporter = App::GetSubmodule<ModelImporter>();
	ModelPtr model{};
	if (!modelImporter ||
		!modelImporter->LoadModel_Immediate(modelId, model) ||
		!model ||
		!model->IsReady())
	{
		return false;
	}

	auto gameObject = m_world->Instantiate(name);
	if (!gameObject)
	{
		return false;
	}

	if (parentGameObject)
	{
		gameObject->SetParent(parentGameObject);
		if (gameObject->GetParent() != parentGameObject)
		{
			m_world->DestroyImmediate(gameObject);
			return false;
		}
	}

	if (worldPosition && !TrySetWorldPosition(gameObject, *worldPosition))
	{
		m_world->DestroyImmediate(gameObject);
		return false;
	}

	auto meshRenderer = gameObject->AddComponent<MeshRendererComponent>();
	if (!meshRenderer)
	{
		m_world->DestroyImmediate(gameObject);
		return false;
	}

	meshRenderer->SetModel(model);
	outInstanceId = gameObject->GetInstanceId();
	NotifyManagedObjectMutation(outInstanceId);
	return true;
}

bool Editor::ResolveViewportDropPosition(
	float normalizedX,
	float normalizedY,
	glm::vec3& outPosition) const
{
	return m_world &&
		m_viewportController &&
		m_viewportController->TryResolveDropPosition(
			*m_world,
			normalizedX,
			normalizedY,
			outPosition);
}

bool Editor::FocusEditorCamera(const InstanceId& instanceId)
{
	return m_world &&
		m_viewportController &&
		m_viewportController->FocusCameraOnObject(*m_world, instanceId);
}

bool Editor::SetPrefabLink(
	const InstanceId& instanceId,
	const FileId& prefabId)
{
	if (!m_world ||
		!instanceId.IsGameObjectId() ||
		!prefabId)
	{
		return false;
	}

	auto root = m_world
		->GetObjectByInstanceId(instanceId)
		.DynamicCast<GameObject>();
	auto prefabImporter = App::GetSubmodule<PrefabImporter>();
	PrefabPtr prefab{};
	if (!root ||
		!prefabImporter ||
		!prefabImporter->LoadPrefab_Immediate(prefabId, prefab) ||
		!prefab ||
		!prefab->IsReady())
	{
		return false;
	}

	std::string diagnostic{};
	if (!m_world->LinkPrefabInstance(root, prefab, diagnostic))
	{
		SAILOR_LOG_ERROR(
			"Cannot link game object '%s' to prefab '%s': %s.",
			instanceId.ToString().c_str(),
			prefabId.ToString().c_str(),
			diagnostic.c_str());
		return false;
	}

	NotifyManagedObjectMutation(instanceId);
	return true;
}

bool Editor::BreakPrefabLink(const InstanceId& instanceId)
{
	if (!m_world ||
		!instanceId.IsGameObjectId() ||
		!m_world->BreakPrefabLink(instanceId))
	{
		return false;
	}

	NotifyManagedObjectMutation(instanceId);
	return true;
}

bool Editor::SetViewportToolState(
	EditorViewport::ETransformOperation operation,
	EditorViewport::ETransformSpace space)
{
	return m_viewportController &&
		m_viewportController->SetTransformToolState(operation, space);
}

void Editor::GetViewportToolState(
	EditorViewport::ETransformOperation& outOperation,
	EditorViewport::ETransformSpace& outSpace) const
{
	outOperation = m_viewportController
		? m_viewportController->GetOperation()
		: EditorViewport::ETransformOperation::Translate;
	outSpace = m_viewportController
		? m_viewportController->GetSpace()
		: EditorViewport::ETransformSpace::World;
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

	size_t numMessages = m_numMessages.load(std::memory_order_relaxed);
	do
	{
		if (numMessages >= 1024)
		{
			return;
		}
	}
	while (!m_numMessages.compare_exchange_weak(
		numMessages,
		numMessages + 1,
		std::memory_order_relaxed));

	m_messagesQueue.push(oss.str());
}

bool Editor::PullMessage(std::string& msg)
{
	if (m_messagesQueue.try_pop(msg))
	{
		m_numMessages.fetch_sub(1, std::memory_order_relaxed);
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
	if (!prefab || !prefab->IsReady())
	{
		SAILOR_LOG_ERROR(
			"Cannot serialize the current world: %s.",
			prefab
				? prefab->GetLoadDiagnostic().c_str()
				: "world serialization did not create a document");
		return YAML::Node();
	}

	return prefab->Serialize();
}
