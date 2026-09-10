#pragma once

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mgpu {

struct DeviceInfo {
    int index = -1;
    std::string name;
    int major = 0;
    int minor = 0;
    int pci_domain = 0;
    int pci_bus = 0;
    int pci_device = 0;
    std::size_t total_memory = 0;
};

struct DirectionReport {
    int source = -1;
    int destination = -1;
    bool peer_possible = false;
    bool peer_enabled = false;
    bool validation_passed = false;
    std::size_t bytes = 0;
    int iterations = 0;
    double seconds = 0.0;
    double gigabytes_per_second = 0.0;
    double microseconds_per_copy = 0.0;
    std::string error;
};

struct RingCompletion {
    int slot = -1;
    std::uint64_t frame_id = 0;
};

struct CpuSyncReport {
    int source = -1;
    int destination = -1;
    std::size_t bytes = 0;
    int slots = 0;
    int frames = 0;
    int completed = 0;
    int stall_timeout_ms = 0;
    bool peer_enabled = false;
    bool validation_passed = false;
    double seconds = 0.0;
    double gigabytes_per_second = 0.0;
    std::string error;
};

std::vector<DeviceInfo> enumerate_devices(std::string* error = nullptr);

class P2PTransport {
public:
    P2PTransport() = default;
    ~P2PTransport();

    P2PTransport(const P2PTransport&) = delete;
    P2PTransport& operator=(const P2PTransport&) = delete;

    bool initialize(int source, int destination, std::size_t bytes,
                    std::string* error = nullptr);
    bool fill_source(std::uint8_t value, std::string* error = nullptr);
    bool copy_source_to_destination(std::string* error = nullptr);
    bool validate_destination(std::uint8_t expected, std::uint64_t* errors,
                              std::string* error = nullptr);

    int source() const { return source_; }
    int destination() const { return destination_; }
    std::size_t bytes() const { return bytes_; }
    bool peer_enabled() const { return peer_enabled_; }

private:
    void release();

    int source_ = -1;
    int destination_ = -1;
    std::size_t bytes_ = 0;
    unsigned char* source_buffer_ = nullptr;
    unsigned char* destination_buffer_ = nullptr;
    unsigned long long* error_counter_ = nullptr;
    cudaStream_t source_stream_ = nullptr;
    cudaStream_t destination_stream_ = nullptr;
    bool peer_enabled_ = false;
};

DirectionReport benchmark_direction(int source, int destination,
                                     std::size_t bytes, int warmup,
                                     int iterations);

CpuSyncReport benchmark_cpu_synchronized_ring(int source, int destination,
                                               std::size_t bytes, int slots,
                                               int frames, int stall_timeout_ms);

class AsyncP2PRing {
public:
    AsyncP2PRing() = default;
    ~AsyncP2PRing();

    AsyncP2PRing(const AsyncP2PRing&) = delete;
    AsyncP2PRing& operator=(const AsyncP2PRing&) = delete;

    bool initialize(int source, int destination, std::size_t bytes,
                    int slot_count, std::string* error = nullptr);
    int acquire_slot();
    unsigned char* source_buffer(int slot);
    unsigned char* destination_buffer(int slot);
    bool fill_source(int slot, std::uint8_t value, std::string* error = nullptr);
    bool submit_copy(int slot, std::uint64_t frame_id, std::string* error = nullptr);
    bool poll(std::vector<RingCompletion>* completed, std::string* error = nullptr);
    bool wait_all(std::string* error = nullptr);

    int source() const { return source_; }
    int destination() const { return destination_; }
    std::size_t bytes() const { return bytes_; }
    int slot_count() const { return static_cast<int>(slots_.size()); }
    bool peer_enabled() const { return peer_enabled_; }

private:
    struct Slot {
        unsigned char* source = nullptr;
        unsigned char* destination = nullptr;
        cudaEvent_t done = nullptr;
        std::uint64_t frame_id = 0;
        bool in_flight = false;
    };

    void release();

    int source_ = -1;
    int destination_ = -1;
    std::size_t bytes_ = 0;
    cudaStream_t source_stream_ = nullptr;
    cudaStream_t destination_stream_ = nullptr;
    std::vector<Slot> slots_;
    bool peer_enabled_ = false;
};

} // namespace mgpu
