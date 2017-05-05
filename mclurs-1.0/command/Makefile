DOCS	= array-cmd.pdf

build:  array-cmd.8a

install:
	install -D array-cmd	${DESTDIR}/usr/sbin
	install -D array-cmd.8a	${DESTDIR}/usr/share/man/man8
	install -d		${DESTDIR}/usr/share/doc/array-cmd
	for A in docs/*; do install -D $$A ${DESTDIR}/usr/share/doc/array-cmd/$$A; done
	install -D array	${DESTDIR}/usr/bin/array
	install -D pack		${DESTDIR}/usr/bin/pack
	install -D lterm	${DESTDIR}/usr/bin/lterm

clean:
	rm -f array-cmd.8a ${DOCS} print.pdf

print.pdf:	array-cmd
	a2ps -1r -L99 -l198 -o print.ps array-cmd 
	ps2pdf print.ps
	rm print.ps

docs:	${DOCS} print.pdf
.PHONY:	docs

array-cmd.pdf:	array-cmd.8a
	groff -Tps -man $< > xx.ps
	ps2pdf xx.ps $@
	@rm -f xx.ps

array-cmd.8a:	array-cmd
	pod2man --name=array-cmd --section=8a -c "MCLURS ARRAY TOOLS" < array-cmd > array-cmd.8a
