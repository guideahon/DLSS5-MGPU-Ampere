#include <cuda.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static void log_cuda(const char *label, CUresult result)
{
    const char *name = nullptr;
    const char *text = nullptr;
    cuGetErrorName(result, &name);
    cuGetErrorString(result, &text);
    fprintf(stderr, "%s: rc=%d name=%s text=%s\n", label, (int)result,
            name ? name : "?", text ? text : "?");
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <fd> <size> <cuda-ordinal>\n", argv[0]);
        return 2;
    }

    int fd = atoi(argv[1]);
    unsigned long long size = strtoull(argv[2], nullptr, 10);
    int ordinal = atoi(argv[3]);
    fprintf(stderr, "CUDA helper: fd=%d size=%llu ordinal=%d\n", fd, size, ordinal);

    CUresult rc = cuInit(0);
    if (rc != CUDA_SUCCESS) { log_cuda("cuInit", rc); return 3; }
    CUdevice device = -1;
    rc = cuDeviceGet(&device, ordinal);
    if (rc != CUDA_SUCCESS) { log_cuda("cuDeviceGet", rc); return 4; }
    CUcontext context = nullptr;
    rc = cuCtxCreate(&context, 0, device);
    if (rc != CUDA_SUCCESS) { log_cuda("cuCtxCreate", rc); return 5; }

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC import_desc{};
    import_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    import_desc.handle.fd = fd;
    import_desc.size = size;
    CUexternalMemory external_memory = nullptr;
    rc = cuImportExternalMemory(&external_memory, &import_desc);
    log_cuda("cuImportExternalMemory", rc);
    if (rc != CUDA_SUCCESS) {
        cuCtxDestroy(context);
        return 6;
    }

    CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer_desc{};
    buffer_desc.offset = 0;
    buffer_desc.size = size;
    CUdeviceptr mapped = 0;
    rc = cuExternalMemoryGetMappedBuffer(&mapped, external_memory, &buffer_desc);
    log_cuda("cuExternalMemoryGetMappedBuffer", rc);
    fprintf(stderr, "cuda_helper_imported=%s mapped=0x%llx\n",
            rc == CUDA_SUCCESS && mapped ? "yes" : "no",
            (unsigned long long)mapped);
    if (mapped) cuMemFree(mapped);
    cuDestroyExternalMemory(external_memory);
    cuCtxDestroy(context);
    return rc == CUDA_SUCCESS && mapped ? 0 : 7;
}
