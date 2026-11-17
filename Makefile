ms912x-y := \
	ms912x_registers.o \
	ms912x_connector.o \
	ms912x_transfer.o \
	ms912x_convert.o \
	ms912x_drv.o

ifeq ($(CONFIG_X86_64),y)
ms912x-y += ms912x_convert_avx2.o
ccflags-y += -DMS912X_HAVE_AVX2
# Only this file may use SIMD registers; callers wrap it in kernel_fpu_begin/end.
CFLAGS_REMOVE_ms912x_convert_avx2.o += -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx -mno-sse4a
CFLAGS_ms912x_convert_avx2.o += $(CC_FLAGS_FPU) -mavx -mavx2 -O3
endif

obj-m := ms912x.o

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build

all:	modules

modules:
	make -C $(KSRC) M=$(CURDIR) modules

clean:
	make -C $(KSRC) M=$(CURDIR) clean
	rm -f $(CURDIR)/Module.symvers $(CURDIR)/*.ur-safe
