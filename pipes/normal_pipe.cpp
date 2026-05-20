#include <unistd.h>

#include <thread>
#include <chrono>
#include <iostream>
#include <vector>
#include <fstream>
#include <algorithm>
#include <numeric>

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

size_t message_size;

std::vector<double> latencies;

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
        "pipe_results.csv",
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

void producer(int write_fd) {

    std::vector<char> payload(
        message_size,
        'X'
    );

    size_t produced = 0;

    while (produced < TOTAL_BYTES) {

        auto send_time =
            std::chrono::high_resolution_clock::now();

        ssize_t n = write(
            write_fd,
            payload.data(),
            payload.size()
        );

        if (n < 0) {
            perror("write");
            exit(1);
        }

        auto recv_time =
            std::chrono::high_resolution_clock::now();

        double latency_us =
            std::chrono::duration<double,
                std::micro>(
                    recv_time - send_time
            ).count();

        latencies.push_back(latency_us);

        produced += n;
    }

    close(write_fd);
}

// ============================================================
// Consumer
// ============================================================

void consumer(
    int read_fd,
    int run_id
) {

    std::vector<char> buffer(
        message_size
    );

    size_t consumed = 0;

    auto start =
        std::chrono::high_resolution_clock::now();

    while (true) {

        ssize_t n = read(
            read_fd,
            buffer.data(),
            buffer.size()
        );

        if (n < 0) {
            perror("read");
            exit(1);
        }

        if (n == 0)
            break;

        volatile uint64_t checksum = 0;

        for (ssize_t i = 0;
             i < n;
             i += 64) {

            checksum += buffer[i];
        }

        consumed += n;
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

    close(read_fd);
}

// ============================================================
// Benchmark Driver
// ============================================================

void run_benchmark(
    int run_id
) {

    latencies.clear();

    int pipefd[2];

    if (pipe(pipefd) < 0) {
        perror("pipe");
        exit(1);
    }

    std::thread producer_thread(
        producer,
        pipefd[1]
    );

    std::thread consumer_thread(
        consumer,
        pipefd[0],
        run_id
    );

    producer_thread.join();

    consumer_thread.join();
}

// ============================================================
// Main
// ============================================================

int main() {

    std::ofstream csv(
        "pipe_results.csv"
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