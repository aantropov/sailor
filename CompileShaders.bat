
for /r %%i in (%CD%/Content/Shaders/*.frag, %CD%/Content/Shaders/*.vert) do %VULKAN_SDK%/Bin32/glslc.exe %%i -o %%~ni.spv
pause