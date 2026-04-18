#if !defined(_WIN32)

#include "Sailor.h"
#include "Core/Reflection.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"
#include "Containers/Map.h"
#include "Containers/List.h"

extern "C"
{
	SAILOR_API void Initialize(const char** commandLineArgs, int32_t num)
	{
		Sailor::App::Initialize(commandLineArgs, num);
	}

	SAILOR_API void Start()
	{
		Sailor::App::Start();
	}

	SAILOR_API void Stop()
	{
		Sailor::App::Stop();
	}

	SAILOR_API void Shutdown()
	{
		Sailor::App::Shutdown();
	}

	SAILOR_API void SetViewport(uint32_t windowPosX, uint32_t windowPosY, uint32_t width, uint32_t height)
	{
		Sailor::App::SetEditorViewport(windowPosX, windowPosY, width, height);
	}

	SAILOR_API void SetEditorRenderTargetSize(uint32_t width, uint32_t height)
	{
		Sailor::App::SetEditorRenderTargetSize(width, height);
	}

	SAILOR_API bool UpsertRemoteViewport(uint64_t viewportId, uint32_t windowPosX, uint32_t windowPosY, uint32_t width, uint32_t height, bool bVisible, bool bFocused)
	{
		return Sailor::App::UpsertEditorRemoteViewport(viewportId, windowPosX, windowPosY, width, height, bVisible, bFocused);
	}

	SAILOR_API bool DestroyRemoteViewport(uint64_t viewportId)
	{
		return Sailor::App::DestroyEditorRemoteViewport(viewportId);
	}

	SAILOR_API uint32_t GetRemoteViewportState(uint64_t viewportId)
	{
		return Sailor::App::GetEditorRemoteViewportState(viewportId);
	}

	SAILOR_API uint32_t GetRemoteViewportDiagnostics(uint64_t viewportId, char** diagnostics)
	{
		return Sailor::App::GetEditorRemoteViewportDiagnostics(viewportId, diagnostics);
	}

	SAILOR_API bool RetryRemoteViewport(uint64_t viewportId)
	{
		return Sailor::App::RetryEditorRemoteViewport(viewportId);
	}

	SAILOR_API bool SetRemoteViewportMacHostHandle(uint64_t viewportId, uint32_t hostHandleKind, uint64_t hostHandleValue)
	{
		return Sailor::App::SetEditorRemoteViewportMacHostHandle(viewportId, hostHandleKind, hostHandleValue);
	}

	SAILOR_API bool SendRemoteViewportInput(uint64_t viewportId, uint32_t kind, float pointerX, float pointerY, float wheelDeltaX, float wheelDeltaY, uint32_t keyCode, uint32_t button, uint32_t modifiers, bool bPressed, bool bFocused, bool bCaptured)
	{
		return Sailor::App::SendEditorRemoteViewportInput(viewportId, kind, pointerX, pointerY, wheelDeltaX, wheelDeltaY, keyCode, button, modifiers, bPressed, bFocused, bCaptured);
	}

	SAILOR_API uint32_t GetMessages(char** messages, uint32_t num)
	{
		return Sailor::App::PullEditorMessages(messages, num);
	}

	SAILOR_API uint32_t SerializeCurrentWorld(char** yamlNode)
	{
		return Sailor::App::SerializeCurrentWorld(yamlNode);
	}

	SAILOR_API uint32_t SerializeEngineTypes(char** yamlNode)
	{
		return Sailor::App::SerializeEngineTypes(yamlNode);
	}

	SAILOR_API void FreeInteropString(char* text)
	{
		delete[] text;
	}

	SAILOR_API bool UpdateObject(char* strInstanceId, char* strYamlNode)
	{
		return Sailor::App::UpdateEditorObject(strInstanceId, strYamlNode);
	}

	SAILOR_API bool ReparentObject(char* strInstanceId, char* strParentInstanceId, bool bKeepWorldTransform)
	{
		return Sailor::App::ReparentEditorObject(strInstanceId, strParentInstanceId, bKeepWorldTransform);
	}

	SAILOR_API bool CreateGameObject(char* strParentInstanceId)
	{
		return Sailor::App::CreateEditorGameObject(strParentInstanceId);
	}

	SAILOR_API bool DestroyObject(char* strInstanceId)
	{
		return Sailor::App::DestroyEditorObject(strInstanceId);
	}

	SAILOR_API bool ResetComponentToDefaults(char* strInstanceId)
	{
		return Sailor::App::ResetEditorComponentToDefaults(strInstanceId);
	}

	SAILOR_API bool AddComponent(char* strInstanceId, char* strComponentTypeName)
	{
		return Sailor::App::AddEditorComponent(strInstanceId, strComponentTypeName);
	}

	SAILOR_API bool RemoveComponent(char* strInstanceId)
	{
		return Sailor::App::RemoveEditorComponent(strInstanceId);
	}

	SAILOR_API bool InstantiatePrefab(char* strFileId, char* strParentInstanceId)
	{
		return Sailor::App::InstantiateEditorPrefab(strFileId, strParentInstanceId);
	}

	SAILOR_API bool RenderPathTracedImage(char* strOutputPath, char* strInstanceId, uint32_t height, uint32_t samplesPerPixel, uint32_t maxBounces)
	{
		if (!strOutputPath || strOutputPath[0] == '\0' || !Sailor::App::HasEditor())
		{
			return false;
		}
		return Sailor::App::RenderPathTracedImage(strOutputPath, strInstanceId, height, samplesPerPixel, maxBounces);
	}

	SAILOR_API void ShowMainWindow(bool bShow)
	{
		Sailor::App::ShowMainWindow(bShow);
	}
}

#endif
