#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int main(void) {
    size_t len = 4096;
    void *buf = aligned_alloc(4096, len);
    if (!buf) return 1;
    memset(buf, 0xAA, len);
    printf("PID=%d VADDR=%p\n", getpid(), buf);
    fflush(stdout);
    /* sleep to allow external /dev/mem write */
    sleep(60);
    printf("buf[0]=0x%02x\n", ((unsigned char*)buf)[0]);
    return 0;
}