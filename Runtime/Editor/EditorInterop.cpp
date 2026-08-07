#include "Sailor.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "Core/Reflection.h"
#include "Engine/EngineLoop.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Engine/InstanceId.h"
#include "Components/AnimatorComponent.h"
#include "Editor/EditorViewportController.h"
#include "Submodules/Editor.h"
#include "Workspace/WorkspaceModuleManager.h"
#include "Workspace/WorkspaceCacheContract.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <utility>

using namespace Sailor;

namespace
{
	constexpr uint32_t c_selectionMutationRevisionKind = 1;
	constexpr uint32_t c_objectMutationRevisionKind = 2;

	void LogEditorTypeSerializationFailure(const char* message) noexcept
	{
		SAILOR_LOG_ERROR("Failed to serialize editor type metadata: %s", message);
	}

	bool TryParseOptionalParent(const std::string& value, InstanceId& outParent)
	{
		outParent = InstanceId::Invalid;
		if (value.empty())
		{
			return true;
		}

		outParent.Deserialize(YAML::Node(value));
		return outParent.IsGameObjectId();
	}

	bool TryParseOptionalGameObjectId(const std::string& value, InstanceId& outInstanceId)
	{
		outInstanceId = InstanceId::Invalid;
		if (value.empty())
		{
			return true;
		}

		outInstanceId.Deserialize(YAML::Node(value));
		return outInstanceId.IsGameObjectId();
	}

	bool TryParseOptionalComponentId(const std::string& value, InstanceId& outInstanceId)
	{
		outInstanceId = InstanceId::Invalid;
		if (value.empty())
		{
			return true;
		}

		outInstanceId.Deserialize(YAML::Node(value));
		return outInstanceId.ComponentId() != InstanceId::Invalid &&
			outInstanceId.GameObjectId() != InstanceId::Invalid;
	}

	void SetInteropString(const std::string& value, char** outValue)
	{
		auto result = TUniquePtr<char[]>::Make(value.size() + 1);
		memcpy(result.GetRawPtr(), value.c_str(), value.size());
		result[value.size()] = '\0';
		outValue[0] = result.Release();
	}

	bool TryParseViewportToolState(
		uint32_t operationValue,
		uint32_t spaceValue,
		EditorViewport::ETransformOperation& outOperation,
		EditorViewport::ETransformSpace& outSpace)
	{
		switch (operationValue)
		{
		case 1:
			outOperation = EditorViewport::ETransformOperation::Select;
			break;
		case 2:
			outOperation = EditorViewport::ETransformOperation::Translate;
			break;
		case 3:
			outOperation = EditorViewport::ETransformOperation::Rotate;
			break;
		case 4:
			outOperation = EditorViewport::ETransformOperation::Scale;
			break;
		default:
			return false;
		}

		switch (spaceValue)
		{
		case 1:
			outSpace = EditorViewport::ETransformSpace::World;
			break;
		case 2:
			outSpace = EditorViewport::ETransformSpace::Local;
			break;
		default:
			return false;
		}

		return true;
	}

	uint32_t ToInteropOperation(EditorViewport::ETransformOperation operation)
	{
		switch (operation)
		{
		case EditorViewport::ETransformOperation::Select: return 1;
		case EditorViewport::ETransformOperation::Translate: return 2;
		case EditorViewport::ETransformOperation::Rotate: return 3;
		case EditorViewport::ETransformOperation::Scale: return 4;
		default: return 0;
		}
	}

	uint32_t ToInteropSpace(EditorViewport::ETransformSpace space)
	{
		switch (space)
		{
		case EditorViewport::ETransformSpace::World: return 1;
		case EditorViewport::ETransformSpace::Local: return 2;
		default: return 0;
		}
	}

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
}

uint32_t App::PullEditorMessages(char** messages, uint32_t num)
{
	auto editor = GetSubmodule<Editor>();
	if (!editor || !messages)
	{
		return 0;
	}

	uint32_t numMsg = std::min(static_cast<uint32_t>(editor->NumMessages()), num);
	for (uint32_t i = 0; i < numMsg; i++)
	{
		std::string msg;
		if (editor->PullMessage(msg))
		{
			messages[i] = new char[msg.size() + 1];
			if (messages[i] == nullptr)
			{
				return i;
			}

			std::copy(msg.begin(), msg.end(), messages[i]);
			messages[i][msg.size()] = '\0';
		}
		else
		{
			return i;
		}
	}

	return numMsg;
}

uint32_t App::PullEditorViewportEvents(char** events, uint32_t num)
{
	if (!events || num == 0)
	{
		return 0;
	}

	return ExecuteOnEngineMainThread<uint32_t>(0, [events, num]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return 0u;
			}

			uint32_t numEvents = 0;
			std::string event;
			while (numEvents < num && editor->PullViewportEvent(event))
			{
				SetInteropString(event, &events[numEvents]);
				++numEvents;
			}

			return numEvents;
		});
}

bool App::TraceViewportRay(
	uint64_t viewportId,
	float normalizedX,
	float normalizedY,
	float& outWorldX,
	float& outWorldY,
	float& outWorldZ)
{
	outWorldX = 0.0f;
	outWorldY = 0.0f;
	outWorldZ = 0.0f;

	return ExecuteOnEngineMainThread<bool>(
		false,
		[viewportId,
			normalizedX,
			normalizedY,
			&outWorldX,
			&outWorldY,
			&outWorldZ]()
		{
			auto editor = GetSubmodule<Editor>();
			glm::vec3 worldPosition{};
			if (!editor ||
				!editor->TraceViewportRay(
					viewportId,
					normalizedX,
					normalizedY,
					worldPosition))
			{
				return false;
			}

			outWorldX = worldPosition.x;
			outWorldY = worldPosition.y;
			outWorldZ = worldPosition.z;
			return true;
		});
}

uint64_t App::GetEditorManagedMutationRevision(uint32_t kind, const char* strInstanceId)
{
	const std::string instanceId = strInstanceId ? strInstanceId : std::string{};

	return ExecuteOnEngineMainThread<uint64_t>(0, [kind, instanceId]()
		{
			auto editor = GetSubmodule<Editor>();
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
				InstanceId parsedInstanceId{};
				parsedInstanceId.Deserialize(YAML::Node(instanceId));
				return editor->GetManagedObjectMutationRevision(parsedInstanceId);
			}

			return uint64_t{ 0 };
		});
}

uint32_t App::SerializeCurrentWorld(char** yamlNode)
{
	if (!yamlNode)
	{
		return 0;
	}

	yamlNode[0] = nullptr;
	return ExecuteOnEngineMainThread<uint32_t>(0, [yamlNode]()
		{
			auto editor = GetSubmodule<Editor>();
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

uint32_t App::SerializeEngineTypes(char** yamlNode)
{
	if (!yamlNode)
	{
		return 0;
	}

	auto node = Reflection::ExportEngineTypes();
	if (!node.IsNull())
	{
		std::string serializedNode = YAML::Dump(node);
		size_t length = serializedNode.length();

		std::filesystem::create_directories(AssetRegistry::GetCacheFolder());
		AssetRegistry::WriteTextFile(AssetRegistry::GetCacheFolder() + "EngineTypes.yaml", serializedNode);

		yamlNode[0] = new char[length + 1];
		memcpy(yamlNode[0], serializedNode.c_str(), length);
		yamlNode[0][length] = '\0';

		return static_cast<uint32_t>(length);
	}

	yamlNode[0] = nullptr;
	return 0;
}

uint32_t App::SerializeEditorTypes(char** yamlNode)
{
	if (!yamlNode)
	{
		return 0;
	}

	yamlNode[0] = nullptr;
	YAML::Node editorTypes = Reflection::ExportEngineTypes();
	if (App* app = GetInstance(); app && app->m_pWorkspaceModuleManager)
	{
		YAML::Node combinedTypes;
		std::string mergeError;
		if (!app->m_pWorkspaceModuleManager->BuildEditorTypeMetadata(
				editorTypes,
				combinedTypes,
				mergeError))
		{
			LogEditorTypeSerializationFailure(mergeError.empty()
				? "workspace editor metadata merge failed"
				: mergeError.c_str());
			return 0;
		}

		editorTypes = std::move(combinedTypes);
	}

	if (editorTypes.IsNull())
	{
		LogEditorTypeSerializationFailure("the editor type catalog is null");
		return 0;
	}

	const std::string serializedNode = YAML::Dump(editorTypes);
	const size_t length = serializedNode.length();
	if (length > std::numeric_limits<uint32_t>::max())
	{
		LogEditorTypeSerializationFailure("the serialized catalog exceeds the interop size limit");
		return 0;
	}

	auto serializedOutput = TUniquePtr<char[]>::Make(length + 1);
	memcpy(serializedOutput.GetRawPtr(), serializedNode.c_str(), length);
	serializedOutput[length] = '\0';
	yamlNode[0] = serializedOutput.Release();

	return static_cast<uint32_t>(length);

	yamlNode[0] = nullptr;
	return 0;
}

uint32_t App::SerializeWorkspaceCacheIdentity(char** yamlNode)
{
	if (!yamlNode || !GetInstance())
	{
		return 0;
	}

	yamlNode[0] = nullptr;
	const auto identity = Workspace::MakeWorkspaceCacheIdentity(
		"editor-types",
		"editor-types-v1",
		1,
		GetWorkspaceContext());

	YAML::Node identityNode;
	identityNode["workspaceIdentity"] = identity.m_workspaceId;
	identityNode["engineVersion"] = identity.m_engineVersion;
	identityNode["buildIdentity"] = identity.m_buildIdentity;
	identityNode["producerIdentity"] = identity.m_producerIdentity;

	const std::string serializedNode = YAML::Dump(identityNode);
	const size_t length = serializedNode.length();
	if (length > std::numeric_limits<uint32_t>::max())
	{
		LogEditorTypeSerializationFailure("the serialized workspace cache identity exceeds the interop size limit");
		return 0;
	}

	auto serializedOutput = TUniquePtr<char[]>::Make(length + 1);
	memcpy(serializedOutput.GetRawPtr(), serializedNode.c_str(), length);
	serializedOutput[length] = '\0';
	yamlNode[0] = serializedOutput.Release();

	return static_cast<uint32_t>(length);

	yamlNode[0] = nullptr;
	return 0;
}

bool App::LoadEditorWorld(const char* strFileId)
{
	if (!strFileId || strFileId[0] == '\0')
	{
		return false;
	}

	const std::string fileIdValue = strFileId;
	return ExecuteOnEngineMainThread<bool>(false, [fileIdValue]()
		{
			auto editor = GetSubmodule<Editor>();
			auto engineLoop = GetSubmodule<EngineLoop>();
			auto assetRegistry = GetSubmodule<AssetRegistry>();
			if (!editor || !engineLoop || !assetRegistry)
			{
				return false;
			}

			FileId fileId;
			fileId.Deserialize(YAML::Node(fileIdValue));
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

bool App::CreateEditorWorld()
{
	return ExecuteOnEngineMainThread<bool>(false, []()
		{
			auto editor = GetSubmodule<Editor>();
			auto engineLoop = GetSubmodule<EngineLoop>();
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

bool App::SetEditorSimulationEnabled(bool bEnabled)
{
	return ExecuteOnEngineMainThread<bool>(false, [bEnabled]()
		{
			auto* editor = GetSubmodule<Editor>();
			return editor && editor->SetSimulationEnabled(bEnabled);
		});
}

bool App::IsEditorSimulationEnabled()
{
	return ExecuteOnEngineMainThread<bool>(false, []()
		{
			const auto* editor = GetSubmodule<Editor>();
			return editor && editor->IsSimulationEnabled();
		});
}

bool App::PreviewEditorAudioAsset(const char* strFileId)
{
	if (!strFileId || strFileId[0] == '\0')
	{
		return false;
	}

	const std::string fileIdValue = strFileId;
	return ExecuteOnEngineMainThread<bool>(false, [fileIdValue]()
		{
			auto* editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			FileId fileId;
			fileId.Deserialize(YAML::Node(fileIdValue));
			return fileId && editor->PreviewAudioAsset(fileId);
		});
}

bool App::UpdateEditorObject(const char* strInstanceId, const char* strYamlNode)
{
	if (!strInstanceId || !strYamlNode)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	const std::string yamlValue = strYamlNode;
	return ExecuteOnEngineMainThread<bool>(false, [instanceIdValue, yamlValue]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			InstanceId instanceId;
			instanceId.Deserialize(YAML::Node(instanceIdValue));
			return editor->UpdateObject(instanceId, yamlValue);
		});
}

bool App::SetEditorAnimatorParameter(
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
	const std::string name = strName;
	return ExecuteOnEngineMainThread<bool>(false,
		[instanceIdValue, name, valueKind, floatValue, intValue, boolValue]()
		{
			auto editor = GetSubmodule<Editor>();
			InstanceId instanceId;
			instanceId.Deserialize(YAML::Node(instanceIdValue));
			auto* animator = FindEditorAnimator(editor, instanceId);
			if (!animator)
			{
				return false;
			}

			switch (valueKind)
			{
			case 1: return animator->SetFloat(name, floatValue);
			case 2: return animator->SetInt(name, intValue);
			case 3: return animator->SetBool(name, boolValue);
			case 4: return animator->SetTrigger(name);
			case 5: return animator->ResetTrigger(name);
			default: return false;
			}
		});
}

bool App::GetEditorAnimatorState(
	const char* strInstanceId,
	bool& outHasController,
	uint64_t& outControllerRevision,
	uint64_t& outActiveStateId,
	char** outActiveStateName,
	float& outActiveStateTime,
	bool& outTransitioning,
	uint64_t& outDestinationStateId,
	char** outDestinationStateName,
	float& outDestinationStateTime,
	float& outTransitionAlpha)
{
	outHasController = false;
	outControllerRevision = 0;
	outActiveStateId = InvalidAnimationControllerNodeId;
	outActiveStateTime = 0.0f;
	outTransitioning = false;
	outDestinationStateId = InvalidAnimationControllerNodeId;
	outDestinationStateTime = 0.0f;
	outTransitionAlpha = 0.0f;
	if (!strInstanceId || !outActiveStateName || !outDestinationStateName)
	{
		return false;
	}
	outActiveStateName[0] = nullptr;
	outDestinationStateName[0] = nullptr;

	const std::string instanceIdValue = strInstanceId;
	return ExecuteOnEngineMainThread<bool>(false,
		[instanceIdValue,
			&outHasController,
			&outControllerRevision,
			&outActiveStateId,
			outActiveStateName,
			&outActiveStateTime,
			&outTransitioning,
			&outDestinationStateId,
			outDestinationStateName,
			&outDestinationStateTime,
			&outTransitionAlpha]()
		{
			auto editor = GetSubmodule<Editor>();
			InstanceId instanceId;
			instanceId.Deserialize(YAML::Node(instanceIdValue));
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
				SetInteropString({}, outActiveStateName);
				SetInteropString({}, outDestinationStateName);
				return true;
			}

			outControllerRevision = controller->GetRevision();
			const auto& states = controller->GetStates();
			const uint32_t activeStateIndex = instance.GetActiveStateIndex();
			if (activeStateIndex < states.Num())
			{
				outActiveStateId = states[activeStateIndex].m_id;
				SetInteropString(states[activeStateIndex].m_name, outActiveStateName);
			}
			else
			{
				SetInteropString({}, outActiveStateName);
			}
			outActiveStateTime = instance.GetActiveStateTime();
			outTransitioning = instance.IsTransitioning();
			const uint32_t destinationStateIndex = instance.GetDestinationStateIndex();
			if (outTransitioning && destinationStateIndex < states.Num())
			{
				outDestinationStateId = states[destinationStateIndex].m_id;
				SetInteropString(states[destinationStateIndex].m_name, outDestinationStateName);
				outDestinationStateTime = instance.GetDestinationStateTime();
				outTransitionAlpha = instance.GetTransitionAlpha();
			}
			else
			{
				SetInteropString({}, outDestinationStateName);
			}
			return true;
		});
}

bool App::ReparentEditorObject(const char* strInstanceId, const char* strParentInstanceId, bool bKeepWorldTransform)
{
	if (!strInstanceId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	const std::string parentInstanceIdValue = strParentInstanceId ? strParentInstanceId : "";
	return ExecuteOnEngineMainThread<bool>(false, [instanceIdValue, parentInstanceIdValue, bKeepWorldTransform]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			InstanceId instanceId;
			instanceId.Deserialize(YAML::Node(instanceIdValue));
			InstanceId parentInstanceId;
			if (!TryParseOptionalParent(parentInstanceIdValue, parentInstanceId))
			{
				return false;
			}

			return editor->ReparentObject(instanceId, parentInstanceId, bKeepWorldTransform);
		});
}

bool App::CreateEditorGameObject(
	const char* strParentInstanceId,
	const char* strPreferredInstanceId,
	char** outInstanceId)
{
	if (!outInstanceId)
	{
		return false;
	}

	outInstanceId[0] = nullptr;
	const std::string parentInstanceIdValue = strParentInstanceId ? strParentInstanceId : "";
	const std::string preferredInstanceIdValue = strPreferredInstanceId ? strPreferredInstanceId : "";
	return ExecuteOnEngineMainThread<bool>(false, [parentInstanceIdValue, preferredInstanceIdValue, outInstanceId]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			InstanceId parentInstanceId;
			if (!TryParseOptionalParent(parentInstanceIdValue, parentInstanceId))
			{
				return false;
			}

			InstanceId preferredInstanceId;
			if (!TryParseOptionalGameObjectId(preferredInstanceIdValue, preferredInstanceId))
			{
				return false;
			}

			InstanceId createdInstanceId;
			if (!editor->CreateGameObject(parentInstanceId, preferredInstanceId, createdInstanceId))
			{
				return false;
			}

			SetInteropString(createdInstanceId.ToString(), outInstanceId);
			return true;
		});
}

bool App::CreateEditorModelInstance(
	const char* strModelFileId,
	const char* strName,
	const char* strParentInstanceId,
	bool bCreateHierarchy,
	bool bHasWorldPosition,
	float worldX,
	float worldY,
	float worldZ,
	const char* strPreferredInstanceId,
	char** outInstanceId)
{
	if (!strModelFileId || !strName || !outInstanceId)
	{
		return false;
	}

	outInstanceId[0] = nullptr;
	FileId modelFileId;
	modelFileId.Deserialize(YAML::Node(strModelFileId));
	auto modelImporter = GetSubmodule<ModelImporter>();
	ModelPtr model;
	if (!modelFileId ||
		!modelImporter ||
		!modelImporter->LoadModel_Immediate(modelFileId, model) ||
		!model ||
		!model->IsStructurallyReady())
	{
		SAILOR_LOG_ERROR(
			"Cannot create editor model instance '%s': model '%s' could not be loaded.",
			strName,
			strModelFileId);
		return false;
	}

	const std::string name = strName;
	const std::string parentInstanceIdValue = strParentInstanceId ? strParentInstanceId : "";
	const std::string preferredInstanceIdValue = strPreferredInstanceId ? strPreferredInstanceId : "";
	return ExecuteOnEngineMainThread<bool>(false, [
		model,
		name,
		parentInstanceIdValue,
		preferredInstanceIdValue,
		bCreateHierarchy,
		bHasWorldPosition,
		worldX,
		worldY,
		worldZ,
		outInstanceId]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			InstanceId parentInstanceId;
			if (!TryParseOptionalParent(parentInstanceIdValue, parentInstanceId))
			{
				return false;
			}

			InstanceId preferredInstanceId;
			if (!TryParseOptionalGameObjectId(preferredInstanceIdValue, preferredInstanceId))
			{
				return false;
			}

			const glm::vec3 worldPosition(worldX, worldY, worldZ);
			InstanceId createdInstanceId;
			if (!editor->CreateModelInstance(
					model,
					name,
					parentInstanceId,
					bCreateHierarchy,
					bHasWorldPosition ? &worldPosition : nullptr,
					preferredInstanceId,
					createdInstanceId))
			{
				return false;
			}

			SetInteropString(createdInstanceId.ToString(), outInstanceId);
			return true;
		});
}

bool App::DestroyEditorObject(const char* strInstanceId)
{
	if (!strInstanceId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	return ExecuteOnEngineMainThread<bool>(false, [instanceIdValue]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			InstanceId instanceId;
			instanceId.Deserialize(YAML::Node(instanceIdValue));
			return editor->DestroyObject(instanceId);
		});
}

bool App::ResetEditorComponentToDefaults(const char* strInstanceId)
{
	if (!strInstanceId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	return ExecuteOnEngineMainThread<bool>(false, [instanceIdValue]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			InstanceId instanceId;
			instanceId.Deserialize(YAML::Node(instanceIdValue));
			return editor->ResetComponentToDefaults(instanceId);
		});
}

bool App::AddEditorComponent(
	const char* strInstanceId,
	const char* strComponentTypeName,
	const char* strPreferredInstanceId,
	char** outInstanceId)
{
	if (!strInstanceId || !strComponentTypeName || !outInstanceId)
	{
		return false;
	}

	outInstanceId[0] = nullptr;
	const std::string instanceIdValue = strInstanceId;
	const std::string componentTypeName = strComponentTypeName;
	const std::string preferredInstanceIdValue = strPreferredInstanceId ? strPreferredInstanceId : "";
	return ExecuteOnEngineMainThread<bool>(false, [instanceIdValue, componentTypeName, preferredInstanceIdValue, outInstanceId]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			InstanceId instanceId;
			instanceId.Deserialize(YAML::Node(instanceIdValue));

			InstanceId preferredInstanceId;
			if (!TryParseOptionalComponentId(preferredInstanceIdValue, preferredInstanceId))
			{
				return false;
			}

			InstanceId createdInstanceId;
			if (!editor->AddComponent(instanceId, componentTypeName, preferredInstanceId, createdInstanceId))
			{
				return false;
			}

			SetInteropString(createdInstanceId.ToString(), outInstanceId);
			return true;
		});
}

bool App::RemoveEditorComponent(const char* strInstanceId)
{
	if (!strInstanceId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	return ExecuteOnEngineMainThread<bool>(false, [instanceIdValue]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			InstanceId instanceId;
			instanceId.Deserialize(YAML::Node(instanceIdValue));
			return editor->RemoveComponent(instanceId);
		});
}

bool App::InstantiateEditorPrefab(const char* strFileId, const char* strParentInstanceId)
{
	if (!strFileId)
	{
		return false;
	}

	const std::string fileIdValue = strFileId;
	const std::string parentInstanceIdValue = strParentInstanceId ? strParentInstanceId : "";
	return ExecuteOnEngineMainThread<bool>(false, [fileIdValue, parentInstanceIdValue]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			FileId fileId;
			fileId.Deserialize(YAML::Node(fileIdValue));
			InstanceId parentInstanceId;
			if (!TryParseOptionalParent(parentInstanceIdValue, parentInstanceId))
			{
				return false;
			}

			return editor->InstantiatePrefab(fileId, parentInstanceId);
		});
}

bool App::InstantiateEditorPrefabInstance(
	const char* strFileId,
	const char* strParentInstanceId,
	bool bHasWorldPosition,
	float worldX,
	float worldY,
	float worldZ,
	char** outInstanceId)
{
	if (!strFileId || !outInstanceId)
	{
		return false;
	}

	outInstanceId[0] = nullptr;
	const std::string fileIdValue = strFileId;
	const std::string parentInstanceIdValue =
		strParentInstanceId ? strParentInstanceId : "";
	return ExecuteOnEngineMainThread<bool>(
		false,
		[fileIdValue,
			parentInstanceIdValue,
			bHasWorldPosition,
			worldX,
			worldY,
			worldZ,
			outInstanceId]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			FileId fileId{};
			fileId.Deserialize(YAML::Node(fileIdValue));
			if (!fileId)
			{
				return false;
			}

			InstanceId parentInstanceId{};
			if (!TryParseOptionalParent(
					parentInstanceIdValue,
					parentInstanceId))
			{
				return false;
			}

			const glm::vec3 worldPosition(worldX, worldY, worldZ);
			InstanceId createdInstanceId{};
			if (!editor->InstantiatePrefab(
					fileId,
					parentInstanceId,
					bHasWorldPosition ? &worldPosition : nullptr,
					createdInstanceId))
			{
				return false;
			}

			SetInteropString(createdInstanceId.ToString(), outInstanceId);
			return true;
		});
}

bool App::InstantiateEditorPrefabFromYaml(
	const char* strPrefabYaml,
	const char* strParentInstanceId)
{
	return InstantiateEditorPrefabFromYaml(
		strPrefabYaml,
		strParentInstanceId,
		false);
}

bool App::InstantiateEditorPrefabFromYaml(
	const char* strPrefabYaml,
	const char* strParentInstanceId,
	bool bStrictInstanceIds)
{
	if (!strPrefabYaml || strPrefabYaml[0] == '\0')
	{
		return false;
	}

	const std::string prefabYaml = strPrefabYaml;
	const std::string parentInstanceIdValue = strParentInstanceId ? strParentInstanceId : "";
	return ExecuteOnEngineMainThread<bool>(
		false,
		[prefabYaml, parentInstanceIdValue, bStrictInstanceIds]()
		{
			auto editor = GetSubmodule<Editor>();
			auto prefabImporter = GetSubmodule<PrefabImporter>();
			if (!editor || !prefabImporter)
			{
				return false;
			}

			InstanceId parentInstanceId;
			if (!TryParseOptionalParent(parentInstanceIdValue, parentInstanceId))
			{
				return false;
			}

			const YAML::Node prefabNode = YAML::Load(prefabYaml);
			if (!prefabNode.IsMap() ||
				!prefabNode["gameObjects"].IsSequence() ||
				!prefabNode["components"].IsSequence())
			{
				return false;
			}

			PrefabPtr prefab = prefabImporter->Create();
			if (!prefab)
			{
				return false;
			}

			prefab->Deserialize(prefabNode);
			if (prefab->IsLinkedPrefabSnapshotRecord())
			{
				if (!bStrictInstanceIds ||
					prefab->GetLinkedParentInstanceId() !=
						parentInstanceId)
				{
					return false;
				}

				std::string diagnostic;
				if (!prefab->ValidateForInstantiation(
						diagnostic))
				{
					return false;
				}

				PrefabPtr sourcePrefab;
				const FileId& sourcePrefabId =
					prefab->GetLinkedSnapshotSourceFileId();
				if (!prefabImporter->LoadPrefab_Immediate(
						sourcePrefabId,
						sourcePrefab) ||
					!sourcePrefab ||
					!sourcePrefab->IsReady())
				{
					return false;
				}

				PrefabPtr linkedPrefab =
					prefabImporter->Create(
						sourcePrefabId);
				if (!linkedPrefab ||
					!linkedPrefab->ConfigureLinkedInstance(
						sourcePrefab,
						prefab->GetLinkedInstanceIds(),
						parentInstanceId,
						prefab->GetLinkedGameObjectOverrides(),
						prefab->GetLinkedComponentOverrides(),
						diagnostic) ||
					!linkedPrefab->AppendDetachedSupplementalHierarchy(
						prefab,
						diagnostic))
				{
					return false;
				}

				prefab = std::move(linkedPrefab);
			}

			return editor->InstantiatePrefab(
				prefab,
				parentInstanceId,
				bStrictInstanceIds);
		});
}

bool App::FocusEditorCamera(const char* strInstanceId)
{
	if (!strInstanceId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	return ExecuteOnEngineMainThread<bool>(
		false,
		[instanceIdValue]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			InstanceId instanceId{};
			instanceId.Deserialize(YAML::Node(instanceIdValue));
			return instanceId.IsGameObjectId() &&
				editor->FocusEditorCamera(instanceId);
		});
}

bool App::SetEditorPrefabLink(
	const char* strInstanceId,
	const char* strFileId)
{
	if (!strInstanceId || !strFileId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	const std::string fileIdValue = strFileId;
	return ExecuteOnEngineMainThread<bool>(
		false,
		[instanceIdValue, fileIdValue]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			InstanceId instanceId{};
			instanceId.Deserialize(YAML::Node(instanceIdValue));
			FileId fileId{};
			fileId.Deserialize(YAML::Node(fileIdValue));
			return editor->SetPrefabLink(instanceId, fileId);
		});
}

bool App::BreakEditorPrefabLink(const char* strInstanceId)
{
	if (!strInstanceId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	return ExecuteOnEngineMainThread<bool>(
		false,
		[instanceIdValue]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			InstanceId instanceId{};
			instanceId.Deserialize(YAML::Node(instanceIdValue));
			return editor->BreakPrefabLink(instanceId);
		});
}

bool App::SetEditorViewportToolState(uint32_t operation, uint32_t space)
{
	return ExecuteOnEngineMainThread<bool>(
		false,
		[operation, space]()
		{
			auto editor = GetSubmodule<Editor>();
			EditorViewport::ETransformOperation parsedOperation{};
			EditorViewport::ETransformSpace parsedSpace{};
			return editor &&
				TryParseViewportToolState(
					operation,
					space,
					parsedOperation,
					parsedSpace) &&
				editor->SetViewportToolState(parsedOperation, parsedSpace);
		});
}

bool App::GetEditorViewportToolState(
	uint32_t& outOperation,
	uint32_t& outSpace)
{
	outOperation = 0;
	outSpace = 0;
	return ExecuteOnEngineMainThread<bool>(
		false,
		[&outOperation, &outSpace]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			EditorViewport::ETransformOperation operation{};
			EditorViewport::ETransformSpace space{};
			editor->GetViewportToolState(operation, space);
			outOperation = ToInteropOperation(operation);
			outSpace = ToInteropSpace(space);
			return outOperation != 0 && outSpace != 0;
		});
}

bool App::SetEditorSelection(const char* strSelectionYaml)
{
	if (!strSelectionYaml)
	{
		return false;
	}

	const std::string selectionYaml = strSelectionYaml;
	return ExecuteOnEngineMainThread<bool>(false, [selectionYaml]()
		{
			auto editor = GetSubmodule<Editor>();
			auto* world = editor ? editor->GetWorld() : nullptr;
			if (!world)
			{
				return false;
			}

			TVector<InstanceId> selection;
			const YAML::Node yaml = YAML::Load(selectionYaml);
			if (yaml && yaml.IsSequence())
			{
				selection.Reserve(yaml.size());
				for (const auto& entry : yaml)
				{
					InstanceId instanceId{};
					instanceId.Deserialize(entry);
					if (instanceId)
					{
						selection.Add(instanceId);
					}
				}
			}

			world->SetEditorSelection(selection);
			editor->NotifyManagedSelectionMutation();
			return true;
		});
}

bool App::RenderPathTracedImage(const char* strOutputPath, const char* strInstanceId, uint32_t height, uint32_t samplesPerPixel, uint32_t maxBounces)
{
	if (!strOutputPath || strOutputPath[0] == '\0')
	{
		return false;
	}

	const std::string outputPath = strOutputPath;
	const std::string instanceIdValue = strInstanceId ? strInstanceId : "";
	return ExecuteOnEngineMainThread<bool>(false, [outputPath, instanceIdValue, height, samplesPerPixel, maxBounces]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			InstanceId instanceId{};
			if (!instanceIdValue.empty())
			{
				instanceId.Deserialize(YAML::Node(instanceIdValue));
			}

			const bool bSuccess = editor->RenderPathTracedImage(instanceId, outputPath, height, samplesPerPixel, maxBounces);
			editor->PushMessage(bSuccess ?
				("Path tracer export succeeded: " + outputPath) :
				("Path tracer export failed: " + outputPath));
			return bSuccess;
		});
}

void App::ShowMainWindow(bool bShow)
{
	if (auto editor = GetSubmodule<Editor>())
	{
#if defined(_WIN32)
		editor->ShowMainWindow(false);
#else
		editor->ShowMainWindow(bShow);
#endif
	}
}
