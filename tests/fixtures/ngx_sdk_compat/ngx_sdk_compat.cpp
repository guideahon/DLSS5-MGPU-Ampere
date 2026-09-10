// Build-only compatibility layer for the public NGX parameter interface.
// It replaces helper entry points normally supplied by nvsdk_ngx*.lib while
// leaving all feature/runtime calls in nvngx_dlss.dll.

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>

namespace
{
    thread_local PFN_NVSDK_NGX_ProgressCallback_C g_progressCallback = nullptr;

    void NVSDK_CONV CompatProgressCallback(float progress, bool& shouldCancel)
    {
        bool cancel = false;
        if (g_progressCallback)
            g_progressCallback(progress, &cancel);
        shouldCancel = cancel;
    }
}

extern "C"
{
    void NVSDK_CONV NVSDK_NGX_Parameter_SetULL(NVSDK_NGX_Parameter* p, const char* name, unsigned long long value)
    { if (p) p->Set(name, value); }
    void NVSDK_CONV NVSDK_NGX_Parameter_SetF(NVSDK_NGX_Parameter* p, const char* name, float value)
    { if (p) p->Set(name, value); }
    void NVSDK_CONV NVSDK_NGX_Parameter_SetD(NVSDK_NGX_Parameter* p, const char* name, double value)
    { if (p) p->Set(name, value); }
    void NVSDK_CONV NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter* p, const char* name, unsigned int value)
    { if (p) p->Set(name, value); }
    void NVSDK_CONV NVSDK_NGX_Parameter_SetI(NVSDK_NGX_Parameter* p, const char* name, int value)
    { if (p) p->Set(name, value); }
    void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d11Resource(NVSDK_NGX_Parameter* p, const char* name, ID3D11Resource* value)
    { if (p) p->Set(name, value); }
    void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d12Resource(NVSDK_NGX_Parameter* p, const char* name, ID3D12Resource* value)
    { if (p) p->Set(name, value); }
    void NVSDK_CONV NVSDK_NGX_Parameter_SetVoidPointer(NVSDK_NGX_Parameter* p, const char* name, void* value)
    { if (p) p->Set(name, value); }

    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_Parameter_GetULL(NVSDK_NGX_Parameter* p, const char* name, unsigned long long* value)
    { return p ? p->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_Parameter_GetF(NVSDK_NGX_Parameter* p, const char* name, float* value)
    { return p ? p->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_Parameter_GetD(NVSDK_NGX_Parameter* p, const char* name, double* value)
    { return p ? p->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_Parameter_GetUI(NVSDK_NGX_Parameter* p, const char* name, unsigned int* value)
    { return p ? p->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_Parameter_GetI(NVSDK_NGX_Parameter* p, const char* name, int* value)
    { return p ? p->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_Parameter_GetD3d11Resource(NVSDK_NGX_Parameter* p, const char* name, ID3D11Resource** value)
    { return p ? p->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_Parameter_GetD3d12Resource(NVSDK_NGX_Parameter* p, const char* name, ID3D12Resource** value)
    { return p ? p->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_Parameter_GetVoidPointer(NVSDK_NGX_Parameter* p, const char* name, void** value)
    { return p ? p->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter; }

    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_DestroyParameters(NVSDK_NGX_Parameter* p)
    {
        // NGX owns the concrete implementation; do not delete through this
        // interface because its public type has no virtual destructor.
        if (p) p->Reset();
        return NVSDK_NGX_Result_Success;
    }

    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_UpdateFeature(const NVSDK_NGX_Application_Identifier*, const NVSDK_NGX_Feature)
    { return NVSDK_NGX_Result_Success; }

    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D11_EvaluateFeature_C(ID3D11DeviceContext* context, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback_C callback)
    {
        g_progressCallback = callback;
        const auto result = NVSDK_NGX_D3D11_EvaluateFeature(context, handle, params, callback ? CompatProgressCallback : nullptr);
        g_progressCallback = nullptr;
        return result;
    }

    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList* commandList, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback_C callback)
    {
        g_progressCallback = callback;
        const auto result = NVSDK_NGX_D3D12_EvaluateFeature(commandList, handle, params, callback ? CompatProgressCallback : nullptr);
        g_progressCallback = nullptr;
        return result;
    }

    NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_CUDA_EvaluateFeature_C(const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback_C callback)
    {
        g_progressCallback = callback;
        const auto result = NVSDK_NGX_CUDA_EvaluateFeature(handle, params, callback ? CompatProgressCallback : nullptr);
        g_progressCallback = nullptr;
        return result;
    }

    const wchar_t* NVSDK_CONV GetNGXResultAsString(NVSDK_NGX_Result)
    { return L"NGX result (compatibility shim)"; }
}
