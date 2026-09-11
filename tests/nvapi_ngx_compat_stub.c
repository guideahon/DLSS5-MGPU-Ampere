/*
 * Diagnostic-only NVAPI surface for the NGX loader experiments.
 *
 * This is not an NVAPI implementation and must never be installed globally.
 * It exists to answer one narrow question: does the NGX core progress when
 * the NVAPI queries it makes are backed by structurally valid Ampere data?
 * Build with scripts/build_nvapi_ngx_compat_stub.sh and pass the resulting
 * DLL through MGPU_NGX_COMPAT_DLL_DIR in the Wine smoke runner.
 */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef uint32_t NvU32;
typedef int32_t NvStatus;
typedef void *NvPhysicalGpuHandle;
typedef void *NvLogicalGpuHandle;
typedef void *NvDRSSessionHandle;
typedef void *NvDRSProfileHandle;
typedef char NvAPI_ShortString[64];

#define NVAPI_OK 0
#define NVAPI_MAX_PHYSICAL_GPUS 64
#define NV_GPU_ARCHITECTURE_GA100 0x00000170u
#define NV_GPU_ARCHITECTURE_AD100 0x00000190u
#define NV_GPU_ARCHITECTURE_GB200 0x000001b0u
#define NV_GPU_ARCH_IMPLEMENTATION_GA102 0x00000002u
#define NV_GPU_ARCH_IMPLEMENTATION_AD102 0x00000002u
#define NV_GPU_ARCH_IMPLEMENTATION_GB202 0x00000002u
#define NV_GPU_CHIP_REV_UNKNOWN 0xffffffffu
#define NV_GPU_ARCH_INFO_VER_1 ((1u << 16) | sizeof(NV_GPU_ARCH_INFO_V1))
#define NV_GPU_ARCH_INFO_VER_2 ((2u << 16) | sizeof(NV_GPU_ARCH_INFO_V2))
#define NV_LOGICAL_GPU_DATA_VER1 ((1u << 16) | sizeof(NV_LOGICAL_GPU_DATA_V1))

typedef struct {
    NvU32 version;
    NvU32 architecture;
    NvU32 implementation;
    NvU32 revision;
} NV_GPU_ARCH_INFO_V1;

typedef struct {
    NvU32 version;
    NvU32 architecture_id;
    NvU32 implementation_id;
    NvU32 revision_id;
} NV_GPU_ARCH_INFO_V2;

typedef struct {
    NvU32 version;
    void *pOSAdapterId;
    NvU32 physicalGpuCount;
    NvPhysicalGpuHandle physicalGpuHandles[NVAPI_MAX_PHYSICAL_GPUS];
    NvU32 reserved[8];
} NV_LOGICAL_GPU_DATA_V1;

static const NvPhysicalGpuHandle k_gpu = (NvPhysicalGpuHandle)(uintptr_t)0x1000;
static const NvLogicalGpuHandle k_logical = (NvLogicalGpuHandle)(uintptr_t)0x1000;
static const NvDRSSessionHandle k_drs_session = (NvDRSSessionHandle)(uintptr_t)0x2000;
static const NvDRSProfileHandle k_drs_profile = (NvDRSProfileHandle)(uintptr_t)0x3000;

static void get_declared_arch(NvU32 *architecture, NvU32 *implementation) {
    const char *mode = getenv("MGPU_NVAPI_STUB_ARCH");
    *architecture = NV_GPU_ARCHITECTURE_GA100;
    *implementation = NV_GPU_ARCH_IMPLEMENTATION_GA102;
    if (mode && strcmp(mode, "ad100") == 0) {
        *architecture = NV_GPU_ARCHITECTURE_AD100;
        *implementation = NV_GPU_ARCH_IMPLEMENTATION_AD102;
    } else if (mode && strcmp(mode, "gb200") == 0) {
        *architecture = NV_GPU_ARCHITECTURE_GB200;
        *implementation = NV_GPU_ARCH_IMPLEMENTATION_GB202;
    }
}

static void log_line(const char *fmt, ...) {
    FILE *f = fopen("C:\\nvapi_probe.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        log_line("compat_stub attach");
    }
    return TRUE;
}

static int __cdecl nvapi_initialize(void) {
    log_line("NvAPI_Initialize");
    return NVAPI_OK;
}

static int __cdecl nvapi_unload(void) {
    log_line("NvAPI_Unload");
    return NVAPI_OK;
}

static int __cdecl enum_physical(NvPhysicalGpuHandle *handles, NvU32 *count) {
    log_line("NvAPI_EnumPhysicalGPUs handles=%p count=%p", handles, count);
    if (handles) handles[0] = k_gpu;
    if (count) *count = 1;
    return NVAPI_OK;
}

static int __cdecl get_arch_info(NvPhysicalGpuHandle gpu, void *info) {
    NvU32 *raw = (NvU32 *)info;
    NvU32 architecture = 0;
    NvU32 implementation = 0;
    get_declared_arch(&architecture, &implementation);
    log_line("NvAPI_GPU_GetArchInfo gpu=%p info=%p version=0x%08x", gpu, info,
             raw ? raw[0] : 0u);
    if (!info) return -1;
    switch (raw[0]) {
    case NV_GPU_ARCH_INFO_VER_1: {
        NV_GPU_ARCH_INFO_V1 *v1 = (NV_GPU_ARCH_INFO_V1 *)info;
        v1->architecture = architecture;
        v1->implementation = implementation;
        v1->revision = NV_GPU_CHIP_REV_UNKNOWN;
        return NVAPI_OK;
    }
    case NV_GPU_ARCH_INFO_VER_2: {
        NV_GPU_ARCH_INFO_V2 *v2 = (NV_GPU_ARCH_INFO_V2 *)info;
        v2->architecture_id = architecture;
        v2->implementation_id = implementation;
        v2->revision_id = NV_GPU_CHIP_REV_UNKNOWN;
        return NVAPI_OK;
    }
    default:
        /* The shipped NGX core asks for a newer 32-byte revision (0x20010).
         * Its first three data fields retain the documented ABI ordering. */
        if ((raw[0] & 0xffff0000u) == 0x00020000u) {
            raw[1] = architecture;
            raw[2] = implementation;
            raw[3] = NV_GPU_CHIP_REV_UNKNOWN;
            return NVAPI_OK;
        }
        return -9;
    }
}

static int __cdecl get_logical_from_physical(NvPhysicalGpuHandle gpu,
                                               NvLogicalGpuHandle *logical) {
    log_line("NvAPI_GetLogicalGPUFromPhysicalGPU gpu=%p logical=%p", gpu, logical);
    if (!logical) return -1;
    *logical = k_logical;
    return NVAPI_OK;
}

static int __cdecl get_logical_info(NvLogicalGpuHandle logical, void *data) {
    NV_LOGICAL_GPU_DATA_V1 *info = (NV_LOGICAL_GPU_DATA_V1 *)data;
    log_line("NvAPI_GPU_GetLogicalGpuInfo logical=%p data=%p version=0x%08x expected=0x%08x size=%zu",
             logical, data, info ? info->version : 0u,
             NV_LOGICAL_GPU_DATA_VER1, sizeof(NV_LOGICAL_GPU_DATA_V1));
    if (!info || info->version != NV_LOGICAL_GPU_DATA_VER1) return -9;
    if (info->pOSAdapterId) {
        /* LUID-sized storage is all NGX needs from this diagnostic path. */
        memset(info->pOSAdapterId, 0, 8);
    }
    info->physicalGpuCount = 1;
    info->physicalGpuHandles[0] = k_gpu;
    memset(info->reserved, 0, sizeof(info->reserved));
    log_line("NvAPI_GPU_GetLogicalGpuInfo success os_adapter=%p physical_count=%u",
             info->pOSAdapterId, info->physicalGpuCount);
    return NVAPI_OK;
}

static int __cdecl get_driver_version(NvU32 *version, NvAPI_ShortString branch) {
    log_line("NvAPI_SYS_GetDriverAndBranchVersion version=%p branch=%p", version,
             branch);
    if (version) *version = 59571;
    if (branch) strcpy(branch, "r595_71");
    return NVAPI_OK;
}

static int __cdecl drs_create_session(NvDRSSessionHandle *session) {
    log_line("NvAPI_DRS_CreateSession session=%p", session);
    if (session) *session = k_drs_session;
    return NVAPI_OK;
}

static int __cdecl drs_get_base_profile(NvDRSSessionHandle session,
                                         NvDRSProfileHandle *profile) {
    log_line("NvAPI_DRS_GetBaseProfile session=%p profile=%p", session, profile);
    if (profile) *profile = k_drs_profile;
    return NVAPI_OK;
}

static int __cdecl status_ok(void) {
    return NVAPI_OK;
}

__declspec(dllexport) void * __cdecl nvapi_QueryInterface(NvU32 id) {
    log_line("query 0x%08x", id);
    switch (id) {
    case 0x0150e828: return nvapi_initialize;
    case 0xd22bdd7e: return nvapi_unload;
    case 0xe5ac921f: return enum_physical;
    case 0xd8265d24: return get_arch_info;
    case 0xadd604d1: return get_logical_from_physical;
    case 0x842b066e: return get_logical_info;
    case 0x2926aaad: return get_driver_version;
    case 0x0694d52e: return drs_create_session;
    case 0xda8466a0: return drs_get_base_profile;
    default: return status_ok;
    }
}

__declspec(dllexport) void * __cdecl nvapi_pepQueryInterface(NvU32 id) {
    log_line("pep_query 0x%08x", id);
    return nvapi_QueryInterface(id);
}
