// The few NGX types the bridge uses, so it builds without the NGX SDK.
#pragma once
#include <cstdint>

struct ID3D11Resource;
struct ID3D12Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

#define NVSDK_CONV __cdecl

enum NVSDK_NGX_Result : uint32_t {
    NVSDK_NGX_Result_Success = 0x1u,
    NVSDK_NGX_Result_Fail    = 0xBAD00000u,
};

enum NVSDK_NGX_Feature : uint32_t {
    NVSDK_NGX_Feature_SuperSampling = 1u,
    NVSDK_NGX_Feature_DlssNr        = 18u,
};

enum NVSDK_NGX_Version : uint32_t {
    NVSDK_NGX_Version_API = 0x0000015u,
};

struct NVSDK_NGX_Handle { uint64_t InternalId; };

using PFN_NVSDK_NGX_ProgressCallback = void(NVSDK_CONV*)(float InCurrentProgress, bool& OutShouldCancel);

// same virtual function order as NVSDK_NGX_Parameter in the NGX runtime
struct NVSDK_NGX_Parameter {
    virtual void Set(const char* InName, unsigned long long InValue) = 0;
    virtual void Set(const char* InName, float InValue) = 0;
    virtual void Set(const char* InName, double InValue) = 0;
    virtual void Set(const char* InName, unsigned int InValue) = 0;
    virtual void Set(const char* InName, int InValue) = 0;
    virtual void Set(const char* InName, ID3D11Resource* InValue) = 0;
    virtual void Set(const char* InName, ID3D12Resource* InValue) = 0;
    virtual void Set(const char* InName, void* InValue) = 0;

    virtual NVSDK_NGX_Result Get(const char* InName, unsigned long long* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, float* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, double* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, unsigned int* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, int* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, ID3D11Resource** OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, ID3D12Resource** OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, void** OutValue) const = 0;

    virtual void Reset() = 0;
};
