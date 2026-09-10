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

static unsigned long long fnv1a(const unsigned char* bytes, unsigned long long size) {
    unsigned long long hash = 1469598103934665603ULL;
    for (unsigned long long i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

int main(int argc, char** argv) {
    const bool batch = argc >= 2 && std::strcmp(argv[1], "--batch") == 0;
    if ((!batch && argc != 11) || (batch && argc < 12)) {
        std::fprintf(stderr,
            "usage: %s <source-fd> <source-heap-size> <source-offset> <bytes> "
            "<source-ordinal> <destination-fd> <destination-heap-size> "
            "<destination-offset> <destination-ordinal> <expected-first-byte>\n"
            "   or: %s --batch <source-fd> <source-heap-size> <source-ordinal> "
            "<destination-fd> <destination-heap-size> <destination-ordinal> "
            "<plane-count> <offset> <bytes> <expected-first-byte> ...\n",
            argv[0], argv[0]);
        return 2;
    }
    struct Plane { unsigned long long offset; unsigned long long bytes; unsigned int expected; };
    Plane planes[3]{};
    int plane_count = 1;
    int source_fd = -1;
    unsigned long long source_heap_size = 0;
    int source_ordinal = 0;
    int destination_fd = -1;
    unsigned long long destination_heap_size = 0;
    int destination_ordinal = 0;
    if (batch) {
        plane_count = std::atoi(argv[8]);
        if (plane_count < 1 || plane_count > 3 || argc != 9 + plane_count * 3)
            return 2;
        source_fd = std::atoi(argv[2]);
        source_heap_size = std::strtoull(argv[3], nullptr, 10);
        source_ordinal = std::atoi(argv[4]);
        destination_fd = std::atoi(argv[5]);
        destination_heap_size = std::strtoull(argv[6], nullptr, 10);
        destination_ordinal = std::atoi(argv[7]);
        for (int index = 0; index < plane_count; ++index) {
            const int base = 9 + index * 3;
            planes[index].offset = std::strtoull(argv[base], nullptr, 10);
            planes[index].bytes = std::strtoull(argv[base + 1], nullptr, 10);
            planes[index].expected =
                static_cast<unsigned int>(std::strtoul(argv[base + 2], nullptr, 16)) & 0xffU;
        }
    } else {
        source_fd = std::atoi(argv[1]);
        source_heap_size = std::strtoull(argv[2], nullptr, 10);
        planes[0].offset = std::strtoull(argv[3], nullptr, 10);
        planes[0].bytes = std::strtoull(argv[4], nullptr, 10);
        source_ordinal = std::atoi(argv[5]);
        destination_fd = std::atoi(argv[6]);
        destination_heap_size = std::strtoull(argv[7], nullptr, 10);
        planes[0].expected =
            static_cast<unsigned int>(std::strtoul(argv[10], nullptr, 16)) & 0xffU;
        destination_ordinal = std::atoi(argv[9]);
    }
    std::fprintf(stderr,
                 "CUDA cross-adapter helper source_fd=%d destination_fd=%d planes=%d "
                 "source=%d destination=%d\n", source_fd, destination_fd, plane_count,
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
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC source_import_desc{};
    source_import_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    source_import_desc.handle.fd = source_fd;
    source_import_desc.size = source_heap_size;
    CUresult import_result = cuCtxSetCurrent(source_context);
    if (import_result == CUDA_SUCCESS)
        import_result = cuImportExternalMemory(&source_external, &source_import_desc);
    log_cuda("cuImportExternalMemory(source)", import_result);
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC destination_import_desc{};
    destination_import_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    destination_import_desc.handle.fd = destination_fd;
    destination_import_desc.size = destination_heap_size;
    if (import_result == CUDA_SUCCESS) {
        import_result = cuCtxSetCurrent(destination_context);
        if (import_result == CUDA_SUCCESS)
            import_result = cuImportExternalMemory(&destination_external,
                                                   &destination_import_desc);
    }
    log_cuda("cuImportExternalMemory(destination)", import_result);
    CUdeviceptr source_buffers[3]{};
    CUdeviceptr destination_buffers[3]{};
    bool mapped = import_result == CUDA_SUCCESS;
    for (int index = 0; mapped && index < plane_count; ++index) {
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC source_buffer_desc{};
        source_buffer_desc.offset = planes[index].offset;
        source_buffer_desc.size = planes[index].bytes;
        mapped = cuCtxSetCurrent(source_context) == CUDA_SUCCESS &&
                 cuExternalMemoryGetMappedBuffer(&source_buffers[index], source_external,
                                                 &source_buffer_desc) == CUDA_SUCCESS;
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC destination_buffer_desc{};
        destination_buffer_desc.offset = planes[index].offset;
        destination_buffer_desc.size = planes[index].bytes;
        mapped = mapped && cuCtxSetCurrent(destination_context) == CUDA_SUCCESS &&
                 cuExternalMemoryGetMappedBuffer(&destination_buffers[index],
                                                 destination_external,
                                                 &destination_buffer_desc) == CUDA_SUCCESS;
    }
    if (!mapped) {
        if (destination_external) cuDestroyExternalMemory(destination_external);
        if (source_external) cuDestroyExternalMemory(source_external);
        cuCtxDestroy(destination_context);
        cuCtxDestroy(source_context);
        return 9;
    }

    for (int index = 0; index < plane_count && result == CUDA_SUCCESS; ++index) {
        result = cuMemcpyPeer(destination_buffers[index], destination_context,
                              source_buffers[index], source_context, planes[index].bytes);
        log_cuda("cuMemcpyPeer(source->destination)", result);
    }
    if (result == CUDA_SUCCESS) {
        cuCtxSetCurrent(destination_context);
        result = cuCtxSynchronize();
        log_cuda("cuCtxSynchronize(destination)", result);
    }

    bool validation = result == CUDA_SUCCESS;
    if (result == CUDA_SUCCESS) {
        for (int index = 0; index < plane_count && validation; ++index) {
            const unsigned long long bytes = planes[index].bytes;
            unsigned char* source_host = static_cast<unsigned char*>(std::malloc(bytes));
            unsigned char* destination_host = static_cast<unsigned char*>(std::malloc(bytes));
            if (source_host != nullptr && destination_host != nullptr) {
                cuCtxSetCurrent(source_context);
                result = cuMemcpyDtoH(source_host, source_buffers[index], bytes);
                log_cuda("cuMemcpyDtoH(source-validation)", result);
                if (result == CUDA_SUCCESS) {
                    cuCtxSetCurrent(destination_context);
                    result = cuMemcpyDtoH(destination_host, destination_buffers[index], bytes);
                    log_cuda("cuMemcpyDtoH(destination-validation)", result);
                }
                validation = result == CUDA_SUCCESS && bytes > 0 &&
                             source_host[0] == planes[index].expected &&
                             std::memcmp(source_host, destination_host, bytes) == 0;
                std::fprintf(stderr,
                             "cuda_cross_adapter_validation plane=%d %s source_first=%02x destination_first=%02x "
                             "expected=%02x source_fnv1a=0x%016llx destination_fnv1a=0x%016llx\n",
                             index, validation ? "ok" : "FAIL", source_host[0], destination_host[0],
                             planes[index].expected,
                             static_cast<unsigned long long>(fnv1a(source_host, bytes)),
                             static_cast<unsigned long long>(fnv1a(destination_host, bytes)));
            } else {
                result = CUDA_ERROR_OUT_OF_MEMORY;
                validation = false;
            }
            std::free(source_host);
            std::free(destination_host);
        }
    }
    if (destination_external) cuDestroyExternalMemory(destination_external);
    if (source_external) cuDestroyExternalMemory(source_external);
    cuCtxDestroy(destination_context);
    cuCtxDestroy(source_context);
    close(source_fd);
    close(destination_fd);
    return validation && result == CUDA_SUCCESS ? 0 : 10;
}
