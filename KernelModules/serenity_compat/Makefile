# Out-of-tree build for serenity_compat.
#
#   make            build against the running kernel
#   make KDIR=...   build against another kernel's headers
#   make install    install, depmod, udev rule, modprobe

obj-m := serenity_compat.o
serenity_compat-objs := serenity_main.o serenity_audio.o serenity_sysfs.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	install -D -m 644 serenity_compat.ko \
		/lib/modules/$(shell uname -r)/extra/serenity_compat.ko
	install -D -m 644 99-serenity-compat.rules /etc/udev/rules.d/99-serenity-compat.rules
	install -D -m 644 serenity-compat.conf /usr/lib/tmpfiles.d/serenity-compat.conf
	install -D -m 644 serenity-compat.modules.conf /etc/modules-load.d/serenity-compat.conf
	depmod -a
	udevadm control --reload-rules
	modprobe serenity_compat || true
	udevadm trigger --subsystem-match=misc

uninstall:
	-rmmod serenity_compat
	-rm -f /lib/modules/$(shell uname -r)/extra/serenity_compat.ko
	-rm -f /etc/udev/rules.d/99-serenity-compat.rules
	-rm -f /usr/lib/tmpfiles.d/serenity-compat.conf /etc/modules-load.d/serenity-compat.conf
	depmod -a

.PHONY: all clean install uninstall
