# Usage (native):           make
# Usage (cross, Buildroot): make CROSS_COMPILE=aarch64-linux-gnu-
# Static build (optional):  make STATIC=1
# Install locally:          make install PREFIX=/usr/local
# Install to sysroot:       make install DESTDIR=$(TARGET_DIR) PREFIX=/usr
# Verbose output:           make V=1

# Toolchain selection (Buildroot sets CROSS_COMPILE, CC, etc. automatically)
CC      ?= $(CROSS_COMPILE)gcc
STRIP   ?= $(CROSS_COMPILE)strip
AR      ?= $(CROSS_COMPILE)ar

PROG    := zl30733_id
SRC     := zl30733_id.c
OBJ     := $(SRC:.c=.o)

CPPFLAGS ?=
CFLAGS   ?= -O0 -g -Wall -Wextra -Wformat=2 -Wshadow -Wundef
LDFLAGS  ?=
LDLIBS   ?=

ifeq ($(STATIC),1)
  LDFLAGS += -static
endif

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
INSTALL ?= install
MODE_BIN?= 0755

ifeq ($(V),1)
  Q :=
else
  Q := @
endif

.PHONY: all clean install uninstall strip

all: $(PROG)

$(PROG): $(OBJ)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

strip: $(PROG)
	@echo "  STRIP   $<"
	$(Q)$(STRIP) $(PROG)

install: $(PROG)
	@echo "  INSTALL $(DESTDIR)$(BINDIR)/$(PROG)"
	$(Q)$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(Q)$(INSTALL) -m $(MODE_BIN) "$(PROG)" "$(DESTDIR)$(BINDIR)/$(PROG)"

uninstall:
	@echo "  RM      $(DESTDIR)$(BINDIR)/$(PROG)"
	$(Q)rm -f "$(DESTDIR)$(BINDIR)/$(PROG)"

clean:
	@echo "  CLEAN"
	$(Q)rm -f $(OBJ) $(PROG)
