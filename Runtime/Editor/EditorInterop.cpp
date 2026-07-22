#include "Sailor.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "Core/Reflection.h"
#include "Engine/EngineLoop.h"
#include "Engine/World.h"
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

bool App::InstantiateEditorPrefabFromYaml(const char* strPrefabYaml, const char* strParentInstanceId)
{
	if (!strPrefabYaml || strPrefabYaml[0] == '\0')
	{
		return false;
	}

	const std::string prefabYaml = strPrefabYaml;
	const std::string parentInstanceIdValue = strParentInstanceId ? strParentInstanceId : "";
	return ExecuteOnEngineMainThread<bool>(false, [prefabYaml, parentInstanceIdValue]()
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
			return editor->InstantiatePrefab(prefab, parentInstanceId);
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
		editor->ShowMainWindow(bShow);
	}
}
