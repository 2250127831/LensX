// C 版 key 测试（无优化编译避免 uprobe 不可达）
// gcc -O0 -o c_test c_test.c -lpthread
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

static uint64_t g_key = 1;

__attribute__((noinline)) void mark_s0(uint64_t id) { volatile uint64_t x = id; (void)x; }
__attribute__((noinline)) void mark_s1(uint64_t id) { volatile uint64_t x = id; (void)x; }
__attribute__((noinline)) void mark_s2(uint64_t id) { volatile uint64_t x = id; (void)x; }

typedef struct { int count; int tid; } Arg;
static void* worker(void *arg) {
    Arg *a = (Arg*)arg;
    for (int i = 0; i < a->count; i++) {
        uint64_t key = __sync_fetch_and_add(&g_key, 1);
        mark_s0(key); mark_s1(key); mark_s2(key);
    }
    return NULL;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 50000;
    int n_threads = 4;
    pthread_t t[8];
    printf("sleep 10s for LensX...\n"); sleep(10);
    printf("c_test: %d threads x %d\n", n_threads, n);
    Arg args[8];
    for (int i = 0; i < n_threads; i++) args[i] = (Arg){n, i};
    for (int i = 0; i < n_threads; i++) pthread_create(&t[i], NULL, worker, &args[i]);
    for (int i = 0; i < n_threads; i++) pthread_join(t[i], NULL);
    printf("done: %lu\n", g_key - 1);
}
