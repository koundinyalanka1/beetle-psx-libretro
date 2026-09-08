#!/usr/bin/env python3
"""Run synthetic GPU/DMA tests through glhost and compare threading modes.

Requires a hardware core and a working local OpenGL context; generates its own
test-only BIOS. This exercises the real GPU, DMA, libretro and state paths.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core", type=Path)
    parser.add_argument("--host", type=Path, default=Path("tools/glhost/glhost"))
    parser.add_argument("--output", type=Path, default=Path("/tmp/beetle-gpu-check"))
    parser.add_argument("--require-worker-queue", action="store_true",
                        help="require nonzero queue depth (for the small-queue stress build)")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    generator = Path(__file__).with_name("make_gpu_test.py")
    for mode in ("pio", "dma"):
        subprocess.run([sys.executable, str(generator), str(out / (mode + ".exe")),
                        "--bios-dir", str(out / "bios")] +
                       (["--dma"] if mode == "dma" else []), check=True)
    (out / "save").mkdir(exist_ok=True)
    logs = {}

    def run(name, mode, threaded, diagnostics="disabled", resolution="1x(native)",
            mirror="disabled", **extra):
        env = os.environ.copy()
        # Keep caller diagnostics/runahead from silently changing a comparison.
        for key in list(env):
            if key.startswith("GLHOST_") or key == "BEETLE_PSX_GPU_TIME":
                del env[key]
        env.update(GLHOST_SYSTEM_DIR=str(out / "bios"), GLHOST_SAVE_DIR=str(out / "save"),
                   GLHOST_TEST_REPORT="1", GLHOST_STATEHASH="1", GLHOST_FRAMEHASH="1",
                   GLHOST_VARS=";".join("beetle_psx_hw_" + key + "=" + value for key, value in {
                       "renderer_software_fb": mirror, "internal_resolution": resolution,
                       "cpu_dynarec": "disabled", "threaded_gpu": threaded,
                       "gpu_diagnostics": diagnostics}.items()))
        env.update(extra)
        with (out / (name + ".log")).open("w") as log:
            subprocess.run([str(args.host.resolve()), str(args.core.resolve()),
                            str(out / (mode + ".exe")), "-", "320", str(out / name)],
                           env=env, stdout=log, stderr=subprocess.STDOUT,
                           timeout=120, check=True)
        text = (out / (name + ".log")).read_text()
        assert "[testrom] PASS: 22 cases, 1734 words checked, 0 mismatches" in text, name
        assert "Threading: GPU worker " + ("on" if threaded == "enabled" else "off") in text, name
        assert "FAILED" not in text and "SERDIFF" not in text, name
        if args.require_worker_queue and threaded == "enabled":
            depths = re.findall(r"depth \d+ now / [\d.]+ avg / (\d+) max", text)
            assert depths and max(map(int, depths)) > 0, name + ": worker never received commands"
        if diagnostics in ("fifo", "all"):
            counts = re.findall(r"\((\d+) checked, (\d+) mismatched\)", text)
            assert counts and sum(int(a) for a, _ in counts) > 0, name + ": no FIFO checks"
            assert all(int(b) == 0 for _, b in counts), name + ": stale FIFO answer"
        if diagnostics in ("timing", "all"):
            assert "per-word timing off" not in text, name + ": timing option ignored"
        logs[name] = text.splitlines()
        print(name + ": PASS", flush=True)

    def compare(a, b, frames=True):
        for prefix in (["[glhost] state frame", "[glhost] frame "] if frames else
                       ["[glhost] state frame"]):
            left, right = [[s for s in logs[n] if s.startswith(prefix)
                            and (prefix != "[glhost] frame " or " hash " in s)] for n in (a, b)]
            assert len(left) == 320 and left == right, (a, b, prefix)
        print(a + " / " + b + ": identical frame-by-frame hashes", flush=True)

    for mode in ("pio", "dma"):
        run(mode + "-off", mode, "disabled")
        run(mode + "-on", mode, "enabled")
        compare(mode + "-off", mode + "-on")
    run("dma-validate", "dma", "enabled", "fifo")
    compare("dma-on", "dma-validate")
    run("dma-timing", "dma", "enabled", "all")
    compare("dma-on", "dma-timing")
    run("dma-timing-inline", "dma", "disabled", "timing")
    compare("dma-off", "dma-timing-inline")
    run("dma-restore-off", "dma", "disabled", GLHOST_PREEMPT="1", GLHOST_SERDIFF="1")
    run("dma-restore", "dma", "enabled", "fifo", GLHOST_PREEMPT="1", GLHOST_SERDIFF="1")
    compare("dma-on", "dma-restore", frames=False)
    compare("dma-restore-off", "dma-restore")
    run("dma-recreate", "dma", "enabled", "fifo", GLHOST_RECREATE_AT="3")
    compare("dma-on", "dma-recreate")
    run("dma-2x", "dma", "enabled", "fifo", resolution="2x")
    compare("dma-on", "dma-2x", frames=False)
    run("dma-mirror", "dma", "enabled", "fifo", mirror="enabled")
    compare("dma-on", "dma-mirror", frames=False)
    print("GPU/DMA checks passed; logs: " + str(out))


if __name__ == "__main__":
    main()
