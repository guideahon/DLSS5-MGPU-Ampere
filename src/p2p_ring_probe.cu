#include "mgpu/p2p_transport.hpp"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    int source = 0;
    int destination = 1;
    std::size_t bytes = 1920ULL * 1080ULL * 4ULL;
    int slots = 3;
    int frames = 300;
};

bool parse_int(const char* value, int* result) {
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (!value || *value == '\0' || !end || *end != '\0') return false;
    *result = static_cast<int>(parsed);
    return true;
}

bool parse_size(const char* value, std::size_t* result) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (!value || *value == '\0' || !end || *end != '\0') return false;
    *result = static_cast<std::size_t>(parsed);
    return true;
}

bool parse_options(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help") {
            std::cout << "Usage: " << argv[0]
                      << " [--source N] [--destination N] [--bytes N]"
                      << " [--slots N] [--frames N]\n";
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
        } else {
            return false;
        }
    }
    return options->source >= 0 && options->destination >= 0 &&
           options->source != options->destination && options->bytes > 0 &&
           options->slots >= 2 && options->frames > 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) return 2;

    mgpu::AsyncP2PRing ring;
    std::string error;
    if (!ring.initialize(options.source, options.destination, options.bytes,
                         options.slots, &error)) {
        std::cerr << "ring initialization failed: " << error << '\n';
        return 1;
    }
    std::cout << "Async P2P ring: " << options.source << " -> "
              << options.destination << ", slots=" << options.slots
              << ", bytes=" << options.bytes << '\n';

    std::vector<unsigned char> sample(64);
    int submitted = 0;
    int completed = 0;
    int validation_failures = 0;
    const auto start = std::chrono::steady_clock::now();

    while (completed < options.frames) {
        while (submitted < options.frames) {
            const int slot = ring.acquire_slot();
            if (slot < 0) break;
            const auto pattern = static_cast<std::uint8_t>(submitted & 0xff);
            if (!ring.fill_source(slot, pattern, &error) ||
                !ring.submit_copy(slot, static_cast<std::uint64_t>(submitted), &error)) {
                std::cerr << "submit failed: " << error << '\n';
                return 1;
            }
            ++submitted;
        }

        std::vector<mgpu::RingCompletion> completions;
        if (!ring.poll(&completions, &error)) {
            std::cerr << "poll failed: " << error << '\n';
            return 1;
        }
        for (const auto& completion : completions) {
            cudaSetDevice(options.destination);
            const cudaError_t status = cudaMemcpy(
                sample.data(), ring.destination_buffer(completion.slot), sample.size(),
                cudaMemcpyDeviceToHost);
            const auto expected = static_cast<unsigned char>(completion.frame_id & 0xff);
            const bool valid = status == cudaSuccess &&
                std::all_of(sample.begin(), sample.end(),
                            [expected](unsigned char value) { return value == expected; });
            if (!valid) ++validation_failures;
            ++completed;
        }
        if (completions.empty()) std::this_thread::yield();
    }

    if (!ring.wait_all(&error)) {
        std::cerr << "wait failed: " << error << '\n';
        return 1;
    }
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    const double gbps = seconds > 0.0
        ? (static_cast<double>(options.bytes) * options.frames) / seconds / 1.0e9
        : 0.0;
    std::cout << "completed=" << completed
              << " validation_failures=" << validation_failures
              << " elapsed=" << seconds << " s"
              << " throughput=" << gbps << " GB/s\n";
    return validation_failures == 0 ? 0 : 1;
}
