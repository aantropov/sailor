#!/usr/bin/env python3

import re
import sys
from pathlib import Path


CONTAINER_PATTERN = re.compile(
    r"\bstd\s*::\s*(vector|deque|list|forward_list|map|multimap|unordered_map|set|multiset|unordered_set)\s*<"
)
SMART_POINTER_PATTERN = re.compile(
    r"\bstd\s*::\s*(shared_ptr|weak_ptr|unique_ptr|make_shared|make_unique|enable_shared_from_this)\b"
)
SOURCE_SUFFIXES = {".h", ".hpp", ".inl", ".cpp", ".cc", ".cxx", ".mm"}
ALLOWED_CONTAINERS = {
    Path("Editor/EditorRuntimeBridge.cpp"): {"vector"},
    Path("Submodules/EditorRemote/RemoteViewportFoundation.h"): {"vector"},
    Path("Submodules/EditorRemote/RemoteViewportHarness.h"): {"deque"},
    Path("Submodules/EditorRemote/RemoteViewportMacTransport.h"): {"vector"},
}


def mask_span(characters: list[str], source: str, start: int, end: int) -> None:
    for index in range(start, end):
        if source[index] != "\n":
            characters[index] = " "


def mask_comments_and_literals(source: str) -> str:
    masked = list(source)
    index = 0

    while index < len(source):
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            end = len(source) if end == -1 else end
            mask_span(masked, source, index, end)
            index = end
            continue

        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            end = len(source) if end == -1 else end + 2
            mask_span(masked, source, index, end)
            index = end
            continue

        if index == 0 or not (source[index - 1].isalnum() or source[index - 1] == "_"):
            raw_match = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(', source[index:])
            if raw_match:
                terminator = ")" + raw_match.group(1) + '"'
                end = source.find(terminator, index + raw_match.end())
                end = len(source) if end == -1 else end + len(terminator)
                mask_span(masked, source, index, end)
                index = end
                continue

        if source[index] in {'"', "'"}:
            quote = source[index]
            end = index + 1
            while end < len(source):
                if source[end] == "\\":
                    end += 2
                    continue
                end += 1
                if source[end - 1] == quote:
                    break
            mask_span(masked, source, index, min(end, len(source)))
            index = end
            continue

        index += 1

    return "".join(masked)


def is_allowed(relative_path: Path, container: str) -> bool:
    if relative_path == Path("Memory/Memory.cpp"):
        return True
    if relative_path.parent == Path("Containers") and relative_path.name.endswith("Benchmark.cpp"):
        return True
    return container in ALLOWED_CONTAINERS.get(relative_path, set())


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {Path(sys.argv[0]).name} <Runtime directory>", file=sys.stderr)
        return 2

    runtime_dir = Path(sys.argv[1]).resolve()
    violations: list[str] = []

    for source_path in sorted(runtime_dir.rglob("*")):
        if not source_path.is_file() or source_path.suffix not in SOURCE_SUFFIXES:
            continue

        relative_path = source_path.relative_to(runtime_dir)
        source = source_path.read_text(encoding="utf-8", errors="replace")
        masked = mask_comments_and_literals(source)
        for match in CONTAINER_PATTERN.finditer(masked):
            container = match.group(1)
            if not is_allowed(relative_path, container):
                line = source.count("\n", 0, match.start()) + 1
                violations.append(f"{relative_path}:{line}: std::{container} is not allowed in Runtime")

        for match in SMART_POINTER_PATTERN.finditer(masked):
            smart_pointer = match.group(1)
            line = source.count("\n", 0, match.start()) + 1
            violations.append(f"{relative_path}:{line}: std::{smart_pointer} is not allowed in Runtime")

    if violations:
        print("Runtime ownership must use Sailor containers and smart pointers:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1

    print("Runtime std container and smart pointer policy passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
