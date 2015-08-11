#

TARGETS=grab trig snapchat snapshot
OFILES=snapshot.o reader.o writer.o util.o lut.o ring.o mman.o param.o talloc.o queue.o rtprio.o
XTRAS=grab.o snapchat.o trig.o

build:	${TARGETS}

snapshot:	${OFILES}
	gcc -o snapshot ${OFILES} -lcomedi -lzmq -lpthread

snapchat:	snapchat.o util.o param.o
	gcc -o snapchat snapchat.o util.o param.o  -lzmq

trig:	trig.o util.o param.o
	gcc -o trig trig.o util.o param.o  -lzmq

grab:		lut.o mman.o grab.o
	gcc -o grab lut.o mman.o grab.o -lcomedi


snapshot.o:	snapshot.c snapshot.h reader.h writer.h util.h param.h

snapchat.o:	snapchat.c snapshot.h util.h param.h

reader.o:	reader.c reader.h snapshot.h util.h queue.h param.h ring.h mman.h lut.h tidy.h

writer.o:	writer.c writer.h snapshot.h util.h queue.h param.h mman.h

rtprio.o:	rtprio.c rtprio.h

util.o:		util.c util.h

lut.o:		lut.c lut.h

adc.o:		adc.c adc.h mman.h queue.h ring.h

ring.o:		ring.c ring.h mman.h

mman.o:		mman.c mman.h

param.o:	param.c param.h

queue.o:	queue.c queue.h

tidy.o:		tidy.c tidy.h

talloc.o:	talloc.c talloc.h queue.h

install:	all
	install -D snapshot	 ${DESTDIR}/usr/sbin/snapshot
	install -D snapchat	 ${DESTDIR}/usr/bin/snapchat
	install -D trig	   	 ${DESTDIR}/usr/bin/trig
	install -D grab	   	 ${DESTDIR}/usr/bin/grab
	install -D snapshot.8a	 ${DESTDIR}/usr/share/man/man8
	install -D snapchat.1a	 ${DESTDIR}/usr/share/man/man1
	install -D trig.1a	 ${DESTDIR}/usr/share/man/man1
	install -D grab.1a	 ${DESTDIR}/usr/share/man/man1

clean:
	rm -f ${TARGETS} ${XTRAS} ${OFILES} *~

print.pdf:	*.c *.h
	a2ps -1r -L99 -l198 -o print.ps *.[ch] 
	ps2pdf print.ps
	rm print.ps
