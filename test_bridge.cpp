// Loads the bridge and checks its exports, that it fails cleanly without nvngx_dlssnr.dll,
// and probes the nvngx_dlssnr.dll given as the first argument, if any.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "bridge_api.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int wmain(int argc, wchar_t** argv) {
    HMODULE h = LoadLibraryW(L"bin\\itr2_dlss5_bridge.dll");
    CHECK(h != nullptr, "LoadLibraryW(bin\\itr2_dlss5_bridge.dll) failed (%lu)", GetLastError());
    if (!h) return 1;

    auto getApi = reinterpret_cast<PFN_ITR2_Dlss5Bridge_GetApi>(GetProcAddress(h, "ITR2_Dlss5Bridge_GetApi"));
    CHECK(getApi != nullptr, "GetProcAddress(ITR2_Dlss5Bridge_GetApi) failed");
    if (!getApi) return 1;

    const Dlss5BridgeApi* api = getApi();
    CHECK(api != nullptr && api->abiVersion == kDlss5BridgeApiVersion, "ABI version mismatch");
    CHECK(api->Probe && api->Initialize && api->Execute && api->ReleaseSlot && api->ReleaseAll &&
          api->Shutdown && api->GetStatusString && api->GetDetailString && api->GetVersionString &&
          api->GetLastNgxResult && api->GetSuccessfulFrames, "null function pointer in Dlss5BridgeApi");

    CHECK(strcmp(api->GetStatusString(), "not-probed") == 0, "initial status should be not-probed, got %s", api->GetStatusString());

    // a folder without nvngx_dlssnr.dll
    const bool okMissing = api->Probe(L"C:\\Windows\\System32");
    CHECK(!okMissing, "Probe should return false when nvngx_dlssnr.dll is absent");
    CHECK(strcmp(api->GetStatusString(), "not-found") == 0, "status after missing probe should be not-found, got %s", api->GetStatusString());
    printf("missing-file probe: status='%s' detail='%s'\n", api->GetStatusString(), api->GetDetailString());

    // not initialised yet: must return false
    Dlss5EvalDesc dummy{};
    CHECK(!api->Execute(&dummy), "Execute while uninitialized must return false");

    if (argc > 1 && argv[1][0]) {
        const bool okProbe = api->Probe(argv[1]);
        printf("explicit probe '%ls': ok=%d status='%s' version='%s' detail='%s'\n",
               argv[1], (int)okProbe, api->GetStatusString(), api->GetVersionString(), api->GetDetailString());
    }

    api->Shutdown();
    FreeLibrary(h);
    printf(g_fail ? "BRIDGETEST FAILED (%d failures)\n" : "BRIDGETEST PASSED (0 failures)\n", g_fail);
    return g_fail ? 1 : 0;
}
