SHELL := /bin/bash

CC := gcc
CFLAGS := -Wall -Wextra -O2
LDLIBS := -lreadline -lhistory -lncurses

TARGET := esh
PREFIX ?= /usr
BINDIR := $(PREFIX)/bin

all: $(TARGET)

$(TARGET): esh.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
	ls -lh ./esh

install: $(TARGET)
	mkdir -p $(DESTDIR)$(BINDIR)
	cp $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	chmod 755 $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
