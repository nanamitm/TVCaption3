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
    static void Run() { TestShortVideoPes(); }
};

int main()
{
    CaptionPipelineTest::Run();
    std::printf("Caption pipeline tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
