// 実際のプラグインの受信・切り替え処理を、TVTestの最小限の代役で検証する。
#define NOMINMAX
#include <windows.h>
#include "Util.hpp"
// この翻訳単位ではプラグインのエクスポート関数を定義しない。
#define TVTEST_PLUGIN_VERSION TVTEST_PLUGIN_VERSION_(0,0,14)
#include "TVTestPlugin.h"
#include "TVCaption3.hpp"
#include <cstdio>
#include <string>

namespace
{
int g_checks = 0;
int g_failures = 0;
// プラグインがAddLog()で記録した警告
int g_warnings = 0;
std::wstring g_lastWarning;

void Check(bool ok, const char *message)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", message);
    }
}

// SEHを別関数に分け、範囲外アクセスをプロセス終了ではなくテスト失敗として報告する。
bool AcceptVideoPes(CViewerClockEstimator *clock, const BYTE *data, size_t len)
{
    __try {
        clock->SetVideoPes(false, true, data, len);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TestShortVideoPes()
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    BYTE *pages = static_cast<BYTE*>(VirtualAlloc(nullptr, info.dwPageSize * 2,
                                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(pages != nullptr, "guard-page allocation");
    if (!pages) return;
    DWORD oldProtect;
    bool protectedPage = VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &oldProtect) != FALSE;
    Check(protectedPage, "guard-page protection");
    if (protectedPage) {
        CViewerClockEstimator clock;
        BYTE *end = pages + info.dwPageSize;
        for (size_t len = 0; len < 9; ++len) {
            std::fill(end - len, end, 0);
            if (len >= 3) end[-static_cast<ptrdiff_t>(len) + 2] = 1;
            Check(AcceptVideoPes(&clock, end - len, len), "a short video PES never reads beyond its payload");
        }
        const BYTE valid[] = {0, 0, 1, 0xE0, 0, 0, 0x80, 0, 0};
        Check(AcceptVideoPes(&clock, valid, sizeof(valid)), "a complete video PES header is accepted");
    }
    VirtualFree(pages, 0, MEM_RELEASE);
}
}

struct CaptionPipelineTest
{
    static LRESULT CALLBACK HostCallback(TVTest::PluginParam *, UINT message, LPARAM lParam1, LPARAM lParam2)
    {
        if (message == TVTest::MESSAGE_ADDLOG && lParam2 == TVTest::LOG_TYPE_WARNING) {
            ++g_warnings;
            g_lastWarning = reinterpret_cast<LPCWSTR>(lParam1);
        }
        return TRUE;
    }

    static void Prepare(CTVCaption2 &plugin)
    {
        static TVTest::PluginParam param = {HostCallback, nullptr, nullptr, nullptr};
        plugin.SetPluginParam(&param);
        plugin.m_viewerClockEstimator.SetStreamPcr(900000);
    }

    static TtmlCaption::Document Document(const std::string &text, int tag = 0x30)
    {
        TtmlCaption::Document document;
        document.componentTag = tag;
        document.tmd = 2;
        document.pts = 1800000; // テスト中に表示時刻にならないよう10秒先に予約する
        document.data = R"(<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling"><head><layout><region xml:id="r" tts:origin="0px 0px" tts:extent="1000px 200px"/></layout></head><body><div><p region="r"><span>)" + text + "</span></p></div></body></tt>";
        return document;
    }

    static TtmlCaption::Document Font(int tag = 0x30)
    {
        TtmlCaption::Document font;
        font.componentTag = tag;
        font.dataType = 6;
        font.data = R"(<svg><font><font-face units-per-em="1024" ascent="974"/><glyph unicode="&#xe123;" d="M0 -50h1024v1024H0z"/></font></svg>)";
        return font;
    }

    static void TestLateFonts()
    {
        CTVCaption2 plugin;
        Prepare(plugin);
        plugin.m_ttmlDocumentQueue.push_back(Document("&#xe123;"));
        plugin.ProcessTtmlCaption();
        Check(plugin.m_pendingTtmlDocuments.size() == 1 && plugin.m_ttmlComponentTag[0] == -1 && plugin.m_ttmlQueue[0].empty(),
              "a missing glyph holds the original document without suppressing B24");
        plugin.m_ttmlDocumentQueue.push_back(Font(0x31));
        plugin.ProcessTtmlCaption();
        Check(plugin.m_pendingTtmlDocuments.size() == 1 && plugin.m_ttmlQueue[0].empty(),
              "another language's resource cannot release a waiting document");
        plugin.m_ttmlDocumentQueue.push_back(Font());
        plugin.ProcessTtmlCaption();
        Check(plugin.m_pendingTtmlDocuments.empty() && plugin.m_ttmlQueue[0].size() == 1 &&
              plugin.m_ttmlQueue[0][0].caption.drcs_map.count(0xE123) == 1,
              "the matching late font rebuilds and schedules the original caption");

        CTVCaption2 sameBatch;
        Prepare(sameBatch);
        sameBatch.m_ttmlDocumentQueue = {Document("&#xe123;"), Font()};
        sameBatch.ProcessTtmlCaption();
        Check(sameBatch.m_pendingTtmlDocuments.empty() && sameBatch.m_ttmlQueue[0].size() == 1 &&
              sameBatch.m_ttmlQueue[0][0].caption.drcs_map.count(0xE123) == 1,
              "a later font in the same batch is available before conversion");

        CTVCaption2 expired;
        Prepare(expired);
        auto alert = Document("&#xe123;", 0x38);
        alert.tmd = 15;
        expired.m_ttmlDocumentQueue.push_back(alert);
        expired.ProcessTtmlCaption();
        Check(expired.m_pendingTtmlDocuments.size() == 1, "a superimpose can wait for a resource");
        if (!expired.m_pendingTtmlDocuments.empty()) {
            expired.m_pendingTtmlDocuments[0].receivedTick = GetTickCount() - 3001;
        }
        expired.m_ttmlDocumentQueue.push_back(Font(0x38));
        expired.ProcessTtmlCaption();
        Check(expired.m_pendingTtmlDocuments.empty() && expired.m_ttmlComponentTag[1] == -1,
              "an expired alert cannot reappear when its font finally arrives");

        CTVCaption2 superseded;
        Prepare(superseded);
        superseded.m_ttmlDocumentQueue = {Document("&#xe123;"), Document("NEW")};
        superseded.ProcessTtmlCaption();
        Check(superseded.m_pendingTtmlDocuments.empty(), "a newer accepted document supersedes an older resource wait");
        superseded.m_ttmlDocumentQueue.push_back(Font());
        superseded.ProcessTtmlCaption();
        Check(superseded.m_ttmlQueue[0].size() == 1 && superseded.m_ttmlQueue[0][0].caption.text == "NEW",
              "a late resource cannot rewind to a superseded caption");

        CTVCaption2 bounded;
        Prepare(bounded);
        for (int i = 0; i < 40; ++i) bounded.m_ttmlDocumentQueue.push_back(Document("&#xe123;"));
        bounded.ProcessTtmlCaption();
        Check(bounded.m_pendingTtmlDocuments.size() == 32, "missing resources cannot grow the pending queue without limit");
        bounded.ResetTtml(true);
        Check(bounded.m_pendingTtmlDocuments.empty(), "a seek clears resource waits even when keeping fonts");
        bounded.m_ttmlDocumentQueue.push_back(Document("&#xe123;"));
        bounded.ProcessTtmlCaption();
        bounded.m_fTtmlCaption = false;
        bounded.ProcessTtmlCaption();
        Check(bounded.m_pendingTtmlDocuments.empty(), "disabling TTML clears pending resource waits");
    }

    static void FeedTtml(CTVCaption2 &plugin, const TtmlCaption::Document &document, int &counter)
    {
        plugin.m_appDataPid = 0xF00;
        std::vector<BYTE> section = {0xC1, 0xB0, 0, 0, static_cast<BYTE>(counter), 0xC1, 0, 0};
        std::vector<BYTE> body = {static_cast<BYTE>(document.componentTag >> 8), static_cast<BYTE>(document.componentTag),
                                  0, static_cast<BYTE>(document.tmd), 0x80};
        for (int shift = 32; shift >= 0; shift -= 8) body.push_back(static_cast<BYTE>(document.pts >> shift));
        for (int shift = 24; shift >= 0; shift -= 8) body.push_back(static_cast<BYTE>(document.data.size() >> shift));
        body.insert(body.end(), {4, 'j', 'p', 'n', 1});
        body.insert(body.end(), document.data.begin(), document.data.end());
        section.insert(section.end(), body.begin(), body.end());
        size_t length = section.size() - 3 + 4;
        section[1] |= static_cast<BYTE>(length >> 8);
        section[2] = static_cast<BYTE>(length);
        uint32_t crc = 0xFFFFFFFF;
        for (BYTE b : section) {
            crc ^= static_cast<uint32_t>(b) << 24;
            for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
        }
        for (int shift = 24; shift >= 0; shift -= 8) section.push_back(static_cast<BYTE>(crc >> shift));
        for (size_t pos = 0; pos < section.size(); ) {
            std::vector<BYTE> ts(188, 0xFF);
            ts[0] = 0x47;
            ts[1] = static_cast<BYTE>(0x0F | (pos == 0 ? 0x40 : 0));
            ts[2] = 0;
            ts[3] = static_cast<BYTE>(0x10 | (counter++ & 15));
            size_t offset = 4;
            if (pos == 0) ts[offset++] = 0;
            size_t count = std::min(section.size() - pos, 188 - offset);
            std::copy_n(section.begin() + pos, count, ts.begin() + offset);
            plugin.ProcessPacket(ts.data());
            pos += count;
        }
    }

    static std::vector<BYTE> B24Statement()
    {
        std::vector<BYTE> pes(19, 0);
        pes[2] = 1;
        pes[3] = 0xBF;
        pes[5] = 13;
        pes[6] = 0x80;
        pes[7] = 0xFF;
        pes[8] = 0xF0;
        pes[9] = 0x04; // 字幕管理データではなく字幕文
        return pes;
    }

    static void TestFallbackAcceptance()
    {
        const auto index = CTVCaption2::STREAM_CAPTION;
        auto statement = B24Statement();
        for (int rejected = 0; rejected < 3; ++rejected) {
            CTVCaption2 plugin;
            Prepare(plugin);
            plugin.m_ttmlDocumentQueue.push_back(Document("GOOD"));
            plugin.ProcessTtmlCaption();
            Check(plugin.m_ttmlComponentTag[index] == 0x30, "a good TTML document activates TTML");
            plugin.m_viewerClockEstimator.SetStreamPcr(1800000);
            plugin.NoteB24Pes(index, statement);
            Check(plugin.m_b24OnlySincePcr[index] == 1800000, "a B24-only statement starts the fallback interval");
            plugin.m_heldPes[index].push_back(statement);
            auto document = Document(rejected == 2 ? "&#xe123;" : "IGNORED", rejected == 1 ? 0x31 : 0x30);
            if (rejected == 0) document.data = "<tt";
            int counter = 0;
            FeedTtml(plugin, document, counter);
            Check(plugin.m_ttmlDocumentQueue.size() == 1, "the rejected TTML travels through the real TS receiver");
            plugin.ProcessTtmlCaption();
            Check(plugin.m_b24OnlySincePcr[index] == 1800000,
                  "invalid XML, unselected language and missing glyphs cannot cancel fallback");
            plugin.m_viewerClockEstimator.SetStreamPcr(1800000 + 3100 * PCR_PER_MSEC);
            FeedTtml(plugin, document, counter);
            plugin.ProcessTtmlCaption();
            Check(plugin.m_ttmlComponentTag[index] == -1 && plugin.m_pendingTtmlDocuments.empty(),
                  "B24 resumes despite continuing unusable TTML and clears resource waits");
            Check(plugin.m_heldPes[index].empty() && plugin.m_caption1PesQueue.size() == 1 &&
                  plugin.m_caption1PesQueue.front() == statement,
                  "fallback restores the held B24 PES without losing its contents");
        }

        CTVCaption2 recovered;
        Prepare(recovered);
        recovered.m_ttmlDocumentQueue.push_back(Document("OLD"));
        recovered.ProcessTtmlCaption();
        recovered.m_viewerClockEstimator.SetStreamPcr(1800000);
        recovered.NoteB24Pes(index, statement);
        recovered.m_viewerClockEstimator.SetStreamPcr(1800000 + 3100 * PCR_PER_MSEC);
        auto good = Document("RECOVERED");
        good.pts = 2700000;
        recovered.m_ttmlDocumentQueue.push_back(good);
        recovered.ProcessTtmlCaption();
        Check(recovered.m_ttmlComponentTag[index] == 0x30 && recovered.m_b24OnlySincePcr[index] == -1,
              "a newly accepted document cancels an expired fallback snapshot in the same processing pass");
    }

    static void TestAdaptationOnly()
    {
        for (bool superimpose : {false, true}) {
            CTVCaption2 plugin;
            plugin.m_caption1Pid = 0x100;
            plugin.m_caption2Pid = 0x101;
            int pid = superimpose ? 0x101 : 0x100;
            std::vector<BYTE> pes(400, 0);
            pes[2] = 1;
            pes[3] = 0xBF;
            pes[4] = static_cast<BYTE>((pes.size() - 6) >> 8);
            pes[5] = static_cast<BYTE>(pes.size() - 6);
            auto packet = [&](size_t offset, int counter) {
                std::vector<BYTE> ts(188, 0xFF);
                ts[0] = 0x47;
                ts[1] = static_cast<BYTE>((offset == 0 ? 0x40 : 0) | (pid >> 8));
                ts[2] = static_cast<BYTE>(pid);
                ts[3] = static_cast<BYTE>(0x10 | counter);
                size_t count = std::min<size_t>(184, pes.size() - offset);
                std::copy_n(pes.begin() + offset, count, ts.begin() + 4);
                return ts;
            };
            auto first = packet(0, 4);
            plugin.ProcessPacket(first.data());
            auto &partial = superimpose ? plugin.m_caption2Pes.second : plugin.m_caption1Pes.second;
            auto &queue = superimpose ? plugin.m_caption2PesQueue : plugin.m_caption1PesQueue;
            Check(partial.size() == 184, "the first TS packet starts a partial B24 PES");

            std::vector<BYTE> adaptation(188, 0);
            adaptation[0] = 0x47;
            adaptation[1] = static_cast<BYTE>(pid >> 8);
            adaptation[2] = static_cast<BYTE>(pid);
            adaptation[3] = 0x24; // adaptation-only、counterは前のまま
            adaptation[4] = 183;
            plugin.ProcessPacket(adaptation.data());
            Check(partial.size() == 184, "adaptation-only TS preserves the partial B24 PES");
            auto second = packet(184, 5);
            auto last = packet(368, 6);
            plugin.ProcessPacket(second.data());
            plugin.ProcessPacket(last.data());
            Check(queue.size() == 1 && queue.front() == pes, "B24 PES completes across an adaptation-only packet");

            queue.clear();
            plugin.ProcessPacket(first.data());
            second[3] = 0x16; // 本当の欠落は引き続き検出する
            plugin.ProcessPacket(second.data());
            plugin.ProcessPacket(last.data());
            Check(queue.empty() && partial.empty(), "a genuine payload continuity loss still drops the PES");
        }
    }

    // 字形を描けない外字。dが有効なパスにならないのでDRCSを作れない
    static TtmlCaption::Document UndrawableFont(int tag = 0x30)
    {
        TtmlCaption::Document font;
        font.componentTag = tag;
        font.dataType = 6;
        font.data = R"(<svg><font><font-face units-per-em="1024" ascent="974"/><glyph unicode="&#xe200;" d="Z"/></font></svg>)";
        return font;
    }

    static void TestUndrawableGlyphWarning()
    {
        CTVCaption2 plugin;
        Prepare(plugin);
        // 描ける外字だけなら何も記録しない
        int base = g_warnings;
        plugin.m_ttmlDocumentQueue.push_back(Font());
        plugin.ProcessTtmlCaption();
        Check(g_warnings == base, "a font whose glyphs all rasterize logs nothing");

        plugin.m_ttmlDocumentQueue.push_back(UndrawableFont());
        plugin.ProcessTtmlCaption();
        Check(g_warnings == base + 1, "an undrawable glyph is reported");
        Check(g_lastWarning.find(L"U+E200") != std::wstring::npos &&
              g_lastWarning.find(L"component_tag=30") != std::wstring::npos,
              "the warning names the codepoint and the component");

        // リソースは繰り返し届くので、同じ原因で記録し続けない
        for (int i = 0; i < 3; ++i) {
            plugin.m_ttmlDocumentQueue.push_back(UndrawableFont());
            plugin.ProcessTtmlCaption();
        }
        Check(g_warnings == base + 1, "a repeated resource is reported only once");

        // 外字を捨てるリセットのあとは、改めて記録できる
        plugin.ResetTtml(false);
        plugin.m_ttmlDocumentQueue.push_back(UndrawableFont(0x38));
        plugin.ProcessTtmlCaption();
        Check(g_warnings == base + 2 && g_lastWarning.find(L"component_tag=38") != std::wstring::npos,
              "a reset that drops fonts arms the warning again");

        // 外字を残すリセットでは、同じ原因を繰り返し記録しない
        plugin.ResetTtml(true);
        plugin.m_ttmlDocumentQueue.push_back(UndrawableFont(0x38));
        plugin.ProcessTtmlCaption();
        Check(g_warnings == base + 2, "a seek that keeps fonts does not repeat the warning");
    }

    static void Run()
    {
        TestShortVideoPes();
        TestAdaptationOnly();
        TestLateFonts();
        TestFallbackAcceptance();
        TestUndrawableGlyphWarning();
    }
};

int main()
{
    CaptionPipelineTest::Run();
    std::printf("Caption pipeline tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
