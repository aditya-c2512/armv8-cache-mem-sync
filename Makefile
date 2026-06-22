obj-m += cache_mem_sync.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean


load: all
	sudo insmod $(PWD)/cache_mem_sync.ko


unload:
	sudo rmmod cache_mem_sync || true

.PHONY: all clean load unload

# Cross-build example:
# make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KERNEL_DIR=/path/to/aarch64/kernel/build
