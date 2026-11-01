//----------------------------------------------------------------------------------
//	テキストの輪郭パス化 by teruyoshi
//	  - フィルタ効果「輪郭生成」: ボタンでテキストの輪郭を作り、スクリプト「輪郭パス」の設定値へ書き込む
//	  - スクリプトモジュール「TextPath_Te」: 「輪郭パス」から呼ばれ、輪郭を塗りつぶして画像にする
//----------------------------------------------------------------------------------
#include <windows.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "plugin2.h"
#include "filter2.h"
#include "module2.h"
#include "config2.h"

using Microsoft::WRL::ComPtr;

namespace {

CONFIG_HANDLE* g_config = nullptr;

const wchar_t* PLUGIN_NAME = L"輪郭生成";
const wchar_t* PATH_EFFECT = L"輪郭パス";
const wchar_t* TEXT_EFFECT = L"テキスト";
const wchar_t* ITEM_POINTS = L"座標";
const wchar_t* ITEM_HANDLES = L"ハンドル";
const wchar_t* ITEM_META = L"輪郭情報";
const wchar_t* ITEM_POINTS2 = L"座標2";
const wchar_t* ITEM_HANDLES2 = L"ハンドル2";
const wchar_t* ITEM_META2 = L"輪郭情報2";
const wchar_t* ITEM_BASE = L"元の座標";
const wchar_t* ITEM_BASE2 = L"元の座標2";
const wchar_t* ITEM_BASE_META = L"元の輪郭情報";
const wchar_t* ITEM_EDIT_SHAPE = L"編集する形状";
const wchar_t* ITEM_STROKES = L"筆順";
const wchar_t* ITEM_MORPH = L"2形状補間";
const wchar_t* ITEM_MORPH_T = L"補間";
const wchar_t* ITEM_EXPANDED = L"展開したアイランド";
const int META_VERSION = 4;
const double HANDLE_BASE_FAR = 100000;	// ハンドルの配列に入れる基準の点を画面の外へ出す量
const int META_GLYPH = 8;	// 1文字の項目数: 文字コード, 原点X, 原点Y, 点の数, 輪郭の数, インクの左端, インクの上端, 無編集の座標のハッシュ
const int META_HEADER = 10;	// 版, 無編集の座標のハッシュ, テキスト設定のハッシュ, フォントのハッシュ, サイズ×100, 文字数, インクの左端, インクの上端, アンカーの数, ハンドルの数

EDIT_HANDLE* g_edit_handle = nullptr;

struct Vec { double x, y; };

// 1つの閉じた輪郭。pointsは始点 + 辺ごとの点(直線は1個、曲線は制御点2個と終点の3個)
struct Contour {
	std::vector<Vec> points;
	std::vector<int> kinds;	// 辺ごとに 0=直線 / 1=曲線 / 2=始点へ戻る曲線(終点を持たない)
};

struct Glyph {
	UINT32 code = 0;
	Vec origin{ 0, 0 };
	std::vector<Contour> contours;
	int point_count() const {
		int n = 0;
		for (auto& c : contours) n += (int)c.points.size();
		return n;
	}
};

// DirectWriteの輪郭を受け取る
class OutlineSink : public ID2D1SimplifiedGeometrySink {
public:
	std::vector<Contour> contours;
	STDMETHOD(QueryInterface)(REFIID, void**) override { return E_NOINTERFACE; }
	STDMETHOD_(ULONG, AddRef)() override { return 1; }
	STDMETHOD_(ULONG, Release)() override { return 1; }
	STDMETHOD_(void, SetFillMode)(D2D1_FILL_MODE) override {}
	STDMETHOD_(void, SetSegmentFlags)(D2D1_PATH_SEGMENT) override {}
	STDMETHOD_(void, BeginFigure)(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN) override {
		contours.emplace_back();
		contours.back().points.push_back({ p.x, p.y });
	}
	STDMETHOD_(void, AddLines)(const D2D1_POINT_2F* p, UINT32 n) override {
		for (UINT32 i = 0; i < n; i++) {
			contours.back().points.push_back({ p[i].x, p[i].y });
			contours.back().kinds.push_back(0);
		}
	}
	STDMETHOD_(void, AddBeziers)(const D2D1_BEZIER_SEGMENT* b, UINT32 n) override {
		for (UINT32 i = 0; i < n; i++) {
			auto& c = contours.back();
			c.points.push_back({ b[i].point1.x, b[i].point1.y });
			c.points.push_back({ b[i].point2.x, b[i].point2.y });
			c.points.push_back({ b[i].point3.x, b[i].point3.y });
			c.kinds.push_back(1);
		}
	}
	STDMETHOD_(void, EndFigure)(D2D1_FIGURE_END) override {
		// 始点へ戻る最後の点は持たない(始点と同じアンカーにする)。直線は閉じる辺として暗黙に引く
		auto& c = contours.back();
		if (!c.kinds.empty() && c.points.size() >= 2) {
			auto& a = c.points.front();
			auto& b = c.points.back();
			if (std::abs(a.x - b.x) < 1e-4 && std::abs(a.y - b.y) < 1e-4) {
				c.points.pop_back();
				if (c.kinds.back() == 0) c.kinds.pop_back();
				else c.kinds.back() = 2;
			}
		}
	}
	STDMETHOD(Close)() override { return S_OK; }
};

UINT32 fnv1a(const void* data, size_t size, UINT32 hash = 2166136261u) {
	auto p = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < size; i++) hash = (hash ^ p[i]) * 16777619u;
	return hash;
}

// 座標は小数2桁で保存され、アンカーの移動で書き直されても同じ値になるよう、100倍の整数でハッシュする
UINT32 hash_points(const std::vector<double>& values) {
	UINT32 hash = 2166136261u;
	for (double v : values) {
		INT32 n = (INT32)std::lround(v * 100);
		hash = fnv1a(&n, sizeof(n), hash);
	}
	return hash;
}

std::wstring to_wide(const char* s) {
	if (!s) return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
	return w;
}

// 設定値の書式の「\n」「\\」を戻す
std::wstring unescape(const std::wstring& s) {
	std::wstring out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == L'\\' && i + 1 < s.size()) {
			i++;
			if (s[i] == L'n') out += L'\n';
			else if (s[i] == L'r') {}
			else out += s[i];
		} else {
			out += s[i];
		}
	}
	return out;
}

std::vector<double> parse_numbers(const char* s) {
	std::vector<double> out;
	if (!s) return out;
	while (*s) {
		char* end = nullptr;
		double v = strtod(s, &end);
		if (end == s) break;
		out.push_back(v);
		s = end;
		if (*s == ',') s++;
	}
	return out;
}

double item_number(EDIT_SECTION* edit, EFFECT_HANDLE effect, LPCWSTR item) {
	// 移動方法が付いている時は開始値を使う
	LPCSTR s = edit->get_effect_item_value(effect, item);
	return s ? strtod(s, nullptr) : 0.0;
}

std::wstring item_string(EDIT_SECTION* edit, EFFECT_HANDLE effect, LPCWSTR item) {
	return to_wide(edit->get_effect_item_value(effect, item));
}

// テキストの設定を読んだもの
struct TextSetting {
	std::wstring text, font, align;
	double size = 0, char_space = 0, line_space = 0;
	UINT32 hash = 0;
};

// 文字を並べて輪郭を作る。座標の原点は行の左上。本体の画像の大きさ・余白は文字の内容でも変わるので、描画時に画像のインクの位置で合わせる
bool build_glyphs(EDIT_SECTION* edit, const TextSetting& t, std::vector<Glyph>& glyphs, std::wstring& error) {
	IDWriteFont* font = edit->get_font(t.font.c_str());
	ComPtr<IDWriteFontFace> face;
	if (!font || FAILED(font->CreateFontFace(&face))) {
		error = L"フォント「" + t.font + L"」を読み込めません。";
		return false;
	}
	DWRITE_FONT_METRICS m;
	face->GetMetrics(&m);
	const double scale = t.size / m.designUnitsPerEm;
	const double line_height = (m.ascent + m.descent + m.lineGap) * scale;

	struct Line { std::vector<size_t> members; double width = 0; };
	std::vector<Line> lines(1);
	for (size_t i = 0; i < t.text.size(); i++) {
		UINT32 code = t.text[i];
		if (code == L'\n') { lines.emplace_back(); continue; }
		if (IS_HIGH_SURROGATE(t.text[i]) && i + 1 < t.text.size() && IS_LOW_SURROGATE(t.text[i + 1])) {
			code = 0x10000 + ((code - 0xD800) << 10) + (t.text[i + 1] - 0xDC00);
			i++;
		}
		UINT16 index = 0;
		if (FAILED(face->GetGlyphIndices(&code, 1, &index))) return false;
		if (!index && code != L' ' && code != 0x3000) {
			error = L"フォント「" + t.font + L"」に無い文字があります。";
			return false;
		}
		Glyph g;
		g.code = code;
		DWRITE_GLYPH_METRICS gm;
		if (FAILED(face->GetDesignGlyphMetrics(&index, 1, &gm))) return false;
		OutlineSink sink;
		if (FAILED(face->GetGlyphRunOutline((float)t.size, &index, nullptr, nullptr, 1, FALSE, FALSE, &sink))) return false;
		g.contours = std::move(sink.contours);
		g.origin.x = lines.back().width;
		lines.back().width += gm.advanceWidth * scale + t.char_space;
		lines.back().members.push_back(glyphs.size());
		glyphs.push_back(std::move(g));
	}

	double width = 0;
	for (auto& l : lines) {
		if (!l.members.empty()) l.width -= t.char_space;	// 字間は文字の間にだけ入る
		width = std::max(width, l.width);
	}
	const double pitch = line_height + t.line_space;
	const bool center = t.align.find(L"中央") == 0;
	const bool right = t.align.find(L"右") == 0;
	for (size_t row = 0; row < lines.size(); row++) {
		double shift = center ? (width - lines[row].width) / 2 : right ? width - lines[row].width : 0;
		for (size_t index : lines[row].members) {
			Glyph& g = glyphs[index];
			// 本体の文字の位置は1/8ピクセル単位(実機で測定)
			g.origin.x = std::round((g.origin.x + shift) * 8) / 8;
			g.origin.y = std::round((pitch * row + m.ascent * scale) * 8) / 8;
		}
	}
	return true;
}

std::string join_numbers(const std::vector<double>& values, const char* format) {
	std::string out;
	out.reserve(values.size() * 9);
	char buf[64];
	for (size_t i = 0; i < values.size(); i++) {
		int n = sprintf_s(buf, format, values[i]);
		if (i) out += ',';
		out.append(buf, n);
	}
	return out;
}

// 前回の結果の1文字分
struct OldGlyph {
	UINT32 code;
	Vec origin;
	size_t first, count;	// 座標の配列の中の範囲(点の単位)
	int contour_num;
	size_t block_first, block_end;	// 輪郭情報の中の、輪郭の構造の範囲
};

// 文字の並びの対応(最長共通部分列)を取る。戻り値は新しい文字ごとの前回の番号(無ければ-1)
std::vector<int> match_glyphs(const std::vector<OldGlyph>& old_glyphs, const std::vector<Glyph>& glyphs) {
	size_t a = old_glyphs.size(), b = glyphs.size();
	std::vector<std::vector<int>> table(a + 1, std::vector<int>(b + 1, 0));
	for (size_t i = a; i-- > 0;) {
		for (size_t j = b; j-- > 0;) {
			table[i][j] = old_glyphs[i].code == glyphs[j].code ? table[i + 1][j + 1] + 1 : std::max(table[i + 1][j], table[i][j + 1]);
		}
	}
	std::vector<int> result(b, -1);
	for (size_t i = 0, j = 0; i < a && j < b;) {
		if (old_glyphs[i].code == glyphs[j].code) result[j++] = (int)i++;
		else if (table[i + 1][j] >= table[i][j + 1]) i++;
		else j++;
	}
	return result;
}

// 塗りつぶし: 辺ごとの符号付き面積を積算する。重なりは1で頭打ちにする
class Rasterizer {
public:
	Rasterizer(int w, int h) : w(w), h(h), accum((size_t)(w + 1) * h + 2, 0.0f) {}
	void line(Vec a, Vec b) {
		if (a.y == b.y) return;
		double dir = 1;
		if (a.y > b.y) { std::swap(a, b); dir = -1; }
		const double dxdy = (b.x - a.x) / (b.y - a.y);
		double x = a.x;
		if (a.y < 0) x -= a.y * dxdy;
		const int y_end = std::min(h, (int)std::ceil(b.y));
		for (int y = std::max(0, (int)std::floor(a.y)); y < y_end; y++) {
			float* row = accum.data() + (size_t)y * (w + 1);
			const double dy = std::min((double)y + 1, b.y) - std::max((double)y, a.y);
			const double xnext = x + dxdy * dy;
			const double d = dy * dir;
			double x0 = std::min(x, xnext), x1 = std::max(x, xnext);
			x = xnext;
			if (x1 <= 0) { row[0] += (float)d; continue; }
			if (x0 >= w) continue;
			x0 = std::max(x0, 0.0);
			x1 = std::min(x1, (double)w);
			const int ix0 = (int)std::floor(x0);
			const int ix1 = (int)std::ceil(x1);
			if (ix1 <= ix0 + 1) {
				const double xm = (x0 + x1) / 2 - ix0;
				row[ix0] += (float)(d * (1 - xm));
				row[ix0 + 1] += (float)(d * xm);
			} else {
				const double s = 1 / (x1 - x0);
				const double x0f = x0 - ix0;
				const double a0 = 0.5 * s * (1 - x0f) * (1 - x0f);
				const double x1f = x1 - ix1 + 1;
				const double am = 0.5 * s * x1f * x1f;
				row[ix0] += (float)(d * a0);
				if (ix1 == ix0 + 2) {
					row[ix0 + 1] += (float)(d * (1 - a0 - am));
				} else {
					const double a1 = s * (1.5 - x0f);
					row[ix0 + 1] += (float)(d * (a1 - a0));
					for (int xi = ix0 + 2; xi < ix1 - 1; xi++) row[xi] += (float)(d * s);
					const double a2 = a1 + (ix1 - ix0 - 3) * s;
					row[ix1 - 1] += (float)(d * (1 - a2 - am));
				}
				row[ix1] += (float)(d * am);
			}
		}
	}
	void write(PIXEL_RGBA* out, int r, int g, int b) {
		for (int y = 0; y < h; y++) {
			float sum = 0;
			const float* row = accum.data() + (size_t)y * (w + 1);
			for (int x = 0; x < w; x++) {
				sum += row[x];
				const float cover = std::min(std::abs(sum), 1.0f);
				PIXEL_RGBA& p = out[(size_t)y * w + x];
				p.r = (unsigned char)r;
				p.g = (unsigned char)g;
				p.b = (unsigned char)b;
				p.a = (unsigned char)std::lround(cover * 255);
			}
		}
	}
private:
	int w, h;
	std::vector<float> accum;
};

void add_curve(Rasterizer& r, Vec p0, Vec p1, Vec p2, Vec p3) {
	const double span = std::abs(p1.x - p0.x) + std::abs(p1.y - p0.y) + std::abs(p2.x - p1.x) + std::abs(p2.y - p1.y) + std::abs(p3.x - p2.x) + std::abs(p3.y - p2.y);
	const int steps = std::clamp((int)std::ceil(std::sqrt(span) * 1.5), 2, 128);
	Vec prev = p0;
	for (int i = 1; i <= steps; i++) {
		const double t = (double)i / steps, u = 1 - t;
		Vec q{ u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
			u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y };
		r.line(prev, q);
		prev = q;
	}
}

// 画像のインクの左端・上端。縁の濃さから小数の位置を出す
struct Ink { double left, top; };

Ink measure_ink(const unsigned char* alpha, int w, int h, int stride, int pixel) {
	Ink ink{ -1, -1 };
	std::vector<int> column(w, 0), row(h, 0);
	for (int y = 0; y < h; y++) {
		const unsigned char* p = alpha + (size_t)y * stride;
		for (int x = 0; x < w; x++, p += pixel) {
			column[x] = std::max<int>(column[x], *p);
			row[y] = std::max<int>(row[y], *p);
		}
	}
	for (int x = 0; x < w; x++) if (column[x]) { ink.left = x + 1 - column[x] / 255.0; break; }
	for (int y = 0; y < h; y++) if (row[y]) { ink.top = y + 1 - row[y] / 255.0; break; }
	return ink;
}

// 辺の持つ点の数
int seg_size(double kind) { return kind == 0 ? 1 : kind == 1 ? 3 : 2; }

// 1つの輪郭の辺をたどる
template <typename Read, typename Line, typename Curve>
void walk_contour(const std::vector<double>& meta, size_t kinds, int seg_num, size_t point, Read read, Line on_line, Curve on_curve) {
	const Vec start = read(point);
	Vec cur = start;
	size_t i = point + 1;
	for (int s = 0; s < seg_num; s++) {
		const double kind = meta[kinds + s];
		if (kind != 0) {
			const Vec c1 = read(i), c2 = read(i + 1);
			const Vec end = kind == 1 ? read(i + 2) : start;
			on_curve(cur, c1, c2, end);
			cur = end;
			i += seg_size(kind);
		} else {
			const Vec end = read(i++);
			on_line(cur, end);
			cur = end;
		}
	}
	on_line(cur, start);
}

// 輪郭情報の輪郭をたどる。only>=0ならその番号の文字だけ処理を呼ぶ
template <typename Func>
void each_contour_meta(const std::vector<double>& meta, Func func, int only = -1) {
	size_t cursor = META_HEADER, point = 0;
	for (int g = 0; cursor + META_GLYPH <= meta.size(); g++) {
		const int contour_num = (int)meta[cursor + 4];
		cursor += META_GLYPH;
		for (int c = 0; c < contour_num && cursor < meta.size(); c++) {
			const int seg_num = (int)meta[cursor++];
			if (only < 0 || only == g) func(cursor, seg_num, point);
			for (int s = 0; s < seg_num; s++) point += seg_size(meta[cursor + s]);
			point += 1;
			cursor += seg_num;
		}
	}
}

// only番の文字の輪郭情報の位置と、点の範囲を探す。無ければfalse
bool find_glyph(const std::vector<double>& meta, int only, size_t& head, size_t& first, size_t& count) {
	size_t cursor = META_HEADER;
	first = 0;
	for (int g = 0; cursor + META_GLYPH <= meta.size(); g++) {
		head = cursor;
		count = (size_t)meta[cursor + 3];
		if (g == only) return true;
		first += count;
		const int contour_num = (int)meta[cursor + 4];
		cursor += META_GLYPH;
		for (int c = 0; c < contour_num && cursor < meta.size(); c++) cursor += 1 + (size_t)meta[cursor];
	}
	return false;
}

// 内部では「始点, 辺ごとの点(直線は終点、曲線は制御点2個と終点)」の並びで扱い、
// 設定値ではアンカー(始点と終点)とハンドル(制御点)の配列に分ける。アンカーの表示を別々に呼び、ハンドルだけ隠せるようにするため
void split_points(const std::vector<double>& points, const std::vector<double>& meta, std::vector<double>& anchors, std::vector<double>& handles) {
	each_contour_meta(meta, [&](size_t kinds, int seg_num, size_t point) {
		auto take = [&](std::vector<double>& to, size_t i) { to.push_back(points[i * 2]); to.push_back(points[i * 2 + 1]); };
		take(anchors, point);
		size_t i = point + 1;
		for (int s = 0; s < seg_num; s++) {
			const double kind = meta[kinds + s];
			if (kind != 0) { take(handles, i); take(handles, i + 1); i += 2; }
			if (kind != 2) take(anchors, i++);
		}
	});
}

// 配列の長さが輪郭情報と合わない時は空を返す
std::vector<double> merge_points(const std::vector<double>& anchors, const std::vector<double>& handles, const std::vector<double>& meta) {
	std::vector<double> points;
	size_t a = 0, h = 0;
	bool ok = true;
	auto take = [&](const std::vector<double>& from, size_t& i) {
		if (i + 2 > from.size()) { ok = false; return; }
		points.push_back(from[i]); points.push_back(from[i + 1]);
		i += 2;
	};
	each_contour_meta(meta, [&](size_t kinds, int seg_num, size_t) {
		take(anchors, a);
		for (int s = 0; s < seg_num; s++) {
			const double kind = meta[kinds + s];
			if (kind != 0) { take(handles, h); take(handles, h); }
			if (kind != 2) take(anchors, a);
		}
	});
	if (!ok) points.clear();
	return points;
}

// ハンドルの設定値は「ハンドル, 保存した時のアンカーの位置(画面の外へ出したもの), 調整用の1点(あれば)」の並び。
// アンカーを動かしてもハンドルの設定値は変わらないので、持ち主のアンカーが保存時から動いた分を足して今の位置を求める
std::vector<double> resolve_handles(const std::vector<double>& anchors, const std::vector<double>& raw, const std::vector<double>& meta) {
	const size_t anchor_num = (size_t)meta[8], handle_num = (size_t)meta[9];
	std::vector<double> handles(raw.begin(), raw.begin() + std::min(raw.size(), handle_num * 2));
	if (raw.size() < (handle_num + anchor_num) * 2 || anchors.size() < anchor_num * 2 || handles.size() < handle_num * 2) return handles;
	const double* base = raw.data() + handle_num * 2;
	size_t a = 0, h = 0;
	auto shift = [&](size_t handle, size_t owner) {
		handles[handle * 2] += anchors[owner * 2] - (base[owner * 2] - HANDLE_BASE_FAR);
		handles[handle * 2 + 1] += anchors[owner * 2 + 1] - (base[owner * 2 + 1] - HANDLE_BASE_FAR);
	};
	each_contour_meta(meta, [&](size_t kinds, int seg_num, size_t) {
		const size_t start = a++;
		size_t cur = start;
		for (int s = 0; s < seg_num; s++) {
			const double kind = meta[kinds + s];
			const size_t end = kind == 2 ? start : a++;
			if (kind != 0) { shift(h++, cur); shift(h++, end); }
			cur = end;
		}
	});
	return handles;
}

// 無編集の輪郭を塗って、インクの位置を測る (only>=0ならその番号の文字だけ)
Ink measure_pristine(const std::vector<double>& points, const std::vector<double>& meta, int only) {
	bool any = false;
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
	each_contour_meta(meta, [&](size_t kinds, int seg_num, size_t point) {
		size_t used = 1;
		for (int s = 0; s < seg_num; s++) used += seg_size(meta[kinds + s]);
		for (size_t i = point; i < point + used; i++) {
			const double x = points[i * 2], y = points[i * 2 + 1];
			if (!any) { x0 = x1 = x; y0 = y1 = y; any = true; }
			x0 = std::min(x0, x); x1 = std::max(x1, x);
			y0 = std::min(y0, y); y1 = std::max(y1, y);
		}
	}, only);
	if (!any) return { -1, -1 };
	const double ox = 2 - std::floor(x0), oy = 2 - std::floor(y0);
	const int w = (int)std::ceil(x1 + ox) + 3, h = (int)std::ceil(y1 + oy) + 3;
	auto read = [&](size_t i) { return Vec{ points[i * 2] + ox, points[i * 2 + 1] + oy }; };
	Rasterizer raster(w, h);
	each_contour_meta(meta, [&](size_t kinds, int seg_num, size_t point) {
		walk_contour(meta, kinds, seg_num, point, read,
			[&](Vec a, Vec b) { raster.line(a, b); },
			[&](Vec a, Vec c1, Vec c2, Vec b) { add_curve(raster, a, c1, c2, b); });
	}, only);
	std::vector<PIXEL_RGBA> image((size_t)w * h);
	raster.write(image.data(), 0, 0, 0);
	Ink ink = measure_ink(&image[0].a, w, h, w * 4, 4);
	if (ink.left >= 0) { ink.left -= ox; ink.top -= oy; }
	return ink;
}

UINT32 hash_glyph(const std::vector<double>& points, size_t first, size_t count) {
	std::vector<double> part(points.begin() + first * 2, points.begin() + (first + count) * 2);
	return hash_points(part);
}

// タイムラインへ展開した、1つのアイランドだけのオブジェクトか
bool is_expanded(EDIT_SECTION* edit, EFFECT_HANDLE path) {
	const std::vector<double> value = parse_numbers(edit->get_effect_item_value(path, ITEM_EXPANDED));
	return !value.empty() && value[0] > 0;
}

void show_message(LPCWSTR text) {
	MessageBoxW(g_edit_handle->get_host_app_window(), text, PLUGIN_NAME, MB_OK | MB_ICONWARNING);
}

// エフェクトの種別が「フィルタ効果」か
bool is_filter_effect(LPCWSTR name) {
	struct Find { const wchar_t* name; bool filter; } find{ name, false };
	g_edit_handle->enum_effect_name(&find, [](void* param, LPCWSTR n, int type, int) {
		auto f = static_cast<Find*>(param);
		if (wcscmp(f->name, n) == 0 && type == EDIT_HANDLE::EFFECT_TYPE_FILTER) f->filter = true;
	});
	return find.filter;
}

// 1文字の筆順。座標は輪郭と同じ。customは利用者が編集したもの(編集していない字は保存せず、筆順データから作る)
struct GlyphStrokes {
	bool custom = false;
	std::vector<std::vector<Vec>> lines;
};

// 設定値の書式: 文字数, 文字ごとに(編集していなければ-1 / 画の数, 画ごとに[点の数, x, y, …])
std::vector<GlyphStrokes> parse_strokes(const std::vector<double>& values, size_t glyph_num) {
	std::vector<GlyphStrokes> strokes(glyph_num);
	if (values.empty() || (size_t)values[0] != glyph_num) return strokes;
	size_t at = 1;
	std::vector<GlyphStrokes> parsed(glyph_num);
	for (size_t g = 0; g < glyph_num; g++) {
		if (at >= values.size()) return strokes;
		const int count = (int)values[at++];
		if (count < 0) continue;
		parsed[g].custom = true;
		for (int s = 0; s < count; s++) {
			if (at >= values.size()) return strokes;
			const size_t points = (size_t)values[at++];
			if (at + points * 2 > values.size()) return strokes;
			std::vector<Vec> line;
			for (size_t i = 0; i < points; i++, at += 2) line.push_back({ values[at], values[at + 1] });
			parsed[g].lines.push_back(std::move(line));
		}
	}
	return parsed;
}

std::string join_strokes(const std::vector<GlyphStrokes>& strokes) {
	std::vector<double> values{ (double)strokes.size() };
	for (const GlyphStrokes& glyph : strokes) {
		values.push_back(glyph.custom ? (double)glyph.lines.size() : -1);
		if (!glyph.custom) continue;
		for (const auto& line : glyph.lines) {
			values.push_back((double)line.size());
			for (const Vec& v : line) { values.push_back(v.x); values.push_back(v.y); }
		}
	}
	return join_numbers(values, "%.2f");
}

// 座標(内部の並び)と輪郭情報を設定値へ書き込む。輪郭情報のアンカーとハンドルの数はここで入れる
void write_points(EDIT_SECTION* edit, EFFECT_HANDLE path, const std::vector<double>& points, std::vector<double>& meta,
	LPCWSTR item_points, LPCWSTR item_handles, LPCWSTR item_meta) {
	std::vector<double> anchors, handles;
	split_points(points, meta, anchors, handles);
	meta[8] = (double)anchors.size() / 2;
	meta[9] = (double)handles.size() / 2;
	// ハンドルの後ろに、今のアンカーの位置を基準として入れる(resolve_handlesを参照)
	if (!handles.empty()) for (double v : anchors) handles.push_back(v + HANDLE_BASE_FAR);
	// 空の配列にすると既定値へ戻るので、無い時は1点を置く(スクリプトは輪郭情報の数だけを見る)
	if (anchors.empty()) anchors = { 0, 0 };
	if (handles.empty()) handles = { 0, 0 };
	edit->set_effect_item_value(path, item_meta, join_numbers(meta, "%.10g").c_str());
	edit->set_effect_item_value(path, item_points, join_numbers(anchors, "%.2f").c_str());
	edit->set_effect_item_value(path, item_handles, join_numbers(handles, "%.2f").c_str());
}

// 1つの形状を設定値へ書き込み、編集を引き継いだ後の座標(内部の並び)と輪郭情報を返す
// 同じフォント・サイズの前回の結果があれば、対応する文字の編集を構造ごと引き継ぐ。無ければinitial(あれば)を使う
std::vector<double> write_shape(EDIT_SECTION* edit, EFFECT_HANDLE path, const std::vector<Glyph>& glyphs, const TextSetting& t,
	LPCWSTR item_points, LPCWSTR item_handles, LPCWSTR item_meta, LPCWSTR item_base, LPCWSTR item_base_meta, bool keep_edits, UINT32 text_hash,
	const std::vector<double>* initial, const std::vector<double>* initial_meta, std::vector<double>& result_meta) {
	std::vector<double> pristine, meta(META_HEADER, 0.0);
	for (auto& g : glyphs) {
		meta.push_back(g.code);
		meta.push_back(g.origin.x);
		meta.push_back(g.origin.y);
		meta.push_back(g.point_count());
		meta.push_back((double)g.contours.size());
		meta.insert(meta.end(), { 0.0, 0.0, 0.0 });	// インクとハッシュは全ての点が揃ってから入れる
		for (auto& c : g.contours) {
			meta.push_back((double)c.kinds.size());
			meta.insert(meta.end(), c.kinds.begin(), c.kinds.end());
			for (auto& p : c.points) {
				pristine.push_back(std::round((p.x + g.origin.x) * 100) / 100);
				pristine.push_back(std::round((p.y + g.origin.y) * 100) / 100);
			}
		}
	}
	const UINT32 font_hash = fnv1a(t.font.data(), t.font.size() * sizeof(wchar_t));
	const double size_key = std::round(t.size * 100);

	std::vector<OldGlyph> old_glyphs;
	std::vector<int> matches(glyphs.size(), -1);
	bool kept = false;
	const std::vector<double> old_meta = parse_numbers(edit->get_effect_item_value(path, item_meta));
	const std::vector<double> old_anchors = parse_numbers(edit->get_effect_item_value(path, item_points));
	const std::vector<double> old_handles = parse_numbers(edit->get_effect_item_value(path, item_handles));
	const std::vector<double> old_points = old_meta.size() >= META_HEADER && old_meta[0] == META_VERSION
		? merge_points(old_anchors, resolve_handles(old_anchors, old_handles, old_meta), old_meta) : std::vector<double>();
	if (keep_edits && old_meta.size() >= META_HEADER && old_meta[0] == META_VERSION && (UINT32)old_meta[3] == font_hash && old_meta[4] == size_key) {
		size_t cursor = META_HEADER, first = 0;
		for (int i = 0; i < (int)old_meta[5] && cursor + META_GLYPH <= old_meta.size(); i++) {
			OldGlyph o{ (UINT32)old_meta[cursor], { old_meta[cursor + 1], old_meta[cursor + 2] }, first, (size_t)old_meta[cursor + 3], (int)old_meta[cursor + 4], 0, 0 };
			cursor += META_GLYPH;
			o.block_first = cursor;
			for (int c = 0; c < o.contour_num && cursor < old_meta.size(); c++) cursor += 1 + (size_t)old_meta[cursor];
			o.block_end = cursor;
			first += o.count;
			old_glyphs.push_back(o);
		}
		if (first * 2 == old_points.size()) {
			kept = true;
			matches = match_glyphs(old_glyphs, glyphs);
		}
	}

	meta[0] = META_VERSION;
	meta[1] = hash_points(pristine);
	meta[2] = text_hash;
	meta[3] = font_hash;
	meta[4] = size_key;
	meta[5] = (double)glyphs.size();
	const Ink ink = measure_pristine(pristine, meta, -1);
	meta[6] = ink.left;
	meta[7] = ink.top;
	for (int g = 0; g < (int)glyphs.size(); g++) {
		size_t head, first, count;
		if (!find_glyph(meta, g, head, first, count)) break;
		const Ink glyph_ink = measure_pristine(pristine, meta, g);
		meta[head + 5] = glyph_ink.left;
		meta[head + 6] = glyph_ink.top;
		meta[head + 7] = hash_glyph(pristine, first, count);
	}

	// 無編集の座標と構造。描画時に、動かしていない部分を本体の画像のまま残すために使う
	{
		std::vector<double> anchors, handles;
		split_points(pristine, meta, anchors, handles);
		meta[8] = (double)anchors.size() / 2;
		meta[9] = (double)handles.size() / 2;
	}
	edit->set_effect_item_value(path, item_base, pristine.empty() ? "0,0" : join_numbers(pristine, "%.2f").c_str());
	if (item_base_meta) edit->set_effect_item_value(path, item_base_meta, join_numbers(meta, "%.10g").c_str());

	// 今の構造と座標: 対応する前回の文字は構造ごと、それ以外は無編集のものを使う
	std::vector<double> points, live(meta.begin(), meta.begin() + META_HEADER);
	if (!kept && initial && initial_meta) {
		points = *initial;
		live = *initial_meta;
	} else {
		for (int j = 0; j < (int)glyphs.size(); j++) {
			size_t head, first, count;
			if (!find_glyph(meta, j, head, first, count)) break;
			const size_t next = head + META_GLYPH;
			size_t end = next;
			for (int c = 0; c < (int)meta[head + 4] && end < meta.size(); c++) end += 1 + (size_t)meta[end];
			live.insert(live.end(), meta.begin() + head, meta.begin() + next);
			if (kept && matches[j] >= 0) {
				const OldGlyph& o = old_glyphs[matches[j]];
				live[live.size() - META_GLYPH + 3] = (double)o.count;
				live[live.size() - META_GLYPH + 4] = o.contour_num;
				live.insert(live.end(), old_meta.begin() + o.block_first, old_meta.begin() + o.block_end);
				for (size_t k = 0; k < o.count; k++) {
					points.push_back(old_points[(o.first + k) * 2] + glyphs[j].origin.x - o.origin.x);
					points.push_back(old_points[(o.first + k) * 2 + 1] + glyphs[j].origin.y - o.origin.y);
				}
			} else {
				live.insert(live.end(), meta.begin() + next, meta.begin() + end);
				points.insert(points.end(), pristine.begin() + first * 2, pristine.begin() + (first + count) * 2);
			}
		}
	}
	write_points(edit, path, points, live, item_points, item_handles, item_meta);
	result_meta = live;
	// 編集した筆順も、対応する文字へ位置の差だけ動かして引き継ぐ。リセットや、フォント・サイズが変わった時は消える
	if (item_base_meta) {
		std::vector<GlyphStrokes> strokes(glyphs.size());
		if (kept) {
			const std::vector<GlyphStrokes> old_strokes = parse_strokes(parse_numbers(edit->get_effect_item_value(path, ITEM_STROKES)), old_glyphs.size());
			for (size_t j = 0; j < glyphs.size(); j++) {
				if (matches[j] < 0 || !old_strokes[matches[j]].custom) continue;
				const OldGlyph& o = old_glyphs[matches[j]];
				strokes[j] = old_strokes[matches[j]];
				for (auto& line : strokes[j].lines) {
					for (Vec& v : line) { v.x += glyphs[j].origin.x - o.origin.x; v.y += glyphs[j].origin.y - o.origin.y; }
				}
			}
		}
		edit->set_effect_item_value(path, ITEM_STROKES, join_strokes(strokes).c_str());
	}
	return points;
}

// テキスト設定のハッシュ。スクリプトが読める値(obj.getvalueの時刻0の値)と同じ文字列で作る
// (返される文字列は次の呼び出しで無効になるので、1回ごとにコピーする)
UINT32 text_setting_hash(EDIT_SECTION* edit, EFFECT_HANDLE text_effect) {
	std::string key = edit->get_effect_item_value(text_effect, L"テキスト");
	key += "|";
	key += edit->get_effect_item_value(text_effect, L"フォント");
	char numbers[128];
	sprintf_s(numbers, "|%.2f|%.2f|%.2f", item_number(edit, text_effect, L"サイズ"), item_number(edit, text_effect, L"字間"), item_number(edit, text_effect, L"行間"));
	key += numbers;
	key += "|";
	key += edit->get_effect_item_value(text_effect, L"文字揃え");
	return fnv1a(key.data(), key.size());
}

void generate(EDIT_SECTION* edit, OBJECT_HANDLE object, LPCWSTR effect, bool keep_edits) {
	// 右クリックのメニューから呼ばれた時は、輪郭生成が付いていないことがある(selfがnullptr)
	EFFECT_HANDLE self = edit->find_effect(object, effect);
	int effect_num = edit->get_effect_list(object, nullptr, 0);
	std::vector<EFFECT_HANDLE> list(effect_num);
	if (edit->get_effect_list(object, list.data(), effect_num) != effect_num) return;

	EFFECT_HANDLE text_effect = edit->find_effect(object, TEXT_EFFECT);
	if (!text_effect) { show_message(L"テキストオブジェクトに追加してください。"); return; }

	TextSetting t;
	t.text = unescape(item_string(edit, text_effect, L"テキスト"));
	t.font = item_string(edit, text_effect, L"フォント");
	t.align = item_string(edit, text_effect, L"文字揃え");
	t.size = item_number(edit, text_effect, L"サイズ");
	t.char_space = item_number(edit, text_effect, L"字間");
	t.line_space = item_number(edit, text_effect, L"行間");

	if (t.text.find(L'<') != std::wstring::npos) { show_message(L"制御文字を含むテキストには対応していません。"); return; }
	if (item_string(edit, text_effect, L"文字装飾") != L"標準文字" || item_number(edit, text_effect, L"B") != 0 || item_number(edit, text_effect, L"I") != 0) {
		show_message(L"文字装飾が「標準文字」で、太字・斜体でないテキストのみ対応しています。");
		return;
	}
	if (t.align.find(L"縦") != std::wstring::npos) { show_message(L"縦書きには対応していません。"); return; }

	std::vector<Glyph> glyphs;
	std::wstring error;
	if (!build_glyphs(edit, t, glyphs, error)) { show_message(error.empty() ? L"輪郭を取得できません。" : error.c_str()); return; }

	EFFECT_HANDLE path = nullptr;
	for (int i = 0; i < effect_num; i++) {
		if (wcsncmp(edit->get_effect_name(list[i]), PATH_EFFECT, wcslen(PATH_EFFECT)) == 0) { path = list[i]; break; }
	}
	// 作り直すと字全体の輪郭へ戻ってしまう
	if (path && is_expanded(edit, path)) { show_message(L"アイランドごとに展開したオブジェクトは、輪郭を作り直せません。"); return; }

	// 基準のエフェクト(輪郭生成、無ければ輪郭パス)より前にフィルタ効果があれば、確認してから先頭へ移す
	EFFECT_HANDLE head = self ? self : path;
	int head_index = 0, first_filter = -1;
	for (int i = 0; head && i < effect_num; i++) {
		if (list[i] == head) { head_index = i; break; }
		if (is_filter_effect(edit->get_effect_name(list[i])) && first_filter < 0) first_filter = i;
	}
	if (first_filter >= 0) {
		const std::wstring question = std::wstring(self ? PLUGIN_NAME : PATH_EFFECT) + L"より前にフィルタ効果があります。\n" +
			(self ? PLUGIN_NAME : PATH_EFFECT) + L"を先頭のフィルタ効果へ移動して続けますか?";
		int answer = MessageBoxW(g_edit_handle->get_host_app_window(), question.c_str(), PLUGIN_NAME, MB_OKCANCEL | MB_ICONWARNING);
		if (answer != IDOK) return;
		head_index = edit->move_effect(object, head, first_filter);
	}

	// 輪郭パスは輪郭生成の直後に置く。輪郭生成が無ければ、新しく作る時だけ先頭のフィルタ効果の前に置く
	if (!path) {
		int before_filter = effect_num;
		for (int i = effect_num; !self && i-- > 0;) {
			if (is_filter_effect(edit->get_effect_name(list[i]))) before_filter = i;
		}
		path = edit->create_effect(object, PATH_EFFECT);
		if (!path) { show_message(L"スクリプト「輪郭パス」が見つかりません。"); return; }
		edit->move_effect(object, path, self ? head_index + 1 : before_filter);
	} else if (self) {
		edit->move_effect(object, path, head_index + 1);
	}

	const UINT32 text_hash = text_setting_hash(edit, text_effect);

	std::vector<double> meta1, meta2;
	const std::vector<double> points1 = write_shape(edit, path, glyphs, t, ITEM_POINTS, ITEM_HANDLES, ITEM_META, ITEM_BASE, ITEM_BASE_META, keep_edits, text_hash, nullptr, nullptr, meta1);

	// 形状2: サイズ・字間・行間に移動方法があれば終了値から作り、補間のトラックバーへ同じ移動方法を設定する
	TextSetting t2 = t;
	std::string motion;
	auto read_end = [&](LPCWSTR item, double& value) {
		const std::string s = edit->get_effect_item_value(text_effect, item);
		const std::vector<double> numbers2 = parse_numbers(s.c_str());
		if (numbers2.size() != 2) return;	// 移動無し、または中間点あり(中間点は扱わない)
		value = numbers2[1];
		if (motion.empty()) {
			const size_t comma = s.find(',', s.find(',') + 1);
			if (comma != std::string::npos) motion = s.substr(comma);
		}
	};
	read_end(L"サイズ", t2.size);
	read_end(L"字間", t2.char_space);
	read_end(L"行間", t2.line_space);
	std::vector<Glyph> glyphs2;
	if (motion.empty()) {
		glyphs2 = glyphs;
	} else if (!build_glyphs(edit, t2, glyphs2, error)) {
		show_message(error.empty() ? L"輪郭を取得できません。" : error.c_str());
		return;
	}
	write_shape(edit, path, glyphs2, t2, ITEM_POINTS2, ITEM_HANDLES2, ITEM_META2, ITEM_BASE2, nullptr, keep_edits, text_hash,
		motion.empty() ? &points1 : nullptr, motion.empty() ? &meta1 : nullptr, meta2);
	if (!motion.empty()) {
		edit->set_effect_item_value(path, ITEM_MORPH_T, ("0.00,100.00" + motion).c_str());
		edit->set_effect_item_value(path, ITEM_MORPH, "1");
	}
}

template <bool KeepEdits>
void on_button(EDIT_SECTION* edit, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR) {
	try {
		generate(edit, object, effect, KeepEdits);
	} catch (const std::exception&) {
		show_message(L"輪郭の作成に失敗しました。");
	} catch (...) {
		show_message(L"輪郭の作成に失敗しました。");
	}
}

//---------------------------------------------------------------------
//	高度な編集: 輪郭の構造を編集するウィンドウ
//---------------------------------------------------------------------

Vec lerp(Vec a, Vec b, double t) { return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t }; }
double distance(Vec a, Vec b) { return std::hypot(a.x - b.x, a.y - b.y); }
bool same_point(Vec a, Vec b) { return std::abs(a.x - b.x) < 0.005 && std::abs(a.y - b.y) < 0.005; }

// 位置は形状ごとに持つ。構造の編集は常に両方の形状へ適用する
struct EdSeg {	// 次のアンカーへの辺
	bool curve = false;
	Vec c1[2]{}, c2[2]{};
};
struct EdNode {
	Vec p[2]{};
	EdSeg next;
};
struct EdContour { std::vector<EdNode> nodes; };
struct EdGlyph {
	std::vector<double> head;
	std::vector<EdContour> contours;
};
using EdModel = std::vector<EdGlyph>;

// 座標と輪郭情報から読む。位置はsの形状へ入れる
EdModel load_model(const std::vector<double>& points, const std::vector<double>& meta, int s) {
	EdModel model;
	size_t cursor = META_HEADER, point = 0;
	auto read = [&](size_t i) { return Vec{ points[i * 2], points[i * 2 + 1] }; };
	for (int g = 0; g < (int)meta[5] && cursor + META_GLYPH <= meta.size(); g++) {
		EdGlyph glyph;
		glyph.head.assign(meta.begin() + cursor, meta.begin() + cursor + META_GLYPH);
		const int contour_num = (int)meta[cursor + 4];
		cursor += META_GLYPH;
		for (int c = 0; c < contour_num && cursor < meta.size(); c++) {
			const int seg_num = (int)meta[cursor++];
			EdContour contour;
			EdNode start;
			start.p[s] = read(point++);
			contour.nodes.push_back(start);
			for (int k = 0; k < seg_num; k++) {
				const double kind = meta[cursor + k];
				EdNode& cur = contour.nodes.back();
				if (kind != 0) {
					cur.next.curve = true;
					cur.next.c1[s] = read(point++);
					cur.next.c2[s] = read(point++);
				}
				if (kind != 2) {
					EdNode node;
					node.p[s] = read(point++);
					contour.nodes.push_back(node);
				}
			}
			cursor += seg_num;
			glyph.contours.push_back(std::move(contour));
		}
		model.push_back(std::move(glyph));
	}
	return model;
}

// 形状2の位置を入れる。構造が違う時はfalse
bool merge_second(EdModel& model, const EdModel& second) {
	if (model.size() != second.size()) return false;
	for (size_t g = 0; g < model.size(); g++) {
		if (model[g].contours.size() != second[g].contours.size()) return false;
		for (size_t c = 0; c < model[g].contours.size(); c++) {
			if (model[g].contours[c].nodes.size() != second[g].contours[c].nodes.size()) return false;
			for (size_t n = 0; n < model[g].contours[c].nodes.size(); n++) {
				if (model[g].contours[c].nodes[n].next.curve != second[g].contours[c].nodes[n].next.curve) return false;
			}
		}
	}
	for (size_t g = 0; g < model.size(); g++) {
		for (size_t c = 0; c < model[g].contours.size(); c++) {
			for (size_t n = 0; n < model[g].contours[c].nodes.size(); n++) {
				EdNode& a = model[g].contours[c].nodes[n];
				const EdNode& b = second[g].contours[c].nodes[n];
				a.p[1] = b.p[1];
				a.next.c1[1] = b.next.c1[1];
				a.next.c2[1] = b.next.c2[1];
			}
		}
	}
	return true;
}

void copy_first_to_second(EdModel& model) {
	for (auto& g : model) for (auto& c : g.contours) for (auto& n : c.nodes) {
		n.p[1] = n.p[0];
		n.next.c1[1] = n.next.c1[0];
		n.next.c2[1] = n.next.c2[0];
	}
}

// sの形状の座標(内部の並び)と輪郭情報にする。headerは輪郭情報の先頭(版など)
void save_model(const EdModel& model, int s, const std::vector<double>& header, std::vector<double>& points, std::vector<double>& meta) {
	points.clear();
	meta.assign(header.begin(), header.begin() + META_HEADER);
	auto put = [&](Vec v) { points.push_back(v.x); points.push_back(v.y); };
	for (const EdGlyph& glyph : model) {
		const size_t head = meta.size();
		const size_t first = points.size();
		meta.insert(meta.end(), glyph.head.begin(), glyph.head.end());
		for (const EdContour& contour : glyph.contours) {
			const size_t count_at = meta.size();
			meta.push_back(0);
			const size_t n = contour.nodes.size();
			put(contour.nodes[0].p[s]);
			for (size_t i = 0; i < n; i++) {
				const EdSeg& seg = contour.nodes[i].next;
				const bool last = i + 1 == n;
				if (seg.curve) {
					put(seg.c1[s]);
					put(seg.c2[s]);
					if (!last) put(contour.nodes[i + 1].p[s]);
					meta.push_back(last ? 2 : 1);
				} else if (!last) {
					put(contour.nodes[i + 1].p[s]);
					meta.push_back(0);
				}
			}
			meta[count_at] = (double)(meta.size() - count_at - 1);
		}
		meta[head + 3] = (double)(points.size() - first) / 2;
		meta[head + 4] = (double)glyph.contours.size();
	}
}

std::vector<Vec> flatten_contour(const EdContour& contour, int s) {
	std::vector<Vec> line;
	const size_t size = contour.nodes.size();
	for (size_t i = 0; i < size; i++) {
		const EdNode& a = contour.nodes[i];
		const EdNode& b = contour.nodes[(i + 1) % size];
		line.push_back(a.p[s]);
		for (int k = 1; a.next.curve && k < 8; k++) {
			const double t = k / 8.0, u = 1 - t;
			line.push_back({ u * u * u * a.p[s].x + 3 * u * u * t * a.next.c1[s].x + 3 * u * t * t * a.next.c2[s].x + t * t * t * b.p[s].x,
				u * u * u * a.p[s].y + 3 * u * u * t * a.next.c1[s].y + 3 * u * t * t * a.next.c2[s].y + t * t * t * b.p[s].y });
		}
	}
	return line;
}

double signed_area(const std::vector<Vec>& line) {
	double area = 0;
	for (size_t i = 0; i < line.size(); i++) {
		const Vec& a = line[i];
		const Vec& b = line[(i + 1) % line.size()];
		area += a.x * b.y - b.x * a.y;
	}
	return area / 2;
}

bool inside_polygon(const std::vector<Vec>& line, Vec v) {
	bool inside = false;
	for (size_t i = 0, j = line.size() - 1; i < line.size(); j = i++) {
		if ((line[i].y > v.y) != (line[j].y > v.y) && v.x < line[i].x + (line[j].x - line[i].x) * (v.y - line[i].y) / (line[j].y - line[i].y)) inside = !inside;
	}
	return inside;
}

// 輪郭ごとのアイランドの番号。アイランドは外周とその中の穴の組で、向きが最大の輪郭と逆のものを穴とみなす
std::vector<int> contour_islands(const EdGlyph& glyph, int s) {
	const int count = (int)glyph.contours.size();
	std::vector<std::vector<Vec>> lines(count);
	std::vector<double> areas(count);
	double largest = 0;
	for (int i = 0; i < count; i++) {
		lines[i] = flatten_contour(glyph.contours[i], s);
		areas[i] = signed_area(lines[i]);
		if (std::abs(areas[i]) > std::abs(largest)) largest = areas[i];
	}
	std::vector<int> owner(count);
	for (int i = 0; i < count; i++) {
		owner[i] = i;
		if (areas[i] * largest >= 0) continue;
		// 穴の点の過半数を含む外周のうち、最も小さいものを持ち主にする
		double best = 0;
		for (int k = 0; k < count; k++) {
			if (k == i || areas[k] * largest <= 0) continue;
			size_t in = 0;
			for (const Vec& v : lines[i]) if (inside_polygon(lines[k], v)) in++;
			if (in * 2 > lines[i].size() && (best == 0 || std::abs(areas[k]) < best)) {
				best = std::abs(areas[k]);
				owner[i] = k;
			}
		}
	}
	std::vector<int> islands(count, -1);
	int next = 0;
	for (int i = 0; i < count; i++) {
		if (islands[owner[i]] < 0) islands[owner[i]] = next++;
		islands[i] = islands[owner[i]];
	}
	return islands;
}

// 選んだ点で輪郭を切り、入ってきた向きのまま直進した先の点へつなぎ直す(交差する線を組み替える)。
// 点は4個とは限らない(多叉路では周りの角を全て渡す)。各点は入る側と出る側の2つに複製される。向きが決まらない時はfalse
// つなぎ方の候補(点ごとのつなぎ先)を、まっすぐ貫く度合いの順に返す。多叉路で、形の検査を通るものを順に試すために使う
std::vector<std::vector<int>> cross_candidates(const EdGlyph& glyph, const std::vector<std::pair<int, int>>& points, int shape, size_t limit) {
	const int count = (int)points.size();
	std::vector<std::vector<int>> result;
	if (count < 3 || count > 8) return result;
	std::vector<Vec> position(count), in_dir(count), out_dir(count);
	for (int i = 0; i < count; i++) {
		const EdContour& contour = glyph.contours[points[i].first];
		const int size = (int)contour.nodes.size();
		const EdNode& node = contour.nodes[points[i].second];
		const EdNode& prev = contour.nodes[(points[i].second + size - 1) % size];
		const EdNode& after = contour.nodes[(points[i].second + 1) % size];
		const Vec p = node.p[shape];
		Vec from = prev.p[shape], to = after.p[shape];
		if (prev.next.curve && !same_point(prev.next.c2[shape], p)) from = prev.next.c2[shape];
		if (node.next.curve && !same_point(node.next.c1[shape], p)) to = node.next.c1[shape];
		const double in_length = distance(from, p), out_length = distance(p, to);
		if (in_length <= 0 || out_length <= 0) return result;
		position[i] = p;
		in_dir[i] = { (p.x - from.x) / in_length, (p.y - from.y) / in_length };
		out_dir[i] = { (to.x - p.x) / out_length, (to.y - p.y) / out_length };
	}
	// つなぎの費用: 進む向きとのずれ + 出ていく向きとのずれ。自分へのつなぎは、その点を切らないことを表す(余分な角を含んでいても試せる)
	std::vector<std::vector<double>> cost(count, std::vector<double>(count));
	for (int i = 0; i < count; i++) {
		for (int k = 0; k < count; k++) {
			if (i == k) { cost[i][k] = 1.5; continue; }
			const double d = distance(position[i], position[k]);
			const double ahead = d > 0 ? (in_dir[i].x * (position[k].x - position[i].x) + in_dir[i].y * (position[k].y - position[i].y)) / d : 0;
			const double straight = in_dir[i].x * out_dir[k].x + in_dir[i].y * out_dir[k].y;
			// 上限を設ける: 生える画を閉じる線は必ず引き返す向きなので、大きな費用を付けると、貫く線を曲げる候補が選ばれてしまう
			cost[i][k] = std::min(1.0, (1 - ahead) + (1 - straight));
		}
	}
	std::vector<int> order(count);
	for (int i = 0; i < count; i++) order[i] = i;
	std::vector<std::pair<double, std::vector<int>>> ranked;
	do {
		double total = 0;
		bool moved = false;
		for (int i = 0; i < count; i++) {
			total += cost[i][order[i]];
			if (order[i] != i) moved = true;
		}
		if (moved) ranked.push_back({ total, order });
	} while (std::next_permutation(order.begin(), order.end()));
	const size_t keep = std::min(limit, ranked.size());
	std::partial_sort(ranked.begin(), ranked.begin() + keep, ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	for (size_t i = 0; i < keep; i++) result.push_back(ranked[i].second);
	return result;
}

// forcedを渡すと、つなぎ先を求めずにそれを使う
bool cross_contours(EdGlyph& glyph, const std::vector<std::pair<int, int>>& points, int shape, const std::vector<int>* forced = nullptr) {
	const int count = (int)points.size();
	auto node_at = [&](int i) -> const EdNode& { return glyph.contours[points[i].first].nodes[points[i].second]; };
	// 点に入る向きと点から出る向き
	std::vector<Vec> in_dir(count), out_dir(count);
	for (int i = 0; i < count; i++) {
		const EdContour& contour = glyph.contours[points[i].first];
		const int size = (int)contour.nodes.size();
		const EdNode& node = node_at(i);
		const EdNode& prev = contour.nodes[(points[i].second + size - 1) % size];
		const EdNode& after = contour.nodes[(points[i].second + 1) % size];
		const Vec p = node.p[shape];
		Vec from = prev.p[shape], to = after.p[shape];
		if (prev.next.curve && !same_point(prev.next.c2[shape], p)) from = prev.next.c2[shape];
		if (node.next.curve && !same_point(node.next.c1[shape], p)) to = node.next.c1[shape];
		const double in_length = distance(from, p), out_length = distance(p, to);
		if (in_length <= 0 || out_length <= 0) return false;
		in_dir[i] = { (p.x - from.x) / in_length, (p.y - from.y) / in_length };
		out_dir[i] = { (to.x - p.x) / out_length, (to.y - p.y) / out_length };
	}
	// 1. まっすぐ貫く線を先につなぐ: 入る向きのまま進んだ先にあり、同じ向きへ出ていく点を、真っ直ぐなものから順に選ぶ
	std::vector<int> target(count, -1);
	std::vector<bool> taken(count, false);
	struct Link { double score; int from, to; };
	std::vector<Link> links;
	for (int i = 0; i < count; i++) {
		for (int k = 0; k < count; k++) {
			if (k == i) continue;
			const Vec p = node_at(i).p[shape], q = node_at(k).p[shape];
			const double d = distance(p, q);
			if (d <= 0) continue;
			const double ahead = (in_dir[i].x * (q.x - p.x) + in_dir[i].y * (q.y - p.y)) / d;
			const double straight = in_dir[i].x * out_dir[k].x + in_dir[i].y * out_dir[k].y;
			// 生える画の辺が貫く線の角を横取りしないように、ほぼ真っ直ぐなものだけにする
			if (ahead > 0.85 && straight > 0.85) links.push_back({ ahead + straight, i, k });
		}
	}
	std::sort(links.begin(), links.end(), [](const Link& a, const Link& b) { return a.score > b.score; });
	for (const Link& link : links) {
		if (target[link.from] >= 0 || taken[link.to]) continue;
		target[link.from] = link.to;
		taken[link.to] = true;
	}
	// 2. 残りは近い点同士で閉じる(交差から生える画は、付け根を閉じて別の輪郭になる)。
	// 来た向きへ引き返す線は、面の無い輪郭を作るので避ける
	for (;;) {
		double best = 1e30;
		int best_from = -1, best_to = -1;
		for (int i = 0; i < count; i++) {
			if (target[i] >= 0) continue;
			for (int k = 0; k < count; k++) {
				if (taken[k]) continue;
				const Vec p = node_at(i).p[shape], q = node_at(k).p[shape];
				const double d = distance(p, q);
				const double ahead = d > 0 ? (in_dir[i].x * (q.x - p.x) + in_dir[i].y * (q.y - p.y)) / d : 0;
				const double cost = d * (1.5 - ahead) * (1.5 - ahead);
				if (cost < best) { best = cost; best_from = i; best_to = k; }
			}
		}
		if (best_from < 0) break;
		target[best_from] = best_to;
		taken[best_to] = true;
	}
	if (forced) target = *forced;
	// 輪郭を、選んだ点から次の選んだ点までの鎖に分ける。鎖の先頭は点の出る側、末尾は次の点の入る側
	std::vector<std::vector<EdNode>> chains(count);
	std::vector<int> ends(count, -1);	// 鎖の末尾の点の番号
	for (int i = 0; i < count; i++) {
		const EdContour& contour = glyph.contours[points[i].first];
		const int size = (int)contour.nodes.size();
		for (int step = 0; step <= size; step++) {
			const int at = (points[i].second + step) % size;
			chains[i].push_back(contour.nodes[at]);
			if (step == 0) continue;
			auto stop = std::find(points.begin(), points.end(), std::make_pair(points[i].first, at));
			if (stop != points.end()) {
				ends[i] = (int)(stop - points.begin());
				break;
			}
		}
		chains[i].back().next = EdSeg();
	}
	std::vector<EdContour> rebuilt;
	std::vector<bool> used(count, false);
	for (int i = 0; i < count; i++) {
		if (used[i]) continue;
		EdContour contour;
		for (int at = i; !used[at]; at = target[ends[at]]) {
			used[at] = true;
			contour.nodes.insert(contour.nodes.end(), chains[at].begin(), chains[at].end());
			// 多叉路で生える画を閉じる線は交差の中心を通す(角から角へ直接閉じると、貫く線との間に隙間ができる)
			const int from = ends[at], to = target[from];
			if (forced && from != to && in_dir[from].x * out_dir[to].x + in_dir[from].y * out_dir[to].y < 0) {
				EdNode center;
				for (int s = 0; s < 2; s++) {
					for (int k = 0; k < count; k++) {
						center.p[s].x += node_at(k).p[s].x / count;
						center.p[s].y += node_at(k).p[s].y / count;
					}
				}
				contour.nodes.push_back(center);
			}
		}
		rebuilt.push_back(std::move(contour));
	}
	// 関わった輪郭を消して、組み替えた輪郭を足す
	std::vector<int> touched;
	for (const auto& p : points) if (std::find(touched.begin(), touched.end(), p.first) == touched.end()) touched.push_back(p.first);
	std::sort(touched.rbegin(), touched.rend());
	const int insert_at = touched.back();
	for (int c : touched) glyph.contours.erase(glyph.contours.begin() + c);
	glyph.contours.insert(glyph.contours.begin() + insert_at, rebuilt.begin(), rebuilt.end());
	return true;
}

// 辺をtの位置で分けて点を足す(形は変わらない)
void split_segment(EdContour& contour, int n, double t) {
	const int size = (int)contour.nodes.size();
	EdNode& a = contour.nodes[n];
	const EdNode& b = contour.nodes[(n + 1) % size];
	EdNode m;
	m.next.curve = a.next.curve;
	for (int s = 0; s < 2; s++) {
		if (a.next.curve) {
			const Vec q0 = lerp(a.p[s], a.next.c1[s], t), q1 = lerp(a.next.c1[s], a.next.c2[s], t), q2 = lerp(a.next.c2[s], b.p[s], t);
			const Vec r0 = lerp(q0, q1, t), r1 = lerp(q1, q2, t);
			m.p[s] = lerp(r0, r1, t);
			m.next.c1[s] = r1;
			m.next.c2[s] = q2;
			a.next.c1[s] = q0;
			a.next.c2[s] = r0;
		} else {
			m.p[s] = lerp(a.p[s], b.p[s], t);
		}
	}
	contour.nodes.insert(contour.nodes.begin() + n + 1, m);
}

// 点を消して前後の辺を1本にする。曲線は分割の逆算で近似する
void remove_node(EdContour& contour, int n) {
	const int size = (int)contour.nodes.size();
	EdNode& prev = contour.nodes[(n + size - 1) % size];
	const EdNode& node = contour.nodes[n];
	const EdNode& after = contour.nodes[(n + 1) % size];
	const bool curve = prev.next.curve || node.next.curve;
	for (int s = 0; curve && s < 2; s++) {
		const Vec c1 = prev.next.curve ? prev.next.c1[s] : lerp(prev.p[s], node.p[s], 1.0 / 3);
		const Vec c2 = node.next.curve ? node.next.c2[s] : lerp(node.p[s], after.p[s], 2.0 / 3);
		const double l1 = distance(prev.p[s], node.p[s]), l2 = distance(node.p[s], after.p[s]);
		prev.next.c1[s] = l1 > 0 ? lerp(prev.p[s], c1, (l1 + l2) / l1) : c1;
		prev.next.c2[s] = l2 > 0 ? lerp(after.p[s], c2, (l1 + l2) / l2) : c2;
	}
	prev.next.curve = curve;
	contour.nodes.erase(contour.nodes.begin() + n);
}

// 点vが線分abの上にあるか(ハンドルが線分の上にあるだけなら、形は直線と同じ)
bool on_segment(Vec v, Vec a, Vec b) {
	const double length = distance(a, b);
	if (length <= 0) return same_point(v, a);
	const double t = ((v.x - a.x) * (b.x - a.x) + (v.y - a.y) * (b.y - a.y)) / (length * length);
	return t >= -1e-6 && t <= 1 + 1e-6 && same_point(v, lerp(a, b, t));
}

// 形が直線と同じ辺は直線に戻す
void normalize_segment(EdContour& contour, int n) {
	EdNode& a = contour.nodes[n];
	const EdNode& b = contour.nodes[(n + 1) % contour.nodes.size()];
	if (!a.next.curve) return;
	for (int s = 0; s < 2; s++) {
		if (!on_segment(a.next.c1[s], a.p[s], b.p[s]) || !on_segment(a.next.c2[s], a.p[s], b.p[s])) return;
	}
	a.next.curve = false;
}

// 点にハンドルを付ける(無い側だけ)。向きは前後の点を結ぶ向きで、形が滑らかになる
void add_handles(EdContour& contour, int n) {
	const int size = (int)contour.nodes.size();
	EdNode& node = contour.nodes[n];
	EdNode& prev = contour.nodes[(n + size - 1) % size];
	const EdNode& after = contour.nodes[(n + 1) % size];
	const bool out_missing = !node.next.curve || same_point(node.next.c1[0], node.p[0]);
	const bool in_missing = !prev.next.curve || same_point(prev.next.c2[0], node.p[0]);
	const bool out_line = !node.next.curve, in_line = !prev.next.curve;
	for (int s = 0; s < 2; s++) {
		double dx = after.p[s].x - prev.p[s].x, dy = after.p[s].y - prev.p[s].y;
		const double length = std::hypot(dx, dy);
		if (length > 0) { dx /= length; dy /= length; }
		const double out = distance(node.p[s], after.p[s]) / 3, in = distance(prev.p[s], node.p[s]) / 3;
		if (out_line) node.next.c2[s] = lerp(node.p[s], after.p[s], 2.0 / 3);
		if (out_missing) node.next.c1[s] = { node.p[s].x + dx * out, node.p[s].y + dy * out };
		if (in_line) prev.next.c1[s] = lerp(prev.p[s], node.p[s], 1.0 / 3);
		if (in_missing) prev.next.c2[s] = { node.p[s].x - dx * in, node.p[s].y - dy * in };
	}
	node.next.curve = true;
	prev.next.curve = true;
}

// 輪郭の点をfirstから1周分、並べ替える
std::vector<EdNode> rotated(const EdContour& contour, int first) {
	std::vector<EdNode> nodes;
	const int size = (int)contour.nodes.size();
	for (int i = 0; i < size; i++) nodes.push_back(contour.nodes[(first + i) % size]);
	return nodes;
}

// 同じ輪郭の2点の間に線を引いて、2つの輪郭に分ける。2点はどちらの輪郭にも入る。戻り値はfalseなら何もしていない
bool split_contour(EdGlyph& glyph, int c, int a, int b) {
	const int size = (int)glyph.contours[c].nodes.size();
	if (a > b) std::swap(a, b);
	if (b - a < 2 || size - (b - a) < 2) return false;
	const std::vector<EdNode> nodes = rotated(glyph.contours[c], a);
	const int span = b - a;
	EdContour first, second;
	first.nodes.assign(nodes.begin(), nodes.begin() + span + 1);
	first.nodes.back().next = EdSeg();
	second.nodes.assign(nodes.begin() + span, nodes.end());
	second.nodes.push_back(nodes[0]);
	second.nodes.back().next = EdSeg();
	glyph.contours[c] = std::move(first);
	glyph.contours.insert(glyph.contours.begin() + c + 1, std::move(second));
	return true;
}

// 別々の輪郭の2点をつないで1つの輪郭にする。辺を2本切り、もう1本は切った辺の残りの端同士を結ぶ
void join_contours(EdGlyph& glyph, int ca, int a, int cb, int b, int shape) {
	const EdContour& first = glyph.contours[ca];
	const EdContour& second = glyph.contours[cb];
	const int na = (int)first.nodes.size(), nb = (int)second.nodes.size();
	// a→b と (bの前)→(aの次) / b→a と (aの前)→(bの次) の短い方を選ぶ
	const double forward = distance(first.nodes[(a + 1) % na].p[shape], second.nodes[(b + nb - 1) % nb].p[shape]);
	const double backward = distance(second.nodes[(b + 1) % nb].p[shape], first.nodes[(a + na - 1) % na].p[shape]);
	const EdContour& from = forward <= backward ? first : second;
	const EdContour& to = forward <= backward ? second : first;
	const int from_at = forward <= backward ? a : b, to_at = forward <= backward ? b : a;
	std::vector<EdNode> tail = rotated(from, (from_at + 1) % (int)from.nodes.size());	// 末尾がfrom_at
	std::vector<EdNode> headn = rotated(to, to_at);	// 先頭がto_at
	tail.back().next = EdSeg();
	headn.back().next = EdSeg();
	EdContour joined;
	joined.nodes = std::move(headn);
	joined.nodes.insert(joined.nodes.end(), tail.begin(), tail.end());
	const int low = std::min(ca, cb), high = std::max(ca, cb);
	glyph.contours[low] = std::move(joined);
	glyph.contours.erase(glyph.contours.begin() + high);
}

struct EdRef {
	int kind = 0;	// 0=アンカー / 1=辺の始点側のハンドル / 2=辺の終点側のハンドル
	int g = 0, c = 0, n = 0;
	bool operator==(const EdRef& o) const { return kind == o.kind && g == o.g && c == o.c && n == o.n; }
	bool operator<(const EdRef& o) const { return std::tie(g, c, n, kind) < std::tie(o.g, o.c, o.n, o.kind); }
};

enum {
	ID_ADD = 101, ID_DELETE, ID_ADD_HANDLE, ID_DELETE_HANDLE, ID_DUPLICATE, ID_RECONNECT, ID_SPLIT, ID_UNDO, ID_FIT, ID_OK, ID_CANCEL,
	ID_TOGGLE_HANDLE, ID_LINK, ID_MOVE_UP, ID_MOVE_DOWN, ID_MOVE_LEFT, ID_MOVE_RIGHT,
	ID_STROKE_MODE, ID_NEW_STROKE, ID_DELETE_STROKE, ID_STROKE_EARLIER, ID_STROKE_LATER, ID_STROKE_REVERSE, ID_STROKE_RESET, ID_STROKE_FINISH,
	ID_PROPOSE, ID_APPLY, ID_PROPOSE_ALL, ID_PROPOSE_CLOSE, ID_PROPOSE_ADD, ID_PROPOSE_KIND,
	ID_PREVIEW, ID_PREVIEW_PLAY, ID_PREVIEW_SLIDER, ID_MAKE_PEN, ID_PREVIEW_CLOSE
};
const int ED_TOOLBAR = 0, ED_STATUS = 24;
const int ED_PANEL = 122;	// 左のボタンの列の幅(編集画面はその下にも続く。全体を表示の時に字がボタンに隠れないように空ける)
const int ED_NEAR = 3;	// 画面上でこの距離以内の点は、重なっているものとして色を分けて表示する

// ショートカットキー: 仮想キーコード + 修飾キー
const int KEY_CTRL = 0x100, KEY_SHIFT = 0x200;
struct Shortcut {
	const wchar_t* key;	// 設定ファイルの項目名
	const wchar_t* label;
	int command;
	int defaults[2];
	int keys[2];
	bool proposal = false;	// 分割案の表示中だけ有効(他の操作と同じキーを割り当てられる)
};
Shortcut g_shortcuts[] = {
	{ L"Delete", L"点を削除", ID_DELETE, { VK_DELETE, 'X' }, {} },
	{ L"ToggleHandle", L"ハンドルの追加・削除", ID_TOGGLE_HANDLE, { 'Q', 'A' }, {} },
	{ L"Link", L"同じアイランドの点を全選択", ID_LINK, { 'L', 0 }, {} },
	{ L"Ok", L"OK(案の適用・画の追加の終了)", ID_OK, { VK_RETURN, 0 }, {} },
	{ L"Reconnect", L"2点でつなぎ直す", ID_RECONNECT, { 'F', 0 }, {} },
	{ L"Split", L"カット・交差の組み替え", ID_SPLIT, { 'S', 0 }, {} },
	{ L"Duplicate", L"点を複製", ID_DUPLICATE, { 'D' | KEY_SHIFT, 0 }, {} },
	{ L"StrokeMode", L"筆順の編集に切り替え", ID_STROKE_MODE, { VK_TAB, 0 }, {} },
	{ L"NewStroke", L"筆順: 画を追加", ID_NEW_STROKE, { 'N', 0 }, {} },
	{ L"DeleteStroke", L"筆順: 画を削除", ID_DELETE_STROKE, { 'X' | KEY_SHIFT, 0 }, {} },
	{ L"StrokeEarlier", L"筆順: 書き順を前へ", ID_STROKE_EARLIER, { VK_OEM_4, 0 }, {} },
	{ L"StrokeLater", L"筆順: 書き順を後ろへ", ID_STROKE_LATER, { VK_OEM_6, 0 }, {} },
	{ L"StrokeReverse", L"筆順: 向きを反転", ID_STROKE_REVERSE, { 'R', 0 }, {} },
	{ L"Propose", L"筆順: 自動アイランド分割", ID_PROPOSE, { 'P', 0 }, {} },
	{ L"Preview", L"筆順: 筆順確認", ID_PREVIEW, { 'V', 0 }, {} },
	{ L"ProposeAdd", L"分割案: 案を追加", ID_PROPOSE_ADD, { 'A', 0 }, {}, true },
	{ L"ProposeKind", L"分割案: 種類を変更", ID_PROPOSE_KIND, { 'Q', 0 }, {}, true },
	{ L"ProposeAll", L"分割案: 全て切り替え", ID_PROPOSE_ALL, { 'S', 0 }, {}, true },
	{ L"MoveUp", L"上へ移動", ID_MOVE_UP, { VK_UP, 0 }, {} },
	{ L"MoveDown", L"下へ移動", ID_MOVE_DOWN, { VK_DOWN, 0 }, {} },
	{ L"MoveLeft", L"左へ移動", ID_MOVE_LEFT, { VK_LEFT, 0 }, {} },
	{ L"MoveRight", L"右へ移動", ID_MOVE_RIGHT, { VK_RIGHT, 0 }, {} },
	{ L"Undo", L"1つ戻す", ID_UNDO, { 'Z' | KEY_CTRL, 0 }, {} },
	{ L"Fit", L"全体を表示", ID_FIT, { VK_HOME, 0 }, {} },
	{ L"Cancel", L"キャンセル", ID_CANCEL, { VK_ESCAPE, 0 }, {} },
};
double g_move_step = 1, g_move_shift = 10, g_move_ctrl = 0.1;

std::wstring settings_path() {
	return g_config && g_config->app_data_path ? std::wstring(g_config->app_data_path) + L"\\TextPath_Te.ini" : L"";
}

void load_settings() {
	const std::wstring path = settings_path();
	for (Shortcut& s : g_shortcuts) {
		for (int i = 0; i < 2; i++) {
			const std::wstring name = std::wstring(s.key) + L"_" + std::to_wstring(i + 1);
			s.keys[i] = path.empty() ? s.defaults[i] : GetPrivateProfileIntW(L"Shortcut", name.c_str(), s.defaults[i], path.c_str());
		}
	}
	if (path.empty()) return;
	auto read = [&](LPCWSTR name, double fallback) {
		wchar_t text[64];
		GetPrivateProfileStringW(L"Move", name, L"", text, 64, path.c_str());
		const double value = wcstod(text, nullptr);
		return value > 0 ? value : fallback;
	};
	g_move_step = read(L"Step", 1);
	g_move_shift = read(L"ShiftScale", 10);
	g_move_ctrl = read(L"CtrlScale", 0.1);
}

void save_settings() {
	const std::wstring path = settings_path();
	if (path.empty()) return;
	for (const Shortcut& s : g_shortcuts) {
		for (int i = 0; i < 2; i++) {
			const std::wstring name = std::wstring(s.key) + L"_" + std::to_wstring(i + 1);
			WritePrivateProfileStringW(L"Shortcut", name.c_str(), std::to_wstring(s.keys[i]).c_str(), path.c_str());
		}
	}
	auto write = [&](LPCWSTR name, double value) {
		wchar_t text[64];
		swprintf_s(text, L"%g", value);
		WritePrivateProfileStringW(L"Move", name, text, path.c_str());
	};
	write(L"Step", g_move_step);
	write(L"ShiftScale", g_move_shift);
	write(L"CtrlScale", g_move_ctrl);
}

bool is_move_command(int command) { return command >= ID_MOVE_UP && command <= ID_MOVE_RIGHT; }

// 押されたキーのコマンドを探す。移動は、修飾キーを倍率に使うので、修飾キー無しの割り当てでも反応する
int find_shortcut(int vk, bool ctrl, bool shift, bool proposing) {
	const int key = vk | (ctrl ? KEY_CTRL : 0) | (shift ? KEY_SHIFT : 0);
	// 分割案の表示中は、その表示専用の割り当てを優先する
	for (const Shortcut& s : g_shortcuts) if (s.proposal && proposing && (s.keys[0] == key || s.keys[1] == key)) return s.command;
	for (const Shortcut& s : g_shortcuts) if (!s.proposal && (s.keys[0] == key || s.keys[1] == key)) return s.command;
	for (const Shortcut& s : g_shortcuts) if (is_move_command(s.command) && (s.keys[0] == vk || s.keys[1] == vk)) return s.command;
	return 0;
}

// 筆順データをその文字のフォントの線に合わせた既定の筆順(データに無い文字は空)。定義はペンの処理の後
std::vector<std::vector<Vec>> default_strokes(const EdGlyph& glyph);

// ペンで書く処理の時刻の表(定義はペンの処理の所)。高度な編集の確認でも使う
struct PenMap;
std::shared_ptr<PenMap> build_pen_map(const std::vector<double>& points, const std::vector<double>& meta, UINT32 key,
	const std::vector<GlyphStrokes>& custom, const std::vector<double>& reference);
float pen_time_at(const PenMap& map, double x, double y);

// アイランド分割の案: 2点は間で切る(L字)、4点は交差の組み替え(交差・T字)。点は輪郭の点か辺の上で、無い点は適用の時に足す
struct Proposal {
	enum Kind { CROSS, TEE, CORNER } kind = CROSS;
	int glyph = 0, stroke_a = 0, stroke_b = 0;
	int junction_strokes = 0;	// 多叉路に集まる画の数
	std::vector<Vec> points;
	bool on = true, failed = false;
	// 切り替えられる切り方(先頭が最初の案)
	struct Variant { Kind kind; std::vector<Vec> points; };
	std::vector<Variant> variants;
	int variant = 0;
};
// 1文字の案を作る。見つけられなかった接合の数をmissedに足す。定義はペンの処理の後
std::vector<Proposal> propose_splits(const EdGlyph& glyph, const std::vector<std::vector<Vec>>& lines, int glyph_index, int& missed);

struct EdSnapshot {
	EdModel model;
	std::vector<GlyphStrokes> strokes;
};

struct Editor {
	HWND hwnd = nullptr;
	OBJECT_HANDLE object = nullptr;
	EdModel model;
	std::vector<EdSnapshot> history;
	std::vector<GlyphStrokes> strokes;
	bool strokes_ready = false;	// 既定の筆順を作ったか(筆順の編集に初めて切り替えた時に作る)
	bool stroke_mode = false;
	bool model_dirty = false, strokes_dirty = false;
	bool previewing = false;	// 書き順を確認中
	std::shared_ptr<PenMap> preview_map;
	double preview_progress = 0;
	bool expanded = false;	// タイムラインへ展開した、1つのアイランドだけのオブジェクト
	bool make_pen = false;	// OKの時に「ペンで書く」と書き進みの移動を設定する
	bool proposing = false;	// アイランド分割の案を表示中
	std::vector<Proposal> proposals;
	int drag_proposal = -1, drag_point = -1;	// 掴んでいる案とその点(点が-1なら案の番号)
	bool drag_proposal_moved = false, drag_proposal_shift = false;
	bool adding_proposal = false;	// 案を手で足している途中(最後の案が作りかけ)
	bool adding_stroke = false;	// 「画を追加」がオン
	int drawing_glyph = -1, drawing_stroke = -1;	// 追加している途中の画
	std::vector<double> header[2];
	bool has_second = false;
	int shape = 0;
	std::vector<EdRef> selection;
	double zoom = 1, pan_x = 0, pan_y = 0;
	enum { NONE, DRAG, PAN, RECT_SELECT } mode = NONE;
	POINT drag_start{}, drag_applied{};	// 掴んだ位置と、動かし済みの量
	POINT last{};
	bool drag_moved = false, cycle = false;
	bool key_moving = false;	// 矢印キーの移動の続き(続けて押した分は「戻す」1回にまとめる)
	std::vector<EdRef> cycle_hits;
	std::wstring status;
} g_ed;

const wchar_t* ED_HINT = L"クリックで選択 / Ctrl+クリックで追加選択 / Ctrl+ドラッグで矩形選択 / 線の上のダブルクリックで点を追加 / 重なった点は再クリックで切り替え / ホイールで拡大 / 右ドラッグで表示を移動";
const wchar_t* ED_STROKE_HINT = L"クリックで点を選択 / 点のドラッグで修正 / 線の上のダブルクリックで点を追加 / ホイールで拡大 / 右ドラッグで表示を移動";

POINT to_screen(Vec v) { return { (LONG)std::lround(v.x * g_ed.zoom + g_ed.pan_x), (LONG)std::lround(v.y * g_ed.zoom + g_ed.pan_y) }; }

void ed_fit() {
	RECT rc;
	GetClientRect(g_ed.hwnd, &rc);
	const double w = rc.right - ED_PANEL, h = rc.bottom - ED_TOOLBAR - ED_STATUS;
	bool any = false;
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
	auto add = [&](Vec v) {
		if (!any) { x0 = x1 = v.x; y0 = y1 = v.y; any = true; }
		x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
		y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
	};
	const int s = g_ed.stroke_mode ? 0 : g_ed.shape;
	for (auto& g : g_ed.model) for (auto& c : g.contours) for (auto& n : c.nodes) {
		add(n.p[s]);
		if (n.next.curve) { add(n.next.c1[s]); add(n.next.c2[s]); }
	}
	if (!any) return;
	g_ed.zoom = std::min((w - 80) / std::max(x1 - x0, 1.0), (h - 80) / std::max(y1 - y0, 1.0));
	g_ed.pan_x = ED_PANEL + (w - g_ed.zoom * (x0 + x1)) / 2;
	g_ed.pan_y = ED_TOOLBAR + (h - g_ed.zoom * (y0 + y1)) / 2;
}

void ed_set_status(const std::wstring& text) {
	g_ed.status = text;
	InvalidateRect(g_ed.hwnd, nullptr, FALSE);
}

void ed_push_history() {
	g_ed.history.push_back({ g_ed.model, g_ed.strokes });
	if (g_ed.stroke_mode) g_ed.strokes_dirty = true;
	else g_ed.model_dirty = true;
}

Vec proposal_center(const Proposal& p) {
	Vec c{ 0, 0 };
	for (const Vec& v : p.points) { c.x += v.x / p.points.size(); c.y += v.y / p.points.size(); }
	return c;
}

// 分割案の描画: 採用は緑、不採用は灰色、適用できなかったものは赤。中央に案の番号を出す
void ed_paint_proposals(HDC dc) {
	SetBkMode(dc, TRANSPARENT);
	HGDIOBJ old_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
	for (int i = 0; i < (int)g_ed.proposals.size(); i++) {
		const Proposal& p = g_ed.proposals[i];
		if (p.points.empty()) continue;	// 足し始めたばかりの案
		const COLORREF color = p.failed ? RGB(255, 70, 70) : p.on ? RGB(70, 240, 110) : RGB(140, 140, 140);
		HPEN pen = CreatePen(p.on ? PS_SOLID : PS_DOT, p.on ? 2 : 1, color);
		HBRUSH brush = CreateSolidBrush(color);
		HGDIOBJ old_pen = SelectObject(dc, pen);
		HGDIOBJ old_brush = SelectObject(dc, brush);
		const POINT center = to_screen(proposal_center(p));
		for (const Vec& v : p.points) {
			const POINT q = to_screen(v);
			if (p.points.size() > 2) { MoveToEx(dc, center.x, center.y, nullptr); LineTo(dc, q.x, q.y); }
			Ellipse(dc, q.x - 5, q.y - 5, q.x + 6, q.y + 6);
		}
		if (p.points.size() == 2) {
			const POINT a = to_screen(p.points[0]), b = to_screen(p.points[1]);
			MoveToEx(dc, a.x, a.y, nullptr);
			LineTo(dc, b.x, b.y);
		}
		Rectangle(dc, center.x - 11, center.y - 9, center.x + 12, center.y + 10);
		// 番号と種類(X=交差 T=T字で共通部分は貫く画のもの T+=T字で共通部分は両方のもの L=切る線)
		const std::wstring number = std::to_wstring(i + 1) + (p.kind == Proposal::CROSS ? L"X" : p.kind == Proposal::CORNER ? L"L" : p.points.size() > 2 ? L"T+" : L"T");
		RECT box{ center.x - 11, center.y - 9, center.x + 12, center.y + 10 };
		SetTextColor(dc, RGB(0, 0, 0));
		DrawTextW(dc, number.c_str(), -1, &box, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
		SelectObject(dc, old_pen);
		SelectObject(dc, old_brush);
		DeleteObject(pen);
		DeleteObject(brush);
	}
	SelectObject(dc, old_font);
}

// 筆順の描画: 画ごとに色を変え、始点に順番の番号、終点に向きの印を付ける
void ed_paint_strokes(HDC dc) {
	SetBkMode(dc, TRANSPARENT);
	HGDIOBJ old_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
	for (int gi = 0; gi < (int)g_ed.strokes.size(); gi++) {
		const auto& lines = g_ed.strokes[gi].lines;
		for (int si = 0; si < (int)lines.size(); si++) {
			const std::vector<Vec>& line = lines[si];
			if (line.empty()) continue;
			static const COLORREF colors[] = { RGB(255, 170, 60), RGB(90, 200, 255), RGB(130, 230, 120), RGB(240, 120, 200), RGB(255, 230, 90), RGB(170, 150, 255) };
			const COLORREF color = colors[si % 6];
			HPEN pen = CreatePen(PS_SOLID, 2, color);
			HBRUSH brush = CreateSolidBrush(color);
			HBRUSH mark = CreateSolidBrush(RGB(255, 60, 60));
			HGDIOBJ old_pen = SelectObject(dc, pen);
			HGDIOBJ old_brush = SelectObject(dc, brush);
			const POINT first = to_screen(line[0]);
			MoveToEx(dc, first.x, first.y, nullptr);
			for (size_t i = 1; i < line.size(); i++) {
				const POINT p = to_screen(line[i]);
				LineTo(dc, p.x, p.y);
			}
			if (line.size() >= 2) {
				// 終点の矢印
				const POINT a = to_screen(line[line.size() - 2]), b = to_screen(line.back());
				const double length = std::max(std::hypot((double)b.x - a.x, (double)b.y - a.y), 1.0);
				const double ux = (b.x - a.x) / length, uy = (b.y - a.y) / length;
				for (const double side : { 1.0, -1.0 }) {
					MoveToEx(dc, b.x, b.y, nullptr);
					LineTo(dc, (int)(b.x - ux * 12 + uy * 7 * side), (int)(b.y - uy * 12 - ux * 7 * side));
				}
			}
			SelectObject(dc, GetStockObject(BLACK_PEN));
			for (int n = 0; n < (int)line.size(); n++) {
				const POINT p = to_screen(line[n]);
				const bool on = std::find(g_ed.selection.begin(), g_ed.selection.end(), EdRef{ 0, gi, si, n }) != g_ed.selection.end();
				SelectObject(dc, on ? mark : brush);
				const int r = on ? 6 : 4;
				Rectangle(dc, p.x - r, p.y - r, p.x + r + 1, p.y + r + 1);
			}
			// 始点の番号
			SelectObject(dc, brush);
			Ellipse(dc, first.x - 10, first.y - 10, first.x + 11, first.y + 11);
			const std::wstring number = std::to_wstring(si + 1);
			RECT box{ first.x - 10, first.y - 10, first.x + 11, first.y + 11 };
			SetTextColor(dc, RGB(0, 0, 0));
			DrawTextW(dc, number.c_str(), -1, &box, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
			SelectObject(dc, old_pen);
			SelectObject(dc, old_brush);
			DeleteObject(pen);
			DeleteObject(brush);
			DeleteObject(mark);
		}
	}
	SelectObject(dc, old_font);
}

COLORREF contour_color(int index) {
	static const COLORREF colors[] = { RGB(255, 170, 60), RGB(90, 200, 255), RGB(130, 230, 120), RGB(240, 120, 200), RGB(255, 230, 90), RGB(170, 150, 255) };
	return colors[index % 6];
}

void ed_paint(HDC target) {
	RECT rc;
	GetClientRect(g_ed.hwnd, &rc);
	const int w = rc.right, h = rc.bottom;
	if (w <= 0 || h <= 0) return;
	HDC dc = CreateCompatibleDC(target);
	HBITMAP bitmap = CreateCompatibleBitmap(target, w, h);
	HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
	FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

	const int s = g_ed.stroke_mode ? 0 : g_ed.shape;	// 筆順は形状1の上で編集する
	// 塗り: 本番と同じ塗り方で表示する
	const int view_h = h - ED_TOOLBAR - ED_STATUS;
	if (view_h > 0) {
		auto screen = [&](Vec v) { return Vec{ v.x * g_ed.zoom + g_ed.pan_x, v.y * g_ed.zoom + g_ed.pan_y - ED_TOOLBAR }; };
		Rasterizer raster(w, view_h);
		for (auto& g : g_ed.model) for (auto& c : g.contours) {
			const int size = (int)c.nodes.size();
			for (int i = 0; i < size; i++) {
				const EdNode& a = c.nodes[i];
				const EdNode& b = c.nodes[(i + 1) % size];
				if (a.next.curve) add_curve(raster, screen(a.p[s]), screen(a.next.c1[s]), screen(a.next.c2[s]), screen(b.p[s]));
				else raster.line(screen(a.p[s]), screen(b.p[s]));
			}
		}
		std::vector<PIXEL_RGBA> pixels((size_t)w * view_h);
		raster.write(pixels.data(), 0, 0, 0);
		std::vector<UINT32> image((size_t)w * view_h);
		for (size_t i = 0; i < image.size(); i++) {
			UINT32 v = 28 + pixels[i].a * 62 / 255;
			if (g_ed.previewing && g_ed.preview_map && pixels[i].a) {
				// 確認中: 書かれた所を白く、まだの所を暗く出す
				const int px = (int)(i % w), py = (int)(i / w) + ED_TOOLBAR;
				const float t = pen_time_at(*g_ed.preview_map, (px + 0.5 - g_ed.pan_x) / g_ed.zoom, (py + 0.5 - g_ed.pan_y) / g_ed.zoom);
				v = t >= 0 && t <= g_ed.preview_progress ? 28 + pixels[i].a * 215 / 255 : 28 + pixels[i].a * 30 / 255;
			}
			image[i] = v | (v << 8) | (v << 16);
		}
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(info.bmiHeader);
		info.bmiHeader.biWidth = w;
		info.bmiHeader.biHeight = -view_h;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		SetDIBitsToDevice(dc, 0, ED_TOOLBAR, w, view_h, 0, 0, 0, view_h, image.data(), &info, DIB_RGB_COLORS);
	}

	auto selected = [&](const EdRef& r) { return std::find(g_ed.selection.begin(), g_ed.selection.end(), r) != g_ed.selection.end(); };
	HGDIOBJ old_pen = SelectObject(dc, GetStockObject(WHITE_PEN));
	HGDIOBJ old_brush = SelectObject(dc, GetStockObject(BLACK_BRUSH));
	int color_index = 0;
	struct Mark { POINT p; COLORREF color; bool on; };
	std::vector<Mark> marks;
	for (int gi = 0; !g_ed.stroke_mode && gi < (int)g_ed.model.size(); gi++) {
		const EdGlyph& g = g_ed.model[gi];
		for (int ci = 0; ci < (int)g.contours.size(); ci++) {
			const EdContour& c = g.contours[ci];
			const int size = (int)c.nodes.size();
			const COLORREF color = contour_color(color_index++);
			HPEN pen = CreatePen(PS_SOLID, 1, color);
			HPEN arm = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
			HBRUSH mark = CreateSolidBrush(RGB(255, 60, 60));
			for (int i = 0; i < size; i++) {
				const EdNode& a = c.nodes[i];
				const EdNode& b = c.nodes[(i + 1) % size];
				SelectObject(dc, pen);
				const POINT p0 = to_screen(a.p[s]);
				if (a.next.curve) {
					const POINT curve[4] = { p0, to_screen(a.next.c1[s]), to_screen(a.next.c2[s]), to_screen(b.p[s]) };
					PolyBezier(dc, curve, 4);
				} else {
					MoveToEx(dc, p0.x, p0.y, nullptr);
					const POINT p1 = to_screen(b.p[s]);
					LineTo(dc, p1.x, p1.y);
				}
			}
			for (int i = 0; i < size; i++) {
				const EdNode& a = c.nodes[i];
				const EdNode& b = c.nodes[(i + 1) % size];
				if (!a.next.curve) continue;
				auto draw_handle = [&](Vec owner, Vec handle, int kind) {
					const POINT from = to_screen(owner), to = to_screen(handle);
					SelectObject(dc, arm);
					MoveToEx(dc, from.x, from.y, nullptr);
					LineTo(dc, to.x, to.y);
					SelectObject(dc, GetStockObject(NULL_PEN));
					SelectObject(dc, selected({ kind, gi, ci, i }) ? mark : (HBRUSH)GetStockObject(LTGRAY_BRUSH));
					Ellipse(dc, to.x - 4, to.y - 4, to.x + 5, to.y + 5);
				};
				draw_handle(a.p[s], a.next.c1[s], 1);
				draw_handle(b.p[s], a.next.c2[s], 2);
			}
			for (int i = 0; i < size; i++) marks.push_back({ to_screen(c.nodes[i].p[s]), color, selected({ 0, gi, ci, i }) });
			SelectObject(dc, GetStockObject(BLACK_PEN));
			SelectObject(dc, GetStockObject(BLACK_BRUSH));
			DeleteObject(pen);
			DeleteObject(arm);
			DeleteObject(mark);
		}
	}
	// アンカー: 重なった点は1つの四角を点の数で縦に分けて、それぞれの色で塗る
	std::sort(marks.begin(), marks.end(), [](const Mark& a, const Mark& b) { return a.p.x < b.p.x; });
	std::vector<bool> drawn(marks.size(), false);
	for (size_t i = 0; i < marks.size(); i++) {
		if (drawn[i]) continue;
		std::vector<size_t> cluster{ i };
		for (size_t k = i + 1; k < marks.size() && marks[k].p.x - marks[i].p.x <= ED_NEAR; k++) {
			if (!drawn[k] && std::abs(marks[k].p.y - marks[i].p.y) <= ED_NEAR) cluster.push_back(k);
		}
		bool on = false;
		for (size_t k : cluster) { drawn[k] = true; on = on || marks[k].on; }
		const int count = (int)cluster.size();
		const int strip = count > 1 ? 5 : 9;
		const int width = strip * count + (on && count == 1 ? 4 : 0), height = count > 1 || on ? 13 : 9;
		const int left = marks[i].p.x - width / 2, top = marks[i].p.y - height / 2;
		SelectObject(dc, GetStockObject(BLACK_PEN));
		SelectObject(dc, GetStockObject(BLACK_BRUSH));
		Rectangle(dc, left - 1, top - 1, left + width + 1, top + height + 1);
		for (int k = 0; k < count; k++) {
			const Mark& m = marks[cluster[k]];
			COLORREF color = m.on ? RGB(255, 60, 60) : m.color;
			// 同じ輪郭の点は同じ色になるので、1つおきに暗くして見分ける
			if (k % 2) color = RGB(GetRValue(color) * 3 / 5, GetGValue(color) * 3 / 5, GetBValue(color) * 3 / 5);
			HBRUSH brush = CreateSolidBrush(color);
			RECT cell{ left + (count > 1 ? k * strip : 0), top, count > 1 ? left + (k + 1) * strip : left + width, top + height };
			FillRect(dc, &cell, brush);
			DeleteObject(brush);
		}
	}
	SelectObject(dc, old_pen);
	SelectObject(dc, old_brush);
	if (g_ed.stroke_mode && !g_ed.previewing) ed_paint_strokes(dc);
	if (g_ed.proposing) ed_paint_proposals(dc);
	if (g_ed.mode == Editor::RECT_SELECT) {
		HPEN pen = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
		HGDIOBJ pen_before = SelectObject(dc, pen);
		HGDIOBJ brush_before = SelectObject(dc, GetStockObject(NULL_BRUSH));
		SetBkMode(dc, TRANSPARENT);
		Rectangle(dc, g_ed.drag_start.x, g_ed.drag_start.y, g_ed.last.x, g_ed.last.y);
		SelectObject(dc, pen_before);
		SelectObject(dc, brush_before);
		DeleteObject(pen);
	}

	RECT foot{ 0, h - ED_STATUS, w, h };
	FillRect(dc, &foot, GetSysColorBrush(COLOR_BTNFACE));
	SetBkMode(dc, TRANSPARENT);
	HGDIOBJ old_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
	foot.left += 8;
	DrawTextW(dc, g_ed.status.c_str(), -1, &foot, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
	SelectObject(dc, old_font);

	BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
	SelectObject(dc, old_bitmap);
	DeleteObject(bitmap);
	DeleteDC(dc);
}

// (x, y)付近の点を、アンカーを優先して集める
std::vector<EdRef> ed_hits(int x, int y) {
	std::vector<EdRef> anchors, handles;
	const int s = g_ed.shape;
	auto near_to = [&](Vec v) { const POINT p = to_screen(v); return std::abs(p.x - x) <= 7 && std::abs(p.y - y) <= 7; };
	for (int gi = 0; gi < (int)g_ed.model.size(); gi++) {
		const EdGlyph& g = g_ed.model[gi];
		for (int ci = 0; ci < (int)g.contours.size(); ci++) {
			const EdContour& c = g.contours[ci];
			for (int i = 0; i < (int)c.nodes.size(); i++) {
				const EdNode& a = c.nodes[i];
				if (near_to(a.p[s])) anchors.push_back({ 0, gi, ci, i });
				if (a.next.curve) {
					if (near_to(a.next.c1[s])) handles.push_back({ 1, gi, ci, i });
					if (near_to(a.next.c2[s])) handles.push_back({ 2, gi, ci, i });
				}
			}
		}
	}
	return anchors.empty() ? handles : anchors;
}

// (x, y)に最も近い辺を探す
bool ed_nearest_segment(int x, int y, int& out_g, int& out_c, int& out_n, double& out_t) {
	const int s = g_ed.shape;
	double best = 10;
	bool found = false;
	for (int gi = 0; gi < (int)g_ed.model.size(); gi++) {
		const EdGlyph& g = g_ed.model[gi];
		for (int ci = 0; ci < (int)g.contours.size(); ci++) {
			const EdContour& c = g.contours[ci];
			const int size = (int)c.nodes.size();
			for (int i = 0; i < size; i++) {
				const EdNode& a = c.nodes[i];
				const EdNode& b = c.nodes[(i + 1) % size];
				if (!a.next.curve) {
					// 直線はクリックした位置の真横に点を置く
					const POINT p0 = to_screen(a.p[s]), p1 = to_screen(b.p[s]);
					const double dx = p1.x - p0.x, dy = p1.y - p0.y, square = dx * dx + dy * dy;
					if (square <= 0) continue;
					const double t = ((x - p0.x) * dx + (y - p0.y) * dy) / square;
					if (t <= 0 || t >= 1) continue;
					const double d = std::hypot(p0.x + dx * t - x, p0.y + dy * t - y);
					if (d < best) { best = d; out_g = gi; out_c = ci; out_n = i; out_t = t; found = true; }
					continue;
				}
				for (int k = 1; k < 64; k++) {
					const double t = k / 64.0, u = 1 - t;
					Vec v;
					if (a.next.curve) {
						const Vec &p0 = a.p[s], &p1 = a.next.c1[s], &p2 = a.next.c2[s], &p3 = b.p[s];
						v = { u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
							u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y };
					} else {
						v = lerp(a.p[s], b.p[s], t);
					}
					const double d = std::hypot(v.x * g_ed.zoom + g_ed.pan_x - x, v.y * g_ed.zoom + g_ed.pan_y - y);
					if (d < best) { best = d; out_g = gi; out_c = ci; out_n = i; out_t = t; found = true; }
				}
			}
		}
	}
	return found;
}

void ed_move_selection(double dx, double dy) {
	if (g_ed.stroke_mode) {
		for (const EdRef& r : g_ed.selection) {
			Vec& v = g_ed.strokes[r.g].lines[r.c][r.n];
			v.x += dx;
			v.y += dy;
			g_ed.strokes[r.g].custom = true;
		}
		return;
	}
	const int s = g_ed.shape;
	for (const EdRef& r : g_ed.selection) {
		EdContour& c = g_ed.model[r.g].contours[r.c];
		EdNode& node = c.nodes[r.n];
		const auto shift = [&](Vec& v) { v.x += dx; v.y += dy; };
		if (r.kind == 1) {
			if (std::find(g_ed.selection.begin(), g_ed.selection.end(), EdRef{ 0, r.g, r.c, r.n }) == g_ed.selection.end()) shift(node.next.c1[s]);
		} else if (r.kind == 2) {
			const int after = (r.n + 1) % (int)c.nodes.size();
			if (std::find(g_ed.selection.begin(), g_ed.selection.end(), EdRef{ 0, r.g, r.c, after }) == g_ed.selection.end()) shift(node.next.c2[s]);
		} else {
			// ハンドルはアンカーに追従する
			shift(node.p[s]);
			shift(node.next.c1[s]);
			EdNode& prev = c.nodes[(r.n + c.nodes.size() - 1) % c.nodes.size()];
			shift(prev.next.c2[s]);
		}
	}
}

std::vector<EdRef> ed_selected_anchors() {
	std::vector<EdRef> anchors;
	for (const EdRef& r : g_ed.selection) if (r.kind == 0) anchors.push_back(r);
	std::sort(anchors.begin(), anchors.end());
	return anchors;
}

void ed_delete_nodes() {
	std::vector<EdRef> anchors = ed_selected_anchors();
	if (anchors.empty()) { ed_set_status(L"削除する点を選んでください。"); return; }
	ed_push_history();
	for (auto it = anchors.rbegin(); it != anchors.rend(); ++it) {
		EdGlyph& g = g_ed.model[it->g];
		EdContour& c = g.contours[it->c];
		if (c.nodes.empty()) continue;
		// 残りが1点になる輪郭は面にならないので、輪郭ごと消す
		if (c.nodes.size() <= 2) c.nodes.clear();
		else remove_node(c, it->n);
	}
	for (auto& g : g_ed.model) {
		g.contours.erase(std::remove_if(g.contours.begin(), g.contours.end(), [](const EdContour& c) { return c.nodes.empty(); }), g.contours.end());
	}
	g_ed.selection.clear();
	ed_set_status(L"点を削除しました。");
}

void ed_add_handles() {
	std::vector<EdRef> anchors = ed_selected_anchors();
	if (anchors.empty()) { ed_set_status(L"ハンドルを付ける点を選んでください。"); return; }
	ed_push_history();
	for (const EdRef& r : anchors) add_handles(g_ed.model[r.g].contours[r.c], r.n);
	ed_set_status(L"ハンドルを追加しました。");
}

void ed_delete_handles() {
	if (g_ed.selection.empty()) { ed_set_status(L"点かハンドルを選んでください。"); return; }
	ed_push_history();
	for (const EdRef& r : g_ed.selection) {
		EdContour& c = g_ed.model[r.g].contours[r.c];
		const int size = (int)c.nodes.size();
		EdNode& node = c.nodes[r.n];
		EdNode& prev = c.nodes[(r.n + size - 1) % size];
		const EdNode& after = c.nodes[(r.n + 1) % size];
		for (int s = 0; s < 2; s++) {
			if (r.kind == 0 || r.kind == 1) node.next.c1[s] = node.p[s];
			if (r.kind == 2) node.next.c2[s] = after.p[s];
			if (r.kind == 0) prev.next.c2[s] = node.p[s];
		}
		normalize_segment(c, r.n);
		if (r.kind == 0) normalize_segment(c, (r.n + size - 1) % size);
	}
	g_ed.selection.erase(std::remove_if(g_ed.selection.begin(), g_ed.selection.end(), [](const EdRef& r) { return r.kind != 0; }), g_ed.selection.end());
	ed_set_status(L"ハンドルを削除しました。");
}

void ed_duplicate() {
	std::vector<EdRef> anchors = ed_selected_anchors();
	if (anchors.empty()) { ed_set_status(L"複製する点を選んでください。"); return; }
	ed_push_history();
	for (auto it = anchors.rbegin(); it != anchors.rend(); ++it) {
		EdContour& c = g_ed.model[it->g].contours[it->c];
		EdNode copy = c.nodes[it->n];
		c.nodes[it->n].next = EdSeg();
		c.nodes.insert(c.nodes.begin() + it->n + 1, copy);
	}
	g_ed.selection.clear();
	ed_set_status(L"点を複製しました(元の点と同じ位置にあります。ドラッグで離してください)。");
}

void ed_reconnect() {
	const std::vector<EdRef> anchors = ed_selected_anchors();
	if (anchors.size() != 2) { ed_set_status(L"つなぎ直す点を2つ選んでください。"); return; }
	const EdRef& a = anchors[0];
	const EdRef& b = anchors[1];
	if (a.g != b.g) { ed_set_status(L"別々の文字の点はつなげません。"); return; }
	ed_push_history();
	EdGlyph& g = g_ed.model[a.g];
	if (a.c == b.c) {
		if (!split_contour(g, a.c, a.n, b.n)) {
			g_ed.history.pop_back();
			ed_set_status(L"隣り合う点の間では切り分けられません。");
			return;
		}
		ed_set_status(L"2点の間で輪郭を切り分けました(2点はそれぞれの輪郭に複製されます)。");
	} else {
		join_contours(g, a.c, a.n, b.c, b.n, g_ed.shape);
		ed_set_status(L"2つの輪郭をつなぎました。");
	}
	g_ed.selection.clear();
}

// 点のハンドルをしまう(辺の両方のハンドルが無くなれば直線に戻す)
void remove_node_handles(EdContour& c, int n) {
	const int size = (int)c.nodes.size();
	EdNode& node = c.nodes[n];
	EdNode& prev = c.nodes[(n + size - 1) % size];
	for (int s = 0; s < 2; s++) {
		node.next.c1[s] = node.p[s];
		prev.next.c2[s] = node.p[s];
	}
	normalize_segment(c, n);
	normalize_segment(c, (n + size - 1) % size);
}

// 点にハンドルがあれば消し、無ければ足す
void ed_toggle_handles() {
	if (g_ed.selection.empty()) { ed_set_status(L"点を選んでください。"); return; }
	ed_push_history();
	for (const EdRef& r : g_ed.selection) {
		EdContour& c = g_ed.model[r.g].contours[r.c];
		const int size = (int)c.nodes.size();
		EdNode& node = c.nodes[r.n];
		if (r.kind == 1) { for (int s = 0; s < 2; s++) node.next.c1[s] = node.p[s]; normalize_segment(c, r.n); continue; }
		if (r.kind == 2) { for (int s = 0; s < 2; s++) node.next.c2[s] = c.nodes[(r.n + 1) % size].p[s]; normalize_segment(c, r.n); continue; }
		const EdNode& prev = c.nodes[(r.n + size - 1) % size];
		const bool has = (node.next.curve && !same_point(node.next.c1[g_ed.shape], node.p[g_ed.shape]))
			|| (prev.next.curve && !same_point(prev.next.c2[g_ed.shape], node.p[g_ed.shape]));
		if (has) remove_node_handles(c, r.n);
		else add_handles(c, r.n);
	}
	g_ed.selection.erase(std::remove_if(g_ed.selection.begin(), g_ed.selection.end(), [](const EdRef& r) { return r.kind != 0; }), g_ed.selection.end());
	ed_set_status(L"ハンドルを切り替えました。");
}

// 選んだ点と同じアイランドのアンカーを全て選ぶ
void ed_link_select() {
	if (g_ed.selection.empty()) { ed_set_status(L"点を選んでください。"); return; }
	const std::vector<EdRef> from = g_ed.selection;
	g_ed.selection.clear();
	std::vector<std::pair<int, int>> done;
	for (const EdRef& r : from) {
		const EdGlyph& g = g_ed.model[r.g];
		const std::vector<int> islands = contour_islands(g, g_ed.shape);
		const std::pair<int, int> id{ r.g, islands[r.c] };
		if (std::find(done.begin(), done.end(), id) != done.end()) continue;
		done.push_back(id);
		for (int c = 0; c < (int)g.contours.size(); c++) {
			if (islands[c] != id.second) continue;
			for (int n = 0; n < (int)g.contours[c].nodes.size(); n++) g_ed.selection.push_back({ 0, r.g, c, n });
		}
	}
	ed_set_status(std::to_wstring(g_ed.selection.size()) + L"個の点を選択中");
}

// 2点: その間の直線で切り分ける / 4点: 交差する2本の線を組み替える
void ed_split() {
	const std::vector<EdRef> anchors = ed_selected_anchors();
	if (anchors.size() != 2 && anchors.size() != 4) { ed_set_status(L"カットは2点、組み替えは4点を選んでください。"); return; }
	const int gi = anchors[0].g;
	for (const EdRef& r : anchors) if (r.g != gi) { ed_set_status(L"別々の文字の点は使えません。"); return; }
	EdGlyph& g = g_ed.model[gi];
	const std::vector<int> islands = contour_islands(g, g_ed.shape);
	for (const EdRef& r : anchors) if (islands[r.c] != islands[anchors[0].c]) { ed_set_status(L"同じアイランドの点を選んでください。"); return; }
	ed_push_history();
	if (anchors.size() == 2) {
		if (anchors[0].c != anchors[1].c) {
			g_ed.history.pop_back();
			ed_set_status(L"外周と穴の間は1本の線では分けられません。同じ輪郭の2点を選んでください。");
			return;
		}
		if (!split_contour(g, anchors[0].c, anchors[0].n, anchors[1].n)) {
			g_ed.history.pop_back();
			ed_set_status(L"隣り合う点の間では切り分けられません。");
			return;
		}
		ed_set_status(L"2点の間で輪郭を切り分けました。");
	} else {
		std::vector<std::pair<int, int>> points;
		for (const EdRef& r : anchors) points.push_back({ r.c, r.n });
		if (!cross_contours(g, points, g_ed.shape)) {
			g_ed.history.pop_back();
			ed_set_status(L"4点から交差する2本の線を決められませんでした。交差の周りの4点を選んでください。");
			return;
		}
		ed_set_status(L"輪郭を組み替えました。");
	}
	g_ed.selection.clear();
}

void ed_move_by_key(int command, bool repeat) {
	if (g_ed.selection.empty()) return;
	double step = g_move_step;
	if (GetKeyState(VK_SHIFT) < 0) step *= g_move_shift;
	if (GetKeyState(VK_CONTROL) < 0) step *= g_move_ctrl;
	if (!repeat || !g_ed.key_moving) ed_push_history();
	g_ed.key_moving = true;
	ed_move_selection(command == ID_MOVE_LEFT ? -step : command == ID_MOVE_RIGHT ? step : 0, command == ID_MOVE_UP ? -step : command == ID_MOVE_DOWN ? step : 0);
}

std::wstring ed_selection_text() {
	if (g_ed.selection.empty()) return g_ed.stroke_mode ? ED_STROKE_HINT : ED_HINT;
	if (g_ed.selection.size() > 3) return std::to_wstring(g_ed.selection.size()) + L"個を選択中";
	std::wstring text;
	for (const EdRef& r : g_ed.selection) {
		text += L"文字" + std::to_wstring(r.g + 1) + (g_ed.stroke_mode ? L" 画" : L" 輪郭") + std::to_wstring(r.c + 1) + L" 点" + std::to_wstring(r.n + 1) + (r.kind ? L"のハンドル / " : L" / ");
	}
	return text + L"を選択中";
}

void ed_layout_buttons();
void ed_toggle_stroke_mode();

//	書き順の確認

const UINT_PTR ED_PREVIEW_TIMER = 1;

void ed_set_preview_slider() {
	SendDlgItemMessageW(g_ed.hwnd, ID_PREVIEW_SLIDER, TBM_SETPOS, TRUE, (LPARAM)std::lround(g_ed.preview_progress * 1000));
}

void ed_stop_preview_play() {
	KillTimer(g_ed.hwnd, ED_PREVIEW_TIMER);
	CheckDlgButton(g_ed.hwnd, ID_PREVIEW_PLAY, BST_UNCHECKED);
}

// 今の輪郭と筆順で時刻の表を作り、画とアイランドの数を知らせる
void ed_open_preview() {
	std::vector<double> points, meta;
	save_model(g_ed.model, 0, g_ed.header[0], points, meta);
	// 編集していない字もウィンドウに出ている線で確認する
	std::vector<GlyphStrokes> lines = g_ed.strokes;
	for (GlyphStrokes& glyph : lines) glyph.custom = !glyph.lines.empty();
	g_ed.preview_map = build_pen_map(points, meta, 0, lines, points);
	g_ed.previewing = true;
	g_ed.selection.clear();
	g_ed.preview_progress = 0;
	ed_set_preview_slider();
	ed_layout_buttons();
	std::wstring text = L"再生かスライダーで確認します。";
	int unmatched = 0;
	for (size_t g = 0; g < g_ed.model.size(); g++) {
		const std::vector<int> islands = contour_islands(g_ed.model[g], 0);
		const int count = islands.empty() ? 0 : *std::max_element(islands.begin(), islands.end()) + 1;
		if (count != (int)g_ed.strokes[g].lines.size()) unmatched++;
	}
	if (unmatched) text += L" " + std::to_wstring(unmatched) + L"文字は画数とアイランドの数が違います(分割の案で分けると正確になります)。";
	ed_set_status(text);
}

void ed_close_preview() {
	ed_stop_preview_play();
	g_ed.previewing = false;
	g_ed.preview_map.reset();
	ed_layout_buttons();
}

//	アイランド分割の適用

// 別々の輪郭の2点を幅0の線でつないで1つの輪郭にする(穴と外周の間を切る時の下準備。形は変わらない)
void bridge_contours(EdGlyph& glyph, int ca, int a, int cb, int b) {
	std::vector<EdNode> first = rotated(glyph.contours[ca], a);
	std::vector<EdNode> second = rotated(glyph.contours[cb], b);
	EdNode first_copy = first[0], second_copy = second[0];
	first_copy.next = EdSeg();
	second_copy.next = EdSeg();
	EdContour joined;
	joined.nodes = first;
	joined.nodes.push_back(first_copy);
	joined.nodes.insert(joined.nodes.end(), second.begin(), second.end());
	joined.nodes.push_back(second_copy);
	const int low = std::min(ca, cb), high = std::max(ca, cb);
	glyph.contours[low] = std::move(joined);
	glyph.contours.erase(glyph.contours.begin() + high);
}

// 位置にある点(複製を含む)を集める。無ければ最も近い辺に足す
std::vector<std::pair<int, int>> resolve_point(EdGlyph& glyph, Vec at) {
	auto collect = [&](double reach) {
		std::vector<std::pair<int, int>> found;
		for (int c = 0; c < (int)glyph.contours.size(); c++) {
			for (int n = 0; n < (int)glyph.contours[c].nodes.size(); n++) {
				if (distance(glyph.contours[c].nodes[n].p[0], at) <= reach) found.push_back({ c, n });
			}
		}
		return found;
	};
	std::vector<std::pair<int, int>> found = collect(1.0);
	if (!found.empty()) return found;
	double best = 1e30, best_t = 0;
	int best_c = -1, best_n = -1;
	for (int c = 0; c < (int)glyph.contours.size(); c++) {
		const EdContour& contour = glyph.contours[c];
		const int size = (int)contour.nodes.size();
		for (int n = 0; n < size; n++) {
			const EdNode& a = contour.nodes[n];
			const EdNode& b = contour.nodes[(n + 1) % size];
			for (int k = 1; k < 64; k++) {
				const double t = k / 64.0, u = 1 - t;
				Vec v = lerp(a.p[0], b.p[0], t);
				if (a.next.curve) {
					v = { u * u * u * a.p[0].x + 3 * u * u * t * a.next.c1[0].x + 3 * u * t * t * a.next.c2[0].x + t * t * t * b.p[0].x,
						u * u * u * a.p[0].y + 3 * u * u * t * a.next.c1[0].y + 3 * u * t * t * a.next.c2[0].y + t * t * t * b.p[0].y };
				}
				const double d = distance(v, at);
				if (d < best) { best = d; best_c = c; best_n = n; best_t = t; }
			}
		}
	}
	if (best_c < 0) return found;
	split_segment(glyph.contours[best_c], best_n, best_t);
	found.push_back({ best_c, best_n + 1 });
	return found;
}

bool same_shape(const EdGlyph& before, const EdGlyph& after);

// 1つの案を文字へ適用する。できなければ文字はそのまま
bool apply_proposal(EdGlyph& glyph, const Proposal& p) {
	EdGlyph work = glyph;
	// 足す点で添字がずれるので、先に全ての点を足してから集め直す
	std::vector<Vec> places;
	for (const Vec& v : p.points) {
		const std::vector<std::pair<int, int>> found = resolve_point(work, v);
		if (found.empty()) return false;
		const EdNode& node = work.contours[found[0].first].nodes[found[0].second];
		places.push_back(distance(node.p[0], v) <= 1.0 ? v : node.p[0]);
	}
	std::vector<std::vector<std::pair<int, int>>> copies;
	for (const Vec& v : places) copies.push_back(resolve_point(work, v));
	const std::vector<int> islands = contour_islands(work, 0);
	if (p.points.size() == 2) {
		// 同じ輪郭の組を優先する
		for (const auto& a : copies[0]) {
			for (const auto& b : copies[1]) {
				if (a.first != b.first) continue;
				EdGlyph trial = work;
				if (split_contour(trial, a.first, a.second, b.second)) { glyph = std::move(trial); return true; }
			}
		}
		for (const auto& a : copies[0]) {
			for (const auto& b : copies[1]) {
				if (a.first == b.first || islands[a.first] != islands[b.first]) continue;
				bridge_contours(work, a.first, a.second, b.first, b.second);
				glyph = std::move(work);
				return true;
			}
		}
		return false;
	}
	// 重なった点があれば、組み替えられる組み合わせを探す
	std::vector<size_t> pick(copies.size(), 0);
	for (;;) {
		std::vector<std::pair<int, int>> points;
		bool usable = true;
		for (size_t i = 0; i < copies.size(); i++) {
			points.push_back(copies[i][pick[i]]);
			if (islands[points[i].first] != islands[points[0].first]) usable = false;
			for (size_t k = 0; k < i; k++) if (points[k] == points[i]) usable = false;
		}
		if (usable && copies.size() > 4) {
			// 多叉路: 集まる画の数に分かれるものを優先し、その中で最もまっすぐ貫くものを使う。無ければ分かれる数を減らして探す
			const std::vector<std::vector<int>> candidates = cross_candidates(work, points, 0, 2000);
			std::vector<int> grown(candidates.size(), -1);
			const int base = (int)glyph.contours.size();
			for (size_t c = 0; c < candidates.size(); c++) {
				EdGlyph trial = work;
				if (cross_contours(trial, points, 0, &candidates[c])) grown[c] = (int)trial.contours.size() - base;
			}
			for (int need = std::max(p.junction_strokes - 1, 1); need >= 1; need--) {
				for (size_t c = 0; c < candidates.size(); c++) {
					if (grown[c] != need) continue;
					EdGlyph trial = work;
					if (cross_contours(trial, points, 0, &candidates[c]) && same_shape(glyph, trial)) {
						glyph = std::move(trial);
						return true;
					}
				}
			}
		} else if (usable) {
			EdGlyph trial = work;
			if (cross_contours(trial, points, 0)) { glyph = std::move(trial); return true; }
		}
		size_t digit = 0;
		while (digit < pick.size() && ++pick[digit] == copies[digit].size()) pick[digit++] = 0;
		if (digit == pick.size()) return false;
	}
}

// 文字の塗り(アイランドごとに塗って濃い方を取る。描画と同じ塗り方)
std::vector<PIXEL_RGBA> glyph_coverage(const EdGlyph& glyph, double x0, double y0, int w, int h) {
	std::vector<PIXEL_RGBA> result((size_t)w * h);
	if (glyph.contours.empty()) return result;
	const std::vector<int> islands = contour_islands(glyph, 0);
	const int count = *std::max_element(islands.begin(), islands.end()) + 1;
	std::vector<PIXEL_RGBA> temp((size_t)w * h);
	for (int island = 0; island < count; island++) {
		Rasterizer raster(w, h);
		for (int c = 0; c < (int)glyph.contours.size(); c++) {
			if (islands[c] != island) continue;
			const std::vector<Vec> line = flatten_contour(glyph.contours[c], 0);
			for (size_t i = 0; i < line.size(); i++) {
				const Vec a = line[i], b = line[(i + 1) % line.size()];
				raster.line({ a.x - x0, a.y - y0 }, { b.x - x0, b.y - y0 });
			}
		}
		raster.write(temp.data(), 0, 0, 0);
		for (size_t i = 0; i < temp.size(); i++) result[i].a = std::max(result[i].a, temp[i].a);
	}
	return result;
}

// 2つの輪郭の塗りが同じか(切った線の周りの丸めの差は許す)
bool same_shape(const EdGlyph& glyph, const EdGlyph& trial) {
	bool any = false;
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
	for (const EdContour& c : glyph.contours) {
		for (const Vec& v : flatten_contour(c, 0)) {
			if (!any) { x0 = x1 = v.x; y0 = y1 = v.y; any = true; }
			x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
			y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
		}
	}
	if (!any) return false;
	const double left = std::floor(x0) - 3, top = std::floor(y0) - 3;
	const int w = (int)(std::ceil(x1) + 3 - left), h = (int)(std::ceil(y1) + 3 - top);
	const std::vector<PIXEL_RGBA> before = glyph_coverage(glyph, left, top, w, h);
	const std::vector<PIXEL_RGBA> after = glyph_coverage(trial, left, top, w, h);
	size_t changed = 0, ink = 0;
	for (size_t i = 0; i < before.size(); i++) {
		if (before[i].a >= 128) ink++;
		if (std::abs((int)before[i].a - (int)after[i].a) >= 128) changed++;
	}
	return changed <= std::max<size_t>(8, ink / 500);
}

// 案を適用して、字の形が変わらない時だけ確定する(誤った案で穴が埋まったり線が消えたりしないように)
bool apply_proposal_checked(EdGlyph& glyph, const Proposal& p) {
	EdGlyph trial = glyph;
	if (!apply_proposal(trial, p) || !same_shape(glyph, trial)) return false;
	glyph = std::move(trial);
	return true;
}

void ed_open_proposals() {
	g_ed.proposals.clear();
	int missed = 0;
	for (int g = 0; g < (int)g_ed.model.size(); g++) {
		std::vector<Proposal> list = propose_splits(g_ed.model[g], g_ed.strokes[g].lines, g, missed);
		g_ed.proposals.insert(g_ed.proposals.end(), list.begin(), list.end());
	}
	g_ed.proposing = true;
	g_ed.selection.clear();
	ed_layout_buttons();
	CheckDlgButton(g_ed.hwnd, ID_PROPOSE_KIND, BST_UNCHECKED);
	std::wstring text = std::to_wstring(g_ed.proposals.size()) + L"件の案。クリックで採用・不採用、Shift+クリックで種類を変更、点のドラッグで修正、Enterで適用します。";
	if (missed) text += L" 自動で切り方を決められなかった接合が" + std::to_wstring(missed) + L"か所あります。";
	ed_set_status(text);
}

// 案の切り方を次の種類へ切り替える(交差⇔T字など)。点を動かしていた時は、選んだ種類の既定の点に戻る
void ed_cycle_proposal(Proposal& p) {
	if (p.variants.size() < 2) { ed_set_status(L"この案には切り替えられる種類がありません。"); return; }
	p.variant = (p.variant + 1) % (int)p.variants.size();
	p.kind = p.variants[p.variant].kind;
	p.points = p.variants[p.variant].points;
	p.on = true;
	p.failed = false;
	const wchar_t* name = p.kind == Proposal::CROSS ? L"交差" : p.kind == Proposal::CORNER ? L"切る線"
		: p.points.size() > 2 ? L"T字(共通部分は両方)" : L"T字(共通部分は貫く画)";
	ed_set_status(std::wstring(L"案を「") + name + L"」に変えました(" + std::to_wstring(p.variant + 1) + L"/" + std::to_wstring(p.variants.size()) + L")。");
}

// 案を手で足す操作を終える。2点なら切る線、4点なら組み替えの案になる。それ以外は捨てる
void ed_finish_adding_proposal() {
	if (!g_ed.adding_proposal) return;
	g_ed.adding_proposal = false;
	CheckDlgButton(g_ed.hwnd, ID_PROPOSE_ADD, BST_UNCHECKED);
	const size_t count = g_ed.proposals.back().points.size();
	if (count == 2) {
		g_ed.proposals.back().kind = Proposal::CORNER;
		ed_set_status(L"切る線の案を足しました。");
	} else if (count == 4) {
		g_ed.proposals.back().kind = Proposal::CROSS;
		ed_set_status(L"組み替えの案を足しました。");
	} else {
		g_ed.proposals.pop_back();
		ed_set_status(L"案は2点(切る線)か4点(組み替え)で作ります。");
	}
}

// 案を手で足している時のクリック: 近くに輪郭の点があればその点を使う。4点で完成する
void ed_add_proposal_point(int x, int y) {
	Proposal& p = g_ed.proposals.back();
	Vec at{ (x - g_ed.pan_x) / g_ed.zoom, (y - g_ed.pan_y) / g_ed.zoom };
	double best = 1e30, best_screen = 9;
	int glyph = 0;
	for (int g = 0; g < (int)g_ed.model.size(); g++) {
		for (const EdContour& c : g_ed.model[g].contours) {
			for (const EdNode& n : c.nodes) {
				const double d = distance(n.p[0], at);
				if (d < best) { best = d; glyph = g; }
				const POINT q = to_screen(n.p[0]);
				const double screen = std::hypot((double)q.x - x, (double)q.y - y);
				if (screen < best_screen) { best_screen = screen; at = n.p[0]; }
			}
		}
	}
	if (p.points.empty()) p.glyph = glyph;
	p.points.push_back(at);
	if (p.points.size() == 4) ed_finish_adding_proposal();
	else ed_set_status(std::to_wstring(p.points.size()) + L"点。4点目で組み替えの案になります。2点で切る線にする時はEnterを押します。");
}

void ed_close_proposals() {
	g_ed.adding_proposal = false;
	g_ed.proposing = false;
	g_ed.proposals.clear();
	g_ed.drag_proposal = -1;
	ed_layout_buttons();
}

// 採用した案を適用して、結果が見える輪郭の編集へ戻る(「戻す」で適用前に戻せる)
void ed_apply_proposals() {
	ed_push_history();
	g_ed.model_dirty = true;
	// 交差を先に行う: 切る線より先に組み替えた方が、点の対応が崩れにくい
	int done = 0, failed = 0;
	for (const Proposal::Kind kind : { Proposal::CROSS, Proposal::TEE, Proposal::CORNER }) {
		for (Proposal& p : g_ed.proposals) {
			if (p.kind != kind || !p.on) continue;
			p.failed = !apply_proposal_checked(g_ed.model[p.glyph], p);
			if (p.failed) failed++;
			else done++;
		}
	}
	if (failed) {
		// 適用できなかった案が見えるように案の表示のままにする
		ed_set_status(std::to_wstring(done) + L"件を適用し、" + std::to_wstring(failed) + L"件(赤)は適用できませんでした。点を動かすか不採用にして、もう一度適用してください。");
		for (Proposal& p : g_ed.proposals) if (p.on && !p.failed) p.on = false;
		return;
	}
	std::wstring summary;
	for (size_t g = 0; g < g_ed.model.size(); g++) {
		const std::vector<int> islands = contour_islands(g_ed.model[g], 0);
		const int count = islands.empty() ? 0 : *std::max_element(islands.begin(), islands.end()) + 1;
		summary += L" 文字" + std::to_wstring(g + 1) + L": " + std::to_wstring(g_ed.strokes[g].lines.size()) + L"画 → " + std::to_wstring(count) + L"アイランド";
		if (summary.size() > 90) break;
	}
	ed_close_proposals();
	ed_toggle_stroke_mode();
	ed_set_status(std::to_wstring(done) + L"件を適用しました。" + summary);
}

//	筆順の編集

std::vector<EdRef> ed_stroke_hits(int x, int y) {
	std::vector<EdRef> hits;
	for (int gi = 0; gi < (int)g_ed.strokes.size(); gi++) {
		const auto& lines = g_ed.strokes[gi].lines;
		for (int si = 0; si < (int)lines.size(); si++) {
			for (int n = 0; n < (int)lines[si].size(); n++) {
				const POINT p = to_screen(lines[si][n]);
				if (std::abs(p.x - x) <= 7 && std::abs(p.y - y) <= 7) hits.push_back({ 0, gi, si, n });
			}
		}
	}
	return hits;
}

// (x, y)に最も近い画の線の上に点を足す
bool ed_stroke_insert(int x, int y) {
	double best = 10;
	int found_g = -1, found_s = -1, found_n = -1;
	Vec found_at{};
	for (int gi = 0; gi < (int)g_ed.strokes.size(); gi++) {
		const auto& lines = g_ed.strokes[gi].lines;
		for (int si = 0; si < (int)lines.size(); si++) {
			for (int n = 0; n + 1 < (int)lines[si].size(); n++) {
				const Vec a = lines[si][n], b = lines[si][n + 1];
				const double ax = a.x * g_ed.zoom + g_ed.pan_x, ay = a.y * g_ed.zoom + g_ed.pan_y;
				const double bx = b.x * g_ed.zoom + g_ed.pan_x, by = b.y * g_ed.zoom + g_ed.pan_y;
				const double dx = bx - ax, dy = by - ay, square = dx * dx + dy * dy;
				if (square <= 0) continue;
				const double t = ((x - ax) * dx + (y - ay) * dy) / square;
				if (t <= 0 || t >= 1) continue;
				const double d = std::hypot(ax + dx * t - x, ay + dy * t - y);
				if (d < best) { best = d; found_g = gi; found_s = si; found_n = n; found_at = lerp(a, b, t); }
			}
		}
	}
	if (found_g < 0) return false;
	ed_push_history();
	auto& line = g_ed.strokes[found_g].lines[found_s];
	line.insert(line.begin() + found_n + 1, found_at);
	g_ed.strokes[found_g].custom = true;
	g_ed.selection = { { 0, found_g, found_s, found_n + 1 } };
	return true;
}

// 2点未満の画と、選択を片付ける
void ed_strokes_cleanup() {
	for (auto& glyph : g_ed.strokes) {
		glyph.lines.erase(std::remove_if(glyph.lines.begin(), glyph.lines.end(), [](const std::vector<Vec>& line) { return line.size() < 2; }), glyph.lines.end());
	}
	g_ed.selection.clear();
}

void ed_stroke_finish() {
	if (g_ed.drawing_glyph >= 0) ed_strokes_cleanup();
	g_ed.drawing_glyph = g_ed.drawing_stroke = -1;
	CheckDlgButton(g_ed.hwnd, ID_NEW_STROKE, BST_UNCHECKED);
}

// 画を追加している時のクリック: 新しい点を足す。最初の点の文字は、範囲に最も近い文字にする
void ed_stroke_add_point(int x, int y) {
	const Vec at{ (x - g_ed.pan_x) / g_ed.zoom, (y - g_ed.pan_y) / g_ed.zoom };
	ed_push_history();
	if (g_ed.drawing_glyph < 0) {
		double best = 1e30;
		for (int gi = 0; gi < (int)g_ed.model.size(); gi++) {
			for (const EdContour& c : g_ed.model[gi].contours) {
				for (const EdNode& n : c.nodes) {
					const double d = distance(n.p[0], at);
					if (d < best) { best = d; g_ed.drawing_glyph = gi; }
				}
			}
		}
		if (g_ed.drawing_glyph < 0) { g_ed.history.pop_back(); return; }
		auto& lines = g_ed.strokes[g_ed.drawing_glyph].lines;
		lines.emplace_back();
		g_ed.drawing_stroke = (int)lines.size() - 1;
	}
	g_ed.strokes[g_ed.drawing_glyph].custom = true;
	g_ed.strokes[g_ed.drawing_glyph].lines[g_ed.drawing_stroke].push_back(at);
}

// 選んでいる点の画(複数の画を選んでいる時は最初の画)
bool ed_selected_stroke(int& g, int& s) {
	if (g_ed.selection.empty()) { ed_set_status(L"画の点を選んでください。"); return false; }
	g = g_ed.selection[0].g;
	s = g_ed.selection[0].c;
	return true;
}

// 筆順の編集の時に処理したらtrue
bool ed_stroke_command(int id) {
	int g, s;
	switch (id) {
	case ID_DELETE: {
		if (g_ed.selection.empty()) { ed_set_status(L"削除する点を選んでください。"); return true; }
		ed_push_history();
		std::vector<EdRef> refs = g_ed.selection;
		std::sort(refs.begin(), refs.end());
		for (auto it = refs.rbegin(); it != refs.rend(); ++it) {
			auto& line = g_ed.strokes[it->g].lines[it->c];
			line.erase(line.begin() + it->n);
			g_ed.strokes[it->g].custom = true;
		}
		ed_strokes_cleanup();
		ed_set_status(L"点を削除しました(2点未満の画は消えます)。");
		return true;
	}
	case ID_LINK:
		if (!ed_selected_stroke(g, s)) return true;
		g_ed.selection.clear();
		for (int n = 0; n < (int)g_ed.strokes[g].lines[s].size(); n++) g_ed.selection.push_back({ 0, g, s, n });
		ed_set_status(L"画" + std::to_wstring(s + 1) + L"の点を全て選びました。");
		return true;
	case ID_DELETE_STROKE:
		if (!ed_selected_stroke(g, s)) return true;
		ed_push_history();
		g_ed.strokes[g].lines.erase(g_ed.strokes[g].lines.begin() + s);
		g_ed.strokes[g].custom = true;
		g_ed.selection.clear();
		ed_set_status(L"画を削除しました。");
		return true;
	case ID_STROKE_EARLIER:
	case ID_STROKE_LATER: {
		if (!ed_selected_stroke(g, s)) return true;
		const int other = id == ID_STROKE_EARLIER ? s - 1 : s + 1;
		if (other < 0 || other >= (int)g_ed.strokes[g].lines.size()) { ed_set_status(L"これ以上動かせません。"); return true; }
		ed_push_history();
		std::swap(g_ed.strokes[g].lines[s], g_ed.strokes[g].lines[other]);
		g_ed.strokes[g].custom = true;
		for (EdRef& r : g_ed.selection) {
			if (r.g == g && r.c == s) r.c = other;
			else if (r.g == g && r.c == other) r.c = s;
		}
		ed_set_status(L"画" + std::to_wstring(other + 1) + L"番目にしました。");
		return true;
	}
	case ID_STROKE_REVERSE:
		if (!ed_selected_stroke(g, s)) return true;
		ed_push_history();
		std::reverse(g_ed.strokes[g].lines[s].begin(), g_ed.strokes[g].lines[s].end());
		g_ed.strokes[g].custom = true;
		g_ed.selection.clear();
		ed_set_status(L"画の向きを反転しました。");
		return true;
	case ID_STROKE_RESET:
		// 既定の筆順は字全体の画なので、1つのアイランドには当てはめられない
		if (g_ed.expanded) { ed_set_status(L"アイランドごとに展開したオブジェクトでは、筆順を既定に戻せません。"); return true; }
		// 選んでいる点の文字、選んでいなければ全ての文字を既定に戻す
		ed_push_history();
		for (int gi = 0; gi < (int)g_ed.strokes.size(); gi++) {
			if (!g_ed.selection.empty() && gi != g_ed.selection[0].g) continue;
			g_ed.strokes[gi].custom = false;
			g_ed.strokes[gi].lines = default_strokes(g_ed.model[gi]);
		}
		g_ed.selection.clear();
		ed_set_status(L"筆順を既定に戻しました。");
		return true;
	case ID_STROKE_FINISH:
		ed_stroke_finish();
		ed_set_status(L"画の追加を終えました。");
		return true;
	case ID_ADD_HANDLE: case ID_DELETE_HANDLE: case ID_DUPLICATE: case ID_RECONNECT: case ID_SPLIT: case ID_TOGGLE_HANDLE:
		ed_set_status(L"この操作は輪郭の編集で使えます。");
		return true;
	}
	return false;
}

const int ED_OUTLINE_BUTTONS[] = { ID_DELETE, ID_ADD_HANDLE, ID_DELETE_HANDLE, ID_DUPLICATE, ID_RECONNECT, ID_SPLIT };
const int ED_STROKE_BUTTONS[] = { ID_DELETE, ID_NEW_STROKE, ID_DELETE_STROKE, ID_STROKE_EARLIER, ID_STROKE_LATER, ID_STROKE_REVERSE, ID_STROKE_RESET, ID_PROPOSE, ID_PREVIEW };

// ツールバーのボタンを、今の編集の種類に合わせて並べる
void ed_layout_buttons() {
	const int proposal_only[] = { ID_APPLY, ID_PROPOSE_KIND, ID_PROPOSE_ADD, ID_PROPOSE_ALL, ID_PROPOSE_CLOSE };
	const int outline_only[] = { ID_TOGGLE_HANDLE, ID_DUPLICATE, ID_RECONNECT, ID_SPLIT };
	const int stroke_only[] = { ID_NEW_STROKE, ID_DELETE_STROKE, ID_STROKE_EARLIER, ID_STROKE_LATER, ID_STROKE_REVERSE, ID_STROKE_RESET, ID_PROPOSE, ID_PREVIEW };
	const int common[] = { ID_STROKE_MODE, ID_ADD, ID_DELETE, ID_UNDO, ID_OK, ID_CANCEL };
	const int preview_only[] = { ID_PREVIEW_PLAY, ID_PREVIEW_SLIDER, ID_MAKE_PEN, ID_PREVIEW_CLOSE };
	const bool normal = !g_ed.proposing && !g_ed.previewing;
	for (int id : proposal_only) ShowWindow(GetDlgItem(g_ed.hwnd, id), g_ed.proposing ? SW_SHOW : SW_HIDE);
	for (int id : preview_only) ShowWindow(GetDlgItem(g_ed.hwnd, id), g_ed.previewing ? SW_SHOW : SW_HIDE);
	for (int id : outline_only) ShowWindow(GetDlgItem(g_ed.hwnd, id), normal && !g_ed.stroke_mode ? SW_SHOW : SW_HIDE);
	for (int id : stroke_only) ShowWindow(GetDlgItem(g_ed.hwnd, id), normal && g_ed.stroke_mode ? SW_SHOW : SW_HIDE);
	for (int id : common) ShowWindow(GetDlgItem(g_ed.hwnd, id), normal ? SW_SHOW : SW_HIDE);
	ShowWindow(GetDlgItem(g_ed.hwnd, ID_FIT), g_ed.previewing ? SW_HIDE : SW_SHOW);
	// 左の列に3列で並べる。区切りごとに行を変える
	const int size_w = 36, size_h = 32, gap = 2, margin = 4;
	int column = 0, row_y = margin;
	auto place = [&](int id) {
		SetWindowPos(GetDlgItem(g_ed.hwnd, id), nullptr, margin + column * (size_w + gap), row_y, size_w, size_h, SWP_NOZORDER | SWP_NOACTIVATE);
		if (++column == 3) { column = 0; row_y += size_h + gap; }
	};
	auto next_group = [&]() {
		if (column) { column = 0; row_y += size_h + gap; }
		row_y += 8;
	};
	const int wide = ED_PANEL - margin * 2;
	// 利用頻度の高いものは横長のボタンにする
	auto place_wide = [&](int id) {
		SetWindowPos(GetDlgItem(g_ed.hwnd, id), nullptr, margin, row_y, wide, size_h, SWP_NOZORDER | SWP_NOACTIVATE);
		row_y += size_h + gap;
	};
	// 縦一列に置く
	auto place_single = [&](int id) {
		SetWindowPos(GetDlgItem(g_ed.hwnd, id), nullptr, margin, row_y, size_w, size_h, SWP_NOZORDER | SWP_NOACTIVATE);
		row_y += size_h + gap;
	};
	RECT client;
	GetClientRect(g_ed.hwnd, &client);
	if (g_ed.proposing) {
		place_wide(ID_APPLY);
		place_wide(ID_PROPOSE_CLOSE);
		row_y += 8;
		for (int id : { ID_PROPOSE_ADD, ID_PROPOSE_KIND, ID_PROPOSE_ALL }) place_single(id);
		row_y += 8;
		place_single(ID_FIT);
		return;
	}
	if (g_ed.previewing) {
		for (int id : { ID_PREVIEW_PLAY, ID_MAKE_PEN, ID_PREVIEW_CLOSE }) place(id);
		SetWindowPos(GetDlgItem(g_ed.hwnd, ID_PREVIEW_SLIDER), nullptr, ED_PANEL + 8, client.bottom - ED_STATUS - 34,
			std::max<int>(client.right - ED_PANEL - 16, 100), 28, SWP_NOZORDER | SWP_NOACTIVATE);
		return;
	}
	place_wide(ID_STROKE_MODE);
	if (g_ed.stroke_mode) {
		place_wide(ID_PROPOSE);
		place_wide(ID_PREVIEW);
		row_y += 8;
		for (int id : { ID_ADD, ID_DELETE }) place(id);
		next_group();
		for (int id : { ID_NEW_STROKE, ID_DELETE_STROKE }) place(id);
		next_group();
		for (int id : { ID_STROKE_EARLIER, ID_STROKE_LATER, ID_STROKE_REVERSE, ID_STROKE_RESET }) place(id);
	} else {
		row_y += 8;
		for (int id : { ID_ADD, ID_DELETE, ID_DUPLICATE, ID_TOGGLE_HANDLE }) place(id);
		next_group();
		for (int id : { ID_RECONNECT, ID_SPLIT }) place(id);
	}
	next_group();
	place(ID_UNDO);
	place(ID_FIT);
	// OKとキャンセルは列の下に置く
	const int bottom = client.bottom - ED_STATUS - margin;
	SetWindowPos(GetDlgItem(g_ed.hwnd, ID_CANCEL), nullptr, margin, bottom - 28, wide, 28, SWP_NOZORDER | SWP_NOACTIVATE);
	SetWindowPos(GetDlgItem(g_ed.hwnd, ID_OK), nullptr, margin, bottom - 28 - gap - 28, wide, 28, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ボタンの絵を描く(座標は24×24)。白で描いて、周りに黒い縁を付ける
HICON make_button_icon(int id) {
	const int scale = 2, large = 24 * scale;
	BITMAPINFO info{};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = large;
	info.bmiHeader.biHeight = -large;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	void* bits = nullptr;
	HDC dc = CreateCompatibleDC(nullptr);
	HBITMAP canvas = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	HGDIOBJ old_bitmap = SelectObject(dc, canvas);
	HPEN pen = CreatePen(PS_SOLID, 2 * scale, RGB(255, 255, 255));
	HGDIOBJ old_pen = SelectObject(dc, pen);
	HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
	auto L = [&](int x0, int y0, int x1, int y1) { MoveToEx(dc, x0 * scale, y0 * scale, nullptr); LineTo(dc, x1 * scale, y1 * scale); };
	auto R = [&](int x0, int y0, int x1, int y1) { Rectangle(dc, x0 * scale, y0 * scale, x1 * scale, y1 * scale); };
	auto Solid = [&](int cx, int cy, int r) {
		SelectObject(dc, GetStockObject(WHITE_BRUSH));
		Ellipse(dc, (cx - r) * scale, (cy - r) * scale, (cx + r) * scale, (cy + r) * scale);
		SelectObject(dc, GetStockObject(NULL_BRUSH));
	};
	auto Plus = [&](int cx, int cy) { L(cx - 4, cy, cx + 4, cy); L(cx, cy - 4, cx, cy + 4); };
	auto Cross = [&](int cx, int cy) { L(cx - 3, cy - 3, cx + 3, cy + 3); L(cx + 3, cy - 3, cx - 3, cy + 3); };
	auto Tri = [&](int x0, int y0, int x1, int y1, int x2, int y2) {
		const POINT p[3] = { { x0 * scale, y0 * scale }, { x1 * scale, y1 * scale }, { x2 * scale, y2 * scale } };
		SelectObject(dc, GetStockObject(WHITE_BRUSH));
		Polygon(dc, p, 3);
		SelectObject(dc, GetStockObject(NULL_BRUSH));
	};
	auto Curve = [&]() { L(3, 20, 13, 10); Solid(8, 15, 3); };
	auto Handle = [&]() { L(3, 15, 15, 15); Solid(9, 15, 2); Solid(3, 15, 2); Solid(15, 15, 2); };
	auto Stroke = [&]() { L(3, 21, 7, 13); L(7, 13, 14, 12); };
	switch (id) {
	case ID_STROKE_MODE:
		// 筆: 斜めの軸、金具、毛の穂先
		L(21, 3, 12, 12);
		L(10, 10, 14, 14);
		Tri(11, 11, 14, 14, 3, 21);
		Tri(11, 11, 8, 15, 3, 21);
		break;
	case ID_ADD: Curve(); Plus(18, 6); break;
	case ID_DELETE: Curve(); Cross(18, 6); break;
	case ID_TOGGLE_HANDLE: Handle(); L(13, 4, 21, 4); L(21, 4, 18, 1); L(21, 8, 13, 8); L(13, 8, 16, 11); break;
	case ID_DUPLICATE: R(3, 3, 15, 15); R(9, 9, 21, 21); break;
	case ID_RECONNECT: Solid(5, 6, 3); Solid(19, 18, 3); L(5, 6, 12, 6); L(12, 6, 12, 18); L(12, 18, 19, 18); break;
	case ID_SPLIT: Solid(6, 6, 3); Solid(6, 18, 3); L(7, 7, 21, 18); L(7, 17, 21, 6); break;
	case ID_UNDO: L(8, 8, 15, 8); Arc(dc, 11 * scale, 8 * scale, 21 * scale, 18 * scale, 16 * scale, 18 * scale, 16 * scale, 8 * scale); L(15, 18, 6, 18); L(8, 8, 12, 4); L(8, 8, 12, 12); break;
	case ID_FIT: L(3, 9, 3, 3); L(3, 3, 9, 3); L(15, 3, 21, 3); L(21, 3, 21, 9); L(21, 15, 21, 21); L(21, 21, 15, 21); L(9, 21, 3, 21); L(3, 21, 3, 15); break;
	case ID_NEW_STROKE: Stroke(); Plus(18, 6); break;
	case ID_DELETE_STROKE: Stroke(); Cross(18, 6); break;
	case ID_STROKE_EARLIER: L(12, 20, 12, 5); L(12, 5, 6, 11); L(12, 5, 18, 11); break;
	case ID_STROKE_LATER: L(12, 4, 12, 19); L(12, 19, 6, 13); L(12, 19, 18, 13); break;
	case ID_STROKE_REVERSE: L(4, 8, 20, 8); L(20, 8, 16, 4); L(20, 8, 16, 12); L(20, 16, 4, 16); L(4, 16, 8, 12); L(4, 16, 8, 20); break;
	case ID_STROKE_RESET: Arc(dc, 4 * scale, 4 * scale, 20 * scale, 20 * scale, 4 * scale, 12 * scale, 12 * scale, 4 * scale); L(12, 4, 8, 1); L(12, 4, 8, 8); break;
	case ID_PROPOSE:
		// 割っているパピコ: 飲み口から2本に開く、太い筒が2本
		for (const int side : { -1, 1 }) {
			L(12 + side * 2, 9, 12 + side * 8, 20);
			L(12 + side * 1, 9, 12 + side * 7, 21);
			L(12 + side * 3, 8, 12 + side * 9, 19);
			L(12 + side * 1, 3, 12 + side * 2, 8);
		}
		break;
	case ID_PREVIEW: Ellipse(dc, 3 * scale, 3 * scale, 21 * scale, 21 * scale); Tri(10, 8, 10, 16, 16, 12); break;
	case ID_APPLY: L(4, 13, 10, 19); L(10, 19, 20, 6); break;
	case ID_PROPOSE_KIND:
		// T/X を斜めに並べる
		L(2, 3, 10, 3); L(6, 3, 6, 11);
		L(18, 3, 6, 21);
		L(14, 13, 22, 21); L(22, 13, 14, 21);
		break;
	case ID_PROPOSE_ADD: L(4, 20, 14, 20); Solid(5, 20, 2); Plus(16, 8); break;
	case ID_PROPOSE_ALL: L(3, 8, 7, 12); L(7, 12, 13, 4); L(10, 18, 14, 22); L(14, 22, 21, 13); break;
	case ID_PREVIEW_PLAY: Tri(7, 4, 7, 20, 20, 12); break;
	case ID_MAKE_PEN: R(3, 6, 21, 19); L(8, 6, 8, 19); L(16, 6, 16, 19); break;
	case ID_PROPOSE_CLOSE:
	case ID_PREVIEW_CLOSE: L(6, 6, 18, 18); L(18, 6, 6, 18); break;
	}
	SelectObject(dc, old_pen);
	SelectObject(dc, old_brush);
	SelectObject(dc, old_bitmap);
	DeleteObject(pen);
	DeleteDC(dc);
	// 半分に縮めて、白い濃さと、周りに広げた黒い縁で塗る
	const auto* source = static_cast<const UINT32*>(bits);
	const int size = 24;
	std::vector<float> white((size_t)size * size, 0);
	for (int y = 0; y < large; y++) {
		for (int x = 0; x < large; x++) {
			if (source[(size_t)y * large + x] & 0xff) white[(size_t)(y / scale) * size + x / scale] += 0.25f;
		}
	}
	BITMAPINFO small_info = info;
	small_info.bmiHeader.biWidth = size;
	small_info.bmiHeader.biHeight = -size;
	void* small_bits = nullptr;
	HBITMAP color = CreateDIBSection(nullptr, &small_info, DIB_RGB_COLORS, &small_bits, nullptr, 0);
	auto* out = static_cast<UINT32*>(small_bits);
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			float edge = 0;
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					const int nx = x + dx, ny = y + dy;
					if (nx >= 0 && nx < size && ny >= 0 && ny < size) edge = std::max(edge, white[(size_t)ny * size + nx]);
				}
			}
			const float w = white[(size_t)y * size + x];
			const float alpha = std::max(w, edge * 0.6f);
			const UINT32 level = alpha > 0 ? (UINT32)(255 * w / alpha) : 0;
			out[(size_t)y * size + x] = ((UINT32)(alpha * 255) << 24) | (level << 16) | (level << 8) | level;
		}
	}
	HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
	ICONINFO icon_info{ TRUE, 0, 0, mask, color };
	HICON icon = CreateIconIndirect(&icon_info);
	DeleteObject(mask);
	DeleteObject(color);
	DeleteObject(canvas);
	return icon;
}

// キーの表示名
std::wstring key_name(int key) {
	std::wstring name;
	if (key & KEY_CTRL) name += L"Ctrl+";
	if (key & KEY_SHIFT) name += L"Shift+";
	const int vk = key & 0xff;
	switch (vk) {
	case VK_DELETE: return name + L"Delete";
	case VK_ESCAPE: return name + L"Esc";
	case VK_RETURN: return name + L"Enter";
	case VK_TAB: return name + L"Tab";
	case VK_HOME: return name + L"Home";
	}
	wchar_t text[32] = {};
	const LONG scan = (LONG)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
	if (GetKeyNameTextW(scan, text, 32) > 0) return name + text;
	return name + (wchar_t)vk;
}

// ボタンのショートカットの表示("(A)"や"(Delete / X)")。割り当てが無ければ空
std::wstring shortcut_text(int id) {
	int command = id;
	if (id == ID_APPLY) command = ID_OK;
	else if (id == ID_PROPOSE_CLOSE || id == ID_PREVIEW_CLOSE) command = ID_CANCEL;
	for (const Shortcut& s : g_shortcuts) {
		if (s.command != command || !(s.keys[0] || s.keys[1])) continue;
		std::wstring text = L" (";
		if (s.keys[0]) text += key_name(s.keys[0]);
		if (s.keys[0] && s.keys[1]) text += L" / ";
		if (s.keys[1]) text += key_name(s.keys[1]);
		return text + L")";
	}
	return L"";
}

void ed_toggle_stroke_mode() {
	ed_stroke_finish();
	g_ed.adding_stroke = false;
	g_ed.stroke_mode = !g_ed.stroke_mode;
	if (g_ed.stroke_mode && !g_ed.strokes_ready) {
		for (size_t g = 0; g < g_ed.strokes.size(); g++) {
			if (!g_ed.strokes[g].custom) g_ed.strokes[g].lines = default_strokes(g_ed.model[g]);
		}
		g_ed.strokes_ready = true;
	}
	g_ed.selection.clear();
	// クリックの処理中に押した状態にすると、ボタンがマウスの解放でもう一度クリックを通知する(2回切り替わる)ので、処理の後で設定する
	PostMessageW(GetDlgItem(g_ed.hwnd, ID_STROKE_MODE), BM_SETSTATE, g_ed.stroke_mode, 0);
	CheckDlgButton(g_ed.hwnd, ID_ADD, BST_UNCHECKED);
	ed_layout_buttons();
	if (g_ed.stroke_mode) {
		size_t missing = 0;
		for (const GlyphStrokes& glyph : g_ed.strokes) if (glyph.lines.empty()) missing++;
		ed_set_status(missing ? std::to_wstring(missing) + L"文字は筆順データがありません。「画を追加」で引けます。" : L"番号が書き順、矢印が向きです。点をドラッグして修正できます。");
	} else {
		ed_set_status(ED_HINT);
	}
}

void ed_close() {
	EnableWindow(g_edit_handle->get_host_app_window(), TRUE);
	SetForegroundWindow(g_edit_handle->get_host_app_window());
	DestroyWindow(g_ed.hwnd);
}

void ed_apply(void*, EDIT_SECTION* edit) {
	EFFECT_HANDLE path = nullptr;
	const int effect_num = edit->get_effect_list(g_ed.object, nullptr, 0);
	std::vector<EFFECT_HANDLE> list(effect_num);
	edit->get_effect_list(g_ed.object, list.data(), effect_num);
	for (EFFECT_HANDLE effect : list) {
		if (wcsncmp(edit->get_effect_name(effect), PATH_EFFECT, wcslen(PATH_EFFECT)) == 0) { path = effect; break; }
	}
	if (!path) return;
	if (g_ed.make_pen) {
		edit->set_effect_item_value(path, L"ペンで書く", "1");
		edit->set_effect_item_value(path, L"書き進み", "0.00,100.00,直線移動,0");
	}
	if (g_ed.strokes_dirty) edit->set_effect_item_value(path, ITEM_STROKES, join_strokes(g_ed.strokes).c_str());
	if (!g_ed.model_dirty) return;
	std::vector<double> points, meta;
	save_model(g_ed.model, 0, g_ed.header[0], points, meta);
	write_points(edit, path, points, meta, ITEM_POINTS, ITEM_HANDLES, ITEM_META);
	if (g_ed.has_second) {
		save_model(g_ed.model, 1, g_ed.header[1], points, meta);
		write_points(edit, path, points, meta, ITEM_POINTS2, ITEM_HANDLES2, ITEM_META2);
	}
}

void ed_command(int id) {
	if (g_ed.previewing) {
		switch (id) {
		case ID_PREVIEW_PLAY:
			if (IsDlgButtonChecked(g_ed.hwnd, ID_PREVIEW_PLAY) == BST_CHECKED) {
				if (g_ed.preview_progress >= 1) g_ed.preview_progress = 0;
				SetTimer(g_ed.hwnd, ED_PREVIEW_TIMER, 30, nullptr);
			} else {
				ed_stop_preview_play();
			}
			break;
		case ID_MAKE_PEN:
			g_ed.make_pen = !g_ed.make_pen;
			CheckDlgButton(g_ed.hwnd, ID_MAKE_PEN, g_ed.make_pen ? BST_CHECKED : BST_UNCHECKED);
			ed_set_status(g_ed.make_pen ? L"OKで「ペンで書く」をオンにし、書き進みをオブジェクトの長さで0から100へ動かします。" : L"アニメの設定を取り消しました。");
			break;
		case ID_PREVIEW_CLOSE:
		case ID_CANCEL:
			ed_close_preview();
			ed_set_status(L"確認を終えました。");
			break;
		}
		InvalidateRect(g_ed.hwnd, nullptr, FALSE);
		return;
	}
	if (id == ID_PREVIEW) {
		if (!g_ed.stroke_mode) { ed_set_status(L"筆順の編集で使えます。"); return; }
		ed_stroke_finish();
		g_ed.adding_stroke = false;
		ed_open_preview();
		InvalidateRect(g_ed.hwnd, nullptr, FALSE);
		return;
	}
	if (g_ed.proposing) {
		// 案を足している途中のEnterとEscは、足す操作を終える(Escは作りかけを捨てる)
		if (g_ed.adding_proposal && (id == ID_STROKE_FINISH || id == ID_CANCEL)) {
			if (id == ID_CANCEL) g_ed.proposals.back().points.clear();
			ed_finish_adding_proposal();
			InvalidateRect(g_ed.hwnd, nullptr, FALSE);
			return;
		}
		switch (id) {
		case ID_PROPOSE_ADD: {
			const bool starting = !g_ed.adding_proposal;
			ed_finish_adding_proposal();
			if (starting) {
				g_ed.proposals.emplace_back();
				g_ed.adding_proposal = true;
				ed_set_status(L"字をクリックして点を選びます(近くに輪郭の点があれば吸着)。2点でEnterなら切る線、4点で組み替えの案になります。");
			}
			CheckDlgButton(g_ed.hwnd, ID_PROPOSE_ADD, starting ? BST_CHECKED : BST_UNCHECKED);
			break;
		}
		case ID_APPLY:
		case ID_STROKE_FINISH:
			ed_finish_adding_proposal();
			ed_apply_proposals();
			break;
		case ID_PROPOSE_ALL: {
			const bool any_off = std::any_of(g_ed.proposals.begin(), g_ed.proposals.end(), [](const Proposal& p) { return !p.on; });
			for (Proposal& p : g_ed.proposals) p.on = any_off;
			break;
		}
		case ID_PROPOSE_CLOSE:
		case ID_CANCEL:
			ed_close_proposals();
			ed_set_status(L"分割案を閉じました。");
			break;
		case ID_FIT:
			ed_fit();
			break;
		}
		InvalidateRect(g_ed.hwnd, nullptr, FALSE);
		return;
	}
	if (id == ID_PROPOSE) {
		if (!g_ed.stroke_mode) { ed_set_status(L"筆順の編集で使えます。"); return; }
		ed_stroke_finish();
		g_ed.adding_stroke = false;
		ed_open_proposals();
		InvalidateRect(g_ed.hwnd, nullptr, FALSE);
		return;
	}
	if (id == ID_STROKE_MODE) {
		ed_toggle_stroke_mode();
		InvalidateRect(g_ed.hwnd, nullptr, FALSE);
		return;
	}
	if (id == ID_NEW_STROKE) {
		// ボタンとキーのどちらからでも切り替える
		if (!g_ed.stroke_mode) return;
		const bool starting = g_ed.drawing_glyph < 0 && !g_ed.adding_stroke;
		ed_stroke_finish();
		g_ed.adding_stroke = starting;
		CheckDlgButton(g_ed.hwnd, ID_NEW_STROKE, starting ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(g_ed.hwnd, ID_ADD, BST_UNCHECKED);
		ed_set_status(starting ? L"クリックで点を置きます。Enterで終わります。" : L"画の追加を終えました。");
		InvalidateRect(g_ed.hwnd, nullptr, FALSE);
		return;
	}
	if (g_ed.stroke_mode && ed_stroke_command(id)) {
		if (id == ID_STROKE_FINISH) g_ed.adding_stroke = false;
		InvalidateRect(g_ed.hwnd, nullptr, FALSE);
		return;
	}
	switch (id) {
	case ID_DELETE: ed_delete_nodes(); break;
	case ID_ADD_HANDLE: ed_add_handles(); break;
	case ID_DELETE_HANDLE: ed_delete_handles(); break;
	case ID_DUPLICATE: ed_duplicate(); break;
	case ID_RECONNECT: ed_reconnect(); break;
	case ID_SPLIT: ed_split(); break;
	case ID_TOGGLE_HANDLE: ed_toggle_handles(); break;
	case ID_LINK: ed_link_select(); break;
	case ID_UNDO:
		if (!g_ed.history.empty()) {
			g_ed.model = std::move(g_ed.history.back().model);
			g_ed.strokes = std::move(g_ed.history.back().strokes);
			g_ed.history.pop_back();
			// 追加の途中で戻した時は、画の追加を終える
			g_ed.drawing_glyph = g_ed.drawing_stroke = -1;
			g_ed.adding_stroke = false;
			CheckDlgButton(g_ed.hwnd, ID_NEW_STROKE, BST_UNCHECKED);
			g_ed.selection.clear();
			ed_set_status(L"1つ前の状態に戻しました。");
		}
		break;
	case ID_FIT: ed_fit(); break;
	case ID_OK:
		ed_stroke_finish();
		// 何も変えていなければ書き込まない(本体のUndoを増やさない)
		if (!g_ed.history.empty() || g_ed.make_pen) g_edit_handle->call_edit_section_param(nullptr, ed_apply);
		ed_close();
		return;
	case ID_CANCEL:
		ed_close();
		return;
	}
	InvalidateRect(g_ed.hwnd, nullptr, FALSE);
}

// Ctrl+クリック: 重なった点はクリックのたびに次の点へ移り、最後の次で選択を外す
void ed_ctrl_click(int x, int y) {
	const std::vector<EdRef> hits = g_ed.stroke_mode ? ed_stroke_hits(x, y) : ed_hits(x, y);
	if (hits.empty()) return;
	const auto chosen = std::find_if(hits.begin(), hits.end(), [](const EdRef& r) { return std::find(g_ed.selection.begin(), g_ed.selection.end(), r) != g_ed.selection.end(); });
	if (chosen == hits.end()) {
		g_ed.selection.push_back(hits[0]);
		return;
	}
	auto held = std::find(g_ed.selection.begin(), g_ed.selection.end(), *chosen);
	if (chosen + 1 != hits.end()) *held = *(chosen + 1);
	else g_ed.selection.erase(held);
}

// 矩形の中のアンカー(筆順の編集では画の点)を選択に足す
void ed_rect_select(int x, int y) {
	const int x0 = std::min<int>(x, g_ed.drag_start.x), x1 = std::max<int>(x, g_ed.drag_start.x);
	const int y0 = std::min<int>(y, g_ed.drag_start.y), y1 = std::max<int>(y, g_ed.drag_start.y);
	auto add = [&](Vec v, EdRef r) {
		const POINT p = to_screen(v);
		if (p.x < x0 || p.x > x1 || p.y < y0 || p.y > y1) return;
		if (std::find(g_ed.selection.begin(), g_ed.selection.end(), r) == g_ed.selection.end()) g_ed.selection.push_back(r);
	};
	if (g_ed.stroke_mode) {
		for (int g = 0; g < (int)g_ed.strokes.size(); g++) {
			const auto& lines = g_ed.strokes[g].lines;
			for (int s = 0; s < (int)lines.size(); s++) {
				for (int n = 0; n < (int)lines[s].size(); n++) add(lines[s][n], { 0, g, s, n });
			}
		}
		return;
	}
	for (int g = 0; g < (int)g_ed.model.size(); g++) {
		const auto& contours = g_ed.model[g].contours;
		for (int c = 0; c < (int)contours.size(); c++) {
			for (int n = 0; n < (int)contours[c].nodes.size(); n++) add(contours[c].nodes[n].p[g_ed.shape], { 0, g, c, n });
		}
	}
}

void ed_key_down(int vk) {
	int command = find_shortcut(vk, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0, g_ed.proposing);
	if (!command) return;
	// 「種類を変更」はボタンのオン・オフだけなので、キーでも同じように切り替える
	if (command == ID_PROPOSE_KIND) {
		const bool on = IsDlgButtonChecked(g_ed.hwnd, ID_PROPOSE_KIND) != BST_CHECKED;
		CheckDlgButton(g_ed.hwnd, ID_PROPOSE_KIND, on ? BST_CHECKED : BST_UNCHECKED);
		ed_set_status(on ? L"種類を変更: オン。案をクリックすると種類が変わります。" : L"種類を変更: オフ。");
		return;
	}
	// OKのキーは、案の表示中と画の追加中は「適用」「追加を終える」になる
	if (command == ID_OK && (g_ed.proposing || g_ed.adding_stroke)) command = ID_STROKE_FINISH;
	if (is_move_command(command)) {
		ed_move_by_key(command, g_ed.key_moving);
		InvalidateRect(g_ed.hwnd, nullptr, FALSE);
	} else {
		g_ed.key_moving = false;
		ed_command(command);
	}
}

void ed_key_up(int vk) {
	// 修飾キーだけを離した時は移動の続きとして扱う
	if (vk != VK_SHIFT && vk != VK_CONTROL) g_ed.key_moving = false;
}

// 本体のショートカットが動かないように、ウィンドウが前面の間はキーをここで受ける
HHOOK g_ed_hook = nullptr;

LRESULT CALLBACK ed_keyboard_hook(int code, WPARAM wparam, LPARAM lparam) {
	const int vk = (int)wparam;
	// 修飾キーは本体へ通す(捨てるとGetKeyStateの状態が更新されない)
	if (code != HC_ACTION || !g_ed.hwnd || GetForegroundWindow() != g_ed.hwnd || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
		return CallNextHookEx(g_ed_hook, code, wparam, lparam);
	}
	if (lparam & 0x80000000) ed_key_up(vk);
	else ed_key_down(vk);
	return 1;
}

LRESULT CALLBACK ed_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	const int x = (short)LOWORD(lparam), y = (short)HIWORD(lparam);
	switch (message) {
	case WM_CREATE: {
		struct Button { int id; const wchar_t* text; DWORD style; };
		const DWORD toggle = BS_AUTOCHECKBOX | BS_PUSHLIKE;
		const Button buttons[] = {
			{ ID_NEW_STROKE, L"画を追加", toggle }, { ID_DELETE_STROKE, L"画を削除", 0 },
			{ ID_STROKE_EARLIER, L"書き順を前へ", 0 }, { ID_STROKE_LATER, L"書き順を後ろへ", 0 },
			{ ID_STROKE_REVERSE, L"向きを反転", 0 }, { ID_STROKE_RESET, L"筆順を既定に戻す", 0 },
			{ ID_PROPOSE_KIND, L"種類を変更", toggle }, { ID_PROPOSE_ADD, L"案を追加", toggle }, { ID_PROPOSE_ALL, L"全て切り替え", 0 },
			{ ID_ADD, L"点を追加", toggle }, { ID_DELETE, L"点を削除", 0 }, { ID_TOGGLE_HANDLE, L"ハンドルの追加・削除", 0 },
			{ ID_DUPLICATE, L"点を複製", 0 }, { ID_RECONNECT, L"つなぎ直す", 0 }, { ID_SPLIT, L"カット・交差", 0 },
			{ ID_UNDO, L"1つ戻す", 0 }, { ID_FIT, L"全体を表示", 0 },
			{ ID_PREVIEW_PLAY, L"再生", toggle }, { ID_MAKE_PEN, L"アニメを作る", toggle }, { ID_PREVIEW_CLOSE, L"確認を終える", 0 },
		};
		// 利用頻度の高いものは、アイコンの右に名前を出す横長のボタンにする(textはボタンの表示、tipはツールチップの名前)
		struct WideButton { int id; const wchar_t* text; const wchar_t* tip; DWORD style; };
		const WideButton wide_buttons[] = {
			{ ID_STROKE_MODE, L"筆順編集", L"筆順編集", 0 },	// 切り替え式のボタンにはアイコンが出ないので、普通のボタンを押した見た目にする
 { ID_PROPOSE, L"自動分割", L"自動アイランド分割", 0 }, { ID_PREVIEW, L"筆順確認", L"筆順確認", 0 },
			{ ID_APPLY, L"適用", L"適用", 0 }, { ID_PROPOSE_CLOSE, L"案を閉じる", L"案を閉じる", 0 },
		};
		INITCOMMONCONTROLSEX init{ sizeof(init), ICC_BAR_CLASSES | ICC_WIN95_CLASSES };
		InitCommonControlsEx(&init);
		HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, nullptr, nullptr);
		auto add_tooltip = [&](HWND child, const std::wstring& text) {
			TTTOOLINFOW tool{};
			tool.cbSize = sizeof(tool);
			tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
			tool.hwnd = hwnd;
			tool.uId = (UINT_PTR)child;
			tool.lpszText = const_cast<LPWSTR>(text.c_str());
			SendMessageW(tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
		};
		for (const Button& b : buttons) {
			HWND child = CreateWindowW(L"BUTTON", b.text, WS_CHILD | BS_ICON | b.style, 0, 0, 36, 32, hwnd, (HMENU)(INT_PTR)b.id, nullptr, nullptr);
			SendMessageW(child, BM_SETIMAGE, IMAGE_ICON, (LPARAM)make_button_icon(b.id));
			add_tooltip(child, std::wstring(b.text) + shortcut_text(b.id));
		}
		for (const WideButton& b : wide_buttons) {
			HWND child = CreateWindowW(L"BUTTON", b.text, WS_CHILD | b.style, 0, 0, 114, 32, hwnd, (HMENU)(INT_PTR)b.id, nullptr, nullptr);
			SendMessageW(child, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
			SendMessageW(child, BM_SETIMAGE, IMAGE_ICON, (LPARAM)make_button_icon(b.id));
			add_tooltip(child, std::wstring(b.tip) + shortcut_text(b.id));
		}
		for (const Button& b : { Button{ ID_OK, L"OK", 0 }, Button{ ID_CANCEL, L"キャンセル", 0 } }) {
			HWND child = CreateWindowW(L"BUTTON", b.text, WS_CHILD | b.style, 0, 0, 114, 28, hwnd, (HMENU)(INT_PTR)b.id, nullptr, nullptr);
			SendMessageW(child, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
			add_tooltip(child, std::wstring(b.text) + shortcut_text(b.id));
		}
		HWND slider = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | TBS_NOTICKS, 0, 0, 420, 28, hwnd, (HMENU)(INT_PTR)ID_PREVIEW_SLIDER, nullptr, nullptr);
		SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
		g_ed.hwnd = hwnd;
		ed_layout_buttons();
		return 0;
	}
	case WM_HSCROLL:
		if ((HWND)lparam == GetDlgItem(hwnd, ID_PREVIEW_SLIDER)) {
			ed_stop_preview_play();
			g_ed.preview_progress = SendMessageW((HWND)lparam, TBM_GETPOS, 0, 0) / 1000.0;
			InvalidateRect(hwnd, nullptr, FALSE);
			SetFocus(hwnd);
		}
		return 0;
	case WM_TIMER:
		if (wparam == ED_PREVIEW_TIMER) {
			g_ed.preview_progress += 0.01;
			if (g_ed.preview_progress >= 1) {
				g_ed.preview_progress = 1;
				ed_stop_preview_play();
			}
			ed_set_preview_slider();
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		ed_paint(dc);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_SIZE:
		if (g_ed.hwnd) ed_layout_buttons();	// OK・キャンセルとスライダーは大きさに合わせて置く
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_LBUTTONDBLCLK: {
		// 線の上のダブルクリックで点を足す(筆順の編集でも同じ)。点の上や案・確認の表示の時は何もしない
		if (g_ed.proposing || g_ed.previewing) return 0;
		if (g_ed.stroke_mode) {
			if (g_ed.adding_stroke) ed_stroke_add_point(x, y);
			else if (ed_stroke_hits(x, y).empty() && ed_stroke_insert(x, y)) ed_set_status(L"点を追加しました。");
		} else if (ed_hits(x, y).empty()) {
			int g, c, n;
			double t;
			if (ed_nearest_segment(x, y, g, c, n, t)) {
				ed_push_history();
				split_segment(g_ed.model[g].contours[c], n, t);
				g_ed.selection = { { 0, g, c, n + 1 } };
				ed_set_status(L"点を追加しました。");
			}
		}
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}
	case WM_COMMAND:
		// 自動で切り替わるボタンは状態を見て処理するので、押された時の切り替えを取り消してから処理する
		if (LOWORD(wparam) == ID_NEW_STROKE || LOWORD(wparam) == ID_PROPOSE_ADD) CheckDlgButton(hwnd, LOWORD(wparam), BST_UNCHECKED);
		if (LOWORD(wparam) != ID_ADD) ed_command(LOWORD(wparam));
		else if (g_ed.adding_stroke) { ed_stroke_finish(); g_ed.adding_stroke = false; }
		SetFocus(hwnd);	// キーの操作を受けられるように戻す
		return 0;
	case WM_LBUTTONDOWN: {
		SetFocus(hwnd);
		if (g_ed.previewing) return 0;
		if (g_ed.proposing && g_ed.adding_proposal) {
			if (y >= ED_TOOLBAR) ed_add_proposal_point(x, y);
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		if (g_ed.proposing) {
			// 案の点を掴む。動かさずに離したら採用・不採用を切り替える
			g_ed.drag_proposal = -1;
			for (int i = 0; i < (int)g_ed.proposals.size() && g_ed.drag_proposal < 0; i++) {
				const Proposal& p = g_ed.proposals[i];
				for (int k = 0; k < (int)p.points.size(); k++) {
					const POINT q = to_screen(p.points[k]);
					if (std::abs(q.x - x) <= 7 && std::abs(q.y - y) <= 7) { g_ed.drag_proposal = i; g_ed.drag_point = k; break; }
				}
				const POINT center = to_screen(proposal_center(p));
				if (g_ed.drag_proposal < 0 && std::abs(center.x - x) <= 12 && std::abs(center.y - y) <= 10) { g_ed.drag_proposal = i; g_ed.drag_point = -1; }
			}
			if (g_ed.drag_proposal >= 0) {
				g_ed.drag_proposal_moved = false;
				g_ed.drag_proposal_shift = (wparam & MK_SHIFT) != 0;
				g_ed.last = { x, y };
				SetCapture(hwnd);
			}
			return 0;
		}
		if (g_ed.stroke_mode && y >= ED_TOOLBAR && g_ed.adding_stroke) {
			ed_stroke_add_point(x, y);
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		if (g_ed.stroke_mode && y >= ED_TOOLBAR && SendDlgItemMessageW(hwnd, ID_ADD, BM_GETCHECK, 0, 0) == BST_CHECKED) {
			if (ed_stroke_insert(x, y)) ed_set_status(L"点を追加しました。");
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		if (y < ED_TOOLBAR || SendDlgItemMessageW(hwnd, ID_ADD, BM_GETCHECK, 0, 0) == BST_CHECKED) {
			int g, c, n;
			double t;
			if (y >= ED_TOOLBAR && ed_nearest_segment(x, y, g, c, n, t)) {
				ed_push_history();
				split_segment(g_ed.model[g].contours[c], n, t);
				g_ed.selection = { { 0, g, c, n + 1 } };
				ed_set_status(L"点を追加しました。");
			}
			return 0;
		}
		// Ctrlを押している時は、ドラッグで矩形選択、動かさずに離せばその点の選択を切り替える(離した時に決める)
		if (wparam & MK_CONTROL) {
			g_ed.mode = Editor::RECT_SELECT;
			g_ed.drag_start = g_ed.last = { x, y };
			SetCapture(hwnd);
			return 0;
		}
		const std::vector<EdRef> hits = g_ed.stroke_mode ? ed_stroke_hits(x, y) : ed_hits(x, y);
		g_ed.cycle = false;
		if (hits.empty()) {
			g_ed.selection.clear();
		} else {
			const auto in_selection = [&](const EdRef& r) { return std::find(g_ed.selection.begin(), g_ed.selection.end(), r) != g_ed.selection.end(); };
			const auto chosen = std::find_if(hits.begin(), hits.end(), in_selection);
			if (chosen == hits.end()) {
				g_ed.selection = { hits[0] };
			} else {
				// 重なった点は、動かさずに離した時に次の点へ切り替える
				g_ed.cycle = hits.size() > 1;
				g_ed.cycle_hits = hits;
			}
			if (!g_ed.selection.empty()) {
				g_ed.mode = Editor::DRAG;
				g_ed.drag_moved = false;
				g_ed.drag_start = g_ed.last = { x, y };
				g_ed.drag_applied = { 0, 0 };
				SetCapture(hwnd);
			}
		}
		ed_set_status(ed_selection_text());
		return 0;
	}
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
		g_ed.mode = Editor::PAN;
		g_ed.last = { x, y };
		SetCapture(hwnd);
		return 0;
	case WM_MOUSEMOVE:
		if (g_ed.proposing && g_ed.drag_proposal >= 0 && g_ed.drag_point >= 0) {
			if (x != g_ed.last.x || y != g_ed.last.y) {
				Proposal& p = g_ed.proposals[g_ed.drag_proposal];
				p.points[g_ed.drag_point].x += (x - g_ed.last.x) / g_ed.zoom;
				p.points[g_ed.drag_point].y += (y - g_ed.last.y) / g_ed.zoom;
				p.failed = false;
				g_ed.drag_proposal_moved = true;
				g_ed.last = { x, y };
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if (g_ed.mode == Editor::PAN) {
			g_ed.pan_x += x - g_ed.last.x;
			g_ed.pan_y += y - g_ed.last.y;
			g_ed.last = { x, y };
			InvalidateRect(hwnd, nullptr, FALSE);
		} else if (g_ed.mode == Editor::DRAG) {
			if (x == g_ed.last.x && y == g_ed.last.y) return 0;
			if (!g_ed.drag_moved) {
				ed_push_history();
				g_ed.drag_moved = true;
				g_ed.cycle = false;
			}
			// 掴んだ位置からの移動量で動かす。Shiftを押している間は、動きの大きい方の軸だけにする
			POINT offset{ x - g_ed.drag_start.x, y - g_ed.drag_start.y };
			if (wparam & MK_SHIFT) {
				if (std::abs(offset.x) >= std::abs(offset.y)) offset.y = 0;
				else offset.x = 0;
			}
			ed_move_selection((offset.x - g_ed.drag_applied.x) / g_ed.zoom, (offset.y - g_ed.drag_applied.y) / g_ed.zoom);
			g_ed.drag_applied = offset;
			g_ed.last = { x, y };
			InvalidateRect(hwnd, nullptr, FALSE);
		} else if (g_ed.mode == Editor::RECT_SELECT) {
			g_ed.last = { x, y };
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	case WM_LBUTTONUP:
		if (g_ed.proposing && g_ed.drag_proposal >= 0) {
			if (!g_ed.drag_proposal_moved) {
				Proposal& p = g_ed.proposals[g_ed.drag_proposal];
				// 「種類を変更」がオンか、Shift+クリックなら種類を切り替える。それ以外は採用・不採用
				if (g_ed.drag_proposal_shift || SendDlgItemMessageW(hwnd, ID_PROPOSE_KIND, BM_GETCHECK, 0, 0) == BST_CHECKED) {
					ed_cycle_proposal(p);
				} else {
					p.on = !p.on;
					p.failed = false;
				}
			}
			g_ed.drag_proposal = -1;
			ReleaseCapture();
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		[[fallthrough]];
	case WM_RBUTTONUP:
	case WM_MBUTTONUP:
		if (g_ed.mode == Editor::RECT_SELECT) {
			g_ed.mode = Editor::NONE;
			ReleaseCapture();
			if (std::abs(x - g_ed.drag_start.x) < 3 && std::abs(y - g_ed.drag_start.y) < 3) ed_ctrl_click(x, y);
			else ed_rect_select(x, y);
			ed_set_status(ed_selection_text());
			return 0;
		}
		if (g_ed.mode != Editor::NONE) {
			if (g_ed.cycle && g_ed.selection.size() == 1) {
				const auto at = std::find(g_ed.cycle_hits.begin(), g_ed.cycle_hits.end(), g_ed.selection[0]);
				if (at != g_ed.cycle_hits.end()) {
					const EdRef next = *(at + 1 == g_ed.cycle_hits.end() ? g_ed.cycle_hits.begin() : at + 1);
					g_ed.selection = { next };
					ed_set_status(ed_selection_text());
				}
			}
			g_ed.cycle = false;
			g_ed.mode = Editor::NONE;
			ReleaseCapture();
		}
		return 0;
	case WM_MOUSEWHEEL: {
		POINT p{ x, y };
		ScreenToClient(hwnd, &p);
		const double factor = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1.2 : 1 / 1.2;
		g_ed.pan_x = p.x - (p.x - g_ed.pan_x) * factor;
		g_ed.pan_y = p.y - (p.y - g_ed.pan_y) * factor;
		g_ed.zoom *= factor;
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}
	case WM_KEYDOWN:
		ed_key_down((int)wparam);
		return 0;
	case WM_KEYUP:
		ed_key_up((int)wparam);
		return 0;
	case WM_CLOSE: {
		// ×ボタン: 編集があれば、反映するかを確認する
		int answer = IDNO;
		if (!g_ed.history.empty() || g_ed.make_pen) {
			answer = MessageBoxW(hwnd, L"編集を反映しますか?\n\nはい: OK(反映して閉じる)\nいいえ: キャンセル(破棄して閉じる)\nキャンセル: 編集を続ける",
				PLUGIN_NAME, MB_YESNOCANCEL | MB_ICONQUESTION);
		}
		if (answer == IDCANCEL) return 0;
		if (g_ed.proposing) ed_close_proposals();
		if (g_ed.previewing) ed_close_preview();
		ed_command(answer == IDYES ? ID_OK : ID_CANCEL);
		return 0;
	}
	case WM_DESTROY:
		if (g_ed_hook) { UnhookWindowsHookEx(g_ed_hook); g_ed_hook = nullptr; }
		KillTimer(hwnd, ED_PREVIEW_TIMER);
		g_ed.preview_map.reset();
		g_ed.hwnd = nullptr;
		g_ed.history.clear();
		g_ed.model.clear();
		g_ed.selection.clear();
		return 0;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

// 文字が変わっていれば先に更新してから、今の輪郭を読んでウィンドウを開く
void open_editor(EDIT_SECTION* edit, OBJECT_HANDLE object) {
	if (g_ed.hwnd) { SetForegroundWindow(g_ed.hwnd); return; }
	EFFECT_HANDLE text_effect = edit->find_effect(object, TEXT_EFFECT);
	if (!text_effect) { show_message(L"テキストオブジェクトで使用してください。"); return; }
	auto find_path = [&]() -> EFFECT_HANDLE {
		const int num = edit->get_effect_list(object, nullptr, 0);
		std::vector<EFFECT_HANDLE> list(num);
		edit->get_effect_list(object, list.data(), num);
		for (EFFECT_HANDLE e : list) if (wcsncmp(edit->get_effect_name(e), PATH_EFFECT, wcslen(PATH_EFFECT)) == 0) return e;
		return nullptr;
	};
	auto numbers = [&](EFFECT_HANDLE e, LPCWSTR item) { return parse_numbers(edit->get_effect_item_value(e, item)); };
	EFFECT_HANDLE path = find_path();
	std::vector<double> meta = path ? numbers(path, ITEM_META) : std::vector<double>();
	if (meta.size() < META_HEADER || meta[0] != META_VERSION || (UINT32)meta[2] != text_setting_hash(edit, text_effect)) {
		generate(edit, object, PLUGIN_NAME, true);
		path = find_path();
		if (!path) return;
		meta = numbers(path, ITEM_META);
		if (meta.size() < META_HEADER || meta[0] != META_VERSION || (UINT32)meta[2] != text_setting_hash(edit, text_effect)) return;
	}
	const std::vector<double> anchors = numbers(path, ITEM_POINTS);
	const std::vector<double> points = merge_points(anchors, resolve_handles(anchors, numbers(path, ITEM_HANDLES), meta), meta);
	if (points.empty()) { show_message(L"編集できる輪郭がありません。"); return; }
	g_ed.model = load_model(points, meta, 0);
	copy_first_to_second(g_ed.model);
	g_ed.header[0] = meta;
	g_ed.has_second = false;
	const std::vector<double> meta2 = numbers(path, ITEM_META2);
	if (meta2.size() >= META_HEADER && meta2[0] == META_VERSION) {
		const std::vector<double> anchors2 = numbers(path, ITEM_POINTS2);
		const std::vector<double> points2 = merge_points(anchors2, resolve_handles(anchors2, numbers(path, ITEM_HANDLES2), meta2), meta2);
		if (!points2.empty()) {
			const EdModel second = load_model(points2, meta2, 1);
			g_ed.has_second = true;
			g_ed.header[1] = meta2;
			// 構造が違えば、形状2は形状1から作り直す(構造を揃えるため)
			merge_second(g_ed.model, second);
		}
	}
	g_ed.strokes = parse_strokes(numbers(path, ITEM_STROKES), g_ed.model.size());
	g_ed.strokes_ready = false;
	g_ed.previewing = false;
	g_ed.make_pen = false;
	g_ed.expanded = is_expanded(edit, path);
	g_ed.proposing = false;
	g_ed.proposals.clear();
	g_ed.drag_proposal = -1;
	g_ed.stroke_mode = false;
	g_ed.adding_stroke = false;
	g_ed.drawing_glyph = g_ed.drawing_stroke = -1;
	g_ed.model_dirty = g_ed.strokes_dirty = false;
	const std::wstring shape = item_string(edit, path, ITEM_EDIT_SHAPE);
	g_ed.shape = g_ed.has_second && item_string(edit, path, ITEM_MORPH) == L"1" && (shape == L"形状2" || shape == L"1") ? 1 : 0;
	g_ed.object = object;
	g_ed.history.clear();
	g_ed.selection.clear();
	g_ed.mode = Editor::NONE;
	g_ed.key_moving = false;
	g_ed.status = ED_HINT;
	load_settings();

	static bool registered = false;
	if (!registered) {
		WNDCLASSW wc{};
		wc.style = CS_DBLCLKS;
		wc.lpfnWndProc = ed_proc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.lpszClassName = L"TextPath_TeEditor";
		RegisterClassW(&wc);
		registered = true;
	}
	HWND owner = g_edit_handle->get_host_app_window();
	const std::wstring title = std::wstring(L"輪郭パス - 高度な編集") + (g_ed.shape == 1 ? L"(形状2を表示)" : L"");
	g_ed.hwnd = CreateWindowW(L"TextPath_TeEditor", title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		CW_USEDEFAULT, CW_USEDEFAULT, 1140, 760, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!g_ed.hwnd) return;
	ed_fit();
	EnableWindow(owner, FALSE);	// 編集中は本体を操作できないようにする(OK・キャンセルで戻す)
	ShowWindow(g_ed.hwnd, SW_SHOW);
	SetFocus(g_ed.hwnd);
	g_ed_hook = SetWindowsHookExW(WH_KEYBOARD, ed_keyboard_hook, nullptr, GetCurrentThreadId());
}

//---------------------------------------------------------------------
//	設定画面: 高度な編集のショートカットキーと移動量
//---------------------------------------------------------------------

enum { ID_SET_OK = 1, ID_SET_CANCEL, ID_SET_DEFAULT, ID_SET_STEP, ID_SET_SHIFT, ID_SET_CTRL, ID_SET_KEY_FIRST = 100 };

bool is_extended_key(int vk) {
	return vk == VK_DELETE || vk == VK_INSERT || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT
		|| vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT;
}

void set_hotkey(HWND control, int key) {
	const int vk = key & 0xff;
	int flags = 0;
	if (key & KEY_CTRL) flags |= HOTKEYF_CONTROL;
	if (key & KEY_SHIFT) flags |= HOTKEYF_SHIFT;
	if (is_extended_key(vk)) flags |= HOTKEYF_EXT;
	SendMessageW(control, HKM_SETHOTKEY, MAKEWORD(vk, flags), 0);
}

int get_hotkey(HWND control) {
	const int value = (int)SendMessageW(control, HKM_GETHOTKEY, 0, 0);
	int key = LOBYTE(value);
	if (HIBYTE(value) & HOTKEYF_CONTROL) key |= KEY_CTRL;
	if (HIBYTE(value) & HOTKEYF_SHIFT) key |= KEY_SHIFT;
	return key;
}

void settings_fill(HWND hwnd, bool defaults) {
	const int count = (int)(sizeof(g_shortcuts) / sizeof(g_shortcuts[0]));
	for (int i = 0; i < count; i++) {
		for (int k = 0; k < 2; k++) {
			set_hotkey(GetDlgItem(hwnd, ID_SET_KEY_FIRST + i * 2 + k), defaults ? g_shortcuts[i].defaults[k] : g_shortcuts[i].keys[k]);
		}
	}
	auto put = [&](int id, double value) {
		wchar_t text[64];
		swprintf_s(text, L"%g", value);
		SetDlgItemTextW(hwnd, id, text);
	};
	put(ID_SET_STEP, defaults ? 1 : g_move_step);
	put(ID_SET_SHIFT, defaults ? 10 : g_move_shift);
	put(ID_SET_CTRL, defaults ? 0.1 : g_move_ctrl);
}

// 入力を設定へ反映する。同じキーが2つの操作に割り当てられていればfalse
bool settings_apply(HWND hwnd) {
	const int count = (int)(sizeof(g_shortcuts) / sizeof(g_shortcuts[0]));
	std::vector<int> keys;
	for (int i = 0; i < count * 2; i++) keys.push_back(get_hotkey(GetDlgItem(hwnd, ID_SET_KEY_FIRST + i)));
	for (int i = 0; i < count * 2; i++) {
		for (int k = i + 1; keys[i] && k < count * 2; k++) {
			// 分割案の表示中だけの割り当ては、他の操作と重なってよい
			if (keys[i] == keys[k] && g_shortcuts[i / 2].proposal == g_shortcuts[k / 2].proposal) {
				const std::wstring text = std::wstring(L"同じキーが「") + g_shortcuts[i / 2].label + L"」と「" + g_shortcuts[k / 2].label + L"」に割り当てられています。";
				MessageBoxW(hwnd, text.c_str(), PLUGIN_NAME, MB_OK | MB_ICONWARNING);
				return false;
			}
		}
	}
	auto read = [&](int id, double& value) {
		wchar_t text[64];
		GetDlgItemTextW(hwnd, id, text, 64);
		const double v = wcstod(text, nullptr);
		if (v > 0) value = v;
	};
	for (int i = 0; i < count; i++) {
		g_shortcuts[i].keys[0] = keys[i * 2];
		g_shortcuts[i].keys[1] = keys[i * 2 + 1];
	}
	read(ID_SET_STEP, g_move_step);
	read(ID_SET_SHIFT, g_move_shift);
	read(ID_SET_CTRL, g_move_ctrl);
	save_settings();
	return true;
}

LRESULT CALLBACK settings_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	switch (message) {
	case WM_COMMAND:
		switch (LOWORD(wparam)) {
		case ID_SET_OK:
			if (settings_apply(hwnd)) DestroyWindow(hwnd);
			return 0;
		case ID_SET_CANCEL:
			DestroyWindow(hwnd);
			return 0;
		case ID_SET_DEFAULT:
			settings_fill(hwnd, true);
			return 0;
		}
		break;
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

void on_config(HWND owner, HINSTANCE) {
	load_settings();
	INITCOMMONCONTROLSEX init{ sizeof(init), ICC_HOTKEY_CLASS };
	InitCommonControlsEx(&init);
	static bool registered = false;
	if (!registered) {
		WNDCLASSW wc{};
		wc.lpfnWndProc = settings_proc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
		wc.lpszClassName = L"TextPath_TeSettings";
		RegisterClassW(&wc);
		registered = true;
	}
	const int count = (int)(sizeof(g_shortcuts) / sizeof(g_shortcuts[0]));
	const int row = 28, client_w = 470, client_h = 44 + count * row + 3 * row + 48;
	RECT frame{ 0, 0, client_w, client_h };
	const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
	AdjustWindowRect(&frame, style, FALSE);
	HWND hwnd = CreateWindowW(L"TextPath_TeSettings", L"輪郭パスの設定 - 高度な編集のショートカットキー", style, CW_USEDEFAULT, CW_USEDEFAULT,
		frame.right - frame.left, frame.bottom - frame.top, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!hwnd) return;
	HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	auto add = [&](LPCWSTR type, LPCWSTR text, DWORD extra, int x, int y, int w, int h, int id) {
		HWND child = CreateWindowW(type, text, WS_CHILD | WS_VISIBLE | extra, x, y, w, h, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
		SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
		return child;
	};
	add(L"STATIC", L"キー欄を選んでキーを押します(Backspaceで割り当てなし)", 0, 12, 12, client_w - 24, 20, -1);
	int y = 40;
	for (int i = 0; i < count; i++) {
		add(L"STATIC", g_shortcuts[i].label, SS_CENTERIMAGE, 12, y, 190, 24, -1);
		for (int k = 0; k < 2; k++) {
			HWND key = add(HOTKEY_CLASSW, L"", WS_TABSTOP | WS_BORDER, 206 + k * 128, y, 122, 24, ID_SET_KEY_FIRST + i * 2 + k);
			SendMessageW(key, HKM_SETRULES, 0, 0);	// 修飾キー無しのキーも割り当てられるようにする
		}
		y += row;
	}
	const struct { LPCWSTR label; int id; } moves[] = {
		{ L"矢印キーの移動量(ピクセル)", ID_SET_STEP }, { L"Shiftを押した時の倍率", ID_SET_SHIFT }, { L"Ctrlを押した時の倍率", ID_SET_CTRL },
	};
	for (const auto& m : moves) {
		add(L"STATIC", m.label, SS_CENTERIMAGE, 12, y, 190, 24, -1);
		add(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 206, y, 122, 24, m.id);
		y += row;
	}
	y += 8;
	add(L"BUTTON", L"既定に戻す", WS_TABSTOP, 12, y, 96, 28, ID_SET_DEFAULT);
	add(L"BUTTON", L"OK", WS_TABSTOP, client_w - 188, y, 84, 28, ID_SET_OK);
	add(L"BUTTON", L"キャンセル", WS_TABSTOP, client_w - 96, y, 84, 28, ID_SET_CANCEL);
	settings_fill(hwnd, false);
	EnableWindow(owner, FALSE);
	ShowWindow(hwnd, SW_SHOW);
	MSG msg;
	while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW(hwnd, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	EnableWindow(owner, TRUE);
	SetForegroundWindow(owner);
}

void on_button_editor(EDIT_SECTION* edit, OBJECT_HANDLE object, LPCWSTR, LPCWSTR) {
	open_editor(edit, object);
}

// 右クリックのメニュー: 選択中のオブジェクトごとに実行する。複数選択の時、テキストでないものは飛ばす
template <bool KeepEdits>
void on_object_menu(EDIT_SECTION* edit) {
	std::vector<OBJECT_HANDLE> objects;
	for (int i = 0; i < edit->get_selected_object_num(); i++) objects.push_back(edit->get_selected_object(i));
	if (objects.empty() && edit->get_focus_object()) objects.push_back(edit->get_focus_object());
	for (OBJECT_HANDLE object : objects) {
		if (objects.size() > 1 && !edit->find_effect(object, TEXT_EFFECT)) continue;
		on_button<KeepEdits>(edit, object, PLUGIN_NAME, nullptr);
	}
}

void on_object_menu_editor(EDIT_SECTION* edit) {
	OBJECT_HANDLE object = edit->get_selected_object_num() > 0 ? edit->get_selected_object(0) : edit->get_focus_object();
	if (object) open_editor(edit, object);
}

//---------------------------------------------------------------------
//	アイランドごとのオブジェクトへ展開
//---------------------------------------------------------------------

double polyline_length(const std::vector<Vec>& line);
std::vector<Vec> resample_line(const std::vector<Vec>& line, double step);
void rasterize(PIXEL_RGBA* out, int w, int h, const std::vector<double>& points, const std::vector<double>& meta, double cx, double cy, int only, int r, int g, int b, const std::vector<int>* contours = nullptr);

// 向きを1文字で表す
const wchar_t* direction_symbol(double dx, double dy) {
	if (std::abs(dx) > std::abs(dy) * 2.4) return L"ー";
	if (std::abs(dy) > std::abs(dx) * 2.4) return L"|";
	return dx * dy > 0 ? L"＼" : L"／";	// 半角の「\」はタイムラインで円記号になるので全角にする
}

// 上下左右のどれに近いか(0=右 1=下 2=左 3=上)
int four_way(Vec a, Vec b) {
	const double dx = b.x - a.x, dy = b.y - a.y;
	if (std::abs(dx) >= std::abs(dy)) return dx >= 0 ? 0 : 2;
	return dy >= 0 ? 1 : 3;
}

// 画の線から概形の記号を決める
std::wstring line_symbol(const std::vector<Vec>& source) {
	const std::vector<Vec> line = resample_line(source, 2);
	if (line.size() < 2) return L"・";
	const Vec first = line.front(), last = line.back();
	const double length = polyline_length(line), chord = distance(first, last);
	if (chord < length * 0.25) return L"○";
	if (chord > length * 0.92) return direction_symbol(last.x - first.x, last.y - first.y);
	// 折れている画: 始点と終点を結ぶ線から最も離れた所を角とする
	size_t corner = line.size() / 2;
	double farthest = -1;
	for (size_t i = 1; i + 1 < line.size(); i++) {
		const double d = std::abs((last.x - first.x) * (first.y - line[i].y) - (first.x - line[i].x) * (last.y - first.y)) / std::max(chord, 1e-6);
		if (d > farthest) { farthest = d; corner = i; }
	}
	const int d1 = four_way(first, line[corner]), d2 = four_way(line[corner], last);
	if (d1 == d2) return direction_symbol(last.x - first.x, last.y - first.y);
	static const wchar_t* const table[4][4] = {
		// 2つ目の向き: 右, 下, 左, 上
		{ L"ー", L"7", L"~", L"」" },	// 1つ目が右
		{ L"L", L"|", L"」", L"~" },	// 下
		{ L"~", L"「", L"ー", L"L" },	// 左
		{ L"「", L"~", L"7", L"|" },	// 上
	};
	return table[d1][d2];
}

// 画の線が無いアイランドの概形: 点の広がりの主軸で決める
std::wstring island_symbol(const std::vector<std::vector<Vec>>& lines) {
	double n = 0, mx = 0, my = 0;
	for (const auto& line : lines) for (const Vec& v : line) { n++; mx += v.x; my += v.y; }
	if (n < 1) return L"・";
	mx /= n; my /= n;
	double xx = 0, yy = 0, xy = 0;
	for (const auto& line : lines) {
		for (const Vec& v : line) { xx += (v.x - mx) * (v.x - mx); yy += (v.y - my) * (v.y - my); xy += (v.x - mx) * (v.y - my); }
	}
	const double spread = std::sqrt((xx - yy) * (xx - yy) + 4 * xy * xy);
	const double major = (xx + yy + spread) / 2, minor = (xx + yy - spread) / 2;
	if (major < minor * 6) return L"○";
	const double angle = 0.5 * std::atan2(2 * xy, xx - yy);
	return direction_symbol(std::cos(angle), std::sin(angle));
}

struct ExpandPart {
	int glyph, order;	// 並べ替えのキー(文字、最初の画の番号)
	std::wstring name;
	std::vector<int> contours;	// その文字の中の輪郭の番号
	std::vector<std::vector<Vec>> lines;	// このアイランドの画の線
};

std::wstring code_text(UINT32 code) {
	std::wstring text;
	if (code >= 0x10000) {
		text += (wchar_t)(0xD800 + ((code - 0x10000) >> 10));
		text += (wchar_t)(0xDC00 + ((code - 0x10000) & 0x3ff));
	} else {
		text += (wchar_t)code;
	}
	return text;
}

// アイランドごとの部品を、画の順に並べ替えて返す
std::vector<ExpandPart> plan_expansion(const EdModel& model, const std::vector<GlyphStrokes>& strokes) {
	std::vector<ExpandPart> parts;
	for (int g = 0; g < (int)model.size(); g++) {
		const EdGlyph& glyph = model[g];
		if (glyph.contours.empty()) continue;
		const std::vector<int> islands = contour_islands(glyph, 0);
		const int count = *std::max_element(islands.begin(), islands.end()) + 1;
		std::vector<std::vector<std::vector<Vec>>> island_lines(count);
		for (int c = 0; c < (int)glyph.contours.size(); c++) island_lines[islands[c]].push_back(flatten_contour(glyph.contours[c], 0));
		// 画は、その線の点を最も多く含むアイランドのものにする(ペンで書く処理と同じ考え方)
		const std::vector<std::vector<Vec>> lines = g < (int)strokes.size() && strokes[g].custom ? strokes[g].lines : default_strokes(glyph);
		std::vector<std::vector<int>> island_strokes(count);
		for (int s = 0; s < (int)lines.size(); s++) {
			const std::vector<Vec> samples = resample_line(lines[s], 2);
			int best = -1;
			size_t best_count = 0;
			for (int i = 0; i < count; i++) {
				size_t inside = 0;
				for (const Vec& v : samples) {
					int winding = 0;
					for (const auto& line : island_lines[i]) {
						for (size_t k = 0; k < line.size(); k++) {
							const Vec a = line[k], b = line[(k + 1) % line.size()];
							if ((a.y <= v.y) != (b.y <= v.y) && v.x < a.x + (b.x - a.x) * (v.y - a.y) / (b.y - a.y)) winding += a.y < b.y ? 1 : -1;
						}
					}
					if (winding) inside++;
				}
				if (inside > best_count) { best_count = inside; best = i; }
			}
			if (best >= 0) island_strokes[best].push_back(s);
		}
		for (int i = 0; i < count; i++) {
			ExpandPart part{ g, island_strokes[i].empty() ? INT_MAX : island_strokes[i][0], L"" };
			for (int c = 0; c < (int)islands.size(); c++) if (islands[c] == i) part.contours.push_back(c);
			for (int s : island_strokes[i]) part.lines.push_back(lines[s]);
			// 画が無いアイランドは、最も近い画で書き進む(ペンで書く処理と同じ考え方)
			if (part.lines.empty() && !lines.empty()) {
				double best = 1e30;
				int nearest = 0;
				for (int s = 0; s < (int)lines.size(); s++) {
					for (const Vec& v : lines[s]) {
						for (const auto& line : island_lines[i]) {
							for (const Vec& p : line) {
								const double d = distance(v, p);
								if (d < best) { best = d; nearest = s; }
							}
						}
					}
				}
				part.lines.push_back(lines[nearest]);
			}
			// 1画だけのアイランドは画の線で、それ以外は形で概形を決める
			const std::wstring symbol = island_strokes[i].size() == 1 ? line_symbol(lines[island_strokes[i][0]]) : island_symbol(island_lines[i]);
			part.name = code_text((UINT32)glyph.head[0]) + L" " + symbol;
			parts.push_back(std::move(part));
		}
	}
	std::stable_sort(parts.begin(), parts.end(), [](const ExpandPart& a, const ExpandPart& b) { return a.glyph != b.glyph ? a.glyph < b.glyph : a.order < b.order; });
	return parts;
}

// 1つのアイランドだけの座標と輪郭情報を作る。インクの位置とテキスト設定のハッシュは元のまま(本体の画像は元のテキストのままなので)
void extract_island(const EdGlyph& glyph, const std::vector<int>& contours, const std::vector<double>& header, std::vector<double>& points, std::vector<double>& meta) {
	EdModel model(1);
	model[0].head = glyph.head;
	for (int c : contours) model[0].contours.push_back(glyph.contours[c]);
	save_model(model, 0, header, points, meta);
	meta[1] = hash_points(points);
	meta[5] = 1;
	meta[META_HEADER + 7] = hash_points(points);
}

// 自動分割の前後で点は変わるので、塗りで比べる: このアイランドの範囲で、字全体の塗りが無編集の時と同じなら、動かしていない
bool island_unedited(const std::vector<double>& points, const std::vector<double>& meta, const std::vector<double>& base, const std::vector<double>& base_meta, int g, const std::vector<int>& contours) {
	if (base.size() < 2 || base_meta.size() < META_HEADER || base_meta[0] != META_VERSION) return false;
	double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
	for (const auto* values : { &points, &base }) {
		for (size_t i = 0; i + 1 < values->size(); i += 2) {
			x0 = std::min(x0, (*values)[i]); x1 = std::max(x1, (*values)[i]);
			y0 = std::min(y0, (*values)[i + 1]); y1 = std::max(y1, (*values)[i + 1]);
		}
	}
	const double cx = 3 - std::floor(x0), cy = 3 - std::floor(y0);
	const int w = (int)std::ceil(x1 + cx) + 3, h = (int)std::ceil(y1 + cy) + 3;
	const size_t total = (size_t)w * h;
	std::vector<PIXEL_RGBA> own(total), whole(total), pristine(total);
	rasterize(own.data(), w, h, points, meta, cx, cy, g, 0, 0, 0, &contours);
	rasterize(whole.data(), w, h, points, meta, cx, cy, g, 0, 0, 0);
	rasterize(pristine.data(), w, h, base, base_meta, cx, cy, g, 0, 0, 0);
	// 分割で点を足した所は僅かに濃さが変わるので、大きく違う画素が数個より多い時だけ動かしたとみなす
	const int tolerance = 64, allowed = 3;
	int different = 0;
	for (size_t i = 0; i < total; i++) {
		if (own[i].a && std::abs((int)whole[i].a - (int)pristine[i].a) > tolerance && ++different > allowed) return false;
	}
	return true;
}

struct ExpandRequest {
	int layer = 0, start = 0;
	std::vector<ExpandPart> parts;
	std::wstring error;
	bool failed = false;
} g_expand;

// 展開先が空いているかを調べ、部品の一覧を作る
void expand_read(void*, EDIT_SECTION* edit) {
	g_expand.parts.clear();
	g_expand.error.clear();
	OBJECT_HANDLE object = edit->find_object(g_expand.layer, g_expand.start);
	if (!object) { g_expand.error = L"オブジェクトが見つかりません。"; return; }
	const OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(object);
	const int num = edit->get_effect_list(object, nullptr, 0);
	std::vector<EFFECT_HANDLE> list(num);
	edit->get_effect_list(object, list.data(), num);
	EFFECT_HANDLE path = nullptr;
	for (EFFECT_HANDLE e : list) if (wcsncmp(edit->get_effect_name(e), PATH_EFFECT, wcslen(PATH_EFFECT)) == 0) path = e;
	if (!path) { g_expand.error = L"先に「輪郭を作成・更新」を押してください。"; return; }
	const std::vector<double> meta = parse_numbers(edit->get_effect_item_value(path, ITEM_META));
	if (meta.size() < META_HEADER || meta[0] != META_VERSION) { g_expand.error = L"先に「輪郭を作成・更新」を押してください。"; return; }
	if (is_expanded(edit, path)) { g_expand.error = L"このオブジェクトは展開済みです。"; return; }
	const std::vector<double> anchors = parse_numbers(edit->get_effect_item_value(path, ITEM_POINTS));
	const std::vector<double> points = merge_points(anchors, resolve_handles(anchors, parse_numbers(edit->get_effect_item_value(path, ITEM_HANDLES)), meta), meta);
	if (points.empty()) { g_expand.error = L"展開できる輪郭がありません。"; return; }
	const EdModel model = load_model(points, meta, 0);
	const std::vector<GlyphStrokes> strokes = parse_strokes(parse_numbers(edit->get_effect_item_value(path, ITEM_STROKES)), model.size());
	g_expand.parts = plan_expansion(model, strokes);
	if (g_expand.parts.size() < 2) {
		g_expand.error = L"アイランドが1つなので展開できません。先に高度な編集の「筆順編集」→「自動分割」で分けてください。";
		return;
	}
	// 展開先: 元のレイヤーから下へ部品の数だけ。元のオブジェクト自身は置き換わるので除く
	for (int i = 0; i < (int)g_expand.parts.size(); i++) {
		const int layer = lf.layer + i;
		OBJECT_HANDLE other = edit->find_object(layer, lf.start);
		if (other && other != object && edit->get_object_layer_frame(other).start <= lf.end) {
			g_expand.error = L"展開先のレイヤー" + std::to_wstring(layer + 1) + L"が埋まっているので展開できません。"
				+ std::to_wstring(g_expand.parts.size()) + L"個分のレイヤー(" + std::to_wstring(lf.layer + 1) + L"～" + std::to_wstring(lf.layer + g_expand.parts.size()) + L")を空けてください。";
			return;
		}
	}
}

void expand_write(void*, EDIT_SECTION* edit) {
	OBJECT_HANDLE object = edit->find_object(g_expand.layer, g_expand.start);
	if (!object) return;
	const OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(object);
	const std::string alias = edit->get_object_alias(object);
	// 元の輪郭と筆順を読み、各オブジェクトには自分のアイランドの分だけを持たせる
	EFFECT_HANDLE source = nullptr;
	{
		const int num = edit->get_effect_list(object, nullptr, 0);
		std::vector<EFFECT_HANDLE> list(num);
		edit->get_effect_list(object, list.data(), num);
		for (EFFECT_HANDLE e : list) if (wcsncmp(edit->get_effect_name(e), PATH_EFFECT, wcslen(PATH_EFFECT)) == 0) source = e;
	}
	if (!source) return;
	auto numbers = [&](LPCWSTR item) { return parse_numbers(edit->get_effect_item_value(source, item)); };
	const std::vector<double> meta = numbers(ITEM_META), meta2 = numbers(ITEM_META2), base = numbers(ITEM_BASE), base_meta = numbers(ITEM_BASE_META);
	const std::vector<double> anchors = numbers(ITEM_POINTS), anchors2 = numbers(ITEM_POINTS2);
	const std::vector<double> points = merge_points(anchors, resolve_handles(anchors, numbers(ITEM_HANDLES), meta), meta);
	const std::vector<double> points2 = meta2.size() >= META_HEADER && meta2[0] == META_VERSION
		? merge_points(anchors2, resolve_handles(anchors2, numbers(ITEM_HANDLES2), meta2), meta2) : std::vector<double>();
	const EdModel model = load_model(points, meta, 0);
	const EdModel model2 = points2.empty() ? EdModel() : load_model(points2, meta2, 0);
	struct Extracted { std::vector<double> points, meta, points2, meta2; bool unedited; };
	std::vector<Extracted> extracted;
	for (const ExpandPart& part : g_expand.parts) {
		Extracted x;
		extract_island(model[part.glyph], part.contours, meta, x.points, x.meta);
		// 形状2は同じ輪郭を取り出す。構造が違う時は形状1と同じにする(2形状補間は構造が同じ時だけ働くので)
		bool same = model2.size() == model.size() && model2[part.glyph].contours.size() == model[part.glyph].contours.size();
		for (int c : part.contours) same = same && model2[part.glyph].contours[c].nodes.size() == model[part.glyph].contours[c].nodes.size();
		if (same) {
			extract_island(model2[part.glyph], part.contours, meta2, x.points2, x.meta2);
		} else {
			x.points2 = x.points;
			x.meta2 = x.meta;
		}
		x.unedited = island_unedited(points, meta, base, base_meta, part.glyph, part.contours);
		extracted.push_back(std::move(x));
	}
	edit->delete_object(object);
	for (int i = 0; i < (int)g_expand.parts.size(); i++) {
		OBJECT_HANDLE clone = edit->create_object_from_alias(alias.c_str(), lf.layer + i, lf.start, lf.end - lf.start + 1);
		if (!clone) { g_expand.failed = true; continue; }
		const int num = edit->get_effect_list(clone, nullptr, 0);
		std::vector<EFFECT_HANDLE> list(num);
		edit->get_effect_list(clone, list.data(), num);
		for (EFFECT_HANDLE e : list) {
			const std::wstring name = edit->get_effect_name(e);
			if (name.rfind(PATH_EFFECT, 0) == 0) {
				Extracted& x = extracted[i];
				edit->set_effect_item_value(e, ITEM_EXPANDED, "1");
				edit->set_effect_item_value(e, L"オブジェクトの分け方", "全体で1つのオブジェクト");
				write_points(edit, e, x.points, x.meta, ITEM_POINTS, ITEM_HANDLES, ITEM_META);
				write_points(edit, e, x.points2, x.meta2, ITEM_POINTS2, ITEM_HANDLES2, ITEM_META2);
				// 元の輪郭は、動かしていない画素に本体の画像を使うためのもの。展開前に動かしたアイランドは自前の塗りだけで描く
				std::vector<double> own_base_meta = x.meta;
				if (!x.unedited) own_base_meta[8] = own_base_meta[9] = 0;
				edit->set_effect_item_value(e, ITEM_BASE, x.unedited ? join_numbers(x.points, "%.2f").c_str() : "0,0");
				edit->set_effect_item_value(e, ITEM_BASE2, x.unedited ? join_numbers(x.points2, "%.2f").c_str() : "0,0");
				edit->set_effect_item_value(e, ITEM_BASE_META, join_numbers(own_base_meta, "%.10g").c_str());
				std::vector<GlyphStrokes> own_strokes(1);
				own_strokes[0].custom = !g_expand.parts[i].lines.empty();
				own_strokes[0].lines = g_expand.parts[i].lines;
				edit->set_effect_item_value(e, ITEM_STROKES, join_strokes(own_strokes).c_str());
			} else if (name == TEXT_EFFECT) {
				// 本体が文字ごとに分けると、1つのアイランドだけを描けない
				edit->set_effect_item_value(e, L"文字毎に個別オブジェクト", "0");
			} else if (name == PLUGIN_NAME) {
				edit->delete_effect(clone, e);	// 展開したものをさらに展開はしないのでボタンは残さない
			}
		}
		edit->set_object_name(clone, g_expand.parts[i].name.c_str());
	}
}

// ボタンの処理の中では自分のオブジェクトを消せないので、編集セクションを出てから行う
void CALLBACK expand_later(HWND, UINT, UINT_PTR id, DWORD) {
	KillTimer(nullptr, id);
	g_edit_handle->call_edit_section_param(nullptr, expand_read);
	if (!g_expand.error.empty()) { show_message(g_expand.error.c_str()); return; }
	std::wstring text = std::to_wstring(g_expand.parts.size()) + L"個のオブジェクトに分割して、レイヤー" + std::to_wstring(g_expand.layer + 1) + L"から下へ展開します。\n\n";
	for (size_t i = 0; i < g_expand.parts.size() && i < 12; i++) text += std::to_wstring(i + 1) + L": " + g_expand.parts[i].name + L"\n";
	if (g_expand.parts.size() > 12) text += L"…\n";
	text += L"\nよろしいですか?";
	if (MessageBoxW(g_edit_handle->get_host_app_window(), text.c_str(), PLUGIN_NAME, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
	g_expand.failed = false;
	g_edit_handle->call_edit_section_param(nullptr, expand_write);
	if (g_expand.failed) show_message(L"作れなかったオブジェクトがあります。元に戻す(Undo)で展開の前へ戻せます。");
}

void request_expand(EDIT_SECTION* edit, OBJECT_HANDLE object) {
	const OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(object);
	g_expand.layer = lf.layer;
	g_expand.start = lf.start;
	SetTimer(nullptr, 0, 1, expand_later);
}

void on_button_expand(EDIT_SECTION* edit, OBJECT_HANDLE object, LPCWSTR, LPCWSTR) {
	request_expand(edit, object);
}

void on_object_menu_expand(EDIT_SECTION* edit) {
	OBJECT_HANDLE object = edit->get_selected_object_num() > 0 ? edit->get_selected_object(0) : edit->get_focus_object();
	if (object) request_expand(edit, object);
}

FILTER_ITEM_BUTTON g_button_generate(L"輪郭を作成・更新", on_button<true>);
FILTER_ITEM_BUTTON g_button_reset(L"全てのアンカーをリセット", on_button<false>);
FILTER_ITEM_BUTTON g_button_editor(L"高度な編集", on_button_editor);
FILTER_ITEM_BUTTON g_button_expand(L"アイランドごとにオブジェクトへ展開", on_button_expand);
void* g_items[] = { &g_button_generate, &g_button_reset, &g_button_editor, &g_button_expand, nullptr };

bool proc_video(FILTER_PROC_VIDEO*) { return true; }	// 描画は輪郭パスが行う

FILTER_PLUGIN_TABLE g_filter_table = {
	FILTER_PLUGIN_TABLE::FLAG_VIDEO | FILTER_PLUGIN_TABLE::FLAG_FILTER,
	PLUGIN_NAME,
	L"teruyoshi",
	L"テキストの輪郭をアンカーで編集できるパスに変換する v1.0 By teruyoshi",
	g_items,
	proc_video,
	nullptr, nullptr, nullptr,
};

//---------------------------------------------------------------------
//	スクリプトモジュール
//---------------------------------------------------------------------

void rasterize(PIXEL_RGBA* out, int w, int h, const std::vector<double>& points, const std::vector<double>& meta, double cx, double cy, int only, int r, int g, int b, const std::vector<int>* contours);

std::vector<double> read_array(SCRIPT_MODULE_PARAM* param, int index) {
	std::vector<double> values(param->get_param_array_num(index));
	for (int i = 0; i < (int)values.size(); i++) values[i] = param->get_param_array_double(index, i);
	return values;
}

// fill(data, w, h, アンカー, ハンドル, 輪郭情報, r, g, b, ox, oy, 文字): 輪郭を塗りつぶして画像データへ書く。
// (ox, oy)は座標の原点の、画像の中心からの位置。文字は番号(0～)で、-1なら全部
void module_fill(SCRIPT_MODULE_PARAM* param) {
	auto data = static_cast<PIXEL_RGBA*>(param->get_param_data(0));
	const int w = param->get_param_int(1), h = param->get_param_int(2);
	if (!data || w <= 0 || h <= 0) { param->set_error(u8"引数の値が正しくありません"); return; }
	const std::vector<double> meta = read_array(param, 5);
	const std::vector<double> points = merge_points(read_array(param, 3), read_array(param, 4), meta);
	const double cx = w / 2.0 + param->get_param_double(9), cy = h / 2.0 + param->get_param_double(10);
	rasterize(data, w, h, points, meta, cx, cy, param->get_param_int(11), param->get_param_int(6), param->get_param_int(7), param->get_param_int(8));
}

// 本体の画像の控え。clearbufferで元のデータが無効になるので、capture→clearbuffer→composeの順に使う
thread_local std::vector<PIXEL_RGBA> g_captured;
thread_local int g_captured_w = 0, g_captured_h = 0;

// capture(data, w, h): 本体の画像を控える
void module_capture(SCRIPT_MODULE_PARAM* param) {
	auto data = static_cast<PIXEL_RGBA*>(param->get_param_data(0));
	const int w = param->get_param_int(1), h = param->get_param_int(2);
	if (!data || w <= 0 || h <= 0) { param->set_error(u8"引数の値が正しくありません"); return; }
	g_captured.assign(data, data + (size_t)w * h);
	g_captured_w = w;
	g_captured_h = h;
}

void rasterize(PIXEL_RGBA* out, int w, int h, const std::vector<double>& points, const std::vector<double>& meta, double cx, double cy, int only, int r, int g, int b, const std::vector<int>* contours) {
	auto read = [&](size_t i) { return Vec{ points[i * 2] + cx, points[i * 2 + 1] + cy }; };
	// 重なるアイランド(画の分解で作ったもの)は別々に塗って濃い方を取る。一緒に塗ると、共有する辺の濃さが足されて濃くなる
	std::vector<int> group;	// 処理する順の輪郭ごとに: 0=まとめて塗る / 1～=別々に塗る組
	int group_num = 0;
	std::vector<std::vector<std::vector<Vec>>> group_lines;	// 別々に塗る組ごとの輪郭(画面の座標)
	if (!points.empty()) {
		const EdModel model = load_model(points, meta, 0);
		for (int gi = 0; gi < (int)model.size(); gi++) {
			if (only >= 0 && gi != only) continue;
			const EdGlyph& glyph = model[gi];
			const std::vector<int> islands = contour_islands(glyph, 0);
			const int count = islands.empty() ? 0 : *std::max_element(islands.begin(), islands.end()) + 1;
			struct Box { double x0, y0, x1, y1; bool any; };
			std::vector<Box> boxes(count, Box{ 0, 0, 0, 0, false });
			for (int c = 0; c < (int)glyph.contours.size(); c++) {
				Box& box = boxes[islands[c]];
				for (const Vec& v : flatten_contour(glyph.contours[c], 0)) {
					if (!box.any) { box = { v.x, v.y, v.x, v.y, true }; continue; }
					box.x0 = std::min(box.x0, v.x); box.x1 = std::max(box.x1, v.x);
					box.y0 = std::min(box.y0, v.y); box.y1 = std::max(box.y1, v.y);
				}
			}
			bool overlap = false;
			for (int i = 0; i < count && !overlap; i++) {
				for (int k = i + 1; k < count; k++) {
					if (boxes[i].x0 < boxes[k].x1 && boxes[k].x0 < boxes[i].x1 && boxes[i].y0 < boxes[k].y1 && boxes[k].y0 < boxes[i].y1) { overlap = true; break; }
				}
			}
			for (int island : islands) group.push_back(overlap ? group_num + island + 1 : 0);
			if (overlap) {
				group_lines.resize(group_num + count);
				for (int c = 0; c < (int)glyph.contours.size(); c++) {
					std::vector<Vec> line = flatten_contour(glyph.contours[c], 0);
					for (Vec& v : line) { v.x += cx; v.y += cy; }
					group_lines[group_num + islands[c]].push_back(std::move(line));
				}
				group_num += count;
			}
		}
	}
	auto paint = [&](Rasterizer& raster, int target) {
		int index = -1;
		size_t order = 0;
		each_contour_meta(meta, [&](size_t kinds, int seg_num, size_t point) {
			index++;
			const int own = order < group.size() ? group[order] : 0;
			order++;
			if (own != target) return;
			if (contours && std::find(contours->begin(), contours->end(), index) == contours->end()) return;
			walk_contour(meta, kinds, seg_num, point, read,
				[&](Vec a, Vec c1) { raster.line(a, c1); },
				[&](Vec a, Vec c1, Vec c2, Vec e) { add_curve(raster, a, c1, c2, e); });
		}, only);
	};
	Rasterizer raster(w, h);
	if (!points.empty()) paint(raster, 0);
	raster.write(out, r, g, b);
	if (group_num) {
		std::vector<PIXEL_RGBA> temp((size_t)w * h);
		for (int target = 1; target <= group_num; target++) {
			Rasterizer separate(w, h);
			paint(separate, target);
			separate.write(temp.data(), r, g, b);
			for (size_t i = 0; i < temp.size(); i++) out[i].a = std::max(out[i].a, temp[i].a);
		}
		// 切り口の上の画素は2つの組が半分ずつ覆うので、濃い方を取ると薄くなる(黒い線が浮かぶ)。
		// 足すと、同じ辺を共有する行が濃くなる。そこで、足した時と違う画素だけ、点を細かく調べて実際に覆われている割合を求める
		if (!contours) {
			Rasterizer joint(w, h);
			for (int target = 1; target <= group_num; target++) paint(joint, target);
			joint.write(temp.data(), r, g, b);
			const int samples = 6;
			auto covered = [&](double x, double y) {
				for (const auto& lines : group_lines) {
					int winding = 0;
					for (const auto& line : lines) {
						for (size_t i = 0; i < line.size(); i++) {
							const Vec a = line[i], c = line[(i + 1) % line.size()];
							if ((a.y <= y) == (c.y <= y)) continue;
							if (x < a.x + (c.x - a.x) * (y - a.y) / (c.y - a.y)) winding += a.y < c.y ? 1 : -1;
						}
					}
					if (winding) return true;
				}
				return false;
			};
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					const size_t at = (size_t)y * w + x;
					if ((int)temp[at].a - (int)out[at].a <= 2) continue;
					int inside = 0;
					for (int sy = 0; sy < samples; sy++) {
						for (int sx = 0; sx < samples; sx++) {
							if (covered(x + (sx + 0.5) / samples, y + (sy + 0.5) / samples)) inside++;
						}
					}
					out[at].a = std::max<unsigned char>(out[at].a, (unsigned char)(inside * 255 / (samples * samples)));
				}
			}
		}
	}
}

// compose(data, w, h, アンカー, ハンドル, 輪郭情報, 元の座標, 元の輪郭情報, r, g, b, ox, oy, 文字):
// 輪郭を塗り、無編集の輪郭と塗りが同じ画素は控えた本体の画像のまま残す(動かしていない部分は画が変わらない)
// 点の追加・削除で構造が変わっても比べられるように、無編集の輪郭は元の構造で塗る
void module_compose(SCRIPT_MODULE_PARAM* param) {
	auto data = static_cast<PIXEL_RGBA*>(param->get_param_data(0));
	const int w = param->get_param_int(1), h = param->get_param_int(2);
	if (!data || w <= 0 || h <= 0) { param->set_error(u8"引数の値が正しくありません"); return; }
	const std::vector<double> meta = read_array(param, 5);
	const std::vector<double> points = merge_points(read_array(param, 3), read_array(param, 4), meta);
	const std::vector<double> base = read_array(param, 6);
	const std::vector<double> base_meta = read_array(param, 7);
	const double cx = w / 2.0 + param->get_param_double(11), cy = h / 2.0 + param->get_param_double(12);
	const int only = param->get_param_int(13);
	rasterize(data, w, h, points, meta, cx, cy, only, param->get_param_int(8), param->get_param_int(9), param->get_param_int(10));
	if (g_captured.empty()) return;
	std::vector<PIXEL_RGBA> pristine((size_t)w * h);
	rasterize(pristine.data(), w, h, base, base_meta, cx, cy, only, 0, 0, 0);
	// 保存時の丸めで生じる僅かな差は同じとみなす
	const int tolerance = 8;
	const int dx = (w - g_captured_w) / 2, dy = (h - g_captured_h) / 2;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			PIXEL_RGBA& p = data[(size_t)y * w + x];
			if (std::abs((int)p.a - (int)pristine[(size_t)y * w + x].a) > tolerance) continue;
			const int sx = x - dx, sy = y - dy;
			if (sx >= 0 && sx < g_captured_w && sy >= 0 && sy < g_captured_h) p = g_captured[(size_t)sy * g_captured_w + sx];
			else p.a = 0;
		}
	}
}

// 個別オブジェクトに分ける単位。1つの文字の中の輪郭の組
struct Part {
	int glyph;
	std::vector<int> contours;
};
thread_local std::vector<Part> g_parts;
thread_local std::vector<double> g_part_points, g_part_meta;

// parts(アンカー, ハンドル, 輪郭情報, 分け方, 文字): 個別オブジェクトの数を返す。分け方は1=文字ごと / 2=アイランドごと。
// 文字は番号(0～)で、-1なら全部。空白は数えない。結果はpart_rect・compose_partで使う
void module_parts(SCRIPT_MODULE_PARAM* param) {
	g_part_meta = read_array(param, 2);
	g_part_points = merge_points(read_array(param, 0), read_array(param, 1), g_part_meta);
	const int mode = param->get_param_int(3), only = param->get_param_int(4);
	g_parts.clear();
	if (!g_part_points.empty()) {
		const EdModel model = load_model(g_part_points, g_part_meta, 0);
		for (int g = 0; g < (int)model.size(); g++) {
			if ((only >= 0 && g != only) || model[g].contours.empty()) continue;
			const std::vector<int> islands = contour_islands(model[g], 0);
			const int count = mode == 2 ? *std::max_element(islands.begin(), islands.end()) + 1 : 1;
			for (int island = 0; island < count; island++) {
				Part part{ g, {} };
				for (int c = 0; c < (int)islands.size(); c++) if (mode != 2 || islands[c] == island) part.contours.push_back(c);
				g_parts.push_back(std::move(part));
			}
		}
	}
	param->push_result_int((int)g_parts.size());
}

// part_rect(番号, ox, oy, w, h): 全体の画像(w, h)の中での、その個別オブジェクトの範囲(x, y, 幅, 高さ)。偶奇は全体に揃える(位置を整数にするため)
void module_part_rect(SCRIPT_MODULE_PARAM* param) {
	const int index = param->get_param_int(0);
	const double ox = param->get_param_double(1), oy = param->get_param_double(2);
	const int w = param->get_param_int(3), h = param->get_param_int(4);
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
	bool any = false;
	if (index >= 0 && index < (int)g_parts.size()) {
		const Part& part = g_parts[index];
		int contour = -1;
		each_contour_meta(g_part_meta, [&](size_t kinds, int seg_num, size_t point) {
			contour++;
			if (std::find(part.contours.begin(), part.contours.end(), contour) == part.contours.end()) return;
			size_t used = 1;
			for (int s = 0; s < seg_num; s++) used += seg_size(g_part_meta[kinds + s]);
			for (size_t i = point; i < point + used; i++) {
				const double x = g_part_points[i * 2], y = g_part_points[i * 2 + 1];
				if (!any) { x0 = x1 = x; y0 = y1 = y; any = true; }
				x0 = std::min(x0, x); x1 = std::max(x1, x);
				y0 = std::min(y0, y); y1 = std::max(y1, y);
			}
		}, part.glyph);
	}
	const int left = (int)std::floor(x0 + w / 2.0 + ox) - 2, top = (int)std::floor(y0 + h / 2.0 + oy) - 2;
	int pw = (int)std::ceil(x1 + w / 2.0 + ox) + 2 - left, ph = (int)std::ceil(y1 + h / 2.0 + oy) + 2 - top;
	if ((pw - w) % 2) pw++;
	if ((ph - h) % 2) ph++;
	param->push_result_int(left);
	param->push_result_int(top);
	param->push_result_int(pw);
	param->push_result_int(ph);
}

// compose_part(data, 番号, 元の座標, 元の輪郭情報, r, g, b, ox, oy, x, y, w, h, pw, ph, 縁の濃さの上限): 個別オブジェクトの画像を作る。
// 元の座標があれば、動かしていない部分は控えた本体の画像のまま残す。他の個別オブジェクトと共有する画素はその輪郭の塗りで分ける
void module_compose_part(SCRIPT_MODULE_PARAM* param) {
	auto data = static_cast<PIXEL_RGBA*>(param->get_param_data(0));
	const int index = param->get_param_int(1);
	const int w = param->get_param_int(11), h = param->get_param_int(12), pw = param->get_param_int(13), ph = param->get_param_int(14);
	if (!data || pw <= 0 || ph <= 0 || index < 0 || index >= (int)g_parts.size()) { param->set_error(u8"引数の値が正しくありません"); return; }
	const Part& part = g_parts[index];
	const std::vector<double> base = read_array(param, 2);
	const std::vector<double> base_meta = read_array(param, 3);
	const int left = param->get_param_int(9), top = param->get_param_int(10), fringe_limit = param->get_param_int(15);
	const double cx = w / 2.0 + param->get_param_double(7) - left, cy = h / 2.0 + param->get_param_double(8) - top;
	const size_t total = (size_t)pw * ph;
	std::vector<PIXEL_RGBA> whole(total), pristine;
	rasterize(data, pw, ph, g_part_points, g_part_meta, cx, cy, part.glyph, param->get_param_int(4), param->get_param_int(5), param->get_param_int(6), &part.contours);
	rasterize(whole.data(), pw, ph, g_part_points, g_part_meta, cx, cy, part.glyph, 0, 0, 0);
	const bool use_base = !base.empty() && !g_captured.empty();
	if (use_base) {
		pristine.resize(total);
		rasterize(pristine.data(), pw, ph, base, base_meta, cx, cy, part.glyph, 0, 0, 0);
	}
	const int tolerance = 8;
	const std::vector<PIXEL_RGBA> own(data, data + total);
	for (int y = 0; use_base && y < ph; y++) {
		for (int x = 0; x < pw; x++) {
			const size_t at = (size_t)y * pw + x;
			if (std::abs((int)whole[at].a - (int)pristine[at].a) > tolerance) continue;
			const int sx = x + left, sy = y + top;
			if (sx < 0 || sx >= g_captured_w || sy < 0 || sy >= g_captured_h) continue;
			const PIXEL_RGBA& body = g_captured[(size_t)sy * g_captured_w + sx];
			if (own[at].a) {
				// 切り口の画素も字全体の濃さにする(半分ずつの2枚を重ねると切り目が薄くなる)
				data[at] = body;
			} else if (!whole[at].a && body.a && body.a <= fringe_limit) {
				// 自前の塗りには入らない本体の縁は、隣がこの個別オブジェクトなら残す。
				// 展開したオブジェクトは他のアイランドの輪郭を持たないので、隣のアイランドの濃い画素を上限で除く
				bool beside = false;
				for (int dy = -1; dy <= 1 && !beside; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int nx = x + dx, ny = y + dy;
						if (nx >= 0 && nx < pw && ny >= 0 && ny < ph && own[(size_t)ny * pw + nx].a) { beside = true; break; }
					}
				}
				if (beside) data[at] = body;
			}
		}
	}
}

//---------------------------------------------------------------------
//	ペンで書くアニメーション: 字の画素ごとに、筆順の中心線のどの時点で塗られるかを求める
//---------------------------------------------------------------------

// プラグインと同じ場所の筆順データ(KanjiVGを変換したもの)。文字コード→1行
struct StrokeDatabase {
	std::string text;
	std::map<UINT32, std::pair<size_t, size_t>> lines;
};

const StrokeDatabase& stroke_database() {
	static StrokeDatabase database;
	static std::once_flag once;
	std::call_once(once, [] {
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&stroke_database, &self);
		wchar_t path[MAX_PATH * 2];
		const DWORD length = GetModuleFileNameW(self, path, MAX_PATH * 2);
		std::wstring file(path, length);
		file = file.substr(0, file.find_last_of(L'\\') + 1) + L"TextPath_TeStrokes.txt";
		FILE* fp = nullptr;
		if (_wfopen_s(&fp, file.c_str(), L"rb") || !fp) return;
		fseek(fp, 0, SEEK_END);
		const long size = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		database.text.resize(size > 0 ? size : 0);
		if (size > 0) fread(database.text.data(), 1, size, fp);
		fclose(fp);
		size_t at = 0;
		const std::string& text = database.text;
		while (at < text.size()) {
			size_t end = text.find('\n', at);
			if (end == std::string::npos) end = text.size();
			if (text[at] != '#') {
				char* stop = nullptr;
				const unsigned long code = strtoul(text.c_str() + at, &stop, 16);
				if (*stop == '\t') database.lines[(UINT32)code] = { (size_t)(stop - text.c_str()) + 1, end };
			}
			at = end + 1;
		}
	});
	return database;
}

// SVGのパス(M, C, S, L, H, Vと相対指定)を折れ線にする
std::vector<Vec> parse_svg_path(const char* p, const char* end) {
	std::vector<Vec> line;
	Vec cur{ 0, 0 }, control{ 0, 0 };
	char command = 0;
	auto number = [&]() {
		char* stop = nullptr;
		const double value = strtod(p, &stop);
		p = stop;
		while (p < end && (*p == ',' || *p == ' ')) p++;
		return value;
	};
	auto cubic = [&](Vec c1, Vec c2, Vec e) {
		for (int k = 1; k <= 12; k++) {
			const double t = k / 12.0, u = 1 - t;
			line.push_back({ u * u * u * cur.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * e.x,
				u * u * u * cur.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * e.y });
		}
		control = c2;
		cur = e;
	};
	while (p < end) {
		if (isalpha((unsigned char)*p)) command = *p++;
		while (p < end && (*p == ',' || *p == ' ')) p++;
		if (p >= end) break;
		const bool rel = islower((unsigned char)command) != 0;
		const Vec base = rel ? cur : Vec{ 0, 0 };
		switch (tolower((unsigned char)command)) {
		case 'm': {
			const double x = number(), y = number();
			cur = { base.x + x, base.y + y };
			control = cur;
			line.push_back(cur);
			command = rel ? 'l' : 'L';
			break;
		}
		case 'l': {
			const double x = number(), y = number();
			cur = { base.x + x, base.y + y };
			control = cur;
			line.push_back(cur);
			break;
		}
		case 'h': cur.x = base.x + number(); control = cur; line.push_back(cur); break;
		case 'v': cur.y = base.y + number(); control = cur; line.push_back(cur); break;
		case 'c': {
			const double a = number(), b = number(), c = number(), d = number(), e = number(), f = number();
			cubic({ base.x + a, base.y + b }, { base.x + c, base.y + d }, { base.x + e, base.y + f });
			break;
		}
		case 's': {
			const double c = number(), d = number(), e = number(), f = number();
			cubic({ cur.x * 2 - control.x, cur.y * 2 - control.y }, { base.x + c, base.y + d }, { base.x + e, base.y + f });
			break;
		}
		default:
			p++;	// 閉じる指定などは筆順データに無いので読み飛ばす
			break;
		}
	}
	return line;
}

std::vector<std::vector<Vec>> find_strokes(UINT32 code) {
	std::vector<std::vector<Vec>> strokes;
	const StrokeDatabase& database = stroke_database();
	const auto found = database.lines.find(code);
	if (found == database.lines.end()) return strokes;
	size_t at = found->second.first;
	const size_t end = found->second.second;
	while (at < end) {
		size_t next = database.text.find('\t', at);
		if (next == std::string::npos || next > end) next = end;
		strokes.push_back(parse_svg_path(database.text.c_str() + at, database.text.c_str() + next));
		at = next + 1;
	}
	return strokes;
}

// 画素の時刻の表。座標は輪郭の座標(行の左上が原点)。-1は字の外
struct PenMap {
	UINT32 key = 0;
	int x0 = 0, y0 = 0, w = 0, h = 0;
	double total = 1;	// 全ての画の長さ(ピクセル)
	std::vector<float> time;
	// 複数のアイランドが覆う画素の、アイランドごとの時刻と覆う割合。切り口の濃さを、書かれたアイランドの分だけにするために使う
	struct Layer { float time, cover; };
	std::vector<int> layer_index;	// 画素ごとのlayersの番号(無ければ-1)
	std::vector<std::vector<Layer>> layers;
};

double polyline_length(const std::vector<Vec>& line) {
	double length = 0;
	for (size_t i = 1; i < line.size(); i++) length += distance(line[i - 1], line[i]);
	return length;
}

// 折れ線の上で最も近い点の距離と、始点からの長さ
void nearest_on_line(const std::vector<Vec>& line, Vec v, double& out_distance, double& out_along, size_t& out_index) {
	out_distance = 1e30;
	out_along = 0;
	out_index = 0;
	double walked = 0;
	for (size_t i = 1; i < line.size(); i++) {
		const Vec a = line[i - 1], b = line[i];
		const double dx = b.x - a.x, dy = b.y - a.y, square = dx * dx + dy * dy;
		double t = square > 0 ? ((v.x - a.x) * dx + (v.y - a.y) * dy) / square : 0;
		t = std::clamp(t, 0.0, 1.0);
		const double ex = a.x + dx * t - v.x, ey = a.y + dy * t - v.y;
		const double d = ex * ex + ey * ey;
		if (d < out_distance) { out_distance = d; out_along = walked + std::sqrt(square) * t; out_index = i; }
		walked += std::sqrt(square);
	}
	out_distance = std::sqrt(out_distance);
}

// 字の中で、外までの距離を求める(チャンファー距離)
std::vector<float> ink_distance(const std::vector<PIXEL_RGBA>& cover, int w, int h) {
	std::vector<float> field((size_t)w * h, 0);
	const float far_value = 1e9f, side = 1.0f, corner = 1.414f;
	for (size_t i = 0; i < field.size(); i++) field[i] = cover[i].a >= 128 ? far_value : 0;
	for (int y = 1; y < h; y++) {
		for (int x = 1; x + 1 < w; x++) {
			float& f = field[(size_t)y * w + x];
			f = std::min({ f, field[(size_t)y * w + x - 1] + side, field[(size_t)(y - 1) * w + x] + side,
				field[(size_t)(y - 1) * w + x - 1] + corner, field[(size_t)(y - 1) * w + x + 1] + corner });
		}
	}
	for (int y = h - 2; y >= 0; y--) {
		for (int x = w - 2; x >= 1; x--) {
			float& f = field[(size_t)y * w + x];
			f = std::min({ f, field[(size_t)y * w + x + 1] + side, field[(size_t)(y + 1) * w + x] + side,
				field[(size_t)(y + 1) * w + x + 1] + corner, field[(size_t)(y + 1) * w + x - 1] + corner });
		}
	}
	return field;
}

// 折れ線を、ほぼ等間隔の点に置き直す
std::vector<Vec> resample_line(const std::vector<Vec>& line, double step) {
	std::vector<Vec> out;
	if (line.empty()) return out;
	out.push_back(line[0]);
	double remain = step;
	for (size_t i = 1; i < line.size(); i++) {
		Vec a = line[i - 1];
		double d = distance(a, line[i]);
		while (d >= remain && d > 0) {
			a = lerp(a, line[i], remain / d);
			out.push_back(a);
			d = distance(a, line[i]);
			remain = step;
		}
		remain -= d;
	}
	if (!same_point(out.back(), line.back())) out.push_back(line.back());
	return out;
}

// 中心線を字の実際の線の中央へ寄せる: 点ごとに、線の法線の方向で外までの距離が最大の所を探す。
// 交差や隣の画で飛ばないように、ずれは近傍の中央値で揃える
void snap_line(std::vector<Vec>& line, const std::vector<float>& field, int w, int h, double reach) {
	const int count = (int)line.size();
	if (count < 2) return;
	auto at = [&](Vec v) {
		const int x = (int)v.x, y = (int)v.y;
		return x >= 0 && x < w && y >= 0 && y < h ? field[(size_t)y * w + x] : 0.0f;
	};
	std::vector<double> offsets(count, 0);
	std::vector<Vec> normals(count);
	for (int i = 0; i < count; i++) {
		const Vec a = line[std::max(i - 2, 0)], b = line[std::min(i + 2, count - 1)];
		const double length = std::max(distance(a, b), 1e-6);
		normals[i] = { -(b.y - a.y) / length, (b.x - a.x) / length };
		float best = at(line[i]);
		for (double d = 0.5; d <= reach; d += 0.5) {
			for (const double side : { d, -d }) {
				const float value = at({ line[i].x + normals[i].x * side, line[i].y + normals[i].y * side });
				// 同じ深さなら近い方を選ぶ(少し深いだけなら動かない)
				if (value > best + 0.5f) { best = value; offsets[i] = side; }
			}
		}
	}
	const int window = std::max(2, (int)std::ceil(reach * 1.5 / 2));
	std::vector<double> smooth(count);
	for (int i = 0; i < count; i++) {
		std::vector<double> near_values;
		for (int k = std::max(i - window, 0); k <= std::min(i + window, count - 1); k++) near_values.push_back(offsets[k]);
		std::nth_element(near_values.begin(), near_values.begin() + near_values.size() / 2, near_values.end());
		smooth[i] = near_values[near_values.size() / 2];
	}
	for (int i = 0; i < count; i++) line[i] = { line[i].x + normals[i].x * smooth[i], line[i].y + normals[i].y * smooth[i] };
}

// 折れ線の点を、形が保てる範囲で減らす(Ramer-Douglas-Peucker)
void simplify_line(const std::vector<Vec>& line, size_t first, size_t last, double tolerance, std::vector<bool>& keep) {
	double worst = 0;
	size_t at = first;
	const Vec a = line[first], b = line[last];
	const double length = distance(a, b);
	for (size_t i = first + 1; i < last; i++) {
		const double d = length > 0 ? std::abs((b.x - a.x) * (a.y - line[i].y) - (a.x - line[i].x) * (b.y - a.y)) / length : distance(a, line[i]);
		if (d > worst) { worst = d; at = i; }
	}
	if (worst <= tolerance) return;
	keep[at] = true;
	simplify_line(line, first, at, tolerance, keep);
	simplify_line(line, at, last, tolerance, keep);
}

std::vector<std::vector<Vec>> default_strokes(const EdGlyph& glyph) {
	std::vector<std::vector<Vec>> strokes = find_strokes((UINT32)glyph.head[0]);
	if (strokes.empty()) return strokes;
	bool any = false;
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
	std::vector<std::vector<Vec>> contours;
	for (const EdContour& c : glyph.contours) {
		contours.push_back(flatten_contour(c, 0));
		for (const Vec& v : contours.back()) {
			if (!any) { x0 = x1 = v.x; y0 = y1 = v.y; any = true; }
			x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
			y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
		}
	}
	if (!any) return {};
	const int left = (int)std::floor(x0) - 3, top = (int)std::floor(y0) - 3;
	const int w = (int)std::ceil(x1) + 3 - left, h = (int)std::ceil(y1) + 3 - top;
	Rasterizer raster(w, h);
	for (const auto& line : contours) {
		for (size_t i = 0; i < line.size(); i++) {
			const Vec a = line[i], b = line[(i + 1) % line.size()];
			raster.line({ a.x - left, a.y - top }, { b.x - left, b.y - top });
		}
	}
	std::vector<PIXEL_RGBA> cover((size_t)w * h);
	raster.write(cover.data(), 0, 0, 0);
	double area = 0;
	for (const PIXEL_RGBA& p : cover) area += p.a / 255.0;
	double cx0 = 1e30, cx1 = -1e30, cy0 = 1e30, cy1 = -1e30, unit = 0;
	for (const auto& line : strokes) {
		unit += polyline_length(line);
		for (const Vec& v : line) {
			cx0 = std::min(cx0, v.x); cx1 = std::max(cx1, v.x);
			cy0 = std::min(cy0, v.y); cy1 = std::max(cy1, v.y);
		}
	}
	// 線の太さ = 面積 / 長さ。中心線の範囲は字の範囲より太さの半分だけ内側にある
	const double ink_w = x1 - x0, ink_h = y1 - y0;
	const double rough = std::max(ink_w, ink_h) / std::max({ cx1 - cx0, cy1 - cy0, 1.0 });
	const double thick = area / std::max(unit * rough, 1.0);
	const double sx = cx1 - cx0 > 1 ? std::max(ink_w - thick, 1.0) / (cx1 - cx0) : rough;
	const double sy = cy1 - cy0 > 1 ? std::max(ink_h - thick, 1.0) / (cy1 - cy0) : rough;
	const std::vector<float> field = ink_distance(cover, w, h);
	for (auto& line : strokes) {
		for (Vec& v : line) v = { (x0 + x1) / 2 + (v.x - (cx0 + cx1) / 2) * sx - left, (y0 + y1) / 2 + (v.y - (cy0 + cy1) / 2) * sy - top };
		line = resample_line(line, 2);
		snap_line(line, field, w, h, std::max(thick, 3.0));
		// 編集しやすいように点を減らす
		std::vector<bool> keep(line.size(), false);
		keep.front() = keep.back() = true;
		if (line.size() >= 2) simplify_line(line, 0, line.size() - 1, 1.0, keep);
		std::vector<Vec> reduced;
		for (size_t i = 0; i < line.size(); i++) if (keep[i]) reduced.push_back({ line[i].x + left, line[i].y + top });
		line = std::move(reduced);
	}
	return strokes;
}

// 輪郭の凹んだ角(字の内側へ折れる点)。穴は向きが逆なので、向きの判定は外周も穴も同じ式でよい
std::vector<Vec> reflex_corners(const EdGlyph& glyph) {
	std::vector<Vec> corners;
	double largest = 0;
	for (const EdContour& c : glyph.contours) {
		const double area = signed_area(flatten_contour(c, 0));
		if (std::abs(area) > std::abs(largest)) largest = area;
	}
	for (const EdContour& c : glyph.contours) {
		const int size = (int)c.nodes.size();
		for (int i = 0; i < size; i++) {
			const EdNode& node = c.nodes[i];
			const EdNode& prev = c.nodes[(i + size - 1) % size];
			const EdNode& after = c.nodes[(i + 1) % size];
			Vec from = prev.p[0], to = after.p[0];
			if (prev.next.curve && !same_point(prev.next.c2[0], node.p[0])) from = prev.next.c2[0];
			if (node.next.curve && !same_point(node.next.c1[0], node.p[0])) to = node.next.c1[0];
			const double ax = node.p[0].x - from.x, ay = node.p[0].y - from.y, bx = to.x - node.p[0].x, by = to.y - node.p[0].y;
			const double angle = std::atan2(ax * by - ay * bx, ax * bx + ay * by);
			if (angle * largest < 0 && std::abs(angle) > 20 * 3.14159265 / 180) corners.push_back(node.p[0]);
		}
	}
	return corners;
}

// 点fromから向きdirへ進んで、輪郭に最初に当たる点(手前のminimum以内は無視)
bool ray_to_outline(const EdGlyph& glyph, Vec from, Vec dir, double minimum, Vec& hit) {
	double best = 1e30;
	for (const EdContour& c : glyph.contours) {
		const std::vector<Vec> line = flatten_contour(c, 0);
		for (size_t i = 0; i < line.size(); i++) {
			const Vec a = line[i], b = line[(i + 1) % line.size()];
			const double ex = b.x - a.x, ey = b.y - a.y;
			const double cross = dir.x * ey - dir.y * ex;
			if (std::abs(cross) < 1e-9) continue;
			const double t = ((a.x - from.x) * ey - (a.y - from.y) * ex) / cross;
			const double u = ((a.x - from.x) * dir.y - (a.y - from.y) * dir.x) / cross;
			if (t > minimum && u >= 0 && u <= 1 && t < best) best = t;
		}
	}
	if (best >= 1e30) return false;
	hit = { from.x + dir.x * best, from.y + dir.y * best };
	return true;
}

Vec unit_vector(Vec a, Vec b) {
	const double length = std::max(distance(a, b), 1e-9);
	return { (b.x - a.x) / length, (b.y - a.y) / length };
}

// 切る先の点の近くに輪郭の点があれば、その点で切る(cornerは切り始めの点)
Vec snap_to_node(const EdGlyph& glyph, Vec at, Vec corner, double reach) {
	double best = reach;
	Vec result = at;
	for (const EdContour& c : glyph.contours) {
		for (const EdNode& n : c.nodes) {
			const double d = distance(n.p[0], at);
			if (d < best && distance(n.p[0], corner) > 1) { best = d; result = n.p[0]; }
		}
	}
	return result;
}

std::vector<Proposal> propose_splits(const EdGlyph& glyph, const std::vector<std::vector<Vec>>& source, int glyph_index, int& missed) {
	std::vector<Proposal> result;
	const int stroke_num = (int)source.size();
	if (stroke_num < 2 || glyph.contours.empty()) return result;
	// 字の中の、外までの距離
	bool any = false;
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
	std::vector<std::vector<Vec>> contours;
	for (const EdContour& c : glyph.contours) {
		contours.push_back(flatten_contour(c, 0));
		for (const Vec& v : contours.back()) {
			if (!any) { x0 = x1 = v.x; y0 = y1 = v.y; any = true; }
			x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
			y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
		}
	}
	const int left = (int)std::floor(x0) - 3, top = (int)std::floor(y0) - 3;
	const int w = (int)std::ceil(x1) + 3 - left, h = (int)std::ceil(y1) + 3 - top;
	Rasterizer raster(w, h);
	for (const auto& line : contours) {
		for (size_t i = 0; i < line.size(); i++) {
			const Vec a = line[i], b = line[(i + 1) % line.size()];
			raster.line({ a.x - left, a.y - top }, { b.x - left, b.y - top });
		}
	}
	std::vector<PIXEL_RGBA> cover((size_t)w * h);
	raster.write(cover.data(), 0, 0, 0);
	const std::vector<float> field = ink_distance(cover, w, h);
	auto depth = [&](Vec v) {
		const int x = (int)(v.x - left), y = (int)(v.y - top);
		return x >= 0 && x < w && y >= 0 && y < h ? (double)field[(size_t)y * w + x] : 0.0;
	};
	// 字のつながった塊の番号
	std::vector<int> ink_label((size_t)w * h, 0);
	{
		int next_label = 0;
		std::vector<size_t> stack;
		for (size_t start = 0; start < ink_label.size(); start++) {
			if (ink_label[start] || field[start] <= 0) continue;
			next_label++;
			ink_label[start] = next_label;
			stack.push_back(start);
			while (!stack.empty()) {
				const size_t at = stack.back();
				stack.pop_back();
				const int px = (int)(at % w);
				auto visit = [&](size_t n) { if (!ink_label[n] && field[n] > 0) { ink_label[n] = next_label; stack.push_back(n); } };
				if (px > 0) visit(at - 1);
				if (px + 1 < w) visit(at + 1);
				if (at >= (size_t)w) visit(at - w);
				if (at + w < ink_label.size()) visit(at + w);
			}
		}
	}
	auto same_ink = [&](Vec p, Vec q) {
		auto label = [&](Vec v) {
			const int x = (int)(v.x - left), y = (int)(v.y - top);
			return x >= 0 && x < w && y >= 0 && y < h ? ink_label[(size_t)y * w + x] : 0;
		};
		// 線の点が字の外へ少し出ていても、中間の点で判定できるようにする
		int a_label = label(p), b_label = label(q);
		for (int step = 1; step < 8 && !a_label; step++) a_label = label(lerp(p, q, step / 8.0));
		for (int step = 1; step < 8 && !b_label; step++) b_label = label(lerp(q, p, step / 8.0));
		return a_label && a_label == b_label;
	};
	// 画を細かい点にして、線の太さの半分を測る
	std::vector<std::vector<Vec>> lines(stroke_num);
	std::vector<double> halves(stroke_num), lengths(stroke_num);
	for (int s = 0; s < stroke_num; s++) {
		lines[s] = resample_line(source[s], 2);
		lengths[s] = polyline_length(lines[s]);
		std::vector<double> values;
		for (const Vec& v : lines[s]) values.push_back(depth(v));
		if (values.empty()) { halves[s] = 1.5; continue; }
		std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
		halves[s] = std::max(1.5, values[values.size() / 2]);
	}
	const std::vector<Vec> corners = reflex_corners(glyph);
	auto tangent = [&](int s, size_t i) {
		const auto& line = lines[s];
		return unit_vector(line[i >= 3 ? i - 3 : 0], line[std::min(i + 3, line.size() - 1)]);
	};
	struct Contact { Vec at; size_t ia, ib; int a, b; };
	std::vector<Contact> all_contacts;
	for (int a = 0; a < stroke_num; a++) {
		for (int b = a + 1; b < stroke_num; b++) {
			if (lines[a].size() < 2 || lines[b].size() < 2) continue;
			// 接点: 線の交点と、一方の端がもう一方に届く所
			std::vector<Contact> contacts;
			const double near_reach = std::max(halves[a], halves[b]) * 2;
			for (size_t i = 0; i + 1 < lines[a].size(); i++) {
				for (size_t k = 0; k + 1 < lines[b].size(); k++) {
					const Vec p = lines[a][i], q = lines[b][k];
					const double rx = lines[a][i + 1].x - p.x, ry = lines[a][i + 1].y - p.y;
					const double sx = lines[b][k + 1].x - q.x, sy = lines[b][k + 1].y - q.y;
					const double cross = rx * sy - ry * sx;
					if (std::abs(cross) < 1e-9) continue;
					const double t = ((q.x - p.x) * sy - (q.y - p.y) * sx) / cross;
					const double u = ((q.x - p.x) * ry - (q.y - p.y) * rx) / cross;
					if (t < 0 || t > 1 || u < 0 || u > 1) continue;
					const Vec at{ p.x + rx * t, p.y + ry * t };
					const bool duplicate = std::any_of(contacts.begin(), contacts.end(), [&](const Contact& c) { return distance(c.at, at) < near_reach; });
					if (!duplicate) contacts.push_back({ at, i, k, a, b });
				}
			}
			// 交点とは別に、画の端がもう一方へ届いている所も接点にする(「あ」の2画目は上で交差し、下で3画目に届く)
			auto add_end_contact = [&](bool end_of_a, bool at_start) {
				const auto& ends = end_of_a ? lines[a] : lines[b];
				const auto& others = end_of_a ? lines[b] : lines[a];
				const size_t end_index = at_start ? 0 : ends.size() - 1;
				double best = 1e30;
				size_t best_other = 0;
				for (size_t i = 0; i < others.size(); i++) {
					const double d = distance(ends[end_index], others[i]);
					if (d < best) { best = d; best_other = i; }
				}
				if (best > (halves[a] + halves[b]) * 1.3) return;
				// 間の半分以上が字の中で、2点が字の中でつながっている時だけ接していると見なす
				// (「永」の5画目の始点と2画目の間には、付け根の隙間が少し入る)
				const Vec p = ends[end_index], q = others[best_other];
				int inside = 0;
				for (int step = 0; step <= 8; step++) if (depth(lerp(p, q, step / 8.0)) > 0) inside++;
				if (inside < 5 || !same_ink(p, q)) return;
				const Vec at = lerp(p, q, 0.5);
				if (std::any_of(contacts.begin(), contacts.end(), [&](const Contact& c) { return distance(c.at, at) < near_reach; })) return;
				contacts.push_back(end_of_a ? Contact{ at, end_index, best_other, a, b } : Contact{ at, best_other, end_index, a, b });
			};
			add_end_contact(true, true);
			add_end_contact(true, false);
			add_end_contact(false, true);
			add_end_contact(false, false);
			all_contacts.insert(all_contacts.end(), contacts.begin(), contacts.end());
		}
	}
	// 多叉路: 近い接点をまとめ、3画以上が集まる所は周りの角を全て使う1つの案にする
	// (2画ずつの案にすると、同じ角を取り合って分割が崩れる)
	const int contact_num = (int)all_contacts.size();
	std::vector<int> contact_group(contact_num);
	for (int i = 0; i < contact_num; i++) contact_group[i] = i;
	std::function<int(int)> group_of = [&](int i) { return contact_group[i] == i ? i : contact_group[i] = group_of(contact_group[i]); };
	for (int i = 0; i < contact_num; i++) {
		for (int k = i + 1; k < contact_num; k++) {
			const Contact& p = all_contacts[i];
			const Contact& q = all_contacts[k];
			const double limit = std::max({ halves[p.a], halves[p.b], halves[q.a], halves[q.b] }) * 2.5;
			if (distance(p.at, q.at) <= limit) contact_group[group_of(i)] = group_of(k);
		}
	}
	std::vector<bool> in_junction(contact_num, false);
	for (int root = 0; root < contact_num; root++) {
		if (group_of(root) != root) continue;
		std::vector<int> members, strokes_here;
		Vec center{ 0, 0 };
		double reach = 0;
		for (int i = 0; i < contact_num; i++) {
			if (group_of(i) != root) continue;
			const Contact& c = all_contacts[i];
			members.push_back(i);
			for (const int s : { c.a, c.b }) if (std::find(strokes_here.begin(), strokes_here.end(), s) == strokes_here.end()) strokes_here.push_back(s);
			center.x += c.at.x;
			center.y += c.at.y;
			reach = std::max(reach, (halves[c.a] + halves[c.b]) * 1.8);
		}
		if (strokes_here.size() < 3) continue;
		center.x /= members.size();
		center.y /= members.size();
		Proposal proposal;
		proposal.glyph = glyph_index;
		proposal.kind = Proposal::CROSS;
		proposal.junction_strokes = (int)strokes_here.size();
		for (const Vec& c : corners) if (distance(c, center) <= reach) proposal.points.push_back(c);
		if (proposal.points.size() < 3) continue;
		for (int i : members) in_junction[i] = true;
		result.push_back(std::move(proposal));
	}
	for (int contact_index = 0; contact_index < contact_num; contact_index++) {
		if (in_junction[contact_index]) continue;
		const Contact& contact = all_contacts[contact_index];
		const int a = contact.a, b = contact.b;
		const double along_a = polyline_length(std::vector<Vec>(lines[a].begin(), lines[a].begin() + contact.ia + 1));
		const double along_b = polyline_length(std::vector<Vec>(lines[b].begin(), lines[b].begin() + contact.ib + 1));
		const bool end_a = std::min(along_a, lengths[a] - along_a) <= halves[b] + halves[a];
		const bool end_b = std::min(along_b, lengths[b] - along_b) <= halves[a] + halves[b];
		const Vec X = contact.at, dir_a = tangent(a, contact.ia), dir_b = tangent(b, contact.ib);
		const double reach = (halves[a] + halves[b]) * 1.8;
		Proposal proposal;
		proposal.glyph = glyph_index;
		proposal.stroke_a = a;
		proposal.stroke_b = b;
		auto nearest_corner = [&](auto accept, Vec& out) {
			double best = reach;
			bool found = false;
			for (const Vec& c : corners) {
				const double d = distance(c, X);
				if (d <= best && accept(Vec{ c.x - X.x, c.y - X.y })) { best = d; out = c; found = true; }
			}
			return found;
		};
		bool made = false;
		// T字: 端の画の両側の角と、その向きに通る画を横切った反対側の2点
		// sharedがfalseなら、共通部分は貫く画だけのものにする(端の画は2つの角を結ぶ線で切る)。trueなら両方の画に持たせる
		auto try_tee = [&](bool tee_end_b, Proposal& out, bool shared = false) {
			const int end_stroke = tee_end_b ? b : a;
			const size_t end_index = tee_end_b ? contact.ib : contact.ia;
			const double end_along = tee_end_b ? along_b : along_a;
			const bool at_start = end_along < lengths[end_stroke] / 2;
			// 端から相手の画へ向かう向き。端の近くは線が曲がっていることがあるので、接点の手前の長い区間で測る
			const auto& end_line = lines[end_stroke];
			const size_t span = std::max<size_t>(3, (size_t)(halves[end_stroke] * 2));
			const Vec behind = end_line[at_start ? std::min(end_index + span, end_line.size() - 1) : (end_index >= span ? end_index - span : 0)];
			const Vec into = unit_vector(behind, X);
			const Vec normal{ -into.y, into.x };
			Vec left_corner{}, right_corner{}, left_far{}, right_far{};
			const double width = std::min(halves[a], halves[b]);
			const bool has_left = nearest_corner([&](Vec v) { return v.x * normal.x + v.y * normal.y > 0; }, left_corner);
			const bool has_right = nearest_corner([&](Vec v) { return v.x * normal.x + v.y * normal.y < 0; }, right_corner);
			if (has_left && has_right && !shared) {
				out.kind = Proposal::TEE;
				out.points = { left_corner, right_corner };
				return true;
			}
			if (has_left && has_right
				&& ray_to_outline(glyph, left_corner, into, width * 0.5, left_far)
				&& ray_to_outline(glyph, right_corner, into, width * 0.5, right_far)) {
				out.kind = Proposal::TEE;
				out.points = { left_corner, right_corner, left_far, right_far };
				return true;
			} else if (has_left || has_right) {
				// 枝分かれ: 凹んだ角が片側にしか無い時は、その角から通る画の向きに字の中を切る(「永」の2画目と5画目)
				const Vec corner = has_left ? left_corner : right_corner;
				const Vec through = tee_end_b ? dir_a : dir_b;
				const Vec reverse{ -through.x, -through.y };
				const double probe = std::max(3.0, width);
				const Vec ahead{ corner.x + through.x * probe, corner.y + through.y * probe };
				const Vec back{ corner.x + reverse.x * probe, corner.y + reverse.y * probe };
				const Vec cut = depth(ahead) >= depth(back) ? through : reverse;
				Vec far_point{};
				if (ray_to_outline(glyph, corner, cut, 1.0, far_point)) {
					out.kind = Proposal::CORNER;
					out.points = { corner, snap_to_node(glyph, far_point, corner, std::max(halves[a], halves[b])) };
					return true;
				}
			}
			return false;
		};
		// 交差: 2本の線で分けた4つの区画に1つずつ角がある
		auto try_cross = [&](Proposal& out) {
			std::vector<Vec> found_points;
			for (const double sa : { 1.0, -1.0 }) {
				for (const double sb : { 1.0, -1.0 }) {
					Vec corner{};
					const bool found = nearest_corner([&](Vec v) {
						return (v.x * -dir_a.y + v.y * dir_a.x) * sa > 0 && (v.x * -dir_b.y + v.y * dir_b.x) * sb > 0;
					}, corner);
					if (!found) return false;
					found_points.push_back(corner);
				}
			}
			out.kind = Proposal::CROSS;
			out.points = std::move(found_points);
			return true;
		};
		if (!end_a && !end_b) {
			made = try_cross(proposal);
			if (!made) {
				// 角が揃わない時は、線の端が接合を突き抜けているT字とみなす(「あ」の2画目の終わり)
				const double rest_a = std::min(along_a, lengths[a] - along_a), rest_b = std::min(along_b, lengths[b] - along_b);
				if (std::min(rest_a, rest_b) <= (halves[a] + halves[b]) * 3) made = try_tee(rest_b < rest_a, proposal);
			}
		} else if (end_a != end_b) {
			made = try_tee(end_b, proposal);
		} else {
			// L字: 内側の角から接点を通って外側まで切る
			Vec corner{}, far_point{};
			if (nearest_corner([](Vec) { return true; }, corner) && distance(corner, X) > 1e-6
				&& ray_to_outline(glyph, corner, unit_vector(corner, X), 1.0, far_point)) {
				proposal.kind = Proposal::CORNER;
				proposal.points = { corner, snap_to_node(glyph, far_point, corner, std::max(halves[a], halves[b])) };
				made = true;
			}
		}
		if (made) {
			// 切り替えられる別の切り方を集める(交差と判定したが実はT字、などを人が直せるように)
			auto add_variant = [&](const Proposal& v) {
				for (const Proposal::Variant& existing : proposal.variants) {
					if (existing.points.size() != v.points.size()) continue;
					bool same_points = true;
					for (size_t i = 0; i < v.points.size(); i++) if (distance(existing.points[i], v.points[i]) >= 1) same_points = false;
					if (same_points) return;
				}
				proposal.variants.push_back({ v.kind, v.points });
			};
			add_variant(proposal);
			Proposal other = proposal;
			// 「共通部分を両方に持たせるT字」を最初の切り替え先にする
			const double rest_of_a = std::min(along_a, lengths[a] - along_a), rest_of_b = std::min(along_b, lengths[b] - along_b);
			if (try_tee(end_a != end_b ? end_b : rest_of_b < rest_of_a, other, true)) add_variant(other);
			if (try_cross(other)) add_variant(other);
			if (try_tee(false, other)) add_variant(other);
			if (try_tee(true, other)) add_variant(other);
			if (try_tee(false, other, true)) add_variant(other);
			if (try_tee(true, other, true)) add_variant(other);
			// 別の画の組から同じ切り方が出たら1つにまとめる
			const bool same = std::any_of(result.begin(), result.end(), [&](const Proposal& other) {
				if (other.points.size() != proposal.points.size()) return false;
				for (const Vec& v : proposal.points) {
					if (std::none_of(other.points.begin(), other.points.end(), [&](const Vec& o) { return distance(o, v) < 1; })) return false;
				}
				return true;
			});
			if (!same) result.push_back(std::move(proposal));
		} else {
			missed++;
		}
	}
	return result;
}

// customは編集された筆順、referenceはその筆順を編集した形状(形状1)の輪郭
std::shared_ptr<PenMap> build_pen_map(const std::vector<double>& points, const std::vector<double>& meta, UINT32 key,
	const std::vector<GlyphStrokes>& custom, const std::vector<double>& reference) {
	auto map = std::make_shared<PenMap>();
	map->key = key;
	const EdModel model = load_model(points, meta, 0);
	const EdModel reference_model = reference.empty() ? EdModel() : load_model(reference, meta, 0);
	bool any = false;
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
	struct GlyphBox { double x0, y0, x1, y1; bool any; };
	std::vector<GlyphBox> boxes(model.size(), GlyphBox{ 0, 0, 0, 0, false });
	for (size_t g = 0; g < model.size(); g++) {
		for (const EdContour& c : model[g].contours) {
			for (const Vec& v : flatten_contour(c, 0)) {
				GlyphBox& box = boxes[g];
				if (!box.any) box = { v.x, v.y, v.x, v.y, true };
				box.x0 = std::min(box.x0, v.x); box.x1 = std::max(box.x1, v.x);
				box.y0 = std::min(box.y0, v.y); box.y1 = std::max(box.y1, v.y);
				if (!any) { x0 = x1 = v.x; y0 = y1 = v.y; any = true; }
				x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
				y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
			}
		}
	}
	if (!any) return map;
	map->x0 = (int)std::floor(x0) - 3;
	map->y0 = (int)std::floor(y0) - 3;
	map->w = (int)std::ceil(x1) + 3 - map->x0;
	map->h = (int)std::ceil(y1) + 3 - map->y0;
	map->time.assign((size_t)map->w * map->h, -1.0f);
	map->layer_index.assign((size_t)map->w * map->h, -1);

	// 文字ごとの中心線を、字の範囲に合わせて置く
	struct Layout {
		std::vector<std::vector<Vec>> strokes;
		std::vector<double> lengths;
		std::vector<std::vector<double>> halves;	// 画の点ごとの、線の太さの半分
		std::vector<PIXEL_RGBA> cover;
		double start = 0, length = 0;
	};
	std::vector<Layout> layouts(model.size());
	double total = 0;
	for (size_t g = 0; g < model.size(); g++) {
		Layout& layout = layouts[g];
		const GlyphBox& box = boxes[g];
		if (!box.any) continue;
		layout.cover.resize((size_t)map->w * map->h);
		std::vector<PIXEL_RGBA>& cover = layout.cover;
		Rasterizer raster(map->w, map->h);
		const auto screen = [&](Vec v) { return Vec{ v.x - map->x0, v.y - map->y0 }; };
		double area = 0;
		for (const EdContour& c : model[g].contours) {
			const std::vector<Vec> line = flatten_contour(c, 0);
			for (size_t i = 0; i < line.size(); i++) raster.line(screen(line[i]), screen(line[(i + 1) % line.size()]));
		}
		raster.write(cover.data(), 0, 0, 0);
		for (const PIXEL_RGBA& p : cover) area += p.a / 255.0;
		const bool edited = g < custom.size() && custom[g].custom;
		std::vector<std::vector<Vec>> strokes = edited ? custom[g].lines : find_strokes((UINT32)model[g].head[0]);
		if (!strokes.empty()) {
			// 編集された筆順は形状1の座標なので、形状の間を描く時は字の範囲に合わせて伸縮させる
			double ref_x0 = box.x0, ref_y0 = box.y0, ref_x1 = box.x1, ref_y1 = box.y1;
			if (edited && g < reference_model.size()) {
				bool found = false;
				for (const EdContour& c : reference_model[g].contours) {
					for (const Vec& v : flatten_contour(c, 0)) {
						if (!found) { ref_x0 = ref_x1 = v.x; ref_y0 = ref_y1 = v.y; found = true; }
						ref_x0 = std::min(ref_x0, v.x); ref_x1 = std::max(ref_x1, v.x);
						ref_y0 = std::min(ref_y0, v.y); ref_y1 = std::max(ref_y1, v.y);
					}
				}
			}
			const double ratio_x = ref_x1 - ref_x0 > 0 ? (box.x1 - box.x0) / (ref_x1 - ref_x0) : 1;
			const double ratio_y = ref_y1 - ref_y0 > 0 ? (box.y1 - box.y0) / (ref_y1 - ref_y0) : 1;
			double cx0 = 1e30, cx1 = -1e30, cy0 = 1e30, cy1 = -1e30, unit = 0;
			for (const auto& line : strokes) {
				unit += polyline_length(line);
				for (const Vec& v : line) {
					cx0 = std::min(cx0, v.x); cx1 = std::max(cx1, v.x);
					cy0 = std::min(cy0, v.y); cy1 = std::max(cy1, v.y);
				}
			}
			const double ink_w = box.x1 - box.x0, ink_h = box.y1 - box.y0;
			// 線の太さ = 面積 / 長さ。中心線の範囲は字の範囲より太さの半分だけ内側にある
			const double rough = std::max(ink_w, ink_h) / std::max({ cx1 - cx0, cy1 - cy0, 1.0 });
			const double thick = area / std::max(unit * rough, 1.0);
			const double sx = cx1 - cx0 > 1 ? std::max(ink_w - thick, 1.0) / (cx1 - cx0) : rough;
			const double sy = cy1 - cy0 > 1 ? std::max(ink_h - thick, 1.0) / (cy1 - cy0) : rough;
			const double center_x = (box.x0 + box.x1) / 2, center_y = (box.y0 + box.y1) / 2;
			const std::vector<float> field = ink_distance(cover, map->w, map->h);
			for (auto& line : strokes) {
				for (Vec& v : line) {
					if (edited) v = { box.x0 + (v.x - ref_x0) * ratio_x - map->x0, box.y0 + (v.y - ref_y0) * ratio_y - map->y0 };
					else v = { center_x + (v.x - (cx0 + cx1) / 2) * sx - map->x0, center_y + (v.y - (cy0 + cy1) / 2) * sy - map->y0 };
				}
				line = resample_line(line, 2);
				// 編集された線は、利用者が決めた位置のまま使う
				if (!edited) snap_line(line, field, map->w, map->h, std::max(thick, 3.0));
				// 中央の点の外までの距離が太さの半分になる。交差した所は距離が大きく出るので、線に沿った近傍の中央値を使う
				const int count = (int)line.size();
				const int window = std::max(3, (int)std::ceil(thick));	// 点は2ピクセル間隔なので、前後に太さの2倍の範囲を見る
				std::vector<double> half(count);
				for (int i = 0; i < count; i++) {
					std::vector<double> near_values;
					for (int k = std::max(i - window, 0); k <= std::min(i + window, count - 1); k++) {
						const int x = (int)line[k].x, y = (int)line[k].y;
						near_values.push_back(x >= 0 && x < map->w && y >= 0 && y < map->h ? field[(size_t)y * map->w + x] : 0.0);
					}
					std::nth_element(near_values.begin(), near_values.begin() + near_values.size() / 2, near_values.end());
					half[i] = std::max(1.5, near_values[near_values.size() / 2]);
				}
				layout.halves.push_back(std::move(half));
				const double length = polyline_length(line);
				layout.lengths.push_back(length);
				layout.length += length;
			}
			layout.strokes = std::move(strokes);
		} else {
			// 筆順データに無い文字は、順番が来た時にまとめて現れる
			layout.length = std::max(box.x1 - box.x0, box.y1 - box.y0);
		}
		layout.start = total;
		total += layout.length;
	}
	map->total = std::max(total, 1.0);

	for (size_t g = 0; g < model.size(); g++) {
		const Layout& layout = layouts[g];
		if (layout.cover.empty()) continue;
		const int stroke_num = (int)layout.strokes.size();
		// アイランドと画の対応: 画は、その線の点を最も多く含むアイランドのものにする。画が無いアイランドは最も近い画のものにする
		const std::vector<int> contour_island = contour_islands(model[g], 0);
		const int island_num = contour_island.empty() ? 0 : *std::max_element(contour_island.begin(), contour_island.end()) + 1;
		std::vector<std::vector<PIXEL_RGBA>> island_covers(island_num);
		for (int i = 0; i < island_num; i++) {
			island_covers[i].resize((size_t)map->w * map->h);
			Rasterizer raster(map->w, map->h);
			for (size_t c = 0; c < model[g].contours.size(); c++) {
				if (contour_island[c] != i) continue;
				const std::vector<Vec> line = flatten_contour(model[g].contours[c], 0);
				for (size_t k = 0; k < line.size(); k++) {
					const Vec a = line[k], b = line[(k + 1) % line.size()];
					raster.line({ a.x - map->x0, a.y - map->y0 }, { b.x - map->x0, b.y - map->y0 });
				}
			}
			raster.write(island_covers[i].data(), 0, 0, 0);
		}
		std::vector<std::vector<int>> island_strokes(std::max(island_num, 1));
		for (int s = 0; s < stroke_num; s++) {
			int best_island = 0;
			size_t best_count = 0;
			for (int i = 0; i < island_num; i++) {
				size_t count = 0;
				for (const Vec& v : layout.strokes[s]) {
					const int x = (int)v.x, y = (int)v.y;
					if (x >= 0 && x < map->w && y >= 0 && y < map->h && island_covers[i][(size_t)y * map->w + x].a >= 128) count++;
				}
				if (count > best_count) { best_count = count; best_island = i; }
			}
			island_strokes[best_island].push_back(s);
		}
		for (int i = 0; i < island_num && stroke_num; i++) {
			if (!island_strokes[i].empty()) continue;
			double best = 1e30;
			int nearest = 0;
			for (int y = 0; y < map->h; y += 2) {
				for (int x = 0; x < map->w; x += 2) {
					if (!island_covers[i][(size_t)y * map->w + x].a) continue;
					for (int s = 0; s < stroke_num; s++) {
						double d, along;
						size_t index;
						nearest_on_line(layout.strokes[s], { x + 0.5, y + 0.5 }, d, along, index);
						if (d < best) { best = d; nearest = s; }
					}
				}
			}
			island_strokes[i].push_back(nearest);
		}
		std::vector<double> stroke_start(stroke_num);
		for (int s = 0; s < stroke_num; s++) stroke_start[s] = s ? stroke_start[s - 1] + layout.lengths[s - 1] : layout.start;
		// 1つのアイランドに複数の画がある時だけ、画ごとの領域を求める(まだ分割していない所)
		std::vector<bool> needs_region(stroke_num, false);
		for (const auto& list : island_strokes) if (list.size() > 1) for (int s : list) needs_region[s] = true;
		// 画ごとの領域: 線の太さの中に入る画素のうち、つながった最大の塊だけにする
		// (中心線がフォントの線とずれて別の線を通っても、離れた場所の線がその画として先に現れない)
		std::vector<std::vector<char>> regions(stroke_num);
		for (int s = 0; s < stroke_num; s++) {
			if (!needs_region[s]) continue;
			std::vector<char>& region = regions[s];
			region.assign((size_t)map->w * map->h, 0);
			for (int y = 0; y < map->h; y++) {
				for (int x = 0; x < map->w; x++) {
					if (!layout.cover[(size_t)y * map->w + x].a) continue;
					double d, along;
					size_t index;
					nearest_on_line(layout.strokes[s], { x + 0.5, y + 0.5 }, d, along, index);
					if (d <= layout.halves[s][index] + 0.25) region[(size_t)y * map->w + x] = 1;
				}
			}
			size_t best_size = 0;
			char best_label = 0, label = 1;
			std::vector<size_t> stack;
			for (size_t start = 0; start < region.size(); start++) {
				if (region[start] != 1) continue;
				if (label == 127) break;
				label++;
				size_t size = 0;
				region[start] = label;
				stack.push_back(start);
				while (!stack.empty()) {
					const size_t at = stack.back();
					stack.pop_back();
					size++;
					const int px = (int)(at % map->w);
					auto visit = [&](size_t n) { if (region[n] == 1) { region[n] = label; stack.push_back(n); } };
					if (px > 0) visit(at - 1);
					if (px + 1 < map->w) visit(at + 1);
					if (at >= (size_t)map->w) visit(at - map->w);
					if (at + map->w < region.size()) visit(at + map->w);
				}
				if (size > best_size) { best_size = size; best_label = label; }
			}
			for (char& value : region) value = value == best_label && best_label ? 1 : 0;
		}
		// 画素の時刻は、属するアイランドの画だけで決める(他の画へはみ出さない)。複数のアイランドに属する所は最も早いものになる。
		// 同じアイランドに複数の画がある時は、領域に入る画のうち最も早く通るものの時刻にする。
		// どの領域にも入らなければ、太さに対して最も近い画にする(細い線のすぐ外にある太い線の端を、細い線のものにしない)
		std::vector<unsigned char> island_count;
		if (island_num >= 2) {
			island_count.assign((size_t)map->w * map->h, 0);
			for (const auto& cover : island_covers) {
				for (size_t i = 0; i < cover.size(); i++) if (cover[i].a) island_count[i]++;
			}
		}
		for (int island = 0; island < std::max(island_num, 1); island++) {
			const std::vector<int>& list = island_strokes[island];
			for (int y = 0; y < map->h; y++) {
				for (int x = 0; x < map->w; x++) {
					const size_t at_pixel = (size_t)y * map->w + x;
					if (island_num ? !island_covers[island][at_pixel].a : !layout.cover[at_pixel].a) continue;
					float& time = map->time[at_pixel];
					double value;
					if (list.empty()) {
						value = layout.start + layout.length;
					} else {
						const Vec v{ x + 0.5, y + 0.5 };
						double best = 1e30, nearest_time = 0, early = 1e30;
						for (int s : list) {
							double d, along;
							size_t index;
							nearest_on_line(layout.strokes[s], v, d, along, index);
							const double half = layout.halves[s][index];
							const double at = stroke_start[s] + along;
							if (d / half < best) { best = d / half; nearest_time = at; }
							if (list.size() > 1 && regions[s][at_pixel] && at < early) early = at;
						}
						value = early < 1e30 ? early : nearest_time;
					}
					const float normalized = (float)(value / map->total);
					if (time < 0 || normalized < time) time = normalized;
					if (!island_count.empty() && island_count[at_pixel] >= 2) {
						int& layer = map->layer_index[at_pixel];
						if (layer < 0) {
							layer = (int)map->layers.size();
							map->layers.emplace_back();
						}
						map->layers[layer].push_back({ normalized, island_covers[island][at_pixel].a / 255.0f });
					}
				}
			}
		}
	}
	// 本体の画像の縁は自前の塗りより少し広いことがあるので、隣の時刻で埋める
	for (int pass = 0; pass < 2; pass++) {
		const std::vector<float> copy = map->time;
		for (int y = 1; y + 1 < map->h; y++) {
			for (int x = 1; x + 1 < map->w; x++) {
				const size_t at = (size_t)y * map->w + x;
				if (copy[at] >= 0) continue;
				float best = -1;
				for (const size_t n : { at - 1, at + 1, at - map->w, at + map->w }) {
					if (copy[n] >= 0 && (best < 0 || copy[n] < best)) best = copy[n];
				}
				map->time[at] = best;
			}
		}
	}
	return map;
}

float pen_time_at(const PenMap& map, double x, double y) {
	const int mx = (int)std::floor(x) - map.x0, my = (int)std::floor(y) - map.y0;
	return mx >= 0 && mx < map.w && my >= 0 && my < map.h ? map.time[(size_t)my * map.w + mx] : -1;
}

std::mutex g_pen_mutex;
std::map<double, std::shared_ptr<PenMap>> g_pen_maps;	// エフェクトのIDごと

// pen(data, w, h, x, y, 全体のw, 全体のh, ox, oy, アンカー, ハンドル, 輪郭情報, 筆順, 形状1のアンカー, 形状1のハンドル, エフェクトのID, 進行0～1):
// 進行の時点でまだ書かれていない画素を消す。(x, y)はこの画像の全体の画像の中での位置
void module_pen(SCRIPT_MODULE_PARAM* param) {
	auto data = static_cast<PIXEL_RGBA*>(param->get_param_data(0));
	const int w = param->get_param_int(1), h = param->get_param_int(2);
	if (!data || w <= 0 || h <= 0) { param->set_error(u8"引数の値が正しくありません"); return; }
	const int left = param->get_param_int(3), top = param->get_param_int(4);
	const int whole_w = param->get_param_int(5), whole_h = param->get_param_int(6);
	const double ox = param->get_param_double(7), oy = param->get_param_double(8);
	const std::vector<double> meta = read_array(param, 11);
	const std::vector<double> points = merge_points(read_array(param, 9), read_array(param, 10), meta);
	const std::vector<double> stroke_values = read_array(param, 12);
	const std::vector<double> reference = merge_points(read_array(param, 13), read_array(param, 14), meta);
	const double id = param->get_param_double(15), progress = param->get_param_double(16);
	if (points.empty()) return;
	const UINT32 key = hash_points(points) ^ (hash_points(meta) * 31) ^ (hash_points(stroke_values) * 131);
	std::shared_ptr<PenMap> map;
	{
		std::lock_guard<std::mutex> lock(g_pen_mutex);
		auto& slot = g_pen_maps[id];
		if (!slot || slot->key != key) slot = build_pen_map(points, meta, key, parse_strokes(stroke_values, (size_t)meta[5]), reference);
		map = slot;
	}
	// 書き進む先端は2ピクセルかけて現す(進行0では何も出ない)
	const double edge = 2 / map->total;
	const double shift_x = left - whole_w / 2.0 - ox - map->x0, shift_y = top - whole_h / 2.0 - oy - map->y0;
	for (int y = 0; y < h; y++) {
		const int my = (int)std::floor(y + 0.5 + shift_y);
		for (int x = 0; x < w; x++) {
			PIXEL_RGBA& p = data[(size_t)y * w + x];
			if (!p.a) continue;
			const int mx = (int)std::floor(x + 0.5 + shift_x);
			float time = -1;
			if (mx >= 0 && mx < map->w && my >= 0 && my < map->h) {
				const size_t at = (size_t)my * map->w + mx;
				time = map->time[at];
				// 複数のアイランドが覆う画素は、書かれたアイランドの分だけの濃さにする
				// (足し合わせて本体の濃さで頭打ち: 切り口は半分ずつ増え、同じ辺を共有する所は濃くならない)
				if (map->layer_index[at] >= 0) {
					double written = 0;
					for (const PenMap::Layer& layer : map->layers[map->layer_index[at]]) {
						written += layer.cover * std::clamp((progress - layer.time) / edge, 0.0, 1.0);
					}
					p.a = (unsigned char)std::lround(std::min((double)p.a, written * 255));
					continue;
				}
			}
			const double factor = time < 0 ? 0 : std::clamp((progress - time) / edge, 0.0, 1.0);
			p.a = (unsigned char)std::lround(p.a * factor);
		}
	}
}

// edited(アンカー, ハンドル, 輪郭情報, 文字): 無編集の時から座標が変わったか。文字は番号(0～)で、-1なら全部
void module_edited(SCRIPT_MODULE_PARAM* param) {
	const std::vector<double> meta = read_array(param, 2);
	const std::vector<double> points = merge_points(read_array(param, 0), read_array(param, 1), meta);
	const int only = param->get_param_int(3);
	if (only < 0) {
		param->push_result_boolean(hash_points(points) != (UINT32)meta[1]);
		return;
	}
	size_t head, first, count;
	if (points.empty() || !find_glyph(meta, only, head, first, count) || (first + count) * 2 > points.size()) {
		param->push_result_boolean(false);
		return;
	}
	param->push_result_boolean(hash_glyph(points, first, count) != (UINT32)meta[head + 7]);
}

// handles(アンカー, ハンドルの設定値, 輪郭情報): 今のハンドルの位置
void module_handles(SCRIPT_MODULE_PARAM* param) {
	std::vector<double> handles = resolve_handles(read_array(param, 0), read_array(param, 1), read_array(param, 2));
	param->push_result_array_double(handles.data(), (int)handles.size());
}

// glyph_ink(輪郭情報, 文字): 無編集の時のその文字のインクの左端・上端 (無ければ-1)
void module_glyph_ink(SCRIPT_MODULE_PARAM* param) {
	const std::vector<double> meta = read_array(param, 0);
	size_t head, first, count;
	const bool found = find_glyph(meta, param->get_param_int(1), head, first, count);
	param->push_result_double(found ? meta[head + 5] : -1);
	param->push_result_double(found ? meta[head + 6] : -1);
}

// bounds(アンカー, ハンドル, 輪郭情報, ox, oy, 文字): 全ての点の、画像の中心からの距離の最大(横, 縦)
void module_bounds(SCRIPT_MODULE_PARAM* param) {
	const std::vector<double> meta = read_array(param, 2);
	const std::vector<double> points = merge_points(read_array(param, 0), read_array(param, 1), meta);
	const double ox = param->get_param_double(3), oy = param->get_param_double(4);
	double ax = 0, ay = 0;
	if (!points.empty()) {
		each_contour_meta(meta, [&](size_t kinds, int seg_num, size_t point) {
			size_t used = 1;
			for (int s = 0; s < seg_num; s++) used += seg_size(meta[kinds + s]);
			for (size_t i = point; i < point + used; i++) {
				ax = std::max(ax, std::abs(points[i * 2] + ox));
				ay = std::max(ay, std::abs(points[i * 2 + 1] + oy));
			}
		}, param->get_param_int(5));
	}
	param->push_result_double(ax);
	param->push_result_double(ay);
}

// text_hash(文字列): テキスト設定のハッシュ
void module_text_hash(SCRIPT_MODULE_PARAM* param) {
	LPCSTR s = param->get_param_string(0);
	param->push_result_double(s ? fnv1a(s, strlen(s)) : 0);
}

// ink(data, w, h): 画像のインクの左端・上端を返す (インクが無ければ-1)
void module_ink(SCRIPT_MODULE_PARAM* param) {
	auto data = static_cast<PIXEL_RGBA*>(param->get_param_data(0));
	const int w = param->get_param_int(1), h = param->get_param_int(2);
	if (!data || w <= 0 || h <= 0) { param->set_error(u8"引数の値が正しくありません"); return; }
	const Ink ink = measure_ink(&data->a, w, h, w * 4, 4);
	param->push_result_double(ink.left);
	param->push_result_double(ink.top);
}

// lines(アンカー, ハンドル, 輪郭情報, ox, oy): 表示用の線の座標を返す
//   1つ目: 輪郭ごとに「点の数, x, y, x, y, …」(曲線は細かく折れ線にした実際の形)
//   2つ目: ハンドルの線ごとに「x0, y0, x1, y1」(アンカーからハンドルまで)
void module_lines(SCRIPT_MODULE_PARAM* param) {
	const std::vector<double> meta = read_array(param, 2);
	const std::vector<double> points = merge_points(read_array(param, 0), read_array(param, 1), meta);
	const double ox = param->get_param_double(3), oy = param->get_param_double(4);
	std::vector<double> outline, arms;
	if (!points.empty()) {
		auto read = [&](size_t i) { return Vec{ points[i * 2] + ox, points[i * 2 + 1] + oy }; };
		each_contour_meta(meta, [&](size_t kinds, int seg_num, size_t point) {
			const size_t head = outline.size();
			outline.push_back(0);
			auto add = [&](Vec v) { outline.push_back(v.x); outline.push_back(v.y); };
			add(read(point));
			walk_contour(meta, kinds, seg_num, point, read,
				[&](Vec, Vec b) { add(b); },
				[&](Vec a, Vec c1, Vec c2, Vec b) {
					for (int k = 1; k <= 8; k++) {
						const double t = k / 8.0, u = 1 - t;
						add({ u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x,
							u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y });
					}
					arms.insert(arms.end(), { a.x, a.y, c1.x, c1.y, b.x, b.y, c2.x, c2.y });
				});
			outline[head] = (double)(outline.size() - head - 1) / 2;
		});
	}
	param->push_result_array_double(outline.data(), (int)outline.size());
	param->push_result_array_double(arms.data(), (int)arms.size());
}

SCRIPT_MODULE_FUNCTION g_functions[] = {
	{ L"fill", module_fill },
	{ L"capture", module_capture },
	{ L"compose", module_compose },
	{ L"parts", module_parts },
	{ L"part_rect", module_part_rect },
	{ L"compose_part", module_compose_part },
	{ L"pen", module_pen },
	{ L"edited", module_edited },
	{ L"text_hash", module_text_hash },
	{ L"lines", module_lines },
	{ L"glyph_ink", module_glyph_ink },
	{ L"handles", module_handles },
	{ L"bounds", module_bounds },
	{ L"ink", module_ink },
	{ nullptr, nullptr },
};
SCRIPT_MODULE_TABLE g_module_table = { L"テキストの輪郭パス化", g_functions };

}	// namespace

EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* handle) {
	g_config = handle;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
	host->set_plugin_information(L"テキストの輪郭パス化 v1.0 By teruyoshi");
	g_edit_handle = host->create_edit_handle();
	host->register_filter_plugin(&g_filter_table);
	host->register_object_menu(L"輪郭パス\\輪郭を作成・更新", on_object_menu<true>);
	host->register_object_menu(L"輪郭パス\\全てのアンカーをリセット", on_object_menu<false>);
	host->register_object_menu(L"輪郭パス\\高度な編集", on_object_menu_editor);
	host->register_object_menu(L"輪郭パス\\アイランドごとにオブジェクトへ展開", on_object_menu_expand);
	host->register_config_menu(L"輪郭パスの設定", on_config);
	host->register_script_module_name(&g_module_table, L"TextPath_Te");
}
