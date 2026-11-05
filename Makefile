MODULE_NAME := kfetch_mod_315581028

# obj-m: generate a kernel object (.ko)
obj-m := $(MODULE_NAME).o

# set default value for ARCH
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-
# CURDIR: absolute working dir to make instr. running dir
PWD := $(CURDIR)
KDIR ?= $(PWD)/linux-v6.8-devkit

all:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) clean

# -C $(KDIR): change to $(KDIR) dir then read Makefile at that location
#M=$(PWD): external module is here