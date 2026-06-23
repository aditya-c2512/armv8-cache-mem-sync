# ARMv8 Cache Memory Synchronisation Helper
```
.
├── cache_mem_sync.c             # Kernel module implementation
├── cache_mem_sync_uapi.h        # Userspace ioctl interface definition
├── test_simulate_write.c        # Userspace test that exercises SIMULATE_WRITE
├── test_physical_write_proof.c  # Proof test that writes via /dev/mem
├── Makefile                     # Kernel module build file
└── README.md

```
to request cache maintenance operations before and after DMA transactions.

---

## Overview

On DMA non-coherent ARM systems, CPU caches and device-visible memory are not
automatically kept consistent.

Before a device reads data written by the CPU:

```

CPU
|
| writes packet data
v
CPU cache
|
| clean cache lines
v
Main memory
|
v
DMA device

```

Before the CPU reads data written by a device:

```

DMA device
|
| writes packet data
v
Main memory
|
| invalidate cache lines
v
CPU cache
|
v
CPU application

```

This module provides the userspace interface required to perform these
ownership transitions.

---

# Features

- ARM64 cache synchronisation support
- Designed for Raspberry Pi 5
- Userspace ioctl interface
- Compatible with DPDK-style hugepage memory regions
- Supports:
  - CPU → device synchronisation
  - Device → CPU synchronisation
- Debug logging for registration and sync operations
- Minimal kernel implementation for experimentation and benchmarking

---

# Repository Structure

```

.
├── cache_mem_sync.c             # Kernel module implementation
├── cache_mem_sync_uapi.h        # Userspace ioctl interface definition
├── test_simulate_write.c        # Userspace test that exercises SIMULATE_WRITE
├── test_physical_write_proof.c  # Proof test that writes via /dev/mem
├── Makefile                     # Kernel module build file
└── README.md

```

---

# Architecture

The intended deployment model is:

```

+-----------------------------+
|        DPDK Application     |
|                             |
|  hugepage packet buffers    |
+-------------+---------------+
|
|
| ioctl()
|
v

+-----------------------------+
|   cache_mem_sync module     |
|                             |
|  pin user pages             |
|  DMA map memory             |
|  cache maintenance          |
+-------------+---------------+
|
|
v

+-----------------------------+
|       DMA Hardware          |
|       NIC / MACB            |
+-----------------------------+

````

---

# Requirements

## Hardware

**Test target:**

- Raspberry Pi 5
- ARMv8 / AArch64 processor
- DMA-capable peripheral

## Software

**Required:**

- Linux kernel headers
- GCC compiler
- Make
- Root privileges for module loading


**Install dependencies:**

```bash
sudo apt install build-essential linux-headers-$(uname -r)
````

---

# Building

- Clone the repository:

```bash
git clone https://github.com/aditya-c2512/armv8-cache-mem-sync.git
cd armv8-cache-mem-sync
```

- Build the kernel module:

```bash
make
```

- Successful compilation produces:

```
cache_mem_sync.ko
```

---

# Loading the Kernel Module

- Insert the module:

```bash
sudo insmod cache_mem_sync.ko
```

- Verify:

```bash
dmesg | tail
```

- Expected output:

```
cache_mem_sync: module loaded
```

---

# Userspace Test Application

- Build the test program:

```bash
gcc -O2 -I. -o test_simulate_write test_simulate_write.c
```

- Find Platform/PCI Device name and pass to test application. (See "Device Discovery")
- Run:
```bash
sudo ./test_simulate_write [device_name]
```
For example:
```bash
sudo ./test_simulate_write 1f00100000.ethernet
```

The application:

1. Allocates a test buffer
2. Registers the memory region
3. Requests cache synchronisation
4. Verifies ioctl communication with the kernel module

---

# Device Discovery

For DMA devices requiring explicit identification, determine the device name
using the following methods.

## Device Tree Search

```bash
grep -R "cdns,gem" \
/proc/device-tree \
/sys/firmware/devicetree/base \
2>/dev/null || true
```

## Platform Devices (RPi Cadence GEM/MACB is a Platform Device)

For platform devices, find a DMA device using the bellow command:

```bash
ls /sys/bus/platform/devices
```

## PCI Devices

For PCI-attached devices:

```bash
ls /sys/bus/pci/devices
```

Example:

```
0000:01:00.0
```

Pass the device name to the userspace tester:

```bash
sudo ./test_simulate_write <device-name>
```

Example:

```bash
sudo ./test_simulate_write 0000:01:00.0
```

---

# Kernel Interface

The module exposes cache synchronisation operations through ioctl commands.

## Register Memory Region

A userspace application first registers the DMA buffer:

```c
struct cache_mem_region {
    uint64_t address;
    uint64_t length;
};
```

The module:

1. Pins the user pages
2. Creates DMA mappings
3. Stores the mapping information

Registration occurs once during application startup.

---

## Synchronise CPU → Device

Used before transmitting data:

```
CPU writes buffer
        |
        v
SYNC_TO_DEVICE
        |
        v
DMA device reads buffer
```

Example:

```c
ioctl(fd, CACHE_SYNC_TO_DEVICE, &request);
```

---

## Synchronise Device → CPU

Used before processing received data:

```
DMA device writes buffer
        |
        v
SYNC_FROM_DEVICE
        |
        v
CPU reads buffer
```

Example:

```c
ioctl(fd, CACHE_SYNC_FROM_DEVICE, &request);
```

---

# Debugging

## Kernel Logs

Monitor module output:

```bash
dmesg --follow
```

or:

```bash
sudo journalctl -k -f
```

---

## Registration Messages

Successful registration:

```
cache_mem_sync:
registered region
addr=<address>
len=<size>
pages=<count>
pid=<pid>
```

---

## Synchronisation Messages

Each synchronisation request reports:

```
SYNC_TO_DEVICE
offset=<offset>
length=<length>
page=<page index>
```

Large regions periodically report progress:

```
processed 64 pages
processed 128 pages
...
```

---

# Permissions and Safety

This module performs privileged memory operations:

* user page pinning
* DMA mappings
* cache maintenance

Restrict access to trusted applications.

After loading, check permissions:

```bash
ls -l /dev/cache_mem_sync
```

Modify permissions if required:

```bash
sudo chmod 660 /dev/cache_mem_sync
```

For persistent deployments, configure a udev rule.

---

# Memory Limits

The module enforces a maximum registered region size:

```
1 GiB
```

This prevents accidental registration of excessive memory.

---

# DPDK Integration

The intended use case is a custom DPDK PMD running on Raspberry Pi 5.

Typical workflow:

```
DPDK EAL
 |
 | allocate hugepage memory
 |
 v

PMD initialisation

 |
 | register DMA region
 |
 v

cache_mem_sync


Packet TX:

mbuf data
   |
   v
SYNC_TO_DEVICE
   |
   v
NIC DMA


Packet RX:

NIC DMA
   |
   v
SYNC_FROM_DEVICE
   |
   v
DPDK application
```

---

# Performance Considerations

This project intentionally separates:

## Initialisation Path

Expensive:

```
pin pages
DMA map
setup metadata
```

Performed once.

---

## Datapath

Fast:

```
cache synchronisation only
```

Performed per packet burst.

---

# Limitations

This project is intended for experimentation and driver development.

It does not replace the Linux DMA subsystem.

Considerations:

* cache line operations are architecture-specific
* incorrect cache maintenance can corrupt DMA buffers
* userspace applications must correctly manage ownership transitions
* production drivers should normally use the Linux DMA API directly

---

# Future Work

Possible improvements:

* Support multiple registered regions
* Add hugepage-specific registration path
* Remove ioctl overhead using mmap/shared state
* Integrate directly into a DPDK PMD
* Benchmark against:

  * `dma_sync_single_for_device()`
  * `dma_alloc_coherent()`
  * direct ARM cache instructions

---

# References

* Linux Kernel DMA API documentation
  [https://www.kernel.org/doc/html/latest/core-api/dma-api.html](https://www.kernel.org/doc/html/latest/core-api/dma-api.html)

* Linux Kernel build documentation
  [https://www.kernel.org/doc/html/latest/kbuild/arch.html](https://www.kernel.org/doc/html/latest/kbuild/arch.html)

---

# License

GPL-2.0

```

This version reads more like a real systems research repository README: it explains **why the module exists**, **how it fits into DPDK**, and **what the limitations are**, rather than only documenting commands.
```
