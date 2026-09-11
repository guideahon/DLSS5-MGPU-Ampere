#include <cuda.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned long long fnv1a(const unsigned char *bytes, unsigned long long size)
{
    unsigned long long hash = 1469598103934665603ULL;
    for (unsigned long long i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static FILE *g_log_file = nullptr;

static void log_init()
{
    const char *path = getenv("MGPU_CUDA_HELPER_LOG");
    if (path && *path)
        g_log_file = fopen(path, "a");
}

static void log_close()
{
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = nullptr;
    }
}

#define LOGF(...) do { \
    fprintf(stderr, __VA_ARGS__); \
    if (g_log_file) { \
        fprintf(g_log_file, __VA_ARGS__); \
        fflush(g_log_file); \
    } \
} while (0)

static void log_cuda(const char *label, CUresult result)
{
    const char *name = nullptr;
    const char *text = nullptr;
    cuGetErrorName(result, &name);
    cuGetErrorString(result, &text);
    LOGF("%s: rc=%d name=%s text=%s\n", label, (int)result,
            name ? name : "?", text ? text : "?");
}

int main(int argc, char **argv)
{
    log_init();
    if (argc < 4 || argc > 7) {
        LOGF("usage: %s <fd> <size> <cuda-source> [cuda-destination] [offset] [readonly]\n", argv[0]);
        log_close();
        return 2;
    }

    int fd = atoi(argv[1]);
    unsigned long long size = strtoull(argv[2], nullptr, 10);
    int ordinal = atoi(argv[3]);
    int destination_ordinal = argc >= 5 ? atoi(argv[4]) : -1;
    unsigned long long offset = 0;
    int readonly = getenv("MGPU_CUDA_IMPORT_READONLY") &&
                   strcmp(getenv("MGPU_CUDA_IMPORT_READONLY"), "1") == 0;
    if (argc >= 6) {
        if (strcmp(argv[5], "readonly") == 0)
            readonly = 1;
        else
            offset = strtoull(argv[5], nullptr, 10);
    }
    if (argc >= 7 && strcmp(argv[6], "readonly") == 0)
        readonly = 1;
    if (offset >= size) {
        LOGF("CUDA helper invalid offset=%llu size=%llu\n", offset, size);
        log_close();
        return 2;
    }
    const unsigned long long transfer_size = size - offset;
    LOGF("CUDA helper: fd=%d size=%llu source=%d destination=%d\n",
            fd, size, ordinal, destination_ordinal);
    LOGF("CUDA helper offset=%llu transfer_size=%llu readonly=%s\n",
            offset, transfer_size, readonly ? "yes" : "no");

    struct stat fd_stat{};
    if (fstat(fd, &fd_stat) == 0) {
        LOGF("CUDA helper fd_kind=%s mode=0%o\n",
                S_ISREG(fd_stat.st_mode) ? "regular" :
                S_ISCHR(fd_stat.st_mode) ? "char" :
                S_ISFIFO(fd_stat.st_mode) ? "fifo" :
                S_ISSOCK(fd_stat.st_mode) ? "socket" :
                S_ISDIR(fd_stat.st_mode) ? "dir" : "other",
                fd_stat.st_mode & 07777);
    } else {
        LOGF("CUDA helper fstat errno=%d\n", errno);
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
    buffer_desc.offset = offset;
    buffer_desc.size = transfer_size;
    CUdeviceptr mapped = 0;
    rc = cuExternalMemoryGetMappedBuffer(&mapped, external_memory, &buffer_desc);
    log_cuda("cuExternalMemoryGetMappedBuffer", rc);
    LOGF("cuda_helper_imported=%s mapped=0x%llx\n",
            rc == CUDA_SUCCESS && mapped ? "yes" : "no",
            (unsigned long long)mapped);
    if (rc != CUDA_SUCCESS || !mapped) {
        cuDestroyExternalMemory(external_memory);
        cuCtxDestroy(context);
        return 7;
    }

    if (!readonly) {
        rc = cuMemsetD8(mapped, 0xA5, transfer_size);
        log_cuda("cuMemsetD8(imported)", rc);
    } else {
        LOGF("cuda_helper_imported_readonly=yes\n");
    }

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
            rc = cuMemAlloc(&destination, transfer_size);
        log_cuda("cuMemAlloc(destination)", rc);
        if (rc == CUDA_SUCCESS)
            rc = cuMemcpyPeer(destination, destination_context, mapped, context, transfer_size);
        log_cuda("cuMemcpyPeer(imported->destination)", rc);
        if (rc == CUDA_SUCCESS) {
            unsigned char *host = (unsigned char *)malloc(transfer_size);
            int valid = 0;
            if (host) {
                rc = cuMemcpyDtoH(host, destination, transfer_size);
                log_cuda("cuMemcpyDtoH(validation)", rc);
                if (rc == CUDA_SUCCESS) {
                    valid = 1;
                    if (!readonly) {
                        for (unsigned long long i = 0; i < transfer_size; ++i)
                            if (host[i] != 0xA5) { valid = 0; break; }
                    } else {
                        unsigned char *source_host = (unsigned char *)malloc(transfer_size);
                        if (source_host) {
                            cuCtxSetCurrent(context);
                            rc = cuMemcpyDtoH(source_host, mapped, transfer_size);
                            valid = rc == CUDA_SUCCESS &&
                                    memcmp(source_host, host, transfer_size) == 0;
                            LOGF("cuda_helper_readonly_validation=%s source_fnv1a=0x%016llx destination_fnv1a=0x%016llx\n",
                                 valid ? "ok" : "FAIL",
                                 fnv1a(source_host, transfer_size),
                                 fnv1a(host, transfer_size));
                            free(source_host);
                        }
                    }
                }
                LOGF("cuda_helper_p2p_validation=%s\n",
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
    int exit_code = rc == CUDA_SUCCESS ? 0 : 7;
    log_close();
    return exit_code;
}
