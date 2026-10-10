#include "Workspace/WorkspaceContext.h"
#include "Workspace/WorkspaceModuleManager.h"

#include <fstream>
#include <iostream>

namespace
{
	int Run(const std::filesystem::path& workspace, const std::filesystem::path& output)
	{
		const auto context = Sailor::Workspace::ResolveWorkspaceContext(workspace);
		if (!context.IsSuccess())
		{
			std::cerr << context.m_message;
			return 1;
		}
		Sailor::Workspace::WorkspaceModuleManager manager;
		const auto& result = manager.Load(context.m_context, "Release");
		if (!result.IsSuccess())
		{
			std::cerr << result.m_message;
			return 1;
		}
		{
			std::ofstream metadata(output, std::ios::binary);
			metadata << manager.GetMetadata();
			metadata.flush();
			if (!metadata.good()) return 1;
		}
		std::cout << "ready" << std::endl;
		std::string stop;
		std::getline(std::cin, stop);
		return manager.Unload() ? 0 : 1;
	}
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
	return argc == 3 ? Run(argv[1], argv[2]) : 1;
}
