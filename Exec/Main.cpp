#include "Sailor.h"
using namespace Sailor;

#if defined(_WIN32)
#include <wtypes.h>
#include <shellapi.h>
#include <windows.h>
#include <string>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int32_t)
{
	int32_t nArgs = 0;
	LPWSTR* wideArguments = CommandLineToArgvW(GetCommandLineW(), &nArgs);
	if (!wideArguments) return 1;

	TVector<std::string> argumentStorage;
	argumentStorage.Reserve(static_cast<size_t>(nArgs));
	for (int32_t i = 0; i < nArgs; ++i) argumentStorage.Add(Utils::wchar_to_UTF8(wideArguments[i]));
	LocalFree(wideArguments);

	TVector<const char*> arguments;
	arguments.Reserve(static_cast<size_t>(nArgs));
	for (const auto& argument : argumentStorage) arguments.Add(argument.c_str());

	const auto initialization = App::Initialize(arguments.GetData(), nArgs);
	if (initialization == EAppInitializationResult::Ready)
	{
		App::Start();
	}
	const int32_t exitCode = initialization == EAppInitializationResult::Failed ? 1 : App::GetExitCode();
	App::Stop();
	const bool bShutdown = App::Shutdown();

	return bShutdown ? exitCode : 1;
}

#else

int main(int argc, const char** argv)
{
	const auto initialization = App::Initialize(argv, argc);
	if (initialization == EAppInitializationResult::Ready)
	{
		App::Start();
	}
	const int32_t exitCode = initialization == EAppInitializationResult::Failed ? 1 : App::GetExitCode();
	App::Stop();
	return App::Shutdown() ? exitCode : 1;
}

#endif
