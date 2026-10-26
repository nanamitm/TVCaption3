// TtmlCaption.cpp (4K/8K放送の字幕をTTMLのまま表示するための受信と変換) のテスト
#include "TtmlCaption.hpp"
#include <cstdio>
#include <string>
#include <vector>

namespace
{

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char *message)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", message);
    }
}

uint32_t Crc32Mpeg2(const std::vector<uint8_t> &data)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t b : data) {
        crc ^= static_cast<uint32_t>(b) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
        }
    }
    return crc;
}

// dantto4kのAppDataTransport::buildCaptionSectionsと同じ形の字幕のセクション
std::vector<std::vector<uint8_t>> CaptionSections(int componentTag, int dataType, int tmd, long long pts,
                                                  int resolution, const std::string &data, int sequence, size_t maxPayload)
{
    std::vector<uint8_t> body = {static_cast<uint8_t>(componentTag >> 8), static_cast<uint8_t>(componentTag),
                                 static_cast<uint8_t>(dataType), static_cast<uint8_t>(tmd),
                                 static_cast<uint8_t>(pts >= 0 ? 0x80 : 0), static_cast<uint8_t>(pts >= 0 ? (pts >> 32) & 1 : 0),
                                 static_cast<uint8_t>(pts >> 24), static_cast<uint8_t>(pts >> 16),
                                 static_cast<uint8_t>(pts >> 8), static_cast<uint8_t>(pts),
                                 static_cast<uint8_t>(data.size() >> 24), static_cast<uint8_t>(data.size() >> 16),
                                 static_cast<uint8_t>(data.size() >> 8), static_cast<uint8_t>(data.size()),
                                 15, 'j', 'p', 'n', static_cast<uint8_t>(resolution)};
    body.resize(body.size() + 11, 0);
    body.insert(body.end(), data.begin(), data.end());
    std::vector<std::vector<uint8_t>> sections;
    size_t count = (body.size() + maxPayload - 1) / maxPayload;
    for (size_t i = 0; i < count; ++i) {
        size_t size = std::min(maxPayload, body.size() - i * maxPayload);
        std::vector<uint8_t> s = {0xC1, 0, 0, static_cast<uint8_t>(sequence >> 8), static_cast<uint8_t>(sequence),
                                  0xC1, static_cast<uint8_t>(i), static_cast<uint8_t>(count - 1)};
        s.insert(s.end(), body.begin() + i * maxPayload, body.begin() + i * maxPayload + size);
        size_t sectionLength = s.size() - 3 + 4;
        s[1] = static_cast<uint8_t>(0xB0 | (sectionLength >> 8));
        s[2] = static_cast<uint8_t>(sectionLength);
        uint32_t crc = Crc32Mpeg2(s);
        s.push_back(static_cast<uint8_t>(crc >> 24));
        s.push_back(static_cast<uint8_t>(crc >> 16));
        s.push_back(static_cast<uint8_t>(crc >> 8));
        s.push_back(static_cast<uint8_t>(crc));
        sections.push_back(std::move(s));
    }
    return sections;
}

// セクションをつなげてTSパケットにする
std::vector<std::vector<uint8_t>> Packetize(const std::vector<std::vector<uint8_t>> &sections, int pid, int &counter)
{
    std::vector<std::vector<uint8_t>> packets;
    for (const auto &section : sections) {
        size_t pos = 0;
        while (pos < section.size()) {
            std::vector<uint8_t> p(188, 0xFF);
            p[0] = 0x47;
            p[1] = static_cast<uint8_t>((pos == 0 ? 0x40 : 0) | (pid >> 8));
            p[2] = static_cast<uint8_t>(pid);
            p[3] = static_cast<uint8_t>(0x10 | (counter++ & 0x0F));
            size_t offset = 4;
            if (pos == 0) {
                p[offset++] = 0;
            }
            size_t size = std::min(section.size() - pos, 188 - offset);
            std::copy(section.begin() + pos, section.begin() + pos + size, p.begin() + offset);
            pos += size;
            packets.push_back(std::move(p));
        }
    }
    return packets;
}

void TestReceiver()
{
    std::string ttml(1500, 'x');
    TtmlCaption::SectionReceiver receiver;
    int counter = 0;
    for (const auto &packet : Packetize(CaptionSections(0x30, 0, 0x02, 0x1ABCDEF01LL, 1, ttml, 7, 600), 0x0F00, counter)) {
        receiver.FeedPacket(packet.data());
    }
    auto documents = receiver.TakeDocuments();
    Check(documents.size() == 1, "a caption split over sections and packets is assembled");
    if (documents.size() == 1) {
        Check(documents[0].componentTag == 0x30 && documents[0].dataType == 0 && documents[0].tmd == 2, "component tag, data type, TMD");
        Check(documents[0].pts == 0x1ABCDEF01LL, "33-bit PTS");
        Check(documents[0].resolution == 1, "resolution");
        Check(documents[0].data == ttml, "data");
    }
    Check(receiver.TakeDocuments().empty(), "documents are taken once");

    for (const auto &packet : Packetize(CaptionSections(0x38, 6, 0x0F, -1, 1, "<svg/>", 8, 600), 0x0F00, counter)) {
        receiver.FeedPacket(packet.data());
    }
    documents = receiver.TakeDocuments();
    Check(documents.size() == 1 && documents[0].pts < 0 && documents[0].dataType == 6, "a resource without PTS");

    // 途中のパケットが欠けたセクションは捨てる
    auto packets = Packetize(CaptionSections(0x30, 0, 2, 0, 1, ttml, 9, 4000), 0x0F00, counter);
    for (size_t i = 0; i < packets.size(); ++i) {
        if (i != 3) {
            receiver.FeedPacket(packets[i].data());
        }
    }
    Check(receiver.TakeDocuments().empty(), "a section with a lost packet is dropped");
    for (const auto &packet : Packetize(CaptionSections(0x30, 0, 2, 0, 1, "<tt/>", 10, 600), 0x0F00, counter)) {
        receiver.FeedPacket(packet.data());
    }
    Check(receiver.TakeDocuments().size() == 1, "the next caption after a loss is received");
}

const char HORIZONTAL[] =
    R"(<?xml version="1.0" encoding="utf-8"?><tt xml:lang="ja" xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling" xmlns:arib-tt="http://www.arib.or.jp/ns/arib-tt">)"
    R"(<head><styling><style xml:id="s1" tts:fontSize="144px 144px" tts:color="#FFFF00FF" tts:backgroundColor="#00000080" tts:lineHeight="240px" arib-tt:letter-spacing="16px"/>)"
    R"(<style xml:id="s2" tts:fontSize="72px 144px" tts:color="#FFFFFFFF" tts:backgroundColor="#00000080" tts:lineHeight="240px" arib-tt:letter-spacing="8px"/></styling>)"
    R"(<layout><region xml:id="r1" tts:origin="1240px 1200px" tts:extent="1360px 480px"/></layout></head>)"
    R"(<body><div begin="00:00:10.000" end="00:00:13.000"><p region="r1"><span style="s1">はい</span><span style="s2">AB</span><br/><span style="s1" begin="00:00:01.000">次</span></p></div></body></tt>)";

void TestHorizontal()
{
    TtmlCaption::Converter converter;
    TtmlCaption::Document document;
    document.componentTag = 0x30;
    document.tmd = 2;
    document.pts = 0;
    document.data = HORIZONTAL;
    std::vector<TtmlCaption::Segment> segments;
    Check(converter.Convert(document, false, segments), "the document converts");
    Check(segments.size() == 3 && segments[0].timeMsec == 10000 && segments[1].timeMsec == 11000 && segments[2].timeMsec == 13000,
          "a segment at every begin and end");
    if (segments.size() != 3) {
        return;
    }
    const auto &first = segments[0].caption;
    Check(first.plane_width == 960 && first.plane_height == 540, "the plane is the B24 one");
    Check(first.flags & aribcaption::CaptionFlags::kCaptionFlagsClearScreen, "each segment replaces the screen");
    Check(first.regions.size() == 1 && first.regions[0].chars.size() == 4, "the first segment has the first line");
    if (first.regions.size() == 1 && first.regions[0].chars.size() == 4) {
        const auto &c = first.regions[0].chars;
        // 3840x2160 → 960x540 (1/4)
        Check(c[0].x == 310 && c[0].y == 300, "position is scaled to the plane");
        Check(c[0].char_width == 36 && c[0].char_height == 36 && c[0].char_horizontal_spacing == 4, "full width size and letter spacing");
        Check(c[0].char_vertical_spacing == 24, "the line height around the character");
        Check(c[1].x == 310 + 40, "characters advance by width and spacing");
        Check(c[2].char_width == 18 && c[2].char_horizontal_spacing == 2, "half width characters keep their size");
        Check(c[0].text_color.r == 255 && c[0].text_color.g == 255 && c[0].text_color.b == 0 && c[0].text_color.a == 255, "text colour");
        Check(c[0].back_color.a == 0x80, "background colour");
        Check(c[0].type == aribcaption::CaptionCharType::kText && c[0].codepoint == 0x306F && std::string(c[0].u8str) == "\xE3\x81\xAF", "text");
        Check(first.regions[0].x == 310 && first.regions[0].y == 300, "the region encloses the characters");
    }
    Check(first.text == "\xE3\x81\xAF\xE3\x81\x84" "AB", "caption text");
    Check(segments[1].caption.regions.size() == 1 && segments[1].caption.regions[0].chars.size() == 5, "the second line appears at its begin");
    Check(segments[2].caption.regions.empty(), "the last segment clears the screen");
}

void TestVertical()
{
    const char vertical[] =
        R"(<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling"><head><styling><style xml:id="s1" tts:fontSize="144px 144px"/><style xml:id="s2" tts:fontSize="72px 144px"/></styling>)"
        R"(<layout><region xml:id="r1" tts:origin="3000px 200px" tts:extent="240px 1600px" tts:writingMode="tbrl"/></layout></head>)"
        R"(<body><div><p region="r1"><span style="s1">ーっ</span><span style="s2">12</span></p></div></body></tt>)";
    TtmlCaption::Converter converter;
    TtmlCaption::Document document;
    document.tmd = 0x0F;
    document.data = vertical;
    std::vector<TtmlCaption::Segment> segments;
    Check(converter.Convert(document, false, segments) && segments.size() == 1 && segments[0].timeMsec == 0, "no time control is one segment");
    if (segments.size() != 1 || segments[0].caption.regions.size() != 1) {
        Check(false, "vertical region");
        return;
    }
    const auto &c = segments[0].caption.regions[0].chars;
    Check(c.size() == 4, "four characters");
    if (c.size() == 4) {
        Check(c[0].codepoint == 0xFE31, "the long vowel mark turns");
        Check(c[1].y < c[0].y + c[0].section_height() && c[1].x > c[0].x, "small kana move to the upper right");
        Check(c[0].y < c[2].y && c[2].y == c[3].y && c[2].x < c[3].x, "two half width digits share one cell");
        // 列は領域の右端 (3240px → 810) から左に幅144*1.2
        Check(c[0].x + c[0].section_width() == 810, "the column is at the right edge of the region");
    }
}

void TestDrcs()
{
    // 全面を塗る外字と、左半分だけ塗る外字
    const char font[] =
        R"(<?xml version="1.0"?><svg xmlns="http://www.w3.org/2000/svg"><defs><font><font-face units-per-em="1024" ascent="974" descent="50"/>)"
        R"(<glyph unicode="&#xe123;" horiz-adv-x="1024" d="M0,-50 L1024,-50 L1024,974 L0,974 Z"/>)"
        R"(<glyph unicode="&#xe124;" horiz-adv-x="1024" d="M0 -50h512v1024h-512z"/></font></defs></svg>)";
    TtmlCaption::Converter converter;
    Check(converter.AddFont(0x30, font), "the SVG font adds glyphs");
    Check(!converter.AddFont(0x30, font), "the same glyphs are not new");

    aribcaption::DRCS drcs;
    Check(TtmlCaption::RasterizeGlyph("M0 -50h512v1024h-512z", 1024, 974, 8, drcs), "a glyph rasterizes");
    auto pixel = [&](int x, int y) {
        size_t bit = (static_cast<size_t>(y) * drcs.width + x) * 2;
        return (drcs.pixels[bit / 8] >> (6 - bit % 8)) & 3;
    };
    Check(drcs.width == 8 && drcs.depth == 4 && drcs.depth_bits == 2, "2-bit gray DRCS");
    Check(pixel(0, 0) == 3 && pixel(3, 7) == 3 && pixel(4, 0) == 0 && pixel(7, 7) == 0, "the left half is filled");
    Check(TtmlCaption::RasterizeGlyph("M 0 0 Q 512 1024 1024 0 T 0 0 Z", 1024, 974, 16, drcs), "quadratic curves");
    Check(!TtmlCaption::RasterizeGlyph("M 0 0 A 10 10 0 0 1 100 100", 1024, 974, 16, drcs), "arcs are not drawn");

    const char text[] =
        R"(<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling"><head><layout><region xml:id="r1" tts:origin="100px 100px" tts:extent="1000px 200px"/></layout></head>)"
        R"(<body><div><p region="r1"><span>&#xe123;萊</span></p></div></body></tt>)";
    TtmlCaption::Document document;
    document.tmd = 0x0F;
    document.data = text;
    document.componentTag = 0x30;
    std::vector<TtmlCaption::Segment> segments;
    Check(converter.Convert(document, false, segments) && segments.size() == 1 && segments[0].caption.regions.size() == 1, "a caption with DRCS converts");
    if (segments.size() == 1 && segments[0].caption.regions.size() == 1) {
        const auto &caption = segments[0].caption;
        const auto &c = caption.regions[0].chars;
        Check(c.size() == 2 && c[0].type == aribcaption::CaptionCharType::kDRCS && c[0].drcs_code == 0xE123, "the private use character is DRCS");
        Check(caption.drcs_map.count(0xE123) == 1 && caption.drcs_map.at(0xE123).width == 96, "the DRCS pattern travels with the caption");
        Check(c.size() == 2 && c[1].type == aribcaption::CaptionCharType::kText, "other characters stay text");
    }
}

void TestDrcsIsolation()
{
    const char full[] = R"(<svg><font><font-face units-per-em="1024" ascent="974"/><glyph unicode="&#xe123;" d="M0 -50h1024v1024H0z"/></font></svg>)";
    const char half[] = R"(<svg><font><font-face units-per-em="1024" ascent="974"/><glyph unicode="&#xe123;" d="M0 -50h512v1024H0z"/></font></svg>)";
    TtmlCaption::Converter converter;
    Check(converter.AddFont(0x30, full), "first-language font");
    Check(converter.AddFont(0x31, half), "second-language font");
    Check(converter.AddFont(0x38, half), "superimpose font");
    Check(!converter.AddFont(0x20, full), "a non-caption component cannot register a font");
    TtmlCaption::Document document;
    document.tmd = 0x0F;
    document.data = R"(<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling"><head><layout><region xml:id="r" tts:origin="0px 0px" tts:extent="1000px 200px"/></layout></head><body><div><p region="r"><span>&#xe123;</span></p></div></body></tt>)";
    auto pixels = [&](int tag) {
        document.componentTag = tag;
        std::vector<TtmlCaption::Segment> segments;
        bool converted = converter.Convert(document, tag >= 0x38, segments);
        Check(converted && segments.size() == 1 && segments[0].caption.drcs_map.count(0xE123) == 1,
              "each component resolves its own DRCS glyph");
        return converted && !segments.empty() && segments[0].caption.drcs_map.count(0xE123)
            ? segments[0].caption.drcs_map.at(0xE123).pixels : std::vector<uint8_t>{};
    };
    auto original = pixels(0x30);
    auto alternate = pixels(0x31);
    Check(!original.empty() && original != alternate, "languages retain distinct glyphs at the same codepoint");
    Check(pixels(0x38) == alternate, "superimpose uses its own half-width glyph");
    converter.AddFont(0x38, full);
    Check(pixels(0x31) == alternate && pixels(0x30) == original && pixels(0x38) == original,
          "updating the superimpose font cannot overwrite either language");
    converter.Reset();
    Check(converter.AddFont(0x30, full) && converter.AddFont(0x31, half) && converter.AddFont(0x38, half),
          "reset clears every component's glyph cache");
}

void TestMissingGlyphs()
{
    TtmlCaption::Converter converter;
    TtmlCaption::Document document;
    document.componentTag = 0x30;
    document.tmd = 15;
    std::vector<TtmlCaption::Segment> segments;
    bool missing = true;
    document.data = "<tt";
    Check(!converter.Convert(document, false, segments, &missing) && !missing,
          "broken XML is distinct from a missing font");
    for (const char *text : {"&#xe123;", "&#xf0001;", "&#x100001;"}) {
        document.data = std::string(R"(<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling"><head><layout><region xml:id="r" tts:origin="0px 0px" tts:extent="1000px 200px"/></layout></head><body><div><p region="r"><span>)") + text + "</span></p></div></body></tt>";
        Check(!converter.Convert(document, false, segments, &missing) && missing && segments.empty(),
              "each Unicode private-use plane waits for its SVG resource");
    }
    document.data = HORIZONTAL;
    Check(converter.Convert(document, false, segments, &missing) && !missing,
          "ordinary text needs no SVG resource");
}

void TestStatement()
{
    const uint8_t management[] = {0x80, 0xFF, 0xF0, 0x00 << 2, 0, 0};
    const uint8_t managementB[] = {0x80, 0xFF, 0xF0, 0x20 << 2, 0, 0};
    const uint8_t statement[] = {0x80, 0xFF, 0xF0, 0x01 << 2, 0, 0};
    const uint8_t statementAfterHeader[] = {0x80, 0xFF, 0xF2, 0, 0, 0x21 << 2, 0};
    Check(!TtmlCaption::IsB24CaptionStatement(management, sizeof(management)), "caption management A");
    Check(!TtmlCaption::IsB24CaptionStatement(managementB, sizeof(managementB)), "caption management B");
    Check(TtmlCaption::IsB24CaptionStatement(statement, sizeof(statement)), "statement");
    Check(TtmlCaption::IsB24CaptionStatement(statementAfterHeader, sizeof(statementAfterHeader)), "statement after the PES data header");
    Check(!TtmlCaption::IsB24CaptionStatement(statement, 2), "too short");
}

void TestErrors()
{
    TtmlCaption::Converter converter;
    TtmlCaption::Document document;
    document.data = "<tt";
    std::vector<TtmlCaption::Segment> segments;
    Check(!converter.Convert(document, false, segments), "broken XML is rejected");
    Check(!converter.AddFont(0x30, "not xml"), "a broken font adds nothing");
}

} // namespace

int main()
{
    TestReceiver();
    TestHorizontal();
    TestVertical();
    TestDrcs();
    TestDrcsIsolation();
    TestMissingGlyphs();
    TestStatement();
    TestErrors();
    if (g_failures) {
        std::printf("TTML caption tests failed: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("TTML caption tests passed: %d checks\n", g_checks);
    return 0;
}
