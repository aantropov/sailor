struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

// dllmain.cpp : Defines the entry point for the DLL application.
#include <windows.h>
#include "Sailor.h"

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

