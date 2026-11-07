# Spectrum

Spectrum is a collection of video filters and sources for OBS Studio. Everything that draws smooth tones works in float and dithers its output, so gradients, glows and grades don't band.

## Features

### Spectrum Blur (filter)
Blur a source using one of several algorithms, then limit it with an effect mask.

- **Algorithms:** Gaussian, Box, Dual Kawase, Pixelate and Temporal.
- **Blur types:** Area, Directional, Zoom, Motion, Tilt-Shift and Vector, where the algorithm supports them.
- **Pixelate shapes:** Square, Hexagonal, Circle, Triangle, Voronoi, Rhomboid and Triakis.
- **Effect masks:** Crop, Rectangle, Circle, Source or Image.
- **Background source:** pick another source to composite the blur with.

### Spectrum Stroke, Spectrum Glow and Spectrum Shadow (filters and sources)
- **Stroke:** an inner or outer outline, or repeating contours, filled with a color, image or source.
- **Glow:** an inner or outer glow with adjustable size, intensity, blur type and fill.
- **Shadow:** an inner or outer shadow with a light angle, distance, size and fill.

Each one is available as a filter on a source, or as a standalone source that draws the effect from another source.

### Spectrum Mask (filter)
- **Shape masks:** rectangle (with rounded corners), circle, ellipse, regular polygon, star, heart and squircle/superformula, with feathering and scene-relative positioning.
- **Source and image masks:** use another source's alpha, a color channel or luminance as a mask.
- **Gradient**, **feather** and **SVG / icon** masks.
- **Chroma key** and an alpha-wipe mask for transitions.
- **Alpha or adjustment mode:** a mask either cuts the source out, or applies brightness, contrast, saturation and hue adjustments inside it.

### Spectrum Gradient (source)
A gradient source built to never band, with plenty of control.

- **Shapes:** linear, reflected, radial (with a movable focal point), elliptical, conic, spiral, diamond and a free-form four-corner blend.
- **Color stops editor:** click to add stops, drag to move them, drag down to remove them, and drag the midpoint diamonds between stops. Each segment has its own easing (linear, smooth, ease in/out, step). Includes built-in presets, and you can save your own.
- **Color blending:** Oklab (perceptual, the default), OkLCh hue paths (shorter, longer, increasing, decreasing), linear RGB or classic sRGB. Colors that would fall outside sRGB are brought back in by reducing chroma, not by clipping.
- **Extend modes:** pad, repeat and mirror, with anti-aliased seams.
- **Animation:** rotation, scrolling and hue cycling, plus a static hue shift.
- **Finish:** opacity and optional film grain.
- **No banding:** stops are baked into a 32-bit float lookup table and blended in float. Only the final output is quantised, and it gets triangular-noise dither sized to the render target (8 or 10 bit). On HDR and float canvases the gradient renders in float and needs no dither.

The same gradient engine is available as fills elsewhere: **Gradient** and **Gradient Across Stroke** in Spectrum Stroke, **Gradient** and **Gradient by Glow Strength** in Glow and Shadow, plus the Shape, Noise, Gradient Map and Long Shadow effects below.

### Spectrum Shape (source)
Rectangles (rounded), ellipses, polygons, stars, hearts, rings and crosses, drawn as distance fields so the edges stay crisp at any size. Shapes can be rotated, stretched and rounded, and filled with a color, a gradient or another source. Each shape has its own stroke (inside, centered or outside) and a soft drop shadow.

### Spectrum Noise (source)
Animated procedural backgrounds such as lava, plasma, aurora, fog, marble and cells. Choose value, Perlin, simplex, cell or cell-edge noise, layered as smooth fBm, turbulence or ridged noise. Add domain warping, flow and evolution, then map the result through a gradient.

### Spectrum Gradient Map (filter)
Maps brightness, perceptual lightness or a single channel through a gradient, for duotones, thermal looks and stylized grades. It has black/white points, gamma, nine blend modes and an amount.

### Spectrum Color Tools (filter)
Exposure, temperature/tint, contrast, saturation and vibrance, levels, master and per-channel curves, HSL adjustments for eight color ranges, split toning, and 3D LUTs from `.cube` files.

### Spectrum Long Shadow and Bevel & Emboss (filters)
- **Long Shadow:** flat-design long shadows or a 3D extrusion along any direction. Fill with a color, a gradient along the length, or the source's own colors darkened with depth.
- **Bevel & Emboss:** inner and outer bevels, emboss and pillow emboss, lit by a directional light.

### Spectrum Distort, Stylize and Light (filters)
- **Distort:** wave, ripple, twirl, bulge/pinch, animated noise displacement, and displacement by another source used as an offset map or as glass refraction.
- **Stylize:** chromatic aberration, glitch, CRT curvature and scanlines, mono or CMYK halftone, posterize with optional ordered dither, film grain and vignette.
- **Light:** bloom (a float blur pyramid), light rays, lens flare, and a point light that uses the source's alpha as a bump map.

### Spectrum Animate (filter)
Drives any numeric setting of any filter on a source, or of the source itself, from a looping waveform (sine, triangle, ramps, square, smooth random) or from the level of an audio source with attack and release. It works with OBS's built-in filters and with other plugins too.

### Presets, transitions and performance
- **Presets:** every Spectrum filter and source can save, load and delete named presets.
- **Move Transition:** when the Move Transition plugin is installed, the Blur, Mask, Stroke, Glow, Shadow, Gradient Map, Color Tools, Long Shadow, Bevel & Emboss, Distort and Stylize filters interpolate between two filter states.
- **Result caching:** Stroke, Glow and Shadow can cache their result for static inputs such as text and logos, so the distance field or blur isn't recomputed every frame.

## Building

Spectrum uses the standard OBS plugin CMake build system. It needs the OBS frontend API, Qt 6 (Widgets, Svg, Xml), libcurl and nlohmann_json.

```sh
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

On Windows and macOS, use the `windows-x64` and `macos` presets. They download the OBS sources, prebuilt dependencies and Qt listed in `buildspec.json`.

## License

Spectrum is licensed under the GNU General Public License v2.0 or later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
