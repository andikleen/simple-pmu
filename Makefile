KDIR = ~/lsrc/obj-2.6.36-full
obj-m := simple-pmu.o
M := make -C ${KDIR} M=`pwd`


all:
	${M} modules

install:
	${M} modules_install

clean:
	${M} clean


