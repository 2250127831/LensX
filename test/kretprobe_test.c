// kretprobe 验证：大量 read() 系统调用
// gcc -O2 -o kretprobe_test kretprobe_test.c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

int main() {
    int fd = open("/dev/urandom", O_RDONLY);
    char buf;
    printf("sleep 10s for LensX...\n"); sleep(10);
    for (int i = 0; i < 50000; i++)
        read(fd, &buf, 1);
    printf("done\n");
    close(fd);
    return 0;
}
