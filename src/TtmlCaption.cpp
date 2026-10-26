#include "TtmlCaption.hpp"
#include "ttml/parser.h"
#include "ttml/resolver.h"
#include "pugixml.hpp"
#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <set>
#include <variant>

namespace TtmlCaption
{

namespace
{

const int TABLE_ID_CAPTION = 0xC1;
const size_t MAX_PENDING_SECTIONS = 16;
const size_t MAX_DOCUMENTS = 64;

uint32_t Crc32Mpeg2(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
        }
    }
    return crc;
}

// UTF-8をコードポイントの列にする。不正なバイトはU+FFFDにする
std::vector<uint32_t> DecodeUtf8(const std::string &text)
{
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size(); ) {
        uint8_t c = static_cast<uint8_t>(text[i]);
        int length = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        uint32_t cp = length == 1 ? c : length == 2 ? (c & 0x1F) : length == 3 ? (c & 0x0F) : (c & 0x07);
        bool valid = length > 0 && i + length <= text.size();
        for (int j = 1; valid && j < length; ++j) {
            uint8_t d = static_cast<uint8_t>(text[i + j]);
            valid = (d & 0xC0) == 0x80;
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!valid) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += length;
    }
    return out;
}

std::string EncodeUtf8(uint32_t cp)
{
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

// 縦書きで字形の変わる約物を縦書き用の字形にする(dantto4kのB24変換、ts-liveと同じ表)
uint32_t VerticalForm(uint32_t cp)
{
    static const std::pair<uint32_t, uint32_t> forms[] = {
        {0xff0c, 0xfe10}, {0x3001, 0xfe11}, {0x3002, 0xfe12}, {0xff1a, 0xfe13}, {0xff1b, 0xfe14},
        {0x3016, 0xfe17}, {0x3017, 0xfe18}, {0x2026, 0xfe19}, {0x2025, 0xfe30}, {0x30fc, 0xfe31},
        {0x2014, 0xfe31}, {0x2015, 0xfe31}, {0x2013, 0xfe32}, {0xff3f, 0xfe33}, {0xff08, 0xfe35},
        {0xff09, 0xfe36}, {0xff5b, 0xfe37}, {0xff5d, 0xfe38}, {0x3014, 0xfe39}, {0x3015, 0xfe3a},
        {0x3010, 0xfe3b}, {0x3011, 0xfe3c}, {0x300a, 0xfe3d}, {0x300b, 0xfe3e}, {0x3008, 0xfe3f},
        {0x3009, 0xfe40}, {0x300c, 0xfe41}, {0x300d, 0xfe42}, {0x300e, 0xfe43}, {0x300f, 0xfe44},
        {0xff3b, 0xfe47}, {0xff3d, 0xfe48}, {0xff5e, 0x2240}, {0x301c, 0x2240}, {0xff0d, 0xff5c},
        {0xff1d, 0x2016}, {0x2010, 0xfe32}, {0x2016, 0xff1d}, {0xff5c, 0x2015}, {0x25b6, 0x25bc},
        {0x25c0, 0x25b2},
    };
    for (const auto &form : forms) {
        if (form.first == cp) {
            return form.second;
        }
    }
    return cp;
}

// 縦書きではセルの右上に寄せる小書きの仮名(ARIB TR-B39 表8-4)
bool IsSmallKana(uint32_t cp)
{
    static const uint32_t kana[] = {
        0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087,
        0x308e, 0x3095, 0x3096, 0x30a1, 0x30a3, 0x30a5, 0x30a7, 0x30a9, 0x30c3,
        0x30e3, 0x30e5, 0x30e7, 0x30ee, 0x30f5, 0x30f6,
    };
    return std::find(std::begin(kana), std::end(kana), cp) != std::end(kana) || (cp >= 0x31f0 && cp <= 0x31ff);
}

// SVGフォントのunicode / unicode-range属性値からコードポイントを1つ取り出す
uint32_t ParseCodepointAttribute(const char *value)
{
    if (!value || !value[0]) {
        return 0;
    }
    std::string text = value;
    if ((text[0] == 'U' || text[0] == 'u') && text.size() > 2 && text[1] == '+') {
        std::string hex;
        for (size_t i = 2; i < text.size() && (isxdigit(static_cast<unsigned char>(text[i])) || text[i] == '?'); ++i) {
            hex += text[i] == '?' ? '0' : text[i];
        }
        uint32_t cp = 0;
        if (hex.empty() || std::from_chars(hex.data(), hex.data() + hex.size(), cp, 16).ec != std::errc()) {
            return 0;
        }
        return cp <= 0x10FFFF ? cp : 0;
    }
    if (text[0] == '&') {
        // 二重にエスケープされて残った文字参照
        return 0;
    }
    std::vector<uint32_t> cps = DecodeUtf8(text);
    return cps.empty() ? 0 : cps[0];
}

struct Point
{
    double x;
    double y;
};

// SVGのpath (d属性) を折れ線の集まりにする。円弧(A)は扱わない
bool FlattenPath(const std::string &d, std::vector<std::vector<Point>> &contours)
{
    const char *p = d.c_str();
    const char *end = p + d.size();
    auto skipSeparators = [&]() {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n')) {
            ++p;
        }
    };
    auto readNumber = [&](double &value) {
        skipSeparators();
        if (p < end && *p == '+') {
            ++p;
        }
        auto result = std::from_chars(p, end, value);
        if (result.ec != std::errc()) {
            return false;
        }
        p = result.ptr;
        return true;
    };
    contours.clear();
    Point current = {0, 0};
    Point start = {0, 0};
    Point lastControl = {0, 0};
    char lastCommand = 0;
    char command = 0;
    auto lineTo = [&](Point point) {
        if (contours.empty()) {
            contours.push_back({current});
        }
        contours.back().push_back(point);
        current = point;
    };
    auto cubicTo = [&](Point c1, Point c2, Point point) {
        const int steps = 16;
        Point from = current;
        for (int i = 1; i <= steps; ++i) {
            double t = static_cast<double>(i) / steps;
            double u = 1 - t;
            lineTo({u * u * u * from.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * point.x,
                    u * u * u * from.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * point.y});
        }
    };
    for (;;) {
        skipSeparators();
        if (p >= end) {
            break;
        }
        if (isalpha(static_cast<unsigned char>(*p))) {
            command = *p++;
        }
        else if (!command) {
            return false;
        }
        bool relative = islower(static_cast<unsigned char>(command)) != 0;
        Point base = relative ? current : Point{0, 0};
        switch (toupper(static_cast<unsigned char>(command))) {
        case 'M': {
            Point point;
            if (!readNumber(point.x) || !readNumber(point.y)) {
                return false;
            }
            point = {base.x + point.x, base.y + point.y};
            contours.push_back({point});
            current = start = point;
            // 続く座標は直線
            command = relative ? 'l' : 'L';
            lastCommand = 'M';
            continue;
        }
        case 'L': {
            Point point;
            if (!readNumber(point.x) || !readNumber(point.y)) {
                return false;
            }
            lineTo({base.x + point.x, base.y + point.y});
            break;
        }
        case 'H': {
            double x;
            if (!readNumber(x)) {
                return false;
            }
            lineTo({base.x + x, current.y});
            break;
        }
        case 'V': {
            double y;
            if (!readNumber(y)) {
                return false;
            }
            lineTo({current.x, base.y + y});
            break;
        }
        case 'C': {
            Point c1, c2, point;
            if (!readNumber(c1.x) || !readNumber(c1.y) || !readNumber(c2.x) || !readNumber(c2.y) ||
                !readNumber(point.x) || !readNumber(point.y)) {
                return false;
            }
            c2 = {base.x + c2.x, base.y + c2.y};
            cubicTo({base.x + c1.x, base.y + c1.y}, c2, {base.x + point.x, base.y + point.y});
            lastControl = c2;
            lastCommand = 'C';
            continue;
        }
        case 'S': {
            Point c2, point;
            if (!readNumber(c2.x) || !readNumber(c2.y) || !readNumber(point.x) || !readNumber(point.y)) {
                return false;
            }
            Point c1 = lastCommand == 'C' ? Point{2 * current.x - lastControl.x, 2 * current.y - lastControl.y} : current;
            c2 = {base.x + c2.x, base.y + c2.y};
            cubicTo(c1, c2, {base.x + point.x, base.y + point.y});
            lastControl = c2;
            lastCommand = 'C';
            continue;
        }
        case 'Q': {
            Point c, point;
            if (!readNumber(c.x) || !readNumber(c.y) || !readNumber(point.x) || !readNumber(point.y)) {
                return false;
            }
            c = {base.x + c.x, base.y + c.y};
            point = {base.x + point.x, base.y + point.y};
            Point from = current;
            cubicTo({from.x + 2.0 / 3 * (c.x - from.x), from.y + 2.0 / 3 * (c.y - from.y)},
                    {point.x + 2.0 / 3 * (c.x - point.x), point.y + 2.0 / 3 * (c.y - point.y)}, point);
            lastControl = c;
            lastCommand = 'Q';
            continue;
        }
        case 'T': {
            Point point;
            if (!readNumber(point.x) || !readNumber(point.y)) {
                return false;
            }
            Point c = lastCommand == 'Q' ? Point{2 * current.x - lastControl.x, 2 * current.y - lastControl.y} : current;
            point = {base.x + point.x, base.y + point.y};
            Point from = current;
            cubicTo({from.x + 2.0 / 3 * (c.x - from.x), from.y + 2.0 / 3 * (c.y - from.y)},
                    {point.x + 2.0 / 3 * (c.x - point.x), point.y + 2.0 / 3 * (c.y - point.y)}, point);
            lastControl = c;
            lastCommand = 'Q';
            continue;
        }
        case 'Z':
            if (!contours.empty()) {
                contours.back().push_back(start);
            }
            current = start;
            lastCommand = 'Z';
            command = 0;
            continue;
        default:
            // 円弧など
            return false;
        }
        lastCommand = static_cast<char>(toupper(static_cast<unsigned char>(command)));
    }
    return !contours.empty();
}

aribcaption::ColorRGBA ToColor(const std::optional<arib::ttml::StyleColorValue> &color, aribcaption::ColorRGBA fallback)
{
    return color ? aribcaption::ColorRGBA(color->r(), color->g(), color->b(), color->a()) : fallback;
}

const double DEFAULT_FONT_SIZE = 144;
// 変換先の字幕プレーン。B24と同じ大きさにして、縁取りの幅などの設定を同じに効かせる
const int PLANE_WIDTH = 960;
const int PLANE_HEIGHT = 540;
// 外字を描く大きさ(ドット)
const int DRCS_SIZE = 96;

struct Run
{
    std::vector<uint32_t> text;
    const arib::ttml::StyleProperties *style;
};

// 1つの段落を字幕の領域にする
class RegionBuilder
{
public:
    RegionBuilder(double scale, const std::unordered_map<uint32_t, aribcaption::DRCS> &drcs, aribcaption::Caption &caption)
        : m_scale(scale), m_drcs(drcs), m_caption(caption) {}

    // (x, y)からw×hのセル(字間などを含む)。字形は(x + hs/2, y + vs/2)からcw×ch
    void AddChar(uint32_t cp, const arib::ttml::StyleProperties &style,
                 double x, double y, double cw, double ch, double hs, double vs)
    {
        aribcaption::CaptionChar c;
        c.x = static_cast<int>(std::lround(x * m_scale));
        c.y = static_cast<int>(std::lround(y * m_scale));
        c.char_width = std::max(1, static_cast<int>(std::lround(cw * m_scale)));
        c.char_height = std::max(1, static_cast<int>(std::lround(ch * m_scale)));
        c.char_horizontal_spacing = std::max(0, static_cast<int>(std::lround(hs * m_scale)));
        c.char_vertical_spacing = std::max(0, static_cast<int>(std::lround(vs * m_scale)));
        c.char_horizontal_scale = 1.0f;
        c.char_vertical_scale = 1.0f;
        c.text_color = ToColor(style.color, aribcaption::ColorRGBA(255, 255, 255, 255));
        c.back_color = ToColor(style.background_color, aribcaption::ColorRGBA(0, 0, 0, 0));
        c.stroke_color = c.text_color;
        int charStyle = aribcaption::CharStyle::kCharStyleDefault;
        if (style.font_weight == arib::ttml::StyleFontWeightValue::Bold) {
            charStyle |= aribcaption::CharStyle::kCharStyleBold;
        }
        if (style.font_style == arib::ttml::StyleFontStyleValue::Italic) {
            charStyle |= aribcaption::CharStyle::kCharStyleItalic;
        }
        if (style.text_decoration == arib::ttml::StyleTextDecorationValue::Underline) {
            charStyle |= aribcaption::CharStyle::kCharStyleUnderline;
        }
        if (style.text_outline) {
            if (const auto *outline = std::get_if<arib::ttml::TextOutline>(&*style.text_outline)) {
                if (outline->border_width > 0) {
                    charStyle |= aribcaption::CharStyle::kCharStyleStroke;
                    c.stroke_color = ToColor(outline->color, c.text_color);
                }
            }
        }
        c.style = static_cast<aribcaption::CharStyle>(charStyle);
        int enclosure = aribcaption::EnclosureStyle::kEnclosureStyleNone;
        if (style.border_top.value_or(false)) {
            enclosure |= aribcaption::EnclosureStyle::kEnclosureStyleTop;
        }
        if (style.border_right.value_or(false)) {
            enclosure |= aribcaption::EnclosureStyle::kEnclosureStyleRight;
        }
        if (style.border_bottom.value_or(false)) {
            enclosure |= aribcaption::EnclosureStyle::kEnclosureStyleBottom;
        }
        if (style.border_left.value_or(false)) {
            enclosure |= aribcaption::EnclosureStyle::kEnclosureStyleLeft;
        }
        c.enclosure_style = static_cast<aribcaption::EnclosureStyle>(enclosure);

        auto drcs = m_drcs.find(cp);
        if (drcs != m_drcs.end()) {
            c.type = aribcaption::CaptionCharType::kDRCS;
            c.drcs_code = cp;
            m_caption.drcs_map[cp] = drcs->second;
        }
        else {
            c.type = aribcaption::CaptionCharType::kText;
            c.codepoint = cp;
            std::string u8 = EncodeUtf8(cp);
            std::copy(u8.begin(), u8.end(), c.u8str);
            c.u8str[u8.size()] = 0;
            m_text += u8;
        }
        m_region.chars.push_back(c);
    }

    void NewLine()
    {
        if (!m_text.empty() && m_text.back() != '\n') {
            m_text += '\n';
        }
    }

    // 文字を囲む矩形を領域にして字幕に加える
    void Finish(bool ruby)
    {
        if (m_region.chars.empty()) {
            return;
        }
        int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
        for (const auto &c : m_region.chars) {
            left = std::min(left, c.x);
            top = std::min(top, c.y);
            right = std::max(right, c.x + c.section_width());
            bottom = std::max(bottom, c.y + c.section_height());
        }
        m_region.x = left;
        m_region.y = top;
        m_region.width = std::max(1, right - left);
        m_region.height = std::max(1, bottom - top);
        m_region.is_ruby = ruby;
        while (!m_text.empty() && m_text.back() == '\n') {
            m_text.pop_back();
        }
        if (!ruby && !m_text.empty()) {
            if (!m_caption.text.empty()) {
                m_caption.text += '\n';
            }
            m_caption.text += m_text;
        }
        m_caption.regions.push_back(std::move(m_region));
    }

private:
    double m_scale;
    const std::unordered_map<uint32_t, aribcaption::DRCS> &m_drcs;
    aribcaption::Caption &m_caption;
    aribcaption::CaptionRegion m_region;
    std::string m_text;
};

std::pair<double, double> FontSizeOf(const arib::ttml::StyleProperties &style)
{
    double w = style.font_size ? style.font_size->x : DEFAULT_FONT_SIZE;
    double h = style.font_size ? style.font_size->y : DEFAULT_FONT_SIZE;
    return { w > 0 ? w : h, h > 0 ? h : DEFAULT_FONT_SIZE };
}

std::optional<double> LineHeightOf(const arib::ttml::StyleProperties &style)
{
    if (style.line_height) {
        if (const auto *px = std::get_if<double>(&*style.line_height)) {
            return *px;
        }
    }
    return std::nullopt;
}

bool IsActive(const arib::ttml::resolved::Timing &timing, int64_t at)
{
    return (!timing.begin || timing.begin->count() <= at) && (!timing.end || at < timing.end->count());
}

// 表示中のspanを<br/>で行に分ける
std::vector<std::vector<Run>> SplitLines(const std::vector<const arib::ttml::resolved::Span *> &spans)
{
    std::vector<std::vector<Run>> lines(1);
    for (const auto *span : spans) {
        for (const auto &item : span->content) {
            if (const auto *text = std::get_if<std::string>(&item)) {
                std::vector<uint32_t> cps = DecodeUtf8(*text);
                if (!cps.empty()) {
                    lines.back().push_back({std::move(cps), &span->style});
                }
            }
            else {
                lines.emplace_back();
            }
        }
    }
    return lines;
}

void LayoutHorizontal(RegionBuilder &builder, const std::vector<std::vector<Run>> &lines, double ox, double oy)
{
    double lineTop = oy;
    for (const auto &line : lines) {
        // 行の高さは行内で最大のlineHeight (normalは文字の高さの1.2倍)
        double lineHeight = line.empty() ? DEFAULT_FONT_SIZE * 1.2 : 0;
        for (const auto &run : line) {
            lineHeight = std::max(lineHeight, LineHeightOf(*run.style).value_or(FontSizeOf(*run.style).second * 1.2));
        }
        double x = ox;
        for (const auto &run : line) {
            auto [w, h] = FontSizeOf(*run.style);
            double ls = std::max(0.0, run.style->letter_spacing.value_or(0));
            for (uint32_t cp : run.text) {
                // 文字は行の高さの中で上下中央に置く
                builder.AddChar(cp, *run.style, x, lineTop, w, h, ls, lineHeight - h);
                x += w + ls;
            }
        }
        builder.NewLine();
        lineTop += lineHeight;
    }
}

// 縦書き: 行は領域の右端から左へ並ぶ列、文字は列を上から下へ。半角の2文字だけの並び
// (2桁の数字など)は1つのセルに横に並べ(縦中横)、小書きの仮名はセルの右上へずらす
void LayoutVertical(RegionBuilder &builder, const std::vector<std::vector<Run>> &lines, double right, double top)
{
    double columnRight = right;
    for (const auto &line : lines) {
        double columnWidth = line.empty() ? DEFAULT_FONT_SIZE * 1.2 : 0;
        for (const auto &run : line) {
            columnWidth = std::max(columnWidth, LineHeightOf(*run.style).value_or(FontSizeOf(*run.style).first * 1.2));
        }
        double columnLeft = columnRight - columnWidth;
        double y = top;
        for (const auto &run : line) {
            auto [w, h] = FontSizeOf(*run.style);
            double ls = std::max(0.0, run.style->letter_spacing.value_or(0));
            if (w < h && run.text.size() == 2) {
                double x = columnLeft + (columnWidth - 2 * w) / 2;
                builder.AddChar(run.text[0], *run.style, x, y, w, h, 0, ls);
                builder.AddChar(run.text[1], *run.style, x + w, y, w, h, 0, ls);
                y += h + ls;
                continue;
            }
            for (uint32_t cp : run.text) {
                if (IsSmallKana(cp)) {
                    builder.AddChar(cp, *run.style, columnLeft + w / 8, y - h / 8, w, h, columnWidth - w, ls);
                }
                else {
                    builder.AddChar(VerticalForm(cp), *run.style, columnLeft, y, w, h, columnWidth - w, ls);
                }
                y += h + ls;
            }
        }
        builder.NewLine();
        columnRight = columnLeft;
    }
}

} // namespace

void SectionReceiver::Reset()
{
    m_buffer.clear();
    m_continuityCounter = -1;
    m_synced = false;
    m_pending.clear();
    m_documents.clear();
}

void SectionReceiver::FeedPacket(const uint8_t *packet)
{
    int adaptationFieldControl = (packet[3] >> 4) & 0x03;
    if (!(adaptationFieldControl & 1)) {
        return;
    }
    int continuityCounter = packet[3] & 0x0F;
    if (m_continuityCounter >= 0 && continuityCounter == m_continuityCounter) {
        // 重複パケット
        return;
    }
    bool continuous = m_continuityCounter < 0 || continuityCounter == ((m_continuityCounter + 1) & 0x0F);
    m_continuityCounter = continuityCounter;
    if (!continuous) {
        m_buffer.clear();
        m_synced = false;
    }

    size_t offset = 4;
    if (adaptationFieldControl & 2) {
        offset += 1 + packet[4];
    }
    if (offset >= 188) {
        return;
    }
    const uint8_t *payload = packet + offset;
    size_t payloadSize = 188 - offset;

    // 溜まったデータから完成したセクションを取り出す
    auto flush = [this](bool keepTail) {
        size_t pos = 0;
        while (m_buffer.size() - pos >= 3 && m_buffer[pos] != 0xFF) {
            size_t length = 3 + (((m_buffer[pos + 1] & 0x0F) << 8) | m_buffer[pos + 2]);
            if (m_buffer.size() - pos < length) {
                break;
            }
            OnSection(m_buffer.data() + pos, length);
            pos += length;
        }
        if (keepTail && pos < m_buffer.size() && m_buffer[pos] != 0xFF) {
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + pos);
        }
        else {
            m_buffer.clear();
        }
    };

    if (packet[1] & 0x40) {
        size_t pointer = payload[0];
        if (1 + pointer > payloadSize) {
            m_buffer.clear();
            m_synced = false;
            return;
        }
        if (m_synced) {
            m_buffer.insert(m_buffer.end(), payload + 1, payload + 1 + pointer);
            flush(false);
        }
        m_buffer.assign(payload + 1 + pointer, payload + payloadSize);
        m_synced = true;
        flush(true);
        return;
    }
    if (!m_synced) {
        return;
    }
    m_buffer.insert(m_buffer.end(), payload, payload + payloadSize);
    if (m_buffer.size() > 4096 + 188) {
        m_buffer.clear();
        m_synced = false;
        return;
    }
    flush(true);
}

void SectionReceiver::OnSection(const uint8_t *section, size_t size)
{
    // section_syntax_indicatorが1で、CRC_32が合うものだけ
    if (size < 12 || section[0] != TABLE_ID_CAPTION || !(section[1] & 0x80) || Crc32Mpeg2(section, size) != 0) {
        return;
    }
    int sequence = (section[3] << 8) | section[4];
    int number = section[6];
    int last = section[7];
    if (number > last) {
        return;
    }
    auto it = m_pending.find(sequence);
    if (it == m_pending.end() || it->second.lastSectionNumber != last) {
        if (m_pending.size() >= MAX_PENDING_SECTIONS) {
            m_pending.erase(m_pending.begin());
        }
        it = m_pending.insert_or_assign(sequence, PendingSections{last, {}}).first;
    }
    it->second.parts[number].assign(section + 8, section + size - 4);
    if (it->second.parts.size() != static_cast<size_t>(last) + 1) {
        return;
    }
    std::vector<uint8_t> body;
    for (const auto &part : it->second.parts) {
        body.insert(body.end(), part.second.begin(), part.second.end());
    }
    m_pending.erase(it);
    OnCaption(body);
}

void SectionReceiver::OnCaption(const std::vector<uint8_t> &body)
{
    // component_tag(16)、data_type(8)、tmd(8)、pts_flag(8)、pts(40)、data_size(32)、
    // 長さの付いた拡張部分(言語、resolution、reference_start_timeなど)、データ
    if (body.size() < 15) {
        return;
    }
    uint32_t dataSize = (body[10] << 24) | (body[11] << 16) | (body[12] << 8) | body[13];
    size_t extensionSize = body[14];
    size_t headerSize = 15 + extensionSize;
    if (body.size() < headerSize || dataSize != body.size() - headerSize) {
        return;
    }
    Document document;
    document.componentTag = (body[0] << 8) | body[1];
    document.dataType = body[2];
    document.tmd = body[3];
    if (body[4] & 0x80) {
        document.pts = ((static_cast<int64_t>(body[5]) << 32) | (static_cast<int64_t>(body[6]) << 24) |
                        (body[7] << 16) | (body[8] << 8) | body[9]) & 0x1FFFFFFFF;
    }
    if (extensionSize >= 4) {
        document.resolution = body[15 + 3];
    }
    document.data.assign(body.begin() + headerSize, body.end());
    if (m_documents.size() >= MAX_DOCUMENTS) {
        m_documents.erase(m_documents.begin());
    }
    m_documents.push_back(std::move(document));
}

std::vector<Document> SectionReceiver::TakeDocuments()
{
    std::vector<Document> documents;
    documents.swap(m_documents);
    return documents;
}

bool RasterizeGlyph(const std::string &path, double unitsPerEm, double ascent, int size, aribcaption::DRCS &drcs)
{
    std::vector<std::vector<Point>> contours;
    if (size <= 0 || unitsPerEm <= 0 || !FlattenPath(path, contours)) {
        return false;
    }
    struct Edge
    {
        double x0, y0, x1, y1;
        int direction;
    };
    std::vector<Edge> edges;
    for (const auto &contour : contours) {
        for (size_t i = 0; i + 1 < contour.size(); ++i) {
            Point a = contour[i];
            Point b = contour[i + 1];
            if (a.y != b.y) {
                edges.push_back(a.y < b.y ? Edge{a.x, a.y, b.x, b.y, 1} : Edge{b.x, b.y, a.x, a.y, -1});
            }
        }
        // 閉じていない輪郭も閉じたものとして塗る
        if (contour.size() > 2) {
            Point a = contour.back();
            Point b = contour.front();
            if (a.y != b.y) {
                edges.push_back(a.y < b.y ? Edge{a.x, a.y, b.x, b.y, 1} : Edge{b.x, b.y, a.x, a.y, -1});
            }
        }
    }
    if (edges.empty()) {
        return false;
    }

    // 1ドットを4x4に分けて、塗られた数を4段階の濃さにする(非ゼロ規則)
    const int sub = 4;
    std::vector<int> coverage(static_cast<size_t>(size) * size, 0);
    double unit = unitsPerEm / size;
    std::vector<std::pair<double, int>> crossings;
    for (int row = 0; row < size * sub; ++row) {
        // グリフの座標はy軸が上向きで、上端がascent
        double gy = ascent - (row + 0.5) / sub * unit;
        crossings.clear();
        for (const auto &edge : edges) {
            if (edge.y0 <= gy && gy < edge.y1) {
                crossings.emplace_back(edge.x0 + (gy - edge.y0) * (edge.x1 - edge.x0) / (edge.y1 - edge.y0), edge.direction);
            }
        }
        if (crossings.empty()) {
            continue;
        }
        std::sort(crossings.begin(), crossings.end());
        size_t next = 0;
        int winding = 0;
        for (int column = 0; column < size * sub; ++column) {
            double gx = (column + 0.5) / sub * unit;
            while (next < crossings.size() && crossings[next].first <= gx) {
                winding += crossings[next++].second;
            }
            if (winding != 0) {
                ++coverage[static_cast<size_t>(row / sub) * size + column / sub];
            }
        }
    }

    drcs = aribcaption::DRCS();
    drcs.width = size;
    drcs.height = size;
    drcs.depth = 4;
    drcs.depth_bits = 2;
    drcs.pixels.assign((static_cast<size_t>(size) * size * 2 + 7) / 8, 0);
    bool any = false;
    for (size_t i = 0; i < coverage.size(); ++i) {
        int value = (coverage[i] * 3 + sub * sub / 2) / (sub * sub);
        if (value > 0) {
            any = true;
            size_t bit = i * 2;
            drcs.pixels[bit / 8] |= static_cast<uint8_t>(value << (6 - bit % 8));
        }
    }
    return any;
}

bool Converter::AddFont(int componentTag, const std::string &svg, FontStats *stats)
{
    if (componentTag < 0x30 || componentTag > 0x3F) {
        return false;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(svg.data(), svg.size())) {
        return false;
    }
    bool added = false;
    // <font>ごとにfont-faceが付く。<font>が無ければ文書全体を1つのフォントとして扱う
    std::vector<pugi::xml_node> fonts;
    for (pugi::xpath_node node : doc.select_nodes("//*[local-name()='font']")) {
        fonts.push_back(node.node());
    }
    if (fonts.empty()) {
        fonts.push_back(doc.document_element());
    }
    for (const auto &font : fonts) {
        pugi::xml_node face = font.select_node(".//*[local-name()='font-face']").node();
        double unitsPerEm = face.attribute("units-per-em").as_double(1000);
        if (unitsPerEm <= 0) {
            unitsPerEm = 1000;
        }
        double ascent = face.attribute("ascent").as_double(unitsPerEm * 0.88);
        // 放送のリソースはコードポイントをfont-faceのunicode-rangeにしか書かないことがある
        uint32_t faceCodepoint = ParseCodepointAttribute(face.attribute("unicode-range").value());
        for (pugi::xpath_node node : font.select_nodes(".//*[local-name()='glyph']")) {
            pugi::xml_node glyph = node.node();
            const char *d = glyph.attribute("d").value();
            uint32_t cp = ParseCodepointAttribute(glyph.attribute("unicode").value());
            if (!cp) {
                cp = ParseCodepointAttribute(glyph.attribute("unicode-range").value());
            }
            if (!cp) {
                cp = faceCodepoint;
            }
            if (!cp) {
                // どの文字のものか分からないグリフは数えない
                continue;
            }
            aribcaption::DRCS drcs;
            if (!d[0] || !RasterizeGlyph(d, unitsPerEm, ascent, DRCS_SIZE, drcs)) {
                // 描けなかった字。この字を含む文書は外字未着のまま捨てられる
                if (stats) {
                    if (!stats->skipped) {
                        stats->firstSkipped = cp;
                    }
                    ++stats->skipped;
                }
                continue;
            }
            auto &glyphs = m_drcs[componentTag];
            added = added || glyphs.find(cp) == glyphs.end();
            glyphs[cp] = std::move(drcs);
        }
    }
    return added;
}

void Converter::Reset()
{
    m_drcs.clear();
}

bool Converter::Convert(const Document &document, bool superimpose, std::vector<Segment> &segments,
                        bool *missingGlyphs) const
{
    namespace ttml = arib::ttml;
    segments.clear();
    if (missingGlyphs) {
        *missingGlyphs = false;
    }
    // 字幕は同期型、文字スーパーは非同期型(dantto4kのB24変換と同じ)。TMDが1111なら
    // 時刻制御なしで、begin/endを読まない(ARIB TR-B39 8.5.9.2)
    const auto mode = superimpose ? ttml::SyncMode::Async : ttml::SyncMode::Sync;
    ttml::ParseResult parsed;
    ttml::ResolveResult resolved;
    try {
        parsed = ttml::parse(document.data, mode, document.tmd == 0x0F);
        if (parsed.has_error() || !parsed.document) {
            return false;
        }
        resolved = ttml::resolve(*parsed.document, mode);
        if (resolved.has_error() || !resolved.document) {
            return false;
        }
    }
    catch (...) {
        return false;
    }

    const int planeWidth = document.resolution == 0 ? 1920 : document.resolution == 2 ? 7680 : 3840;
    const double scale = static_cast<double>(PLANE_WIDTH) / planeWidth;
    const auto &division = resolved.document->division;
    // 別言語や文字スーパーが同じコードポイントに別の字形を定義することがある
    static const std::unordered_map<uint32_t, aribcaption::DRCS> emptyGlyphs;
    auto glyphs = m_drcs.find(document.componentTag);
    const auto &drcs = glyphs != m_drcs.end() ? glyphs->second : emptyGlyphs;
    if (division) {
        // 私用領域の文字はSVGリソースを待つ。欠けた外字を通常のフォントで確定させない
        for (const auto &paragraph : division->paragraphs) {
            for (const auto &span : paragraph.spans) {
                for (const auto &item : span.content) {
                    const auto *text = std::get_if<std::string>(&item);
                    if (!text) continue;
                    for (uint32_t cp : DecodeUtf8(*text)) {
                        bool privateUse = (cp >= 0xE000 && cp <= 0xF8FF) ||
                                          (cp >= 0xF0000 && cp <= 0xFFFFD) ||
                                          (cp >= 0x100000 && cp <= 0x10FFFD);
                        if (privateUse && drcs.find(cp) == drcs.end()) {
                            if (missingGlyphs) *missingGlyphs = true;
                            return false;
                        }
                    }
                }
            }
        }
    }

    // 表示が切り替わる時刻
    std::set<int64_t> times;
    double largestFont = 0;
    if (division) {
        for (const auto &paragraph : division->paragraphs) {
            for (const auto &span : paragraph.spans) {
                if (span.timing.begin) {
                    times.insert(span.timing.begin->count());
                }
                if (span.timing.end) {
                    times.insert(span.timing.end->count());
                }
                largestFont = std::max(largestFont, FontSizeOf(span.style).second);
            }
        }
    }
    if (times.empty()) {
        times.insert(0);
    }

    for (int64_t at : times) {
        Segment segment;
        segment.timeMsec = at;
        aribcaption::Caption &caption = segment.caption;
        caption.type = superimpose ? aribcaption::CaptionType::kSuperimpose : aribcaption::CaptionType::kCaption;
        caption.flags = aribcaption::CaptionFlags::kCaptionFlagsClearScreen;
        caption.iso6392_language_code = 0x006A706E; // jpn
        caption.plane_width = PLANE_WIDTH;
        caption.plane_height = PLANE_HEIGHT;
        caption.wait_duration = aribcaption::DURATION_INDEFINITE;
        if (division) {
            for (const auto &paragraph : division->paragraphs) {
                if (!paragraph.region || !paragraph.region->style.origin) {
                    continue;
                }
                const auto &regionStyle = paragraph.region->style;
                std::vector<const ttml::resolved::Span *> spans;
                double paragraphFont = 0;
                bool ruby = false;
                for (const auto &span : paragraph.spans) {
                    if (IsActive(span.timing, at)) {
                        spans.push_back(&span);
                        paragraphFont = std::max(paragraphFont, FontSizeOf(span.style).second);
                        ruby = ruby || (span.style.ruby && *span.style.ruby != "none");
                    }
                }
                if (spans.empty()) {
                    continue;
                }
                // 振り仮名らしきもの(小さい文字だけの段落)
                ruby = ruby || (paragraphFont > 0 && paragraphFont <= largestFont * 0.6);
                RegionBuilder builder(scale, drcs, caption);
                auto lines = SplitLines(spans);
                double ox = regionStyle.origin->x;
                double oy = regionStyle.origin->y;
                if (regionStyle.writing_mode == ttml::StyleWritingModeValue::Tbrl && regionStyle.extent) {
                    LayoutVertical(builder, lines, ox + regionStyle.extent->x, oy);
                }
                else {
                    LayoutHorizontal(builder, lines, ox, oy);
                }
                builder.Finish(ruby);
            }
        }
        segments.push_back(std::move(segment));
    }
    return true;
}

bool IsB24CaptionStatement(const uint8_t *payload, size_t size)
{
    // data_identifier、private_stream_id、PES_data_packet_header_lengthの後にデータグループ。
    // 字幕管理データ(data_group_idが0x00/0x20)は字幕が無くても送られてくる
    if (size < 3) {
        return false;
    }
    size_t index = 3 + (payload[2] & 0x0F);
    return index < size && ((payload[index] >> 2) & 0x0F) != 0;
}

} // namespace TtmlCaption
