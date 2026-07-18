CC ?= cc
PKG_CONFIG ?= pkg-config

CAIRO_CFLAGS := $(shell $(PKG_CONFIG) --cflags cairo 2>/dev/null || echo -I/opt/homebrew/include/cairo)
CAIRO_LIBS := $(shell $(PKG_CONFIG) --libs cairo 2>/dev/null || echo -L/opt/homebrew/lib -lcairo)
CPPFLAGS += -Iinclude $(CAIRO_CFLAGS)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra
LDLIBS += $(CAIRO_LIBS) -lm

TARGET := cinderplot
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:.c=.o)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJECTS): include/cinderplot.h

test: $(TARGET)
	sh tests/test.sh

clean:
	rm -f $(TARGET) $(OBJECTS)

.PHONY: clean test
