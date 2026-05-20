#define _GNU_SOURCE /* needed for CPU_SET, sched_setaffinity */
#include <sched.h>  /* sched_setaffinity, cpu_set_t          */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MSG_SIZE 4096
#define ITERATIONS 100000

/* ── NEW: pin calling process to a specific CPU core ─────────────── */
void pin_to_cpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (sched_setaffinity(0, sizeof(set), &set) != 0)
    perror("sched_setaffinity"); /* warn but continue */
}
/* ─────────────────────────────────────────────────────────────────── */

int main() {
  int fd[2];

  if (pipe(fd) == -1) {
    perror("pipe");
    return 1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }

  /* ── Parent = Producer ── */
  if (pid > 0) {
    pin_to_cpu(0); /* NEW: producer pinned to CPU 0 */

    close(fd[0]);

    char *message = malloc(MSG_SIZE);
    if (!message) {
      perror("malloc");
      return 1;
    }
    memset(message, 'A', MSG_SIZE);

    for (int i = 0; i < ITERATIONS; i++) {
      ssize_t bytes_written = write(fd[1], message, MSG_SIZE);
      if (bytes_written < 0) {
        perror("write");
        break;
      }
    }

    close(fd[1]);
    wait(NULL);
    free(message);
  }

  /* ── Child = Consumer ── */
  else {
    pin_to_cpu(1); /* NEW: consumer pinned to CPU 1 */

    close(fd[1]);

    char *buffer = malloc(MSG_SIZE);
    if (!buffer) {
      perror("malloc");
      return 1;
    }

    for (int i = 0; i < ITERATIONS; i++) {
      ssize_t bytes_read = read(fd[0], buffer, MSG_SIZE);
      if (bytes_read < 0) {
        perror("read");
        break;
      }
    }

    close(fd[0]);
    free(buffer);
  }

  return 0;
}
