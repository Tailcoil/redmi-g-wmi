obj-m := redmi-g-wmi.o

KDIR ?= /lib/modules/$(shell uname -r)/build
# 内核用 clang+LTO 编译时，模块必须用同一套工具链
LLVM ?= $(if $(shell grep -s '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config),1)

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) LLVM=$(LLVM) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) LLVM=$(LLVM) clean
