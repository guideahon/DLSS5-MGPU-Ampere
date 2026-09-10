#include <cuda.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static void log_cuda(const char* label, CUresult result) {
    const char* name = nullptr;
    const char* text = nullptr;
    cuGetErrorName(result, &name);
    cuGetErrorString(result, &text);
    std::fprintf(stderr, "%s: rc=%d name=%s text=%s\n", label, static_cast<int>(result),
                 name ? name : "?", text ? text : "?");
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: %s <fd> <size> <cuda-source> <cuda-destination>\n", argv[0]);
        return 2;
    }
    const int fd = std::atoi(argv[1]);
    const unsigned long long size = std::strtoull(argv[2], nullptr, 10);
    const int source_ordinal = std::atoi(argv[3]);
    const int destination_ordinal = std::atoi(argv[4]);
    std::fprintf(stderr, "CUDA readback helper: fd=%d size=%llu source=%d destination=%d\n",
                 fd, size, source_ordinal, destination_ordinal);
    struct stat fd_stat{};
    if (fstat(fd, &fd_stat) == 0)
        std::fprintf(stderr, "cuda_readback_fd_kind=%s\n", S_ISCHR(fd_stat.st_mode) ? "char" : "other");
    else
        std::fprintf(stderr, "cuda_readback_fstat_errno=%d\n", errno);

    CUresult result = cuInit(0);
    if (result != CUDA_SUCCESS) { log_cuda("cuInit", result); return 3; }
    CUdevice source_device = -1;
    result = cuDeviceGet(&source_device, source_ordinal);
    if (result != CUDA_SUCCESS) { log_cuda("cuDeviceGet(source)", result); return 4; }
    CUcontext source_context = nullptr;
    result = cuCtxCreate(&source_context, 0, source_device);
    if (result != CUDA_SUCCESS) { log_cuda("cuCtxCreate(source)", result); return 5; }

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC import_desc{};
    import_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    import_desc.handle.fd = fd;
    import_desc.size = size;
    CUexternalMemory imported = nullptr;
    result = cuImportExternalMemory(&imported, &import_desc);
    log_cuda("cuImportExternalMemory(readback)", result);
    if (result != CUDA_SUCCESS) {
        close(fd);
        cuCtxDestroy(source_context);
        return 6;
    }
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer_desc{};
    buffer_desc.size = size;
    CUdeviceptr source_buffer = 0;
    result = cuExternalMemoryGetMappedBuffer(&source_buffer, imported, &buffer_desc);
    log_cuda("cuExternalMemoryGetMappedBuffer(readback)", result);
    if (result != CUDA_SUCCESS || source_buffer == 0) {
        cuDestroyExternalMemory(imported);
        cuCtxDestroy(source_context);
        return 7;
    }

    CUdevice destination_device = -1;
    CUcontext destination_context = nullptr;
    CUdeviceptr destination_buffer = 0;
    result = cuDeviceGet(&destination_device, destination_ordinal);
    if (result == CUDA_SUCCESS)
        result = cuCtxCreate(&destination_context, 0, destination_device);
    if (result == CUDA_SUCCESS)
        result = cuMemAlloc(&destination_buffer, size);
    log_cuda("cuMemAlloc(destination)", result);
    if (result == CUDA_SUCCESS)
        result = cuMemcpyPeer(destination_buffer, destination_context,
                              source_buffer, source_context, size);
    log_cuda("cuMemcpyPeer(readback->destination)", result);

    unsigned char first[8]{};
    if (result == CUDA_SUCCESS) {
        result = cuMemcpyDtoH(first, destination_buffer, sizeof(first));
        log_cuda("cuMemcpyDtoH(first_pixel)", result);
    }
    std::fprintf(stderr, "cuda_readback_first_pixel=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                 first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7]);
    const unsigned char expected[8] = {0x00, 0x34, 0x00, 0x38, 0x00, 0x3a, 0x00, 0x3c};
    const bool valid = result == CUDA_SUCCESS && std::memcmp(first, expected, sizeof(first)) == 0;
    std::fprintf(stderr, "cuda_readback_validation=%s\n", valid ? "ok" : "FAIL");

    if (destination_buffer) {
        cuCtxSetCurrent(destination_context);
        cuMemFree(destination_buffer);
    }
    if (destination_context) cuCtxDestroy(destination_context);
    cuCtxSetCurrent(source_context);
    cuMemFree(source_buffer);
    cuDestroyExternalMemory(imported);
    cuCtxDestroy(source_context);
    return valid ? 0 : 8;
}
