PKG_CONFIG ?= pkg-config
CC         ?= cc
INSTALL    ?= install
RM         ?= rm -f

XORG_CFLAGS := $(shell $(PKG_CONFIG) --cflags xorg-server)
XI_CFLAGS   := $(shell $(PKG_CONFIG) --cflags xi x11)
XI_LIBS     := $(shell $(PKG_CONFIG) --libs xi x11)
IO_URING ?= auto

ifeq ($(filter $(IO_URING),auto 0 1),)
$(error IO_URING must be auto, 0 or 1)
endif

URING_AVAILABLE := $(shell $(PKG_CONFIG) --exists liburing && printf '%s\n' '#include <liburing.h>' 'int main(void) { (void)io_uring_setup_buf_ring; (void)io_uring_prep_read_multishot; (void)io_uring_register_sync_cancel; return IORING_SETUP_NO_SQARRAY == 0; }' | $(CC) $(shell $(PKG_CONFIG) --cflags liburing 2>/dev/null) -x c -fsyntax-only - >/dev/null 2>&1 && echo 1)
ifeq ($(IO_URING),1)
ifneq ($(URING_AVAILABLE),1)
$(error IO_URING=1 requires liburing with multishot, buffer ring and sync cancellation APIs)
endif
endif

ifneq ($(IO_URING),0)
ifeq ($(URING_AVAILABLE),1)
URING_CFLAGS := -DAINPUT_IO_URING $(shell $(PKG_CONFIG) --cflags liburing)
URING_LIBS := $(shell $(PKG_CONFIG) --libs liburing)
endif
endif

CPPFLAGS ?=
OPTFLAGS ?= -O2
CFLAGS   ?= -Wall
LDFLAGS  ?=

# I will still save it
ifeq ($(AGGRESIVE),1)
$(warning AGGRESIVE is misspelled; use AGGRESSIVE=1)
AGGRESSIVE := 1
endif

ifeq ($(AGGRESSIVE),1)
OPTFLAGS = -O3 -flto
LDFLAGS += -flto
endif

ifeq ($(NATIVE),1)
OPTFLAGS += -march=native -mtune=native
endif

ifeq ($(XSERVER_DIRECT),1)
CPPFLAGS += -DAINPUT_XSERVER_DIRECT
endif

ifeq ($(READ_BUDGET_DEBUG),1)
CPPFLAGS += -DAINPUT_READ_BUDGET_DEBUG
endif

DRIVER_CFLAGS = -std=c11 -fPIC -D_POSIX_C_SOURCE=200809L $(XORG_CFLAGS) $(URING_CFLAGS)
TOOL_CFLAGS   = -std=c11 -D_POSIX_C_SOURCE=200809L $(XI_CFLAGS)

DRIVER = ainput_drv.so
SRCS   = src/ainput_drv.c
LATENCY_TOOL = tools/mouse_latency_xi2
KEYBOARD_TOOL = tools/keyboard_latency_xi2

# Directory where Xorg/XLibre looks for input drivers.
DRIVER_DIR ?= $(shell $(PKG_CONFIG) --variable=moduledir xorg-server)/input

.DEFAULT_GOAL := all
.PHONY: all tools latency-tool install uninstall clean FORCE

export AINPUT_BUILD_CC = $(CC)
export AINPUT_BUILD_FLAGS = $(CPPFLAGS) $(OPTFLAGS) $(CFLAGS) $(DRIVER_CFLAGS) $(TOOL_CFLAGS) $(LDFLAGS) $(XI_LIBS) $(URING_LIBS)
export AINPUT_BUILD_OPTIONS = $(IO_URING) $(XSERVER_DIRECT) $(NATIVE) $(AGGRESSIVE) $(READ_BUDGET_DEBUG)
export AINPUT_BUILD_ABI = $(shell $(PKG_CONFIG) --modversion xorg-server) $(shell $(PKG_CONFIG) --variable=abi_xinput xorg-server)

.build-config: FORCE scripts/build_config.py Makefile
	@python3 scripts/build_config.py $@

$(DRIVER) $(LATENCY_TOOL) $(KEYBOARD_TOOL): .build-config

all: $(DRIVER)

tools: $(LATENCY_TOOL) $(KEYBOARD_TOOL)

latency-tool: $(LATENCY_TOOL)

$(DRIVER): $(SRCS)
	$(CC) $(CPPFLAGS) $(OPTFLAGS) $(CFLAGS) $(DRIVER_CFLAGS) $(LDFLAGS) -shared -nostartfiles $(SRCS) -o $(DRIVER) $(URING_LIBS)

$(LATENCY_TOOL): tools/mouse_latency_xi2.c tools/latency_common.h tools/latency_match.h
	$(CC) $(CPPFLAGS) $(OPTFLAGS) $(CFLAGS) $(TOOL_CFLAGS) $(LDFLAGS) tools/mouse_latency_xi2.c -o $(LATENCY_TOOL) $(XI_LIBS)

$(KEYBOARD_TOOL): tools/keyboard_latency_xi2.c tools/latency_common.h tools/latency_match.h
	$(CC) $(CPPFLAGS) $(OPTFLAGS) $(CFLAGS) $(TOOL_CFLAGS) $(LDFLAGS) tools/keyboard_latency_xi2.c -o $@ $(XI_LIBS)

install: $(DRIVER)
	$(INSTALL) -d "$(DESTDIR)$(DRIVER_DIR)"
	@set -eu; \
	tmp=$$(mktemp "$(DESTDIR)$(DRIVER_DIR)/.$(DRIVER).XXXXXX"); \
	trap 'rm -f "$$tmp"' EXIT HUP INT TERM; \
	$(INSTALL) -m755 "$(DRIVER)" "$$tmp"; \
	mv -f "$$tmp" "$(DESTDIR)$(DRIVER_DIR)/$(DRIVER)"; \
	trap - EXIT HUP INT TERM

uninstall:
	$(RM) $(DESTDIR)$(DRIVER_DIR)/$(DRIVER)

clean:
	$(RM) $(DRIVER)
	$(RM) ainput_drv.so-ainput_drv.su
	$(RM) $(LATENCY_TOOL)
	$(RM) $(KEYBOARD_TOOL) .build-config
