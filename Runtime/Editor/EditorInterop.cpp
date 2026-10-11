#include "EditorInterop.h"
#include "EditorScene.h"
#include "Sailor.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "Core/Reflection.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Engine/InstanceId.h"
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
	void LogEditorTypeSerializationFailure(const char* message) noexcept
	{
		SAILOR_LOG_ERROR("Failed to serialize editor type metadata: %s", message);
	}


	bool TryParseOptionalParent(std::string_view value, InstanceId& outParent)
	{
		outParent = InstanceId::Invalid;
		if (value.empty())
		{
			return true;
		}

		outParent = InstanceId(value);
		return outParent.IsGameObjectId();
	}

	bool TryParseOptionalGameObjectId(std::string_view value, InstanceId& outInstanceId)
	{
		outInstanceId = InstanceId::Invalid;
		if (value.empty())
		{
			return true;
		}

		outInstanceId = InstanceId(value);
		return outInstanceId.IsGameObjectId();
	}

	bool TryParseOptionalComponentId(std::string_view value, InstanceId& outInstanceId)
	{
		outInstanceId = InstanceId::Invalid;
		if (value.empty())
		{
			return true;
		}

		outInstanceId = InstanceId(value);
		return outInstanceId.ComponentId() != InstanceId::Invalid &&
			outInstanceId.GameObjectId() != InstanceId::Invalid;
	}

	void SetInteropString(std::string_view value, char** outValue)
	{
		auto result = TUniquePtr<char[]>::Make(value.size() + 1);
		std::copy(value.begin(), value.end(), result.GetRawPtr());
		result[value.size()] = '\0';
		outValue[0] = result.Release();
	}
}

uint32_t EditorRuntime::PullEditorMessages(char** messages, uint32_t num)
{
	auto editor = App::GetSubmodule<Editor>();
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

uint32_t EditorRuntime::SerializeEngineTypes(char** yamlNode)
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

		std::filesystem::create_directories(Workspace::PathFromUtf8(AssetRegistry::GetCacheFolder()));
		AssetRegistry::WriteTextFile(AssetRegistry::GetCacheFolder() + "EngineTypes.yaml", serializedNode);

		yamlNode[0] = new char[length + 1];
		memcpy(yamlNode[0], serializedNode.c_str(), length);
		yamlNode[0][length] = '\0';

		return static_cast<uint32_t>(length);
	}

	yamlNode[0] = nullptr;
	return 0;
}

uint32_t EditorRuntime::SerializeEditorTypes(char** yamlNode)
{
	if (!yamlNode)
	{
		return 0;
	}

	yamlNode[0] = nullptr;
	YAML::Node editorTypes = Reflection::ExportEngineTypes();
	if (const auto* module = App::GetWorkspaceModuleManager())
	{
		YAML::Node combinedTypes;
		std::string mergeError;
		if (!module->BuildEditorTypeMetadata(
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
}

uint32_t EditorRuntime::SerializeWorkspaceCacheIdentity(char** yamlNode)
{
	if (!yamlNode || !App::GetInstance())
	{
		return 0;
	}

	yamlNode[0] = nullptr;
	auto identity = Workspace::MakeWorkspaceCacheIdentity(
		"editor-types",
		"editor-types-v1",
		1,
		App::GetWorkspaceContext());
	const auto* module = App::GetWorkspaceModuleManager();
	if (module && module->IsRegistered())
	{
		identity.m_producerIdentity += ";module-types=" + std::to_string(module->GetTypeCatalogHash());
	}

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
}

bool EditorRuntime::PreviewEditorAudioAsset(const char* strFileId)
{
	if (!strFileId || strFileId[0] == '\0')
	{
		return false;
	}

	const std::string fileIdValue = strFileId;
	return App::ExecuteOnEngineMainThread<bool>(false, [fileIdValue]()
		{
			auto* editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			const FileId fileId(fileIdValue);
			return fileId && editor->PreviewAudioAsset(fileId);
		});
}

bool EditorRuntime::RequestModelFingerprint(const char* strFileId)
{
	if (!strFileId || !strFileId[0])
	{
		return false;
	}
	return App::ExecuteOnEngineMainThread<bool>(false, [value = std::string(strFileId)]()
		{
			auto* importer = App::GetSubmodule<ModelImporter>();
			const FileId fileId(value);
			return importer && fileId && importer->RequestFingerprint(fileId);
		});
}

uint32_t EditorRuntime::GetModelFingerprintStatus(const char* strFileId)
{
	if (!strFileId || !strFileId[0])
	{
		return static_cast<uint32_t>(ModelImporter::EFingerprintStatus::Unavailable);
	}
	return App::ExecuteOnEngineMainThread<uint32_t>(static_cast<uint32_t>(ModelImporter::EFingerprintStatus::Unavailable),
		[value = std::string(strFileId)]()
		{
			auto* importer = App::GetSubmodule<ModelImporter>();
			const FileId fileId(value);
			return static_cast<uint32_t>(importer && fileId ? importer->GetFingerprintStatus(fileId) :
				ModelImporter::EFingerprintStatus::Unavailable);
		});
}

bool EditorRuntime::ReparentEditorObject(const char* strInstanceId, const char* strParentInstanceId, bool bKeepWorldTransform)
{
	if (!strInstanceId)
	{
		return false;
	}

	const std::string instanceIdValue = strInstanceId;
	const std::string parentInstanceIdValue = strParentInstanceId ? strParentInstanceId : "";
	return App::ExecuteOnEngineMainThread<bool>(false, [instanceIdValue, parentInstanceIdValue, bKeepWorldTransform]()
		{
			auto editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			const InstanceId instanceId(instanceIdValue);
			InstanceId parentInstanceId;
			if (!TryParseOptionalParent(parentInstanceIdValue, parentInstanceId))
			{
				return false;
			}

			return editor->ReparentObject(instanceId, parentInstanceId, bKeepWorldTransform);
		});
}

bool EditorRuntime::CreateEditorGameObject(
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
	return App::ExecuteOnEngineMainThread<bool>(false, [parentInstanceIdValue, preferredInstanceIdValue, outInstanceId]()
		{
			auto editor = App::GetSubmodule<Editor>();
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

bool EditorRuntime::CreateEditorModelInstance(
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
	const FileId modelFileId(strModelFileId);
	auto modelImporter = App::GetSubmodule<ModelImporter>();
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
	return App::ExecuteOnEngineMainThread<bool>(false, [
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
			auto editor = App::GetSubmodule<Editor>();
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

bool EditorRuntime::AddEditorComponent(
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
	return App::ExecuteOnEngineMainThread<bool>(false, [instanceIdValue, componentTypeName, preferredInstanceIdValue, outInstanceId]()
		{
			auto editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			const InstanceId instanceId(instanceIdValue);

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

bool EditorRuntime::InstantiateEditorPrefab(const char* strFileId, const char* strParentInstanceId)
{
	if (!strFileId)
	{
		return false;
	}

	const std::string fileIdValue = strFileId;
	const std::string parentInstanceIdValue = strParentInstanceId ? strParentInstanceId : "";
	return App::ExecuteOnEngineMainThread<bool>(false, [fileIdValue, parentInstanceIdValue]()
		{
			auto editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			const FileId fileId(fileIdValue);
			InstanceId parentInstanceId;
			if (!TryParseOptionalParent(parentInstanceIdValue, parentInstanceId))
			{
				return false;
			}

			return editor->InstantiatePrefab(fileId, parentInstanceId);
		});
}

bool EditorRuntime::InstantiateEditorPrefabInstance(
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
	return App::ExecuteOnEngineMainThread<bool>(
		false,
		[fileIdValue,
			parentInstanceIdValue,
			bHasWorldPosition,
			worldX,
			worldY,
			worldZ,
			outInstanceId]()
		{
			auto editor = App::GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			const FileId fileId(fileIdValue);
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

bool EditorRuntime::InstantiateEditorPrefabFromYaml(
	const char* strPrefabYaml,
	const char* strParentInstanceId)
{
	return InstantiateEditorPrefabFromYaml(
		strPrefabYaml,
		strParentInstanceId,
		false);
}

bool EditorRuntime::InstantiateEditorPrefabFromYaml(
	const char* strPrefabYaml,
	const char* strParentInstanceId,
	bool bStrictInstanceIds)
{
	return InstantiateEditorPrefabFromYaml(
		strPrefabYaml,
		strParentInstanceId,
		bStrictInstanceIds,
		nullptr);
}

bool EditorRuntime::InstantiateEditorPrefabFromYaml(
	const char* strPrefabYaml,
	const char* strParentInstanceId,
	bool bStrictInstanceIds,
	char** outInstanceId)
{
	if (!strPrefabYaml || strPrefabYaml[0] == '\0')
	{
		return false;
	}

	const std::string prefabYaml = strPrefabYaml;
	const std::string parentInstanceIdValue = strParentInstanceId ? strParentInstanceId : "";
	return App::ExecuteOnEngineMainThread<bool>(
		false,
		[prefabYaml, parentInstanceIdValue, bStrictInstanceIds, outInstanceId]()
		{
			auto editor = App::GetSubmodule<Editor>();
			auto prefabImporter = App::GetSubmodule<PrefabImporter>();
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

			InstanceId createdInstanceId;
			const bool bInstantiated = editor->InstantiatePrefab(
				prefab,
				parentInstanceId,
				nullptr,
				createdInstanceId,
				bStrictInstanceIds ? EPrefabInstanceIdPolicy::RequireExact : EPrefabInstanceIdPolicy::GenerateNew);
			if (bInstantiated && outInstanceId)
			{
				SetInteropString(
					createdInstanceId.ToString(),
					outInstanceId);
			}

			return bInstantiated;
		});
}
