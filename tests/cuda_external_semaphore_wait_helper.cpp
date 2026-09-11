#include <cuda.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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

int main(int argc, char **argv)
{
    if (argc != 5 && argc != 6 && argc != 7) {
        std::fprintf(stderr, "usage: %s <wait-fd> <cuda-ordinal> <value> <status-log> "
                     "[wait-gate] [signal-fd]\n",
                     argv[0]);
        return 2;
    }
    const int fd = std::atoi(argv[1]);
    const int ordinal = std::atoi(argv[2]);
    const unsigned long long value = std::strtoull(argv[3], nullptr, 10);
    const char *status_log = argv[4];
    const char *wait_gate = argc == 6 ? argv[5] : nullptr;
    const int signal_fd = argc == 7 ? std::atoi(argv[6]) : -1;
    struct stat fd_stat{};
    if (fd < 0 || (signal_fd == fd) || fstat(fd, &fd_stat) != 0 ||
        (signal_fd >= 0 && fstat(signal_fd, &fd_stat) != 0)) {
        std::fprintf(stderr, "fstat failed fd=%d errno=%d\n", fd, errno);
        write_status(status_log, "fstat_failed", CUDA_ERROR_INVALID_HANDLE);
        return 3;
    }

    CUresult result = cuInit(0);
    if (result != CUDA_SUCCESS) {
        log_cuda("cuInit", result);
        write_status(status_log, "cuInit_failed", result);
        return 4;
    }
    CUdevice cuda_device = -1;
    result = cuDeviceGet(&cuda_device, ordinal);
    if (result != CUDA_SUCCESS) {
        log_cuda("cuDeviceGet", result);
        write_status(status_log, "cuDeviceGet_failed", result);
        return 5;
    }
    CUcontext context = nullptr;
    result = cuCtxCreate(&context, 0, cuda_device);
    if (result != CUDA_SUCCESS) {
        log_cuda("cuCtxCreate", result);
        write_status(status_log, "cuCtxCreate_failed", result);
        return 6;
    }
    CUstream stream = nullptr;
    result = cuStreamCreate(&stream, CU_STREAM_DEFAULT);
    if (result != CUDA_SUCCESS) {
        log_cuda("cuStreamCreate", result);
        write_status(status_log, "cuStreamCreate_failed", result);
        cuCtxDestroy(context);
        return 7;
    }

    CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC description{};
    description.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD;
    description.handle.fd = fd;
    CUexternalSemaphore external = nullptr;
    result = cuImportExternalSemaphore(&external, &description);
    log_cuda("cuImportExternalSemaphore(opaque-fd)", result);
    if (result != CUDA_SUCCESS) {
        write_status(status_log, "import_failed", result);
        cuStreamDestroy(stream);
        cuCtxDestroy(context);
        return 8;
    }
    write_status(status_log, "ready", CUDA_SUCCESS);

    CUexternalSemaphore signal_external = nullptr;
    if (signal_fd >= 0) {
        CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC signal_description{};
        signal_description.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD;
        signal_description.handle.fd = signal_fd;
        result = cuImportExternalSemaphore(&signal_external, &signal_description);
        log_cuda("cuImportExternalSemaphore(signal-opaque-fd)", result);
        if (result != CUDA_SUCCESS) {
            write_status(status_log, "signal_import_failed", result);
            cuDestroyExternalSemaphore(external);
            cuStreamDestroy(stream);
            cuCtxDestroy(context);
            return 9;
        }
    }

    if (wait_gate && *wait_gate) {
        bool gate_open = false;
        for (unsigned i = 0; i < 1000; ++i) {
            std::ifstream file(wait_gate);
            std::string contents((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
            if (contents.find("go") != std::string::npos) {
                gate_open = true;
                break;
            }
            usleep(10000);
        }
        if (!gate_open) {
            write_status(status_log, "gate_timeout", CUDA_ERROR_TIMEOUT);
            cuDestroyExternalSemaphore(external);
            cuStreamDestroy(stream);
            cuCtxDestroy(context);
            if (signal_external) cuDestroyExternalSemaphore(signal_external);
            return 10;
        }
    }

    CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wait_params{};
    wait_params.params.fence.value = value;
    result = cuWaitExternalSemaphoresAsync(&external, &wait_params, 1, stream);
    log_cuda("cuWaitExternalSemaphoresAsync", result);
    if (result == CUDA_SUCCESS && signal_external) {
        CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signal_params{};
        signal_params.params.fence.value = value;
        result = cuSignalExternalSemaphoresAsync(&signal_external, &signal_params,
                                                  1, stream);
        log_cuda("cuSignalExternalSemaphoresAsync", result);
    }
    if (result == CUDA_SUCCESS)
        result = cuStreamSynchronize(stream);
    log_cuda("cuStreamSynchronize", result);
    write_status(status_log, result == CUDA_SUCCESS ? "done" : "wait_failed", result);

    cuDestroyExternalSemaphore(external);
    if (signal_external) cuDestroyExternalSemaphore(signal_external);
    cuStreamDestroy(stream);
    cuCtxDestroy(context);
    return result == CUDA_SUCCESS ? 0 : 9;
}
