# armv8-cache-mem-sync

Small Linux kernel module for Raspberry Pi 5 that provides explicit cache <-> memory
synchronization helpers for DMA-non-coherent hardware. It is intended to be invoked
by a userspace DPDK poll-mode driver which allocates hugepage-backed buffers.

This repository contains a minimal kernel module and a userspace test scaffold.
Files of interest at the repo root:

- `cache_mem_sync.c` — kernel module implementation (minimal per-page mapping)
- `cache_mem_sync_uapi.h` — ioctl UAPI header
- `test_cache_mem_sync.c` — tiny userspace tester
- `Makefile` — builds the kernel module

Add or replace sources as you iterate; see the build and debug sections below.

Quick workflow

- Mount hugepages (if testing userspace DPDK integration):
```bash
sudo mkdir -p /dev/hugepages
sudo mount -t hugetlbfs nodev /dev/hugepages
```

Build the kernel module (native build):
```bash
make
```

- Cross-compile (host cross-build) example:
```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KERNEL_DIR=/path/to/aarch64/kernel/build
```

Load/unload and inspect kernel logs:
```bash
# load
sudo insmod cache_mem_sync.ko

# follow kernel log output (shows registration/sync progress)
dmesg --follow

# or use journalctl if available
sudo journalctl -k -f

# unload
sudo rmmod cache_mem_sync
```

Check device node and permissions:
```bash
ls -l /dev/cache_mem_sync
# if needed, restrict access to a driver user or group
sudo chown root:dpdk /dev/cache_mem_sync || true
sudo chmod 0660 /dev/cache_mem_sync || true
```

Build the userspace test (on target Linux):
```bash
gcc -O2 -I. -o test_cache_mem_sync test_cache_mem_sync.c
```

Run the test (requires the module loaded):
```bash
./test_cache_mem_sync
```

Makefile notes
- The `Makefile` in this repo builds a single kernel module object named
  `cache_mem_sync.ko`. If you change the module source filename, update the
  `obj-m +=` line in the `Makefile` accordingly.

Debug & monitoring tips
- Watch kernel logs for progress and diagnostics (registration, per-page progress):
  - `dmesg --follow`
  - `sudo journalctl -k -f`
- When a region is registered you should see a log like:
  - `cache_mem_sync: registered region <addr> len=<n> pages=<m> pid=<p>`
- Per-sync operations (SYNC_TO_DEVICE / SYNC_FROM_DEVICE) emit:
  - start line with offset, length and starting page
  - periodic progress every 64 pages
  - completion line
- If you need more or less logging, search for `pr_info("cache_mem_sync:` in
  `cache_mem_sync.c` and edit or add module parameters to control verbosity.

Permissions & safety
- The module pins user pages and performs DMA mappings — restrict which users
  can register regions. By default the misc device may be world-accessible;
  change its ownership/permissions in `udev` or via `chown`/`chmod` after load.
- Limit region sizes in userspace; the module also enforces a 1 GiB upper bound.

Development suggestions for contributors
- Add a minimal `cache_mem_sync.c` implementing `module_init/module_exit` and a
  userspace-facing IO control or misc device (e.g. `ioctl` on a char device).
- Provide a tiny userspace test program that mmap()s a hugepage buffer and
  demonstrates calling the module to `clean` and `invalidate` ranges.
- Add CI workflow (`.github/workflows`) only if you can provide cross-build
  artifacts or run tests on Pi 5 hardware.

Helpful links
- Kernel cross-build guide: https://www.kernel.org/doc/html/latest/kbuild/arch.html
