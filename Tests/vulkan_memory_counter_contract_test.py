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


def require(text: str, fragment: str, message: str) -> None:
    if fragment not in text:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: vulkan_memory_counter_contract_test.py <Runtime directory>")

    source_path = pathlib.Path(sys.argv[1]) / "GraphicsDriver" / "Vulkan" / "VulkanMemory.cpp"
    source = source_path.read_text(encoding="utf-8")
    subtract = extract_function(source, "size_t SaturatingSubtract(")
    free = extract_function(source, "void GlobalVulkanMemoryAllocator::Free(")

    require(subtract, "compare_exchange_weak", "counter decrement must use an atomic CAS loop")
    require(subtract, "amount >= current ? 0u : current - amount",
            "counter decrement must saturate at zero")
    require(free, "SaturatingSubtract(m_totalDeviceMemoryAllocated, allocationSize)",
            "device-memory diagnostics must use the saturating decrement")
    require(free, "SaturatingSubtract(m_totalHostMemoryAllocated, size)",
            "host-memory diagnostics must use the saturating decrement")
    require(free, "pData.m_deviceMemory.Clear();",
            "diagnostics changes must not remove the real memory release")

    if "fetch_sub" in free or "m_totalHostMemoryAllocated -=" in free:
        raise AssertionError("Free must not use a racy or wrapping atomic decrement")

    print("Vulkan memory counter contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
