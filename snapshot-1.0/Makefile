#

TARGETS=adc trig snapchat snapshot
OFILES=snapshot.o reader.o writer.o util.o lut.o ring.o mman.o param.o queue.o
XTRAS=adc.o snapchat.o trig.o

all:	${TARGETS}

snapshot:	${OFILES}
	gcc -o snapshot ${OFILES} -lcomedi -lzmq -lpthread

snapchat:	snapchat.o util.o param.o
	gcc -o snapchat snapchat.o util.o param.o  -lzmq

trig:	trig.o util.o param.o
	gcc -o trig trig.o util.o param.o  -lzmq

adc:		lut.o mman.o adc.o
	gcc -o adc lut.o mman.o adc.o -lcomedi


snapshot.o:	snapshot.c snapshot.h reader.h writer.h util.h param.h

snapchat.o:	snapchat.c snapshot.h util.h param.h

reader.o:	reader.c reader.h snapshot.h util.h queue.h param.h ring.h mman.h lut.h

writer.o:	writer.c writer.h snapshot.h util.h queue.h param.h mman.h

util.o:		util.c util.h

lut.o:		lut.h

ring.o:		ring.c ring.h mman.h

mman.o:		mman.c mman.h

param.o:	param.c param.h

queue.o:	queue.c queue.h

install:	all
	install -D snapshot ${DESTDIR}/usr/sbin/snapshot
	install -D snapchat ${DESTDIR}/usr/bin/snapchat
	install -D trig	    ${DESTDIR}/usr/bin/trig
	install -D adc	    ${DESTDIR}/usr/bin/adc

clean:
	rm -f ${TARGETS} ${XTRAS} ${OFILES} *~

print.pdf:	*.c *.h
	a2ps -1r -L99 -l198 -o print.ps *.[ch] 
	ps2pdf print.ps
	rm print.ps
