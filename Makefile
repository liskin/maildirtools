CFLAGS=-std=c99 -pedantic -Wall -g $(shell pkg-config --cflags glib-2.0)
LDLIBS=$(shell pkg-config --libs glib-2.0)
ALL=libmaildirpp.so mailcheck
SOURCES=$(wildcard *.c)
DESTDIR=/usr

.PHONY: all clean

all: $(ALL)
clean:
	$(RM) $(wildcard *.o) $(wildcard *.d) $(wildcard $(ALL))
install: all
	$(INSTALL_BIN) libmaildirpp.so $(DESTDIR)/lib
	$(INSTALL_BIN) mailcheck $(DESTDIR)/bin

libmaildirpp.so: libmaildirpp.o maildir.o

mailcheck: mailcheck.o libmaildirpp.so

-include $(SOURCES:.c=.d)


%.so: CFLAGS += -fPIC
%.so: LDFLAGS += -shared
%.so: %.o
	$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@

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
