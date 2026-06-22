#ifndef _CACHE_MEM_SYNC_UAPI_H
#define _CACHE_MEM_SYNC_UAPI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint64_t __u64;
typedef uint32_t __u32;
#endif

#define CACHE_MEM_SYNC_IOC_MAGIC 'c'

struct cache_mem_sync_register {
    __u64 user_addr; /* user virtual address */
    __u32 length;    /* length in bytes */
};

struct cache_mem_sync_range {
    __u64 offset; /* offset into registered region */
    __u32 length; /* bytes */
};

struct cache_mem_sync_simulate {
    __u64 offset;
    __u32 length;
    __u8 pattern;
};

#define CACHE_MEM_SYNC_REGISTER _IOW(CACHE_MEM_SYNC_IOC_MAGIC, 1, struct cache_mem_sync_register)
#define CACHE_MEM_SYNC_UNREGISTER _IO(CACHE_MEM_SYNC_IOC_MAGIC, 2)
#define CACHE_MEM_SYNC_TO_DEVICE _IOW(CACHE_MEM_SYNC_IOC_MAGIC, 3, struct cache_mem_sync_range)
#define CACHE_MEM_SYNC_FROM_DEVICE _IOW(CACHE_MEM_SYNC_IOC_MAGIC, 4, struct cache_mem_sync_range)
#define CACHE_MEM_SYNC_CHECKSUM _IOWR(CACHE_MEM_SYNC_IOC_MAGIC, 5, struct cache_mem_sync_range)
#define CACHE_MEM_SYNC_SIMULATE_WRITE _IOW(CACHE_MEM_SYNC_IOC_MAGIC, 6, struct cache_mem_sync_simulate)

#endif /* _CACHE_MEM_SYNC_UAPI_H */
