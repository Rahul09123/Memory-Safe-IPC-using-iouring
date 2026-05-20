#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define MSG_SIZE   4096 
#define ITERATIONS 100000
#define RING_SIZE  256      /* must be power of 2 */

/* Shared ring control — lives in mmap'd memory */
struct ring {
    _Atomic uint32_t head;  /* consumer advances this */
    _Atomic uint32_t tail;  /* producer advances this */
};

/* One slot = one message */
struct slot {
    char data[MSG_SIZE];
};

/* ARM64/x86 spin-wait hint */
static inline void cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#else
    __asm__ volatile("pause" ::: "memory");
#endif
}

void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        perror("sched_setaffinity");
}

int main()
{
    /* Single mmap shared across fork — no kernel pipe buffer */
    size_t shm_size = sizeof(struct ring)
                    + sizeof(struct slot) * RING_SIZE;

    void *shm = mmap(NULL, shm_size,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) { perror("mmap"); return 1; }

    struct ring *ring  = (struct ring *)shm;
    struct slot *slots = (struct slot *)((char *)shm + sizeof(struct ring));

    atomic_store(&ring->head, 0);
    atomic_store(&ring->tail, 0);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    /* ── Parent = Producer ── */
    if (pid > 0) {
        pin_to_cpu(0);

        char *message = malloc(MSG_SIZE);
        if (!message) { perror("malloc"); return 1; }
        memset(message, 'A', MSG_SIZE);

        for (int i = 0; i < ITERATIONS; i++) {
            uint32_t tail = atomic_load_explicit(
                                &ring->tail, memory_order_relaxed);

            /* Wait for a free slot */
            while (tail - atomic_load_explicit(
                               &ring->head, memory_order_acquire)
                   >= RING_SIZE)
                cpu_relax();

            /* Write directly into shared slot — zero kernel involvement */
            memcpy(slots[tail & (RING_SIZE - 1)].data, message, MSG_SIZE);

            /* Publish: one atomic store, consumer sees it instantly */
            atomic_store_explicit(&ring->tail, tail + 1,
                                  memory_order_release);
        }

        wait(NULL);
        free(message);
    }

    /* ── Child = Consumer ── */
    else {
        pin_to_cpu(1);

        char *buffer = malloc(MSG_SIZE);
        if (!buffer) { perror("malloc"); return 1; }

        for (int i = 0; i < ITERATIONS; i++) {
            uint32_t head = atomic_load_explicit(
                                &ring->head, memory_order_relaxed);

            /* Wait for a message */
            while (atomic_load_explicit(
                       &ring->tail, memory_order_acquire) == head)
                cpu_relax();

            /* Read directly from shared slot */
            memcpy(buffer, slots[head & (RING_SIZE - 1)].data, MSG_SIZE);

            /* Advance head — signals producer that slot is free */
            atomic_store_explicit(&ring->head, head + 1,
                                  memory_order_release);
        }

        free(buffer);
    }

    munmap(shm, shm_size);
    return 0;
}
