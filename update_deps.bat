@echo off
if not defined VCPKG_ROOT (
    echo VCPKG_ROOT is not set. Please set the VCPKG_ROOT environment variable to your vcpkg installation path.
    exit /b 1
)

pushd %VCPKG_ROOT%

if not exist vcpkg.exe (
    echo Bootstrap vcpkg...
    call bootstrap-vcpkg.bat
)

vcpkg update

vcpkg upgrade --no-dry-run

vcpkg install glm:x64-windows
vcpkg install imgui[win32-binding]:x64-windows
vcpkg install magic-enum:x64-windows
vcpkg install nlohmann-json:x64-windows
vcpkg install refl-cpp:x64-windows
vcpkg install spirv-reflect:x64-windows
vcpkg install stb:x64-windows
vcpkg install tinygltf:x64-windows
vcpkg install tracy:x64-windows
vcpkg install yaml-cpp:x64-windows

popd

echo Dependencies updated and installed.
pause
