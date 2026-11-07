# Spectrum

Spectrum is a collection of video filters and sources for OBS Studio.

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

When the Move Transition plugin is installed, the blur and mask filters can be animated between two filter states.

## Building

Spectrum uses the standard OBS plugin CMake build system. It needs the OBS frontend API, Qt 6 (Widgets, Svg, Xml), libcurl and nlohmann_json.

```sh
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

On Windows and macOS, use the `windows-x64` and `macos` presets. They download the OBS sources, prebuilt dependencies and Qt listed in `buildspec.json`.

## License

Spectrum is licensed under the GNU General Public License v2.0 or later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
