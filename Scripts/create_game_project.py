import os
import sys

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
GAMES_DIR = os.path.join(ROOT_DIR, 'Games')

CMAKE_INSERT_AFTER = 'add_subdirectory(Lib)'

MAIN_CPP = '''struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#include <wtypes.h>
#include <shellapi.h>
#include <windows.h>

#include "Sailor.h"

using namespace Sailor;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
\tApp::Initialize(nullptr, 0);
\tApp::Start();
\tApp::Stop();
\tApp::Shutdown();
\treturn 0;
}
'''

def create_project(name: str):
    project_dir = os.path.join(GAMES_DIR, name)
    os.makedirs(project_dir, exist_ok=True)

    cmake_content = f"add_executable({name} WIN32 Main.cpp)\n" \
                     f"add_dependencies({name} SailorLib)\n" \
                     f"target_link_libraries({name} SailorLib)\n" \
                     f"target_compile_definitions({name} PUBLIC NOMINMAX)\n" \
                     f"set_property(TARGET {name} PROPERTY FOLDER \"Games\")\n" \
                     f"set_property(TARGET {name} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY \"\${{SAILOR_BINARIES_DIR}}\")\n"

    with open(os.path.join(project_dir, 'CMakeLists.txt'), 'w') as f:
        f.write(cmake_content)

    with open(os.path.join(project_dir, 'Main.cpp'), 'w') as f:
        f.write(MAIN_CPP)

    update_root_cmake(name)


def update_root_cmake(project_name: str):
    cmake_path = os.path.join(ROOT_DIR, 'CMakeLists.txt')
    subdir_line = f"add_subdirectory(Games/{project_name})\n"

    with open(cmake_path, 'r') as f:
        lines = f.readlines()

    if any(line.strip() == subdir_line.strip() for line in lines):
        return

    for idx, line in enumerate(lines):
        if line.strip().startswith(CMAKE_INSERT_AFTER):
            lines.insert(idx + 1, subdir_line)
            break
    else:
        lines.append(subdir_line)

    with open(cmake_path, 'w') as f:
        f.writelines(lines)


def main():
    if len(sys.argv) != 2:
        print('Usage: create_game_project.py <ProjectName>')
        return 1
    create_project(sys.argv[1])
    print(f'Created game project {sys.argv[1]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
