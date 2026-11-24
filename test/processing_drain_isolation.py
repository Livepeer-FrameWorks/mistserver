#!/usr/bin/env python3
"""A stuck recorder must release only its own pipe reader."""
import subprocess
import sys

unrelated = subprocess.Popen(["sleep", "400"])
try:
    result = subprocess.run(sys.argv[1:], timeout=420)
    assert unrelated.poll() is None, "drain cleanup killed an unrelated sleep process"
    if result.returncode == 77:
        sys.exit(77)
    assert result.returncode == 0, ("drain scenario failed", result.returncode)
    print("PASS stuck recorder cleanup preserved the unrelated sleep process", flush=True)
finally:
    if unrelated.poll() is None:
        unrelated.terminate()
        try:
            unrelated.wait(timeout=3)
        except subprocess.TimeoutExpired:
            unrelated.kill()
            unrelated.wait()
