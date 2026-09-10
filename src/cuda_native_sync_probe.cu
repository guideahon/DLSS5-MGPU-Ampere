#include <cuda_runtime.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    int source = 0;
    int destination = 1;
    std::size_t bytes = 1920ULL * 1080ULL * 4ULL;
    int slots = 3;
    int frames = 120;
    int timeout_ms = 5000;
    bool json = false;
};

struct Slot {
    unsigned char* source = nullptr;
    unsigned char* destination = nullptr;
    cudaEvent_t ready = nullptr;
    cudaEvent_t done = nullptr;
    std::uint64_t frame_id = 0;
    bool in_flight = false;
    bool has_done = false;
};

struct Report {
    int source = 0;
    int destination = 1;
    std::size_t bytes = 0;
    int slots = 0;
    int frames = 0;
    int completed = 0;
    int timeout_ms = 0;
    bool peer_enabled = false;
    bool gpu_native_waits = false;
    bool validation_passed = false;
    double seconds = 0.0;
    double gigabytes_per_second = 0.0;
    std::string error;
};

std::string cuda_error(const char* operation, cudaError_t status) {
    std::ostringstream out;
    out << operation << ": " << cudaGetErrorName(status) << " - "
        << cudaGetErrorString(status);
    return out.str();
}

bool parse_int(const char* value, int* output) {
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (!value || *value == '\0' || !end || *end != '\0') return false;
    *output = static_cast<int>(parsed);
    return true;
}

bool parse_size(const char* value, std::size_t* output) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (!value || *value == '\0' || !end || *end != '\0') return false;
    *output = static_cast<std::size_t>(parsed);
    return true;
}

bool parse_options(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--json") {
            options->json = true;
            continue;
        }
        if (arg == "--help") {
            std::cout << "Usage: " << argv[0]
                      << " [--source N] [--destination N] [--bytes N]"
                      << " [--slots N] [--frames N] [--timeout-ms N] [--json]\n";
            std::exit(0);
        }
        if (i + 1 >= argc) return false;
        if (arg == "--source") {
            if (!parse_int(argv[++i], &options->source)) return false;
        } else if (arg == "--destination") {
            if (!parse_int(argv[++i], &options->destination)) return false;
        } else if (arg == "--bytes") {
            if (!parse_size(argv[++i], &options->bytes)) return false;
        } else if (arg == "--slots") {
            if (!parse_int(argv[++i], &options->slots)) return false;
        } else if (arg == "--frames") {
            if (!parse_int(argv[++i], &options->frames)) return false;
        } else if (arg == "--timeout-ms") {
            if (!parse_int(argv[++i], &options->timeout_ms)) return false;
        } else {
            return false;
        }
    }
    return options->source >= 0 && options->destination >= 0 &&
           options->source != options->destination && options->bytes > 0 &&
           options->slots >= 2 && options->frames > 0 && options->timeout_ms > 0;
}

bool enable_peer(int owner, int peer, std::string* error) {
    int possible = 0;
    cudaError_t status = cudaDeviceCanAccessPeer(&possible, owner, peer);
    if (status != cudaSuccess) {
        if (error) *error = cuda_error("cudaDeviceCanAccessPeer", status);
        return false;
    }
    if (!possible) {
        if (error) *error = "CUDA reports peer access unavailable";
        return false;
    }
    status = cudaSetDevice(owner);
    if (status != cudaSuccess) {
        if (error) *error = cuda_error("cudaSetDevice(owner)", status);
        return false;
    }
    status = cudaDeviceEnablePeerAccess(peer, 0);
    if (status != cudaSuccess && status != cudaErrorPeerAccessAlreadyEnabled) {
        if (error) *error = cuda_error("cudaDeviceEnablePeerAccess", status);
        return false;
    }
    cudaGetLastError();
    return true;
}

void release_slots(std::vector<Slot>* slots, int source, int destination,
                   cudaStream_t source_stream, cudaStream_t destination_stream) {
    cudaSetDevice(destination);
    for (auto& slot : *slots) {
        if (slot.done) cudaEventDestroy(slot.done);
        if (slot.destination) cudaFree(slot.destination);
    }
    cudaSetDevice(source);
    for (auto& slot : *slots) {
        if (slot.ready) cudaEventDestroy(slot.ready);
        if (slot.source) cudaFree(slot.source);
    }
    if (destination_stream) {
        cudaSetDevice(destination);
        cudaStreamDestroy(destination_stream);
    }
    if (source_stream) {
        cudaSetDevice(source);
        cudaStreamDestroy(source_stream);
    }
}

Report run(const Options& options) {
    Report report;
    report.source = options.source;
    report.destination = options.destination;
    report.bytes = options.bytes;
    report.slots = options.slots;
    report.frames = options.frames;
    report.timeout_ms = options.timeout_ms;

    int count = 0;
    cudaError_t status = cudaGetDeviceCount(&count);
    if (status != cudaSuccess || options.source >= count || options.destination >= count) {
        report.error = status == cudaSuccess
            ? "GPU index is outside the CUDA device list"
            : cuda_error("cudaGetDeviceCount", status);
        return report;
    }
    std::string error;
    if (!enable_peer(options.source, options.destination, &error) ||
        !enable_peer(options.destination, options.source, &error)) {
        report.error = error;
        return report;
    }
    report.peer_enabled = true;

    cudaStream_t source_stream = nullptr;
    cudaStream_t destination_stream = nullptr;
    std::vector<Slot> slots(static_cast<std::size_t>(options.slots));
    auto fail = [&](const char* operation, cudaError_t result) {
        report.error = cuda_error(operation, result);
        release_slots(&slots, options.source, options.destination,
                      source_stream, destination_stream);
        return report;
    };

    status = cudaSetDevice(options.source);
    if (status != cudaSuccess) return fail("cudaSetDevice(source)", status);
    status = cudaStreamCreateWithFlags(&source_stream, cudaStreamNonBlocking);
    if (status != cudaSuccess) return fail("cudaStreamCreate(source)", status);
    status = cudaSetDevice(options.destination);
    if (status != cudaSuccess) return fail("cudaSetDevice(destination)", status);
    status = cudaStreamCreateWithFlags(&destination_stream, cudaStreamNonBlocking);
    if (status != cudaSuccess) return fail("cudaStreamCreate(destination)", status);

    for (auto& slot : slots) {
        if ((status = cudaSetDevice(options.source)) != cudaSuccess)
            return fail("cudaSetDevice(source)", status);
        if ((status = cudaMalloc(reinterpret_cast<void**>(&slot.source), options.bytes)) != cudaSuccess)
            return fail("cudaMalloc(source)", status);
        if ((status = cudaEventCreateWithFlags(&slot.ready, cudaEventDisableTiming)) != cudaSuccess)
            return fail("cudaEventCreate(ready)", status);
        if ((status = cudaSetDevice(options.destination)) != cudaSuccess)
            return fail("cudaSetDevice(destination)", status);
        if ((status = cudaMalloc(reinterpret_cast<void**>(&slot.destination), options.bytes)) != cudaSuccess)
            return fail("cudaMalloc(destination)", status);
        if ((status = cudaEventCreateWithFlags(&slot.done, cudaEventDisableTiming)) != cudaSuccess)
            return fail("cudaEventCreate(done)", status);
    }

    std::vector<unsigned char> sample(64);
    int submitted = 0;
    auto last_progress = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(options.timeout_ms);
    const auto start = last_progress;
    while (report.completed < options.frames) {
        bool progress = false;
        for (auto& slot : slots) {
            if (!slot.in_flight) continue;
            if ((status = cudaSetDevice(options.destination)) != cudaSuccess)
                return fail("cudaSetDevice(destination)", status);
            status = cudaEventQuery(slot.done);
            if (status == cudaErrorNotReady) continue;
            if (status != cudaSuccess) return fail("cudaEventQuery(done)", status);
            if ((status = cudaMemcpy(sample.data(), slot.destination, sample.size(),
                                     cudaMemcpyDeviceToHost)) != cudaSuccess)
                return fail("cudaMemcpy(validation)", status);
            const unsigned char expected =
                static_cast<unsigned char>(slot.frame_id & 0xffU);
            for (unsigned char value : sample) {
                if (value != expected) {
                    report.error = "CUDA native sync validation mismatch";
                    release_slots(&slots, options.source, options.destination,
                                  source_stream, destination_stream);
                    return report;
                }
            }
            slot.in_flight = false;
            slot.has_done = true;
            ++report.completed;
            progress = true;
        }

        for (auto& slot : slots) {
            if (submitted >= options.frames || slot.in_flight) continue;
            if ((status = cudaSetDevice(options.source)) != cudaSuccess)
                return fail("cudaSetDevice(source)", status);
            if (slot.has_done) {
                status = cudaStreamWaitEvent(source_stream, slot.done, 0);
                if (status != cudaSuccess)
                    return fail("cudaStreamWaitEvent(source<-destination)", status);
            }
            const unsigned char pattern = static_cast<unsigned char>(submitted & 0xff);
            if ((status = cudaMemsetAsync(slot.source, pattern, options.bytes,
                                          source_stream)) != cudaSuccess)
                return fail("cudaMemsetAsync(source)", status);
            if ((status = cudaEventRecord(slot.ready, source_stream)) != cudaSuccess)
                return fail("cudaEventRecord(ready)", status);
            if ((status = cudaSetDevice(options.destination)) != cudaSuccess)
                return fail("cudaSetDevice(destination)", status);
            status = cudaStreamWaitEvent(destination_stream, slot.ready, 0);
            if (status != cudaSuccess)
                return fail("cudaStreamWaitEvent(destination<-source)", status);
            status = cudaMemcpyPeerAsync(slot.destination, options.destination,
                                         slot.source, options.source, options.bytes,
                                         destination_stream);
            if (status != cudaSuccess) return fail("cudaMemcpyPeerAsync", status);
            status = cudaEventRecord(slot.done, destination_stream);
            if (status != cudaSuccess) return fail("cudaEventRecord(done)", status);
            slot.frame_id = static_cast<std::uint64_t>(submitted);
            slot.in_flight = true;
            ++submitted;
            report.gpu_native_waits = true;
            progress = true;
        }

        if (progress) {
            last_progress = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - last_progress > timeout) {
            report.error = "CUDA native sync completion timeout";
            release_slots(&slots, options.source, options.destination,
                          source_stream, destination_stream);
            return report;
        } else {
            std::this_thread::yield();
        }
    }
    const auto end = std::chrono::steady_clock::now();
    report.seconds = std::chrono::duration<double>(end - start).count();
    report.validation_passed = true;
    if (report.seconds > 0.0) {
        report.gigabytes_per_second =
            static_cast<double>(options.bytes) * report.completed /
            report.seconds / 1.0e9;
    }
    release_slots(&slots, options.source, options.destination,
                  source_stream, destination_stream);
    return report;
}

void print_report(const Report& report, bool json) {
    if (json) {
        std::cout << "{\"source\":" << report.source
                  << ",\"destination\":" << report.destination
                  << ",\"bytes\":" << report.bytes
                  << ",\"slots\":" << report.slots
                  << ",\"frames\":" << report.frames
                  << ",\"completed\":" << report.completed
                  << ",\"timeout_ms\":" << report.timeout_ms
                  << ",\"peer_enabled\":"
                  << (report.peer_enabled ? "true" : "false")
                  << ",\"gpu_native_waits\":"
                  << (report.gpu_native_waits ? "true" : "false")
                  << ",\"validation_passed\":"
                  << (report.validation_passed ? "true" : "false")
                  << ",\"seconds\":" << std::setprecision(12) << report.seconds
                  << ",\"gigabytes_per_second\":" << report.gigabytes_per_second
                  << ",\"error\":\"" << report.error << "\"}\n";
        return;
    }
    std::cout << "CUDA native P2P sync: " << report.source << " -> "
              << report.destination << ", frames=" << report.completed << "/"
              << report.frames << ", slots=" << report.slots
              << ", waits=" << (report.gpu_native_waits ? "GPU" : "none")
              << ", validation=" << (report.validation_passed ? "ok" : "FAIL") << '\n';
    if (!report.error.empty()) std::cout << "error: " << report.error << '\n';
    if (report.validation_passed)
        std::cout << std::fixed << std::setprecision(3)
                  << "throughput=" << report.gigabytes_per_second << " GB/s\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        std::cerr << "invalid arguments; use --help\n";
        return 2;
    }
    const Report report = run(options);
    print_report(report, options.json);
    return report.validation_passed ? 0 : 1;
}
