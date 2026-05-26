// io_uring 跨内核 key 配对测试
// gcc -O2 -o io_uring_test io_uring_test.c -luring
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <liburing.h>

static struct io_uring ring;

__attribute__((noinline)) void mark_submit(uint64_t key) {
    int ret = io_uring_submit(&ring); (void)ret;
}

int main(int argc, char **argv) {
    int count = argc > 1 ? atoi(argv[1]) : 10000;
    if (io_uring_queue_init(256, &ring, 0) < 0) return 1;
    printf("sleep 10s for LensX...\n"); sleep(10);
    printf("submitting %d NOPs\n", count);
    for (int i = 0; i < count; i++) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if (!sqe) { io_uring_submit(&ring); sqe = io_uring_get_sqe(&ring); }
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, (void*)(uintptr_t)(i + 1));
        mark_submit(i + 1);
    }
    io_uring_submit(&ring);
    int done = 0;
    while (done < count) {
        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe(&ring, &cqe) < 0) return 1;
        io_uring_cqe_seen(&ring, cqe); done++;
    }
    printf("done: %d\n", done);
    io_uring_queue_exit(&ring);
}
