
DSSIDIR		= /usr/local/lib/dssi
LADSPADIR	= /usr/local/lib/ladspa
BINDIR		= /usr/local/bin

# To compile with the VeSTige compatibility header:
CXXFLAGS	= -Ivestige -Wall -fPIC

# To compile with the official VST SDK v2.4r2:
#CXXFLAGS	= -I./vstsdk2.4/pluginterfaces/vst2.x -Wall -fPIC

LDFLAGS		= 

TARGETS		= dssi-vst-server.exe.so \
		  dssi-vst-scanner.exe.so \
		  dssi-vst.so \
		  dssi-vst_gui \
		  vsthost

HEADERS		= remoteplugin.h \
		  remotepluginclient.h \
		  remotepluginserver.h \
		  remotevstclient.h \
		  rdwrops.h \
		  paths.h

OBJECTS		= remotevstclient.o \
		  remotepluginclient.o \
		  remotepluginserver.o \
		  rdwrops.o \
		  paths.o

OBJECTS_W32	= remotepluginclient.w32.o \
		  remotepluginserver.w32.o \
		  rdwrops.w32.o \
		  paths.w32.o

all:		$(TARGETS)

install:	all
		mkdir -p $(DSSIDIR)/dssi-vst
		mkdir -p $(LADSPADIR)
		mkdir -p $(BINDIR)
		install dssi-vst.so $(DSSIDIR)
		install dssi-vst.so $(LADSPADIR)
		install dssi-vst-server.exe.so dssi-vst-server dssi-vst-scanner.exe.so dssi-vst-scanner dssi-vst_gui $(DSSIDIR)/dssi-vst
		install vsthost $(BINDIR)

clean:
		rm -f $(OBJECTS) $(OBJECTS_W32) libremoteplugin.a libremoteplugin.w32.a

distclean:	clean
		rm -f $(TARGETS) dssi-vst-scanner dssi-vst-server *~ *.bak

%.exe.so:	%.cpp libremoteplugin.w32.a $(HEADERS)
		wineg++ -m32 $(CXXFLAGS) $< -o $* $(LDFLAGS) -L. -lremoteplugin.w32 -lpthread

libremoteplugin.a:	remotepluginclient.o remotepluginserver.o rdwrops.o paths.o
		ar r $@ $^

libremoteplugin.w32.a:	remotepluginclient.w32.o remotepluginserver.w32.o rdwrops.w32.o paths.w32.o
		ar r $@ $^

%.w32.o:	%.cpp $(HEADERS)
		wineg++ -m32 $(CXXFLAGS) $< -c -o $@

%.o:		%.cpp $(HEADERS)
		g++ $(CXXFLAGS) $< -c

dssi-vst.so:	dssi-vst.cpp libremoteplugin.a remotevstclient.o $(HEADERS)
		g++ -shared -Wl,-Bsymbolic -g3 $(CXXFLAGS) -o dssi-vst.so dssi-vst.cpp remotevstclient.o $(LDFLAGS) -L. -lremoteplugin -lasound

vsthost:	vsthost.cpp libremoteplugin.a remotevstclient.o $(HEADERS)
		g++ $(CXXFLAGS) vsthost.cpp remotevstclient.o -o vsthost $(LDFLAGS) -L. -lremoteplugin -ljack -lasound -lpthread

dssi-vst_gui:	dssi-vst_gui.cpp rdwrops.h
		g++ $(CXXFLAGS) dssi-vst_gui.cpp rdwrops.o -o dssi-vst_gui $(LDFLAGS) -llo

