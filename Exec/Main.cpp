struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#include <wtypes.h>
#include <shellapi.h>
#include <iostream>
#include <windows.h>

#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"

using namespace Sailor;

const char** ConvertToAnsi(LPWSTR* szArglist, int nArgs) {
	std::vector<std::string> ansiArgs;

	for (int i = 0; i < nArgs; ++i) {
		int len = WideCharToMultiByte(CP_ACP, 0, szArglist[i], -1, NULL, 0, NULL, NULL);
		std::string str(len, '\0');
		WideCharToMultiByte(CP_ACP, 0, szArglist[i], -1, &str[0], len, NULL, NULL);
		ansiArgs.push_back(str);
	}

	// Create an array to hold the converted ANSI strings
	const char** result = new const char* [nArgs];

	for (int i = 0; i < nArgs; ++i) {
		result[i] = _strdup(ansiArgs[i].c_str());
	}

	return result;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmdArds, int32_t num)
{
	int32_t nArgs = 0;
	LPWSTR* szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
	const char** ansiArgs = ConvertToAnsi(szArglist, nArgs);

	{
		App::Initialize(ansiArgs, nArgs);
		App::Start();
		App::Stop();
		App::Shutdown();
	}

	for (int i = 0; i < nArgs; ++i)
	{
		free((void*)ansiArgs[i]);
	}

	delete[] ansiArgs;

	if (szArglist)
	{
		LocalFree(szArglist);
	}

	return 0;
}
