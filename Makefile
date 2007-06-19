CFLAGS=-std=c99 -pedantic -fgnu89-inline -Wall -g $(shell pkg-config --cflags glib-2.0)
LDLIBS=$(shell pkg-config --libs glib-2.0)
ALL=libmaildirpp.so mailcheck
SOURCES=$(wildcard *.c)

.PHONY: all clean

all: $(ALL)
clean:
	$(RM) $(wildcard *.o) $(wildcard *.d) $(wildcard $(ALL))

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
