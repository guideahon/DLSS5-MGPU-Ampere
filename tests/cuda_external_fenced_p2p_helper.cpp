#include <cuda.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

struct Pair {
    int source_fd;
    unsigned long long source_size;
    unsigned long long source_offset;
    int destination_fd;
    unsigned long long destination_size;
    unsigned long long destination_offset;
    unsigned long long bytes;
    unsigned int expected;
};

static void log_cuda(const char *label, CUresult result)
{
    const char *name = nullptr;
    const char *text = nullptr;
    cuGetErrorName(result, &name);
    cuGetErrorString(result, &text);
    std::fprintf(stderr, "%s: rc=%d name=%s text=%s\n", label,
                 static_cast<int>(result), name ? name : "?", text ? text : "?");
}

static void write_status(const char *path, const char *status, CUresult result)
{
    if (!path || !*path) return;
    FILE *file = std::fopen(path, "a");
    if (!file) return;
    if (result == CUDA_SUCCESS) {
        std::fprintf(file, "%s rc=0\n", status);
    } else {
        const char *name = nullptr;
        const char *text = nullptr;
        cuGetErrorName(result, &name);
        cuGetErrorString(result, &text);
        std::fprintf(file, "%s rc=%d name=%s text=%s\n", status,
                     static_cast<int>(result), name ? name : "?", text ? text : "?");
    }
    std::fflush(file);
    std::fclose(file);
}

static bool wait_gate(const char *path)
{
    if (!path || !*path) return true;
    for (unsigned i = 0; i < 2000; ++i) {
        std::ifstream file(path);
        std::string contents((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        if (contents.find("go") != std::string::npos) return true;
        usleep(10000);
    }
    return false;
}

static void destroy_all(CUcontext source_context, CUcontext destination_context,
                        CUexternalMemory *source_external,
                        CUexternalMemory *destination_external,
                        int pair_count, CUexternalSemaphore wait_semaphore,
                        CUexternalSemaphore signal_semaphore)
{
    if (wait_semaphore) {
        cuCtxSetCurrent(destination_context);
        cuDestroyExternalSemaphore(wait_semaphore);
    }
    if (signal_semaphore) {
        cuCtxSetCurrent(destination_context);
        cuDestroyExternalSemaphore(signal_semaphore);
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
}

static void append_status_line(const char *path, const std::string &line)
{
    if (!path || !*path) return;
    FILE *file = std::fopen(path, "a");
    if (!file) return;
    std::fprintf(file, "%s\n", line.c_str());
    std::fflush(file);
    std::fclose(file);
}

static int run_fenced_persistent(int argc, char **argv)
{
    // Keep resources, contexts, semaphores and stream alive.  Each frame gets a
    // one-shot fence pair imported up front; this avoids relying on timeline-value
    // reuse, which is not implemented consistently by every D3D12/Vulkan bridge.
    if (argc < 17) return 2;
    const int source_ordinal = std::atoi(argv[2]);
    const int destination_ordinal = std::atoi(argv[3]);
    const int fence_count = std::atoi(argv[4]);
    const char *status_log = argv[5];
    const char *command_path = argv[6];
    if (fence_count < 1 || fence_count > 16) return 2;
    const int resource_base = 7 + fence_count * 2;
    if (argc != resource_base + 8) return 2;

    Pair pair{};
    pair.source_fd = std::atoi(argv[resource_base]);
    pair.source_size = std::strtoull(argv[resource_base + 1], nullptr, 10);
    pair.source_offset = std::strtoull(argv[resource_base + 2], nullptr, 10);
    pair.destination_fd = std::atoi(argv[resource_base + 3]);
    pair.destination_size = std::strtoull(argv[resource_base + 4], nullptr, 10);
    pair.destination_offset = std::strtoull(argv[resource_base + 5], nullptr, 10);
    pair.bytes = std::strtoull(argv[resource_base + 6], nullptr, 10);
    struct stat source_stat{}, destination_stat{};
    if (pair.source_fd < 0 || pair.destination_fd < 0 || pair.bytes == 0 ||
        pair.source_offset + pair.bytes > pair.source_size ||
        pair.destination_offset + pair.bytes > pair.destination_size ||
        fstat(pair.source_fd, &source_stat) != 0 ||
        fstat(pair.destination_fd, &destination_stat) != 0 ||
        !S_ISCHR(source_stat.st_mode) || !S_ISCHR(destination_stat.st_mode)) {
        write_status(status_log, "persistent_validate_failed", CUDA_ERROR_INVALID_HANDLE);
        return 3;
    }

    CUresult result = cuInit(0);
    if (result != CUDA_SUCCESS) {
        write_status(status_log, "persistent_cuInit_failed", result);
        return 4;
    }
    CUdevice source_device = -1, destination_device = -1;
    result = cuDeviceGet(&source_device, source_ordinal);
    if (result == CUDA_SUCCESS) result = cuDeviceGet(&destination_device, destination_ordinal);
    if (result != CUDA_SUCCESS) {
        write_status(status_log, "persistent_device_failed", result);
        return 5;
    }
    CUcontext source_context = nullptr, destination_context = nullptr;
    result = cuCtxCreate(&source_context, 0, source_device);
    if (result == CUDA_SUCCESS) result = cuCtxCreate(&destination_context, 0, destination_device);
    if (result != CUDA_SUCCESS) {
        write_status(status_log, "persistent_context_failed", result);
        if (destination_context) cuCtxDestroy(destination_context);
        if (source_context) cuCtxDestroy(source_context);
        return 6;
    }

    CUexternalMemory source_external = nullptr, destination_external = nullptr;
    CUdeviceptr source_buffer = 0, destination_buffer = 0;
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC source_desc{};
    source_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    source_desc.handle.fd = pair.source_fd;
    source_desc.size = pair.source_size;
    cuCtxSetCurrent(source_context);
    result = cuImportExternalMemory(&source_external, &source_desc);
    if (result == CUDA_SUCCESS) {
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer{};
        buffer.offset = pair.source_offset;
        buffer.size = pair.bytes;
        result = cuExternalMemoryGetMappedBuffer(&source_buffer, source_external, &buffer);
    }
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC destination_desc{};
    destination_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    destination_desc.handle.fd = pair.destination_fd;
    destination_desc.size = pair.destination_size;
    if (result == CUDA_SUCCESS) {
        cuCtxSetCurrent(destination_context);
        result = cuImportExternalMemory(&destination_external, &destination_desc);
    }
    if (result == CUDA_SUCCESS) {
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer{};
        buffer.offset = pair.destination_offset;
        buffer.size = pair.bytes;
        result = cuExternalMemoryGetMappedBuffer(&destination_buffer, destination_external, &buffer);
    }
    if (result != CUDA_SUCCESS) {
        log_cuda("persistent_resource_import", result);
        write_status(status_log, "persistent_resource_import_failed", result);
        CUexternalMemory source_array[1] = {source_external};
        CUexternalMemory destination_array[1] = {destination_external};
        destroy_all(source_context, destination_context, source_array,
                    destination_array, 1, nullptr, nullptr);
        return 7;
    }

    CUexternalSemaphore wait_semaphores[16]{}, signal_semaphores[16]{};
    cuCtxSetCurrent(destination_context);
    for (int index = 0; index < fence_count && result == CUDA_SUCCESS; ++index) {
        const int wait_fd = std::atoi(argv[7 + index * 2]);
        const int signal_fd = std::atoi(argv[8 + index * 2]);
        struct stat wait_stat{}, signal_stat{};
        if (wait_fd < 0 || signal_fd < 0 || fstat(wait_fd, &wait_stat) != 0 ||
            fstat(signal_fd, &signal_stat) != 0) {
            result = CUDA_ERROR_INVALID_HANDLE;
            break;
        }
        CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC wait_desc{};
        wait_desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD;
        wait_desc.handle.fd = wait_fd;
        CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC signal_desc{};
        signal_desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD;
        signal_desc.handle.fd = signal_fd;
        result = cuImportExternalSemaphore(&wait_semaphores[index], &wait_desc);
        if (result == CUDA_SUCCESS)
            result = cuImportExternalSemaphore(&signal_semaphores[index], &signal_desc);
    }
    if (result != CUDA_SUCCESS) {
        log_cuda("persistent_semaphore_import", result);
        write_status(status_log, "persistent_semaphore_import_failed", result);
        CUexternalMemory source_array[1] = {source_external};
        CUexternalMemory destination_array[1] = {destination_external};
        for (int index = 0; index < fence_count; ++index) {
            if (wait_semaphores[index]) cuDestroyExternalSemaphore(wait_semaphores[index]);
            if (signal_semaphores[index]) cuDestroyExternalSemaphore(signal_semaphores[index]);
        }
        destroy_all(source_context, destination_context, source_array,
                    destination_array, 1, nullptr, nullptr);
        return 8;
    }
    CUstream stream = nullptr;
    result = cuStreamCreate(&stream, CU_STREAM_DEFAULT);
    if (result != CUDA_SUCCESS) {
        write_status(status_log, "persistent_stream_failed", result);
        CUexternalMemory source_array[1] = {source_external};
        CUexternalMemory destination_array[1] = {destination_external};
        for (int index = 0; index < fence_count; ++index) {
            if (wait_semaphores[index]) cuDestroyExternalSemaphore(wait_semaphores[index]);
            if (signal_semaphores[index]) cuDestroyExternalSemaphore(signal_semaphores[index]);
        }
        destroy_all(source_context, destination_context, source_array,
                    destination_array, 1, nullptr, nullptr);
        return 9;
    }
    write_status(status_log, "ready", CUDA_SUCCESS);

    std::string last_command;
    bool running = true;
    while (running) {
        std::ifstream command_file(command_path);
        std::string command;
        std::getline(command_file, command);
        if (!command.empty() && command.back() == '\r') command.pop_back();
        if (command.empty() || command == last_command) {
            usleep(1000);
            continue;
        }
        if (command == "quit") {
            append_status_line(status_log, "stopped");
            break;
        }
        std::istringstream parser(command);
        std::string operation;
        int slot = -1;
        parser >> operation;
        if (operation != "go" || !(parser >> slot) || slot < 0 || slot >= fence_count) {
            // The producer replaces the command file.  A reader can observe a
            // transient empty/partial line between truncate and write; leave it
            // unconsumed so the complete command is retried on the next poll.
            usleep(1000);
            continue;
        }
        last_command = command;
        CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wait_params{};
        wait_params.params.fence.value = 1;
        result = cuWaitExternalSemaphoresAsync(&wait_semaphores[slot], &wait_params, 1, stream);
        if (result == CUDA_SUCCESS)
            result = cuMemcpyPeerAsync(destination_buffer, destination_context,
                                       source_buffer, source_context, pair.bytes, stream);
        if (result == CUDA_SUCCESS) {
            CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signal_params{};
            signal_params.params.fence.value = 1;
            result = cuSignalExternalSemaphoresAsync(&signal_semaphores[slot], &signal_params,
                                                     1, stream);
        }
        if (result == CUDA_SUCCESS) result = cuStreamSynchronize(stream);
        if (result == CUDA_SUCCESS) {
            append_status_line(status_log,
                               "done " + std::to_string(slot) + " rc=0");
        } else {
            log_cuda("persistent_frame", result);
            append_status_line(status_log,
                               "done " + std::to_string(slot) + " rc=" +
                               std::to_string(static_cast<int>(result)));
            running = false;
        }
    }
    cuStreamDestroy(stream);
    for (int index = 0; index < fence_count; ++index) {
        if (wait_semaphores[index]) cuDestroyExternalSemaphore(wait_semaphores[index]);
        if (signal_semaphores[index]) cuDestroyExternalSemaphore(signal_semaphores[index]);
    }
    CUexternalMemory source_array[1] = {source_external};
    CUexternalMemory destination_array[1] = {destination_external};
    destroy_all(source_context, destination_context, source_array,
                destination_array, 1, nullptr, nullptr);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && std::strcmp(argv[1], "--fenced-pair-persistent") == 0)
        return run_fenced_persistent(argc, argv);
    if (argc < 18 || std::strcmp(argv[1], "--fenced-pairs") != 0) {
        std::fprintf(stderr,
            "usage: %s --fenced-pairs <source-ordinal> <destination-ordinal> "
            "<wait-fd> <signal-fd> <value> <pair-count> <status-log> <gate> "
            "<source-fd> <source-size> <source-offset> <destination-fd> "
            "<destination-size> <destination-offset> <bytes> <expected> ...\n",
            argv[0]);
        return 2;
    }
    const int source_ordinal = std::atoi(argv[2]);
    const int destination_ordinal = std::atoi(argv[3]);
    const int wait_fd = std::atoi(argv[4]);
    const int signal_fd = std::atoi(argv[5]);
    const unsigned long long value = std::strtoull(argv[6], nullptr, 10);
    const int pair_count = std::atoi(argv[7]);
    const char *status_log = argv[8];
    const char *gate = argv[9];
    if (pair_count < 1 || pair_count > 8 || argc != 10 + pair_count * 8)
        return 2;

    Pair pairs[8]{};
    for (int index = 0; index < pair_count; ++index) {
        const int base = 10 + index * 8;
        Pair &pair = pairs[index];
        pair.source_fd = std::atoi(argv[base]);
        pair.source_size = std::strtoull(argv[base + 1], nullptr, 10);
        pair.source_offset = std::strtoull(argv[base + 2], nullptr, 10);
        pair.destination_fd = std::atoi(argv[base + 3]);
        pair.destination_size = std::strtoull(argv[base + 4], nullptr, 10);
        pair.destination_offset = std::strtoull(argv[base + 5], nullptr, 10);
        pair.bytes = std::strtoull(argv[base + 6], nullptr, 10);
        pair.expected = static_cast<unsigned int>(
            std::strtoul(argv[base + 7], nullptr, 16)) & 0xffU;
        struct stat source_stat{}, destination_stat{};
        if (pair.source_fd < 0 || pair.destination_fd < 0 || pair.bytes == 0 ||
            pair.source_offset + pair.bytes > pair.source_size ||
            pair.destination_offset + pair.bytes > pair.destination_size ||
            fstat(pair.source_fd, &source_stat) != 0 ||
            fstat(pair.destination_fd, &destination_stat) != 0 ||
            !S_ISCHR(source_stat.st_mode) || !S_ISCHR(destination_stat.st_mode)) {
            write_status(status_log, "resource_fd_failed", CUDA_ERROR_INVALID_HANDLE);
            return 3;
        }
    }
    struct stat fence_stat{};
    if (wait_fd < 0 || signal_fd < 0 || wait_fd == signal_fd ||
        fstat(wait_fd, &fence_stat) != 0 || fstat(signal_fd, &fence_stat) != 0) {
        write_status(status_log, "fence_fd_failed", CUDA_ERROR_INVALID_HANDLE);
        return 4;
    }

    CUresult result = cuInit(0);
    if (result != CUDA_SUCCESS) { write_status(status_log, "cuInit_failed", result); return 5; }
    CUdevice source_device = -1, destination_device = -1;
    result = cuDeviceGet(&source_device, source_ordinal);
    if (result == CUDA_SUCCESS) result = cuDeviceGet(&destination_device, destination_ordinal);
    if (result != CUDA_SUCCESS) { write_status(status_log, "cuDeviceGet_failed", result); return 6; }
    CUcontext source_context = nullptr, destination_context = nullptr;
    result = cuCtxCreate(&source_context, 0, source_device);
    if (result == CUDA_SUCCESS) result = cuCtxCreate(&destination_context, 0, destination_device);
    if (result != CUDA_SUCCESS) {
        write_status(status_log, "cuCtxCreate_failed", result);
        if (destination_context) cuCtxDestroy(destination_context);
        if (source_context) cuCtxDestroy(source_context);
        return 7;
    }

    CUexternalMemory source_external[8]{}, destination_external[8]{};
    CUdeviceptr source_buffers[8]{}, destination_buffers[8]{};
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
        cuCtxSetCurrent(source_context);
        result = cuImportExternalMemory(&source_external[index], &source_desc);
        if (result == CUDA_SUCCESS) {
            CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer{};
            buffer.offset = pairs[index].source_offset;
            buffer.size = pairs[index].bytes;
            result = cuExternalMemoryGetMappedBuffer(&source_buffers[index],
                                                      source_external[index], &buffer);
        }
        cuCtxSetCurrent(destination_context);
        if (result == CUDA_SUCCESS)
            result = cuImportExternalMemory(&destination_external[index], &destination_desc);
        if (result == CUDA_SUCCESS) {
            CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer{};
            buffer.offset = pairs[index].destination_offset;
            buffer.size = pairs[index].bytes;
            result = cuExternalMemoryGetMappedBuffer(&destination_buffers[index],
                                                      destination_external[index], &buffer);
        }
        if (result != CUDA_SUCCESS) log_cuda("fenced_resource_import", result);
        mapped = result == CUDA_SUCCESS;
    }
    if (!mapped) {
        write_status(status_log, "resource_import_failed", result);
        destroy_all(source_context, destination_context, source_external,
                    destination_external, pair_count, nullptr, nullptr);
        return 8;
    }

    CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC wait_desc{};
    wait_desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD;
    wait_desc.handle.fd = wait_fd;
    CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC signal_desc{};
    signal_desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD;
    signal_desc.handle.fd = signal_fd;
    CUexternalSemaphore wait_semaphore = nullptr, signal_semaphore = nullptr;
    cuCtxSetCurrent(destination_context);
    result = cuImportExternalSemaphore(&wait_semaphore, &wait_desc);
    if (result == CUDA_SUCCESS)
        result = cuImportExternalSemaphore(&signal_semaphore, &signal_desc);
    if (result != CUDA_SUCCESS) {
        log_cuda("fenced_semaphore_import", result);
        write_status(status_log, "semaphore_import_failed", result);
        destroy_all(source_context, destination_context, source_external,
                    destination_external, pair_count, wait_semaphore, signal_semaphore);
        return 9;
    }
    write_status(status_log, "ready", CUDA_SUCCESS);
    if (!wait_gate(gate)) {
        write_status(status_log, "gate_timeout", CUDA_ERROR_TIMEOUT);
        destroy_all(source_context, destination_context, source_external,
                    destination_external, pair_count, wait_semaphore, signal_semaphore);
        return 10;
    }

    CUstream stream = nullptr;
    result = cuStreamCreate(&stream, CU_STREAM_DEFAULT);
    if (result == CUDA_SUCCESS) {
        CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wait_params{};
        wait_params.params.fence.value = value;
        result = cuWaitExternalSemaphoresAsync(&wait_semaphore, &wait_params, 1, stream);
        log_cuda("fenced_wait", result);
    }
    if (result == CUDA_SUCCESS) {
        for (int index = 0; index < pair_count; ++index) {
            result = cuMemcpyPeerAsync(destination_buffers[index], destination_context,
                                        source_buffers[index], source_context,
                                        pairs[index].bytes, stream);
            if (result != CUDA_SUCCESS) {
                log_cuda("fenced_p2p_copy", result);
                break;
            }
        }
    }
    if (result == CUDA_SUCCESS) {
        CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signal_params{};
        signal_params.params.fence.value = value;
        result = cuSignalExternalSemaphoresAsync(&signal_semaphore, &signal_params,
                                                  1, stream);
        log_cuda("fenced_signal", result);
    }
    if (result == CUDA_SUCCESS) result = cuStreamSynchronize(stream);
    log_cuda("fenced_stream_sync", result);
    write_status(status_log, result == CUDA_SUCCESS ? "done" : "fenced_failed", result);
    if (stream) cuStreamDestroy(stream);
    destroy_all(source_context, destination_context, source_external,
                destination_external, pair_count, wait_semaphore, signal_semaphore);
    return result == CUDA_SUCCESS ? 0 : 11;
}
