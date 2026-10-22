# SPDX-License-Identifier: GPL-2.0-only
obj-m += ax860i_native.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all modules test clean
all: modules
modules:
	python3 build_module.py --kdir "$(KDIR)" --module-dir "$(CURDIR)"
test:
	python3 -m unittest discover -s tests -v
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
