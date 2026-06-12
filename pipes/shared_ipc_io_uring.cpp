// =============================================================================
// io_uring + Shared Ring Buffer IPC Benchmark
// Project P03: Memory-Safe IPC Using io_uring and Shared Ring Buffers
//
// Literature grounding:
//  [Paper 1] Didona et al., SYSTOR '22 — iou+k needs a dedicated core per
//            kernel polling thread or throughput collapses (Fig 2a, Lesson 1).
//            Zero-syscall submission path described in Section 3.1.
//  [Paper 2] Ren & Trivedi, CHEOPS '23 — polling improves throughput ~1.7x
//            but costs ~2.3x CPU instructions (Section 3.1.1/3.1.2).
//
// Latency design:
//   send_ns is embedded INSIDE each ring slot by the producer.
//   The consumer reads send_ns directly from the slot it just dequeued —
//   no shared index, no race condition, no synchronisation needed.
//   Latency = (recv_ns - slot.send_ns) / 1000.0  [microseconds]
//
// VM note: SQPOLL may not be available without CAP_SYS_ADMIN. The code
//   falls back to standard io_uring automatically.
// =============================================================================

#include <liburing.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <atomic>
#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <cmath>
// Cross-platform CPU pause: reduces pipeline pressure during busy-wait.
// x86: PAUSE instruction. ARM: YIELD instruction. Falls back to nothing.
#if defined(__x86_64__) || defined(__i386__)
#  include <immintrin.h>
#  define CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#  define CPU_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#  define CPU_PAUSE() ((void)0)
#endif

// =============================================================================
// Configuration
// =============================================================================

constexpr int    PRODUCER_CORE = 1;
constexpr int    CONSUMER_CORE = 2;
constexpr int    SQPOLL_CORE   = 3;  // Dedicated core for SQPOLL kernel thread
                                      // Paper 1 Sec 3.1: must be separate from
                                      // app thread or latency jumps to ~8 ms

constexpr size_t MESSAGE_SIZES[] = {
    64, 256, 1024, 4096, 16384, 65536, 262144, 1048576
};
constexpr int    NUM_RUNS       = 5;
constexpr size_t TOTAL_BYTES    = 2ULL * 1024 * 1024 * 1024;  // 2 GB per run
constexpr size_t NUM_SLOTS      = 64;
constexpr size_t MAX_PAYLOAD    = 1048576;
constexpr size_t MAX_LAT_SAMPLES = 4 * 1024 * 1024;  // 4M samples max

// =============================================================================
// Shared Ring Buffer
// =============================================================================
// Cache-line aligned to prevent false sharing between head and tail.
// send_ns lives INSIDE each slot — producer writes it, consumer reads it back.
// This is the only correct way to measure latency across two concurrent
// processes without a shared index that can race.

struct alignas(64) RingBuffer {

    alignas(64) std::atomic<uint64_t> head;  // written only by producer
    alignas(64) std::atomic<uint64_t> tail;  // written only by consumer

    struct alignas(64) Slot {
        uint64_t send_ns;          // timestamp written by producer into slot
        uint32_t size;
        char     data[MAX_PAYLOAD];
    };

    Slot slots[NUM_SLOTS];
};

// =============================================================================
// Telemetry — written ONLY by the consumer (one writer, no races)
// =============================================================================

struct Telemetry {
    double   latencies[MAX_LAT_SAMPLES]; // computed (recv_ns - send_ns)/1000
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

// =============================================================================
// Producer
// =============================================================================
// Uses io_uring SQPOLL: kernel thread polls SQ — zero syscall per message.
// Paper 1 Fig 1c / Paper 2 Sec 2.2 "iou-s": eliminates io_uring_enter() cost.

void run_producer(RingBuffer* rb, size_t msg_sz) {
    set_affinity(PRODUCER_CORE);

    struct io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    params.flags          = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
    params.sq_thread_cpu  = SQPOLL_CORE;
    params.sq_thread_idle = 100000;  // park kernel thread after 100 ms idle

    struct io_uring ring;
    bool sqpoll_active = (io_uring_queue_init_params(256, &ring, &params) == 0);
    if (!sqpoll_active) {
        std::cerr << "[Producer] SQPOLL unavailable (needs CAP_SYS_ADMIN), "
                     "falling back to standard io_uring\n";
        std::memset(&params, 0, sizeof(params));
        io_uring_queue_init_params(256, &ring, &params);
    }

    std::vector<char> payload(msg_sz, 'X');
    size_t produced = 0;

    while (produced < TOTAL_BYTES) {

        uint64_t h = rb->head.load(std::memory_order_relaxed);
        uint64_t t = rb->tail.load(std::memory_order_acquire);

        // Busy-wait with CPU pause hint.
        // Paper 2 Sec 3.1: polling libraries intentionally spin; CPU_PAUSE()
        // reduces pipeline pressure and memory ordering violations.
        if ((h - t) >= NUM_SLOTS) {
            CPU_PAUSE();
            continue;
        }

        auto& slot = rb->slots[h % NUM_SLOTS];
        std::memcpy(slot.data, payload.data(), msg_sz);
        slot.size = static_cast<uint32_t>(msg_sz);

        // Stamp AFTER data copy, BEFORE making slot visible to consumer.
        // Consumer reads this exact value from the same slot — no shared
        // index, no race condition possible.
        slot.send_ns = now_ns();

        // Publish slot with release: guarantees data + send_ns are visible
        // to the consumer before it sees the new head value.
        rb->head.store(h + 1, std::memory_order_release);

        // Submit NOP to exercise the SQPOLL zero-syscall submission path.
        // Paper 1 Sec 3.1 / Paper 2 Sec 2.2: with SQPOLL the kernel thread
        // picks up SQEs without any io_uring_enter() call from userspace.
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (sqe) {
            io_uring_prep_nop(sqe);
            if (!sqpoll_active)
                io_uring_submit(&ring);

            struct io_uring_cqe* cqe;
            if (io_uring_peek_cqe(&ring, &cqe) == 0)
                io_uring_cqe_seen(&ring, cqe);
        }

        produced += msg_sz;
    }

    io_uring_queue_exit(&ring);
}

// =============================================================================
// Consumer
// =============================================================================
// Completion polling — no interrupts, no syscalls in the hot path.
// Paper 2 Sec 3.1.1: "iou-c (completion polling) has 34-39% higher IOPS
// than default iou."
// Reads send_ns DIRECTLY from the dequeued slot — single source of truth,
// no shared array index needed.

void run_consumer(RingBuffer* rb, Telemetry* tel) {
    set_affinity(CONSUMER_CORE);

    size_t   consumed = 0;
    uint64_t lat_idx  = 0;

    auto wall_start = std::chrono::high_resolution_clock::now();

    while (consumed < TOTAL_BYTES) {

        uint64_t t = rb->tail.load(std::memory_order_relaxed);
        uint64_t h = rb->head.load(std::memory_order_acquire);

        if (t == h) {
            CPU_PAUSE();
            continue;
        }

        // Record receive time immediately after acquire load confirms slot
        // is visible — this is the earliest possible receive timestamp.
        uint64_t recv_ns = now_ns();

        auto& slot = rb->slots[t % NUM_SLOTS];

        // Read send_ns FROM THE SLOT — written by producer into this exact
        // slot before publishing. No index mismatch possible.
        uint64_t send_ns = slot.send_ns;

        // Cache-line stride read to simulate realistic consumer work.
        // Paper 2 Sec 3.1.3: ensures data flows through CPU caches.
        volatile char checksum = 0;
        for (size_t i = 0; i < slot.size; i += 64)
            checksum += slot.data[i];
        (void)checksum;

        // Store computed latency (microseconds). Consumer is the ONLY writer
        // to tel->latencies so there is no race even without locks.
        if (lat_idx < MAX_LAT_SAMPLES) {
            double lat_us = static_cast<double>(recv_ns - send_ns) / 1000.0;
            tel->latencies[lat_idx++] = lat_us;
        }

        consumed += slot.size;
        rb->tail.store(t + 1, std::memory_order_release);
    }

    auto wall_end = std::chrono::high_resolution_clock::now();
    tel->execution_time_sec =
        std::chrono::duration<double>(wall_end - wall_start).count();
    tel->latency_count = lat_idx;
}

// =============================================================================
// Statistics
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

    // Drop negatives (clock skew artifacts — rare but possible in VMs)
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
    std::ofstream csv("io_uring_results.csv", std::ios::app);
    csv << msg_sz   << "," << run      << ","
        << s.throughput_gbps << ","
        << s.avg    << "," << s.stddev << ","
        << s.p50    << "," << s.p95   << "," << s.p99 << "\n";
}

// =============================================================================
// Main
// =============================================================================

int main() {

    auto* rb = static_cast<RingBuffer*>(
        mmap(nullptr, sizeof(RingBuffer),
             PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));

    auto* tel = static_cast<Telemetry*>(
        mmap(nullptr, sizeof(Telemetry),
             PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));

    if (rb == MAP_FAILED || tel == MAP_FAILED) {
        std::perror("mmap"); return 1;
    }

    {
        std::ofstream csv("io_uring_results.csv");
        csv << "message_size_bytes,run,throughput_gbps,"
               "avg_latency_us,stddev_us,p50_us,p95_us,p99_us\n";
    }

    std::cout << "\n[io_uring + Shared Ring Buffer IPC Benchmark]\n";
    std::cout << "Total transfer per run : "
              << TOTAL_BYTES / (1024*1024) << " MB\n";
    std::cout << "Ring slots             : " << NUM_SLOTS << "\n";
    std::cout << "Polling mode           : SQPOLL + completion polling\n";
    std::cout << "Runs per message size  : " << NUM_RUNS << " + 1 warmup\n\n";

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "=========================================\n";
        std::cout << "Message Size : " << sz << " bytes\n";
        std::cout << "=========================================\n";

        for (int run = 0; run <= NUM_RUNS; ++run) {

            // Full reset of shared state before every run
            std::memset(rb,  0, sizeof(RingBuffer));
            std::memset(tel, 0, sizeof(Telemetry));
            rb->head.store(0, std::memory_order_relaxed);
            rb->tail.store(0, std::memory_order_relaxed);

            pid_t pid = fork();
            if (pid == 0) {
                run_consumer(rb, tel);
                _Exit(0);
            } else if (pid > 0) {
                run_producer(rb, sz);
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

    munmap(rb,  sizeof(RingBuffer));
    munmap(tel, sizeof(Telemetry));
    std::cout << "\nResults written to io_uring_results.csv\n";
    return 0;
}

// =============================================================================
// Build:
//   sudo apt install liburing-dev
//   g++ -O2 -std=c++17 -o io_uring_bench io_uring_bench.cpp -luring
//   sudo ./io_uring_bench          # SQPOLL needs CAP_SYS_ADMIN
//   # OR without root (falls back automatically):
//   ./io_uring_bench
// =============================================================================