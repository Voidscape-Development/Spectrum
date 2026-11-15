#include "text-layout.hpp"

#include <util/bmem.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QFont>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <tuple>

namespace spectrum_text {

namespace {

constexpr int kSupersample = 3;
constexpr double kInf = 1e20;
constexpr int kMaxRegion = 384;     // texels, per glyph side
constexpr int kMaxAtlasSide = 8192; // texels

// One dimensional squared distance transform (Felzenszwalb & Huttenlocher).
void edt_1d(const double *f, int n, double *d, int *v, double *z)
{
	int k = 0;
	v[0] = 0;
	z[0] = -kInf;
	z[1] = kInf;
	for (int q = 1; q < n; q++) {
		double s = ((f[q] + (double)q * q) - (f[v[k]] + (double)v[k] * v[k])) / (2.0 * q - 2.0 * v[k]);
		while (s <= z[k]) {
			k--;
			s = ((f[q] + (double)q * q) - (f[v[k]] + (double)v[k] * v[k])) / (2.0 * q - 2.0 * v[k]);
		}
		k++;
		v[k] = q;
		z[k] = s;
		z[k + 1] = kInf;
	}
	k = 0;
	for (int q = 0; q < n; q++) {
		while (z[k + 1] < q)
			k++;
		const double dq = (double)q - v[k];
		d[q] = dq * dq + f[v[k]];
	}
}

// In place 2D squared distance transform of grid (0 at features, kInf
// elsewhere).
void edt_2d(std::vector<double> &grid, int w, int h)
{
	const int n = std::max(w, h);
	std::vector<double> f(n), d(n), z(n + 1);
	std::vector<int> v(n);
	for (int x = 0; x < w; x++) {
		for (int y = 0; y < h; y++)
			f[y] = grid[(size_t)y * w + x];
		edt_1d(f.data(), h, d.data(), v.data(), z.data());
		for (int y = 0; y < h; y++)
			grid[(size_t)y * w + x] = d[y];
	}
	for (int y = 0; y < h; y++) {
		double *row = &grid[(size_t)y * w];
		std::copy(row, row + w, f.begin());
		edt_1d(f.data(), w, d.data(), v.data(), z.data());
		std::copy(d.begin(), d.begin() + w, row);
	}
}

// Distance field of one glyph outline. `scale` is texels per font px and
// `spread` the font px kept around the outline.
void build_glyph_sdf(const QPainterPath &path, float scale, float spread, std::vector<float> &out, GlyphEntry &e)
{
	const QRectF b = path.boundingRect();
	e.empty = path.isEmpty() || b.width() <= 0.0 || b.height() <= 0.0;
	e.scale = scale;
	e.cx = (float)b.center().x();
	e.cy = (float)b.center().y();
	if (e.empty)
		return;

	const int pad = (int)std::ceil(spread * scale) + 2;
	const int x0 = (int)std::floor(b.left() * scale) - pad;
	const int y0 = (int)std::floor(b.top() * scale) - pad;
	const int x1 = (int)std::ceil(b.right() * scale) + pad;
	const int y1 = (int)std::ceil(b.bottom() * scale) + pad;
	const int w = x1 - x0, h = y1 - y0;
	e.aw = w;
	e.ah = h;
	e.ox = (float)x0 / scale;
	e.oy = (float)y0 / scale;

	const int ss = kSupersample;
	const int W = w * ss, H = h * ss;
	QImage img(W, H, QImage::Format_Grayscale8);
	img.fill(0);
	{
		QPainter p(&img);
		p.setRenderHint(QPainter::Antialiasing, true);
		const double k = scale * ss;
		p.setTransform(QTransform(k, 0.0, 0.0, k, -x0 * (double)ss, -y0 * (double)ss));
		p.fillPath(path, Qt::white);
	}

	std::vector<double> to_inside((size_t)W * H), to_outside((size_t)W * H);
	for (int y = 0; y < H; y++) {
		const uchar *line = img.constScanLine(y);
		for (int x = 0; x < W; x++) {
			const bool inside = line[x] >= 128;
			to_inside[(size_t)y * W + x] = inside ? 0.0 : kInf;
			to_outside[(size_t)y * W + x] = inside ? kInf : 0.0;
		}
	}
	edt_2d(to_inside, W, H);
	edt_2d(to_outside, W, H);

	out.assign((size_t)w * h, 0.0f);
	const double norm = 1.0 / ((double)ss * ss) / ss / scale; // hi-res px -> font px
	for (int ty = 0; ty < h; ty++) {
		for (int tx = 0; tx < w; tx++) {
			double sum = 0.0;
			for (int sy = 0; sy < ss; sy++) {
				const size_t row = (size_t)(ty * ss + sy) * W;
				for (int sx = 0; sx < ss; sx++) {
					const size_t i = row + tx * ss + sx;
					if (to_inside[i] > 0.0)
						sum += std::sqrt(to_inside[i]) - 0.5;
					else
						sum -= std::sqrt(to_outside[i]) - 0.5;
				}
			}
			out[(size_t)ty * w + tx] = (float)(sum * norm);
		}
	}
}

// IEEE half float (round to nearest, tiny values flush to zero, large ones
// clamp), for the R16F atlas.
uint16_t to_half(float f)
{
	uint32_t x;
	std::memcpy(&x, &f, sizeof x);
	const uint32_t sign = (x >> 16) & 0x8000u;
	const int exp = (int)((x >> 23) & 0xFFu) - 127 + 15;
	const uint32_t mant = x & 0x7FFFFFu;
	if (exp <= 0)
		return (uint16_t)sign;
	if (exp >= 31)
		return (uint16_t)(sign | 0x7BFFu);
	uint32_t h = sign | ((uint32_t)exp << 10) | (mant >> 13);
	h += (mant >> 12) & 1u; // a carry into the exponent is still correct
	if ((h & 0x7FFFu) > 0x7BFFu)
		h = sign | 0x7BFFu;
	return (uint16_t)h;
}

std::string glyph_key(const QRawFont &font, quint32 index)
{
	return (font.familyName() + QLatin1Char('|') + font.styleName() + QLatin1Char('|') +
		QString::number(font.weight()) + QLatin1Char('|') + QString::number((int)font.style()) +
		QLatin1Char('|') + QString::number(font.pixelSize(), 'f', 2) + QLatin1Char('|') +
		QString::number(index))
		.toStdString();
}

QString read_text_file(const std::string &path)
{
	if (path.empty())
		return QString();
	char *data = os_quick_read_utf8_file(path.c_str());
	if (!data)
		return QString();
	QString text = QString::fromUtf8(data);
	bfree(data);
	return text;
}

} // namespace

bool qt_fonts_available()
{
	return qobject_cast<QGuiApplication *>(QCoreApplication::instance()) != nullptr;
}

std::shared_ptr<LayoutResult> Builder::build(const LayoutParams &params)
{
	auto result = std::make_shared<LayoutResult>();
	if (!qt_fonts_available())
		return result;

	QString text = params.from_file ? read_text_file(params.file) : params.text;
	text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
	// Trailing newlines from files only add empty lines.
	if (params.from_file)
		while (text.endsWith(QLatin1Char('\n')))
			text.chop(1);
	result->text = text;
	text.replace(QLatin1Char('\n'), QChar::LineSeparator);

	QFont font;
	if (!params.face.isEmpty())
		font.setFamily(params.face);
	font.setBold(params.bold);
	font.setItalic(params.italic);
	if (!params.style.isEmpty())
		font.setStyleName(params.style);
	const int size = std::clamp(params.size, 1, 1024);
	font.setPixelSize(size);
	font.setHintingPreference(QFont::PreferNoHinting);
	font.setLetterSpacing(QFont::AbsoluteSpacing, params.letter_spacing);

	// Atlas resolution: about 48-128 texels per em, less for very wide
	// spreads so a glyph's region stays bounded.
	const float spread = std::max(params.spread, 1.0f);
	float scale = (float)std::clamp(size, 48, 128) / (float)size;
	scale = std::min(scale, (float)kMaxRegion / ((float)size * 1.3f + 2.0f * spread));

	const std::string signature =
		font.key().toStdString() + "|" + std::to_string(scale) + "|" + std::to_string(spread);
	if (signature != _cacheSignature || _cache.size() > 4096) {
		_cache.clear();
		_cacheSignature = signature;
	}

	// Pass 1 finds the box width, pass 2 lays out and aligns inside it.
	QTextLayout layout(text, font);
	auto run_layout = [&](double line_width, bool wrap, int align) {
		QTextOption option;
		option.setWrapMode(wrap ? QTextOption::WordWrap : QTextOption::NoWrap);
		const Qt::Alignment h = align == 1 ? Qt::AlignHCenter : (align == 2 ? Qt::AlignRight : Qt::AlignLeft);
		option.setAlignment(h | Qt::AlignAbsolute);
		layout.setTextOption(option);
		layout.setCacheEnabled(true);
		layout.beginLayout();
		double y = 0.0, natural = 0.0, bottom = 0.0;
		for (;;) {
			QTextLine line = layout.createLine();
			if (!line.isValid())
				break;
			line.setLineWidth(line_width);
			line.setPosition(QPointF(0.0, y));
			natural = std::max(natural, line.naturalTextWidth());
			bottom = y + line.height();
			y += line.height() * std::max(params.line_spacing, 0.0);
		}
		layout.endLayout();
		return std::make_pair(natural, bottom);
	};
	const double first_width = params.wrap ? std::max(params.wrap_width, 1.0) : 1e6;
	const double box_width = std::ceil(run_layout(first_width, params.wrap, 0).first);
	const double box_height = run_layout(std::max(box_width, 1.0), params.wrap, params.align).second;
	result->width = (float)box_width;
	result->height = (float)std::ceil(box_height);

	// Typing order: one ordinal per character (surrogate pairs count once).
	const int len = (int)text.size();
	std::vector<int> ordinal(len + 1, 0);
	std::vector<int> ordinal_pos;
	int count = 0;
	for (int i = 0; i < len; i++) {
		if (text.at(i).isLowSurrogate() && i > 0 && text.at(i - 1).isHighSurrogate()) {
			ordinal[i] = count - 1;
			continue;
		}
		ordinal[i] = count++;
		ordinal_pos.push_back(i);
	}
	ordinal_pos.push_back(len);
	result->ordinal_count = count;

	for (int pos : ordinal_pos) {
		QTextLine line = layout.lineForTextPosition(pos);
		if (!line.isValid() && layout.lineCount() > 0)
			line = layout.lineAt(layout.lineCount() - 1);
		if (line.isValid()) {
			result->caret_x.push_back((float)line.cursorToX(pos));
			result->caret_y.push_back((float)(line.y() + line.ascent()));
		} else {
			result->caret_x.push_back(0.0f);
			result->caret_y.push_back((float)QFontMetricsF(font).ascent());
		}
	}

	// Glyphs, per character in logical order (so animations follow the
	// reading order, also for right-to-left text). Ligatures go to their
	// first character.
	std::map<std::string, int> entry_index;
	std::vector<std::string> entry_keys;
	auto entry_for = [&](const QRawFont &raw, quint32 index) -> int {
		const std::string key = glyph_key(raw, index);
		auto found = entry_index.find(key);
		if (found != entry_index.end())
			return found->second;
		auto cached = _cache.find(key);
		if (cached == _cache.end()) {
			Cached c;
			build_glyph_sdf(raw.pathForGlyph(index), scale, spread, c.sdf, c.entry);
			c.w = c.entry.aw;
			c.h = c.entry.ah;
			cached = _cache.emplace(key, std::move(c)).first;
		}
		const int idx = (int)entry_keys.size();
		entry_keys.push_back(key);
		entry_index.emplace(key, idx);
		return idx;
	};

	std::set<std::tuple<std::string, int, int>> seen;
	for (int l = 0; l < layout.lineCount(); l++) {
		QTextLine line = layout.lineAt(l);
		const int start = line.textStart(), end = start + line.textLength();
		for (int i = start; i < end; i++) {
			if (text.at(i).isLowSurrogate())
				continue;
			const int n = (i + 1 < end && text.at(i).isHighSurrogate()) ? 2 : 1;
			const QList<QGlyphRun> runs = line.glyphRuns(i, n);
			for (const QGlyphRun &run : runs) {
				const QRawFont raw = run.rawFont();
				const QList<quint32> indexes = run.glyphIndexes();
				const QList<QPointF> positions = run.positions();
				for (qsizetype g = 0; g < indexes.size() && g < positions.size(); g++) {
					const QPointF p = positions[g];
					auto id = std::make_tuple(glyph_key(raw, indexes[g]),
								  (int)std::lround(p.x() * 16.0),
								  (int)std::lround(p.y() * 16.0));
					if (!seen.insert(id).second)
						continue;
					GlyphInstance gi;
					gi.entry = entry_for(raw, indexes[g]);
					gi.x = (float)p.x();
					gi.y = (float)p.y();
					gi.ordinal = ordinal[i];
					result->glyphs.push_back(gi);
				}
			}
		}
	}

	// Typewriter cursor: a bar glyph from the font.
	{
		const QRawFont raw = QRawFont::fromFont(font);
		const QList<quint32> idx = raw.glyphIndexesForString(QStringLiteral("|"));
		if (!idx.isEmpty() && idx[0] != 0)
			result->cursor_entry = entry_for(raw, idx[0]);
	}

	// Pack the atlas (shelves, tallest first).
	std::vector<int> order(entry_keys.size());
	for (size_t i = 0; i < order.size(); i++)
		order[i] = (int)i;
	result->entries.resize(entry_keys.size());
	long long area = 0;
	int widest = 1;
	for (size_t i = 0; i < entry_keys.size(); i++) {
		const Cached &c = _cache[entry_keys[i]];
		result->entries[i] = c.entry;
		area += (long long)(c.w + 1) * (c.h + 1);
		widest = std::max(widest, c.w + 1);
	}
	std::sort(order.begin(), order.end(),
		  [&](int a, int b) { return result->entries[a].ah > result->entries[b].ah; });
	int atlas_w = std::max(widest, (int)std::ceil(std::sqrt((double)area) * 1.15));
	atlas_w = std::min(std::max(atlas_w, 16), kMaxAtlasSide);
	int x = 0, y = 0, shelf = 0;
	for (int i : order) {
		GlyphEntry &e = result->entries[i];
		if (e.empty)
			continue;
		if (x + e.aw > atlas_w) {
			x = 0;
			y += shelf + 1;
			shelf = 0;
		}
		e.ax = x;
		e.ay = y;
		x += e.aw + 1;
		shelf = std::max(shelf, e.ah);
	}
	const int atlas_h = y + shelf;
	if (atlas_h <= 0 || atlas_h > kMaxAtlasSide || atlas_w > kMaxAtlasSide) {
		// Nothing to draw (or far too much text): no atlas.
		for (GlyphEntry &e : result->entries)
			e.empty = true;
		return result;
	}
	result->atlas_w = atlas_w;
	result->atlas_h = atlas_h;
	result->atlas.assign((size_t)atlas_w * atlas_h, to_half(spread * 4.0f));
	for (size_t i = 0; i < entry_keys.size(); i++) {
		const GlyphEntry &e = result->entries[i];
		if (e.empty)
			continue;
		const Cached &c = _cache[entry_keys[i]];
		for (int row = 0; row < c.h; row++) {
			const float *src = c.sdf.data() + (size_t)row * c.w;
			uint16_t *dst = result->atlas.data() + (size_t)(e.ay + row) * atlas_w + e.ax;
			for (int col = 0; col < c.w; col++)
				dst[col] = to_half(src[col]);
		}
	}
	return result;
}

} // namespace spectrum_text
