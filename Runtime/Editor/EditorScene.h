#pragma once

#include "Core/Defines.h"

#include <cstdint>
#include <string>

namespace Sailor::EditorRuntime
{
	SAILOR_API uint32_t SerializeCurrentWorld(char** yamlNode);
	SAILOR_API bool LoadEditorWorld(const char* strFileId);
	SAILOR_API bool CreateEditorWorld();
	SAILOR_API bool SetEditorSimulationEnabled(bool bEnabled);
	SAILOR_API bool IsEditorSimulationEnabled();

	SAILOR_API uint64_t GetEditorManagedMutationRevision(uint32_t kind, const char* strInstanceId);
	SAILOR_API bool UpdateEditorObject(const char* strInstanceId, const char* strYamlNode);
	SAILOR_API bool DestroyEditorObject(const char* strInstanceId);
	SAILOR_API bool ResetEditorComponentToDefaults(const char* strInstanceId);
	SAILOR_API bool RemoveEditorComponent(const char* strInstanceId);
	SAILOR_API bool SetEditorPrefabLink(
		const char* strInstanceId,
		const char* strFileId);
	SAILOR_API bool BreakEditorPrefabLink(const char* strInstanceId);

	SAILOR_API bool ReparentEditorObject(const char* strInstanceId, const char* strParentInstanceId, bool bKeepWorldTransform);
	SAILOR_API bool CreateEditorGameObject(const char* strParentInstanceId, const char* strPreferredInstanceId, char** outInstanceId);
	SAILOR_API bool CreateEditorModelInstance(
		const char* strModelFileId,
		const char* strName,
		const char* strParentInstanceId,
		bool bCreateHierarchy,
		bool bHasWorldPosition,
		float worldX,
		float worldY,
		float worldZ,
		const char* strPreferredInstanceId,
		char** outInstanceId);
	SAILOR_API bool AddEditorComponent(const char* strInstanceId, const char* strComponentTypeName, const char* strPreferredInstanceId, char** outInstanceId);
	SAILOR_API bool InstantiateEditorPrefab(const char* strFileId, const char* strParentInstanceId);
	SAILOR_API bool InstantiateEditorPrefabInstance(
		const char* strFileId,
		const char* strParentInstanceId,
		bool bHasWorldPosition,
		float worldX,
		float worldY,
		float worldZ,
		char** outInstanceId);
	SAILOR_API bool InstantiateEditorPrefabFromYaml(
		const char* strPrefabYaml,
		const char* strParentInstanceId);
	SAILOR_API bool InstantiateEditorPrefabFromYaml(
		const char* strPrefabYaml,
		const char* strParentInstanceId,
		bool bStrictInstanceIds);
	SAILOR_API bool InstantiateEditorPrefabFromYaml(
		const char* strPrefabYaml,
		const char* strParentInstanceId,
		bool bStrictInstanceIds,
		char** outInstanceId);

	SAILOR_API bool SetEditorAnimatorParameter(
		const char* strInstanceId,
		const char* strName,
		uint32_t valueKind,
		float floatValue,
		int32_t intValue,
		bool boolValue);
	SAILOR_API bool GetEditorAnimatorState(
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
		float& outTransitionAlpha);
}
