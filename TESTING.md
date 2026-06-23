# TESTING

This document explains how to run the automated proof test that demonstrates
CPU cache <-> device memory synchronization behavior on non-coherent platforms
(like Raspberry Pi 5).

## Overview
The supplied test program `test_physical_write_proof` does the following:
- allocates a page-aligned userspace buffer and fills it with 0xAA
- registers that region with `/dev/cache_mem_sync` if the module is loaded
- obtains the physical address of the page via `/proc/<pid>/pagemap`
- writes a byte `0x55` directly to that physical address using `/dev/mem`
  (this simulates a device DMA write)
- reads the userspace buffer immediately (this may still show stale CPU-cached
  data if the platform is non-coherent)
- if the helper module is loaded and registered, calls `CACHE_MEM_SYNC_FROM_DEVICE`
  and shows the buffer byte after cache invalidation

## Requirements
- Run on the target board (RPi5). This test requires root because it opens
  `/dev/mem` and uses `/proc/<pid>/pagemap`.
- Build the kernel module and userspace programs first.

## Build
**On the target (RPi5):**

```bash
# build kernel module
make KERNEL_DIR=/lib/modules/$(uname -r)/build

# build tests
gcc -O2 -I. -o test_physical_write_proof test_physical_write_proof.c
gcc -O2 -I. -o test_simulate_write test_simulate_write.c
```

**Run (recommended):**
1. Load the module and (optionally) register the NIC device name for correct DMA ops:

```bash
sudo insmod cache_mem_sync.ko
# Optionally: choose a device name from /sys/bus/platform/devices
# e.g. DEVICE_NAME=$(ls /sys/bus/platform/devices | head -n1)
```

2. Run the proof test (as root) and pass the device name if you want the
module to register the NIC for DMA ops (recommended):

```bash
sudo ./test_physical_write_proof <device-name>
# or without device argument: sudo ./test_physical_write_proof
```

3. Observe output similar to:

```
registered region user_addr=0x... len=4096
pid=1234 vaddr=0x7f... 
phys=0xdeadbeef
before write: buf[0]=0xaa
after physical write, before FROM_DEVICE: buf[0]=0xaa   <-- stale (expected)
after FROM_DEVICE: buf[0]=0x55                      <-- updated (expected)
```

If the `after physical write, before FROM_DEVICE` line shows `0xAA` and the
`after FROM_DEVICE` shows `0x55`, that demonstrates the CPU had a stale
cache copy that required explicit invalidation.

**Alternative when `/dev/mem` is restricted**

If `/dev/mem` is not available on your kernel (some distributions restrict it),
use the module's `CACHE_MEM_SYNC_SIMULATE_WRITE` ioctl to simulate a device
write. The simulation is done from the kernel and performs a write to the
mapping's pages. Example:

```bash
# Use test_simulate_write which exercises SIMULATE_WRITE
sudo ./test_simulate_write <device-name>
```

This shows the same before/after behavior, but keep in mind the simulation
is provided by the module itself and should be used only for functional
verification.

**Reproducing the "without module" failure**

To show that the same sequence would fail without the module:

1. Unload the module:
```bash
sudo rmmod cache_mem_sync
```

2. Allocate a user buffer in a small program and use `/proc/<pid>/pagemap` and
   `/dev/mem` (as above) to write to the physical address. Attempt to observe
   the buffer contents from userspace after the device write. On a non-coherent
   platform you should observe stale data, because there is no helper to
   invalidate the CPU cache.

### Manual proof (step-by-step)
You can reproduce the failure manually using a small helper program that
allocates a page, prints its PID and virtual address, and sleeps while you
perform the physical write from another shell. Example helper (compile and
run as root):

```c
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
```

Run the helper and note the printed PID and VADDR. In another root shell, compute
the physical address and write a byte `0x55`:

```bash
# replace PID and VADDR with printed values
PID=<pid>
VADDR=0x...   # printed vaddr
PAGE=$(getconf PAGESIZE)
VPN=$(( VADDR / PAGE ))

# read 8-byte entry from pagemap
ENTRY=$(sudo dd if=/proc/$PID/pagemap bs=8 skip=$VPN count=1 2>/dev/null | od -An -t u8)
PFN=$(( ENTRY & ((1<<55)-1) ))
PHYS=$(( PFN * PAGE + VADDR % PAGE ))

# write one byte using dd (careful!)
printf '\x55' > /tmp/one
sudo dd if=/tmp/one of=/dev/mem bs=1 seek=$PHYS conv=notrunc
sync
```

Now observe the helper's output (it prints `buf[0]` after the sleep). On a
non-coherent platform you should see the original value (0xAA) until a cache
invalidation is performed; without the module there is no invalidation, so the
value remains stale.


### Helper Script
This repo includes `scripts/write_phys.sh` to automate the pagemap -> phys
calculation and write one byte to `/dev/mem`. It prompts for confirmation by
default to avoid accidental writes.

Make it executable and run it from a root shell (or with `sudo`). Example:

```bash
chmod +x scripts/write_phys.sh
# interactive prompt
./scripts/write_phys.sh <PID> <VADDR>
# non-interactive (dangerous)
./scripts/write_phys.sh <PID> <VADDR> --yes
```

**Caveats:**
- `/dev/mem` may be restricted on some kernels/distributions; if so use the
  module's `SIMULATE_WRITE` ioctl or run the automated `test_physical_write_proof`.
- Writing to `/dev/mem` is privileged and potentially dangerous; run on test
  hardware only.

## Notes and safety
- Writing to `/dev/mem` is dangerous; run these tests only on isolated test
  hardware.
- If your environment disallows `/dev/mem`, use `CACHE_MEM_SYNC_SIMULATE_WRITE`
  to exercise behavior with the module loaded.
- For production DPDK drivers, prefer integrating cache-sync operations into
  the NIC driver or ensure the userspace driver registers the NIC device with
  `CACHE_MEM_SYNC_SET_DEV` so the kernel helper uses proper DMA ops and IOMMU
  mappings.
