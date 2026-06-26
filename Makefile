PKG_CONFIG ?= pkg-config
CC         ?= cc
INSTALL    ?= install
RM         ?= rm -f

XORG_CFLAGS := $(shell $(PKG_CONFIG) --cflags xorg-server)
XI_CFLAGS   := $(shell $(PKG_CONFIG) --cflags xi x11)
XI_LIBS     := $(shell $(PKG_CONFIG) --libs xi x11)

CPPFLAGS ?=
OPTFLAGS ?= -O2
CFLAGS   ?= -Wall
LDFLAGS  ?=

ifeq ($(AGGRESSIVE),1)
OPTFLAGS = -O3 -flto
LDFLAGS += -flto
endif

ifeq ($(NATIVE),1)
OPTFLAGS += -march=native -mtune=native
endif

ifeq ($(XSERVER_FAST_REL2D),1)
CPPFLAGS += -DAINPUT_XSERVER_FAST_REL2D
endif

ifeq ($(XSERVER_DIRECT_REL2D),1)
CPPFLAGS += -DAINPUT_XSERVER_DIRECT_REL2D
endif

DRIVER_CFLAGS = -std=c11 -fPIC -D_POSIX_C_SOURCE=200809L $(XORG_CFLAGS)
TOOL_CFLAGS   = -std=c11 -D_POSIX_C_SOURCE=200809L $(XI_CFLAGS)

DRIVER = ainput_drv.so
SRCS   = src/ainput_drv.c
LATENCY_TOOL = tools/mouse_latency_xi2

# Directory where Xorg/XLibre looks for input drivers.
DRIVER_DIR ?= $(shell $(PKG_CONFIG) --variable=moduledir xorg-server)/input

.PHONY: all tools latency-tool install uninstall clean

all: $(DRIVER)

tools: $(LATENCY_TOOL)

latency-tool: $(LATENCY_TOOL)

$(DRIVER): $(SRCS)
	$(CC) $(CPPFLAGS) $(OPTFLAGS) $(CFLAGS) $(DRIVER_CFLAGS) $(LDFLAGS) -shared -nostartfiles $(SRCS) -o $(DRIVER)

$(LATENCY_TOOL): tools/mouse_latency_xi2.c
	$(CC) $(CPPFLAGS) $(OPTFLAGS) $(CFLAGS) $(TOOL_CFLAGS) $(LDFLAGS) tools/mouse_latency_xi2.c -o $(LATENCY_TOOL) $(XI_LIBS)

install: $(DRIVER)
	$(INSTALL) -Dm755 $(DRIVER) $(DESTDIR)$(DRIVER_DIR)/$(DRIVER)

uninstall:
	$(RM) $(DESTDIR)$(DRIVER_DIR)/$(DRIVER)

clean:
	$(RM) $(DRIVER)
	$(RM) $(LATENCY_TOOL)
