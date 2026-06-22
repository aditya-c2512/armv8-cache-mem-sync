#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdint.h>

#include "cache_mem_sync_uapi.h"

int main(void)
{
    int fd = open("/dev/cache_mem_sync", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    size_t len = 4096 * 2;
    void *buf = aligned_alloc(4096, len);
    if (!buf) {
        perror("aligned_alloc");
        close(fd);
        return 1;
    }

    memset(buf, 0xAA, len);

    struct cache_mem_sync_register reg = { .user_addr = (uint64_t)(uintptr_t)buf, .length = (uint32_t)len };

    if (ioctl(fd, CACHE_MEM_SYNC_REGISTER, &reg) < 0) {
        perror("CACHE_MEM_SYNC_REGISTER");
        free(buf);
        close(fd);
        return 1;
    }

    /* Sync to device (CPU -> device) */
    struct cache_mem_sync_range r = { .offset = 0, .length = (uint32_t)len };

    /* Check checksum before cleaning */
    struct cache_mem_sync_range cr = r;
    if (ioctl(fd, CACHE_MEM_SYNC_CHECKSUM, &cr) < 0) {
        perror("CACHE_MEM_SYNC_CHECKSUM before");
    } else {
        printf("checksum before clean = 0x%x\n", cr.length);
    }

    if (ioctl(fd, CACHE_MEM_SYNC_TO_DEVICE, &r) < 0) {
        perror("CACHE_MEM_SYNC_TO_DEVICE");
    } else {
        printf("SYNC_TO_DEVICE succeeded\n");
    }

    /* Check checksum after cleaning */
    cr = r;
    if (ioctl(fd, CACHE_MEM_SYNC_CHECKSUM, &cr) < 0) {
        perror("CACHE_MEM_SYNC_CHECKSUM after");
    } else {
        printf("checksum after clean = 0x%x\n", cr.length);
    }

    /* Simulate device writing by kernel (simulate DMA) */
    struct cache_mem_sync_simulate s = { .offset = 0, .length = (uint32_t)len, .pattern = 0x55 };
    if (ioctl(fd, CACHE_MEM_SYNC_SIMULATE_WRITE, &s) < 0) {
        perror("CACHE_MEM_SYNC_SIMULATE_WRITE");
    } else {
        printf("SIMULATE_WRITE succeeded (kernel wrote pattern 0x55)\n");
    }

    /* Read buffer now without invalidation (may be stale in CPU cache) */
    printf("First byte seen by userspace before FROM_DEVICE: 0x%02x\n", ((unsigned char*)buf)[0]);

    /* Sync from device (device -> CPU) */
    if (ioctl(fd, CACHE_MEM_SYNC_FROM_DEVICE, &r) < 0) {
        perror("CACHE_MEM_SYNC_FROM_DEVICE");
    } else {
        printf("SYNC_FROM_DEVICE succeeded\n");
    }

    printf("First byte seen by userspace after FROM_DEVICE: 0x%02x\n", ((unsigned char*)buf)[0]);

    if (ioctl(fd, CACHE_MEM_SYNC_UNREGISTER) < 0)
        perror("CACHE_MEM_SYNC_UNREGISTER");

    free(buf);
    close(fd);
    return 0;
}
