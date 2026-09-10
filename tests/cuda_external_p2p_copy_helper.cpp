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
    std::fprintf(stderr, "%s: rc=%d name=%s text=%s\n", label,
                 static_cast<int>(result), name ? name : "?", text ? text : "?");
}

static bool import_buffer(int fd, unsigned long long heap_size,
                          unsigned long long offset, unsigned long long size,
                          CUcontext context, CUexternalMemory* external,
                          CUdeviceptr* mapped) {
    if (cuCtxSetCurrent(context) != CUDA_SUCCESS) return false;
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC import_desc{};
    import_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    import_desc.handle.fd = fd;
    import_desc.size = heap_size;
    CUresult result = cuImportExternalMemory(external, &import_desc);
    log_cuda("cuImportExternalMemory", result);
    if (result != CUDA_SUCCESS) return false;
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer_desc{};
    buffer_desc.offset = offset;
    buffer_desc.size = size;
    result = cuExternalMemoryGetMappedBuffer(mapped, *external, &buffer_desc);
    log_cuda("cuExternalMemoryGetMappedBuffer", result);
    return result == CUDA_SUCCESS && *mapped != 0;
}

static unsigned long long fnv1a(const unsigned char* bytes, unsigned long long size) {
    unsigned long long hash = 1469598103934665603ULL;
    for (unsigned long long i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

int main(int argc, char** argv) {
    if (argc != 11) {
        std::fprintf(stderr,
            "usage: %s <source-fd> <source-heap-size> <source-offset> <bytes> "
            "<source-ordinal> <destination-fd> <destination-heap-size> "
            "<destination-offset> <destination-ordinal> <expected-first-byte>\n", argv[0]);
        return 2;
    }
    const int source_fd = std::atoi(argv[1]);
    const unsigned long long source_heap_size = std::strtoull(argv[2], nullptr, 10);
    const unsigned long long source_offset = std::strtoull(argv[3], nullptr, 10);
    const unsigned long long bytes = std::strtoull(argv[4], nullptr, 10);
    const int source_ordinal = std::atoi(argv[5]);
    const int destination_fd = std::atoi(argv[6]);
    const unsigned long long destination_heap_size = std::strtoull(argv[7], nullptr, 10);
    const unsigned long long destination_offset = std::strtoull(argv[8], nullptr, 10);
    const int destination_ordinal = std::atoi(argv[9]);
    const unsigned int expected_first_byte =
        static_cast<unsigned int>(std::strtoul(argv[10], nullptr, 16)) & 0xffU;
    std::fprintf(stderr,
                 "CUDA cross-adapter helper source_fd=%d destination_fd=%d bytes=%llu "
                 "source=%d destination=%d\n", source_fd, destination_fd, bytes,
                 source_ordinal, destination_ordinal);

    struct stat source_stat{};
    struct stat destination_stat{};
    const bool source_fd_ok = fstat(source_fd, &source_stat) == 0;
    const bool destination_fd_ok = fstat(destination_fd, &destination_stat) == 0;
    std::fprintf(stderr, "source_fd_kind=%s destination_fd_kind=%s\n",
                 source_fd_ok && S_ISCHR(source_stat.st_mode) ? "char" : "other",
                 destination_fd_ok && S_ISCHR(destination_stat.st_mode) ? "char" : "other");
    if (!source_fd_ok || !destination_fd_ok) return 3;

    CUresult result = cuInit(0);
    if (result != CUDA_SUCCESS) { log_cuda("cuInit", result); return 4; }
    CUdevice source_device = -1;
    CUdevice destination_device = -1;
    result = cuDeviceGet(&source_device, source_ordinal);
    if (result != CUDA_SUCCESS) { log_cuda("cuDeviceGet(source)", result); return 5; }
    result = cuDeviceGet(&destination_device, destination_ordinal);
    if (result != CUDA_SUCCESS) { log_cuda("cuDeviceGet(destination)", result); return 6; }
    CUcontext source_context = nullptr;
    CUcontext destination_context = nullptr;
    result = cuCtxCreate(&source_context, 0, source_device);
    if (result != CUDA_SUCCESS) { log_cuda("cuCtxCreate(source)", result); return 7; }
    result = cuCtxCreate(&destination_context, 0, destination_device);
    if (result != CUDA_SUCCESS) {
        log_cuda("cuCtxCreate(destination)", result);
        cuCtxDestroy(source_context);
        return 8;
    }

    CUexternalMemory source_external = nullptr;
    CUexternalMemory destination_external = nullptr;
    CUdeviceptr source_buffer = 0;
    CUdeviceptr destination_buffer = 0;
    const bool source_imported = import_buffer(
        source_fd, source_heap_size, source_offset, bytes, source_context,
        &source_external, &source_buffer);
    const bool destination_imported = source_imported && import_buffer(
        destination_fd, destination_heap_size, destination_offset, bytes,
        destination_context, &destination_external, &destination_buffer);
    if (!source_imported || !destination_imported) {
        if (destination_external) cuDestroyExternalMemory(destination_external);
        if (source_external) cuDestroyExternalMemory(source_external);
        cuCtxDestroy(destination_context);
        cuCtxDestroy(source_context);
        return 9;
    }

    result = cuMemcpyPeer(destination_buffer, destination_context,
                          source_buffer, source_context, bytes);
    log_cuda("cuMemcpyPeer(source->destination)", result);
    if (result == CUDA_SUCCESS) {
        cuCtxSetCurrent(destination_context);
        result = cuCtxSynchronize();
        log_cuda("cuCtxSynchronize(destination)", result);
    }

    unsigned char* source_host = nullptr;
    unsigned char* destination_host = nullptr;
    bool validation = false;
    if (result == CUDA_SUCCESS) {
        source_host = static_cast<unsigned char*>(std::malloc(bytes));
        destination_host = static_cast<unsigned char*>(std::malloc(bytes));
        if (source_host != nullptr && destination_host != nullptr) {
            cuCtxSetCurrent(source_context);
            result = cuMemcpyDtoH(source_host, source_buffer, bytes);
            log_cuda("cuMemcpyDtoH(source-validation)", result);
            if (result == CUDA_SUCCESS) {
                cuCtxSetCurrent(destination_context);
                result = cuMemcpyDtoH(destination_host, destination_buffer, bytes);
                log_cuda("cuMemcpyDtoH(destination-validation)", result);
            }
            validation = result == CUDA_SUCCESS && bytes > 0 &&
                         source_host[0] == expected_first_byte &&
                         std::memcmp(source_host, destination_host, bytes) == 0;
            std::fprintf(stderr,
                         "cuda_cross_adapter_validation=%s source_first=%02x destination_first=%02x "
                         "expected=%02x source_fnv1a=0x%016llx destination_fnv1a=0x%016llx\n",
                         validation ? "ok" : "FAIL", source_host[0], destination_host[0],
                         expected_first_byte,
                         static_cast<unsigned long long>(fnv1a(source_host, bytes)),
                         static_cast<unsigned long long>(fnv1a(destination_host, bytes)));
        } else {
            result = CUDA_ERROR_OUT_OF_MEMORY;
        }
    }

    std::free(source_host);
    std::free(destination_host);
    if (destination_external) cuDestroyExternalMemory(destination_external);
    if (source_external) cuDestroyExternalMemory(source_external);
    cuCtxDestroy(destination_context);
    cuCtxDestroy(source_context);
    close(source_fd);
    close(destination_fd);
    return validation && result == CUDA_SUCCESS ? 0 : 10;
}
