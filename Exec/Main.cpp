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
	if (!wideArguments)
	{
		return 1;
	}

	TVector<std::string> argumentStorage;
	argumentStorage.Reserve(static_cast<size_t>(nArgs));
	for (int32_t i = 0; i < nArgs; ++i)
	{
		argumentStorage.Add(Utils::wchar_to_UTF8(wideArguments[i]));
	}
	LocalFree(wideArguments);

	TVector<const char*> arguments;
	arguments.Reserve(static_cast<size_t>(nArgs));
	for (const auto& argument : argumentStorage)
	{
		arguments.Add(argument.c_str());
	}

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

#if defined(__linux__)
#include "Workspace/WorkspaceContext.h"
#include "Workspace/WorkspacePathEncoding.h"
#include <nlohmann/json.hpp>
#include <iostream>
#endif

int main(int argc, const char** argv)
{
#if defined(__linux__)
	if (argc == 3 && std::string_view(argv[1]) == "--describe-workspace")
	{
		const auto resolved = Workspace::ResolveWorkspaceContext(Workspace::PathFromUtf8(argv[2]));
		if (!resolved.IsSuccess())
		{
			std::cerr << resolved.m_message << '\n';
			return 1;
		}
		const auto& context = resolved.m_context;
		const nlohmann::json description = {
			{ "schemaVersion", 1 },
			{ "legacy", context.IsLegacy() },
			{ "workspaceId", context.GetWorkspaceId() },
			{ "name", context.GetWorkspaceName() },
			{ "content", Workspace::PathToUtf8(context.GetContent()) },
			{ "engineContent", Workspace::PathToUtf8(context.GetEngineContent()) },
			{ "logicOutput", Workspace::PathToUtf8(context.GetLogicOutput()) },
			{ "logicModuleName", context.GetModuleName() }
		};
		std::cout << description.dump() << '\n';
		return 0;
	}
#endif
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
