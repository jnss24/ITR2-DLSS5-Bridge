// DLSS 5 (NGX feature 18) for the ITR2 DLSS mod, which loads this DLL when it's in the game folder.
// Based on src/Features/Upscaling/NeuralRendering/Runtime.cpp from Maeyanie/OpenShadersNRForked
// (commit e9c5f967), GPL-3.0-or-later.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "ngx_min.h"
#include "bridge_api.h"

namespace {

constexpr wchar_t kRuntimeName[] = L"nvngx_dlssnr.dll";
constexpr auto kFeatureDlssNr = NVSDK_NGX_Feature_DlssNr;
constexpr std::array<const char*, 5> kRequiredExports{
    "NVSDK_NGX_D3D12_Init_Ext",
    "NVSDK_NGX_D3D12_CreateFeature",
    "NVSDK_NGX_D3D12_EvaluateFeature",
    "NVSDK_NGX_D3D12_ReleaseFeature",
    "NVSDK_NGX_D3D12_Shutdown1",
};

using GetUnsignedValue   = unsigned int(NVSDK_CONV*)();
using InitD3D12          = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using ShutdownD3D12      = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using AllocateParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using DestroyParameters  = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using CreateFeature      = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFeature    = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFeature     = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using GetModuleFileNameWFunction = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

enum class RuntimeStatus {
    NotProbed, NotFound, VersionUnavailable, UnsupportedVersion, LoadFailed, MissingExport,
    Ready, InitializationFailed, CoreUnavailable, ParameterAllocationFailed, Initialized,
};

const char* StatusToString(RuntimeStatus status) {
    switch (status) {
    case RuntimeStatus::NotProbed:                 return "not-probed";
    case RuntimeStatus::NotFound:                  return "not-found";
    case RuntimeStatus::VersionUnavailable:        return "version-unavailable";
    case RuntimeStatus::UnsupportedVersion:        return "unsupported-version";
    case RuntimeStatus::LoadFailed:                return "load-failed";
    case RuntimeStatus::MissingExport:             return "missing-export";
    case RuntimeStatus::Ready:                     return "ready";
    case RuntimeStatus::InitializationFailed:      return "initialization-failed";
    case RuntimeStatus::CoreUnavailable:           return "core-unavailable";
    case RuntimeStatus::ParameterAllocationFailed: return "parameter-allocation-failed";
    case RuntimeStatus::Initialized:               return "initialized";
    }
    return "unknown";
}

struct DllVersion {
    uint16_t major = 0, minor = 0, build = 0, rev = 0;
};

bool QueryDllVersion(const std::wstring& path, DllVersion& out, std::string& formatted) {
    DWORD dummy = 0;
    const DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!sz) return false;
    std::vector<uint8_t> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return false;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len) || !ffi || len < sizeof(VS_FIXEDFILEINFO))
        return false;
    out.major = HIWORD(ffi->dwFileVersionMS);
    out.minor = LOWORD(ffi->dwFileVersionMS);
    out.build = HIWORD(ffi->dwFileVersionLS);
    out.rev   = LOWORD(ffi->dwFileVersionLS);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", out.major, out.minor, out.build, out.rev);
    formatted = tmp;
    return true;
}

GetModuleFileNameWFunction g_originalGetModuleFileNameW = nullptr;
HMODULE g_callerModule = nullptr;
std::wstring g_spoofedRuntimePath;
uint32_t g_proxyHits = 0;

HMODULE FindNgxCoreModule() {
    std::array<HMODULE, 1024> modules{};
    DWORD bytesNeeded = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(sizeof(modules)), &bytesNeeded))
        return nullptr;
    const std::size_t count = std::min<std::size_t>(modules.size(), bytesNeeded / sizeof(HMODULE));
    for (std::size_t index = 0; index < count; ++index) {
        if (GetProcAddress(modules[index], "NVSDK_NGX_D3D12_AllocateParameters") &&
            GetProcAddress(modules[index], "NVSDK_NGX_D3D12_DestroyParameters"))
            return modules[index];
    }
    return nullptr;
}

DWORD WINAPI SignedRuntimeGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
    if (module == g_callerModule && filename && size && !g_spoofedRuntimePath.empty()) {
        ++g_proxyHits;
        const DWORD length = static_cast<DWORD>(g_spoofedRuntimePath.size());
        const DWORD copyLength = std::min(length, size - 1);
        std::memcpy(filename, g_spoofedRuntimePath.data(), copyLength * sizeof(wchar_t));
        filename[copyLength] = L'\0';
        return copyLength < length ? size : length;
    }
    return g_originalGetModuleFileNameW ? g_originalGetModuleFileNameW(module, filename, size) : 0;
}

class SignedRuntimePathScope {
public:
    SignedRuntimePathScope(HMODULE runtime, const std::filesystem::path& ngxPath) {
        if (!runtime) return;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&SignedRuntimeGetModuleFileNameW), &g_callerModule);
        g_spoofedRuntimePath = ngxPath.wstring();
        g_proxyHits = 0;

        auto* base = reinterpret_cast<uint8_t*>(runtime);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!imports.VirtualAddress) return;

        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
        for (; descriptor->Name; ++descriptor) {
            if (!descriptor->OriginalFirstThunk) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
            auto* functions = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++functions) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(import->Name), "GetModuleFileNameW")) continue;
                slot_ = reinterpret_cast<void**>(&functions->u1.Function);
                g_originalGetModuleFileNameW = reinterpret_cast<GetModuleFileNameWFunction>(*slot_);
                DWORD oldProtection = 0;
                if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &oldProtection)) {
                    *slot_ = reinterpret_cast<void*>(&SignedRuntimeGetModuleFileNameW);
                    DWORD ignored = 0;
                    VirtualProtect(slot_, sizeof(*slot_), oldProtection, &ignored);
                    FlushInstructionCache(GetCurrentProcess(), slot_, sizeof(*slot_));
                    installed_ = true;
                }
                return;
            }
        }
    }

    ~SignedRuntimePathScope() {
        if (installed_ && slot_) {
            DWORD oldProtection = 0;
            if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &oldProtection)) {
                *slot_ = reinterpret_cast<void*>(g_originalGetModuleFileNameW);
                DWORD ignored = 0;
                VirtualProtect(slot_, sizeof(*slot_), oldProtection, &ignored);
                FlushInstructionCache(GetCurrentProcess(), slot_, sizeof(*slot_));
            }
        }
        g_spoofedRuntimePath.clear();
        g_callerModule = nullptr;
        g_originalGetModuleFileNameW = nullptr;
    }

    bool IsInstalled() const { return installed_; }
    uint32_t Hits() const { return g_proxyHits; }

private:
    void** slot_ = nullptr;
    bool installed_ = false;
};

std::filesystem::path GetBridgeDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetBridgeDir), &self);
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(self, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

std::filesystem::path ResolveRuntimePath(const wchar_t* explicitPathOrDir) {
    std::error_code ec;
    if (explicitPathOrDir && explicitPathOrDir[0]) {
        std::filesystem::path p(explicitPathOrDir);
        std::filesystem::path cand = std::filesystem::is_directory(p, ec) ? (p / kRuntimeName) : p;
        if (std::filesystem::is_regular_file(cand, ec)) return cand;
    }
    const auto dir = GetBridgeDir();
    const auto cand = dir / kRuntimeName;
    if (std::filesystem::is_regular_file(cand, ec)) return cand;
    return {};
}

class BridgeState {
public:
    static constexpr uint32_t kMaxSlots = 4u;   // 0-1: first pass, left/right eye; 2-3: second pass
    ~BridgeState() { Shutdown(); }

    bool Probe(const wchar_t* explicitPathOrDir) {
        Shutdown();
        path_ = ResolveRuntimePath(explicitPathOrDir);
        if (path_.empty()) {
            status_ = RuntimeStatus::NotFound;
            detail_ = "nvngx_dlssnr.dll was not found";
            return false;
        }
        DllVersion ver{};
        if (!QueryDllVersion(path_.wstring(), ver, version_)) {
            status_ = RuntimeStatus::VersionUnavailable;
            detail_ = "DLL version resource is unavailable";
            return false;
        }
        if (ver.major != 310 || ver.minor != 8) {
            status_ = RuntimeStatus::UnsupportedVersion;
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "expected DLSSNR 310.8.x, found %s", version_.c_str());
            detail_ = tmp;
            return false;
        }
        module_ = LoadLibraryExW(path_.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module_) {
            status_ = RuntimeStatus::LoadFailed;
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "LoadLibraryExW failed with %lu", GetLastError());
            detail_ = tmp;
            return false;
        }
        for (const char* exportName : kRequiredExports) {
            if (!GetProcAddress(module_, exportName)) {
                status_ = RuntimeStatus::MissingExport;
                detail_ = std::string("missing export ") + exportName;
                FreeLibrary(module_);
                module_ = nullptr;
                return false;
            }
        }
        auto getAppId = reinterpret_cast<GetUnsignedValue>(GetProcAddress(module_, "NVSDK_NGX_GetApplicationId"));
        auto getApi   = reinterpret_cast<GetUnsignedValue>(GetProcAddress(module_, "NVSDK_NGX_GetAPIVersion"));
        if (!getAppId || !getApi) {
            status_ = RuntimeStatus::MissingExport;
            detail_ = "signed runtime identity exports are missing";
            FreeLibrary(module_);
            module_ = nullptr;
            return false;
        }
        applicationId_ = getAppId();
        apiVersion_ = getApi();
        status_ = RuntimeStatus::Ready;
        detail_ = "ready";
        return true;
    }

    bool Initialize(ID3D12Device* device, const wchar_t* writableDataDir) {
        if (!device) return false;
        if (status_ == RuntimeStatus::Initialized && device_ != device)
            Shutdown();
        if (!module_ && !Probe(nullptr))
            return false;
        if (status_ == RuntimeStatus::Initialized && device_ == device)
            return true;

        auto initialize = reinterpret_cast<InitD3D12>(GetProcAddress(module_, "NVSDK_NGX_D3D12_Init_Ext"));
        std::filesystem::path writablePath = (writableDataDir && writableDataDir[0]) ? std::filesystem::path(writableDataDir) : std::filesystem::path();
        if (writablePath.empty()) {
            wchar_t tempPath[MAX_PATH]{};
            GetTempPathW(MAX_PATH, tempPath);
            writablePath = std::filesystem::path(tempPath) / L"ITR2-DLSS5-NGX";
        }
        std::error_code ec;
        std::filesystem::create_directories(writablePath, ec);
        SignedRuntimePathScope scope(module_, path_.parent_path() / L"nvngx.dll");
        if (!scope.IsInstalled()) {
            status_ = RuntimeStatus::InitializationFailed;
            detail_ = "failed to install signed-runtime path proxy";
            return false;
        }
        ngxResult_ = static_cast<uint32_t>(initialize(applicationId_, writablePath.c_str(), device,
            static_cast<NVSDK_NGX_Version>(apiVersion_), nullptr));
        if (ngxResult_ != NVSDK_NGX_Result_Success) {
            status_ = RuntimeStatus::InitializationFailed;
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "NGX init failed 0x%08X, proxyHits=%u", ngxResult_, scope.Hits());
            detail_ = tmp;
            return false;
        }
        device_ = device;
        device_->AddRef();

        HMODULE core = FindNgxCoreModule();
        if (!core) {
            Shutdown();
            status_ = RuntimeStatus::CoreUnavailable;
            detail_ = "NGX parameter API module was not found";
            return false;
        }
        auto allocate = reinterpret_cast<AllocateParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters"));
        for (uint32_t s = 0; s < kMaxSlots; ++s) {
            NVSDK_NGX_Parameter* params = nullptr;
            ngxResult_ = static_cast<uint32_t>(allocate(&params));
            if (ngxResult_ != NVSDK_NGX_Result_Success || !params) {
                const auto res = ngxResult_;
                Shutdown();
                status_ = RuntimeStatus::ParameterAllocationFailed;
                ngxResult_ = res;
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "NGX parameter allocation failed 0x%08X", res);
                detail_ = tmp;
                return false;
            }
            params->Set("FreeMemOnReleaseFeature", 1);
            parameters_[s] = params;
        }
        status_ = RuntimeStatus::Initialized;
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "DLSSNR %s initialized (appId=%u api=0x%X hits=%u)", version_.c_str(), applicationId_, apiVersion_, scope.Hits());
        detail_ = tmp;
        return true;
    }

    bool Execute(const Dlss5EvalDesc* d) {
        if (!d || status_ != RuntimeStatus::Initialized || !d->cl || d->slot >= kMaxSlots ||
            !d->color || !d->depth || !d->motionVectors || !d->output ||
            !d->inputWidth || !d->inputHeight || !d->outputWidth || !d->outputHeight)
            return false;
        const uint32_t slot = d->slot;
        auto* parameters = parameters_[slot];
        if (!parameters) return false;

        auto create   = reinterpret_cast<CreateFeature>(GetProcAddress(module_, "NVSDK_NGX_D3D12_CreateFeature"));
        auto evaluate = reinterpret_cast<EvaluateFeature>(GetProcAddress(module_, "NVSDK_NGX_D3D12_EvaluateFeature"));
        auto release  = reinterpret_cast<ReleaseFeature>(GetProcAddress(module_, "NVSDK_NGX_D3D12_ReleaseFeature"));
        SignedRuntimePathScope scope(module_, path_.parent_path() / L"nvngx.dll");
        if (!scope.IsInstalled()) return false;

        const bool rebuild = featureInputWidth_[slot] != d->inputWidth ||
                             featureInputHeight_[slot] != d->inputHeight ||
                             featureOutputWidth_[slot] != d->outputWidth ||
                             featureOutputHeight_[slot] != d->outputHeight ||
                             featureQuality_[slot] != d->tuning.performanceQuality ||
                             featurePreset_[slot] != d->tuning.outputPreset;
        if (featureHandles_[slot] && rebuild) {
            release(featureHandles_[slot]);
            featureHandles_[slot] = nullptr;
        }

        bool reset = d->reset != 0;
        if (!featureHandles_[slot]) {
            parameters->Reset();
            parameters->Set("FreeMemOnReleaseFeature", 1);
            parameters->Set("Width", d->outputWidth);
            parameters->Set("Height", d->outputHeight);
            parameters->Set("DLSSNR.Width", d->outputWidth);
            parameters->Set("DLSSNR.Height", d->outputHeight);
            parameters->Set("DLSSNR.ScalingRatio", static_cast<float>(d->outputWidth) / static_cast<float>(d->inputWidth));
            parameters->Set("PerfQualityValue", d->tuning.performanceQuality);
            parameters->Set("DLSSNR.Hint.Render.Preset", d->tuning.outputPreset);
            NVSDK_NGX_Handle* handle = nullptr;
            ngxResult_ = static_cast<uint32_t>(create(d->cl, kFeatureDlssNr, parameters, &handle));
            if (ngxResult_ != NVSDK_NGX_Result_Success || !handle) {
                char tmp[160];
                snprintf(tmp, sizeof(tmp), "Feature 18 create failed slot=%u result=0x%08X proxyHits=%u", slot, ngxResult_, scope.Hits());
                detail_ = tmp;
                return false;
            }
            featureHandles_[slot]      = handle;
            featureInputWidth_[slot]   = d->inputWidth;
            featureInputHeight_[slot]  = d->inputHeight;
            featureOutputWidth_[slot]  = d->outputWidth;
            featureOutputHeight_[slot] = d->outputHeight;
            featureQuality_[slot]      = d->tuning.performanceQuality;
            featurePreset_[slot]       = d->tuning.outputPreset;
            reset = true;
        }

        parameters->Reset();
        parameters->Set("FreeMemOnReleaseFeature", 1);
        parameters->Set("DLSSNR.Color", d->color);
        parameters->Set("DLSSNR.Depth", d->depth);
        parameters->Set("DLSSNR.MVec", d->motionVectors);
        parameters->Set("DLSSNR.Output", d->output);
        parameters->Set("DLSSNR.ColorSubrectBaseX", 0u);
        parameters->Set("DLSSNR.ColorSubrectBaseY", 0u);
        parameters->Set("DLSSNR.ColorSubrectWidth", d->outputWidth);
        parameters->Set("DLSSNR.ColorSubrectHeight", d->outputHeight);
        parameters->Set("DLSSNR.DepthSubrectBaseX", 0u);
        parameters->Set("DLSSNR.DepthSubrectBaseY", 0u);
        parameters->Set("DLSSNR.DepthSubrectWidth", d->inputWidth);
        parameters->Set("DLSSNR.DepthSubrectHeight", d->inputHeight);
        parameters->Set("DLSSNR.MVecSubrectBaseX", 0u);
        parameters->Set("DLSSNR.MVecSubrectBaseY", 0u);
        parameters->Set("DLSSNR.MVecSubrectWidth", d->inputWidth);
        parameters->Set("DLSSNR.MVecSubrectHeight", d->inputHeight);
        parameters->Set("DLSSNR.OutputSubrectBaseX", 0u);
        parameters->Set("DLSSNR.OutputSubrectBaseY", 0u);
        parameters->Set("DLSSNR.OutputSubrectWidth", d->outputWidth);
        parameters->Set("DLSSNR.OutputSubrectHeight", d->outputHeight);
        parameters->Set("DLSSNR.MVecScaleX", d->mvScaleX);
        parameters->Set("DLSSNR.MVecScaleY", d->mvScaleY);
        parameters->Set("DLSSNR.DepthInverted", d->depthInverted ? 1u : 0u);
        parameters->Set("DLSSNR.Enabled", 1u);
        parameters->Set("DLSSNR.Reset", reset ? 1u : 0u);
        parameters->Set("DLSSNR.Intensity", d->tuning.intensity);
        parameters->Set("DLSSNR.LocalToneStrength", d->tuning.localToneStrength);
        parameters->Set("DLSSNR.LocalStructureStrength", d->tuning.localStructureStrength);
        parameters->Set("DLSSNR.SkinStructureStrength", d->tuning.skinStructureStrength);
        parameters->Set("DLSSNR.UseAutoMask", d->tuning.useAutoMask ? 1u : 0u);
        parameters->Set("DLSSNR.Style", d->tuning.style);
        parameters->Set("DLSSNR.UICorrection", d->tuning.uiCorrection ? 1u : 0u);

        ngxResult_ = static_cast<uint32_t>(evaluate(d->cl, featureHandles_[slot], parameters, nullptr));
        if (ngxResult_ != NVSDK_NGX_Result_Success) {
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "Feature 18 evaluate failed slot=%u result=0x%08X", slot, ngxResult_);
            detail_ = tmp;
            return false;
        }
        ++successfulFrames_;
        return true;
    }

    void ReleaseSlot(uint32_t slot) {
        if (!module_ || slot >= kMaxSlots) return;
        SignedRuntimePathScope scope(module_, path_.parent_path() / L"nvngx.dll");
        auto release = reinterpret_cast<ReleaseFeature>(GetProcAddress(module_, "NVSDK_NGX_D3D12_ReleaseFeature"));
        if (featureHandles_[slot] && release) {
            release(featureHandles_[slot]);
        }
        featureHandles_[slot] = nullptr;
        featureInputWidth_[slot] = featureInputHeight_[slot] = 0;
        featureOutputWidth_[slot] = featureOutputHeight_[slot] = 0;
        featureQuality_[slot] = featurePreset_[slot] = UINT32_MAX;
    }

    void ReleaseAll() {
        for (uint32_t s = 0; s < kMaxSlots; ++s) ReleaseSlot(s);
        successfulFrames_ = 0;
    }

    void Shutdown() {
        if (device_ && module_) {
            ReleaseAll();
            HMODULE core = FindNgxCoreModule();
            auto destroy = core ? reinterpret_cast<DestroyParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters")) : nullptr;
            for (uint32_t s = 0; s < kMaxSlots; ++s) {
                if (parameters_[s] && destroy) destroy(parameters_[s]);
                parameters_[s] = nullptr;
            }
            auto shutdown = reinterpret_cast<ShutdownD3D12>(GetProcAddress(module_, "NVSDK_NGX_D3D12_Shutdown1"));
            SignedRuntimePathScope scope(module_, path_.parent_path() / L"nvngx.dll");
            if (shutdown) shutdown(device_);
            device_->Release();
            device_ = nullptr;
        }
        if (module_) FreeLibrary(module_);
        module_ = nullptr;
        status_ = RuntimeStatus::NotProbed;
        path_.clear();
        version_.clear();
        detail_.clear();
        ngxResult_ = applicationId_ = apiVersion_ = 0;
        successfulFrames_ = 0;
    }

    const char* GetStatusString() const { return StatusToString(status_); }
    const char* GetDetailString() const { return detail_.c_str(); }
    const char* GetVersionString() const { return version_.c_str(); }
    uint32_t    GetLastNgxResult() const { return ngxResult_; }
    uint64_t    GetSuccessfulFrames() const { return successfulFrames_; }

private:
    HMODULE module_ = nullptr;
    NVSDK_NGX_Parameter* parameters_[kMaxSlots]{};
    NVSDK_NGX_Handle* featureHandles_[kMaxSlots]{};
    uint32_t featureInputWidth_[kMaxSlots]{};
    uint32_t featureInputHeight_[kMaxSlots]{};
    uint32_t featureOutputWidth_[kMaxSlots]{};
    uint32_t featureOutputHeight_[kMaxSlots]{};
    uint32_t featureQuality_[kMaxSlots]{ UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    uint32_t featurePreset_[kMaxSlots]{ UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    ID3D12Device* device_ = nullptr;
    RuntimeStatus status_ = RuntimeStatus::NotProbed;
    std::filesystem::path path_;
    std::string version_;
    std::string detail_;
    uint32_t ngxResult_ = 0;
    uint32_t applicationId_ = 0;
    uint32_t apiVersion_ = 0;
    uint64_t successfulFrames_ = 0;
};

BridgeState g_state;

bool __cdecl Api_Probe(const wchar_t* dir) { return g_state.Probe(dir); }
bool __cdecl Api_Initialize(ID3D12Device* dev, const wchar_t* dataDir) { return g_state.Initialize(dev, dataDir); }
bool __cdecl Api_Execute(const Dlss5EvalDesc* desc) { return g_state.Execute(desc); }
void __cdecl Api_ReleaseSlot(uint32_t slot) { g_state.ReleaseSlot(slot); }
void __cdecl Api_ReleaseAll() { g_state.ReleaseAll(); }
void __cdecl Api_Shutdown() { g_state.Shutdown(); }
const char* __cdecl Api_GetStatusString() { return g_state.GetStatusString(); }
const char* __cdecl Api_GetDetailString() { return g_state.GetDetailString(); }
const char* __cdecl Api_GetVersionString() { return g_state.GetVersionString(); }
uint32_t __cdecl Api_GetLastNgxResult() { return g_state.GetLastNgxResult(); }
uint64_t __cdecl Api_GetSuccessfulFrames() { return g_state.GetSuccessfulFrames(); }

const Dlss5BridgeApi g_api = {
    kDlss5BridgeApiVersion,
    &Api_Probe,
    &Api_Initialize,
    &Api_Execute,
    &Api_ReleaseSlot,
    &Api_ReleaseAll,
    &Api_Shutdown,
    &Api_GetStatusString,
    &Api_GetDetailString,
    &Api_GetVersionString,
    &Api_GetLastNgxResult,
    &Api_GetSuccessfulFrames,
};

} // namespace

extern "C" __declspec(dllexport) const Dlss5BridgeApi* __cdecl ITR2_Dlss5Bridge_GetApi() {
    return &g_api;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
