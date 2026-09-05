# Luz

**Luz** is a C++20 Path Tracer developed from scratch with zero third-party dependencies.

It supports Monte Carlo path tracing, global illumination, BVH acceleration,
adaptive sampling, denoising, atmospheric scattering, physically parameterized
scene files, and a Blender-to-Luz exporter.

https://github.com/user-attachments/assets/7dc03485-9418-47af-a7e7-c4c4c53b6b70

<img width="1920" height="1080" alt="bust-statue" src="https://github.com/user-attachments/assets/3173b5c1-f81f-404a-8740-20635b583e5c" />


## Features

- Monte Carlo path tracing
- Global illumination
- Multithreaded CPU rendering
- Adaptive sampling
- Denoiser (NFOR-style)
- Spheres, planes, rectangles, triangles, cubes, volumes, and OBJ meshes
- Procedural clouds with cumulus, stratus, cirrus, and storm presets
- Imported cloud volumes in the sparse `.luzvol` format (optional VDB converter)
- Scene-linear ACEScg rendering with sRGB input/output transforms
- Spectral authoring helpers: wavelength, blackbody, solar, and reflectance curves
- Lambertian, GGX metal, rough dielectric, layered principled with subsurface
  scattering, emissive, isotropic, and Henyey-Greenstein phase materials
- Measured conductor, glass, and volume presets, plus IES lamp profiles
- Area, point, sphere and directional lights with physical units
- PPM and HDR equirectangular environment maps with calibrated lighting and MIS
- Custom `.luz` scene files
- .blend to .luz converter
- Fully customizable render parameters via CLI or scene file
- Importance sampling with PDFs, MIS, and optional caustic photon mapping
- Optional volume path guiding
- BVH acceleration, including packed mesh BVHs with binned SAH construction and near-first traversal
- Atmospheric simulation w/ scattering
- Physical camera focal length, sensor size, aperture/f-stop, focus distance,
  photographic exposure, antialiasing, contrast, tone mapping, sRGB encoding,
  and bloom
- BMP, PNG, and 32-bit floating-point TIFF output
- Deterministic benchmark harness with render, denoise, post-process, and score breakdowns

## Requirements

- C++20 compiler
- Make or CMake 3.16+
- Python 3, only for optional tools/scripts

## Quick Start

Build with the Makefile:

```sh
make
```

Render a bundled example scene:

```sh
./luz examples/scenes/cornell.luz --samples 50 --resolution 300x300
```

The primary output is `render.bmp`, with `render_denoised.bmp` written by
default. Scene files can set `outputfilename=...`, and the CLI can override
common render settings. Use a `.bmp`, `.png`, or `.tiff` output path to select
the format.

The volumetric fog and godrays sample is:

```sh
./luz --file examples/scenes/volumetric_godrays.luz --threads 8
```

Run the test suite:

```sh
make test
```

## Clouds

Render procedural clouds with no extra assets:

```sh
./luz examples/scenes/procedural-cumulus-daylight.luz --samples 32 --resolution 960x540
```

More presets and a layered cloudscape are in [`examples/scenes/`](examples/scenes/).
Use `--samples` and `--resolution` to balance render time and quality.

For the Disney golden-hour hero, first follow the
[volume import instructions](docs/scene-files.md#authored-sparse-volumes)
to create `assets/volumes/wdas_cloud_half.luzvol`, then render:

```sh
./luz examples/scenes/disney-cloud-golden-hero.luz --samples 32 --resolution 960x540
```

Disney scenes need a separately downloaded dataset. Only the optional VDB
converter requires OpenVDB; Luz itself has no third-party dependencies.
See the [cloud reference](docs/scene-files.md#procedural-clouds) for customization.

## Benchmarking

Luz includes deterministic benchmarks for render, denoise, post-process, and
overall score comparisons.

```bash
make benchmark BENCH_CPUS=1 BENCH_THREADS=1 > before.csv
make benchmark BENCH_CPUS=1 BENCH_THREADS=1 > after.csv
make benchmark-compare BEFORE=before.csv AFTER=after.csv
```

For details, see [`docs/benchmarks.md`](docs/benchmarks.md).

## CMake

A CMake build is also available:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## Platform Support

Supported platforms:

- macOS
- Linux
- Windows

On macOS and Linux, the Makefile is the primary path. On Windows, use CMake with
MSVC or the MinGW-based Makefile target:

```sh
make windows
```

WSL is also supported as a Linux build environment.

## Build Optimizations

Release builds are tuned for the machine doing the build by default. The
Makefile enables `-O3`, native CPU tuning with `-march=native`, and link-time
optimization with `-flto`. It also enables a fast floating-point mode where the
compiler/platform supports it. CMake uses the same release intent: `-O3`, native
CPU tuning, and interprocedural optimization/LTO when supported.

These defaults produce faster local renders, but binaries built with
`-march=native` may not run on older or different CPUs, and LTO can expose
toolchain-specific linker issues. If you hit an illegal-instruction crash,
linker error, or need a more portable binary, disable the aggressive options and
rebuild from clean objects:

```sh
make clean
make NATIVE=0 LTO=0
```

For CMake builds, configure with the optimization toggles off:

```sh
cmake -S . -B build -DLUZ_NATIVE_OPTIMIZATIONS=OFF -DLUZ_ENABLE_LTO=OFF
cmake --build build --clean-first
```

## CLI

```text
Usage: ./luz [scene.luz] [options]

Arguments:
  PATH                        Load a .luz scene file

Options:
  -f, --file PATH             Load a .luz scene file
  -r, --resolution WxH        Override render resolution
  -s, --samples N             Override samples per pixel
  --adaptive [true|false]     Toggle adaptive sampling (default: true)
  --no-adaptive               Disable adaptive sampling
  --adaptive-min-samples N    Minimum samples before adaptive stopping
  --adaptive-background-min-samples N
                               Background floor (0 inherits adaptive minimum)
  --adaptive-volume-min-samples N
                               Volume floor (0 inherits adaptive minimum)
  --adaptive-threshold F      Relative adaptive noise threshold
  --adaptive-check-interval N Adaptive convergence check interval
	-mlb, --maxLightBounces N   Override maximum light bounces
	    --max-light-bounces N   Alias for --maxLightBounces
	-t, --threads N             Render with N worker threads
	--seed N                    Seed random sampling
	--view-transform standard|agx|aces|raw
	                              Select display transform; raw is debugging/HDR data, not viewing
	--bloom true|false          Toggle bloom
  --exposure EV              Exposure compensation in stops
  --contrast F               Display contrast multiplier
  --denoise [true|false]      Toggle denoised companion render (default: true)
  --no-denoise                Disable denoising
  -o, --output PATH.EXT       Override render output path
  --denoise-output PATH.EXT   Override denoised output path
  --render-times              Write renderTime.bmp
  --benchmark                 Run the built-in benchmark scene
  --benchmark-case NAME       Benchmark case: default, many-objects, mesh-bvh, diffuse, postprocess, atmosphere, lights, emissive-geometry, primitives-materials, volumes, obj-mesh
```

TIFF output stores RGB as uncompressed 32-bit IEEE floating-point samples. Use
`--output render.tiff --view-transform raw` to preserve scene-linear ACEScg HDR
values above 1.0. Raw output is for debugging, measurement, and compositing data,
not normal viewing.

## Physically Based Authoring

Luz scene units are controlled with `meters_per_unit`, and color values are
converted into scene-linear ACEScg before rendering. Numeric triples are ACEScg
values by default; use explicit functions for other sources:

```text
color=srgb(0.8,0.2,0.1)
color=wavelength(550nm)
color=blackbody(3000K)
color=solar
color=reflectance(materials/red_paint.spd)
```

Lights can be authored with physical quantities:

```text
area_light softbox {
position=(0,3,0)
size=(2,1)
normal=(0,-1,0)
color=blackbody(3200K)
lumens=12000
}

directional_light sun {
direction=(-0.2,-1,-0.1)
color=solar
solar=1
}
```

Cameras can use real lens controls:

```text
camera main {
position=(0,1.5,5)
direction=(0,0,-1)
focal_length_mm=50
sensor_width_mm=36
sensor_height_mm=24
f_stop=2.8
focus_distance=4
shutter=0.0166667
iso=400
}
```

The complete scene-file reference is in
[`docs/scene-files.md`](docs/scene-files.md).

## Adaptive Sampling

Adaptive sampling is enabled by default: `--samples` sets the maximum samples
per pixel, and converged pixels stop early. Lower `--adaptive-threshold` for
more detail, or use `--no-adaptive` to render every pixel at the full sample count.
See the [scene-file reference](docs/scene-files.md) for advanced settings.

Lower thresholds keep more detail and cost more time. For final renders, start
with a high max sample count and tune with values like:

```sh
./luz exports/stormtroopers.luz --samples 4096 --adaptive-min-samples 512 --adaptive-check-interval 64 --adaptive-threshold 0.005
```

## Denoising

Denoising is enabled by default. Luz's NFOR-style feature-buffer denoiser writes
a separate companion image: by default, `render.bmp` becomes
`render_denoised.bmp`; use `--denoise-output PATH.EXT` to choose the exact
path. Use `--no-denoise` or `--denoise false` to skip the companion render.

PNG output writes post-processed 8-bit RGB SDR images using stored DEFLATE
blocks for dependency-free writing.

The denoiser has no hard minimum resolution or sample count, but it needs enough
signal to estimate useful color and feature statistics. One sample per pixel is
mainly a stress test: there is no per-pixel variance estimate, so the denoised
image can look almost unchanged or can smooth the wrong details. Use at least a
few samples per pixel for previews, and prefer roughly 16+ samples per pixel
when judging denoiser quality. Very low resolutions also make evaluation
misleading because each local filter window covers too much of the image.

## Scene Files

Example scenes live in `examples/scenes/`. The scene-file format is documented in [`docs/scene-files.md`](docs/scene-files.md).

Object paths in `.luz` files use the path provided by the scene: absolute paths are used as-is, and relative paths are resolved from the directory containing the `.luz` file. Keep large or generated OBJ assets local unless they are intentionally reviewed for inclusion.

OBJ meshes can be declared, transformed, and assigned named scene materials:

```text
[materials]
material gold {
type=metal
preset=gold
roughness=0.15
}

[meshes]
mesh statue_mesh {
file=objects/statue.obj
}

[scene]
object statue {
mesh=statue_mesh
position=(0,0,0)
material=gold
}
```

## Blender Exporter

Blender scenes can be exported through Blender's Python API:

```sh
"/Applications/Blender.app/Contents/MacOS/Blender" -b scene.blend --python tools/blender_export_luz.py -- --output exports/scene.luz
./luz --file exports/scene.luz --threads 8
```

The exporter writes a `.luz` file plus OBJ meshes. Usage and current fidelity
limits are documented in [`docs/blender-exporter.md`](docs/blender-exporter.md).

## Repository Layout

```text
include/luz/       Public headers
src/core/          Math, geometry, materials, image, and sampling code
src/renderer/      Rendering implementation
src/scene/         Scene model and scene helpers
src/io/            Scene-file, OBJ, BMP, PNG, and TIFF loading/writing
src/cli/           Command-line entry point and flags
examples/scenes/   Example .luz scene files
examples/objects/  Example OBJ assets
docs/images/       Compressed showcase images
tools/             Export and utility scripts
tests/             Standard-library-only test program
docker/            Benchmark container
```

## Showcase

<img width="1920" height="1080" alt="stormtroopers" src="https://github.com/user-attachments/assets/8ccda949-9107-4211-be9d-a0daa55e3dd9" />
<img width="1920" height="1920" alt="cornell" src="https://github.com/user-attachments/assets/11d3b842-879e-4e30-b523-bed9678d43d8" />
<img width="1920" height="1920" alt="atmosphere" src="https://github.com/user-attachments/assets/b918af83-6c9a-4d31-a0f5-e5df7a01f2df" />
<img width="1920" height="1080" alt="rtionw" src="https://github.com/user-attachments/assets/6a215186-6b6a-4bff-b66a-822262e1d4dc" />

## Personal Note

Special thanks to the [Ray Tracing in One Weekend](https://github.com/RayTracing/raytracing.github.io) book series. It was a great source of inspiration and information during a big part of the development of Luz, specially since those were times before AI.

## Attribution

Stormtrooper Scene by @[ScottGraham](https://blendswap.com/profile/120125) on [BlendSwap](https://blendswap.com/blend/13953).

Bust Statue by @[geoffreymarchal](https://blendswap.com/profile/180520) on [BlendSwap](https://blendswap.com/blend/21704).

## License

MIT. See `LICENSE`.
