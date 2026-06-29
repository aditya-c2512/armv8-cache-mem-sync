/*
 * test_physical_write_proof.c
 * Demonstrate that device writes to the physical page behind a userspace
 * buffer are not visible to the CPU until a cache-from-device sync is
 * performed (when required by the platform).
 *
 * Usage: run as root (requires /dev/mem). Optionally pass a device-name
 * to register with the kernel helper for correct DMA ops.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "cache_mem_sync_uapi.h"

#define PAGE_SIZE_SYS (4096UL)

static int64_t get_physical_address(pid_t pid, void *vaddr)
{
    char pagemap_path[128];
    int fd;
    unsigned long page_size = sysconf(_SC_PAGESIZE);
    unsigned long vpn = (unsigned long)vaddr / page_size;
    unsigned long offset = vpn * sizeof(uint64_t);
    uint64_t entry;

    snprintf(pagemap_path, sizeof(pagemap_path), "/proc/%d/pagemap", pid);
    fd = open(pagemap_path, O_RDONLY);
    if (fd < 0) {
        perror("open pagemap");
        return -1;
    }

    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        perror("lseek pagemap");
        close(fd);
        return -1;
    }

    if (read(fd, &entry, sizeof(entry)) != sizeof(entry)) {
        perror("read pagemap");
        close(fd);
        return -1;
    }
    close(fd);

    if (!(entry & (1ULL << 63))) {
        fprintf(stderr, "page not present in memory\n");
        return -1;
    }

    uint64_t pfn = entry & ((1ULL << 55) - 1);
    uint64_t phys = (pfn * page_size) + ((unsigned long)vaddr % page_size);
    return (int64_t)phys;
}

int main(int argc, char **argv)
{
    size_t len = PAGE_SIZE_SYS;
    void *buf = NULL;
    int fd_mod = -1;
    int registered = 0;
    struct cache_mem_sync_register reg;
    struct cache_mem_sync_range range;
    pid_t pid = getpid();

    buf = aligned_alloc(PAGE_SIZE_SYS, len);
    if (!buf) {
        perror("aligned_alloc");
        return 1;
    }
    memset(buf, 0xAA, len);

    /* Try open module device; not fatal if missing */
    fd_mod = open("/dev/cache_mem_sync", O_RDWR);
    if (fd_mod >= 0 && argc > 1) {
        struct cache_mem_sync_set_dev sd;
        memset(&sd, 0, sizeof(sd));
        strncpy(sd.name, argv[1], sizeof(sd.name)-1);
        if (ioctl(fd_mod, CACHE_MEM_SYNC_SET_DEV, &sd) < 0)
            perror("CACHE_MEM_SYNC_SET_DEV");
        else
            printf("registered device %s with helper\n", sd.name);
    }

    if (fd_mod >= 0) {
        reg.user_addr = (uint64_t)(uintptr_t)buf;
        reg.length = (uint32_t)len;
        if (ioctl(fd_mod, CACHE_MEM_SYNC_REGISTER, &reg) == 0) {
            registered = 1;
            printf("registered region user_addr=%p len=%zu\n", buf, len);
        } else {
            perror("CACHE_MEM_SYNC_REGISTER");
        }
    }

    printf("pid=%d vaddr=%p\n", pid, buf);

    int64_t phys = get_physical_address(pid, buf);
    if (phys < 0) {
        fprintf(stderr, "Failed to get physical address; aborting\n");
        goto cleanup;
    }
    printf("phys=0x%llx\n", (unsigned long long)phys);

    /* read before any device write */
    printf("before write: buf[0]=0x%02x\n", ((unsigned char *)buf)[0]);

    /* write to physical address via /dev/mem */
    int fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_mem < 0) {
        perror("open /dev/mem");
        fprintf(stderr, "/dev/mem unavailable; try running as root or use module SIMULATE_WRITE\n");
        goto cleanup;
    }

    unsigned char pat = 0x55;
    ssize_t w = pwrite(fd_mem, &pat, 1, phys);
    if (w != 1) {
        perror("pwrite /dev/mem");
        close(fd_mem);
        goto cleanup;
    }
    fsync(fd_mem);
    close(fd_mem);

    /* read immediately after device write (without FROM_DEVICE) */
    printf("after physical write, before FROM_DEVICE: buf[0]=0x%02x\n", ((unsigned char *)buf)[0]);

    /* If module present and registered, issue FROM_DEVICE to invalidate CPU cache */
    if (fd_mod >= 0 && registered) {
        range.offset = 0;
        range.length = (uint32_t)len;
        if (ioctl(fd_mod, CACHE_MEM_SYNC_FROM_DEVICE, &range) < 0) {
            perror("CACHE_MEM_SYNC_FROM_DEVICE");
        } else {
            printf("after FROM_DEVICE: buf[0]=0x%02x\n", ((unsigned char *)buf)[0]);
        }
    } else {
        printf("module not available or not registered; cannot FROM_DEVICE here\n");
    }

cleanup:
    if (fd_mod >= 0) {
        if (registered)
            ioctl(fd_mod, CACHE_MEM_SYNC_UNREGISTER);
        close(fd_mod);
    }
    free(buf);
    return 0;
}
