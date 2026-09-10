#include "mgpu/p2p_transport.hpp"

#include <cuda_runtime.h>

#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>

namespace mgpu {
namespace {

std::string cuda_error(const char* operation, cudaError_t status) {
    std::ostringstream out;
    out << operation << ": " << cudaGetErrorName(status) << " - "
        << cudaGetErrorString(status);
    return out.str();
}

bool check(cudaError_t status, const char* operation, std::string* error) {
    if (status == cudaSuccess) return true;
    if (error) *error = cuda_error(operation, status);
    return false;
}

__global__ void validate_kernel(const unsigned char* data, std::size_t bytes,
                                unsigned char expected,
                                unsigned long long* errors) {
    const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index < bytes && data[index] != expected) atomicAdd(errors, 1ULL);
}

bool enable_peer(int owner, int peer, bool* enabled, std::string* error) {
    int possible = 0;
    if (!check(cudaDeviceCanAccessPeer(&possible, owner, peer),
               "cudaDeviceCanAccessPeer", error)) return false;
    if (!possible) {
        if (enabled) *enabled = false;
        return true;
    }

    if (!check(cudaSetDevice(owner), "cudaSetDevice(owner)", error)) return false;
    const cudaError_t status = cudaDeviceEnablePeerAccess(peer, 0);
    if (status != cudaSuccess && status != cudaErrorPeerAccessAlreadyEnabled) {
        if (error) *error = cuda_error("cudaDeviceEnablePeerAccess", status);
        return false;
    }
    if (status == cudaErrorPeerAccessAlreadyEnabled) {
        // cudaDeviceEnablePeerAccess reports an already-enabled link through
        // the CUDA error state. Clear that expected status before launching
        // the first validation kernel on this context.
        cudaGetLastError();
    }
    if (enabled) *enabled = true;
    return true;
}

} // namespace

std::vector<DeviceInfo> enumerate_devices(std::string* error) {
    int count = 0;
    if (!check(cudaGetDeviceCount(&count), "cudaGetDeviceCount", error)) return {};

    std::vector<DeviceInfo> devices;
    devices.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        cudaDeviceProp prop{};
        if (!check(cudaGetDeviceProperties(&prop, index),
                   "cudaGetDeviceProperties", error)) return {};

        DeviceInfo info;
        info.index = index;
        info.name = prop.name;
        info.major = prop.major;
        info.minor = prop.minor;
        info.pci_domain = prop.pciDomainID;
        info.pci_bus = prop.pciBusID;
        info.pci_device = prop.pciDeviceID;
        info.total_memory = prop.totalGlobalMem;
        devices.push_back(std::move(info));
    }
    return devices;
}

P2PTransport::~P2PTransport() { release(); }

bool P2PTransport::initialize(int source, int destination, std::size_t bytes,
                              std::string* error) {
    release();
    source_ = source;
    destination_ = destination;
    bytes_ = bytes;

    if (source < 0 || destination < 0 || source == destination || bytes == 0) {
        if (error) *error = "source/destination invalid or buffer size is zero";
        release();
        return false;
    }

    int count = 0;
    if (!check(cudaGetDeviceCount(&count), "cudaGetDeviceCount", error) ||
        source >= count || destination >= count) {
        if (error && error->empty()) *error = "GPU index is outside the CUDA device list";
        release();
        return false;
    }

    if (!check(cudaSetDevice(source), "cudaSetDevice(source)", error) ||
        !check(cudaMalloc(reinterpret_cast<void**>(&source_buffer_), bytes),
               "cudaMalloc(source)", error) ||
        !check(cudaStreamCreateWithFlags(&source_stream_, cudaStreamNonBlocking),
               "cudaStreamCreateWithFlags(source)", error)) {
        release();
        return false;
    }

    if (!check(cudaSetDevice(destination), "cudaSetDevice(destination)", error) ||
        !check(cudaMalloc(reinterpret_cast<void**>(&destination_buffer_), bytes),
               "cudaMalloc(destination)", error) ||
        !check(cudaMalloc(reinterpret_cast<void**>(&error_counter_), sizeof(*error_counter_)),
               "cudaMalloc(error counter)", error) ||
        !check(cudaStreamCreateWithFlags(&destination_stream_, cudaStreamNonBlocking),
               "cudaStreamCreateWithFlags(destination)", error)) {
        release();
        return false;
    }

    bool source_to_destination = false;
    bool destination_to_source = false;
    if (!enable_peer(source, destination, &source_to_destination, error) ||
        !enable_peer(destination, source, &destination_to_source, error)) {
        release();
        return false;
    }
    peer_enabled_ = source_to_destination && destination_to_source;

    return true;
}

bool P2PTransport::fill_source(std::uint8_t value, std::string* error) {
    if (!source_buffer_) {
        if (error) *error = "transport is not initialized";
        return false;
    }
    if (!check(cudaSetDevice(source_), "cudaSetDevice(source)", error)) return false;
    return check(cudaMemsetAsync(source_buffer_, value, bytes_, source_stream_),
                 "cudaMemsetAsync(source)", error) &&
           check(cudaStreamSynchronize(source_stream_), "cudaStreamSynchronize(source)", error);
}

bool P2PTransport::copy_source_to_destination(std::string* error) {
    if (!source_buffer_ || !destination_buffer_) {
        if (error) *error = "transport is not initialized";
        return false;
    }
    if (!check(cudaSetDevice(destination_), "cudaSetDevice(destination)", error)) return false;
    return check(cudaMemcpyPeerAsync(destination_buffer_, destination_, source_buffer_,
                                     source_, bytes_, destination_stream_),
                 "cudaMemcpyPeerAsync", error) &&
           check(cudaStreamSynchronize(destination_stream_), "cudaStreamSynchronize(copy)", error);
}

bool P2PTransport::validate_destination(std::uint8_t expected, std::uint64_t* errors,
                                        std::string* error) {
    if (!destination_buffer_ || !error_counter_) {
        if (error) *error = "transport is not initialized";
        return false;
    }
    if (!check(cudaSetDevice(destination_), "cudaSetDevice(destination)", error) ||
        !check(cudaMemsetAsync(error_counter_, 0, sizeof(*error_counter_), destination_stream_),
               "cudaMemsetAsync(error counter)", error)) return false;

    const int block_size = 256;
    const int grid_size = static_cast<int>((bytes_ + block_size - 1) / block_size);
    validate_kernel<<<grid_size, block_size, 0, destination_stream_>>>(
        destination_buffer_, bytes_, expected, error_counter_);
    if (!check(cudaGetLastError(), "validate_kernel launch", error) ||
        !check(cudaStreamSynchronize(destination_stream_), "cudaStreamSynchronize(validate)", error)) {
        return false;
    }

    unsigned long long host_errors = 0;
    if (!check(cudaMemcpy(&host_errors, error_counter_, sizeof(host_errors),
                          cudaMemcpyDeviceToHost),
               "cudaMemcpy(error counter)", error)) return false;
    if (errors) *errors = host_errors;
    return host_errors == 0;
}

void P2PTransport::release() {
    if (source_stream_) {
        if (source_ >= 0) cudaSetDevice(source_);
        cudaStreamDestroy(source_stream_);
        source_stream_ = nullptr;
    }
    if (destination_stream_) {
        if (destination_ >= 0) cudaSetDevice(destination_);
        cudaStreamDestroy(destination_stream_);
        destination_stream_ = nullptr;
    }
    if (source_ >= 0) {
        cudaSetDevice(source_);
        if (source_buffer_) cudaFree(source_buffer_);
    }
    if (destination_ >= 0) {
        cudaSetDevice(destination_);
        if (destination_buffer_) cudaFree(destination_buffer_);
        if (error_counter_) cudaFree(error_counter_);
    }
    source_buffer_ = nullptr;
    destination_buffer_ = nullptr;
    error_counter_ = nullptr;
    source_ = -1;
    destination_ = -1;
    bytes_ = 0;
    peer_enabled_ = false;
}

DirectionReport benchmark_direction(int source, int destination,
                                     std::size_t bytes, int warmup,
                                     int iterations) {
    DirectionReport report;
    report.source = source;
    report.destination = destination;
    report.bytes = bytes;
    report.iterations = iterations;

    int possible = 0;
    cudaError_t status = cudaDeviceCanAccessPeer(&possible, destination, source);
    if (status != cudaSuccess) {
        report.error = cuda_error("cudaDeviceCanAccessPeer", status);
        return report;
    }
    report.peer_possible = possible != 0;

    std::string error;
    P2PTransport transport;
    if (!transport.initialize(source, destination, bytes, &error)) {
        report.error = error;
        return report;
    }
    report.peer_enabled = transport.peer_enabled();

    constexpr std::uint8_t pattern = 0xA5;
    if (!transport.fill_source(pattern, &error)) {
        report.error = error;
        return report;
    }
    if (!transport.copy_source_to_destination(&error)) {
        report.error = error;
        return report;
    }
    std::uint64_t validation_errors = 0;
    report.validation_passed = transport.validate_destination(pattern, &validation_errors, &error);
    if (!report.validation_passed) {
        report.error = error.empty() ? "destination validation failed" : error;
        return report;
    }

    for (int i = 0; i < warmup; ++i) {
        if (!transport.copy_source_to_destination(&error)) {
            report.error = error;
            return report;
        }
    }

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        if (!transport.copy_source_to_destination(&error)) {
            report.error = error;
            return report;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    report.seconds = std::chrono::duration<double>(end - start).count();
    if (report.seconds > 0.0 && iterations > 0) {
        report.gigabytes_per_second =
            (static_cast<double>(bytes) * iterations) / report.seconds / 1.0e9;
        report.microseconds_per_copy = report.seconds * 1.0e6 / iterations;
    }
    return report;
}

AsyncP2PRing::~AsyncP2PRing() { release(); }

bool AsyncP2PRing::initialize(int source, int destination, std::size_t bytes,
                              int slot_count, std::string* error) {
    release();
    if (source < 0 || destination < 0 || source == destination || bytes == 0 || slot_count < 2) {
        if (error) *error = "invalid source, destination, buffer size, or slot count";
        return false;
    }
    source_ = source;
    destination_ = destination;
    bytes_ = bytes;

    int count = 0;
    if (!check(cudaGetDeviceCount(&count), "cudaGetDeviceCount", error) ||
        source >= count || destination >= count) {
        if (error && error->empty()) *error = "GPU index is outside the CUDA device list";
        release();
        return false;
    }

    bool source_to_destination = false;
    bool destination_to_source = false;
    if (!enable_peer(source, destination, &source_to_destination, error) ||
        !enable_peer(destination, source, &destination_to_source, error)) {
        release();
        return false;
    }
    peer_enabled_ = source_to_destination && destination_to_source;

    if (!check(cudaSetDevice(source_), "cudaSetDevice(source)", error) ||
        !check(cudaStreamCreateWithFlags(&source_stream_, cudaStreamNonBlocking),
               "cudaStreamCreateWithFlags(source)", error)) {
        release();
        return false;
    }
    if (!check(cudaSetDevice(destination_), "cudaSetDevice(destination)", error) ||
        !check(cudaStreamCreateWithFlags(&destination_stream_, cudaStreamNonBlocking),
               "cudaStreamCreateWithFlags(destination)", error)) {
        release();
        return false;
    }

    slots_.resize(static_cast<std::size_t>(slot_count));
    for (auto& slot : slots_) {
        if (!check(cudaSetDevice(source_), "cudaSetDevice(source)", error) ||
            !check(cudaMalloc(reinterpret_cast<void**>(&slot.source), bytes_),
                   "cudaMalloc(ring source)", error) ||
            !check(cudaSetDevice(destination_), "cudaSetDevice(destination)", error) ||
            !check(cudaMalloc(reinterpret_cast<void**>(&slot.destination), bytes_),
                   "cudaMalloc(ring destination)", error) ||
            !check(cudaEventCreateWithFlags(&slot.done, cudaEventDisableTiming),
                   "cudaEventCreateWithFlags", error)) {
            release();
            return false;
        }
    }
    return true;
}

int AsyncP2PRing::acquire_slot() {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (!slots_[index].in_flight) return static_cast<int>(index);
    }
    return -1;
}

unsigned char* AsyncP2PRing::source_buffer(int slot) {
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) return nullptr;
    return slots_[static_cast<std::size_t>(slot)].source;
}

unsigned char* AsyncP2PRing::destination_buffer(int slot) {
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) return nullptr;
    return slots_[static_cast<std::size_t>(slot)].destination;
}

bool AsyncP2PRing::fill_source(int slot, std::uint8_t value, std::string* error) {
    if (slot < 0 || slot >= static_cast<int>(slots_.size()) || slots_[slot].in_flight) {
        if (error) *error = "ring slot is invalid or still in flight";
        return false;
    }
    if (!check(cudaSetDevice(source_), "cudaSetDevice(source)", error) ||
        !check(cudaMemsetAsync(slots_[slot].source, value, bytes_, source_stream_),
               "cudaMemsetAsync(ring source)", error) ||
        !check(cudaStreamSynchronize(source_stream_), "cudaStreamSynchronize(ring source)", error)) {
        return false;
    }
    return true;
}

bool AsyncP2PRing::submit_copy(int slot, std::uint64_t frame_id, std::string* error) {
    if (slot < 0 || slot >= static_cast<int>(slots_.size()) || slots_[slot].in_flight) {
        if (error) *error = "ring slot is invalid or already in flight";
        return false;
    }
    if (!check(cudaSetDevice(destination_), "cudaSetDevice(destination)", error) ||
        !check(cudaMemcpyPeerAsync(slots_[slot].destination, destination_,
                                   slots_[slot].source, source_, bytes_,
                                   destination_stream_),
               "cudaMemcpyPeerAsync(ring)", error) ||
        !check(cudaEventRecord(slots_[slot].done, destination_stream_),
               "cudaEventRecord(ring)", error)) {
        return false;
    }
    slots_[slot].frame_id = frame_id;
    slots_[slot].in_flight = true;
    return true;
}

bool AsyncP2PRing::poll(std::vector<RingCompletion>* completed, std::string* error) {
    if (completed) completed->clear();
    if (!check(cudaSetDevice(destination_), "cudaSetDevice(destination)", error)) return false;
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        auto& slot = slots_[index];
        if (!slot.in_flight) continue;
        const cudaError_t status = cudaEventQuery(slot.done);
        if (status == cudaErrorNotReady) continue;
        if (status != cudaSuccess) {
            if (error) *error = cuda_error("cudaEventQuery(ring)", status);
            return false;
        }
        if (completed) completed->push_back(
            RingCompletion{static_cast<int>(index), slot.frame_id});
        slot.in_flight = false;
    }
    return true;
}

bool AsyncP2PRing::wait_all(std::string* error) {
    if (!check(cudaSetDevice(destination_), "cudaSetDevice(destination)", error)) return false;
    for (auto& slot : slots_) {
        if (!slot.in_flight) continue;
        if (!check(cudaEventSynchronize(slot.done), "cudaEventSynchronize(ring)", error)) {
            return false;
        }
        slot.in_flight = false;
    }
    return true;
}

void AsyncP2PRing::release() {
    if (destination_ >= 0) cudaSetDevice(destination_);
    for (auto& slot : slots_) {
        if (slot.done) cudaEventDestroy(slot.done);
        if (slot.destination) cudaFree(slot.destination);
    }
    if (source_ >= 0) cudaSetDevice(source_);
    for (auto& slot : slots_) {
        if (slot.source) cudaFree(slot.source);
    }
    slots_.clear();
    if (destination_stream_) {
        if (destination_ >= 0) cudaSetDevice(destination_);
        cudaStreamDestroy(destination_stream_);
    }
    if (source_stream_) {
        if (source_ >= 0) cudaSetDevice(source_);
        cudaStreamDestroy(source_stream_);
    }
    destination_stream_ = nullptr;
    source_stream_ = nullptr;
    source_ = -1;
    destination_ = -1;
    bytes_ = 0;
    peer_enabled_ = false;
}

CpuSyncReport benchmark_cpu_synchronized_ring(int source, int destination,
                                               std::size_t bytes, int slots,
                                               int frames, int stall_timeout_ms) {
    CpuSyncReport report;
    report.source = source;
    report.destination = destination;
    report.bytes = bytes;
    report.slots = slots;
    report.frames = frames;
    report.stall_timeout_ms = stall_timeout_ms;

    if (slots < 2 || frames <= 0 || bytes == 0 || stall_timeout_ms <= 0) {
        report.error = "invalid ring size, frame count, payload, or timeout";
        return report;
    }

    AsyncP2PRing ring;
    std::string error;
    if (!ring.initialize(source, destination, bytes, slots, &error)) {
        report.error = error;
        return report;
    }
    report.peer_enabled = ring.peer_enabled();

    constexpr std::size_t sample_size = 64;
    std::vector<unsigned char> sample(sample_size);
    int submitted = 0;
    auto last_progress = std::chrono::steady_clock::now();
    const auto stall_timeout = std::chrono::milliseconds(stall_timeout_ms);
    const auto start = last_progress;

    while (report.completed < frames) {
        while (submitted < frames) {
            const int slot = ring.acquire_slot();
            if (slot < 0) break;
            const auto pattern = static_cast<std::uint8_t>(submitted & 0xff);
            if (!ring.fill_source(slot, pattern, &error) ||
                !ring.submit_copy(slot, static_cast<std::uint64_t>(submitted), &error)) {
                report.error = error;
                return report;
            }
            ++submitted;
        }

        std::vector<RingCompletion> completions;
        if (!ring.poll(&completions, &error)) {
            report.error = error;
            return report;
        }
        if (!completions.empty()) {
            if (!check(cudaSetDevice(destination), "cudaSetDevice(destination)", &error)) {
                report.error = error;
                return report;
            }
            for (const auto& completion : completions) {
                if (!check(cudaMemcpy(sample.data(), ring.destination_buffer(completion.slot),
                                      sample.size(), cudaMemcpyDeviceToHost),
                           "cudaMemcpy(cpu sync validation)", &error)) {
                    report.error = error;
                    return report;
                }
                const auto expected = static_cast<unsigned char>(completion.frame_id & 0xff);
                for (const auto value : sample) {
                    if (value != expected) {
                        report.error = "CPU-gated P2P validation checksum mismatch";
                        return report;
                    }
                }
                ++report.completed;
            }
            last_progress = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - last_progress > stall_timeout) {
            report.error = "CPU-gated P2P completion timeout";
            return report;
        } else {
            std::this_thread::yield();
        }
    }

    const auto end = std::chrono::steady_clock::now();
    report.seconds = std::chrono::duration<double>(end - start).count();
    report.validation_passed = true;
    if (report.seconds > 0.0)
        report.gigabytes_per_second =
                (static_cast<double>(bytes) * report.completed) / report.seconds / 1.0e9;
    return report;
}

} // namespace mgpu
