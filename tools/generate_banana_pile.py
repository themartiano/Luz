#!/usr/bin/env python3
"""Generate deterministic banana pile or ocean mesh instances in a .luz scene."""

from __future__ import annotations

import argparse
import math
import os
import random
import re
import tempfile
from pathlib import Path
from typing import Iterable


BEGIN_MARKER = "# BEGIN_GENERATED_BANANA_PILE"
END_MARKER = "# END_GENERATED_BANANA_PILE"
DEFAULT_SCENE = Path("examples/scenes/cornell.luz")
DEFAULT_COUNT = 10000
DEFAULT_REFERENCE_COUNT = 10000
DEFAULT_SIZE_RATIO = 1.0
DEFAULT_HEIGHT_RATIO = 1.0
DEFAULT_HEIGHT_TO_RADIUS_RATIO = 0.32
DEFAULT_IRREGULARITY = 0.35
DEFAULT_LAYOUT = "ocean"
DEFAULT_OCEAN_BANANA_SCALE = 160.0
BASE_RADIUS_X = 218.0
BASE_RADIUS_Z = 188.0
BASE_MAX_HEIGHT = 175.0
BASE_AVERAGE_RADIUS = (BASE_RADIUS_X + BASE_RADIUS_Z) * 0.5
OCEAN_CELL_FOOTPRINT_RATIO = 0.58
OCEAN_DEPTH_RATIO = 1.85
OCEAN_CENTER_Z = 1800.0
OCEAN_WAVE_HEIGHT_RATIO = 1.72
DEFAULT_SEED = 20260622
DEFAULT_MESH_BOUNDS = (
    (0.220682, -0.038038, -0.092663),
    (0.354533, 0.140727, 0.13021),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Replace the single banana in a scene with a generated banana pile or ocean."
    )
    parser.add_argument("--scene", type=Path, default=DEFAULT_SCENE)
    parser.add_argument("--count", type=int, default=DEFAULT_COUNT)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--ground-y", type=float, default=0.0)
    parser.add_argument(
        "--layout",
        choices=("pile", "ocean"),
        default=DEFAULT_LAYOUT,
        help="Generated banana layout.",
    )
    parser.add_argument(
        "--reference-count",
        type=int,
        default=DEFAULT_REFERENCE_COUNT,
        help="Banana count used as the density reference for automatic pile volume.",
    )
    parser.add_argument(
        "--pile-scale",
        type=float,
        default=None,
        help="Explicit footprint multiplier. Height still follows the natural aspect ratio.",
    )
    parser.add_argument(
        "--height-to-radius-ratio",
        type=float,
        default=DEFAULT_HEIGHT_TO_RADIUS_RATIO,
        help="Pile height divided by average footprint radius for the count-derived shape.",
    )
    parser.add_argument(
        "--size-ratio",
        type=float,
        default=DEFAULT_SIZE_RATIO,
        help="Optional horizontal multiplier after count-derived natural scaling.",
    )
    parser.add_argument(
        "--height-ratio",
        type=float,
        default=DEFAULT_HEIGHT_RATIO,
        help="Optional vertical multiplier after count-derived natural scaling.",
    )
    parser.add_argument(
        "--banana-scale",
        type=float,
        default=DEFAULT_OCEAN_BANANA_SCALE,
        help="Average banana object scale for --layout ocean; the desert footprint grows with it.",
    )
    parser.add_argument(
        "--irregularity",
        type=float,
        default=DEFAULT_IRREGULARITY,
        help="Deterministic footprint and height roughness in [0, 1].",
    )
    parser.add_argument(
        "--fit-camera",
        action="store_true",
        help="Move camera main back/up to frame the generated pile.",
    )
    return parser.parse_args()


def resolve_mesh_path(scene_path: Path, scene_text: str) -> Path | None:
    mesh_match = re.search(r"(?ms)^mesh\s+banana\s*\{\s*(.*?)^\}", scene_text)
    if not mesh_match:
        return None

    for raw_line in mesh_match.group(1).splitlines():
        line = raw_line.strip()
        if line.startswith("file="):
            mesh_path = Path(line.split("=", 1)[1].strip())
            if mesh_path.is_absolute():
                return mesh_path
            return (scene_path.parent / mesh_path).resolve()
    return None


def resolve_mesh_path_from_file(scene_path: Path) -> Path | None:
    in_banana_mesh = False

    with scene_path.open("r", encoding="utf-8", errors="replace") as scene_file:
        for raw_line in scene_file:
            line = raw_line.strip()
            if not in_banana_mesh:
                if re.match(r"^mesh\s+banana\s*\{\s*$", line):
                    in_banana_mesh = True
                continue

            if line == "}":
                return None
            if line.startswith("file="):
                mesh_path = Path(line.split("=", 1)[1].strip())
                if mesh_path.is_absolute():
                    return mesh_path
                return (scene_path.parent / mesh_path).resolve()
    return None


def read_obj_bounds(mesh_path: Path | None) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    if mesh_path is None or not mesh_path.exists():
        return DEFAULT_MESH_BOUNDS

    min_x = min_y = min_z = math.inf
    max_x = max_y = max_z = -math.inf
    found_vertex = False

    with mesh_path.open("r", encoding="utf-8", errors="replace") as obj_file:
        for line in obj_file:
            if not line.startswith("v "):
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            try:
                x, y, z = float(parts[1]), float(parts[2]), float(parts[3])
            except ValueError:
                continue

            min_x = min(min_x, x)
            min_y = min(min_y, y)
            min_z = min(min_z, z)
            max_x = max(max_x, x)
            max_y = max(max_y, y)
            max_z = max(max_z, z)
            found_vertex = True

    if not found_vertex:
        return DEFAULT_MESH_BOUNDS
    return (min_x, min_y, min_z), (max_x, max_y, max_z)


def rotate_point(point: tuple[float, float, float], rotation_degrees: tuple[float, float, float]) -> tuple[float, float, float]:
    x, y, z = point
    rx, ry, rz = (math.radians(value) for value in rotation_degrees)

    cos_x, sin_x = math.cos(rx), math.sin(rx)
    next_y = (y * cos_x) - (z * sin_x)
    next_z = (y * sin_x) + (z * cos_x)
    y, z = next_y, next_z

    cos_y, sin_y = math.cos(ry), math.sin(ry)
    next_x = (x * cos_y) + (z * sin_y)
    next_z = (-x * sin_y) + (z * cos_y)
    x, z = next_x, next_z

    cos_z, sin_z = math.cos(rz), math.sin(rz)
    next_x = (x * cos_z) - (y * sin_z)
    next_y = (x * sin_z) + (y * cos_z)

    return next_x, next_y, z


def scaled_bounds_corners(
    bounds: tuple[tuple[float, float, float], tuple[float, float, float]],
    scale: float,
) -> list[tuple[float, float, float]]:
    minimum, maximum = bounds
    min_x, min_y, min_z = minimum
    max_x, max_y, max_z = maximum
    corners = []
    for x in (min_x, max_x):
        for y in (min_y, max_y):
            for z in (min_z, max_z):
                corners.append((x * scale, y * scale, z * scale))
    return corners


def bottom_offset(
    bounds: tuple[tuple[float, float, float], tuple[float, float, float]],
    scale: float,
    rotation: tuple[float, float, float],
) -> float:
    min_y = min(rotate_point(corner, rotation)[1] for corner in scaled_bounds_corners(bounds, scale))
    return -min_y


def format_float(value: float) -> str:
    return f"{value:.3f}".rstrip("0").rstrip(".")


def height_scale_per_footprint_scale(height_to_radius_ratio: float) -> float:
    height_to_radius_ratio = validate_positive_finite(
        height_to_radius_ratio,
        "--height-to-radius-ratio",
    )
    return height_to_radius_ratio * BASE_AVERAGE_RADIUS / BASE_MAX_HEIGHT


def pile_scale_for_count(
    count: int,
    reference_count: int,
    pile_scale_override: float | None,
    height_to_radius_ratio: float,
) -> float:
    if count <= 0:
        raise ValueError("--count must be positive")
    if reference_count <= 0:
        raise ValueError("--reference-count must be positive")

    natural_height_scale = height_scale_per_footprint_scale(height_to_radius_ratio)
    if pile_scale_override is None:
        pile_scale = ((count / reference_count) / natural_height_scale) ** (1.0 / 3.0)
    else:
        pile_scale = pile_scale_override
    if not math.isfinite(pile_scale) or pile_scale <= 0.0:
        raise ValueError("--pile-scale must be finite and positive")
    return pile_scale


def validate_positive_finite(value: float, name: str) -> float:
    if not math.isfinite(value) or value <= 0.0:
        raise ValueError(f"{name} must be finite and positive")
    return value


def validate_unit_interval(value: float, name: str) -> float:
    if not math.isfinite(value) or value < 0.0 or value > 1.0:
        raise ValueError(f"{name} must be finite and in [0, 1]")
    return value


def clamp(value: float, minimum: float, maximum: float) -> float:
    return max(minimum, min(maximum, value))


def pile_shape_scales(
    count: int,
    reference_count: int,
    pile_scale_override: float | None,
    size_ratio: float,
    height_ratio: float,
    height_to_radius_ratio: float = DEFAULT_HEIGHT_TO_RADIUS_RATIO,
) -> tuple[float, float, float]:
    pile_scale = pile_scale_for_count(
        count,
        reference_count,
        pile_scale_override,
        height_to_radius_ratio,
    )
    size_ratio = validate_positive_finite(size_ratio, "--size-ratio")
    height_ratio = validate_positive_finite(height_ratio, "--height-ratio")
    natural_height_scale = pile_scale * height_scale_per_footprint_scale(height_to_radius_ratio)
    return pile_scale, pile_scale * size_ratio, natural_height_scale * height_ratio


def shape_wave(theta: float, phases: tuple[float, ...]) -> float:
    return (
        0.50 * math.sin((2.0 * theta) + phases[0])
        + 0.32 * math.sin((5.0 * theta) + phases[1])
        + 0.18 * math.sin((9.0 * theta) + phases[2])
    )


def ground_wave(x: float, z: float, phases: tuple[float, ...]) -> float:
    return (
        0.42 * math.sin((0.0067 * x) + phases[3])
        + 0.34 * math.sin((0.0089 * z) + phases[4])
        + 0.24 * math.sin((0.0049 * (x + z)) + phases[5])
    )


def pile_envelope(
    x: float,
    z: float,
    mounds: tuple[tuple[float, float, float, float, float, float], ...],
) -> tuple[float, float]:
    best_height = 0.0
    best_radial = 1.35

    for center_x, center_z, radius_x, radius_z, height, exponent in mounds:
        dx = (x - center_x) / radius_x
        dz = (z - center_z) / radius_z
        radial = math.sqrt((dx * dx) + (dz * dz))
        if radial >= 1.0:
            continue

        crown_radius = 0.22
        if radial < crown_radius:
            profile = 1.0 - (0.018 * ((radial / crown_radius) ** 2.0))
        else:
            shoulder_radial = (radial - crown_radius) / (1.0 - crown_radius)
            profile = max(0.0, 1.0 - (shoulder_radial ** exponent))
        mound_height = height * (profile ** 1.06)
        if mound_height > best_height:
            best_height = mound_height
            best_radial = radial

    return best_height, best_radial


def ocean_cell_size(
    bounds: tuple[tuple[float, float, float], tuple[float, float, float]],
    banana_scale: float,
) -> float:
    banana_scale = validate_positive_finite(banana_scale, "--banana-scale")
    minimum, maximum = bounds
    horizontal_extent = max(maximum[0] - minimum[0], maximum[2] - minimum[2])
    if not math.isfinite(horizontal_extent) or horizontal_extent <= 0.0:
        fallback_minimum, fallback_maximum = DEFAULT_MESH_BOUNDS
        horizontal_extent = max(
            fallback_maximum[0] - fallback_minimum[0],
            fallback_maximum[2] - fallback_minimum[2],
        )
    return horizontal_extent * banana_scale * OCEAN_CELL_FOOTPRINT_RATIO


def ocean_shape(
    count: int,
    reference_count: int,
    pile_scale_override: float | None,
    size_ratio: float,
    height_ratio: float,
    bounds: tuple[tuple[float, float, float], tuple[float, float, float]],
    banana_scale: float,
) -> tuple[float, float, float, float, float]:
    if count <= 0:
        raise ValueError("--count must be positive")
    if reference_count <= 0:
        raise ValueError("--reference-count must be positive")

    if pile_scale_override is None:
        area_scale = math.sqrt(count / reference_count)
    else:
        area_scale = pile_scale_override
    if not math.isfinite(area_scale) or area_scale <= 0.0:
        raise ValueError("--pile-scale must be finite and positive")

    size_ratio = validate_positive_finite(size_ratio, "--size-ratio")
    height_ratio = validate_positive_finite(height_ratio, "--height-ratio")
    cell_size = ocean_cell_size(bounds, banana_scale)
    reference_width = math.sqrt((reference_count * cell_size * cell_size) / OCEAN_DEPTH_RATIO)
    width = reference_width * area_scale * size_ratio
    depth = width * OCEAN_DEPTH_RATIO
    wave_height = banana_scale * OCEAN_WAVE_HEIGHT_RATIO * height_ratio
    return area_scale, width, depth, wave_height, cell_size


def ocean_wave_height(
    x: float,
    z: float,
    width: float,
    depth: float,
    wave_height: float,
    irregularity: float,
    phases: tuple[float, ...],
) -> float:
    main_wave_length = max(520.0, depth * 0.16)
    cross_wave_length = max(620.0, width * 0.42)
    ripple_wave_length = max(95.0, width * 0.055)
    cross_drift = 0.36 * math.sin(((x / cross_wave_length) * math.tau) + phases[0])
    primary = 0.5 + (0.5 * math.sin(((z / main_wave_length) * math.tau) + cross_drift + phases[1]))
    secondary = 0.5 + (0.5 * math.sin((((z + (0.32 * x)) / (main_wave_length * 0.56)) * math.tau) + phases[2]))
    ripples = (
        0.55 * math.sin((((z + (0.18 * x)) / ripple_wave_length) * math.tau) + phases[3])
        + 0.45 * math.sin((((x - (0.12 * z)) / (ripple_wave_length * 1.35)) * math.tau) + phases[4])
    )
    dune = primary ** 2.15
    shoulder = secondary ** 1.65
    height = wave_height * (0.14 + (0.72 * dune) + (0.18 * shoulder))
    height += wave_height * irregularity * 0.09 * ripples
    return max(0.0, height)


def ocean_wave_slope(
    x: float,
    z: float,
    width: float,
    depth: float,
    wave_height: float,
    irregularity: float,
    phases: tuple[float, ...],
) -> tuple[float, float]:
    delta = 12.0
    left = ocean_wave_height(x - delta, z, width, depth, wave_height, irregularity, phases)
    right = ocean_wave_height(x + delta, z, width, depth, wave_height, irregularity, phases)
    back = ocean_wave_height(x, z - delta, width, depth, wave_height, irregularity, phases)
    front = ocean_wave_height(x, z + delta, width, depth, wave_height, irregularity, phases)
    return (right - left) / (2.0 * delta), (front - back) / (2.0 * delta)


def iter_ocean_lines(
    count: int,
    seed: int,
    bounds: tuple[tuple[float, float, float], tuple[float, float, float]],
    ground_y: float = 0.0,
    reference_count: int = DEFAULT_REFERENCE_COUNT,
    pile_scale_override: float | None = None,
    size_ratio: float = DEFAULT_SIZE_RATIO,
    height_ratio: float = DEFAULT_HEIGHT_RATIO,
    irregularity: float = DEFAULT_IRREGULARITY,
    banana_scale: float = DEFAULT_OCEAN_BANANA_SCALE,
) -> Iterable[str]:
    area_scale, width, depth, wave_height, cell_size = ocean_shape(
        count,
        reference_count,
        pile_scale_override,
        size_ratio,
        height_ratio,
        bounds,
        banana_scale,
    )
    irregularity = validate_unit_interval(irregularity, "--irregularity")

    rng = random.Random(seed)
    shape_rng = random.Random(seed ^ 0x0CEAA11)
    phases = tuple(shape_rng.random() * math.tau for _ in range(5))
    min_z = OCEAN_CENTER_Z - (depth * 0.5)
    columns = max(1, math.ceil(math.sqrt(count * (width / depth))))
    rows = math.ceil(count / columns)
    cell_x = width / columns
    cell_z = depth / rows
    mesh_height = (bounds[1][1] - bounds[0][1]) * banana_scale

    yield (
        f"{BEGIN_MARKER} layout=ocean count={count} seed={seed}"
        f" area_scale={format_float(area_scale)}"
        f" banana_scale={format_float(banana_scale)}"
        f" cell_size={format_float(cell_size)}"
        f" width={format_float(width)}"
        f" depth={format_float(depth)}"
        f" wave_height={format_float(wave_height)}"
        f" thickness=2"
        f" irregularity={format_float(irregularity)}"
    )
    yield "# Generated by tools/generate_banana_pile.py; rerun with --layout ocean to reshape this banana desert."

    for index in range(count):
        row = index // columns
        column = index % columns
        x = (-width * 0.5) + ((column + 0.5 + rng.uniform(-0.48, 0.48)) * cell_x)
        z = min_z + ((row + 0.5 + rng.uniform(-0.48, 0.48)) * cell_z)
        x += math.sin((row * 0.29) + phases[0]) * cell_x * 0.34
        z += math.sin((column * 0.17) + phases[1]) * cell_z * 0.20

        surface_y = ocean_wave_height(x, z, width, depth, wave_height, irregularity, phases)
        layer = 1 if rng.random() < 0.34 else 0
        layer_offset = layer * mesh_height * rng.uniform(0.42, 0.95)
        scale = banana_scale * rng.uniform(0.85, 1.15)
        slope_x, slope_z = ocean_wave_slope(x, z, width, depth, wave_height, irregularity, phases)
        yaw = 90.0 + (10.0 * math.sin((z / max(1.0, depth)) * math.tau * 3.0 + phases[2]))
        yaw += rng.gauss(0.0, 18.0)
        rotation = (
            clamp(-math.degrees(math.atan(slope_z)) + rng.gauss(0.0, 7.0), -32.0, 32.0),
            yaw % 360.0,
            clamp(math.degrees(math.atan(slope_x)) + rng.gauss(0.0, 6.0), -26.0, 26.0),
        )
        y = ground_y + surface_y + layer_offset + rng.uniform(-3.5, 3.5)
        y += bottom_offset(bounds, scale, rotation)

        yield f"object banana_{index:04d} {{"
        yield "mesh=banana"
        yield f"position=({format_float(x)},{format_float(y)},{format_float(z)})"
        yield f"rotation=({format_float(rotation[0])},{format_float(rotation[1])},{format_float(rotation[2])})"
        yield f"scale=({format_float(scale)},{format_float(scale)},{format_float(scale)})"
        yield "material=banana"
        yield "}"

    yield END_MARKER


def iter_pile_lines(
    count: int,
    seed: int,
    bounds: tuple[tuple[float, float, float], tuple[float, float, float]],
    ground_y: float = 0.0,
    reference_count: int = DEFAULT_REFERENCE_COUNT,
    pile_scale_override: float | None = None,
    size_ratio: float = DEFAULT_SIZE_RATIO,
    height_ratio: float = DEFAULT_HEIGHT_RATIO,
    irregularity: float = DEFAULT_IRREGULARITY,
    height_to_radius_ratio: float = DEFAULT_HEIGHT_TO_RADIUS_RATIO,
) -> Iterable[str]:
    pile_scale, footprint_scale, height_scale = pile_shape_scales(
        count,
        reference_count,
        pile_scale_override,
        size_ratio,
        height_ratio,
        height_to_radius_ratio,
    )
    irregularity = validate_unit_interval(irregularity, "--irregularity")

    rng = random.Random(seed)
    shape_rng = random.Random(seed ^ 0xBADA551)
    phases = tuple(shape_rng.random() * math.tau for _ in range(6))
    center_x = 0.0
    center_z = 45.0
    base_radius_x = BASE_RADIUS_X * footprint_scale
    base_radius_z = BASE_RADIUS_Z * footprint_scale
    max_pile_height = BASE_MAX_HEIGHT * height_scale
    position_jitter = 3.0 * footprint_scale
    vertical_jitter_min = -1.5 * height_scale
    vertical_jitter_max = 3.5 * height_scale
    mounds = (
        (0.0, center_z, base_radius_x * 0.66, base_radius_z * 0.60, max_pile_height * 0.76, 2.30),
        (-base_radius_x * 0.20, center_z + (base_radius_z * 0.08), base_radius_x * 0.66, base_radius_z * 0.58, max_pile_height * 0.70, 2.10),
        (base_radius_x * 0.24, center_z - (base_radius_z * 0.05), base_radius_x * 0.62, base_radius_z * 0.55, max_pile_height * 0.64, 2.00),
        (-base_radius_x * 0.05, center_z + (base_radius_z * 0.38), base_radius_x * 0.80, base_radius_z * 0.38, max_pile_height * 0.26, 1.45),
        (base_radius_x * 0.10, center_z - (base_radius_z * 0.44), base_radius_x * 0.86, base_radius_z * 0.36, max_pile_height * 0.24, 1.40),
    )

    yield (
        f"{BEGIN_MARKER} count={count} seed={seed}"
        f" pile_scale={format_float(pile_scale)}"
        f" height_to_radius={format_float(height_to_radius_ratio)}"
        f" size_ratio={format_float(size_ratio)}"
        f" height_ratio={format_float(height_ratio)}"
        f" irregularity={format_float(irregularity)}"
    )
    yield "# Generated by tools/generate_banana_pile.py; rerun that script to reshape this pile."

    for index in range(count):
        spill = rng.random()
        if spill < 0.28:
            mound = mounds[3 if rng.random() < 0.5 else 4]
            rho = 0.84 + (0.42 * rng.random())
            height_fraction = rng.random() ** 5.0
        elif spill < 0.52:
            mound = mounds[1 if rng.random() < 0.58 else 2]
            rho = rng.random() ** 0.38
            height_fraction = rng.random() ** 3.6
        else:
            pick = rng.random()
            if pick < 0.48:
                mound = mounds[0]
            elif pick < 0.78:
                mound = mounds[1]
            else:
                mound = mounds[2]
            rho = math.sqrt(rng.random())
            height_fraction = rng.random() ** 2.15

        theta = rng.random() * math.tau
        footprint_lobe = max(0.68, 1.0 + (0.72 * irregularity * shape_wave(theta, phases)))
        height_lobe = max(0.68, 1.0 + (0.62 * irregularity * shape_wave(theta + 1.1, phases)))
        mound_x, mound_z, mound_radius_x, mound_radius_z, _, _ = mound
        x = mound_x + (math.cos(theta) * rho * mound_radius_x * footprint_lobe) + rng.gauss(0.0, position_jitter)
        z = mound_z + (math.sin(theta) * rho * mound_radius_z * footprint_lobe) + rng.gauss(0.0, position_jitter)
        envelope, radial = pile_envelope(x, z, mounds)
        shoulder = max(0.15, 1.0 - radial)
        rough_height = ground_wave(x, z, phases) * irregularity * max_pile_height * 0.22
        envelope *= height_lobe
        contact_y = (2.0 * pile_scale) + (height_fraction * envelope) + rng.uniform(vertical_jitter_min, vertical_jitter_max)
        contact_y += rough_height * shoulder
        contact_y += rng.uniform(-1.0, 1.0) * max_pile_height * irregularity * 0.07 * (height_fraction ** 1.4)
        peak_drift = irregularity * (height_fraction ** 1.35)
        x += peak_drift * base_radius_x * 0.12 * math.sin((4.0 * height_fraction) + phases[0])
        z += peak_drift * base_radius_z * 0.12 * math.sin((5.0 * height_fraction) + phases[1])

        scale = rng.uniform(135.0, 215.0) * (1.0 - (0.08 * height_fraction))
        if rng.random() < 0.08:
            x_tilt = rng.uniform(-60.0, 60.0)
            z_tilt = rng.uniform(-70.0, 70.0)
        else:
            tilt_span = 20.0 + (24.0 * height_fraction)
            x_tilt = max(-54.0, min(54.0, rng.gauss(0.0, tilt_span)))
            z_tilt = max(-58.0, min(58.0, rng.gauss(0.0, tilt_span * 1.12)))
        rotation = (x_tilt, rng.uniform(0.0, 360.0), z_tilt)
        y = ground_y + max(1.0 * pile_scale, contact_y) + bottom_offset(bounds, scale, rotation)

        yield f"object banana_{index:04d} {{"
        yield "mesh=banana"
        yield f"position=({format_float(x)},{format_float(y)},{format_float(z)})"
        yield f"rotation=({format_float(rotation[0])},{format_float(rotation[1])},{format_float(rotation[2])})"
        yield f"scale=({format_float(scale)},{format_float(scale)},{format_float(scale)})"
        yield "material=banana"
        yield "}"

    yield END_MARKER


def generate_pile(
    count: int,
    seed: int,
    bounds: tuple[tuple[float, float, float], tuple[float, float, float]],
    ground_y: float = 0.0,
    reference_count: int = DEFAULT_REFERENCE_COUNT,
    pile_scale_override: float | None = None,
    size_ratio: float = DEFAULT_SIZE_RATIO,
    height_ratio: float = DEFAULT_HEIGHT_RATIO,
    irregularity: float = DEFAULT_IRREGULARITY,
    height_to_radius_ratio: float = DEFAULT_HEIGHT_TO_RADIUS_RATIO,
) -> str:
    return "\n".join(iter_pile_lines(
        count,
        seed,
        bounds,
        ground_y,
        reference_count,
        pile_scale_override,
        size_ratio,
        height_ratio,
        irregularity,
        height_to_radius_ratio,
    ))


def replace_pile(scene_text: str, generated_pile: str) -> str:
    marker_pattern = re.compile(
        rf"(?ms)^{re.escape(BEGIN_MARKER)}.*?^{re.escape(END_MARKER)}$"
    )
    if marker_pattern.search(scene_text):
        return marker_pattern.sub(generated_pile, scene_text)

    single_banana_pattern = re.compile(r"(?ms)^object\s+banana\s*\{\s*.*?^\}")
    if single_banana_pattern.search(scene_text):
        return single_banana_pattern.sub(generated_pile, scene_text, count=1)

    raise RuntimeError(
        f"Could not find a generated pile or a single 'object banana' block in {DEFAULT_SCENE}."
    )


def write_pile_lines(scene_path: Path, pile_lines: Iterable[str]) -> None:
    scene_directory = scene_path.parent
    scene_mode = scene_path.stat().st_mode & 0o7777
    temp_file = tempfile.NamedTemporaryFile(
        "w",
        delete=False,
        dir=scene_directory,
        encoding="utf-8",
    )
    temp_path = Path(temp_file.name)
    replaced = False
    skipping_generated = False
    skipping_single_banana = False
    pile_iterator = iter(pile_lines)

    def write_generated_pile() -> None:
        for pile_line in pile_iterator:
            temp_file.write(pile_line)
            temp_file.write("\n")

    try:
        with temp_file:
            with scene_path.open("r", encoding="utf-8", errors="replace") as scene_file:
                for raw_line in scene_file:
                    stripped = raw_line.strip()

                    if skipping_generated:
                        if stripped.startswith(END_MARKER):
                            skipping_generated = False
                        continue

                    if skipping_single_banana:
                        if stripped == "}":
                            skipping_single_banana = False
                        continue

                    if stripped.startswith(BEGIN_MARKER):
                        write_generated_pile()
                        replaced = True
                        skipping_generated = True
                        continue

                    if re.match(r"^object\s+banana\s*\{\s*$", stripped):
                        write_generated_pile()
                        replaced = True
                        skipping_single_banana = True
                        continue

                    temp_file.write(raw_line)

        if not replaced:
            raise RuntimeError(
                f"Could not find a generated pile or a single 'object banana' block in {scene_path}."
            )
        os.chmod(temp_path, scene_mode)
        os.replace(temp_path, scene_path)
    except Exception:
        temp_path.unlink(missing_ok=True)
        raise


def fitted_camera_lines(ground_y: float, footprint_scale: float, height_scale: float) -> list[str]:
    center_z = 45.0
    camera_distance = 740.0 * footprint_scale
    camera_height = max(240.0 * height_scale, 110.0 * footprint_scale)
    target_height = 0.50 * 175.0 * height_scale
    camera_z = center_z - camera_distance
    direction_y = (target_height - camera_height) / camera_distance
    focus_distance = math.sqrt(camera_distance * camera_distance + (camera_height - target_height) ** 2) * 0.01

    return [
        "camera main {",
        f"position=(0,{format_float(ground_y + camera_height)},{format_float(camera_z)})",
        f"direction=(0,{format_float(direction_y)},1)",
        "up=(0,1,0)",
        "focal_length_mm=35",
        "sensor_width_mm=36",
        "sensor_height_mm=22.5",
        "pinhole=1",
        f"focus_distance={format_float(focus_distance)}",
        "}",
    ]


def fitted_ocean_camera_lines(
    ground_y: float,
    count: int,
    reference_count: int,
    pile_scale_override: float | None,
    size_ratio: float,
    height_ratio: float,
    irregularity: float,
    seed: int,
    bounds: tuple[tuple[float, float, float], tuple[float, float, float]],
    banana_scale: float,
) -> list[str]:
    _, width, depth, wave_height, _ = ocean_shape(
        count,
        reference_count,
        pile_scale_override,
        size_ratio,
        height_ratio,
        bounds,
        banana_scale,
    )
    irregularity = validate_unit_interval(irregularity, "--irregularity")
    shape_rng = random.Random(seed ^ 0x0CEAA11)
    phases = tuple(shape_rng.random() * math.tau for _ in range(5))
    min_z = OCEAN_CENTER_Z - (depth * 0.5)
    camera_x = -width * 0.16
    camera_z = min_z + (depth * 0.07)
    camera_surface_y = ocean_wave_height(camera_x, camera_z, width, depth, wave_height, irregularity, phases)
    camera_clearance = clamp(wave_height * 1.95, 260.0, depth * 0.12)
    look_distance = clamp(camera_clearance * 1.05, 420.0, depth * 0.16)
    target_z = camera_z + look_distance
    target_x = camera_x + (look_distance * 0.16)
    target_y = ocean_wave_height(target_x, target_z, width, depth, wave_height, irregularity, phases) + (wave_height * 0.08) - (look_distance * 0.16)
    distance_z = max(1.0, target_z - camera_z)
    direction_x = (target_x - camera_x) / distance_z
    direction_y = (target_y - camera_surface_y - camera_clearance) / distance_z
    focus_distance = math.sqrt(
        ((target_x - camera_x) * (target_x - camera_x))
        + (distance_z * distance_z)
        + ((target_y - camera_surface_y - camera_clearance) * (target_y - camera_surface_y - camera_clearance))
    ) * 0.01

    return [
        "camera main {",
        f"position=({format_float(camera_x)},{format_float(ground_y + camera_surface_y + camera_clearance)},{format_float(camera_z)})",
        f"direction=({format_float(direction_x)},{format_float(direction_y)},1)",
        "up=(0,1,0)",
        "focal_length_mm=12",
        "sensor_width_mm=36",
        "sensor_height_mm=22.5",
        "pinhole=1",
        f"focus_distance={format_float(focus_distance)}",
        "}",
    ]


def fit_camera(
    scene_path: Path,
    ground_y: float,
    footprint_scale: float,
    height_scale: float | None = None,
    replacement_lines: list[str] | None = None,
) -> None:
    if height_scale is None:
        height_scale = footprint_scale
    if replacement_lines is None:
        replacement_lines = fitted_camera_lines(ground_y, footprint_scale, height_scale)

    scene_directory = scene_path.parent
    scene_mode = scene_path.stat().st_mode & 0o7777
    temp_file = tempfile.NamedTemporaryFile(
        "w",
        delete=False,
        dir=scene_directory,
        encoding="utf-8",
    )
    temp_path = Path(temp_file.name)
    replaced = False
    skipping_camera = False

    try:
        with temp_file:
            with scene_path.open("r", encoding="utf-8", errors="replace") as scene_file:
                for raw_line in scene_file:
                    stripped = raw_line.strip()

                    if skipping_camera:
                        if stripped == "}":
                            skipping_camera = False
                        continue

                    if re.match(r"^camera\s+main\s*\{\s*$", stripped):
                        for camera_line in replacement_lines:
                            temp_file.write(camera_line)
                            temp_file.write("\n")
                        replaced = True
                        skipping_camera = True
                        continue

                    temp_file.write(raw_line)

        if not replaced:
            raise RuntimeError(f"Could not find 'camera main' block in {scene_path}.")
        os.chmod(temp_path, scene_mode)
        os.replace(temp_path, scene_path)
    except Exception:
        temp_path.unlink(missing_ok=True)
        raise


def main() -> None:
    args = parse_args()
    scene_path = args.scene
    mesh_path = resolve_mesh_path_from_file(scene_path)
    bounds = read_obj_bounds(mesh_path)
    validate_unit_interval(args.irregularity, "--irregularity")

    if args.layout == "ocean":
        _, width, depth, _, cell_size = ocean_shape(
            args.count,
            args.reference_count,
            args.pile_scale,
            args.size_ratio,
            args.height_ratio,
            bounds,
            args.banana_scale,
        )
        pile_lines = iter_ocean_lines(
            args.count,
            args.seed,
            bounds,
            args.ground_y,
            args.reference_count,
            args.pile_scale,
            args.size_ratio,
            args.height_ratio,
            args.irregularity,
            args.banana_scale,
        )
        camera_lines = fitted_ocean_camera_lines(
            args.ground_y,
            args.count,
            args.reference_count,
            args.pile_scale,
            args.size_ratio,
            args.height_ratio,
            args.irregularity,
            args.seed,
            bounds,
            args.banana_scale,
        )
        reference_depth = math.sqrt((args.reference_count * cell_size * cell_size) / OCEAN_DEPTH_RATIO) * OCEAN_DEPTH_RATIO
        footprint_scale = max(width, depth) / reference_depth
        height_scale = args.height_ratio
    else:
        _, footprint_scale, height_scale = pile_shape_scales(
            args.count,
            args.reference_count,
            args.pile_scale,
            args.size_ratio,
            args.height_ratio,
            args.height_to_radius_ratio,
        )
        pile_lines = iter_pile_lines(
            args.count,
            args.seed,
            bounds,
            args.ground_y,
            args.reference_count,
            args.pile_scale,
            args.size_ratio,
            args.height_ratio,
            args.irregularity,
            args.height_to_radius_ratio,
        )
        camera_lines = None

    write_pile_lines(scene_path, pile_lines)
    if args.fit_camera:
        fit_camera(scene_path, args.ground_y, footprint_scale, height_scale, camera_lines)
    print(f"generated {args.count} banana instances in {scene_path}")


if __name__ == "__main__":
    main()
