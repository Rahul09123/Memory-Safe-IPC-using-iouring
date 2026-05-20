#include <liburing.h>

#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <vector>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <cstring>

constexpr size_t MESSAGE_SIZES[] = {
    64,
    256,
    1024,
    4096,
    16384,
    65536,
    262144,
    1048576
};

constexpr int NUM_RUNS = 5;

constexpr size_t TOTAL_BYTES =
    4ULL * 1024 * 1024 * 1024;

constexpr size_t NUM_SLOTS = 64;

constexpr char SHM_NAME[] = "/ipc_ring";

size_t message_size;

std::vector<double> latencies;

// ============================================================
// Ring Buffer
// ============================================================

struct alignas(64) RingBuffer {

    alignas(64)
    std::atomic<uint64_t> head;

    alignas(64)
    std::atomic<uint64_t> tail;

    struct alignas(64) Slot {

        uint32_t size;

        char data[1048576];
    };

    Slot slots[NUM_SLOTS];
};

RingBuffer* ring_buffer = nullptr;

int notify_fd;

io_uring ring;

// ============================================================
// Helpers
// ============================================================

inline bool ring_full(
    uint64_t head,
    uint64_t tail
) {
    return (head - tail) >= NUM_SLOTS;
}

inline bool ring_empty(
    uint64_t head,
    uint64_t tail
) {
    return head == tail;
}

// ============================================================
// CSV Logging
// ============================================================

void log_results(
    size_t msg_size,
    int run,
    double throughput,
    double avg_latency,
    double p50,
    double p95,
    double p99
) {

    std::ofstream csv(
        "ring_results.csv",
        std::ios::app
    );

    csv
        << msg_size << ","
        << run << ","
        << throughput << ","
        << avg_latency << ","
        << p50 << ","
        << p95 << ","
        << p99 << "\n";
}

// ============================================================
// Producer
// ============================================================

void producer() {

    std::vector<char> payload(
        message_size,
        'X'
    );

    size_t produced = 0;

    while (produced < TOTAL_BYTES) {

        uint64_t head =
            ring_buffer->head.load(
                std::memory_order_relaxed
            );

        uint64_t tail =
            ring_buffer->tail.load(
                std::memory_order_acquire
            );

        if (ring_full(head, tail))
            continue;

        auto& slot =
            ring_buffer->slots[
                head % NUM_SLOTS
            ];

        auto send_time =
            std::chrono::high_resolution_clock::now();

        memcpy(
            slot.data,
            payload.data(),
            message_size
        );

        slot.size = message_size;

        ring_buffer->head.store(
            head + 1,
            std::memory_order_release
        );

        uint64_t one = 1;

        eventfd_write(
            notify_fd,
            one
        );

        auto recv_time =
            std::chrono::high_resolution_clock::now();

        double latency_us =
            std::chrono::duration<double,
                std::micro>(
                    recv_time - send_time
            ).count();

        latencies.push_back(latency_us);

        produced += message_size;
    }
}

// ============================================================
// io_uring Poll
// ============================================================

void submit_poll() {

    io_uring_sqe* sqe =
        io_uring_get_sqe(&ring);

    io_uring_prep_poll_add(
        sqe,
        notify_fd,
        POLLIN
    );

    io_uring_submit(&ring);
}

// ============================================================
// Consumer
// ============================================================

void consumer(
    int run_id
) {

    size_t consumed = 0;

    auto start =
        std::chrono::high_resolution_clock::now();

    submit_poll();

    while (consumed < TOTAL_BYTES) {

        io_uring_cqe* cqe;

        int ret =
            io_uring_wait_cqe(
                &ring,
                &cqe
            );

        if (ret < 0) {
            std::cerr
                << "wait_cqe failed\n";
            exit(1);
        }

        uint64_t value;

        eventfd_read(
            notify_fd,
            &value
        );

        while (true) {

            uint64_t tail =
                ring_buffer->tail.load(
                    std::memory_order_relaxed
                );

            uint64_t head =
                ring_buffer->head.load(
                    std::memory_order_acquire
                );

            if (ring_empty(head, tail))
                break;

            auto& slot =
                ring_buffer->slots[
                    tail % NUM_SLOTS
                ];

            volatile uint64_t checksum = 0;

            for (size_t i = 0;
                 i < slot.size;
                 i += 64) {

                checksum += slot.data[i];
            }

            consumed += slot.size;

            ring_buffer->tail.store(
                tail + 1,
                std::memory_order_release
            );
        }

        io_uring_cqe_seen(
            &ring,
            cqe
        );

        submit_poll();
    }

    auto end =
        std::chrono::high_resolution_clock::now();

    double sec =
        std::chrono::duration<double>(
            end - start
        ).count();

    double gbps =
        (double)consumed /
        (1024.0 * 1024.0 * 1024.0) /
        sec;

    std::sort(
        latencies.begin(),
        latencies.end()
    );

    double avg =
        std::accumulate(
            latencies.begin(),
            latencies.end(),
            0.0
        ) / latencies.size();

    double p50 =
        latencies[
            latencies.size() * 0.50
        ];

    double p95 =
        latencies[
            latencies.size() * 0.95
        ];

    double p99 =
        latencies[
            latencies.size() * 0.99
        ];

    std::cout
        << "Run: "
        << run_id
        << "\n";

    std::cout
        << "Transferred: "
        << consumed / (1024 * 1024)
        << " MB\n";

    std::cout
        << "Time: "
        << sec
        << " sec\n";

    std::cout
        << "Throughput: "
        << gbps
        << " GB/s\n";

    std::cout
        << "Avg Latency: "
        << avg
        << " us\n";

    std::cout
        << "P50: "
        << p50
        << " us\n";

    std::cout
        << "P95: "
        << p95
        << " us\n";

    std::cout
        << "P99: "
        << p99
        << " us\n";

    std::cout
        << "---------------------------------\n";

    log_results(
        message_size,
        run_id,
        gbps,
        avg,
        p50,
        p95,
        p99
    );
}

// ============================================================
// Benchmark Driver
// ============================================================

void run_benchmark(
    int run_id
) {

    latencies.clear();

    int shm_fd = shm_open(
        SHM_NAME,
        O_CREAT | O_RDWR,
        0666
    );

    size_t shm_size =
        sizeof(RingBuffer);

    ftruncate(
        shm_fd,
        shm_size
    );

    void* ptr = mmap(
        nullptr,
        shm_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_fd,
        0
    );

    ring_buffer =
        static_cast<RingBuffer*>(ptr);

    ring_buffer->head.store(0);
    ring_buffer->tail.store(0);

    notify_fd =
        eventfd(0, EFD_NONBLOCK);

    io_uring_queue_init(
        256,
        &ring,
        0
    );

    std::thread producer_thread(
        producer
    );

    std::thread consumer_thread(
        consumer,
        run_id
    );

    producer_thread.join();

    consumer_thread.join();

    io_uring_queue_exit(&ring);

    close(notify_fd);

    munmap(ptr, shm_size);

    shm_unlink(SHM_NAME);
}

// ============================================================
// Main
// ============================================================

int main() {

    std::ofstream csv(
        "ring_results.csv"
    );

    csv << "message_size,run,throughput_gbps,"
           "avg_latency_us,p50,p95,p99\n";

    csv.close();

    for (size_t sz : MESSAGE_SIZES) {

        message_size = sz;

        std::cout
            << "=================================\n";

        std::cout
            << "Message Size: "
            << sz
            << " bytes\n";

        std::cout
            << "=================================\n";

        // Warmup
        run_benchmark(0);

        for (int run = 1;
             run <= NUM_RUNS;
             run++) {

            run_benchmark(run);
        }
    }

    return 0;
}