import ctypes
import os
import platform
import sys


def main():
    if platform.system() != "Windows":
        print("Skipping scheduler tests: requires Windows build")
        return 0

    build_type = os.environ.get("CONFIG", "Release")
    binaries_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "Binaries"))
    lib_path = os.path.join(binaries_dir, f"Sailor-{build_type}.dll")

    if not os.path.exists(lib_path):
        print(f"Skipping scheduler tests: {lib_path} not found")
        return 0

    lib = ctypes.CDLL(lib_path)

    tests = [
        ("SchedulerTest_Initialization", lib.SchedulerTest_Initialization),
        ("SchedulerTest_BasicTaskExecution", lib.SchedulerTest_BasicTaskExecution),
        ("SchedulerTest_TaskChaining", lib.SchedulerTest_TaskChaining),
        ("SchedulerTest_TaskDependencies", lib.SchedulerTest_TaskDependencies),
        ("SchedulerTest_ThreadSpecificExecution", lib.SchedulerTest_ThreadSpecificExecution),
        ("SchedulerTest_WaitIdleRHI", lib.SchedulerTest_WaitIdleRHI),
        ("SchedulerTest_RunExplicitThread", lib.SchedulerTest_RunExplicitThread),
        ("SchedulerTest_ConcurrentInsertion", lib.SchedulerTest_ConcurrentInsertion),
        ("SchedulerTest_TaskSyncBlockReuse", lib.SchedulerTest_TaskSyncBlockReuse),
        ("SchedulerTest_MainThreadProcessing", lib.SchedulerTest_MainThreadProcessing),
    ]

    for name, func in tests:
        func.restype = ctypes.c_bool
        if not func():
            print(f"{name} failed")
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
