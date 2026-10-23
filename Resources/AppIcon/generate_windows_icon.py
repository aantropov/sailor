#!/usr/bin/env python3
"""Regenerate the Windows icon from appicon.png with macOS sips."""

from pathlib import Path
import struct
import subprocess
import tempfile


def main() -> None:
    icon_directory = Path(__file__).resolve().parent
    source = icon_directory / "appicon.png"
    sizes = (16, 32, 48, 64, 128, 256)
    entries = []
    images = []
    with tempfile.TemporaryDirectory(prefix="sailor-appicon-") as temporary:
        for size in sizes:
            image = Path(temporary) / f"{size}.ico"
            subprocess.run(
                ["sips", "-s", "format", "ico", "-z", str(size), str(size), str(source), "--out", str(image)],
                check=True,
                stdout=subprocess.DEVNULL,
            )
            icon = image.read_bytes()
            if struct.unpack_from("<HHH", icon) != (0, 1, 1):
                raise ValueError(f"Expected a single-image ICO from sips for size {size}")
            entry = struct.unpack_from("<BBBBHHII", icon, 6)
            length, offset = entry[-2:]
            payload = icon[offset:offset + length]
            # sips reports 32 bits in the directory even for opaque RGB payloads.
            if payload.startswith(b"\x89PNG\r\n\x1a\n"):
                channels = {2: 3, 6: 4}[payload[25]]
                planes, depth = 1, payload[24] * channels
            else:
                planes, depth = struct.unpack_from("<HH", payload, 12)
            entries.append((*entry[:4], planes, depth))
            images.append(payload)

    directory = bytearray(struct.pack("<HHH", 0, 1, len(sizes)))
    offset = 6 + 16 * len(sizes)
    for entry, image in zip(entries, images):
        directory.extend(struct.pack("<BBBBHHII", *entry, len(image), offset))
        offset += len(image)

    (icon_directory / "appicon.ico").write_bytes(directory + b"".join(images))


if __name__ == "__main__":
    main()
