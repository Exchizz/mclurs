
build:
	pod2man --name=array-cmd --section=8a -c "MCLURS ARRAY TOOLS" < array-cmd > array-cmd.8a

install:
	install -D array-cmd	${DESTDIR}/usr/sbin
	install -D array-cmd.8a	${DESTDIR}/usr/share/man/man8
	install -d		${DESTDIR}/usr/share/doc/array-cmd
	for A in docs/*; do install $A ${DESTDIR}/usr/share/doc/array-cmd; done

clean:
	rm array-cmd.8a
