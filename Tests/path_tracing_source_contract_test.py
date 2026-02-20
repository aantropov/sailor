import os
import re
import sys


def read_file(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def assert_contains(text, needle, description, failures):
    if needle not in text:
        failures.append(f"Missing {description}: {needle}")


def assert_regex(text, pattern, description, failures):
    if re.search(pattern, text, flags=re.MULTILINE | re.DOTALL) is None:
        failures.append(f"Missing {description}: /{pattern}/")


def main():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

    editor_h = os.path.join(repo_root, "Runtime", "Submodules", "Editor.h")
    editor_cpp = os.path.join(repo_root, "Runtime", "Submodules", "Editor.cpp")
    dllmain_cpp = os.path.join(repo_root, "Lib", "DllMain.cpp")
    pathtracer_ecs_cpp = os.path.join(repo_root, "Runtime", "ECS", "PathTracerECS.cpp")
    engine_service_cs = os.path.join(repo_root, "Editor", "Services", "EngineService.cs")

    required_paths = [editor_h, editor_cpp, dllmain_cpp, pathtracer_ecs_cpp, engine_service_cs]
    missing_paths = [p for p in required_paths if not os.path.exists(p)]
    if missing_paths:
        print("Missing required source files:")
        for p in missing_paths:
            print(f"- {p}")
        return 1

    failures = []

    editor_h_text = read_file(editor_h)
    editor_cpp_text = read_file(editor_cpp)
    dllmain_text = read_file(dllmain_cpp)
    pathtracer_ecs_text = read_file(pathtracer_ecs_cpp)
    engine_service_text = read_file(engine_service_cs)

    assert_contains(
        editor_h_text,
        "bool RenderPathTracedImage(const class InstanceId& instanceId, const std::string& outputPath, uint32_t height, uint32_t samplesPerPixel, uint32_t maxBounces);",
        "Editor::RenderPathTracedImage declaration",
        failures,
    )

    assert_contains(
        editor_cpp_text,
        "bool Editor::RenderPathTracedImage(const InstanceId& instanceId, const std::string& outputPath, uint32_t height, uint32_t samplesPerPixel, uint32_t maxBounces)",
        "Editor::RenderPathTracedImage definition",
        failures,
    )
    assert_contains(editor_cpp_text, "Raytracing::PathTracer::Params params{}", "path tracer params construction", failures)
    assert_contains(editor_cpp_text, "Raytracing::PathTracer::ParseCommandLineArgs(params, nullptr, 0);", "path tracer defaults parsing", failures)
    assert_contains(editor_cpp_text, "return pPathTracerProxy ? pPathTracerProxy->RenderScene(params) : false;", "proxy-scoped export path", failures)
    assert_contains(editor_cpp_text, "return pPathTracerEcs->RenderScene(params);", "scene export path", failures)

    assert_contains(
        dllmain_text,
        "SAILOR_API bool RenderPathTracedImage(char* strOutputPath, char* strInstanceId, uint32_t height, uint32_t samplesPerPixel, uint32_t maxBounces)",
        "DLL RenderPathTracedImage export",
        failures,
    )
    assert_contains(dllmain_text, "editor->RenderPathTracedImage(instanceId, outputPath, height, samplesPerPixel, maxBounces);", "DLL -> Editor forward call", failures)
    assert_contains(dllmain_text, "Path tracer export succeeded:", "success message push", failures)
    assert_contains(dllmain_text, "Path tracer export failed:", "failure message push", failures)

    assert_regex(
        pathtracer_ecs_text,
        r"if\s*\(\s*data\.m_materials\.Num\(\)\s*==\s*0\s*\)\s*\{[^}]*inOutMaterials\.Add\(MaterialPtr\(\)\)",
        "fallback material injection for empty proxy materials",
        failures,
    )
    assert_regex(
        pathtracer_ecs_text,
        r"if\s*\(\s*params\.m_maxBounces\s*==\s*0\s*\)\s*\{[^}]*runtimeParams\.m_maxBounces",
        "per-proxy maxBounces override when unset",
        failures,
    )
    assert_regex(
        pathtracer_ecs_text,
        r"if\s*\(\s*params\.m_numSamples\s*==\s*0\s*&&\s*params\.m_msaa\s*==\s*0\s*\)\s*\{[^}]*options\.m_samplesPerPixel",
        "per-proxy samples override when unset",
        failures,
    )

    assert_regex(
        engine_service_text,
        r"RenderPathTracedImage\(string strOutputPath,\s*string strInstanceId,\s*uint height,\s*uint samplesPerPixel,\s*uint maxBounces\)",
        "C# P/Invoke RenderPathTracedImage signature",
        failures,
    )
    assert_contains(
        engine_service_text,
        "public bool ExportPathTracedImage(string outputPath, InstanceId targetInstance = null, uint height = 720, uint samplesPerPixel = 64, uint maxBounces = 4)",
        "C# export wrapper",
        failures,
    )

    if failures:
        print("Path tracing source contract test failed:")
        for failure in failures:
            print(f"- {failure}")
        return 1

    print("Path tracing source contract checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
