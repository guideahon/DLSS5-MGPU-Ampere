#include <cuda.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s <fd> <size> <cuda-source> [cuda-destination]\n", argv[0]);
        return 2;
    }

    int fd = atoi(argv[1]);
    unsigned long long size = strtoull(argv[2], nullptr, 10);
    int ordinal = atoi(argv[3]);
    int destination_ordinal = argc == 5 ? atoi(argv[4]) : -1;
    fprintf(stderr, "CUDA helper: fd=%d size=%llu source=%d destination=%d\n",
            fd, size, ordinal, destination_ordinal);

    struct stat fd_stat{};
    if (fstat(fd, &fd_stat) == 0) {
        fprintf(stderr, "CUDA helper fd_kind=%s mode=0%o\n",
                S_ISREG(fd_stat.st_mode) ? "regular" :
                S_ISCHR(fd_stat.st_mode) ? "char" :
                S_ISFIFO(fd_stat.st_mode) ? "fifo" :
                S_ISSOCK(fd_stat.st_mode) ? "socket" :
                S_ISDIR(fd_stat.st_mode) ? "dir" : "other",
                fd_stat.st_mode & 07777);
    } else {
        fprintf(stderr, "CUDA helper fstat errno=%d\n", errno);
    }

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
        close(fd);
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
    if (rc != CUDA_SUCCESS || !mapped) {
        cuDestroyExternalMemory(external_memory);
        cuCtxDestroy(context);
        return 7;
    }

    rc = cuMemsetD8(mapped, 0xA5, size);
    log_cuda("cuMemsetD8(imported)", rc);

    CUcontext destination_context = nullptr;
    CUdeviceptr destination = 0;
    if (rc == CUDA_SUCCESS && destination_ordinal >= 0) {
        CUdevice destination_device = -1;
        rc = cuDeviceGet(&destination_device, destination_ordinal);
        log_cuda("cuDeviceGet(destination)", rc);
        if (rc == CUDA_SUCCESS)
            rc = cuCtxCreate(&destination_context, 0, destination_device);
        log_cuda("cuCtxCreate(destination)", rc);
        if (rc == CUDA_SUCCESS)
            rc = cuMemAlloc(&destination, size);
        log_cuda("cuMemAlloc(destination)", rc);
        if (rc == CUDA_SUCCESS)
            rc = cuMemcpyPeer(destination, destination_context, mapped, context, size);
        log_cuda("cuMemcpyPeer(imported->destination)", rc);
        if (rc == CUDA_SUCCESS) {
            unsigned char *host = (unsigned char *)malloc(size);
            int valid = 0;
            if (host) {
                rc = cuMemcpyDtoH(host, destination, size);
                log_cuda("cuMemcpyDtoH(validation)", rc);
                if (rc == CUDA_SUCCESS) {
                    valid = 1;
                    for (unsigned long long i = 0; i < size; ++i) {
                        if (host[i] != 0xA5) { valid = 0; break; }
                    }
                }
                fprintf(stderr, "cuda_helper_p2p_validation=%s\n",
                        valid ? "ok" : "FAIL");
                free(host);
                if (!valid && rc == CUDA_SUCCESS)
                    rc = CUDA_ERROR_UNKNOWN;
            } else {
                rc = CUDA_ERROR_OUT_OF_MEMORY;
            }
        }
    }
    if (destination) {
        cuCtxSetCurrent(destination_context);
        cuMemFree(destination);
    }
    if (destination_context)
        cuCtxDestroy(destination_context);
    cuCtxSetCurrent(context);
    cuMemFree(mapped);
    cuDestroyExternalMemory(external_memory);
    cuCtxDestroy(context);
    return rc == CUDA_SUCCESS ? 0 : 7;
}
