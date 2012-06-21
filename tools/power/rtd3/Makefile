obj-m	:= rtd3.o

KDIR	:= /lib/modules/$(shell uname -r)/build
PWD		:= $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD)

clean:
	- rm -rf *.o *.ko .tmp-versions .*.cmd .*.mod.* *.mod.c
	- rm -rf .tmp_versions* Module.symvers modules.order

install: default
	install -d $(KMISC)
	install -m 644 -c *.ko $(KMISC)
	/sbin/depmod -a

all:	default
