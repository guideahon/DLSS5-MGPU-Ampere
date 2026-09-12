#include <windows.h>
#include <stdio.h>

static void check_export(HMODULE module, const char *name) {
    FARPROC symbol = GetProcAddress(module, name);
    printf("export %-42s %s\n", name, symbol != NULL ? "ok" : "missing");
}

int main(void) {
    HMODULE proton_core = LoadLibraryW(L"_nvngx.dll");
    printf("load _nvngx.dll: %s error=%lu\n", proton_core != NULL ? "ok" : "failed", GetLastError());
    if (proton_core != NULL) {
        check_export(proton_core, "NVSDK_NGX_D3D12_Init_Ext");
        check_export(proton_core, "NVSDK_NGX_D3D12_CreateFeature");
        check_export(proton_core, "NVSDK_NGX_D3D12_EvaluateFeature");
    }

    HMODULE proxy = LoadLibraryW(L"nvngx_dlss.dll");
    printf("load nvngx_dlss.dll: %s error=%lu\n", proxy != NULL ? "ok" : "failed", GetLastError());
    if (proxy != NULL) {
        check_export(proxy, "NVSDK_NGX_D3D12_Init_Ext");
        check_export(proxy, "NVSDK_NGX_D3D12_CreateFeature");
        check_export(proxy, "NVSDK_NGX_D3D12_EvaluateFeature");
        check_export(proxy, "NVSDK_NGX_D3D12_GetFeatureRequirements");
    }

    HMODULE bridge = LoadLibraryW(L"bridge-nvngx.dll");
    printf("load bridge-nvngx.dll: %s error=%lu\n", bridge != NULL ? "ok" : "failed", GetLastError());
    if (bridge != NULL) {
        check_export(bridge, "Bridge_DLSS_Init");
        check_export(bridge, "Bridge_Init");
    }

    return (proton_core != NULL && proxy != NULL && bridge != NULL) ? 0 : 1;
}
