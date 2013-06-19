#!/usr/bin/make -f
# Makefile for dssi-vst #
# ------------------------------- #
# Created by falkTX
#

CXX     ?= g++
WINECXX ?= wineg++ -m32

PREFIX  ?= /usr/local

BIN_DIR    = $(DESTDIR)$(PREFIX)/bin
DSSI_DIR   = $(DESTDIR)$(PREFIX)/lib/dssi
LADSPA_DIR = $(DESTDIR)$(PREFIX)/lib/ladspa

BUILD_FLAGS  = -O2 -ffast-math -fvisibility=hidden -fPIC -mtune=generic -msse -Wall -Ivestige $(CXX_FLAGS)
BUILD_FLAGS += $(shell pkg-config --cflags alsa liblo zlib)
LINK_FLAGS   = $(shell pkg-config --libs zlib) $(LDFLAGS)

LINK_PLUGIN = -shared $(shell pkg-config --libs alsa jack) $(LINK_FLAGS)
LINK_HOST   = $(shell pkg-config --libs alsa jack) $(LINK_FLAGS)
LINK_GUI    = $(shell pkg-config --libs liblo) $(LINK_FLAGS)
LINK_WINE   = -m32 -L/usr/lib32/wine -L/usr/lib/i386-linux-gnu/wine -lpthread -lrt $(LINK_FLAGS)

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
	$(WINECXX) $^ $(LINK_WINE) -ljack -o $@

vsthost: remotevstclient.o vsthost.o libremoteplugin.unix.a
	$(CXX) $^ $(LINK_HOST) -o $@

# --------------------------------------------------------------

paths.unix.o: paths.cpp
	$(CXX) $^ $(BUILD_FLAGS) -c -o $@

remotepluginclient.unix.o: remotepluginclient.cpp
	$(CXX) $^ $(BUILD_FLAGS) -c -o $@

remotepluginserver.unix.o: remotepluginserver.cpp
	$(CXX) $^ $(BUILD_FLAGS) -c -o $@

rdwrops.unix.o: rdwrops.cpp
	$(CXX) $^ $(BUILD_FLAGS) -c -o $@

libremoteplugin.unix.a: paths.unix.o remotepluginclient.unix.o remotepluginserver.unix.o rdwrops.unix.o
	ar rs $@ $^

# --------------------------------------------------------------

paths.wine.o: paths.cpp
	$(WINECXX) $^ $(BUILD_FLAGS) -c -o $@

remotepluginclient.wine.o: remotepluginclient.cpp
	$(WINECXX) $^ $(BUILD_FLAGS) -c -o $@

remotepluginserver.wine.o: remotepluginserver.cpp
	$(WINECXX) $^ $(BUILD_FLAGS) -c -o $@

rdwrops.wine.o: rdwrops.cpp
	$(WINECXX) $^ $(BUILD_FLAGS) -c -o $@

dssi-vst-scanner.wine.o: dssi-vst-scanner.cpp
	$(WINECXX) $^ $(BUILD_FLAGS) -O0 -c -o $@

dssi-vst-server.wine.o: dssi-vst-server.cpp
	$(WINECXX) $^ $(BUILD_FLAGS) -O0 -c -o $@

libremoteplugin.wine.a: paths.wine.o remotepluginclient.wine.o remotepluginserver.wine.o rdwrops.wine.o
	ar rs $@ $^

# --------------------------------------------------------------

.cpp.o:
	$(CXX) $< $(BUILD_FLAGS) -c -o $@

# --------------------------------------------------------------

clean:
	rm -f *.a *.o *.exe *.so $(TARGETS)

install:
	install -d $(BIN_DIR)
	install -d $(DSSI_DIR)
	install -d $(DSSI_DIR)/dssi-vst
	install -d $(LADSPA_DIR)
	install -m 755 vsthost $(BIN_DIR)
	install -m 755 dssi-vst.so $(DSSI_DIR)
	install -m 755 dssi-vst.so $(LADSPA_DIR)
	install -m 755 dssi-vst_gui $(DSSI_DIR)/dssi-vst
	install -m 755 dssi-vst-scanner.exe dssi-vst-scanner.exe.so $(DSSI_DIR)/dssi-vst
	install -m 755 dssi-vst-server.exe dssi-vst-server.exe.so $(DSSI_DIR)/dssi-vst
