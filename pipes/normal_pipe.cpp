// =============================================================================
// Pipe IPC Benchmark — Comparison Baseline for Project P03
// Project P03: Memory-Safe IPC Using io_uring and Shared Ring Buffers
//
// Design identical to io_uring_bench.cpp for a fair comparison:
//   same message sizes, same TOTAL_BYTES, same NUM_RUNS, same statistics.
//
// Latency design (matches io_uring_bench.cpp):
//   send_ns is embedded in MessageHeader — producer fills it before write().
//   Consumer reads hdr.send_ns DIRECTLY from the wire — no shared array,
//   no index mismatch, no race condition.
//   Latency = (recv_ns - hdr.send_ns) / 1000.0  [microseconds]
//
// Expected result from literature (Paper 1 SYSTOR '22):
//   Pipe incurs 2 syscalls per message (write + read).
//   io_uring SQPOLL incurs 0 syscalls per message.
//   Gap should widen as message size grows and syscall overhead amortises less.
// =============================================================================

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <cmath>

// =============================================================================
// Configuration — MUST match io_uring_bench.cpp exactly
// =============================================================================

constexpr int    PRODUCER_CORE = 1;
constexpr int    CONSUMER_CORE = 2;

constexpr size_t MESSAGE_SIZES[] = {
    64, 256, 1024, 4096, 16384, 65536, 262144, 1048576
};
constexpr int    NUM_RUNS        = 5;
constexpr size_t TOTAL_BYTES     = 2ULL * 1024 * 1024 * 1024;
constexpr size_t MAX_PAYLOAD     = 1048576;
constexpr size_t MAX_LAT_SAMPLES = 4 * 1024 * 1024;

// =============================================================================
// Wire format
// =============================================================================
// send_ns is in the header — consumer reads it from the pipe, no shared array.

struct MessageHeader {
    uint64_t send_ns;       // set by producer, read by consumer off the wire
    uint32_t payload_size;
};

// =============================================================================
// Telemetry — written ONLY by consumer (no races)
// =============================================================================

struct Telemetry {
    double   latencies[MAX_LAT_SAMPLES];
    uint64_t latency_count;
    double   execution_time_sec;
};

// =============================================================================
// Helpers
// =============================================================================

static void set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0)
        std::perror("sched_setaffinity (non-fatal on VMs)");
}

static inline uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now()
            .time_since_epoch().count());
}

// Loop until all n bytes are written (pipe write() can return short)
static bool write_all(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w; n -= w;
    }
    return true;
}

// Loop until all n bytes are read
static bool read_all(int fd, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

// =============================================================================
// Producer
// =============================================================================

void run_producer(int write_fd, size_t msg_sz) {
    set_affinity(PRODUCER_CORE);

    // Combine header + payload into one wire buffer to minimise write() calls
    std::vector<char> wire(sizeof(MessageHeader) + msg_sz);
    auto* hdr = reinterpret_cast<MessageHeader*>(wire.data());
    char* dat = wire.data() + sizeof(MessageHeader);

    // Fill payload once
    std::memset(dat, 'X', msg_sz);
    hdr->payload_size = static_cast<uint32_t>(msg_sz);

    size_t produced = 0;

    while (produced < TOTAL_BYTES) {
        // Stamp immediately before the syscall — this is what we are measuring.
        hdr->send_ns = now_ns();

        if (!write_all(write_fd, wire.data(), wire.size())) {
            std::cerr << "[Producer] write() failed\n";
            break;
        }
        produced += msg_sz;
    }

    close(write_fd);
}

// =============================================================================
// Consumer
// =============================================================================

void run_consumer(int read_fd, Telemetry* tel, size_t msg_sz) {
    set_affinity(CONSUMER_CORE);

    std::vector<char> payload_buf(msg_sz);
    MessageHeader     hdr;
    size_t   consumed = 0;
    uint64_t lat_idx  = 0;

    auto wall_start = std::chrono::high_resolution_clock::now();

    while (consumed < TOTAL_BYTES) {

        // Read header — blocks until producer writes
        if (!read_all(read_fd, &hdr, sizeof(hdr))) break;

        // Read payload BEFORE recording recv_ns so that latency covers the
        // full message (header + payload), not just the header arrival.
        // For 1 MB messages the payload read takes significant time; capturing
        // recv_ns before it would silently understate latency by milliseconds.
        if (!read_all(read_fd, payload_buf.data(), hdr.payload_size)) break;

        // Capture receive time AFTER full message has arrived.
        uint64_t recv_ns = now_ns();

        // Cache-line stride checksum — mirrors io_uring_bench.cpp consumer
        volatile char checksum = 0;
        for (size_t i = 0; i < hdr.payload_size; i += 64)
            checksum += payload_buf[i];
        (void)checksum;

        // Compute latency from the timestamp that arrived ON THE WIRE.
        // hdr.send_ns was set by the producer in this same process run.
        // No shared array, no index, no race.
        if (lat_idx < MAX_LAT_SAMPLES) {
            double lat_us = static_cast<double>(recv_ns - hdr.send_ns) / 1000.0;
            tel->latencies[lat_idx++] = lat_us;
        }

        consumed += hdr.payload_size;
    }

    auto wall_end = std::chrono::high_resolution_clock::now();
    tel->execution_time_sec =
        std::chrono::duration<double>(wall_end - wall_start).count();
    tel->latency_count = lat_idx;

    close(read_fd);
}

// =============================================================================
// Statistics — identical to io_uring_bench.cpp
// =============================================================================

struct Stats {
    double avg, stddev, p50, p95, p99, throughput_gbps;
};

static Stats compute_stats(Telemetry* tel) {
    Stats s{};
    if (tel->latency_count == 0) return s;

    std::vector<double> lats(
        tel->latencies,
        tel->latencies + tel->latency_count);

    lats.erase(
        std::remove_if(lats.begin(), lats.end(),
                       [](double v){ return v < 0.0; }),
        lats.end());
    if (lats.empty()) return s;

    std::sort(lats.begin(), lats.end());
    double sum = std::accumulate(lats.begin(), lats.end(), 0.0);
    s.avg = sum / lats.size();

    double var = 0.0;
    for (double v : lats) var += (v - s.avg) * (v - s.avg);
    s.stddev = std::sqrt(var / lats.size());

    s.p50 = lats[lats.size() * 50 / 100];
    s.p95 = lats[lats.size() * 95 / 100];
    s.p99 = lats[lats.size() * 99 / 100];

    s.throughput_gbps =
        (TOTAL_BYTES / (1024.0 * 1024.0 * 1024.0)) /
        tel->execution_time_sec;

    return s;
}

static void log_csv(size_t msg_sz, int run, const Stats& s) {
    std::ofstream csv("pipe_results.csv", std::ios::app);
    csv << msg_sz   << "," << run      << ","
        << s.throughput_gbps << ","
        << s.avg    << "," << s.stddev << ","
        << s.p50    << "," << s.p95   << "," << s.p99 << "\n";
}

// =============================================================================
// Main
// =============================================================================

int main() {

    auto* tel = static_cast<Telemetry*>(
        mmap(nullptr, sizeof(Telemetry),
             PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));

    if (tel == MAP_FAILED) { std::perror("mmap"); return 1; }

    {
        std::ofstream csv("pipe_results.csv");
        csv << "message_size_bytes,run,throughput_gbps,"
               "avg_latency_us,stddev_us,p50_us,p95_us,p99_us\n";
    }

    std::cout << "\n[Pipe IPC Benchmark — Comparison Baseline]\n";
    std::cout << "Total transfer per run : "
              << TOTAL_BYTES / (1024*1024) << " MB\n";
    std::cout << "IPC mechanism          : pipe() — 2 syscalls per message\n";
    std::cout << "Runs per message size  : " << NUM_RUNS << " + 1 warmup\n\n";

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "=========================================\n";
        std::cout << "Message Size : " << sz << " bytes\n";
        std::cout << "=========================================\n";

        for (int run = 0; run <= NUM_RUNS; ++run) {

            int pipefd[2];
            if (pipe(pipefd) < 0) { std::perror("pipe"); return 1; }

            // Expand pipe buffer to max payload size so large messages don't
            // stall inside the kernel buffer — we want syscall overhead, not
            // artificial pipe-buffer throttling.
            fcntl(pipefd[1], F_SETPIPE_SZ, static_cast<int>(MAX_PAYLOAD));

            std::memset(tel, 0, sizeof(Telemetry));

            pid_t pid = fork();
            if (pid == 0) {
                close(pipefd[1]);
                run_consumer(pipefd[0], tel, sz);
                _Exit(0);
            } else if (pid > 0) {
                close(pipefd[0]);
                run_producer(pipefd[1], sz);
                waitpid(pid, nullptr, 0);
            } else {
                std::perror("fork"); return 1;
            }

            Stats s = compute_stats(tel);

            if (run == 0) {
                std::cout << "  [Warmup]  throughput=" << s.throughput_gbps
                          << " GB/s  (discarded)\n";
            } else {
                std::cout << "  Run " << run
                          << "  throughput=" << s.throughput_gbps << " GB/s"
                          << "  avg_lat="   << s.avg    << " us"
                          << "  stddev="    << s.stddev << " us"
                          << "  p50="       << s.p50    << " us"
                          << "  p99="       << s.p99    << " us\n";
                log_csv(sz, run, s);
            }
        }
    }

    munmap(tel, sizeof(Telemetry));
    std::cout << "\nResults written to pipe_results.csv\n";
    return 0;
}

// =============================================================================
// Build:
//   g++ -O2 -std=c++17 -o pipe_bench pipe_bench.cpp
//   ./pipe_bench
// =============================================================================