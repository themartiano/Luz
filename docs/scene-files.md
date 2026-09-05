# Luz Scene Files

Luz scene files use a small line-oriented format. Simple scenes can use only
`[settings]` and `[scene]`:

```text
[settings]
key=value

[scene]
camera main {
position=(x,y,z)
direction=(dx,dy,dz)
focal_length_mm=50
sensor_width_mm=36
sensor_height_mm=24
pinhole=1
focus_distance=4
}
objects{
object=...
}
```

Exporter-friendly scenes can also use `[materials]` and `[meshes]` sections.
Do not put blank lines between blocks inside the same section; a blank line ends
the section.

Blank lines end the current top-level section. Comments are lines whose first character is `#`. Do not put spaces around setting or object keys.

The parser is intentionally strict: unknown lines and malformed values throw an error instead of being silently ignored.

## Settings

| Setting | Format | Notes |
| --- | --- | --- |
| `resolution` | `resolution=WIDTH,HEIGHT` | Width and height must be positive. |
| `samples` | `samples=N` | Rays per pixel. |
| `adaptive` | `adaptive=0` or `adaptive=1` | Toggles adaptive per-pixel sampling. Enabled by default. When enabled, `samples` is the maximum samples per pixel. Aliases: `adaptivesampling`, `adaptive_sampling`. |
| `adaptiveminsamples` | `adaptiveminsamples=N` | Minimum samples before adaptive stopping can occur. Alias: `adaptive_min_samples`. |
| `adaptivebackgroundminsamples` | `adaptivebackgroundminsamples=N` | Background-only sample floor. `0` inherits `adaptiveminsamples`; a configured value is internally kept at two samples so thin-cloud opacity is classified before stopping. Alias: `adaptive_background_min_samples`. |
| `adaptivevolumeminsamples` | `adaptivevolumeminsamples=N` | Cloud and participating-media sample floor. `0` inherits `adaptiveminsamples`. Alias: `adaptive_volume_min_samples`. |
| `adaptivethreshold` | `adaptivethreshold=F` | Relative 95% confidence interval threshold for luminance convergence. Lower values render longer. Alias: `adaptive_threshold`. |
| `adaptivecheckinterval` | `adaptivecheckinterval=N` | Sample interval between adaptive convergence checks. Alias: `adaptive_check_interval`. |
| `maxlightbounces` | `maxlightbounces=N` | Maximum recursive light bounces. |
| `volume_primary_samples` | `volume_primary_samples=N` | Camera samples for deterministic primary cloud lighting, 1–256, default 4; clamped by total pixel samples. Adaptive rendering waits for this budget. Increase for thin silhouettes and depth of field. |
| `volume_primary_max_steps` | `volume_primary_max_steps=N` | Camera integration cap, 48–65536, default 1024. Use with cloud/grid `primary_detail` for fine structures over long intervals. |
| `volume_reference` | `volume_reference=0` or `1` | Default 0. At 1, bypass primary-light replacement, finite-order compensation, directional caches and bounce-dependent extinction/phase reduction. Procedural shadows use ratio tracking; grid shadows use residual ratio tracking. The scene's finite bounce limit, atmosphere model, adaptive sampling, output transforms and denoiser still apply; disable adaptive/denoise and increase bounces/samples for comparisons. |
| `volume_guiding_samples` | `volume_guiding_samples=N` | Total camera paths used to train the optional spatial-directional volume guide before rendering. `0` disables learned guiding and is the default. Alias: `volumeguidingsamples`. |
| `volume_guiding_resolution` | `volume_guiding_resolution=N` | Cells per axis in the learned guide, from 1 through 128. Defaults to 16. The combined field is rejected before allocation if it would exceed 512 MiB. Alias: `volumeguidingresolution`. |
| `volume_guiding_lobes` | `volume_guiding_lobes=N` | Directional lobes per cell, from 4 through 64. Defaults to 16. Alias: `volumeguidinglobes`. |
| `volume_guiding_anisotropy` | `volume_guiding_anisotropy=F` | Concentration of each learned directional lobe, from 0 through 0.95. Defaults to 0.8. Alias: `volumeguidinganisotropy`. |
| `volume_guiding_strength` | `volume_guiding_strength=F` | Fraction of the continuation proposal assigned to the frozen learned field, from 0 through 0.75. Defaults to 0.25 when training is enabled. Alias: `volumeguidingstrength`. |
| `volume_guiding_start_bounce` | `volume_guiding_start_bounce=N` | First path bounce allowed to use the learned proposal, from 0 through 64. Defaults to 1, preserving the analytic primary-cloud proposal while guiding multiple scattering. Alias: `volumeguidingstartbounce`. |
| `view_transform` | `view_transform=standard`, `agx`, `aces`, or `raw` | Selects the display transform. `standard` converts scene-linear ACEScg to clipped display sRGB. `agx` uses an AgX-style highlight rolloff and is the default. `aces` uses the ACES-fitted display transform. `raw` preserves scene-linear ACEScg HDR data for debugging/compositing and is not for display viewing. |
| `bloom` | `bloom=0` or `bloom=1` | Enables bloom when set to `1`. Bloom ignores isolated extreme firefly pixels so rare path samples do not expand into square glow blocks; display output also suppresses isolated saturated white fireflies. |
| `exposure` | `exposure=F` | Exposure compensation in stops. `1.0` doubles light before bloom and the view transform; `-1.0` halves it. |
| `photographic_exposure` | `photographic_exposure=F_NUMBER,SHUTTER_SECONDS,ISO` | Sets exposure from physical camera controls using `shutter * ISO / 100 / F_NUMBER^2`. `f/1`, `1s`, `ISO 100` equals `exposure=0`. Aliases: `photographicexposure`, `camera_exposure`, `cameraexposure`. |
| `contrast` | `contrast=F` | Display contrast multiplier applied after the display transform and before sRGB encoding. `1.0` keeps contrast unchanged. |
| `denoise` | `denoise=0` or `denoise=1` | Toggles the NFOR denoised companion image. Enabled by default. The denoiser runs before exposure, bloom, display transform, contrast, and sRGB encoding. |
| `denoiseoutputfilename` | `denoiseoutputfilename=PATH` | Optional denoised companion output path. Defaults to `outputfilename` with `_denoised` before the extension. Must use a `.bmp`, `.png`, or `.tiff` suffix. Aliases: `denoiseoutput`, `denoise_output`. |
| `outputfilename` | `outputfilename=PATH` | `.bmp` is appended if no suffix is present. Explicit suffixes must be `.bmp`, `.png`, or `.tiff`; `.tif` is not accepted. PNG output is 8-bit RGB SDR with sRGB metadata after a display view transform. BMP is plain 8-bit display output. TIFF output is uncompressed 32-bit floating-point RGB with Luz color-encoding metadata and is the required format for `view_transform=raw`. |
| `sky` | `sky=none`, `sky=linear`, `sky=atmosphere`, or `sky=environment` | Selects background rendering. |
| `background` | `background=COLOR` | Background color used when `sky=none`. Aliases: `backgroundcolor`, `background_color`. |
| `environment` | `environment=PATH[,ROTATION_DEGREES]` | Equirectangular environment map. If no explicit `sky=` appeared earlier in settings, `environment=...` also selects `sky=environment`. Aliases: `environmentmap`, `environment_map`, `backgroundimage`, `background_image`. |
| `environment_scale` | `environment_scale=F` | Direct multiplier for already calibrated environment radiance. Defaults to `1.0`. Mutually exclusive with physical environment calibration settings. Alias: `environmentscale`. |
| `environment_lighting` | `environment_lighting=0` or `1` | Toggles environment-map direct lighting/MIS while leaving visibility controlled by `sky`. Enabled by default. Alias: `environmentlighting`. |
| `environment_radiance` | `environment_radiance=F` | Scales the map so its solid-angle average luminance channel equals `F` in renderer radiance units. Alias: `environment_average_radiance`. |
| `environment_luminance` | `environment_luminance=F` | Scales the map so its solid-angle average luminance equals `F cd/m^2`, converted through 683 lm/W. Alias: `environment_average_luminance`. |
| `environment_irradiance` | `environment_irradiance=F` | Scales the map so upper-hemisphere horizontal irradiance equals `F W/m^2`. Alias: `environment_horizontal_irradiance`. |
| `environment_illuminance` | `environment_illuminance=F` | Scales the map so upper-hemisphere horizontal illuminance equals `F lux`, converted through 683 lm/W. Alias: `environment_horizontal_illuminance`. |
| `environmentrotation` | `environmentrotation=DEGREES` | Offsets the equirectangular U coordinate around world Y. Defaults to `0`. Aliases: `environment_rotation`, `worldrotation`, `world_rotation`. |
| `meters_per_unit` | `meters_per_unit=F` | Physical scale of Luz world coordinates. Defaults to `1.0`. Finite light `power`/`lumens` use physical area in square meters, and atmosphere ray distances are converted through this value. Alias: `metersperunit`. |
| `caustics` | `caustics=0` or `caustics=1` | Enables progressive caustic photon mapping. Disabled by default because the photon prepass is scene-dependent work. |
| `caustic_photons` | `caustic_photons=N` | Number of photons emitted in the caustic prepass. Defaults to `100000`. Alias: `causticphotons`. |
| `caustic_passes` | `caustic_passes=N` | Progressive radius-shrink passes used while building the caustic map. Defaults to `8`. Alias: `causticpasses`. |
| `caustic_radius` | `caustic_radius=METERS` | Initial caustic gather radius in meters. Luz converts it through `meters_per_unit`; progressive passes shrink the final lookup radius. Defaults to `0.05`. Alias: `causticradius`. |
| `caustic_alpha` | `caustic_alpha=F` | Progressive radius update factor in `(0,1]`. Lower values shrink faster and are sharper/noisier; higher values are smoother. Defaults to `0.7`. Alias: `causticalpha`. |
| `atmosphere` | `atmosphere=SUN,EARTH_RADIUS,ATMOSPHERE_RADIUS,HR,HM,SAMPLES,LIGHT_SAMPLES,STARS` | Only valid after `sky=atmosphere`. `SUN` is a fallback sun angle. If the scene has a `directional_light`, the first one drives atmosphere sun direction and source intensity instead. Without a directional light, atmosphere uses calibrated direct solar irradiance. |
| `atmosphere_sun_scale` | `atmosphere_sun_scale=F` | Multiplies atmosphere sun source intensity after it is sourced from the first `directional_light`, or from the fallback atmosphere sun when no directional light exists. Defaults to `1.0`. Aliases: `atmospheresunscale`, `atmosphere_sun_multiplier`, `atmospheresunmultiplier`. |
| `distanceblueness` | `distanceblueness=0` or `distanceblueness=1` | Enables distance blue tinting when set to `1`. |

### Adaptive Sampling Notes

Adaptive sampling is enabled by default and never exceeds `samples`. Surface
pixels render at least `adaptiveminsamples`; background and volume pixels can
use their dedicated floors. Luz classifies the primary ray from deterministic
surface/volume guides and cloud opacity, so thin wisps do not accidentally use
the background budget. It then periodically estimates luminance and RGB
variance and stops only when the configured confidence threshold is met.
Deterministic background misses can finish quickly. Dark surfaces and volumes
retain the conservative low-light safeguard so rare bright paths and deep cloud
shadow structure continue sampling.

### Learned Volume Path Guiding Notes

Learned volume guiding is opt-in. When `volume_guiding_samples` is positive,
Luz first traces a deterministic low-discrepancy set of camera paths through the
same scene. Visibility-tested finite-light, environment, atmosphere, emissive,
and indirect continuation observations accumulate into a bounded 3D grid of
directional radiance lobes. Accumulation uses saturating fixed-point atomics, so
the frozen field does not depend on worker scheduling. Image paths never update
it: training completes and the field freezes before the first production sample.

At render time, the learned proposal is mixed with the physical phase sampler
and the analytic sun/sky proposals. Luz evaluates the complete mixture PDF in
continuation and next-event MIS, preserving an unbiased estimator. This feature
is intended for difficult finite-light, HDR-environment, interior/exterior, and
indirectly lit media. A simple sunlit hero cloud already has a strong analytic
sun/sky guide, so training overhead may not pay for itself; leave the default of
zero unless a same-seed convergence test shows a benefit. A useful starting
point for complex lighting is:

```text
volume_guiding_samples=32768
volume_guiding_resolution=12
volume_guiding_lobes=16
volume_guiding_anisotropy=0.8
volume_guiding_strength=0.1
volume_guiding_start_bounce=1
```

### Caustic Photon Mapping Notes

When `caustics=1`, Luz emits a prepass photon map from finite emissive lights
and directional lights. Photons are traced through specular, rough metal,
dielectric, and transmissive principled transport; when they land on diffuse
receivers, the camera path gathers nearby photons as a caustic radiance estimate.
The regular path tracer still handles direct light, environment light, BSDF
sampling, volumes, and emissive-hit MIS. Increase `caustic_photons` for cleaner
caustics, and tune `caustic_radius` in meters: larger radii are smoother and more
biased, smaller radii are sharper and noisier.

### Denoising Notes

Denoising is enabled by default. It has no hard minimum resolution or sample
count, but NFOR needs enough signal to estimate useful color and feature
statistics. One sample per pixel is not a good quality target: there is no
per-pixel variance estimate, so the denoised image can look almost unchanged or
can smooth the wrong details. Use at least a few samples per pixel for quick
previews, and prefer roughly 16+ samples per pixel when judging denoiser
quality. Very low resolutions can also be misleading because each local filter
window covers too much of the image.

### Color Management Notes

Luz's renderer RGB is scene-linear ACEScg: AP1 primaries with the ACES D60 white.
Every color input is converted into that working space before rendering. Bare
triples are ACEScg values:

```text
color=(0.8,0.2,0.1)
color=acescg(0.8,0.2,0.1)
```

Use explicit source-space functions when authoring display or linear-sRGB
values:

```text
color=srgb(0.8,0.2,0.1)
color=linear_srgb(0.8,0.2,0.1)
color=wavelength(550nm)
color=blackbody(3000K)
color=solar
color=reflectance(materials/red_paint.spd)
```

`srgb(...)` values are decoded with the IEC sRGB transfer function and converted
to ACEScg. `linear_srgb(...)` skips the transfer decode but still converts
primaries. `wavelength(...)`, `blackbody(...)`, and `solar` convert through CIE
XYZ into ACEScg and are normalized chromaticities.

The default post-process path is:

```text
scene-linear ACEScg
-> exposure
-> bloom
-> selected view transform to display-linear sRGB
-> contrast
-> sRGB display encoding
```

Use `view_transform=raw` only for scene-linear debugging, measurement, or float
TIFF output. Raw is not a display/viewing transform, and PNG/BMP output rejects
raw scene-linear images. PNG and TIFF carry Luz color metadata; BMP is plain
8-bit BGR output and should be treated as display sRGB by convention.

### Environment Map Notes

Environment maps use latitude-longitude/equirectangular projection. Luz supports
PPM `P3`/`P6` files for ordinary background images and Radiance RGBE `.hdr`/`.pic`
files for HDR world lighting. PPM environment maps are treated as sRGB display
images and converted to scene-linear ACEScg. Radiance RGBE maps are treated as
linear RGB radiance and converted to ACEScg. HDR values above `1.0` are preserved
in scene-linear rendering, so they can drive bright reflections, bloom, and
diffuse illumination.

Paths are resolved like other assets: relative to the scene file, relative to
the current working directory, then under common asset directories including
`textures/` and `assets/textures/`. When `sky=environment`, the map is visible
to camera rays and specular/refraction misses. When `sky=atmosphere` and an
environment map is loaded, Luz composites the map behind the atmosphere as
`atmosphere in-scattering + atmosphere transmittance * environment radiance`.
This allows calibrated HDR horizons, interiors, or space backgrounds to coexist
with atmospheric scattering.

Environment lighting is independent from visibility. With `environment_lighting=1`
the map is sampled as an infinite light using luminance-weighted solid-angle
importance sampling and MIS, even when the visible sky is `atmosphere`. Use
`environment_lighting=0` when the map should be a camera/reflection backdrop
only. Use at most one of `environment_scale`, `environment_radiance`,
`environment_luminance`, `environment_irradiance`, or `environment_illuminance`.
For real HDRI calibration, horizontal illuminance in lux is usually the most
useful input because it ties the map to measured incident light at the capture
location.

The procedural atmosphere evaluates its compact Rayleigh/Mie channel model in
linear sRGB and converts the resulting radiance into Luz's scene-linear ACEScg
working space. This avoids treating wavelength-sampled scattering coefficients
as AP1 primaries and losing the red channel during display conversion. Earth
Rayleigh coefficients use `(5.802,13.558,33.1)e-6 1/m`, matching the production
model in [A Scalable and Production Ready Sky and Atmosphere Rendering Technique](https://doi.org/10.1111/cgf.14050).

## Scene

Each scene needs at least one named camera block:

```text
camera main {
position=(x,y,z)
direction=(dx,dy,dz)
focal_length_mm=50
sensor_width_mm=36
sensor_height_mm=24
f_stop=8
focus_distance=4
}
```

Camera position is in Luz world coordinates. Physical lens and focus quantities
are in meters, or in millimeters for fields ending in `_mm`. At render time Luz
converts `focus_distance` and lens aperture through `meters_per_unit`, so the
same camera behaves consistently when exported coordinates are scaled. Sensor
width and height define the captured gate; Luz fits that gate to the render
resolution aspect for square-pixel output. Wider renders preserve sensor width
and crop gate height; taller renders preserve sensor height and crop gate width.

| Camera Property | Format | Notes |
| --- | --- | --- |
| `position` | `position=(x,y,z)` | Camera origin in Luz world space. |
| `direction` | `direction=(x,y,z)` | Look direction. It does not need to be normalized. |
| `up` | `up=(x,y,z)` | Image-up direction used to preserve camera roll. Defaults to `(0,1,0)`. |
| `focal_length` | `focal_length=METERS` | Physical lens focal length. Defaults to `0.050`. |
| `focal_length_mm` | `focal_length_mm=MM` | Millimeter form of `focal_length`; common for camera authoring. |
| `sensor_width` | `sensor_width=METERS` | Physical sensor/gate width. Defaults to `0.036`. |
| `sensor_width_mm` | `sensor_width_mm=MM` | Millimeter form of `sensor_width`. |
| `sensor_height` | `sensor_height=METERS` | Physical sensor/gate height. Defaults to `0.02025`. |
| `sensor_height_mm` | `sensor_height_mm=MM` | Millimeter form of `sensor_height`. |
| `f_stop` | `f_stop=N` | Lens f-number. Sets aperture diameter to `focal_length / f_stop`. If `shutter` and `iso` are present, also sets scene exposure from camera controls. Aliases: `fstop`, `f_number`, `fnumber`. |
| `aperture_diameter` | `aperture_diameter=METERS` | Alternative to `f_stop`; Luz derives f-number from focal length. Do not combine with `f_stop`. |
| `aperture_diameter_mm` | `aperture_diameter_mm=MM` | Millimeter form of `aperture_diameter`. |
| `pinhole` | `pinhole=0` or `pinhole=1` | Disables thin-lens depth of field when set to `1`. |
| `focus_distance` | `focus_distance=METERS` | Physical distance to the focal plane along the camera direction. Defaults to `10`. |
| `shutter` | `shutter=SECONDS` | Optional photographic shutter time in seconds. Requires `iso`; uses the camera f-number for exposure. Aliases: `shutter_seconds`, `shutter_speed`. |
| `iso` | `iso=N` | Optional photographic ISO speed. Requires `shutter`. |

Objects are placed inside an `objects{` block:

```text
objects{
sphere=(0,1,-2),1,material[
lambertian=(0.8,0.2,0.2)
]
}
```

The parser also supports named blocks in `[materials]`, `[meshes]`, and `[scene]`.
This is the preferred target for exporters because it keeps Blender-like object,
material, mesh, camera, and light structure visible in the `.luz` file.

```text
[materials]
material brushed_metal {
type=principled
base_color=(0.75,0.72,0.68)
metallic=1
roughness=0.18
}

[meshes]
mesh helmet_mesh {
file=../objects/blender_mandalorian.obj
}

[scene]
camera main {
position=(6.2,3.8,8.2)
direction=(-6.2,-1.54,-8.2)
up=(0,1,0)
focal_length_mm=42.405
sensor_width_mm=36
sensor_height_mm=20.25
f_stop=2.8
focus_distance=10.5
}
object helmet {
mesh=helmet_mesh
position=(0,0,0)
rotation=(0,0,0)
scale=(1,1,1)
material=brushed_metal
}
area_light key {
position=(-2.5,11.0,1.5)
normal=(0,-1,0)
size=(10,8)
color=(1.0,0.86,0.62)
lumens=12000
}
directional_light sun {
direction=(0,-1,0)
solar=1
}
volume room_fog {
shape=box
position=(0,2.5,2.8)
size=(8,5,9)
density=0.05
color=(0.72,0.78,0.9)
anisotropy=0.55
}
point_light fill {
position=(3,4,5)
radius=0.1
color=(0.45,0.55,1.0)
lumens=500
visible=0
}
```

## Objects

| Object | Format |
| --- | --- |
| Sphere | `sphere=(x,y,z),radius,material[` or `sphere=(x,y,z),radius,material=NAME` |
| Named sphere | `sphere name { position=(x,y,z) radius=R material=name }` |
| Plane | `plane=y,(ox,oy,oz),material[` or `plane=y,(ox,oy,oz),material=NAME` |
| Rectangle | `rectangle=(x,y,z),(ox,oy,oz),width,height,material=NAME` or `rectangle=(x,y,z),(ox,oy,oz),(wx,wy,wz),(hx,hy,hz),width,height,uvs=(u0,v0),(u1,v1),(u2,v2),(u3,v3),material=NAME` |
| Triangle | `triangle=(x0,y0,z0),(x1,y1,z1),(x2,y2,z2),material[` or `triangle=(x0,y0,z0),(x1,y1,z1),(x2,y2,z2),material=NAME` |
| Cube | `cube=(x,y,z),(ox,oy,oz),width,height,depth,material[` or `cube=(x,y,z),(ox,oy,oz),width,height,depth,material=NAME` |
| OBJ mesh | `obj=path/to/file.obj` |
| Transformed OBJ mesh | `obj=path/to/file.obj,(x,y,z),material[` or `obj=path/to/file.obj,(x,y,z),material=NAME` |
| Volume block | `volume name { ... }` |
| Procedural cloud | `cloud name { ... }` |
| Authored sparse volume | `volume_grid name { ... }` |

Compact primitive lines can either use an inline material block with
`material[` and a closing `]`, or bind a named material from `[materials]` with
`material=NAME`. Plain `obj=path/to/file.obj` meshes use the default material.

Named sphere blocks may appear in `[scene]` or inside `objects{}`. Use them
when an analytic sphere needs a named material, including materials with
`texture=`. Sphere textures use longitude/latitude UVs generated from the
sphere normal:

```text
material earth_surface {
type=principled
base_color=(1,1,1)
roughness=0.9
texture=textures/earth_diff_jpg.ppm
}

sphere earth {
position=(0,0,0)
radius=6360000
material=earth_surface
uv_projection=latlong
}
```

Set `uv_projection=cube_cross` when the texture is a cube-cross atlas instead
of an equirectangular map. The bundled planet scene uses this for
`textures/earth_diff_jpg.ppm`.

### Volumes

Volume blocks create constant-density participating media bounded by an internal
sphere or box. They are intended for fog, mist, smoke, colored glass interiors,
and visible light shafts. The boundary is not rendered as a surface unless you
also add a normal object using the same shape.

| Volume Property | Format | Notes |
| --- | --- | --- |
| `shape` | `shape=box` or `shape=sphere` | Defaults to `box`. Alias: `type`. |
| `position` | `position=(x,y,z)` | Center of the volume. Alias: `center`. |
| `size` | `size=(width,height,depth)` | Box dimensions. Alias: `dimensions`; `width`, `height`, and `depth` are also accepted. |
| `radius` | `radius=R` | Sphere radius. |
| `density` | `density=F` | Extinction density in `1/m`; Luz converts it through `meters_per_unit` for sampling. Higher values create thicker fog. Aliases: `extinction`, `sigma_t`. |
| `color` | `color=(r,g,b)` | Scattering albedo/tint when no named phase material is used. Aliases: `albedo`, `scattering_color`. |
| `preset` | `preset=NAME` | Measured/reference homogeneous medium preset. Presets: `clear_air`, `air`, `haze`, `mist`, `fog`, `smoke`, `cloud`. Alias: `volume_preset`, `medium`. |
| `density_scale` | `density_scale=F` | Scales preset or explicit measured coefficients while preserving their scattering/absorption ratio. |
| `sigma_s` | `sigma_s=COLOR` | Scattering coefficient in `1/m`. Luz converts `sigma_s`/`sigma_a` to scalar extinction plus RGB scattering albedo for the current homogeneous volume model. Aliases: `scattering`, `scattering_coefficient`. |
| `sigma_a` | `sigma_a=COLOR` | Absorption coefficient in `1/m`. Aliases: `absorption`, `absorption_coefficient`. |
| `anisotropy` | `anisotropy=G` | Henyey-Greenstein phase parameter in `[-0.99,0.99]`. Positive values create forward-scattering godrays; `0` uses isotropic scattering. Alias: `g`. |
| `material` | `material=name` | Optional named phase material from `[materials]`. |

```text
volume sun_mist {
shape=sphere
position=(0,2,2)
radius=8
density=0.035
color=(0.85,0.9,1.0)
anisotropy=0.65
}

volume measured_haze {
shape=sphere
position=(0,2,2)
radius=8
preset=haze
density_scale=0.5
}

volume measured_medium {
shape=box
position=(0,2,0)
size=(4,3,4)
sigma_s=(0.05,0.02,0.01)
sigma_a=(0.01,0.02,0.04)
anisotropy=0.4
}
```

### Authored Sparse Volumes

`volume_grid` loads a dependency-free `.luzvol` sparse density field. This is
the preferred path for production VDB clouds, smoke simulations, and other
authored media: an industry file is converted once, then normal Luz builds parse
and render it without OpenVDB, NanoVDB, Vulkan, or any other third-party library.
`grid_volume` and `vdb_volume` are accepted as block aliases, but the runtime
file is always `.luzvol`, not `.vdb`.

The native format stores 8x8x8 bricks with per-brick 16-bit density
quantization. Empty bricks are omitted, the brick lattice is direct-indexed at
load time, and each brick carries a conservative density maximum. Camera and
continuation collisions use local-majorant delta tracking; path visibility uses
piecewise [residual ratio tracking](https://www.jannovak.info/publications/RRTracking/index.html)
with an analytic per-segment control; deterministic primary lighting uses a lower-frequency
Beer-Lambert control that is composited separately from the denoised path
residual. Repeated sun visibility is accelerated by a lazily built directional
optical-depth field: Luz sweeps the grid from the light-facing boundary,
integrates each cell with two-point Gaussian quadrature, and trilinearly queries
the result. One field is retained per directional-light direction. Each field is
hard-capped at 16 million float samples (about 64 MiB), so very large or very
sparse-domain assets reduce cache resolution instead of allocating without
bound. The swept field is used only for full-volume visibility toward an
infinite directional light. Finite point/area-light segments and stochastic
atmosphere or environment directions retain residual ratio tracking against the
source density, so an unrelated lighting direction cannot reuse the sun cache.
Production multiple-scattering
reconstruction also reads the non-exponentiated cached depth and applies a
diffusion-rate attenuation derived from absorption. Deep regions therefore keep
graded self-shadow structure after direct transmittance has underflowed, while
full `multiple_scattering_falloff=1` reference transport remains unchanged.
Continuation directions in high-albedo atmosphere-lit media use a three-way
mixture: 50% physical droplet phase, 25% phase-shaped guidance toward the
strongest directional light, and 25% phase-shaped guidance toward local sky-up.
If either guide is unavailable its weight returns to the physical proposal. The
estimator evaluates the complete mixture PDF, so guiding changes variance and
path length rather than the expected transport. This is a compact, zero-training
form of product guiding; more general radiance-field guiding follows the same
principle described by [Practical Path Guiding](https://jannovak.info/publications/PathGuide/index.html)
and Pixar's [Virtual Density Segments](https://graphics.pixar.com/library/CandidateSampling/index.html).
For scenes that enable learned guiding, the frozen radiance field becomes an
additional mixture component. Isotropic fog uses the learned field against its
uniform physical phase; droplet media retain the analytic sun/sky components as
well.

| Grid Volume Property | Format | Notes |
| --- | --- | --- |
| `file` | `file=PATH.luzvol` | Required sparse density asset. Paths resolve relative to the scene. Aliases: `source`, `path`. |
| `position` | `position=(x,y,z)` | Center in scene units. Alias: `center`. |
| `rotation` | `rotation=(x,y,z)` | Rigid Euler rotation in degrees, applied X then Y then Z around the volume center. Density lookup, ray traversal, world bounds, and directional optical-depth caches all use the rotated frame. Aliases: `rotation_degrees`, `orientation`. |
| `size` | `size=(width,height,depth)` | Placed dimensions. If omitted, Luz uses the voxel size and dimensions embedded by the converter, divided by `meters_per_unit`. Alias: `dimensions`. |
| `extinction` | `extinction=F` | Density multiplier in `1/m`, converted through `meters_per_unit`. Aliases: `density`, `density_scale`, `sigma_t`. |
| `density_threshold` | `density_threshold=F` | Morphology cutoff as a fraction of the asset's global maximum density in `[0,1)`. Values below the cutoff become empty and the remaining range is normalized back to the original peak, tightening cauliflower cores without changing peak extinction. Aliases: `threshold`, `density_cutoff`. |
| `density_gamma` | `density_gamma=F` | Contrast curve from `0.05` through `20`, applied after `density_threshold`. Values above one contract soft material around dense cores; values below one expand low-density wisps. The remap uses a monotonic 65,536-interval LUT, so rendering adds no per-sample `pow`. Alias: `density_contrast`. |
| `albedo` | `albedo=COLOR` | Single-scattering albedo. Aliases: `color`, `scattering_color`. |
| `anisotropy` | `anisotropy=G` | Primary HG anisotropy in `[-0.99,0.99]`. Alias: `g`. |
| `backscatter` | `backscatter=G` | Secondary HG anisotropy. Alias: `backscatter_anisotropy`. |
| `forward_weight` | `forward_weight=F` | Primary-lobe mixture weight. Alias: `phase_mix`. |
| `droplet_size` | `droplet_size=F` | Enables Luz's fitted HG+Draine water-droplet phase model for `5` through `50` microns; `0` uses the explicit dual-HG controls. |
| `multiple_scattering_falloff` | `multiple_scattering_falloff=F` | Per-bounce extinction falloff in `(0,1]`; `1` is full path-traced reference mode. Alias: `scatter_falloff`. |
| `multiple_scattering_compensation` | `multiple_scattering_compensation=F` | Smooth finite-order energy reconstruction in `[0,2]` when falloff is below one. Alias: `scatter_compensation`. |
| `quality` | `preview`, `production`, `cinematic`, `final`, or `reference` | Sets deterministic shadow samples per occupied brick to 2, 4, 8, 12, or 16. Primary-control detail is 0.5x, 1x, 2x, 8x, or 16x; the two highest tiers keep deterministic camera marches below the visible pixel scale of hero-cloud renders. |
| `directional_cache` | `directional_cache=0` or `1` | Default 1. Set to 0 for a cache-only comparison: integrate directional optical depth directly while preserving the reconstruction model and depth falloff. This does not enable `volume_reference`. |
| `shadow_samples_per_brick` | `shadow_samples_per_brick=N` | Explicit deterministic finite-segment and feature-guide integration budget from 1 through 32; overrides `quality`. Only full-volume infinite-directional-light shadows use the swept optical-depth field; finite lights and environment directions do not. Alias: `shadow_quality`. |
| `primary_detail` | `primary_detail=F` | Primary single-scattering control detail from 0.25 through 16. Higher values take finer camera-ray steps and a finer directional optical-depth lattice, preserving small billows and preventing visible march bands in the unfiltered deterministic layer. Camera integration defaults to a 1024-step cap (`volume_primary_max_steps` raises it) and the cache is capped at 16 million samples; overrides `quality`. Aliases: `control_detail`, `primarydetail`. |

```text
volume_grid disney_hero {
file=../../assets/volumes/wdas_cloud_half.luzvol
position=(0,5200,-14000)
rotation=(90,0,0)
size=(10500,7100,12900)
extinction=0.1575
density_threshold=0.04
density_gamma=1.15
albedo=(0.998,0.999,1.0)
droplet_size=20
multiple_scattering_falloff=0.8
multiple_scattering_compensation=0.72
quality=final
}
```

Rotation is particularly useful for simulation caches whose authored vertical
axis differs from Luz world Y. It is always a runtime rigid transform: changing
the orientation never resamples or rewrites the source field. The supplied
`examples/scenes/disney-cloud-hero-closeup.luz` instead keeps Disney's broad
native face and uses camera framing to preserve its coherent cauliflower mass.

The optional converter is intentionally outside Luz's build. With OpenVDB
available only on the conversion machine:

```sh
tools/build-vdb-converter.sh /tmp/vdb-to-luzvol
/tmp/vdb-to-luzvol input.vdb output.luzvol density
```

For [Disney's CC BY-SA 3.0 cloud dataset](https://disneyanimation.com/resources/clouds/),
download the official `wdas_cloud.zip`, extract one of the VDB resolutions, and
run the same command. The quarter field is a good interactive/look-development
asset; use the half field referenced by the sample for final fine structure.
The repository's `assets/volumes/` directory is ignored so multi-gigabyte source
and converted data cannot be committed accidentally. See
`examples/scenes/disney-cloud-hero-closeup.luz` for an atmosphere-lit hero-cloud setup. The
asset was enlarged by about 25.4x from Disney's packaged Mitsuba scene, so its
sample extinction is `4 / 25.4 = 0.1575` inverse metres; keeping the old
thin-medium value erases the characteristic cauliflower structure. The default
scene uses the bounded `0.8`/`0.72` production reconstruction. The separate
`examples/scenes/disney-cloud-reference.luz` uses falloff `1`, compensation `0`,
`volume_reference=1`, 64 light bounces, and a fixed 1024 spp without denoising
for an expensive stochastic convergence reference.

The backlit, dusk, and interior companion scenes exercise highlight latitude,
deep self-shadowing, opposite-side lighting, and a camera beginning inside
occupied density. The interior scene deliberately lowers optical scale to make
an embedded camera navigable instead of opaque within a few metres. For exposure
audits, render `--view-transform raw --exposure 0 --output audit.tiff` and inspect
the finite, unexposed scene-linear values before choosing an ACES display
exposure. To audit an existing scene's authored exposure, omit the `--exposure`
override while keeping raw float TIFF output. Also inspect the final PNG for
display clipping and digital-black shadow counts; raw values above one are HDR
headroom, not clipping. These are regression tests, not automatic-exposure
values.

### Procedural Clouds

Cloud blocks create bounded, heterogeneous participating media. Their density is
generated from a coarse convective formation field, buoyant columns, wind-sheared
hierarchical cloudlets, and a fine erosion layer, so large fields need no texture
or voxel assets. Cloudlets are binned spatially before rendering. Luz samples
collisions with delta tracking, scatters them with a dual-lobe Henyey-Greenstein
phase function, and sends the resulting paths through the same direct-light,
shadow, multiple-scattering, atmosphere, and MIS paths as other scene geometry.
A cloud therefore self-shadows and casts volumetric shadows; it is not a sky
texture or post-process.

| Cloud Property | Format | Notes |
| --- | --- | --- |
| `type` | `type=NAME` | Generation preset: `cumulus`, `stratocumulus`, `stratus`, `cirrus`, or `cumulonimbus`. `strato_cumulus` and `strato-cumulus` alias `stratocumulus`; `storm` aliases `cumulonimbus`. Alias: `preset`. Defaults to `cumulus`. |
| `position` | `position=(x,y,z)` | Center of the axis-aligned generation region. Alias: `center`. |
| `size` | `size=(width,height,depth)` | Region dimensions in scene units. Alias: `dimensions`. `width`, `height`, and `depth` override individual axes after either vector form. |
| `coverage` | `coverage=F` | Large-scale cloud coverage in `[0,1]`. Low values make isolated formations; high values make overcast layers. |
| `extinction` | `extinction=F` | Peak extinction coefficient in `1/m`. It is the delta-tracking majorant and is converted through `meters_per_unit`. Aliases: `density`, `sigma_t`. |
| `albedo` | `albedo=COLOR` | Single-scattering albedo in `[0,1]`; the near-white default preserves energy through realistic multiple scattering. Aliases: `color`, `scattering_color`. |
| `anisotropy` | `anisotropy=G` | Henyey-Greenstein phase anisotropy in `[-0.99,0.99]`. Cloud presets use strong forward scattering. Alias: `g`. |
| `backscatter` | `backscatter=G` | Secondary phase-lobe anisotropy in `[-0.99,0.99]`. Negative values add the broad backscatter response of water droplets. Alias: `backscatter_anisotropy`. |
| `forward_weight` | `forward_weight=F` | Forward-lobe mixture weight in `[0,1]`. The remaining weight uses `backscatter`. Alias: `phase_mix`. |
| `droplet_size` | `droplet_size=F` | Enables the fitted HG+Draine water-droplet phase function for radii from `5` through `50` microns. This captures the strong forward peak and broad cloud backscatter more faithfully than dual HG. `0` (the default) retains the explicit `anisotropy`, `backscatter`, and `forward_weight` controls. Alias: `droplet_size_microns`. |
| `macro_scale` | `macro_scale=F` | Coarse-formation multiplier from `0.25` through `3` for cumulus, cumulonimbus, and stratocumulus. Values above one broaden the hero tower and supporting masses without making every boundary billow larger; values below one tighten the formation. Stratus and cirrus ignore it. Defaults to `1`. Alias: `formation_scale`. |
| `feature_scale` | `feature_scale=F` | Approximate size of boundary billows and procedural detail in scene units. This is independent of `macro_scale`. Aliases: `noise_scale`, `scale`. |
| `detail` | `detail=F` | Strength of high-frequency internal structure in `[0,1]`. |
| `erosion` | `erosion=F` | Amount of high-frequency edge breakup in `[0,1]`. |
| `puffiness` | `puffiness=F` | Strength of coherent cellular billows in `[0,1]`. High values produce cauliflower-like lobes. Cirrus uses a streak field instead, so this control has no effect there. Aliases: `billowing`, `billow`. |
| `towering` | `towering=F` | Vertical growth in `[0,1]` for cumulus, cumulonimbus, and stratocumulus. It controls column height while preserving a flat condensation base; stratus and cirrus use fixed shallow profiles. Aliases: `vertical_growth`, `convection`. |
| `dominance` | `dominance=F` | Convective-only control that concentrates buoyancy into one hero thermal in `[0,1]`. High values produce one dominant tower with shorter supporting cells; stratus and cirrus ignore it. Aliases: `hero`, `thermal_dominance`. |
| `overhang` | `overhang=F` | Wind-sheared upper-crown spread in `[0,1]`. It broadens and offsets lobe-based upper caps and stretches cirrus streaks; stratus ignores it. Aliases: `crown`, `crown_spread`. |
| `fine_detail` | `fine_detail=F` | Small-scale structure in `[0,1]`. Lobe presets add tertiary/quaternary boundary cloudlets; stratus and cirrus increase the density-detail frequency. Aliases: `micro_detail`, `microdetail`. |
| `multiple_scattering_falloff` | `multiple_scattering_falloff=F` | Optional depth approximation in `(0,1]`, inspired by production cloud renderers. `1` preserves the full path-traced medium. Lower values reduce effective extinction after each volume bounce, greatly lowering deep-path variance and render time at the cost of controlled bias; values around `0.8` are useful for previews and hero-cloud production renders. Alias: `scatter_falloff`. |
| `multiple_scattering_compensation` | `multiple_scattering_compensation=F` | Finite-order deterministic energy reconstruction used when `multiple_scattering_falloff` is below `1`. It rebuilds the smooth, isotropized second-through-fifth scattering energy without blurring the separately integrated directional detail. `0` disables it, `0.72` is the default, and the valid range is `[0,2]`. Alias: `scatter_compensation`. |
| `shear_direction` | `shear_direction=(x,y,z)` | Horizontal direction of upper-crown advection and cirrus streaks. Only X/Z are used and the vector is normalized; `(0,0,0)` selects a deterministic direction from `seed`. Alias: `wind_direction`. |
| `seed` | `seed=N` | Unsigned 32-bit procedural generation seed. |
| `offset` | `offset=(x,y,z)` | Translates the noise field without moving the bounds. Use changing offsets to generate wind animation frames. Alias: `noise_offset`. |
| `quality` | `quality=preview`, `production`, or `cinematic` | Integration budget only: primary detail 0.5x, 1x or 2x and shadow/feature caps 128, 512 or 1024. `final` aliases `production`. Does not change density octaves. Explicit `primary_detail` or `max_steps` wins regardless of order. |
| `primary_detail` | `primary_detail=F` | Camera integration detail, 0.25–16; default 1. Increase with `volume_primary_max_steps` to resolve small structures. |
| `directional_cache_resolution` | `directional_cache_resolution=F` | Opt-in approximate sun/sky optical-depth cache; 0 disables it (default), 1–16 cells per feature is a practical range. Each axis is capped at 256 cells, each direction at 1,048,576 float samples and each cloud at four cached directions (16 MiB total). Further directions and finite shadow segments fall back to integration. Increase resolution and compare with cache disabled to assess interpolation error. |
| `tracking_majorants` | `tracking_majorants=0` or `1` | Default 1. Conservative cell bounds accelerate free flight and empty-density queries. Zero keeps global occupied-cell bounds and direct density evaluation for diagnostics. |
| `weather_variation` | `weather_variation=F` | Convective construction variation, 0–1, default 0. Correlates lobe sizes, growth and gaps with a broad weather field, and increases column jitter. |
| `base_variation` | `base_variation=F` | Convective condensation-base variation, 0–1, default 0. Raises and softens the base with a coherent horizontal field. |
| `detail_octaves` | `detail_octaves=N` | Explicit detail octave count from 1 through 8. Alias: `octaves`. |
| `max_steps` | `max_steps=N` | Deterministic shadow and feature-guide integration cap, from 1 through 4096. It does not cap stochastic collision tracking: camera and continuation rays delta-track until they leave the cloud, avoiding a biased early exit. Alias: `tracking_steps`. |

Preset baselines are deliberately distinct starting points; all properties above
remain independently overrideable:

| Preset | Default region | Coverage | Extinction | Structural intent |
| --- | ---: | ---: | ---: | --- |
| `cumulus` | `4000 x 1800 x 4000` | `0.45` | `0.012` | Isolated cauliflower cells and modest towers. |
| `stratocumulus` | `8000 x 1200 x 8000` | `0.68` | `0.009` | Connected low bank with cellular tops. |
| `stratus` | `12000 x 650 x 12000` | `0.88` | `0.0045` | Broad, shallow overcast layer. |
| `cirrus` | `14000 x 500 x 14000` | `0.34` | `0.0014` | Thin wind-oriented ice streaks. |
| `cumulonimbus` | `7000 x 9000 x 7000` | `0.38` | `0.016` | Deep hero convection and a sheared crown. |

```text
cloud thunderhead {
type=cumulonimbus
position=(0,4500,-12000)
size=(9000,9000,9000)
coverage=0.4
extinction=0.016
macro_scale=1.2
feature_scale=1500
dominance=0.85
overhang=0.7
fine_detail=0.9
multiple_scattering_falloff=0.8
multiple_scattering_compensation=0.72
droplet_size=20
shear_direction=(1,0,0)
seed=73
quality=cinematic
}
```

The presets establish useful physical and structural defaults, and every field
can be overridden. Use `sky=atmosphere` plus a `directional_light` with
`solar=1` for physically calibrated sky and sun lighting. On planet-scale
atmosphere scenes, cloud positions are Earth-centered just like the camera and
ground sphere. For iteration, keep the cloud parameters fixed and lower image
samples or set `quality=preview`; raise path samples and light bounces for final
multiple-scattering convergence. Camera collisions use unbiased delta tracking;
free-flight and null-acceptance samples use progressive dimensions so sparse
edges converge coherently instead of as IID salt-and-pepper noise. Volume path
continuations use a defensive mixture of the physical phase function and a
sun-oriented proposal with exact PDF compensation. This preserves the target
integral while reducing wasted paths and bounding the per-bounce guide weight.
Primary directional radiance uses a fine deterministic Beer-Lambert camera
integral and deterministic shadow transmittance, so silhouettes, silver linings,
and the recursive lobe hierarchy remain sharp at low path counts. When depth
falloff is enabled, a bounded finite-order reconstruction restores the smooth,
isotropized energy of scattering orders two through five. Authored grids use
their cached optical depth with a diffusion attenuation profile so deep shadows
retain structure instead of collapsing to a uniform fill; the stochastic path
tracer supplies the remaining directional residual. A cached equal-solid-angle
hemisphere estimate of the procedural atmosphere drives only the reconstructed
high orders, with upward cached optical depth controlling diffusion into the
cloud. This restores low-frequency blue sky fill in backlit cores without
replacing the exactly sampled environment light or softening the deterministic
sun detail. The volume denoiser filters only the diffuse residual with depth-,
density-, camera-opacity-, and direct-radiance-aware a-trous passes, then
composites the deterministic layer unfiltered. Camera opacity comes from the
same primary integration, so faint wisps below the fixed feature-hit threshold
remain volume pixels without another density march. This avoids treating
iso-density gradients as surface normals and prevents low-spp regression curves
from being embossed into the cloud. Below 64 samples the full residual filter
remains active because sparse events can underestimate variance; at 64 or more,
per-pixel mean variance reduces unnecessary wide filtering in converged regions.
With falloff `1`, reconstruction is inactive; deterministic primary integration and directional caches still apply. Set `volume_reference=1` to disable those volume transport approximations as well. See `examples/scenes/path-traced-clouds.luz` for a close-up and
`examples/scenes/path-traced-cloudscape.luz` for an aerial layered scene.

The implementation follows the macro/detail separation used by Schneider and
Vos in [The Real-time Volumetric Cloudscapes of Horizon: Zero Dawn](https://advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf),
the formation principles surveyed by Dobashi et al. in
[Visual simulation of clouds](https://doi.org/10.1016/j.visinf.2017.01.001),
and the null-collision transport framework summarized in
[Monte Carlo methods for physically based volume rendering](https://cs.dartmouth.edu/~wjarosz/publications/novak18monte-sig.html).
The bounded lighting reconstruction follows the same physical split between
directional sunlight, diffuse skylight, and low-frequency high-order transport
described in [Real-time Rendering of Endless Cloud Animation](https://doi.org/10.2312/PE/PG/PG2011short/073-076),
while its cached optical-depth strategy is informed by
[High-Performance Rendering of Realistic Cumulus Clouds Using Pre-computed Lighting](https://doi.org/10.2312/hpg.20141101)
and the spatial/angular spreading model in
[Practical Rendering of Multiple Scattering Effects in Participating Media](https://doi.org/10.2312/EGWR/EGSR04/363-374).
The optional depth-dependent extinction and anisotropy approximation, sparse
regional skipping, and HG+Draine droplet fit are informed by Zydak's
[Vulkan path-traced cloud implementation](https://zydak.github.io/Clouds/index.html).

OBJ paths use the path provided by the scene file. Absolute paths are used
as-is, and relative paths are resolved from the directory containing the
`.luz` file.

## Materials

Each material block must define exactly one material:

| Material | Format |
| --- | --- |
| Lambertian | `lambertian=(r,g,b)` |
| Metal | `metal=(r,g,b),roughness` |
| Dielectric | `dielectric=(r,g,b)` |
| Isotropic phase | `isotropic=(r,g,b)` |
| Henyey-Greenstein phase | `henyey_greenstein=(r,g,b),anisotropy` |

Color values can be ACEScg triples, explicit sRGB or linear-sRGB triples, single
wavelengths, or blackbody color temperatures:

```text
color=(0.8,0.2,0.1)
color=srgb(0.8,0.2,0.1)
color=linear_srgb(0.8,0.2,0.1)
color=wavelength(550nm)
color=blackbody(3000K)
color=solar
```

RGB channels are floating point values. Non-emissive material colors normally
use the `0.0` to `1.0` range. Bare triples and `acescg(...)` are scene-linear
ACEScg values. `srgb(...)` is for ordinary display/UI color picker values.
Spectral colors are converted through CIE 1931 color matching to normalized
scene-linear ACEScg chromaticities when the scene file is loaded. `solar` is a
5778 K solar chromaticity preset. `reflectance(PATH)`, `reflectance_curve(PATH)`,
`spectrum(PATH)`, and `spectral(PATH)` load measured spectral reflectance files
and integrate them to ACEScg. Reflectance files are plain text or CSV with one
`wavelength_nm,reflectance` sample per line; `#` starts a comment. Wavelengths
must be unique samples within `360-830 nm`, and reflectance values must be in
`[0,1]`. Paths are resolved like textures, relative to the scene file first.
Scene files can also define named reflectance curves inline before `[materials]`:

```text
[spectra]
reflectance measured_green {
400 0.092
500 0.285
600 0.160
700 0.159
}

[materials]
material painted_wall {
type=lambertian
color=reflectance(measured_green)
}
```

For `reflectance(...)`, `reflectance_curve(...)`, `spectrum(...)`, and
`spectral(...)`, Luz first checks the `[spectra]` names loaded in the current
scene file. If no name matches, the argument is treated as a file path.

Named material blocks can use the direct material lines above, or property syntax:

```text
[materials]
material glass {
type=dielectric
color=(1.0,1.0,1.0)
}
material principled_export {
type=principled
base_color=(0.8,0.2,0.1)
texture=textures/albedo.ppm
metallic=0
roughness=0.5
}
material measured_panel {
type=emissive
color=(1.0,0.86,0.62)
luminance=1200
}
material warm_fog {
type=phase
color=(1.0,0.86,0.68)
anisotropy=0.6
}
```

Named material property blocks can attach an image texture with `texture=PATH`.
Aliases are `baseColorTexture`, `base_color_texture`, and `albedo`. Texture
paths are resolved like other assets: relative to the scene file, relative to
the current working directory, then under common asset directories including
`textures/` and `assets/textures/`. Luz currently loads PPM `P3` and `P6`
texture files for base color. These textures are treated as sRGB albedo images,
decoded, converted to ACEScg, sampled with OBJ UV coordinates, and multiplied by
the material's base color. Data textures such as roughness, metallic, and normal
maps are not part of the material graph yet; when added, they must be loaded as
data with no color transform.

`type=principled` is Luz's layered surface model for Blender-style materials.
It combines energy-conserving diffuse, GGX dielectric reflection, GGX metallic
reflection, rough dielectric transmission, Burley-style subsurface scattering,
thin translucency, clearcoat, and sheen. Use
`type=metal` when you have measured conductor `eta`/`k`; use `type=dielectric`
for dedicated glass volumes with Beer-Lambert absorption.

`type=glossy` is a colored GGX reflection lobe, useful for importing Blender
Glossy BSDF nodes that are not metallic conductors. `type=diffuse_glossy`
combines Lambertian diffuse with a Glossy BSDF-style reflection lobe; it is used
by the Blender exporter for Diffuse+Glossy `Mix Shader` materials.

Principled material property blocks support:

| Property | Meaning |
| --- | --- |
| `base_color=COLOR` | Base diffuse/metal/transmission color. Alias: `color`. |
| `metallic=F` | Metallic blend in `[0,1]`. Metallic reflection uses GGX and colored Schlick Fresnel from the base color. |
| `roughness=F` | GGX roughness in `[0,1]`. |
| `transmission=F` | Rough dielectric transmission layer in `[0,1]`. |
| `ior=F` | Dielectric refractive index for Fresnel and rough refraction. Alias: `refractive_index`. |
| `glass_preset=NAME` | Measured glass preset for IOR/dispersion. Presets: `bk7`, `borosilicate`, `fused_silica`, `silica`, `water`, `diamond`, `sapphire`. Alias: `glass`. |
| `ior_wavelength=NM` | Evaluates the glass preset, Abbe model, or Sellmeier model at a specific wavelength. Defaults to the sodium d-line, `587.5618 nm`. |
| `abbe_number=F` | Approximate dispersion from `ior`/preset d-line IOR and Abbe number. Alias: `abbe`, `vd`. |
| `sellmeier_b=(b1,b2,b3)` / `sellmeier_c=(c1,c2,c3)` | Explicit Sellmeier coefficients. `c` terms are in micrometer squared. |
| `clearcoat=F` | White dielectric clearcoat layer in `[0,1]`. Aliases: `clear_coat`, `coat`. |
| `clearcoat_roughness=F` | Clearcoat GGX roughness in `[0,1]`. Aliases: `clear_coat_roughness`, `coat_roughness`. |
| `sheen=F` | Grazing-angle sheen layer in `[0,1]`. |
| `subsurface=F` | Subsurface blend in `[0,1]`. It moves energy from surface diffuse into the selected SSS profile. Aliases: `subsurface_weight`, `sss`. |
| `subsurface_method=NAME` | SSS model. `burley`/`normalized_diffusion` samples a nearby same-material exit point; `thin` uses two-sided local translucency for thin surfaces. Alias: `sss_method`. |
| `subsurface_radius=COLOR` | Per-channel relative diffusion radius. Red normally scatters farthest for skin. Alias: `sss_radius`. |
| `subsurface_scale=F` | Physical scale in meters applied to `subsurface_radius`. Use millimeter-scale values for skin. Alias: `sss_scale`. |
| `subsurface_color=COLOR` | Tint for the subsurface layer. Alias: `sss_color`. |
| `subsurface_profile=skin` | Skin defaults: Burley diffusion, radius `(1,0.35,0.18)`, scale `0.0012`, color `(1,0.42,0.28)`, and `subsurface=0.5` when no explicit value is provided. Alias: `sss_profile`. |
| `absorption=(r,g,b)` | Transmission absorption coefficient in `1/m`. Aliases: `absorption_coefficient`, `sigma_a`. |
| `transmittance=COLOR` | Alternative to `absorption`: desired transmitted color over `attenuation_distance`. |

Glossy material property blocks support:

| Property | Meaning |
| --- | --- |
| `color=COLOR` / `base_color=COLOR` | GGX reflection color. |
| `roughness=F` | GGX roughness in `[0,1]`. |

Diffuse-glossy material property blocks support:

| Property | Meaning |
| --- | --- |
| `color=COLOR` / `base_color=COLOR` | Diffuse color. |
| `glossy_color=COLOR` | Glossy reflection color. Defaults to the diffuse color. Aliases: `specular_color`. |
| `glossy_weight=F` | Mix weight for the glossy lobe in `[0,1]`. |
| `roughness=F` | Glossy GGX roughness in `[0,1]`. |

Metal material property blocks can either use RGB reflectance via `color`, or
measured conductor parameters:

| Property | Meaning |
| --- | --- |
| `roughness=F` | GGX conductor roughness in `[0,1]`; `fuzz` is accepted as an alias from compact metal syntax. |
| `preset=NAME` | Built-in measured conductor eta/k preset. Presets: `aluminum`, `copper`, `gold`, `silver`, `iron`, `nickel`, `chromium`; chemical aliases like `au`, `ag`, and `cu` are accepted. Aliases: `metal_preset`, `conductor_preset`, `conductor`. |
| `eta=(r,g,b)` | Real refractive index for conductor Fresnel. Alias: `conductor_eta`. |
| `k=(r,g,b)` | Extinction coefficient for conductor Fresnel. Aliases: `extinction`, `extinction_coefficient`, `conductor_k`. |

Dielectric material property blocks support physical glass controls:

| Property | Meaning |
| --- | --- |
| `ior=F` | Refractive index. Defaults to ordinary glass. Alias: `refractive_index`. |
| `glass_preset=NAME` | Measured glass preset for IOR/dispersion. Presets: `bk7`, `borosilicate`, `fused_silica`, `silica`, `water`, `diamond`, `sapphire`. Alias: `glass`; generic `preset=NAME` also works for dielectric materials. |
| `ior_wavelength=NM` | Evaluates the glass preset, Abbe model, or Sellmeier model at a specific wavelength. Defaults to `587.5618 nm`. |
| `abbe_number=F` | Approximate dispersion from `ior`/preset d-line IOR and Abbe number. Alias: `abbe`, `vd`. |
| `sellmeier_b=(b1,b2,b3)` / `sellmeier_c=(c1,c2,c3)` | Explicit Sellmeier coefficients. `c` terms are in micrometer squared. |
| `roughness=F` | GGX rough glass reflection/transmission roughness in `[0,1]`. |
| `absorption=(r,g,b)` | Beer-Lambert absorption coefficient in `1/m`, applied by physical path length inside the medium. Aliases: `absorption_coefficient`, `sigma_a`. |
| `transmittance=COLOR` | Alternative to `absorption`: desired medium transmittance over `attenuation_distance`. Aliases: `transmittance_color`, `attenuation`, `attenuation_color`. |
| `attenuation_distance=F` | Distance in meters used with `transmittance`. Defaults to `1.0`. Alias: `absorption_distance`. |

`type=isotropic` and `type=phase`/`type=henyey_greenstein` are intended for
volume blocks. A positive Henyey-Greenstein anisotropy favors forward scattering,
which is the useful control for fog shafts and godrays.

Emissive material property blocks require exactly one scalar unit property, with
`color` as chromaticity:

| Property | Meaning |
| --- | --- |
| `radiance=F` | Surface radiance in renderer radiance units, normalized so `luminance(color * scale) == F`. Aliases: `surface_radiance`, `emission_radiance`. |
| `luminance=F` | Surface luminance in cd/m^2, converted with 683 lm/W. Alias: `nits`. |

## Named Meshes, Objects, And Lights

Named meshes bind a mesh name to an OBJ file:

```text
[meshes]
mesh suzanne {
file=../../assets/objects/suzanne.obj
}
```

Named object blocks can reference `mesh=NAME` or provide `file=PATH` directly.
OBJ vertices are transformed with `scale`, then `rotation` in degrees around
X/Y/Z, then `position`.
OBJ `vn` normals are used for smooth shading. OBJ `vt` texture coordinates are
used when the selected material has a texture.

```text
object suzanne {
mesh=suzanne
position=(0,1,0)
rotation=(0,45,0)
scale=(1,1,1)
material=matte_red
}
```

`area_light` creates an emissive rectangle and supports arbitrary normals.
Light blocks must define exactly one unit property:

| Light | Unit properties |
| --- | --- |
| `area_light` | `radiance`, `luminance`/`nits`, `power`/`watts`, `lumens`/`luminous_flux` |
| `point_light`, `sphere_light` | `radiance`, `luminance`/`nits`, `power`/`watts`, `lumens`/`luminous_flux`, `radiant_intensity`/`w_sr`, `candela`/`cd`, or `ies` |
| `directional_light` | `irradiance`/`w_m2`, `illuminance`/`lux`, or `solar=SCALE` |

For Lambertian surface emitters, `power` is converted to radiance with
`power / (pi * area)`, where `area` is measured in square meters after applying
`meters_per_unit`. For sphere and point lights, physical area is
`4 * pi * (radius * meters_per_unit)^2`.
`lumens` and `candela` are converted through luminance using 683 lm/W. `color`
accepts the same RGB, `wavelength(NM)`, `blackbody(K)`, and `solar` values as
material colors and is treated as chromaticity for physical unit properties;
zero-luminance colors are rejected. `radiant_intensity` is W/sr for isotropic
sphere/point emitters; `candela` is lm/sr.

`directional_light` creates an infinite light whose `direction` is the direction
light travels, suitable for sun lights. When `sky=atmosphere`, the first
`directional_light` is also the atmosphere sun source: its opposite direction is
used for scattering rays toward the sun, and its emitted light value sets the
atmosphere source intensity. With `solar=SCALE`, Luz uses 1361 W/m^2 direct
solar irradiance for both surfaces and atmosphere scattering. Use
`atmosphere_sun_scale` only when you need an artistic atmosphere-only
multiplier.
If no directional light exists, the first `atmosphere=` value is used as the
vertical sun-angle fallback with the atmosphere fallback source intensity.
`point_light` and `sphere_light` create emissive spheres. These lights are still
sampled through Luz's emissive-hittable lighting path. Sphere and point lights
also accept `visible=0` to hide the light surface from camera and shadow rays
while keeping it available for direct-light sampling.

Sphere and point lights can use LM-63 IES files:

```text
point_light lamp {
position=(0,2,0)
radius=0.05
color=blackbody(3000K)
ies=fixtures/downlight.ies
ies_direction=(0,-1,0)
ies_rotation=0
visible=0
}
```

When no scalar light unit is supplied, Luz integrates the IES candela table to
derive total lumens. If a scalar unit is supplied, the IES profile shapes the
angular distribution while the scalar unit controls total output.

## Minimal Example

```text
[settings]
resolution=300,300
samples=16
maxlightbounces=6
view_transform=agx
sky=linear

[scene]
camera main {
position=(0,1,4)
direction=(0,0,-1)
focal_length_mm=31.177
sensor_width_mm=36
sensor_height_mm=36
pinhole=1
focus_distance=4
}
objects{
sphere=(0,0,-1),1,material[
lambertian=(0.8,0.2,0.2)
]
plane=-1,(0,1,0),material[
lambertian=(0.8,0.8,0.8)
]
}
```
