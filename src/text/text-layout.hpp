#pragma once

// Spectrum Text: text layout (Qt shaping, bidi and line breaking) and a
// per glyph signed distance field atlas, built on the CPU off the graphics
// thread.
//
// Each glyph outline is rasterised at 3x the atlas resolution, an exact
// Euclidean distance transform (Felzenszwalb & Huttenlocher) gives the
// distance inside and outside, and the supersampled signed distance is
// averaged down to the atlas texel. Distances are stored in font pixels
// (16-bit float), so the shader can draw outlines, glows and shadows of any
// width the atlas was built for, anti-aliased at the output size.

#include <QString>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace spectrum_text {

struct LayoutParams {
	QString text;
	bool from_file = false;
	std::string file;
	QString face;
	QString style;
	int size = 96;
	bool bold = false;
	bool italic = false;
	int align = 0; // 0 left, 1 center, 2 right
	double line_spacing = 1.0;
	double letter_spacing = 0.0;
	bool wrap = false;
	double wrap_width = 800.0;
	float spread = 4.0f; // font px of distance needed outside the glyphs
	uint64_t file_stamp = 0;
};

// A glyph in the atlas.
struct GlyphEntry {
	int ax = 0, ay = 0, aw = 0, ah = 0; // texels in the atlas
	float ox = 0.0f, oy = 0.0f;         // region top left, font px from the pen position
	float scale = 1.0f;                 // texels per font px
	float cx = 0.0f, cy = 0.0f;         // centre of the outline bounds, from the pen position
	bool empty = true;
};

// A glyph placed by the layout.
struct GlyphInstance {
	int entry = -1;
	float x = 0.0f, y = 0.0f; // pen position (baseline), layout px
	int ordinal = 0;          // typing order of its character
};

struct LayoutResult {
	std::vector<GlyphEntry> entries;
	std::vector<GlyphInstance> glyphs;
	// Caret position before each typed character, and after the last
	// (ordinal_count + 1 entries), at the baseline.
	std::vector<float> caret_x, caret_y;
	int ordinal_count = 0;
	int cursor_entry = -1;
	float width = 0.0f, height = 0.0f; // layout box, px
	QString text;                      // the laid out text
	std::vector<uint16_t> atlas;       // R16F, distances in font px
	int atlas_w = 0, atlas_h = 0;
};

// Builds layouts, caching glyph distance fields between builds with the
// same font, size and spread. Not thread safe: one builder per thread.
class Builder {
public:
	std::shared_ptr<LayoutResult> build(const LayoutParams &params);

private:
	struct Cached {
		std::vector<float> sdf;
		int w = 0, h = 0;
		GlyphEntry entry;
	};
	std::string _cacheSignature;
	std::map<std::string, Cached> _cache;
};

// True when Qt's font system can be used (a QGuiApplication exists).
bool qt_fonts_available();

} // namespace spectrum_text
