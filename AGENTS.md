This repository uses git submodules for dependencies (`External/renderdoc` and `External/vcpkg`).

The project is a game engine written in C++ and built with CMake. It targets Windows x64 and can be compiled with either the MSVC or clang toolchains.

Dependencies are managed through vcpkg. Whenever you update the repository (for example, pull new commits or fetch submodule changes) run `python update_deps.py` from the repository root to update and install the required packages.

The editor application is written in C# using .NET MAUI. Its source code lives in the `Editor` directory.
