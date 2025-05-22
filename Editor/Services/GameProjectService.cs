using System;
using System.IO;
using System.Linq;

namespace SailorEditor.Services
{
    internal class GameProjectService
    {
        private static readonly string RootDir = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", ".."));
        private static readonly string GamesDir = Path.Combine(RootDir, "Games");

        private const string CMakeInsertAfter = "add_subdirectory(Lib)";

        private const string MainCpp = """struct IUnknown; // Workaround for \"combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here\" when using /permissive-

#include <wtypes.h>
#include <shellapi.h>
#include <windows.h>

#include \"Sailor.h\"

using namespace Sailor;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
\tApp::Initialize(nullptr, 0);
\tApp::Start();
\tApp::Stop();
\tApp::Shutdown();
\treturn 0;
}
""";

        public void CreateGameProject(string name)
        {
            Directory.CreateDirectory(GamesDir);
            string projectDir = Path.Combine(GamesDir, name);
            Directory.CreateDirectory(projectDir);

            string cmakeContent = $"add_executable({name} WIN32 Main.cpp)\n" +
                                   $"add_dependencies({name} SailorLib)\n" +
                                   $"target_link_libraries({name} SailorLib)\n" +
                                   $"target_compile_definitions({name} PUBLIC NOMINMAX)\n" +
                                   $"set_property(TARGET {name} PROPERTY FOLDER \"Games\")\n" +
                                   $"set_property(TARGET {name} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY \"${{SAILOR_BINARIES_DIR}}\")\n";

            File.WriteAllText(Path.Combine(projectDir, "CMakeLists.txt"), cmakeContent);
            File.WriteAllText(Path.Combine(projectDir, "Main.cpp"), MainCpp);

            UpdateRootCMake(name);
        }

        private static void UpdateRootCMake(string projectName)
        {
            string cmakePath = Path.Combine(RootDir, "CMakeLists.txt");
            string subdirLine = $"add_subdirectory(Games/{projectName})\n";

            var lines = File.ReadAllLines(cmakePath).ToList();

            if (lines.Any(l => l.Trim() == subdirLine.Trim()))
            {
                return;
            }

            int idx = lines.FindIndex(l => l.Trim().StartsWith(CMakeInsertAfter));
            if (idx >= 0)
            {
                lines.Insert(idx + 1, subdirLine);
            }
            else
            {
                lines.Add(subdirLine);
            }

            File.WriteAllLines(cmakePath, lines);
        }
    }
}
