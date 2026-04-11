struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

// dllmain.cpp : Defines the entry point for the DLL application.
#include <windows.h>
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
		SAILOR_PROFILE_FUNCTION();
		Sailor::App::SetEditorViewport(windowPosX, windowPosY, width, height);
	}

	SAILOR_API bool UpsertRemoteViewport(uint64_t viewportId, uint32_t windowPosX, uint32_t windowPosY, uint32_t width, uint32_t height, bool bVisible, bool bFocused)
	{
		SAILOR_PROFILE_FUNCTION();
		return Sailor::App::UpsertEditorRemoteViewport(viewportId, windowPosX, windowPosY, width, height, bVisible, bFocused);
	}

	SAILOR_API bool DestroyRemoteViewport(uint64_t viewportId)
	{
		SAILOR_PROFILE_FUNCTION();
		return Sailor::App::DestroyEditorRemoteViewport(viewportId);
	}

	SAILOR_API uint32_t GetRemoteViewportState(uint64_t viewportId)
	{
		SAILOR_PROFILE_FUNCTION();
		return Sailor::App::GetEditorRemoteViewportState(viewportId);
	}

	SAILOR_API uint32_t GetRemoteViewportDiagnostics(uint64_t viewportId, char** diagnostics)
	{
		SAILOR_PROFILE_FUNCTION();
		return Sailor::App::GetEditorRemoteViewportDiagnostics(viewportId, diagnostics);
	}

	SAILOR_API bool RetryRemoteViewport(uint64_t viewportId)
	{
		SAILOR_PROFILE_FUNCTION();
		return Sailor::App::RetryEditorRemoteViewport(viewportId);
	}

	SAILOR_API uint32_t GetMessages(char** messages, uint32_t num)
	{
		SAILOR_PROFILE_FUNCTION();
		return Sailor::App::PullEditorMessages(messages, num);
	}

	SAILOR_API uint32_t SerializeCurrentWorld(char** yamlNode)
	{
		SAILOR_PROFILE_FUNCTION();
		return Sailor::App::SerializeCurrentWorld(yamlNode);
	}

	SAILOR_API uint32_t SerializeEngineTypes(char** yamlNode)
	{
		SAILOR_PROFILE_FUNCTION();

		auto node = Sailor::Reflection::ExportEngineTypes();

		if (!node.IsNull())
		{
			std::string serializedNode = YAML::Dump(node);
			size_t length = serializedNode.length();

			yamlNode[0] = new char[length + 1];
			strcpy_s(yamlNode[0], length + 1, serializedNode.c_str());
			yamlNode[0][length] = '\0';

			return static_cast<uint32_t>(length);
		}
		else
		{
			yamlNode[0] = nullptr;
			return 0;
		}
	}

	SAILOR_API bool UpdateObject(char* strInstanceId, char* strYamlNode)
	{
		SAILOR_PROFILE_FUNCTION();
		return Sailor::App::UpdateEditorObject(strInstanceId, strYamlNode);
	}

	SAILOR_API bool RenderPathTracedImage(char* strOutputPath, char* strInstanceId, uint32_t height, uint32_t samplesPerPixel, uint32_t maxBounces)
	{
		SAILOR_PROFILE_FUNCTION();

		if (!strOutputPath || strOutputPath[0] == '\0')
		{
			return false;
		}

		if (!Sailor::App::HasEditor())
		{
			return false;
		}

		return Sailor::App::RenderPathTracedImage(strOutputPath, strInstanceId, height, samplesPerPixel, maxBounces);
	}

	SAILOR_API void ShowMainWindow(bool bShow)
	{
		Sailor::App::ShowMainWindow(bShow);
	}

	SAILOR_API void RunVectorBenchmark()
	{
		Sailor::RunVectorBenchmark();
	}

	SAILOR_API void RunSetBenchmark()
	{
		Sailor::RunSetBenchmark();
	}

	SAILOR_API void RunMapBenchmark()
	{
		Sailor::RunMapBenchmark();
	}

	SAILOR_API void RunListBenchmark()
	{
		Sailor::RunListBenchmark();
	}
}

BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

