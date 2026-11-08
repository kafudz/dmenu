# dmenu - dynamic menu
# See LICENSE file for copyright and license details.

include config.mk

SRC = drw.c dmenu.c desktop.c stest.c util.c
OBJ = $(SRC:.c=.o)

all: dmenu stest

.c.o:
	$(CC) -c $(CFLAGS) $<

config.h:
	cp config.def.h $@

$(OBJ): arg.h config.h config.mk drw.h desktop.h util.h

dmenu: dmenu.o drw.o desktop.o util.o
	${CC} -o $@ dmenu.o drw.o desktop.o util.o ${LDFLAGS}

stest: stest.o
	$(CC) -o $@ stest.o

tests/desktop_dump: tests/desktop_dump.c desktop.c util.c desktop.h util.h config.mk
	$(CC) $(CFLAGS) -o $@ tests/desktop_dump.c desktop.c util.c

# parser tests; need no X server and no X libraries
test: tests/desktop_dump stest
	sh tests/desktop_test.sh tests/desktop_dump
	sh tests/stest_test.sh ./stest

clean:
	rm -f dmenu stest tests/desktop_dump $(OBJ) dmenu-$(VERSION).tar.gz

dist: clean
	mkdir -p dmenu-$(VERSION)
	cp LICENSE Makefile README README.md arg.h config.def.h config.mk dmenu.1\
		desktop.h drw.h util.h dmenu_apps dmenu_path dmenu_run stest.1 $(SRC)\
		dmenu-$(VERSION)
	cp -r tests dmenu-$(VERSION)
	tar -cf dmenu-$(VERSION).tar dmenu-$(VERSION)
	gzip dmenu-$(VERSION).tar
	rm -rf dmenu-$(VERSION)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f dmenu dmenu_apps dmenu_path dmenu_run stest $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/dmenu
	chmod 755 $(DESTDIR)$(PREFIX)/bin/dmenu_apps
	chmod 755 $(DESTDIR)$(PREFIX)/bin/dmenu_path
	chmod 755 $(DESTDIR)$(PREFIX)/bin/dmenu_run
	chmod 755 $(DESTDIR)$(PREFIX)/bin/stest
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < dmenu.1 > $(DESTDIR)$(MANPREFIX)/man1/dmenu.1
	sed "s/VERSION/$(VERSION)/g" < stest.1 > $(DESTDIR)$(MANPREFIX)/man1/stest.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/dmenu.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/stest.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dmenu\
		$(DESTDIR)$(PREFIX)/bin/dmenu_apps\
		$(DESTDIR)$(PREFIX)/bin/dmenu_path\
		$(DESTDIR)$(PREFIX)/bin/dmenu_run\
		$(DESTDIR)$(PREFIX)/bin/stest\
		$(DESTDIR)$(MANPREFIX)/man1/dmenu.1\
		$(DESTDIR)$(MANPREFIX)/man1/stest.1

.PHONY: all clean dist install uninstall test
