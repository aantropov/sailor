#!/usr/bin/env python3

import re
import sys
from pathlib import Path


SOURCE_SUFFIXES = {".cc", ".cpp", ".cxx", ".h", ".hpp", ".inl"}
FORBIDDEN_PATTERN = re.compile(
    r"\b(?:try|catch|throw)\b|"
    r"^\s*#\s*include\s*<(?:exception|stdexcept)>|"
    r"\bstd\s*::\s*(?:"
    r"exception|bad_alloc|bad_cast|bad_typeid|bad_exception|"
    r"runtime_error|logic_error|domain_error|invalid_argument|length_error|out_of_range|"
    r"range_error|overflow_error|underflow_error|system_error|bad_optional_access|"
    r"bad_variant_access|bad_any_cast"
    r")\b",
    re.MULTILINE,
)


def strip_comments_and_literals(source: str) -> str:
    result = list(source)
    index = 0
    size = len(source)

    def blank(start: int, end: int) -> None:
        for position in range(start, end):
            if result[position] not in "\r\n":
                result[position] = " "

    while index < size:
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            end = size if end < 0 else end
            blank(index, end)
            index = end
            continue

        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            end = size if end < 0 else end + 2
            blank(index, end)
            index = end
            continue

        if source.startswith('R"', index):
            delimiter_end = source.find("(", index + 2)
            if delimiter_end >= 0:
                delimiter = source[index + 2:delimiter_end]
                end_marker = ")" + delimiter + '"'
                end = source.find(end_marker, delimiter_end + 1)
                end = size if end < 0 else end + len(end_marker)
                blank(index, end)
                index = end
                continue

        if source[index] in {'"', "'"}:
            quote = source[index]
            end = index + 1
            while end < size:
                if source[end] == "\\":
                    end += 2
                elif source[end] == quote:
                    end += 1
                    break
                else:
                    end += 1
            blank(index, min(end, size))
            index = end
            continue

        index += 1

    return "".join(result)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: runtime_no_exceptions_test.py <runtime-directory>")
        return 2

    runtime_directory = Path(sys.argv[1])
    failures = []
    for path in sorted(runtime_directory.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue

        source = strip_comments_and_literals(path.read_text(encoding="utf-8"))
        for match in FORBIDDEN_PATTERN.finditer(source):
            line = source.count("\n", 0, match.start()) + 1
            failures.append(f"{path}:{line}: forbidden exception syntax: {match.group(0).strip()}")

    if failures:
        print("\n".join(failures))
        return 1

    print("Runtime contains no Sailor-owned exception syntax.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
