#include "mgpu/p2p_transport.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

struct Options {
    int source = 0;
    int destination = 1;
    std::size_t bytes = 1920ULL * 1080ULL * 4ULL;
    int slots = 3;
    int frames = 120;
    int stall_timeout_ms = 5000;
    bool json = false;
};

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
            if (!parse_int(argv[++i], &options->stall_timeout_ms)) return false;
        } else {
            return false;
        }
    }
    return options->source >= 0 && options->destination >= 0 &&
           options->source != options->destination && options->bytes > 0 &&
           options->slots >= 2 && options->frames > 0 &&
           options->stall_timeout_ms > 0;
}

void print_report(const mgpu::CpuSyncReport& report, bool json) {
    if (json) {
        std::cout << "{\"source\":" << report.source
                  << ",\"destination\":" << report.destination
                  << ",\"bytes\":" << report.bytes
                  << ",\"slots\":" << report.slots
                  << ",\"frames\":" << report.frames
                  << ",\"completed\":" << report.completed
                  << ",\"stall_timeout_ms\":" << report.stall_timeout_ms
                  << ",\"peer_enabled\":" << (report.peer_enabled ? "true" : "false")
                  << ",\"validation_passed\":"
                  << (report.validation_passed ? "true" : "false")
                  << ",\"seconds\":" << std::setprecision(12) << report.seconds
                  << ",\"gigabytes_per_second\":" << report.gigabytes_per_second
                  << ",\"error\":\"" << report.error << "\"}\n";
        return;
    }
    std::cout << "CPU-gated P2P: " << report.source << " -> " << report.destination
              << ", frames=" << report.completed << "/" << report.frames
              << ", slots=" << report.slots
              << ", timeout=" << report.stall_timeout_ms << " ms"
              << ", peer=" << (report.peer_enabled ? "yes" : "no")
              << ", validation=" << (report.validation_passed ? "ok" : "FAIL") << '\n';
    if (!report.error.empty()) std::cout << "error: " << report.error << '\n';
    if (report.validation_passed)
        std::cout << std::fixed << std::setprecision(3)
                  << "throughput=" << report.gigabytes_per_second << " GB/s\n";
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        std::cerr << "invalid arguments; use --help\n";
        return 2;
    }
    const auto report = mgpu::benchmark_cpu_synchronized_ring(
            options.source, options.destination, options.bytes, options.slots,
            options.frames, options.stall_timeout_ms);
    print_report(report, options.json);
    return report.validation_passed ? 0 : 1;
}
