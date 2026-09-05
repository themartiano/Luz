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
| `adaptivebackgroundminsamples` | `adaptivebackgroundminsamples=N` | Background sample minimum; 0 inherits the global minimum, otherwise at least 2. Alias: `adaptive_background_min_samples`. |
| `adaptivevolumeminsamples` | `adaptivevolumeminsamples=N` | Volume sample minimum; 0 inherits the global minimum. Alias: `adaptive_volume_min_samples`. |
| `adaptivethreshold` | `adaptivethreshold=F` | Relative 95% confidence interval threshold for luminance convergence. Lower values render longer. Alias: `adaptive_threshold`. |
| `adaptivecheckinterval` | `adaptivecheckinterval=N` | Sample interval between adaptive convergence checks. Alias: `adaptive_check_interval`. |
| `maxlightbounces` | `maxlightbounces=N` | Maximum recursive light bounces. |
| `volume_primary_samples` | `volume_primary_samples=N` | Primary cloud lighting samples, 1–256; default 4, limited by pixel samples. |
| `volume_primary_max_steps` | `volume_primary_max_steps=N` | Camera integration cap, 48–65536; default 1024. Increase to resolve fine cloud detail. |
| `volume_reference` | `volume_reference=0` or `1` | Disable volume lighting approximations and caches; default 0. See Cloud Lighting below. |
| `volume_guiding_samples` | `volume_guiding_samples=N` | Training paths before rendering; default 0 (disabled). |
| `volume_guiding_resolution` | `volume_guiding_resolution=N` | Guide cells per axis, 1–128; default 16. Maximum allocation: 512 MiB. |
| `volume_guiding_lobes` | `volume_guiding_lobes=N` | Directions per guide cell, 4–64; default 16. |
| `volume_guiding_anisotropy` | `volume_guiding_anisotropy=F` | Guide directional concentration, 0–0.95; default 0.8. |
| `volume_guiding_strength` | `volume_guiding_strength=F` | Learned sampling weight, 0–0.75; default 0.25. |
| `volume_guiding_start_bounce` | `volume_guiding_start_bounce=N` | First guided bounce, 0–64; default 1. |
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

Adaptive sampling stops converged pixels early, up to the `samples` limit.
Use `adaptiveminsamples` for the minimum budget and `adaptivethreshold` to
control convergence. Background and volume pixels can use separate minimums;
`0` inherits the global minimum.

### Learned Volume Path Guiding Notes

Optional guiding traces training paths before rendering, then uses what it
learned to sample promising light directions. It can reduce volume noise, but
adds setup time. Leave it off unless comparisons show a benefit for your scene.
To try it, add `volume_guiding_samples=32768` to `[settings]`.

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

Map paths are resolved relative to the scene file, then the working directory
and common asset directories. `sky=environment` displays the map;
`sky=atmosphere` displays it behind the atmosphere.

`environment_lighting=1` also uses the map to illuminate the scene; set it to
`0` for a backdrop only. Choose one brightness control: `environment_scale`,
`environment_radiance`, `environment_luminance`, `environment_irradiance`, or
`environment_illuminance`.

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

`volume_grid` loads a `.luzvol` density asset for clouds or smoke. Convert VDB
files once using the optional OpenVDB tool; rendering needs no extra libraries.

For the Disney examples, download and extract the
[Disney cloud dataset](https://disneyanimation.com/resources/clouds/) (CC BY-SA 3.0),
then run from the repository root:

```sh
mkdir -p assets/volumes
tools/build-vdb-converter.sh /tmp/vdb-to-luzvol
/tmp/vdb-to-luzvol wdas_cloud_half.vdb assets/volumes/wdas_cloud_half.luzvol density
./luz examples/scenes/disney-cloud-golden-hero.luz --samples 32 --resolution 960x540
```

OpenVDB must be installed to build the converter. Keep downloaded and converted
assets in the ignored `assets/volumes/` directory.

Add a volume to `[scene]` (asset paths are relative to the scene file):

```text
volume_grid cloud {
file=../../assets/volumes/wdas_cloud_half.luzvol
position=(0,5200,-14000)
size=(10500,7100,12900)
extinction=0.1575
quality=production
}
```

| Property | Purpose |
| --- | --- |
| `file` | Path to a `.luzvol` file. |
| `position`, `size` | Center and dimensions in scene units. Omit `size` to use asset dimensions. |
| `rotation` | Euler rotation in degrees, applied X, Y, then Z. |
| `extinction` | Density multiplier in inverse meters. Higher values make a thicker cloud. |
| `density_threshold` | Remove density below a fraction of the asset maximum, from 0 to less than 1. |
| `density_gamma` | Density contrast, 0.05–20. Above 1 tightens cores; below 1 expands wisps. |
| `quality` | `preview`, `production`, `cinematic`, `final`, or `reference`; higher tiers use finer integration. |
| `directional_cache` | Cache directional-light optical depth, enabled by default. Set to `0` for direct integration. |
| `primary_detail` | Override camera integration detail, 0.25–16. Increase if fine structures show bands. |
| `shadow_samples_per_brick` | Override shadow integration samples, 1–32. |

Sparse storage skips empty space. A bounded directional cache reuses light
attenuation through the volume to speed up sun shadows. Turning it off keeps
the same lighting model; it does not enable reference transport.

### Procedural Clouds

Procedural clouds generate density without external assets. Start with a preset
and adjust its size, coverage, and seed. Add this block to `[scene]`:

```text
cloud clouds {
type=cumulus
position=(0,2000,-6000)
size=(4000,1800,4000)
coverage=0.45
seed=73
quality=production
}
```

Presets: `cumulus` (puffy towers), `stratocumulus` (low cloud banks), `stratus`
(overcast), `cirrus` (thin streaks), and `cumulonimbus` (tall storm clouds).
Each provides defaults that you can override.

| Property | Purpose |
| --- | --- |
| `position`, `size` | Center and dimensions of the generation region in scene units. |
| `coverage` | Cloud coverage, 0–1. |
| `extinction` | Peak density in inverse meters; higher values make thicker clouds. |
| `seed` | Reproducible generation seed. |
| `macro_scale` | Overall formation scale, 0.25–3; applies to convective presets. |
| `feature_scale` | Size of billows and noise in scene units. |
| `detail`, `fine_detail`, `erosion` | Internal detail, small structures, and edge breakup, each 0–1. |
| `puffiness`, `towering`, `dominance`, `overhang` | Billows, vertical growth, dominant tower, and crown spread, each 0–1. Effects depend on the preset. |
| `weather_variation`, `base_variation` | Vary formations and condensation height, each 0–1; default 0. |
| `shear_direction` | Wind direction `(x,y,z)`; uses X/Z. |
| `offset` | Shift the noise field without moving the region, useful for animation. |
| `quality` | `preview`, `production`, or `cinematic`. Changes integration accuracy, not cloud shape. |
| `directional_cache_resolution` | Optional approximate sun/sky cache. Default 0 disables it; try 4 for faster previews. |
| `primary_detail` | Camera integration detail, 0.25–16; default 1. |
| `detail_octaves` | Noise detail layers, 1–8. |
| `max_steps` | Shadow integration cap, 1–4096. |
| `tracking_majorants` | Empty-space acceleration, enabled by default; set to 0 for diagnostics. |

Use `sky=atmosphere` and a directional sun for outdoor lighting. See
[`procedural-cumulus-daylight.luz`](../examples/scenes/procedural-cumulus-daylight.luz)
for a complete scene. Lower resolution and samples for previews; increase
`primary_detail` and `volume_primary_max_steps` if camera integration shows bands.

### Cloud Lighting

Both cloud types scatter light, self-shadow, and cast shadows onto geometry.
The renderer combines integrated direct lighting with sampled multiple scattering.
Production settings approximate higher scattering orders for faster renders.

These properties work on both `cloud` and `volume_grid` blocks:

| Property | Purpose |
| --- | --- |
| `albedo` | Scattering color, with channels in 0–1. |
| `anisotropy`, `backscatter`, `forward_weight` | Directional scattering controls: two lobes in −0.99–0.99 and their blend in 0–1. |
| `droplet_size` | Water-droplet scattering model, 5–50 microns; 0 uses the lobe controls. |
| `multiple_scattering_falloff` | Per-bounce extinction reduction, greater than 0 through 1. Lower values trade accuracy for speed; 1 disables reduction. |
| `multiple_scattering_compensation` | Reconstructed diffuse fill, 0–2; default 0.72. Active when falloff is below 1. |

For a slower stochastic comparison, set `volume_reference=1` in `[settings]`,
disable denoising and adaptive sampling, and increase samples and light bounces.
This bypasses volume caches and lighting reconstruction; the atmosphere model
and finite bounce limit still apply. See
[`disney-cloud-reference.luz`](../examples/scenes/disney-cloud-reference.luz).

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

`directional_light` creates sunlight; `direction` points in the direction light
travels. With `sky=atmosphere`, the first directional light also controls the
sky's sun. `solar=1` sets irradiance to 1361 W/m²; `atmosphere_sun_scale` adjusts
only the atmosphere brightness. Without a directional light, the sun angle
comes from `atmosphere=`.

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
