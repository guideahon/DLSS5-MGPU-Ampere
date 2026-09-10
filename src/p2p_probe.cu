#include "mgpu/p2p_transport.hpp"

#include <cuda_runtime.h>

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    int source = 0;
    int destination = 1;
    std::size_t bytes = 1920ULL * 1080ULL * 4ULL;
    int warmup = 10;
    int iterations = 100;
    bool json = false;
};

void usage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --source N       source CUDA GPU, default 0\n"
        << "  --destination N  destination CUDA GPU, default 1\n"
        << "  --bytes N        bytes per copy, default 8294400 (1080p RGBA8)\n"
        << "  --warmup N       warmup copies, default 10\n"
        << "  --iterations N   measured copies, default 100\n"
        << "  --json           emit machine-readable JSON\n"
        << "  --help           show this help\n";
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
        const std::string argument(argv[i]);
        if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else if (argument == "--json") {
            options->json = true;
        } else if (argument == "--source") {
            if (i + 1 >= argc || !parse_int(argv[++i], &options->source)) return false;
        } else if (argument == "--destination") {
            if (i + 1 >= argc || !parse_int(argv[++i], &options->destination)) return false;
        } else if (argument == "--bytes") {
            if (i + 1 >= argc || !parse_size(argv[++i], &options->bytes)) return false;
        } else if (argument == "--warmup") {
            if (i + 1 >= argc || !parse_int(argv[++i], &options->warmup)) return false;
        } else if (argument == "--iterations") {
            if (i + 1 >= argc || !parse_int(argv[++i], &options->iterations)) return false;
        } else {
            return false;
        }
    }
    return options->source >= 0 && options->destination >= 0 &&
           options->source != options->destination && options->bytes > 0 &&
           options->warmup >= 0 && options->iterations > 0;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char character : value) {
        if (character == '\\') out << "\\\\";
        else if (character == '"') out << "\\\"";
        else if (character == '\n') out << "\\n";
        else out << character;
    }
    return out.str();
}

void print_report(const mgpu::DirectionReport& report, bool json) {
    if (json) {
        std::cout << "{\"source\":" << report.source
                  << ",\"destination\":" << report.destination
                  << ",\"peer_possible\":" << (report.peer_possible ? "true" : "false")
                  << ",\"peer_enabled\":" << (report.peer_enabled ? "true" : "false")
                  << ",\"validation_passed\":" << (report.validation_passed ? "true" : "false")
                  << ",\"bytes\":" << report.bytes
                  << ",\"iterations\":" << report.iterations
                  << ",\"seconds\":" << std::setprecision(12) << report.seconds
                  << ",\"gigabytes_per_second\":" << report.gigabytes_per_second
                  << ",\"microseconds_per_copy\":" << report.microseconds_per_copy
                  << ",\"error\":\"" << json_escape(report.error) << "\"}\n";
        return;
    }
    std::cout << "  " << report.source << " -> " << report.destination
              << ": peer_possible=" << (report.peer_possible ? "yes" : "no")
              << ", peer_enabled=" << (report.peer_enabled ? "yes" : "no")
              << ", validation=" << (report.validation_passed ? "ok" : "FAIL") << '\n';
    if (!report.error.empty()) std::cout << "    error: " << report.error << '\n';
    if (report.validation_passed) {
        std::cout << "    " << std::fixed << std::setprecision(3)
                  << report.gigabytes_per_second << " GB/s, "
                  << report.microseconds_per_copy << " us/copy\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        usage(argv[0]);
        return 2;
    }

    std::string error;
    const auto devices = mgpu::enumerate_devices(&error);
    if (devices.empty()) {
        std::cerr << "CUDA device enumeration failed: " << error << '\n';
        return 1;
    }

    if (!options.json) {
        int driver_version = 0;
        int runtime_version = 0;
        cudaDriverGetVersion(&driver_version);
        cudaRuntimeGetVersion(&runtime_version);
        std::cout << "DLSS5-MGPU CUDA P2P probe\n"
                  << "CUDA driver API: " << driver_version << "\n"
                  << "CUDA runtime: " << runtime_version << "\n"
                  << "Devices: " << devices.size() << "\n";
        for (const auto& device : devices) {
            std::cout << "  GPU" << device.index << ": " << device.name
                      << " SM " << device.major << '.' << device.minor
                      << " PCI " << std::hex << std::setfill('0')
                      << std::setw(4) << device.pci_domain << ':'
                      << std::setw(2) << device.pci_bus << ':'
                      << std::setw(2) << device.pci_device << std::dec
                      << ", VRAM " << (device.total_memory / (1024 * 1024)) << " MiB\n";
        }
    }

    if (options.source >= static_cast<int>(devices.size()) ||
        options.destination >= static_cast<int>(devices.size())) {
        std::cerr << "GPU index is outside the CUDA device list\n";
        return 2;
    }

    const auto forward = mgpu::benchmark_direction(
        options.source, options.destination, options.bytes,
        options.warmup, options.iterations);
    const auto reverse = mgpu::benchmark_direction(
        options.destination, options.source, options.bytes,
        options.warmup, options.iterations);

    if (options.json) {
        std::cout << "{\"source_gpu\":" << options.source
                  << ",\"destination_gpu\":" << options.destination
                  << ",\"bytes\":" << options.bytes
                  << ",\"warmup\":" << options.warmup
                  << ",\"iterations\":" << options.iterations
                  << ",\"forward\":";
        print_report(forward, true);
        std::cout << ",\"reverse\":";
        print_report(reverse, true);
        std::cout << "}\n";
    } else {
        std::cout << "\nMeasured payload: " << options.bytes << " bytes\n";
        print_report(forward, false);
        print_report(reverse, false);
    }

    return (forward.validation_passed && reverse.validation_passed) ? 0 : 1;
}
