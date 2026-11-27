# Spectrum

Spectrum is a collection of video filters and sources for OBS Studio. Everything that draws smooth tones works in float and dithers its output, so gradients, glows and grades don't band.

## Features

### Spectrum Blur (filter)
Blur a source using one of several algorithms, then limit it with an effect mask.

- **Algorithms:** Gaussian, Box, Dual Kawase, Pixelate, Temporal, Lens (Bokeh) and Frosted Glass.
- **Blur types:** Area, Directional, Zoom, Motion, Tilt-Shift, Vector and Depth Map, where the algorithm supports them.
- **Lens (Bokeh):** a camera-lens blur in linear light with a circle, hexagon, octagon or custom N-blade aperture (rotation, blade roundness), four quality levels, and a highlight boost that blooms bright spots into bokeh shapes (threshold and gain).
- **Depth Map:** depth of field for the Lens algorithm. The blur radius varies per pixel with a depth map taken from a source's luminance or alpha (focus distance or a focus point, focus range, falloff, max radius; out-of-focus background doesn't bleed over a sharp foreground), or from the effect mask as a smooth variable blur.
- **Frosted Glass:** a blur seen through textured glass: Frosted, Ribbed (reeded), Pebbled (bathroom) or Rippled, with grain size, scatter amount, texture angle and an optional tint.
- **Pixelate shapes:** Square, Hexagonal, Circle, Triangle, Voronoi, Rhomboid and Triakis.
- **Effect masks:** Crop, Rectangle, Circle, Source or Image.
- **Background source:** pick another source to composite the blur with.

### Spectrum Stroke, Spectrum Glow and Spectrum Shadow (filters and sources)
- **Stroke:** an inner or outer outline, or repeating contours, filled with a color, image or source.
  Strokes and contours can be **dashed** or **dotted** (square or round dash ends, offset, and an animated "marching ants" speed). Dashes follow every outline of text and logos: each outline is traced and measured along its length, and the dashes are evened out so closed outlines have no seam.
- **Corners:** draw only viewfinder-style brackets at the corners. Brackets go on a rectangle (the source frame or the box around its content), or on the corners of every outline of text and logos, found on the GPU (angle and detection radius). Arm lengths are set separately for horizontal and vertical arms, in pixels or percent, with sharp (mitred) or round bends, square, round or fading arm ends, per-corner switches, and animated pulsing and sliding along the outline.
- **Glow:** an inner or outer glow with adjustable size, intensity, blur type and fill.
- **Shadow:** an inner or outer shadow with a light angle, distance, size and fill.
- **Corner brackets** for glows and shadows too: either keep the effect only around the corners, or cast it from the corner brackets of the shape.

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
- **Mesh gradients:** a grid of 2x2 up to 5x5 color points that you can move freely. Colors blend smoothly between the points as bicubic patches, evaluated in float per pixel in the chosen color space (Oklab by default). Turn off smooth blending for straight bilinear patches.
- **Free color points:** 2 to 8 colors placed anywhere, with a softness control from near flat color areas to one wide, soft wash.
- **Point drift:** mesh and color points can wander slowly for a living background. Mesh corners stay put and edge points slide along their edge, so the frame stays covered.
- **Colors from Source:** the "Colors from Source..." button reads a frame of any source, finds its dominant colors (k-means in Oklab) and turns them into color stops, ordered dark to light, around the hue wheel or by how common they are. On a mesh or color point gradient it also recolors the points. The button is available everywhere the gradient settings appear: Stroke, Glow, Shadow, Gradient Map and Long Shadow.
- **Color stops editor:** click to add stops, drag to move them, drag down to remove them, and drag the midpoint diamonds between stops. Each segment has its own easing (linear, smooth, ease in/out, step). Includes built-in presets, and you can save your own.
- **Color blending:** Oklab (perceptual, the default), OkLCh hue paths (shorter, longer, increasing, decreasing), linear RGB or classic sRGB. Colors that would fall outside sRGB are brought back in by reducing chroma, not by clipping.
- **Extend modes:** pad, repeat and mirror, with anti-aliased seams.
- **Animation:** rotation, scrolling and hue cycling, plus a static hue shift.
- **Finish:** opacity and optional film grain.
- **No banding:** stops are baked into a 32-bit float lookup table and blended in float. Only the final output is quantised, and it gets triangular-noise dither sized to the render target (8 or 10 bit). On HDR and float canvases the gradient renders in float and needs no dither.

The same gradient engine is available as fills elsewhere: **Gradient** and **Gradient Across Stroke** in Spectrum Stroke, **Gradient** and **Gradient by Glow Strength** in Glow and Shadow, plus the Gradient Map and Long Shadow effects below.

### Spectrum Gradient Map (filter)
Maps brightness, perceptual lightness or a single channel through a gradient, for duotones, thermal looks and stylized grades. It has black/white points, gamma, 26 blend modes and an amount.

### Spectrum Color Tools (filter)
Exposure, temperature/tint, contrast, saturation and vibrance, levels, master and per-channel curves, HSL adjustments for eight color ranges, split toning, and 3D LUTs from `.cube` files.

### Spectrum Long Shadow (filter)
Flat-design long shadows or a 3D extrusion along any direction. Fill with a color, a gradient along the length, or the source's own colors darkened with depth.

### Spectrum Stylize (filter)
Chromatic aberration, glitch, CRT curvature and scanlines, mono or CMYK halftone, posterize with optional ordered dither, film grain and vignette.

### Spectrum Composite (filter)
Blends another source or an image file onto the source with 26 Photoshop-style blend modes (multiply, screen, overlay, soft/hard/vivid/linear/pin light, color dodge/burn, linear dodge/burn, hard mix, difference, exclusion, subtract, divide, hue, saturation, color, luminosity, lighter/darker color...) and an opacity. Place the layer stretched, fit, filled, tiled or at its original size, then move, scale and rotate it. Composite over the source or only inside its alpha, swap the order to put the layer underneath, and fade it with an opacity mask from another source's brightness or alpha, or from a gradient.

### Spectrum Warp (filter)
Reshapes a source in 2D and 3D. Every mode maps output pixels back to the source, so edges stay anti-aliased and squeezed areas are averaged over each pixel's footprint instead of shimmering.

- **Corner pin:** move the four corners anywhere; the mapping is a true perspective (projective) one, not a bilinear stretch.
- **Mesh warp:** a 3x3 or 4x4 grid of control points, interpolated with smooth bicubic (Catmull-Rom) curves.
- **Perspective tilt:** rotate the source in X, Y and Z with an adjustable field of view and zoom.
- **Bend:** arc the source horizontally or vertically, up to a full circle.
- **Page curl** and **roll:** peel the source back from a corner or edge, or roll it into a tube, with shading, a tinted back side and a soft shadow.
- **Extra drawing area:** lets the result draw outside the source's original bounds.

Corners and mesh points are offsets in pixels from where they start, so the defaults leave the source untouched at any size.

### Spectrum Time FX (filter)
Effects built from the source's recent frames.

- **Echo:** blends up to 16 earlier frames, a set number of frames apart, fading with age.
- **Feedback trails:** a float feedback buffer that fades each frame, with optional drift, scale and rotation per frame for tunnel and spiral trails, blended normally or with lighten, add or screen.
- **Delayed ghost:** a delayed copy of the source at an opacity, with a tint and normal, add, screen, difference, multiply or lighten blending.
- **Slit-scan:** each row, column, angled line or ring shows a different moment from a buffer of up to 240 frames, cross-faded between neighbouring frames so there are no steps.
- **Freeze / strobe:** freeze and unfreeze with a hotkey or button, or hold every Nth frame for a strobe look.

Past frames live in video memory: width x height x 4 bytes each (about 8 MB per frame at 1080p, 33 MB at 4K), capped at 240 frames or 1 GB. Everything is freed when the filter is disabled or its source is hidden, and the history restarts when the source changes size.

### Spectrum transitions
Scene transitions that blend in linear light, dither their output, keep HDR values, and crossfade the audio. Each has an easing setting (linear, smooth, ease in-out, ease in, ease out).

- **Gradient Wipe:** a soft linear, split or clock wipe at any angle, with an edge glow or border filled with a color or a gradient.
- **Shape Wipe:** an iris that opens or closes through a circle, rectangle, diamond, star, heart or polygon, around any center. It can rotate, spin while it grows, and stretch to the frame, with a soft edge and a glow or border.
- **Luma Wipe:** wipes through the brightness of any image (OBS's own luma wipe images work), any source (animated ones too) or built-in clouds, with softness, contrast, invert and an edge glow.
- **Blur Dissolve:** blurs the old scene out and the new one in, crossfading at the peak (a float Gaussian at reduced resolution, so wide blurs stay cheap).
- **Glitch:** blocky displacement, torn rows, RGB split, noise and scanlines that peak in the middle while blocks switch to the new scene.
- **Pixelate:** the pixels grow to a peak, then resolve into the new scene.
- **Noise Dissolve:** the new scene burns through animated noise with a glowing edge (a fire gradient by default), optionally travelling in one direction.

### Hotkeys and performance
- **Hotkeys:** every Spectrum filter gets Enable and Disable hotkeys (bind one key to both to toggle) in Settings > Hotkeys. Bindings are saved with the scene collection.
- **Move Transition:** when the Move Transition plugin is installed, the Blur, Mask, Stroke, Glow, Shadow, Gradient Map, Color Tools, Long Shadow, Stylize, Composite and Warp filters interpolate between two filter states.
- **Result caching:** Stroke, Glow and Shadow can cache their result for static inputs such as text and logos, so the distance field or blur isn't recomputed every frame (marching dashes keep moving on a cached stroke).

## Building

Spectrum uses the standard OBS plugin CMake build system. It needs the OBS frontend API, Qt 6 (Widgets, Svg, Xml), libcurl and nlohmann_json.

```sh
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

On Windows and macOS, use the `windows-x64` and `macos` presets. They download the OBS sources, prebuilt dependencies and Qt listed in `buildspec.json`.

## License

Spectrum is licensed under the GNU General Public License v2.0 or later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
