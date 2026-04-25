# Target name
TARGET = kpsintent.so
SRC = kpsintent.c

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O3 -fPIC

# OS detection
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
    LDFLAGS = -shared
    AUDIO_LIBS = -lasound
endif
ifeq ($(UNAME_S),Darwin)
    LDFLAGS = -bundle -undefined dynamic_lookup
    AUDIO_LIBS = -framework CoreAudio -framework AudioToolbox
endif

# Libraries
# Use pkg-config to find Lua. Set to lua5.5 as the primary target.
LUA_PKG = lua5.5
LUA_CFLAGS = $(shell pkg-config --cflags $(LUA_PKG) 2>/dev/null || pkg-config --cflags lua5.4 || pkg-config --cflags lua)
LUA_LIBS = $(shell pkg-config --libs $(LUA_PKG) 2>/dev/null || pkg-config --libs lua5.4 || pkg-config --libs lua)

LUA_BIN = $(shell which lua5.5 2>/dev/null || which lua5.4 || which lua)

LIBS = $(LUA_LIBS) $(AUDIO_LIBS) -lm

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LUA_CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)

test: $(TARGET)
	$(LUA_BIN) test.lua

.PHONY: all clean test