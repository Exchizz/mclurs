#

CFLAGS	    = -Wall -Wno-format
TARGETS     = grab trig snapchat snapshot
DOCS	    = ${TARGETS:=.pdf}
OFILES      = snapshot.o reader.o writer.o tidy.o
LIBOBJS     = util.o lut.o mman.o param.o queue.o rtprio.o strbuf.o chunk.o adc.o
XTRAS       = grab.o snapchat.o trig.o
LIBRARY     = utilities.a
ARGTAB_SRCS = argtab-int16.c argtab-int64.c
ARGTAB_XTRA = argtab-helper.h
ARGTAB_OBJS = ${ARGTAB_SRCS:.c=.o}
ARGTAB_HDRS = ${ARGTAB_SRCS:.c=.h} ${ARGTAB_XTRA}

build:	${TARGETS}
.PHONY:	build

snapchat:	snapchat.o ${LIBRARY}
	gcc -o snapchat snapchat.o ${LIBRARY} -lzmq -largtable2

trig:	trig.o ${LIBRARY}
	gcc -o trig trig.o ${LIBRARY} -lzmq -largtable2

grab:		grab.o ${LIBRARY}
	gcc -o grab grab.o ${LIBRARY} -lcomedi -largtable2 -lm

snapshot:	${OFILES} ${LIBRARY}
	gcc -o snapshot ${OFILES} ${LIBRARY} -lcomedi -lzmq -lpthread -largtable2 -lcap

snapchat.o:	snapchat.c snapshot.h argtab.h util.h param.h

grab.o:		grab.c mman.h lut.h argtab.h

trig.o:		trig.c snapshot.h util.h param.h argtab.h

snapshot.o:	snapshot.c snapshot.h reader.h writer.h util.h param.h argtab.h general.h error.h

reader.o:	reader.c reader.h snapshot.h util.h queue.h param.h adc.h mman.h tidy.h chunk.h general.h error.h

writer.o:	writer.c writer.h snapshot.h util.h queue.h param.h mman.h chunk.h general.h error.h

tidy.o:		tidy.c tidy.h chunk.h general.h error.h

adc.o:		adc.c adc.h mman.h queue.h general.h error.h

rtprio.o:	rtprio.c rtprio.h general.h

util.o:		util.c util.h general.h

lut.o:		lut.c lut.h general.h

chunk.o:	chunk.c chunk.h mman.h queue.h general.h error.h

mman.o:		mman.c mman.h general.h

strbuf.o:	strbuf.c queue.h strbuf.h general.h

param.o:	param.c param.h argtab.h assert.h general.h

queue.o:	queue.c queue.h general.h

argtab-int64.o:	argtab-int64.c argtab-int64.h general.h

argtab-int16.o:	argtab-int16.c argtab-int16.h general.h

argtab.a:	${ARGTAB_OBJS}
	ar rcs argtab.a ${ARGTAB_OBJS}

argtab.h:	${ARGTAB_HDRS}
	( echo '#'; for ARG in ${ARGTAB_HDRS}; do echo '#include' "\"$${ARG}\"" ; done ) > argtab.h

${LIBRARY}:	${LIBOBJS}
	ar crs	$@ $?
	ranlib	$@

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
	rm -f ${TARGETS} ${DOCS} ${XTRAS} ${OFILES} ${LIBRARY} ${LIBOBJS} argtab.a argtab.h ${ARGTAB_OBJS} *~

docs:	${DOCS} print.pdf
.PHONY:	docs

grab.pdf:  grab.1a
	groff -Tps -man $< > xx.ps
	ps2pdf xx.ps $@
	@rm -f xx.ps

trig.pdf:  trig.1a
	groff -Tps -man $< > xx.ps
	ps2pdf xx.ps $@
	@rm -f xx.ps

snapchat.pdf:  snapchat.1a
	groff -Tps -man $< > xx.ps
	ps2pdf xx.ps $@
	@rm -f xx.ps

snapshot.pdf:	snapshot.8a
	groff -Tps -man $< > xx.ps
	ps2pdf xx.ps $@
	@rm -f xx.ps

print.pdf:	*.c *.h
	a2ps -1r -L99 -l198 -o print.ps *.[ch] 
	ps2pdf print.ps
	rm print.ps

