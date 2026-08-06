#!/usr/bin/env python3

import pathlib
import sys


def extract_function(source: str, signature: str) -> str:
    signature_offset = source.find(signature)
    if signature_offset < 0:
        raise AssertionError(f"Missing function: {signature}")

    body_offset = source.find("{", signature_offset)
    if body_offset < 0:
        raise AssertionError(f"Missing function body: {signature}")

    depth = 0
    for offset in range(body_offset, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[body_offset:offset + 1]

    raise AssertionError(f"Unterminated function body: {signature}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: shader_constants_mode_contract_test.py <Runtime directory>")

    runtime = pathlib.Path(sys.argv[1])
    context_header = (runtime / "Workspace" / "WorkspaceContext.h").read_text(
        encoding="utf-8"
    )
    compiler_source = (runtime / "AssetRegistry" / "Shader" / "ShaderCompiler.cpp").read_text(
        encoding="utf-8"
    )
    update = extract_function(
        compiler_source,
        "void ShaderCompiler::UpdateConstantsLibrary()",
    )

    if "bool IsEngineMode() const noexcept { return m_content == m_engineContent; }" not in context_header:
        raise AssertionError("Engine mode must require the active and Engine Content roots to match")

    guard = "if (!App::GetWorkspaceContext().IsEngineMode())"
    resolver = "ResolveWorkspaceContentPathForWrite("
    if guard not in update:
        raise AssertionError("workspace startup must skip generated Engine shader constants")
    if resolver not in update:
        raise AssertionError("Engine mode must resolve Constants.glsl inside its writable Content root")
    if update.index(guard) > update.index(resolver):
        raise AssertionError("Engine-mode guard must run before resolving a writable Content path")
    if "GetEngineContentFolder" in update or "GetEngineContent() /" in update:
        raise AssertionError("a distinct workspace must not write through to read-only Engine Content")
    if "const std::string generatedLibrary = GenerateConstantsLibrary(CacheProducerVersion);" not in update:
        raise AssertionError("Engine mode must generate the current constants source before validating it")
    if "currentLibrary != generatedLibrary" not in update:
        raise AssertionError("Constants.glsl must be repaired when its contents are stale even if its version matches")

    print("Shader constants mode contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
