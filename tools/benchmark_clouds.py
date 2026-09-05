#!/usr/bin/env python3
"""Repeatable native cloud renders, with optional display-space reference metrics."""
import argparse
import csv
import hashlib
import re
import subprocess
import tempfile
import time
from pathlib import Path

from bmp_metrics import compare

ROOT = Path(__file__).resolve().parents[1]
SCENES = {
    "daylight": "procedural-cumulus-daylight.luz",
    "backlit": "disney-cloud-backlit.luz",
    "interior": "disney-cloud-interior.luz",
    "cirrus": "procedural-cirrus-twilight.luz",
    "stratus": "procedural-stratus-overcast.luz",
}


def positive_int(value):
    result = int(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return result


def prepared_scene(text, cache_resolution, reference):
    # Append explicit controls at the end of cloud blocks so quality/property
    # ordering cannot change the benchmark's authored density or march detail.
    lines = []
    in_cloud = False
    for line in text.splitlines():
        if line.strip() == "[settings]":
            lines.extend([line, f"volume_reference={int(reference)}"])
            continue
        if re.match(r"\s*cloud\s+.*\{", line):
            in_cloud = True
        if in_cloud and line.strip() == "}":
            lines.extend([f"directional_cache_resolution={cache_resolution}", "primary_detail=1"])
            in_cloud = False
        lines.append(line)
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "luz")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", nargs="+", choices=SCENES, default=list(SCENES))
    parser.add_argument("--resolution", default="128x72")
    parser.add_argument("--samples", type=positive_int, default=8)
    parser.add_argument("--threads", type=positive_int, default=1)
    parser.add_argument("--repeat", type=positive_int, default=3)
    parser.add_argument("--seeds", type=int, nargs="+", default=[987654, 987655])
    parser.add_argument("--cache-resolution", type=float, default=0)
    parser.add_argument("--reference", action="store_true", help="disable volume lighting approximations")
    parser.add_argument("--reference-directory", type=Path, help="directory of reference BMPs from this tool")
    args = parser.parse_args()
    if not 0 <= args.cache_resolution <= 16:
        parser.error("cache resolution must be between 0 and 16")
    if not re.fullmatch(r"[1-9][0-9]*x[1-9][0-9]*", args.resolution):
        parser.error("resolution must be WIDTHxHEIGHT")
    if any(seed < 0 or seed > 0xFFFFFFFF for seed in args.seeds):
        parser.error("seeds must be unsigned 32-bit integers")
    args.output.mkdir(parents=True, exist_ok=True)
    fields = ["case", "seed", "repeat", "resolution", "samples", "threads", "cache_resolution",
              "reference", "elapsed_seconds", "sha256", "display_rmse", "display_psnr"]
    with (args.output / "results.csv").open("w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fields)
        writer.writeheader()
        for case in args.cases:
            scene = ROOT / "examples/scenes" / SCENES[case]
            # Keep relative asset paths valid and remove the temporary scene even
            # when the renderer fails. Missing authored assets fail explicitly.
            with tempfile.NamedTemporaryFile(mode="w", suffix=".luz", dir=scene.parent) as prepared:
                prepared.write(prepared_scene(scene.read_text(), args.cache_resolution, args.reference))
                prepared.flush()
                for seed in args.seeds:
                    for repeat in range(args.repeat):
                        stem = f"{case}-{seed}-{repeat}"
                        image = (args.output / f"{stem}.bmp").resolve()
                        command = [str(args.binary.resolve()), prepared.name, "--resolution", args.resolution,
                                   "--samples", str(args.samples), "--threads", str(args.threads), "--seed", str(seed),
                                   "--no-adaptive", "--no-denoise", "--output", str(image)]
                        started = time.perf_counter()
                        with (args.output / f"{stem}.log").open("w") as log:
                            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
                        elapsed = time.perf_counter() - started
                        row = dict(case=case, seed=seed, repeat=repeat, resolution=args.resolution,
                                   samples=args.samples, threads=args.threads, cache_resolution=args.cache_resolution,
                                   reference=args.reference, elapsed_seconds=elapsed,
                                   sha256=hashlib.sha256(image.read_bytes()).hexdigest())
                        if args.reference_directory:
                            metrics = compare(args.reference_directory / f"{stem}.bmp", image)
                            row.update(display_rmse=metrics["rmse"] / 255, display_psnr=metrics["psnr"])
                        writer.writerow(row)
                        csv_file.flush()
                        print(f"{stem}: {elapsed:.3f} s", flush=True)


if __name__ == "__main__":
    main()
