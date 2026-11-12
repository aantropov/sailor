#include "Sailor.h"
using namespace Sailor;

#include "Platform/FileSystem.h"
#include "Workspace/WorkspaceContext.h"
#include "Workspace/WorkspacePathEncoding.h"
#include "Workspace/WorkspacePackage.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <optional>

namespace
{
	std::optional<int> RunToolCommand(int argc, const char** argv)
	{
		if (argc >= 2 && (std::string_view(argv[1]) == "--pack-assets" ||
			std::string_view(argv[1]) == "--pack-cache"))
		{
			if (argc != 5)
			{
				std::cerr << "Usage: --pack-assets SOURCE OUTPUT stored|fastest|balanced|smallest\n"
					"       --pack-cache WORKSPACE OUTPUT stored|fastest|balanced|smallest\n";
				return 1;
			}
			Sailor::FileSystem::EArchiveCompression compression;
			const std::string_view mode(argv[4]);
			if (mode == "stored")
			{
				compression = Sailor::FileSystem::EArchiveCompression::Stored;
			}
			else if (mode == "fastest")
			{
				compression = Sailor::FileSystem::EArchiveCompression::Fastest;
			}
			else if (mode == "balanced")
			{
				compression = Sailor::FileSystem::EArchiveCompression::Balanced;
			}
			else if (mode == "smallest")
			{
				compression = Sailor::FileSystem::EArchiveCompression::Smallest;
			}
			else
			{
				std::cerr << "Unsupported archive compression: " << mode << '\n';
				return 1;
			}
			std::string diagnostic;
			bool success;
			if (std::string_view(argv[1]) == "--pack-cache")
			{
				const auto resolved = Sailor::Workspace::ResolveWorkspaceContext(Sailor::Workspace::PathFromUtf8(argv[2]));
				if (!resolved.IsSuccess())
				{
					std::cerr << resolved.m_message << '\n';
					return 1;
				}
				success = Sailor::Workspace::WriteInitialCacheArchive(resolved.m_context,
					Sailor::Workspace::PathFromUtf8(argv[3]), compression, diagnostic);
			}
			else
			{
				success = Sailor::FileSystem::WriteZipArchive(Sailor::Workspace::PathFromUtf8(argv[2]),
					Sailor::Workspace::PathFromUtf8(argv[3]), compression, diagnostic);
			}
			if (!success)
			{
				std::cerr << diagnostic << '\n';
				return 1;
			}
			std::cout << "Asset archive: " << argv[3] << '\n';
			return 0;
		}
		if (argc >= 2 && std::string_view(argv[1]) == "--describe-workspace")
		{
			if (argc != 3)
			{
				std::cerr << "Usage: --describe-workspace WORKSPACE\n";
				return 1;
			}
			const auto resolved = Sailor::Workspace::ResolveWorkspaceContext(Sailor::Workspace::PathFromUtf8(argv[2]));
			if (!resolved.IsSuccess())
			{
				std::cerr << resolved.m_message << '\n';
				return 1;
			}
			const auto& context = resolved.m_context;
			const nlohmann::json description = {
				{ "schemaVersion", 1 }, { "legacy", context.IsLegacy() },
				{ "workspaceId", context.GetWorkspaceId() }, { "name", context.GetWorkspaceName() },
				{ "content", Sailor::Workspace::PathToUtf8(context.GetContent()) },
				{ "engineContent", Sailor::Workspace::PathToUtf8(context.GetEngineContent()) },
				{ "logicOutput", Sailor::Workspace::PathToUtf8(context.GetLogicOutput()) },
				{ "logicModuleName", context.GetModuleName() }
			};
			std::cout << description.dump() << '\n';
			return 0;
		}
		return std::nullopt;
	}
}

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

	if (const auto result = RunToolCommand(nArgs, arguments.GetData()))
	{
		return *result;
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

int main(int argc, const char** argv)
{
	if (const auto result = RunToolCommand(argc, argv))
	{
		return *result;
	}
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
