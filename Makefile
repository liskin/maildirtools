# This file is a part of the maildirtools package. See the COPYRIGHT file for
# details.

LDCONFIG=/sbin/ldconfig
CFLAGS=-std=c99 -pedantic -Wall -g $(shell pkg-config --cflags glib-2.0)
LDLIBS=$(shell pkg-config --libs glib-2.0)
SOMAJOR=0
SOMINOR=1
LIBS=libmaildirpp.so
BINS=mailcheck
ALLLIBS=$(foreach lib,$(LIBS),$(lib).$(SOMAJOR).$(SOMINOR) $(lib).$(SOMAJOR) $(lib))
ALL=$(ALLLIBS) $(BINS)
SOURCES=$(wildcard *.c)
DESTDIR=/usr

.PHONY: all clean

all: $(ALL)
clean:
	$(RM) $(wildcard *.o) $(wildcard *.d) $(wildcard $(ALL))
install: all
	for i in $(ALLLIBS); do \
		$(INSTALL_BIN) $$i $(DESTDIR)/lib; \
	done
	for i in $(BINS); do \
		$(INSTALL_BIN) $$i $(DESTDIR)/bin; \
	done
	$(LDCONFIG)

libmaildirpp.so.$(SOMAJOR).$(SOMINOR): libmaildirpp.o maildir.o

mailcheck: LDLIBS += -lncurses
mailcheck: mailcheck.o libmaildirpp.so

-include $(SOURCES:.c=.d)


%.so.$(SOMAJOR).$(SOMINOR): CFLAGS += -fPIC
%.so.$(SOMAJOR).$(SOMINOR): LDFLAGS += -shared \
	-Wl,-soname,$(patsubst %.so.$(SOMAJOR).$(SOMINOR),%.so.$(SOMAJOR),$@)
%.so.$(SOMAJOR).$(SOMINOR): %.o
	$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@

%.so.$(SOMAJOR): %.so.$(SOMAJOR).$(SOMINOR)
	ln -s $< $@
%.so: %.so.$(SOMAJOR)
	ln -s $< $@

%.d: %.c
	@set -e; rm -f $@; \
	$(CC) -M $(CFLAGS) $< > $@.$$$$; \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

INSTALL=install -c -m 644
INSTALL_BIN=install -c -m 755

# if gcc is at least 4.1.3, add -fgnu89-inline
CCVERS=$(shell $(CC) -dumpversion)
ifeq ($(word 1, $(sort 4.1.3 $(CCVERS))),4.1.3)
    CFLAGS+=-fgnu89-inline
endif
