obj-m += nomfs.o
nomfs-objs := nomfs_main.o nomfs_super.o nomfs_inode.o nomfs_creature.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean