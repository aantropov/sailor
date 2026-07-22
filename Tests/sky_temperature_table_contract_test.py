#!/usr/bin/env python3

import pathlib
import re
import sys


def require(text: str, fragment: str, message: str) -> None:
    if fragment not in text:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: sky_temperature_table_contract_test.py <Runtime directory> <Content directory>"
        )

    runtime = pathlib.Path(sys.argv[1])
    content = pathlib.Path(sys.argv[2])
    header = (runtime / "FrameGraph" / "SkyNode.h").read_text(encoding="utf-8")
    source = (runtime / "FrameGraph" / "SkyNode.cpp").read_text(encoding="utf-8")
    table = (content / "StarsColor.yaml").read_text(encoding="utf-8")

    require(header, "s_minRgbTemperature = 1000", "sky color range must start at 1000 K")
    require(header, "s_maxRgbTemperature = 40000", "sky color range must include 40000 K")
    require(header, "s_rgbTemperatureStep = 100", "sky color range must use 100 K steps")
    require(
        header,
        "((s_maxRgbTemperature - s_minRgbTemperature) / s_rgbTemperatureStep) + 1u",
        "inclusive sky color range must reserve a slot for both endpoints",
    )
    require(
        header,
        "s_rgbTemperatures[s_numRgbTemperatures]",
        "sky color table storage must use the inclusive entry count",
    )
    require(
        source,
        "(temperatureK - s_minRgbTemperature) / s_rgbTemperatureStep",
        "sky color loading must map the minimum temperature to index zero",
    )
    require(
        source,
        "(clampedTemperature - s_minRgbTemperature) / s_rgbTemperatureStep",
        "sky color lookup must map the clamped temperature into the table",
    )

    temperatures = [
        int(match.group(1))
        for match in re.finditer(r"^- \[(\d+),", table, flags=re.MULTILINE)
    ]
    if not temperatures:
        raise AssertionError("StarsColor.yaml contains no color temperatures")

    minimum = 1000
    maximum = 40000
    step = 100
    count = ((maximum - minimum) // step) + 1
    if min(temperatures) != minimum or max(temperatures) != maximum:
        raise AssertionError("StarsColor.yaml must cover the inclusive 1000..40000 K range")
    if any((temperature - minimum) % step != 0 for temperature in temperatures):
        raise AssertionError("StarsColor.yaml contains a temperature outside the 100 K grid")
    if any((temperature - minimum) // step >= count for temperature in temperatures):
        raise AssertionError("StarsColor.yaml maps beyond the allocated sky color table")

    print("Sky temperature table contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
