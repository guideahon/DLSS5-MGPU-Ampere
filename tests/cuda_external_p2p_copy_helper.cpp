#include <cuda.h>

#include <chrono>
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
    const bool pairs_mode = argc >= 2 && std::strcmp(argv[1], "--pairs") == 0;
    const bool pairs_repeat_mode = argc >= 2 && std::strcmp(argv[1], "--pairs-repeat") == 0;
    if (pairs_mode || pairs_repeat_mode) {
        const int header_size = pairs_repeat_mode ? 6 : 5;
        if (argc < header_size + 8) {
            std::fprintf(stderr,
                "usage: %s --pairs[-repeat] <source-ordinal> <destination-ordinal> "
                "<pair-count> [repeat-count] "
                "<source-fd> <source-size> <source-offset> <destination-fd> "
                "<destination-size> <destination-offset> <bytes> <expected> ...\n",
                argv[0]);
            return 2;
        }
        const int source_ordinal = std::atoi(argv[2]);
        const int destination_ordinal = std::atoi(argv[3]);
        const int pair_count = std::atoi(argv[4]);
        const int repeat_count = pairs_repeat_mode ? std::atoi(argv[5]) : 1;
        if (pair_count < 1 || pair_count > 8 || repeat_count < 1 ||
            argc != header_size + pair_count * 8)
            return 2;

        struct Pair {
            int source_fd;
            unsigned long long source_size;
            unsigned long long source_offset;
            int destination_fd;
            unsigned long long destination_size;
            unsigned long long destination_offset;
            unsigned long long bytes;
            unsigned int expected;
        } pairs[8]{};
        for (int index = 0; index < pair_count; ++index) {
            const int base = header_size + index * 8;
            pairs[index].source_fd = std::atoi(argv[base]);
            pairs[index].source_size = std::strtoull(argv[base + 1], nullptr, 10);
            pairs[index].source_offset = std::strtoull(argv[base + 2], nullptr, 10);
            pairs[index].destination_fd = std::atoi(argv[base + 3]);
            pairs[index].destination_size = std::strtoull(argv[base + 4], nullptr, 10);
            pairs[index].destination_offset = std::strtoull(argv[base + 5], nullptr, 10);
            pairs[index].bytes = std::strtoull(argv[base + 6], nullptr, 10);
            pairs[index].expected = static_cast<unsigned int>(
                std::strtoul(argv[base + 7], nullptr, 16)) & 0xffU;
            if (pairs[index].source_fd < 0 || pairs[index].destination_fd < 0 ||
                pairs[index].bytes == 0 ||
                pairs[index].source_offset != pairs[index].destination_offset)
                return 2;
        }
        std::fprintf(stderr,
                     "CUDA cross-adapter pair helper pairs=%d source=%d destination=%d "
                     "repeat_count=%d persistent=%s\n",
                     pair_count, source_ordinal, destination_ordinal, repeat_count,
                     pairs_repeat_mode ? "yes" : "no");
        for (int index = 0; index < pair_count; ++index) {
            struct stat source_stat{};
            struct stat destination_stat{};
            if (fstat(pairs[index].source_fd, &source_stat) != 0 ||
                fstat(pairs[index].destination_fd, &destination_stat) != 0 ||
                !S_ISCHR(source_stat.st_mode) || !S_ISCHR(destination_stat.st_mode))
                return 3;
        }

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

        CUexternalMemory source_external[8]{};
        CUexternalMemory destination_external[8]{};
        CUdeviceptr source_buffers[8]{};
        CUdeviceptr destination_buffers[8]{};
        bool mapped = true;
        for (int index = 0; index < pair_count && mapped; ++index) {
            CUDA_EXTERNAL_MEMORY_HANDLE_DESC source_desc{};
            source_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
            source_desc.handle.fd = pairs[index].source_fd;
            source_desc.size = pairs[index].source_size;
            CUDA_EXTERNAL_MEMORY_HANDLE_DESC destination_desc{};
            destination_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
            destination_desc.handle.fd = pairs[index].destination_fd;
            destination_desc.size = pairs[index].destination_size;
            result = cuCtxSetCurrent(source_context);
            if (result == CUDA_SUCCESS)
                result = cuImportExternalMemory(&source_external[index], &source_desc);
            log_cuda("cuImportExternalMemory(pair-source)", result);
            if (result == CUDA_SUCCESS)
                result = cuCtxSetCurrent(destination_context);
            if (result == CUDA_SUCCESS)
                result = cuImportExternalMemory(&destination_external[index], &destination_desc);
            log_cuda("cuImportExternalMemory(pair-destination)", result);
            if (result != CUDA_SUCCESS) { mapped = false; break; }

            CUDA_EXTERNAL_MEMORY_BUFFER_DESC source_buffer{};
            source_buffer.offset = pairs[index].source_offset;
            source_buffer.size = pairs[index].bytes;
            CUDA_EXTERNAL_MEMORY_BUFFER_DESC destination_buffer{};
            destination_buffer.offset = pairs[index].destination_offset;
            destination_buffer.size = pairs[index].bytes;
            result = cuCtxSetCurrent(source_context);
            if (result == CUDA_SUCCESS)
                result = cuExternalMemoryGetMappedBuffer(&source_buffers[index],
                                                          source_external[index], &source_buffer);
            if (result == CUDA_SUCCESS)
                result = cuCtxSetCurrent(destination_context);
            if (result == CUDA_SUCCESS)
                result = cuExternalMemoryGetMappedBuffer(&destination_buffers[index],
                                                          destination_external[index],
                                                          &destination_buffer);
            log_cuda("cuExternalMemoryGetMappedBuffer(pair)", result);
            mapped = result == CUDA_SUCCESS;
        }
        const auto copy_start = std::chrono::steady_clock::now();
        if (mapped) {
            for (int repeat = 0; repeat < repeat_count && mapped; ++repeat) {
                for (int index = 0; index < pair_count; ++index) {
                    result = cuMemcpyPeer(destination_buffers[index], destination_context,
                                          source_buffers[index], source_context,
                                          pairs[index].bytes);
                    if (repeat == 0 || result != CUDA_SUCCESS)
                        log_cuda("cuMemcpyPeer(pair)", result);
                    if (result != CUDA_SUCCESS) { mapped = false; break; }
                }
            }
        }
        if (mapped) {
            cuCtxSetCurrent(destination_context);
            result = cuCtxSynchronize();
            log_cuda("cuCtxSynchronize(destination)", result);
            mapped = result == CUDA_SUCCESS;
        }
        const auto copy_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - copy_start).count();
        std::fprintf(stderr, "cuda_pair_repeat_result=%s iterations=%d copy_us=%lld\n",
                     mapped ? "ok" : "FAIL", repeat_count,
                     static_cast<long long>(copy_us));
        bool validation = mapped;
        for (int index = 0; index < pair_count && validation; ++index) {
            auto* source_host = static_cast<unsigned char*>(std::malloc(pairs[index].bytes));
            auto* destination_host = static_cast<unsigned char*>(std::malloc(pairs[index].bytes));
            if (!source_host || !destination_host) {
                validation = false;
                std::free(source_host);
                std::free(destination_host);
                break;
            }
            cuCtxSetCurrent(source_context);
            result = cuMemcpyDtoH(source_host, source_buffers[index], pairs[index].bytes);
            if (result == CUDA_SUCCESS) {
                cuCtxSetCurrent(destination_context);
                result = cuMemcpyDtoH(destination_host, destination_buffers[index], pairs[index].bytes);
            }
            validation = result == CUDA_SUCCESS && pairs[index].bytes > 0 &&
                         source_host[0] == pairs[index].expected &&
                         std::memcmp(source_host, destination_host, pairs[index].bytes) == 0;
            std::fprintf(stderr, "cuda_pair_validation pair=%d %s source_first=%02x destination_first=%02x\n",
                         index, validation ? "ok" : "FAIL", source_host[0], destination_host[0]);
            std::free(source_host);
            std::free(destination_host);
        }
        for (int index = 0; index < pair_count; ++index) {
            if (destination_external[index]) {
                cuCtxSetCurrent(destination_context);
                cuDestroyExternalMemory(destination_external[index]);
            }
            if (source_external[index]) {
                cuCtxSetCurrent(source_context);
                cuDestroyExternalMemory(source_external[index]);
            }
        }
        cuCtxDestroy(destination_context);
        cuCtxDestroy(source_context);
        return validation ? 0 : 10;
    }
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
