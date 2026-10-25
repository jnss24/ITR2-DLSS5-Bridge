// C interface of itr2_dlss5_bridge.dll. No NGX headers needed.
#pragma once
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

constexpr uint32_t kDlss5BridgeApiVersion = 1u;

struct Dlss5Tuning {
    float    intensity              = 0.8f;
    float    localToneStrength      = 0.75f;
    float    localStructureStrength = 0.9f;
    float    skinStructureStrength  = 0.9f;
    uint32_t style                  = 3u;
    uint32_t useAutoMask            = 1u;
    uint32_t uiCorrection           = 0u;
    uint32_t performanceQuality     = 2u;   // NVSDK_NGX_PerfQuality_Value, 2 = MaxQuality
    uint32_t outputPreset           = 0u;   // 0 = runtime default
};

struct Dlss5EvalDesc {
    ID3D12GraphicsCommandList* cl            = nullptr;
    uint32_t                   slot          = 0u;      // 0-1: first pass, left/right eye; 2-3: second pass
    ID3D12Resource*            color         = nullptr;
    ID3D12Resource*            depth         = nullptr;
    ID3D12Resource*            motionVectors = nullptr;
    ID3D12Resource*            output        = nullptr;
    uint32_t                   inputWidth    = 0u;
    uint32_t                   inputHeight   = 0u;
    uint32_t                   outputWidth   = 0u;
    uint32_t                   outputHeight  = 0u;
    float                      mvScaleX      = 1.f;
    float                      mvScaleY      = 1.f;
    uint32_t                   depthInverted = 1u;
    uint32_t                   reset         = 0u;
    Dlss5Tuning                tuning;
};

struct Dlss5BridgeApi {
    uint32_t abiVersion;
    bool        (__cdecl* Probe)(const wchar_t* explicitPathOrDir);
    bool        (__cdecl* Initialize)(ID3D12Device* device, const wchar_t* writableDataDir);
    bool        (__cdecl* Execute)(const Dlss5EvalDesc* desc);
    void        (__cdecl* ReleaseSlot)(uint32_t slot);
    void        (__cdecl* ReleaseAll)();
    void        (__cdecl* Shutdown)();
    const char* (__cdecl* GetStatusString)();
    const char* (__cdecl* GetDetailString)();
    const char* (__cdecl* GetVersionString)();
    uint32_t    (__cdecl* GetLastNgxResult)();
    uint64_t    (__cdecl* GetSuccessfulFrames)();
};

using PFN_ITR2_Dlss5Bridge_GetApi = const Dlss5BridgeApi* (__cdecl*)();
