CFLAGS=-g -Wall
PREFIX?=/usr/local

all: ipnotfd

ipnotfd: ipnotfd.c
	${CC} ${CFLAGS} -o $@ $>

install: ipnotfd
	cp ipnotfd ${DESTDIR}/${PREFIX}/sbin/
	cp ipnotfd.8 ${DESTDIR}/${PREFIX}/man/man8/ipnotfd.8

clean:
	rm -f ipnotfd
