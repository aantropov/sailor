import subprocess
import sys
import time
import os


def main():
    # command to run a simple process that stays alive
    cmd = [sys.executable, "-c", "import time; time.sleep(20)"]

    proc = subprocess.Popen(cmd)

    try:
        time.sleep(10)
        if proc.poll() is not None:
            print("Process exited before 10 seconds")
            return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    return 0


if __name__ == "__main__":
    sys.exit(main())

