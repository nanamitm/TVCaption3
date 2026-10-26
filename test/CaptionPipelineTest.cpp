// 実際のプラグインの受信・切り替え処理を、TVTestの最小限の代役で検証する。
#define NOMINMAX
#include <windows.h>
#include "Util.hpp"
// この翻訳単位ではプラグインのエクスポート関数を定義しない。
#define TVTEST_PLUGIN_VERSION TVTEST_PLUGIN_VERSION_(0,0,14)
#include "TVTestPlugin.h"
#include "TVCaption3.hpp"
#include <cstdio>

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

    static void Run()
    {
        TestShortVideoPes();
        TestAdaptationOnly();
    }
};

int main()
{
    CaptionPipelineTest::Run();
    std::printf("Caption pipeline tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
