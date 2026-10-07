# Usage (native):           make
# Usage (cross, Buildroot): make CROSS_COMPILE=aarch64-linux-gnu-
# Static build (optional):  make STATIC=1
# Build the man pages:      make man        (needs pandoc)
# Install locally:          make install install-man PREFIX=/usr/local
# Install to sysroot:       make install DESTDIR=$(TARGET_DIR) PREFIX=/usr
# Verbose output:           make V=1

ifeq ($(origin CC),default)
  CC    := $(CROSS_COMPILE)gcc
endif
ifeq ($(origin STRIP),default)
  STRIP := $(CROSS_COMPILE)strip
endif
ifeq ($(origin AR),default)
  AR    := $(CROSS_COMPILE)ar
endif

# Programs: one self-contained C source per tool.
PROGS   := zl30733_id zl3073x-dump

# Man pages generated from <name>.<section>.md via pandoc.
MANSRC  := zl3073x-dump.1.md
MANPAGES:= $(MANSRC:.md=)
PANDOC  ?= pandoc

CPPFLAGS ?=
CFLAGS   ?= -O0 -g -Wall -Wextra -Wformat=2 -Wshadow -Wundef
LDFLAGS  ?=
LDLIBS   ?=

ifeq ($(STATIC),1)
  LDFLAGS += -static
endif

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man
INSTALL ?= install
MODE_BIN?= 0755
MODE_DOC?= 0644

ifeq ($(V),1)
  Q :=
else
  Q := @
endif

.PHONY: all man clean install install-man uninstall strip

# Binaries build by default; man pages need pandoc, so build them on demand
# (or via `install-man`).
all: $(PROGS)

man: $(MANPAGES)

# Single-file tools: link straight from the matching <prog>.c (no .o kept).
# The stem is the same for both the underscore and the hyphen binaries.
zl30733_id:   zl30733_id.c
zl3073x-dump: zl3073x-dump.c
zl3073x-dump: LDLIBS += -lm

$(PROGS):
	@echo "  CCLD    $@"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# man page: <name>.<section> from <name>.<section>.md
%.1: %.1.md
	@echo "  PANDOC  $@"
	$(Q)$(PANDOC) -s -t man $< -o $@

strip: $(PROGS)
	@echo "  STRIP   $(PROGS)"
	$(Q)$(STRIP) $(PROGS)

install: $(PROGS)
	@echo "  INSTALL $(DESTDIR)$(BINDIR)/ <- $(PROGS)"
	$(Q)$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(Q)for p in $(PROGS); do \
		$(INSTALL) -m $(MODE_BIN) "$$p" "$(DESTDIR)$(BINDIR)/$$p"; \
	done

# Install man pages (depends on `man` so pandoc runs first).
install-man: man
	@echo "  INSTALL $(DESTDIR)$(MANDIR)/manN/ <- $(MANPAGES)"
	$(Q)for m in $(MANPAGES); do \
		sec=$${m##*.}; \
		$(INSTALL) -d "$(DESTDIR)$(MANDIR)/man$$sec"; \
		$(INSTALL) -m $(MODE_DOC) "$$m" "$(DESTDIR)$(MANDIR)/man$$sec/$$m"; \
	done

uninstall:
	@echo "  RM      binaries + man pages"
	$(Q)for p in $(PROGS); do rm -f "$(DESTDIR)$(BINDIR)/$$p"; done
	$(Q)for m in $(MANPAGES); do sec=$${m##*.}; rm -f "$(DESTDIR)$(MANDIR)/man$$sec/$$m"; done

clean:
	@echo "  CLEAN"
	$(Q)rm -f $(PROGS) $(MANPAGES) *.o
