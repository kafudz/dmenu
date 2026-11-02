# dmenu version
VERSION = 5.4

# paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

# X11 / Xinerama / FreeType — use pkg-config when available,
# otherwise fall back to the legacy hardcoded paths.
X11INC := $(shell pkg-config --cflags x11 2>/dev/null || echo -I/usr/X11R6/include)
X11LIB := $(shell pkg-config --libs   x11 2>/dev/null || echo -L/usr/X11R6/lib -lX11)

# Xinerama, comment out both lines if you don't want it
XINERAMALIBS  := $(shell pkg-config --libs xinerama 2>/dev/null || echo -lXinerama)
XINERAMAFLAGS := -DXINERAMA

# freetype
FREETYPELIBS := $(shell pkg-config --libs xft fontconfig 2>/dev/null || echo -lfontconfig -lXft)
FREETYPEINC  := $(shell pkg-config --cflags xft fontconfig 2>/dev/null || echo -I/usr/include/freetype2)
# OpenBSD (uncomment)
#FREETYPEINC = $(X11INC)/freetype2
#MANPREFIX = ${PREFIX}/man

# includes and libs
INCS = $(X11INC) $(FREETYPEINC)
LIBS = $(X11LIB) $(XINERAMALIBS) $(FREETYPELIBS)

# flags
CPPFLAGS = -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700 -DVERSION=\"$(VERSION)\" $(XINERAMAFLAGS)
CFLAGS   = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wformat=2 -Wvla -Os $(INCS) $(CPPFLAGS)
LDFLAGS  = $(LIBS)

# compiler and linker
CC = cc