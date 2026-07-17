# vcv-osc — VCV Rack 2 plugin Makefile
#
# Build with the Rack SDK:
#   make RACK_DIR=/path/to/Rack-SDK            # build plugin.{so,dylib,dll}
#   make RACK_DIR=/path/to/Rack-SDK install    # build + copy into Rack's user dir
#
# The target platform (lin/mac/win) is auto-detected from the compiler by the
# SDK's arch.mk. Cross-compiling for Windows from Linux: install mingw-w64 and
# pass CC/CXX=x86_64-w64-mingw32-gcc/g++ with a Windows Rack SDK in RACK_DIR.

RACK_DIR ?= ../Rack-SDK

# All C/C++ sources under src/ (recursive), including the OSC networking layer.
SOURCES += $(wildcard src/*.cpp)
SOURCES += $(wildcard src/osc/*.cpp)

# Bundle the panel SVGs and license (plugin.mk already bundles plugin.json).
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)

# Pull in ARCH_* detection early so the platform conditional below works.
include $(RACK_DIR)/arch.mk

# Winsock is needed on Windows for the UDP OSC transport.
ifdef ARCH_WIN
	LDFLAGS += -lws2_32
endif

# Include the Rack plugin build system.
include $(RACK_DIR)/plugin.mk
