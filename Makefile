# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for the GalaxyCore GC2607 sensor driver.
#
#   make            - build gc2607.ko against the running kernel
#   make clean      - remove build artefacts
#   KDIR=/path make - build against a specific kernel source tree

obj-m += gc2607.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
