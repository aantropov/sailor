#include "EditorScene.h"
#include "Sailor.h"
#include "Submodules/Editor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "Components/AnimatorComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Core/LogMacros.h"
#include "Core/Reflection.h"
#include "ECS/TransformECS.h"
#include "Engine/EngineLoop.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Math/Math.h"
#include "Math/Transform.h"

#include <cmath>
#include <cstring>
#include <limits>

using namespace Sailor;

namespace
{
	constexpr uint32_t c_selectionMutationRevisionKind = 1;
	constexpr uint32_t c_objectMutationRevisionKind = 2;

	AnimatorComponent* FindEditorAnimator(
		Editor* editor,
		const InstanceId& componentInstanceId)
	{
		if (!editor || !editor->GetWorld() ||
			componentInstanceId.ComponentId() == InstanceId::Invalid)
		{
			return nullptr;
		}

		auto gameObject = editor->GetWorld()
			->GetObjectByInstanceId(componentInstanceId.GameObjectId())
			.DynamicCast<GameObject>();
		if (!gameObject)
		{
			return nullptr;
		}

		for (auto component : gameObject->GetComponents())
		{
			if (component &&
				component->GetInstanceId() == componentInstanceId)
			{
				return component.DynamicCast<AnimatorComponent>().GetRawPtr();
			}
		}
		return nullptr;
	}

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

	bool TryInvertTransformMatrix(const glm::mat4& matrix, glm::mat4& outInverse)
	{
		if (!Math::AllFinite(matrix))
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
		return Math::AllFinite(outInverse) &&
			Math::AreNearlyEqual(
				matrix * outInverse,
				glm::identity<glm::mat4>(),
				0.001f);
	}

	bool TryMakeExactTransform(const glm::mat4& matrix, Math::Transform& outTransform)
	{
		if (!Math::AllFinite(matrix))
		{
			return false;
		}

		outTransform = Math::Transform::FromMatrix(matrix);
		if (!Math::AllFinite(outTransform.m_position) ||
			!Math::AllFinite(outTransform.m_scale))
		{
			return false;
		}

		return Math::AreNearlyEqual(outTransform.Matrix(), matrix);
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

	bool IsEditableModelHierarchyValid(const Model& model)
	{
		if (!model.SupportsEditableHierarchy() || model.GetNodes().IsEmpty())
		{
			return false;
		}

		const auto& nodes = model.GetNodes();
		for (uint32_t nodeIndex = 0; nodeIndex < nodes.Num(); ++nodeIndex)
		{
			const auto& node = nodes[nodeIndex];
			const auto& transform = node.m_localTransform;
			if (node.m_parentIndex < -1 ||
				node.m_parentIndex >= static_cast<int32_t>(nodeIndex) ||
				node.m_meshIndex < Model::AllMeshes ||
				(node.m_meshIndex >= 0 &&
					!model.IsSourceMeshIndexValid(node.m_meshIndex)) ||
				node.m_skinIndex >= 0 ||
				!Math::AllFinite(transform.m_position) ||
				!Math::AllFinite(transform.m_scale))
			{
				return false;
			}
		}

		return true;
	}

	bool AttachModelRenderer(
		GameObjectPtr gameObject,
		const ModelPtr& model,
		int32_t meshIndex,
		const TVector<MaterialPtr>& defaultMaterials)
	{
		auto meshRenderer = gameObject
			? gameObject->AddComponent<MeshRendererComponent>()
			: MeshRendererComponentPtr{};
		if (!meshRenderer)
		{
			return false;
		}

		meshRenderer->SetMeshIndex(meshIndex);
		auto& rendererData = meshRenderer->GetData();
		rendererData.SetModel(model);
		rendererData.GetMaterials() = defaultMaterials;
		rendererData.MarkDirty();
		return true;
	}
}

bool Editor::UpdateObject(const InstanceId& instanceId, const std::string& strYamlNode)
{
	SAILOR_PROFILE_FUNCTION();
	if (!m_world) return false;

	auto gameObject = m_world->GetObjectByInstanceId(instanceId.GameObjectId()).DynamicCast<GameObject>();
	if (!gameObject) return false;

	const YAML::Node data = YAML::Load(strYamlNode);
	if (instanceId.ComponentId())
	{
		ReflectedData reflected;
		reflected.Deserialize(data);
		if (!reflected.IsValid()) return false;

		const auto components = gameObject->GetComponents();
		for (auto component : components)
		{
			if (component->GetInstanceId().ComponentId() == instanceId.ComponentId())
			{
				if (component->GetTypeInfo().Name() != reflected.GetTypeInfo().Name()) return false;
				m_world->ApplyComponentReflection(component, reflected, true);
				return true;
			}
		}
		return false;
	}

	Prefab::ReflectedGameObject reflected;
	reflected.Deserialize(data);
	gameObject->SetName(reflected.m_name);
	gameObject->SetMobilityType(reflected.m_mobilityType);
	auto& transform = gameObject->GetTransformComponent();
	transform.SetPosition(reflected.m_position);
	transform.SetRotation(reflected.m_rotation);
	transform.SetScale(reflected.m_scale);
	NotifyManagedObjectMutation(instanceId);
	return true;
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
		transform.SetRotation(localTransform.GetRotation());
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

bool Editor::CreateModelInstance(
	const ModelPtr& model,
	const std::string& name,
	const InstanceId& parentInstanceId,
	bool bCreateHierarchy,
	const glm::vec3* worldPosition,
	const InstanceId& preferredInstanceId,
	InstanceId& outInstanceId)
{
	SAILOR_PROFILE_FUNCTION();
	outInstanceId = InstanceId::Invalid;

	if (!m_world || !model || !model->IsStructurallyReady() || name.empty())
	{
		SAILOR_LOG_ERROR("Cannot create model instance '%s': invalid world, model, or name.", name.c_str());
		return false;
	}

	GameObjectPtr parentGameObject;
	if (!ResolveParent(m_world, parentInstanceId, parentGameObject))
	{
		SAILOR_LOG_ERROR(
			"Cannot create model instance '%s': parent '%s' is invalid.",
			name.c_str(),
			parentInstanceId.ToString().c_str());
		return false;
	}

	// Bulk hierarchy creation must not rebuild the same model material table for
	// every mesh node. Bistro-scale models can otherwise create hundreds of
	// thousands of duplicate material-load task joins in one engine update.
	TVector<MaterialPtr> defaultMaterials;
	if (model->GetFileId())
	{
		if (auto* modelImporter = App::GetSubmodule<ModelImporter>())
		{
			modelImporter->LoadDefaultMaterials(
				model->GetFileId(),
				defaultMaterials);
		}
	}

	auto root = preferredInstanceId
		? m_world->Instantiate(name, preferredInstanceId)
		: m_world->Instantiate(name);
	if (!root)
	{
		SAILOR_LOG_ERROR(
			"Cannot create model instance '%s': root '%s' could not be instantiated.",
			name.c_str(),
			preferredInstanceId.ToString().c_str());
		return false;
	}

	const auto rollback = [this, &root]()
		{
			m_world->DestroyImmediate(root);
		};

	if (parentGameObject)
	{
		root->SetParent(parentGameObject);
		if (root->GetParent() != parentGameObject)
		{
			SAILOR_LOG_ERROR(
				"Cannot create model instance '%s': root could not be parented to '%s'.",
				name.c_str(),
				parentInstanceId.ToString().c_str());
			rollback();
			return false;
		}
	}

	if (worldPosition && !TrySetWorldPosition(root, *worldPosition))
	{
		SAILOR_LOG_ERROR(
			"Cannot create model instance '%s': world position could not be resolved.",
			name.c_str());
		rollback();
		return false;
	}

	const bool bCreateEditableHierarchy =
		bCreateHierarchy && IsEditableModelHierarchyValid(*model);
	if (!bCreateEditableHierarchy)
	{
		if (!AttachModelRenderer(
				root,
				model,
				Model::AllMeshes,
				defaultMaterials))
		{
			SAILOR_LOG_ERROR(
				"Cannot create model instance '%s': root MeshRendererComponent could not be created.",
				name.c_str());
			rollback();
			return false;
		}
	}
	else
	{
		const auto& modelNodes = model->GetNodes();
		TVector<GameObjectPtr> createdNodes;
		createdNodes.Reserve(modelNodes.Num());
		for (uint32_t nodeIndex = 0; nodeIndex < modelNodes.Num(); ++nodeIndex)
		{
			const auto& modelNode = modelNodes[nodeIndex];
			const std::string nodeName = modelNode.m_name.empty()
				? "Node_" + std::to_string(modelNode.m_sourceNodeIndex)
				: modelNode.m_name;
			auto node = m_world->Instantiate(nodeName);
			if (!node)
			{
				SAILOR_LOG_ERROR(
					"Cannot create model instance '%s': node %u ('%s') could not be instantiated.",
					name.c_str(),
					nodeIndex,
					nodeName.c_str());
				rollback();
				return false;
			}

			const GameObjectPtr& nodeParent = modelNode.m_parentIndex >= 0
				? createdNodes[static_cast<uint32_t>(modelNode.m_parentIndex)]
				: root;
			node->SetParent(nodeParent);
			if (node->GetParent() != nodeParent)
			{
				SAILOR_LOG_ERROR(
					"Cannot create model instance '%s': node %u ('%s') could not be parented to source parent %d.",
					name.c_str(),
					nodeIndex,
					nodeName.c_str(),
					modelNode.m_parentIndex);
				m_world->DestroyImmediate(node);
				rollback();
				return false;
			}

			auto& transform = node->GetTransformComponent();
			transform.SetPosition(glm::vec3(modelNode.m_localTransform.m_position));
			transform.SetRotation(modelNode.m_localTransform.GetRotation());
			transform.SetScale(modelNode.m_localTransform.m_scale);
			if (modelNode.m_meshIndex >= 0 &&
				!AttachModelRenderer(
					node,
					model,
					modelNode.m_meshIndex,
					defaultMaterials))
			{
				SAILOR_LOG_ERROR(
					"Cannot create model instance '%s': node %u ('%s') MeshRendererComponent for source mesh %d could not be created.",
					name.c_str(),
					nodeIndex,
					nodeName.c_str(),
					modelNode.m_meshIndex);
				rollback();
				return false;
			}

			createdNodes.Add(std::move(node));
		}
	}

	outInstanceId = root->GetInstanceId();
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
	const InstanceId& parentInstanceId,
	EPrefabInstanceIdPolicy idPolicy)
{
	InstanceId instanceId{};
	return InstantiatePrefab(
		prefab,
		parentInstanceId,
		nullptr,
		instanceId,
		idPolicy);
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
	EPrefabInstanceIdPolicy idPolicy)
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
		if (idPolicy != EPrefabInstanceIdPolicy::RequireExact ||
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

	auto root = m_world->Instantiate(prefab, idPolicy);
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

uint32_t EditorRuntime::SerializeCurrentWorld(char** yamlNode)
{
	if (!yamlNode)
	{
		return 0;
	}

	yamlNode[0] = nullptr;
	return App::ExecuteOnEngineMainThread<uint32_t>(0, [yamlNode]()
		{
			auto editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return 0u;
			}

			auto node = editor->SerializeWorld();
			if (node.IsNull())
			{
				return 0u;
			}

			const std::string serializedNode = YAML::Dump(node);
			const size_t length = serializedNode.length();
			yamlNode[0] = new char[length + 1];
			memcpy(yamlNode[0], serializedNode.c_str(), length);
			yamlNode[0][length] = '\0';
			return static_cast<uint32_t>(length);
		});
}

bool EditorRuntime::LoadEditorWorld(const char* strFileId)
{
	if (!strFileId || strFileId[0] == '\0')
	{
		return false;
	}

	const std::string fileIdValue = strFileId;
	return App::ExecuteOnEngineMainThread<bool>(false, [fileIdValue]()
		{
			auto editor = App::GetSubmodule<Editor>();
			auto engineLoop = App::GetSubmodule<EngineLoop>();
			auto assetRegistry = App::GetSubmodule<AssetRegistry>();
			if (!editor || !engineLoop || !assetRegistry)
			{
				return false;
			}

			const FileId fileId(fileIdValue);
			auto worldPrefab = assetRegistry->LoadAssetFromFile<WorldPrefab>(fileId);
			if (!worldPrefab || !worldPrefab->IsReady())
			{
				return false;
			}
			if (editor->IsSimulationEnabled() &&
				!editor->SetSimulationEnabled(false))
			{
				return false;
			}

			auto oldWorld = editor->GetWorld();
			auto newWorld = engineLoop->InstantiateWorld(worldPrefab, EngineLoop::EditorWorldMask);
			if (!newWorld)
			{
				return false;
			}

			editor->SetWorld(newWorld.GetRawPtr());
			if (oldWorld)
			{
				engineLoop->ExitWorld(oldWorld);
				engineLoop->ProcessPendingWorldExits();
			}

			return true;
		});
}

bool EditorRuntime::CreateEditorWorld()
{
	return App::ExecuteOnEngineMainThread<bool>(false, []()
		{
			auto editor = App::GetSubmodule<Editor>();
			auto engineLoop = App::GetSubmodule<EngineLoop>();
			if (!editor || !engineLoop)
			{
				return false;
			}
			if (editor->IsSimulationEnabled() &&
				!editor->SetSimulationEnabled(false))
			{
				return false;
			}

			auto oldWorld = editor->GetWorld();
			auto newWorld = engineLoop->CreateEmptyWorld("New Scene", EngineLoop::EditorWorldMask);
			if (!newWorld)
			{
				return false;
			}

			editor->SetWorld(newWorld.GetRawPtr());
			if (oldWorld)
			{
				engineLoop->ExitWorld(oldWorld);
				engineLoop->ProcessPendingWorldExits();
			}

			return true;
		});
}

bool EditorRuntime::SetEditorSimulationEnabled(bool bEnabled)
{
	return App::ExecuteOnEngineMainThread<bool>(false, [bEnabled]()
		{
			auto* editor = App::GetSubmodule<Editor>();
			return editor && editor->SetSimulationEnabled(bEnabled);
		});
}

bool EditorRuntime::IsEditorSimulationEnabled()
{
	return App::ExecuteOnEngineMainThread<bool>(false, []()
		{
			const auto* editor = App::GetSubmodule<Editor>();
			return editor && editor->IsSimulationEnabled();
		});
}

uint64_t EditorRuntime::GetEditorManagedMutationRevision(uint32_t kind, const char* strInstanceId)
{
	const std::string instanceId = strInstanceId ? strInstanceId : std::string{};

	return App::ExecuteOnEngineMainThread<uint64_t>(0, [kind, instanceId]()
		{
			auto editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return uint64_t{ 0 };
			}

			if (kind == c_selectionMutationRevisionKind)
			{
				return editor->GetManagedSelectionMutationRevision();
			}

			if (kind == c_objectMutationRevisionKind && !instanceId.empty())
			{
				const InstanceId parsedInstanceId(instanceId);
				return editor->GetManagedObjectMutationRevision(parsedInstanceId);
			}

			return uint64_t{ 0 };
		});
}

bool EditorRuntime::UpdateEditorObject(const char* strInstanceId, const char* strYamlNode)
{
	if (!strInstanceId || !strYamlNode)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	const std::string yamlValue = strYamlNode;
	return App::ExecuteOnEngineMainThread<bool>(false, [instanceIdValue, yamlValue]()
		{
			auto editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			const InstanceId instanceId(instanceIdValue);
			return editor->UpdateObject(instanceId, yamlValue);
		});
}

bool EditorRuntime::DestroyEditorObject(const char* strInstanceId)
{
	if (!strInstanceId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	return App::ExecuteOnEngineMainThread<bool>(false, [instanceIdValue]()
		{
			auto editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			const InstanceId instanceId(instanceIdValue);
			return editor->DestroyObject(instanceId);
		});
}

bool EditorRuntime::ResetEditorComponentToDefaults(const char* strInstanceId)
{
	if (!strInstanceId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	return App::ExecuteOnEngineMainThread<bool>(false, [instanceIdValue]()
		{
			auto editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			const InstanceId instanceId(instanceIdValue);
			return editor->ResetComponentToDefaults(instanceId);
		});
}

bool EditorRuntime::RemoveEditorComponent(const char* strInstanceId)
{
	if (!strInstanceId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	return App::ExecuteOnEngineMainThread<bool>(false, [instanceIdValue]()
		{
			auto editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			const InstanceId instanceId(instanceIdValue);
			return editor->RemoveComponent(instanceId);
		});
}

bool EditorRuntime::SetEditorPrefabLink(
	const char* strInstanceId,
	const char* strFileId)
{
	if (!strInstanceId || !strFileId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	const std::string fileIdValue = strFileId;
	return App::ExecuteOnEngineMainThread<bool>(
		false,
		[instanceIdValue, fileIdValue]()
		{
			auto editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			const InstanceId instanceId(instanceIdValue);
			const FileId fileId(fileIdValue);
			return editor->SetPrefabLink(instanceId, fileId);
		});
}

bool EditorRuntime::BreakEditorPrefabLink(const char* strInstanceId)
{
	if (!strInstanceId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	return App::ExecuteOnEngineMainThread<bool>(
		false,
		[instanceIdValue]()
		{
			auto editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			const InstanceId instanceId(instanceIdValue);
			return editor->BreakPrefabLink(instanceId);
		});
}

bool EditorRuntime::SetEditorAnimatorParameter(
	const char* strInstanceId,
	const char* strName,
	uint32_t valueKind,
	float floatValue,
	int32_t intValue,
	bool boolValue)
{
	if (!strInstanceId || !strName || strName[0] == '\0')
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	const auto name = StringHash::Runtime(strName);
	return App::ExecuteOnEngineMainThread<bool>(false,
		[instanceIdValue, name, valueKind, floatValue, intValue, boolValue]()
		{
			auto editor = App::GetSubmodule<Editor>();
			const InstanceId instanceId(instanceIdValue);
			auto* animator = FindEditorAnimator(editor, instanceId);
			if (!animator)
			{
				return false;
			}

			switch (valueKind)
			{
			case 1:
				return animator->SetFloat(name, floatValue);
			case 2:
				return animator->SetInt(name, intValue);
			case 3:
				return animator->SetBool(name, boolValue);
			case 4:
				return animator->SetTrigger(name);
			case 5:
				return animator->ResetTrigger(name);
			default:
				return false;
			}
		});
}

bool EditorRuntime::GetEditorAnimatorState(
	const char* strInstanceId,
	bool& outHasController,
	uint64_t& outControllerRevision,
	uint64_t& outActiveStateId,
	std::string& outActiveStateName,
	float& outActiveStateTime,
	bool& outTransitioning,
	uint64_t& outDestinationStateId,
	std::string& outDestinationStateName,
	float& outDestinationStateTime,
	float& outTransitionAlpha)
{
	outHasController = false;
	outControllerRevision = 0;
	outActiveStateId = InvalidAnimationControllerNodeId;
	outActiveStateName.clear();
	outActiveStateTime = 0.0f;
	outTransitioning = false;
	outDestinationStateId = InvalidAnimationControllerNodeId;
	outDestinationStateName.clear();
	outDestinationStateTime = 0.0f;
	outTransitionAlpha = 0.0f;
	if (!strInstanceId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	return App::ExecuteOnEngineMainThread<bool>(false,
		[instanceIdValue,
			&outHasController,
			&outControllerRevision,
			&outActiveStateId,
			&outActiveStateName,
			&outActiveStateTime,
			&outTransitioning,
			&outDestinationStateId,
			&outDestinationStateName,
			&outDestinationStateTime,
			&outTransitionAlpha]()
		{
			auto editor = App::GetSubmodule<Editor>();
			const InstanceId instanceId(instanceIdValue);
			auto* animator = FindEditorAnimator(editor, instanceId);
			if (!animator)
			{
				return false;
			}

			const auto& instance = animator->GetData().GetControllerInstance();
			const auto& controller = instance.GetController();
			outHasController = controller && instance.IsValid();
			if (!outHasController)
			{
				return true;
			}

			outControllerRevision = controller->GetRevision();
			const auto& states = controller->GetStates();
			const uint32_t activeStateIndex = instance.GetActiveStateIndex();
			if (activeStateIndex < states.Num())
			{
				outActiveStateId = states[activeStateIndex].m_id;
				outActiveStateName = states[activeStateIndex].m_name;
			}
			outActiveStateTime = instance.GetActiveStateTime();
			outTransitioning = instance.IsTransitioning();
			const uint32_t destinationStateIndex = instance.GetDestinationStateIndex();
			if (outTransitioning && destinationStateIndex < states.Num())
			{
				outDestinationStateId = states[destinationStateIndex].m_id;
				outDestinationStateName = states[destinationStateIndex].m_name;
				outDestinationStateTime = instance.GetDestinationStateTime();
				outTransitionAlpha = instance.GetTransitionAlpha();
			}
			return true;
		});
}
