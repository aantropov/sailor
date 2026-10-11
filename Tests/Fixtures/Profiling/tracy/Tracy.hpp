#pragma once

// Record the profiling boundary without starting a Tracy client in this test.
#define ZoneText(data, size) CaptureProfileText(data, size)
