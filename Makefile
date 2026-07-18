CC ?= cc
PKG_CONFIG ?= pkg-config

# Cairo flags: prefer pkg-config; else fall back to a Homebrew prefix ONLY if
# the headers are really there (macs often have cairo via brew but no
# pkg-config); otherwise stop with a clear error instead of guessing a path
# that may not exist. Pass CAIRO_CFLAGS and CAIRO_LIBS on the command line (as
# the conda recipe does) to bypass detection entirely.
ifndef CAIRO_CFLAGS
ifeq ($(shell $(PKG_CONFIG) --exists cairo 2>/dev/null && echo ok),ok)
CAIRO_CFLAGS := $(shell $(PKG_CONFIG) --cflags cairo)
CAIRO_LIBS := $(shell $(PKG_CONFIG) --libs cairo)
else
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
ifneq ($(wildcard $(BREW_PREFIX)/include/cairo/cairo.h),)
CAIRO_CFLAGS := -I$(BREW_PREFIX)/include/cairo
CAIRO_LIBS := -L$(BREW_PREFIX)/lib -lcairo
else
$(error Cairo not found. Install it — "brew install cairo pkg-config" (macOS) \
or "apt-get install libcairo2-dev pkg-config" (Debian/Ubuntu) — \
or pass CAIRO_CFLAGS and CAIRO_LIBS explicitly)
endif
endif
endif

CPPFLAGS += -D_DEFAULT_SOURCE -Iinclude $(CAIRO_CFLAGS)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra
LDLIBS += $(CAIRO_LIBS) -lm

TARGET := cinderplot
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:.c=.o)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJECTS): include/cinderplot.h

# The regression suite and example data live in the cinderplot-examples repo.
# Build here, then: CINDERPLOT=$(PWD)/$(TARGET) sh ../cinderplot-examples/tests/test.sh

PREFIX ?= /usr/local
install: $(TARGET)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(TARGET) $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f $(TARGET) $(OBJECTS)

.PHONY: clean install
