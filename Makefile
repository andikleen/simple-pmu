KDIR = /lib/modules/`uname -r`/build
obj-m := simple-pmu.o
M := make -C ${KDIR} M=`pwd`


all:
	${M} modules

install:
	${M} modules_install

clean:
	${M} clean
	rm -f tcyc

tcyc: tcyc.c cycles.h
	gcc -o tcyc tcyc.c -Wall -O2


	
