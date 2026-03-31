#include "Sailor.h"
#include "RHI/Renderer.h"

using namespace Sailor;

#if defined(_WIN32)
#include <wtypes.h>
#include <shellapi.h>
#include <windows.h>
#include <vector>
#include <string>

static const char** ConvertToAnsi(LPWSTR* szArglist, int nArgs)
{
	std::vector<std::string> ansiArgs;
	ansiArgs.reserve(nArgs);

	for (int i = 0; i < nArgs; ++i)
	{
		const int len = WideCharToMultiByte(CP_ACP, 0, szArglist[i], -1, NULL, 0, NULL, NULL);
		std::string str(len, '\0');
		WideCharToMultiByte(CP_ACP, 0, szArglist[i], -1, &str[0], len, NULL, NULL);
		ansiArgs.push_back(str);
	}

	const char** result = new const char* [nArgs];
	for (int i = 0; i < nArgs; ++i)
	{
		result[i] = _strdup(ansiArgs[i].c_str());
	}

	return result;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int32_t)
{
	int32_t nArgs = 0;
	LPWSTR* szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
	const char** ansiArgs = ConvertToAnsi(szArglist, nArgs);

	App::Initialize(ansiArgs, nArgs);
	if (auto renderer = App::GetSubmodule<RHI::Renderer>(); !renderer || !renderer->IsInitialized())
	{
		App::Shutdown();
		return 1;
	}
	App::Start();
	const int32_t exitCode = App::GetExitCode();
	App::Stop();
	App::Shutdown();

	for (int i = 0; i < nArgs; ++i)
	{
		free((void*)ansiArgs[i]);
	}

	delete[] ansiArgs;

	if (szArglist)
	{
		LocalFree(szArglist);
	}

	return exitCode;
}

#else

int main(int argc, const char** argv)
{
	App::Initialize(argv, argc);
	if (auto renderer = App::GetSubmodule<RHI::Renderer>(); !renderer || !renderer->IsInitialized())
	{
		App::Shutdown();
		return 1;
	}
	App::Start();
	const int32_t exitCode = App::GetExitCode();
	App::Stop();
	App::Shutdown();
	return exitCode;
}

#endif
