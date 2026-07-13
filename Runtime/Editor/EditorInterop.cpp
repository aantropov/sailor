#include "Sailor.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/FileId.h"
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
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <utility>

using namespace Sailor;

namespace
{
	void LogEditorTypeSerializationFailure(const char* message) noexcept
	{
		try
		{
			SAILOR_LOG_ERROR("Failed to serialize editor type metadata: %s", message);
		}
		catch (...)
		{
		}
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
	auto editor = GetSubmodule<Editor>();
	if (!editor || !yamlNode)
	{
		return 0;
	}

	auto node = editor->SerializeWorld();
	if (!node.IsNull())
	{
		std::string serializedNode = YAML::Dump(node);
		size_t length = serializedNode.length();

		yamlNode[0] = new char[length + 1];
		memcpy(yamlNode[0], serializedNode.c_str(), length);
		yamlNode[0][length] = '\0';

		return static_cast<uint32_t>(length);
	}

	yamlNode[0] = nullptr;
	return 0;
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
	try
	{
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

		auto serializedOutput = std::make_unique<char[]>(length + 1);
		memcpy(serializedOutput.get(), serializedNode.c_str(), length);
		serializedOutput[length] = '\0';
		yamlNode[0] = serializedOutput.release();

		return static_cast<uint32_t>(length);
	}
	catch (const std::exception& e)
	{
		LogEditorTypeSerializationFailure(e.what());
	}
	catch (...)
	{
		LogEditorTypeSerializationFailure("an unknown native exception was raised");
	}

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
	try
	{
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

		auto serializedOutput = std::make_unique<char[]>(length + 1);
		memcpy(serializedOutput.get(), serializedNode.c_str(), length);
		serializedOutput[length] = '\0';
		yamlNode[0] = serializedOutput.release();

		return static_cast<uint32_t>(length);
	}
	catch (const std::exception& e)
	{
		LogEditorTypeSerializationFailure(e.what());
	}
	catch (...)
	{
		LogEditorTypeSerializationFailure("an unknown native exception was raised while serializing workspace cache identity");
	}

	yamlNode[0] = nullptr;
	return 0;
}

bool App::LoadEditorWorld(const char* strFileId)
{
	auto editor = GetSubmodule<Editor>();
	auto engineLoop = GetSubmodule<EngineLoop>();
	auto assetRegistry = GetSubmodule<AssetRegistry>();
	if (!editor || !engineLoop || !assetRegistry || !strFileId || strFileId[0] == '\0')
	{
		return false;
	}

	FileId fileId;
	fileId.Deserialize(YAML::Node(std::string(strFileId)));

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
}

bool App::UpdateEditorObject(const char* strInstanceId, const char* strYamlNode)
{
	auto editor = GetSubmodule<Editor>();
	if (!editor || !strInstanceId || !strYamlNode)
	{
		return false;
	}

	InstanceId instanceId;
	YAML::Node instanceIdYaml = YAML::Load(strInstanceId);
	instanceId.Deserialize(instanceIdYaml);

	return editor->UpdateObject(instanceId, strYamlNode);
}

bool App::ReparentEditorObject(const char* strInstanceId, const char* strParentInstanceId, bool bKeepWorldTransform)
{
	auto editor = GetSubmodule<Editor>();
	if (!editor || !strInstanceId)
	{
		return false;
	}

	InstanceId instanceId;
	instanceId.Deserialize(YAML::Node(std::string(strInstanceId)));

	InstanceId parentInstanceId;
	if (strParentInstanceId && strParentInstanceId[0] != '\0')
	{
		parentInstanceId.Deserialize(YAML::Node(std::string(strParentInstanceId)));
	}

	return editor->ReparentObject(instanceId, parentInstanceId, bKeepWorldTransform);
}

bool App::CreateEditorGameObject(const char* strParentInstanceId)
{
	auto editor = GetSubmodule<Editor>();
	if (!editor)
	{
		return false;
	}

	InstanceId parentInstanceId;
	if (strParentInstanceId && strParentInstanceId[0] != '\0')
	{
		parentInstanceId.Deserialize(YAML::Node(std::string(strParentInstanceId)));
	}

	return editor->CreateGameObject(parentInstanceId);
}

bool App::DestroyEditorObject(const char* strInstanceId)
{
	auto editor = GetSubmodule<Editor>();
	if (!editor || !strInstanceId)
	{
		return false;
	}

	InstanceId instanceId;
	instanceId.Deserialize(YAML::Node(std::string(strInstanceId)));

	return editor->DestroyObject(instanceId);
}

bool App::ResetEditorComponentToDefaults(const char* strInstanceId)
{
	auto editor = GetSubmodule<Editor>();
	if (!editor || !strInstanceId)
	{
		return false;
	}

	InstanceId instanceId;
	instanceId.Deserialize(YAML::Node(std::string(strInstanceId)));

	return editor->ResetComponentToDefaults(instanceId);
}

bool App::AddEditorComponent(const char* strInstanceId, const char* strComponentTypeName)
{
	auto editor = GetSubmodule<Editor>();
	if (!editor || !strInstanceId || !strComponentTypeName)
	{
		return false;
	}

	InstanceId instanceId;
	instanceId.Deserialize(YAML::Node(std::string(strInstanceId)));

	return editor->AddComponent(instanceId, strComponentTypeName);
}

bool App::RemoveEditorComponent(const char* strInstanceId)
{
	auto editor = GetSubmodule<Editor>();
	if (!editor || !strInstanceId)
	{
		return false;
	}

	InstanceId instanceId;
	instanceId.Deserialize(YAML::Node(std::string(strInstanceId)));

	return editor->RemoveComponent(instanceId);
}

bool App::InstantiateEditorPrefab(const char* strFileId, const char* strParentInstanceId)
{
	auto editor = GetSubmodule<Editor>();
	if (!editor || !strFileId)
	{
		return false;
	}

	FileId fileId;
	fileId.Deserialize(YAML::Node(std::string(strFileId)));

	InstanceId parentInstanceId;
	if (strParentInstanceId && strParentInstanceId[0] != '\0')
	{
		parentInstanceId.Deserialize(YAML::Node(std::string(strParentInstanceId)));
	}

	return editor->InstantiatePrefab(fileId, parentInstanceId);
}

bool App::SetEditorSelection(const char* strSelectionYaml)
{
	auto editor = GetSubmodule<Editor>();
	auto* world = editor ? editor->GetWorld() : nullptr;
	if (!world || !strSelectionYaml)
	{
		return false;
	}

	TVector<InstanceId> selection;
	const YAML::Node yaml = YAML::Load(strSelectionYaml);
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
}

bool App::RenderPathTracedImage(const char* strOutputPath, const char* strInstanceId, uint32_t height, uint32_t samplesPerPixel, uint32_t maxBounces)
{
	if (!strOutputPath || strOutputPath[0] == '\0')
	{
		return false;
	}

	auto editor = GetSubmodule<Editor>();
	if (!editor)
	{
		return false;
	}

	InstanceId instanceId{};
	if (strInstanceId && strInstanceId[0] != '\0')
	{
		YAML::Node instanceIdYaml = YAML::Load(strInstanceId);
		instanceId.Deserialize(instanceIdYaml);
	}

	const std::string outputPath = strOutputPath;
	const bool bSuccess = editor->RenderPathTracedImage(instanceId, outputPath, height, samplesPerPixel, maxBounces);
	editor->PushMessage(bSuccess ?
		("Path tracer export succeeded: " + outputPath) :
		("Path tracer export failed: " + outputPath));

	return bSuccess;
}

void App::ShowMainWindow(bool bShow)
{
	if (auto editor = GetSubmodule<Editor>())
	{
		editor->ShowMainWindow(bShow);
	}
}
