#!/usr/bin/make -f
# Makefile for dssi-vst #
# ------------------------------- #
# Created by falkTX
#

CXX     ?= g++
WINECXX ?= wineg++ -m32

PREFIX  ?= /usr/local

BIN_DIR    = $(PREFIX)/bin
DSSI_DIR   = $(PREFIX)/lib/dssi
LADSPA_DIR = $(PREFIX)/lib/ladspa

BUILD_FLAGS  = -O2 -ffast-math -fomit-frame-pointer -fvisibility=hidden -fPIC -mtune=generic -msse -Wall -Ivestige $(CXX_FLAGS)
# BUILD_FLAGS  = -O0 -g -fPIC -Wall -Ivestige $(CXX_FLAGS)
BUILD_FLAGS += $(shell pkg-config --cflags alsa liblo)
LINK_FLAGS   = $(LDFLAGS)

LINK_PLUGIN = -shared $(shell pkg-config --libs alsa) $(LINK_FLAGS)
LINK_HOST   = $(shell pkg-config --libs alsa jack) $(LINK_FLAGS)
LINK_GUI    = $(shell pkg-config --libs liblo) $(LINK_FLAGS)
LINK_WINE   = -m32 -L/usr/lib32/wine -L/usr/lib/i386-linux-gnu/wine -lpthread $(LINK_FLAGS)

TARGETS     = dssi-vst.so dssi-vst_gui vsthost dssi-vst-scanner.exe dssi-vst-server.exe

# --------------------------------------------------------------

all: $(TARGETS)

dssi-vst.so: dssi-vst.o remotevstclient.o libremoteplugin.unix.a
	$(CXX) $^ $(LINK_PLUGIN) -o $@

dssi-vst_gui: dssi-vst_gui.o rdwrops.o
	$(CXX) $^ $(LINK_GUI) -o $@

dssi-vst-scanner.exe: dssi-vst-scanner.wine.o libremoteplugin.wine.a
	$(WINECXX) $^ $(LINK_WINE) -o $@

dssi-vst-server.exe: dssi-vst-server.wine.o libremoteplugin.wine.a
	$(WINECXX) $^ $(LINK_WINE) -o $@

vsthost: remotevstclient.o vsthost.o libremoteplugin.unix.a
	$(CXX) $^ $(LINK_HOST) -o $@

# --------------------------------------------------------------

paths.unix.o:
	$(CXX) paths.cpp $(BUILD_FLAGS) -c -o $@

remotepluginclient.unix.o:
	$(CXX) remotepluginclient.cpp $(BUILD_FLAGS) -c -o $@

remotepluginserver.unix.o:
	$(CXX) remotepluginserver.cpp $(BUILD_FLAGS) -c -o $@

rdwrops.unix.o:
	$(CXX) rdwrops.cpp $(BUILD_FLAGS) -c -o $@

libremoteplugin.unix.a: paths.unix.o remotepluginclient.unix.o remotepluginserver.unix.o rdwrops.unix.o
	ar rs $@ $^

# --------------------------------------------------------------

paths.wine.o:
	$(WINECXX) paths.cpp $(BUILD_FLAGS) -c -o $@

remotepluginclient.wine.o:
	$(WINECXX) remotepluginclient.cpp $(BUILD_FLAGS) -c -o $@

remotepluginserver.wine.o:
	$(WINECXX) remotepluginserver.cpp $(BUILD_FLAGS) -c -o $@

rdwrops.wine.o:
	$(WINECXX) rdwrops.cpp $(BUILD_FLAGS) -c -o $@

dssi-vst-scanner.wine.o:
	$(WINECXX) dssi-vst-scanner.cpp $(BUILD_FLAGS) -c -o $@

dssi-vst-server.wine.o:
	$(WINECXX) dssi-vst-server.cpp $(BUILD_FLAGS) -c -o $@

libremoteplugin.wine.a: paths.wine.o remotepluginclient.wine.o remotepluginserver.wine.o rdwrops.wine.o
	ar rs $@ $^

# --------------------------------------------------------------

.cpp.o:
	$(CXX) $< $(BUILD_FLAGS) -c -o $@

# --------------------------------------------------------------

clean:
	rm -f *.a *.o *.exe *.so $(TARGETS)
