# Benchmarks

Luz includes deterministic benchmark workflows for measuring renderer changes
across render time, denoise time, post-process time, and overall score.

## Quick Comparison

Run the containerized benchmark matrix and save raw results:

```sh
make benchmark BENCH_CPUS=1 BENCH_THREADS=1 > before.csv
```

After an optimization, run the same matrix again and compare medians:

```sh
make benchmark BENCH_CPUS=1 BENCH_THREADS=1 > after.csv
make benchmark-compare BEFORE=before.csv AFTER=after.csv
```

The benchmark score is printed to stderr at the end of the run, so redirecting
stdout still writes a clean raw CSV.

## Direct Runs

Run the deterministic default benchmark case without the container:

```sh
./luz --benchmark --seed 424242424 --threads 1
```

Run a scene in benchmark mode:

```sh
./luz examples/scenes/cornell.luz --resolution 320x180 --samples 128 --max-light-bounces 5 --benchmark
```

## Profiling Sparse Volumes on macOS

Use a deterministic, single-threaded render so sampled stacks are easy to
attribute and repeat:

```sh
./luz examples/scenes/disney-cloud-hero-closeup.luz \
  --resolution 240x135 --samples 96 --no-adaptive --threads 1 \
  --seed 987654 --no-denoise --output /tmp/cloud-profile.png
sample luz 10 1 -file /tmp/luz-cloud-profile.txt
```

The July 2026 hero-cloud profile found scalar sparse-volume work, not memory
movement, dominating active CPU samples. Before optimization, rescanning each
brick's 3x3x3 interpolation halo consumed about 38% of sampled active CPU time.
The renderer now builds immutable conservative interpolation bounds when a
`.luzvol` is loaded, transforms each ray once per volume traversal, and uses a
one-lookup trilinear path when all eight taps occupy the same 8x8x8 brick. Its
stateful 3D-DDA traversal also retains brick coordinates and exact per-axis
boundary times instead of reconstructing them from a floating-point position
at every step.

On the same 320x180, 16-spp, 12-thread, fixed-seed hero render, the three-run
median render time fell from 20.3 seconds to 12.5 seconds (38.4%, or 1.62x
faster); median total time fell from 22.2 seconds to 14.4 seconds. The PNG
SHA-256 stayed identical. In an alternating same-session A/B after those
measurements, the DDA reduced render time from 14.2 to 13.8 seconds and from
13.3 to 12.5 seconds in the two pairs.

The post-DDA sample's largest active leaves were trilinear density sampling
(22.5%), brick-segment traversal (14.5%), `exp` (11.9%), sampler hashing (9.0%),
and `log` (7.7%); `memmove` was 0.13%. Future sparse-volume work should therefore
target density access locality before ownership or allocation changes.

The authored Disney half-cloud is a 409 MB 16-bit brick field. Predecoding it
to floats would add roughly another 400 MB of resident voxel data, so Luz keeps
the compact representation. Instead, public grid samples retain finite and
range validation while volume collision and shadow points—already constrained
to a successful integration interval—enter the same interpolation through an
internal checked-by-construction path. Alternating pairs improved from 12.9 to
12.5 seconds and from 14.5 to 13.1 seconds with identical output hashes. Those
lookups also reuse the traversal's local-space ray; this is algebraically
equivalent and avoids redundant transforms, although its isolated wall-clock
effect was within run-to-run noise.

Same-brick trilinear footprints contain four contiguous pairs along X. Luz
loads those pairs with four alias-safe `memcpy` operations instead of eight
independent halfword loads; optimized ARM64 builds lower them to four unaligned
native 32-bit loads. The float decode and double interpolation order are
unchanged. Alternating pairs improved from 12.3 to 12.2 seconds and from 12.3
to 11.5 seconds, again with identical output hashes.

After packed loading, the sampler leaf fell from 21.2% to 18.8% of interval
samples. Brick traversal was 14.9% and the system `exp` implementation was
13.7%, so density interpolation is no longer overwhelmingly dominant; further
work should evaluate transcendental-call count as carefully as grid access.

The deterministic primary-cloud control previously evaluated `expm1(-tau)`
for integrated segment energy and `exp(-tau)` again for transmittance. Luz now
reuses the first result through the exact identity `exp(-tau) = 1 + expm1(-tau)`;
feature-ray threshold crossings likewise retain their already sampled density.
Alternating hero pairs improved from 12.3 to 12.1 seconds and from 12.6 to 12.3
seconds with byte-identical output. The subsequent `exp` leaf fell from 13.7%
to 12.0% of interval samples without introducing an approximate exponential.

The optional authored-density threshold and gamma remap is evaluated through a
65,536-interval monotonic LUT and applies the same mapping to collision samples
and conservative brick bounds. Profiling the shaped hero cloud showed no LUT
leaf and only 11 `memmove` leaves in 8,508 active worker samples (0.13%). The
initial conservative bound rounding used `nextafterf`; including its dynamic
link stubs it consumed about 1.9% of active samples. Luz now advances positive
IEEE-754 float bounds directly by one ULP, retaining identical conservative
bounds without function calls. The deterministic shaped PNG remained
byte-identical; the immediate 320x180, 16-spp pair improved from 13.7 to 13.3
seconds, and the single-threaded 240x135 profile render improved from 49.8 to
44.0 seconds. In the confirmation profile, `nextafterf` disappeared. The top
active leaves were density interpolation (24.4%), brick traversal (19.1%), the
remaining shadow traversal body (17.8%), sampler hashing (10.6%), `exp` (9.5%),
and `log` (9.2%).

For convergence decisions, compare independent fixed-seed renders rather than
judging a single noisy image. On the 320x180, 16-spp exterior hero, enabling a
32,768-path 12-cubed, 16-lobe volume guide at strength 0.25 reduced display-space
luminance RMSE between two seeds from 0.08747 to 0.08195 (6.3%) and mean absolute
disagreement from 0.05625 to 0.05150 (8.5%). Guide training took about one
second, so the final 768-spp hero enables it. The same experiment from inside
the volume changed RMSE from 0.04475 to 0.04514, a slight regression; the
interior preset therefore leaves learned guiding disabled. Guiding is a
scene-dependent variance tool, not a universal quality switch.

The hero-cloud lighting audit also separates highlight exposure from shadow
fill. At 320x180 and 32 spp, increasing `atmosphere_sun_scale` from 0.5 to 1.0
and using restrained multiple-scattering compensation of 0.26 raised display
median luminance from 0.410 to 0.575, while p99 moved only from 0.947 to 0.953
and clipped pixels remained at 0%. This is preferable to a global exposure
increase: procedural skylight opens blue-gray cavities while the directional
sun retains textured white lobes.

The deterministic primary-cloud march must also stay below the projected voxel
scale. On the square 500x500 hero crop, the former `final` detail of 4 produced
visible concentric and vertical march bands around deep cavities. Detail 8
removed most of the structure and rendered in 35.2 seconds versus 36.0 seconds
for detail 4 at the same 8 spp; stochastic transport remained the bottleneck.
The new `reference` tier uses detail 16 and rendered in 39.2 seconds. A diagnostic
four-times-larger directional cache produced only a marginal visual change,
raised guide training from 1.23 to 2.62 seconds, and rendered in 40.3 seconds,
so Luz retains the 16-million-point cache cap. `final` now selects detail 8 and
`reference` selects detail 16 rather than spending hundreds of megabytes on the
wrong source of the artifact.

## Scoring Results

Score an existing benchmark CSV:

```sh
make benchmark-score RESULTS=after.csv
```

Each case score is the median kilo-samples per minute for that case. The overall
score uses the geometric mean of per-case scores, so `before.csv` and
`after.csv` are comparable when benchmark settings are the same.

## CSV Output

Raw benchmark CSVs include:

- Full process elapsed time, including scene loading
- Render time
- Learned volume-guide training time
- Denoise time
- Post-process time
- Total samples rendered
- Average samples per pixel
- Display luminance p01, p50, and p99 after the selected view transform
- Near-black, near-white, and clipped display-pixel fractions
- Actual score
- Render-only score

The compare script reports elapsed speedup, render-time speedup, score speedup,
render-score speedup, and how much non-render work is in each case.

## Benchmark Matrix

The default matrix covers Cornell-style lighting, many objects, mesh BVH
traversal, diffuse scattering, post-processing, atmosphere, mixed light types,
emissive geometry, primitives/materials, volumes, OBJ meshes, and representative
scene cases. The Suzanne scene cases exercise `.luz` mesh loading with one OBJ
use and many repeated uses of the same OBJ file.

The Makefile currently requests these default cases:

```text
default many-objects mesh-bvh diffuse postprocess atmosphere lights
emissive-geometry primitives-materials volumes obj-mesh
suzanne-single suzanne-instances
stormtroopers-preview stormtroopers-adaptive-tuned stormtroopers-denoise-micro
```

Scene-backed cases are skipped if their scene file is not present. For example,
the stormtrooper cases use `exports/stormtroopers.luz` when that export exists.

## Useful Settings

Keep benchmark settings fixed between before/after runs. The most common knobs
are:

- `BENCH_CPUS`: CPU quota passed to Docker
- `BENCH_THREADS`: Luz render worker count
- `BENCH_REPEAT`: measured runs per case
- `BENCH_WARMUP`: warmup runs per case
- `BENCH_SEED`: deterministic render seed
- `BENCH_CASES`: space-separated benchmark cases
- `BENCH_WIDTH` and `BENCH_HEIGHT`: override case resolution
- `BENCH_SAMPLES`: override samples per pixel
- `BENCH_DENOISE`: override the default denoising state
- `BENCH_ADAPTIVE`: override the default adaptive sampling state
- `BENCH_SCORE_SAMPLE_UNIT`: score divisor, defaulting to `1000`
