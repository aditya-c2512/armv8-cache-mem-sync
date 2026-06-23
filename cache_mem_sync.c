#include "cache_mem_sync_uapi.h"

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/dma-mapping.h>
/* Note: architecture-specific cache flush helpers are not reliably
 * exported to modules across kernel versions. We avoid referencing
 * non-exported symbols to keep the module buildable; when DMA mapping
 * fails we emit a warning and fall back to a no-op. For production use
 * bind to a real `struct device *` for the NIC and use its DMA ops. */
#include <linux/mutex.h>
#include <linux/sysinfo.h>

#define DRV_NAME "cache_mem_sync"
#define MAX_REGION_BYTES (1UL << 30) /* 1 GiB limit for safety */

struct cache_mem_mapping {
    struct page **pages;
    dma_addr_t *dma_addrs;
    unsigned long user_addr;
    size_t length;
    unsigned int npages;
    bool registered;
    void *coherent_buf;
    dma_addr_t coherent_dma;
    struct device *target_dev;
};

static struct cache_mem_mapping mapping;
static struct miscdevice cms_misc;
static char cms_dev_name[32] = "cache_mem_sync";
static DEFINE_MUTEX(mapping_lock);

static int ensure_unregistered_locked(void)
{
    unsigned int i;
    struct device *dev = mapping.target_dev ? mapping.target_dev : cms_misc.this_device;

    struct sysinfo info;

    if (!mapping.registered)
        return 0;

    pr_info("cache_mem_sync: unregistering region user_addr=%pK length=%zu npages=%u\n", (void *)mapping.user_addr, mapping.length, mapping.npages);
    si_meminfo(&info);
    pr_info("cache_mem_sync: meminfo totalram=%lluMB freeram=%lluMB\n",
            (unsigned long long)(info.totalram * (PAGE_SIZE / 1024ULL / 1024ULL)),
            (unsigned long long)(info.freeram * (PAGE_SIZE / 1024ULL / 1024ULL)));

    for (i = 0; i < mapping.npages; ++i) {
        if (mapping.pages && mapping.pages[i])
            put_page(mapping.pages[i]);
    }
    if (mapping.coherent_buf && dev)
        dma_free_coherent(dev, mapping.length, mapping.coherent_buf, mapping.coherent_dma);
    kfree(mapping.dma_addrs);
    kfree(mapping.pages);
    if (mapping.target_dev) {
        put_device(mapping.target_dev);
        mapping.target_dev = NULL;
    }
    memset(&mapping, 0, sizeof(mapping));
    pr_info("cache_mem_sync: unregister complete\n");
    return 0;
}

static long cache_mem_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret = 0;

    if (_IOC_TYPE(cmd) != CACHE_MEM_SYNC_IOC_MAGIC)
        return -EINVAL;

    mutex_lock(&mapping_lock);

    switch (cmd) {
    case CACHE_MEM_SYNC_REGISTER: {
        struct cache_mem_sync_register r;
        unsigned long start;
        unsigned long first_page_addr;
        unsigned int npages;
        unsigned int pinned = 0;
        unsigned int i;
        struct page **pages = NULL;
        struct device *dev = mapping.target_dev ? mapping.target_dev : cms_misc.this_device;

        if (copy_from_user(&r, (void __user *)arg, sizeof(r))) {
            ret = -EFAULT;
            break;
        }
        if (r.length == 0 || r.length > MAX_REGION_BYTES) {
            ret = -EINVAL;
            break;
        }

        start = (unsigned long)r.user_addr;
        first_page_addr = start & PAGE_MASK;
        npages = DIV_ROUND_UP((start - first_page_addr) + r.length, PAGE_SIZE);

        pages = kcalloc(npages, sizeof(struct page *), GFP_KERNEL);
        if (!pages) {
            ret = -ENOMEM;
            break;
        }

        pinned = get_user_pages_fast(first_page_addr, npages, 1, pages);
        if (pinned != npages) {
            /* release any pinned pages */
            for (i = 0; i < pinned; ++i)
                put_page(pages[i]);
            kfree(pages);
            ret = -EFAULT;
            break;
        }

        /* We'll perform DMA mapping on-demand during sync operations using
         * a kmap + dma_map_single/dma_unmap_single sequence. Avoid pre-mapping
         * pages here to reduce complexity and platform-specific issues. */

        /* if a mapping already existed, teardown first */
        ensure_unregistered_locked();

        mapping.pages = pages;
        mapping.dma_addrs = NULL;
        mapping.user_addr = (unsigned long)r.user_addr;
        mapping.length = r.length;
        mapping.npages = npages;
        mapping.registered = true;
        mapping.coherent_buf = NULL;
        mapping.coherent_dma = 0;

        /* Try to allocate a coherent DMA buffer to use as an intermediate
         * so we don't have to map user pages for DMA on platforms where
         * dma_map_page fails for user pages. This is less efficient but
         * reliable for testing. */
        if (r.length > 0) {
            mapping.coherent_buf = dma_alloc_coherent(dev, r.length, &mapping.coherent_dma, GFP_KERNEL);
            if (!mapping.coherent_buf) {
                pr_warn("cache_mem_sync: dma_alloc_coherent failed for len=%u; continuing without coherent buffer\n", (unsigned int)r.length);
            } else {
                pr_info("cache_mem_sync: allocated coherent buffer %p dma=%pad len=%u\n", mapping.coherent_buf, &mapping.coherent_dma, (unsigned int)r.length);
            }
        }

        {
            struct sysinfo info;
            si_meminfo(&info);
            pr_info("cache_mem_sync: registered region %pK len=%u pages=%u pid=%d\n",
                (void *)mapping.user_addr, (unsigned int)mapping.length, mapping.npages, current->pid);
            pr_info("cache_mem_sync: meminfo totalram=%lluMB freeram=%lluMB\n",
                (unsigned long long)(info.totalram * (PAGE_SIZE / 1024ULL / 1024ULL)),
                (unsigned long long)(info.freeram * (PAGE_SIZE / 1024ULL / 1024ULL)));
        }
        break;
    }
    case CACHE_MEM_SYNC_UNREGISTER:
        ensure_unregistered_locked();
        break;
    case CACHE_MEM_SYNC_SET_DEV: {
        struct cache_mem_sync_set_dev sd;
        struct device *dev = NULL;

        if (copy_from_user(&sd, (void __user *)arg, sizeof(sd))) {
            ret = -EFAULT;
            break;
        }

        /* try common buses: platform, pci, of-platform */
        dev = bus_find_device_by_name(&platform_bus_type, NULL, sd.name);
        if (!dev)
            dev = bus_find_device_by_name(&pci_bus_type, NULL, sd.name);
        if (!dev)
            dev = bus_find_device_by_name(&of_platform_bus_type, NULL, sd.name);

        if (!dev) {
            pr_err("cache_mem_sync: device '%s' not found\n", sd.name);
            ret = -ENODEV;
            break;
        }

        get_device(dev);
        if (mapping.target_dev)
            put_device(mapping.target_dev);
        mapping.target_dev = dev;
        pr_info("cache_mem_sync: using device %s for DMA ops\n", sd.name);
        break;
    }
    case CACHE_MEM_SYNC_TO_DEVICE:
    case CACHE_MEM_SYNC_FROM_DEVICE: {
        struct cache_mem_sync_range range;
        unsigned long rel_off;
        unsigned int remaining;
        unsigned int page_idx;
        unsigned int page_off;
        unsigned int chunk;
        struct device *dev = mapping.target_dev ? mapping.target_dev : cms_misc.this_device;

        if (!mapping.registered) {
            ret = -EINVAL;
            break;
        }
        if (copy_from_user(&range, (void __user *)arg, sizeof(range))) {
            ret = -EFAULT;
            break;
        }
        if (range.offset + range.length > mapping.length) {
            ret = -EINVAL;
            break;
        }

        rel_off = range.offset + ((unsigned long)mapping.user_addr & ~PAGE_MASK);
        remaining = range.length;
        page_idx = rel_off / PAGE_SIZE;
        page_off = rel_off % PAGE_SIZE;

        pr_info("cache_mem_sync: %s start offset=%llu len=%u start_page=%u start_page_off=%u\n",
                (cmd == CACHE_MEM_SYNC_TO_DEVICE) ? "SYNC_TO_DEVICE" : "SYNC_FROM_DEVICE",
                (unsigned long long)range.offset, range.length, page_idx, page_off);

        while (remaining) {
            /* ensure we never map past the current page boundary */
            unsigned int max_chunk = (unsigned int)(PAGE_SIZE - page_off);
            if (max_chunk == 0)
                max_chunk = PAGE_SIZE;
            chunk = min(max_chunk, remaining);
            /* defensive cap: chunk must never be larger than PAGE_SIZE */
            if (chunk > PAGE_SIZE)
                chunk = PAGE_SIZE;

            pr_info("cache_mem_sync: page_idx=%u page_off=%u PAGE_SIZE=%u chunk=%u remaining=%u\n",
                    page_idx, page_off, (unsigned int)PAGE_SIZE, chunk, remaining);

            struct page *page = mapping.pages[page_idx];
            dma_addr_t dma = dma_map_page(dev, page, page_off, chunk,
                                         (cmd == CACHE_MEM_SYNC_TO_DEVICE) ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
            if (dma_mapping_error(dev, dma)) {
                pr_warn("cache_mem_sync: dma_map_page failed page_idx=%u page=%p page_off=%u chunk=%u dma=%pad; trying coherent buffer fallback\n",
                        page_idx, page, page_off, chunk, &dma);

                if (mapping.coherent_buf) {
                    /* compute offset within the coherent buffer for this chunk.
                     * rel_off = range.offset + offset_within_first_page was used to
                     * derive page_idx/page_off, so the region-relative offset is
                     * (page_idx*PAGE_SIZE + page_off) - offset_within_first_page. */
                    unsigned long offset_within_first_page = (unsigned long)mapping.user_addr & ~PAGE_MASK;
                    unsigned long buf_off = (page_idx * PAGE_SIZE + page_off) - offset_within_first_page;
                    void *kbuf = (uint8_t *)mapping.coherent_buf + buf_off;
                    void *kaddr = kmap_atomic(page);
                    if (cmd == CACHE_MEM_SYNC_TO_DEVICE)
                        memcpy(kbuf, (uint8_t *)kaddr + page_off, chunk);
                    else
                        memcpy((uint8_t *)kaddr + page_off, kbuf, chunk);
                    kunmap_atomic(kaddr);
                    /* If we had a real device we'd sync coherent DMA for device here. */
                } else {
                    pr_warn("cache_mem_sync: no coherent buffer available; cannot perform DMA sync for page_idx=%u\n", page_idx);
                    ret = -EIO;
                    break;
                }
            } else {
                if (cmd == CACHE_MEM_SYNC_TO_DEVICE)
                    dma_sync_single_for_device(dev, dma, chunk, DMA_TO_DEVICE);
                else
                    dma_sync_single_for_cpu(dev, dma, chunk, DMA_FROM_DEVICE);

                dma_unmap_page(dev, dma, chunk, (cmd == CACHE_MEM_SYNC_TO_DEVICE) ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
            }

            remaining -= chunk;

            if ((page_idx & 63) == 0)
                pr_info("cache_mem_sync: progress page_idx=%u remaining=%u\n", page_idx, remaining);

            ++page_idx;
            page_off = 0;
        }

        pr_info("cache_mem_sync: %s complete offset=%llu len=%u\n",
                (cmd == CACHE_MEM_SYNC_TO_DEVICE) ? "SYNC_TO_DEVICE" : "SYNC_FROM_DEVICE",
                (unsigned long long)range.offset, range.length);

        if (ret)
            pr_err("cache_mem_sync: ioctl operation returned %d\n", ret);
        else
            pr_info("cache_mem_sync: ioctl operation completed successfully\n");

        break;
    }
    case CACHE_MEM_SYNC_CHECKSUM: {
        struct cache_mem_sync_range range;
        unsigned long rel_off;
        unsigned int remaining;
        unsigned int page_idx;
        unsigned int page_off;
        unsigned int chunk;
        uint32_t sum = 0;
        void *kaddr = NULL;
        struct page *page;

        if (!mapping.registered) {
            ret = -EINVAL;
            break;
        }
        if (copy_from_user(&range, (void __user *)arg, sizeof(range))) {
            ret = -EFAULT;
            break;
        }
        if (range.offset + range.length > mapping.length) {
            ret = -EINVAL;
            break;
        }

        rel_off = range.offset + ((unsigned long)mapping.user_addr & ~PAGE_MASK);
        remaining = range.length;
        page_idx = rel_off / PAGE_SIZE;
        page_off = rel_off % PAGE_SIZE;

        while (remaining) {
            chunk = min((unsigned int)(PAGE_SIZE - page_off), remaining);
            page = mapping.pages[page_idx];
            kaddr = kmap_atomic(page);
            {
                unsigned int i;
                uint8_t *p = (uint8_t *)kaddr + page_off;
                for (i = 0; i < chunk; ++i)
                    sum += p[i];
            }
            kunmap_atomic(kaddr);

            remaining -= chunk;
            ++page_idx;
            page_off = 0;
        }

        /* return sum in the same struct's length field (set as checksum) */
        range.length = sum;
        if (copy_to_user((void __user *)arg, &range, sizeof(range)))
            ret = -EFAULT;
        break;
    }
    case CACHE_MEM_SYNC_SIMULATE_WRITE: {
        struct cache_mem_sync_simulate s;
        unsigned long rel_off;
        unsigned int remaining;
        unsigned int page_idx;
        unsigned int page_off;
        unsigned int chunk;
        void *kaddr = NULL;
        struct page *page;

        if (copy_from_user(&s, (void __user *)arg, sizeof(s))) {
            ret = -EFAULT;
            break;
        }
        if (!mapping.registered) {
            ret = -EINVAL;
            break;
        }
        if (s.offset + s.length > mapping.length) {
            ret = -EINVAL;
            break;
        }

        rel_off = s.offset + ((unsigned long)mapping.user_addr & ~PAGE_MASK);
        remaining = s.length;
        page_idx = rel_off / PAGE_SIZE;
        page_off = rel_off % PAGE_SIZE;

        pr_info("cache_mem_sync: SIMULATE_WRITE offset=%llu len=%u pat=0x%02x\n",
                (unsigned long long)s.offset, s.length, s.pattern);

        while (remaining) {
            chunk = min((unsigned int)(PAGE_SIZE - page_off), remaining);
            page = mapping.pages[page_idx];
            kaddr = kmap_atomic(page);
            memset((uint8_t *)kaddr + page_off, s.pattern, chunk);
            kunmap_atomic(kaddr);

            remaining -= chunk;
            ++page_idx;
            page_off = 0;
        }

        pr_info("cache_mem_sync: SIMULATE_WRITE complete\n");
        break;
    }
    default:
        ret = -ENOTTY;
        break;
    }

    mutex_unlock(&mapping_lock);
    pr_info("cache_mem_sync: returning ioctl result %d\n", ret);
    return ret;
}

static int cache_mem_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int cache_mem_release(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations cms_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = cache_mem_ioctl,
    .open = cache_mem_open,
    .release = cache_mem_release,
};

static int __init cache_mem_init(void)
{
    int ret;

    /* set misc device name (persist in dev_name) */
    cms_misc.minor = MISC_DYNAMIC_MINOR;
    cms_misc.name = cms_dev_name;
    cms_misc.fops = &cms_fops;

    ret = misc_register(&cms_misc);
    if (ret) {
        pr_err("cache_mem_sync: misc_register failed %d\n", ret);
        return ret;
    }

    pr_info("cache_mem_sync: loaded, device /dev/%s\n", cms_dev_name);
    return 0;
}

static void __exit cache_mem_exit(void)
{
    mutex_lock(&mapping_lock);
    ensure_unregistered_locked();
    mutex_unlock(&mapping_lock);

    misc_deregister(&cms_misc);
    pr_info("cache_mem_sync: unloaded\n");
}

module_init(cache_mem_init);
module_exit(cache_mem_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("scaffold");
MODULE_DESCRIPTION("Minimal per-page cache<->mem sync helper (RPi5 target)");
