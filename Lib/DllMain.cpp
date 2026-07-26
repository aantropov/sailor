struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

// DllMain.cpp : Defines the entry point for the DLL application.
#include <windows.h>

#include "Containers/List.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/Vector.h"
#include "Core/Reflection.h"
#include "Sailor.h"

extern "C"
{
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

BOOL APIENTRY DllMain(
	HMODULE hModule,
	DWORD ulReasonForCall,
	LPVOID lpReserved)
{
	(void)hModule;
	(void)lpReserved;

	switch (ulReasonForCall)
	{
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}
