
DSSIDIR		= /usr/local/lib/dssi
BINDIR		= /usr/local/bin
CXXFLAGS	= -I./vstsdk2.4/pluginterfaces/vst2.x -Wall
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

all:		$(TARGETS)

install:	all
		mkdir -p $(DSSIDIR)/dssi-vst
		mkdir -p $(BINDIR)
		install dssi-vst.so $(DSSIDIR)
		install dssi-vst-server.exe.so dssi-vst-server dssi-vst-scanner.exe.so dssi-vst-scanner dssi-vst_gui $(DSSIDIR)/dssi-vst
		install vsthost $(BINDIR)

clean:
		rm -f $(OBJECTS) libremoteplugin.a

distclean:	clean
		rm -f $(TARGETS) dssi-vst-scanner dssi-vst-server *~ *.bak

%.exe.so:	%.cpp libremoteplugin.a $(HEADERS)
		wineg++ $(CXXFLAGS) $< -o $* $(LDFLAGS) -L. -lremoteplugin

libremoteplugin.a:	remotepluginclient.o remotepluginserver.o rdwrops.o paths.o
		ar r $@ $^

remotepluginclient.o:	remotepluginclient.cpp	$(HEADERS)
		g++ $(CXXFLAGS) remotepluginclient.cpp -c

remotevstclient.o:	remotevstclient.cpp $(HEADERS)
		g++ $(CXXFLAGS) remotevstclient.cpp -c

remotepluginserver.o:	remotepluginserver.cpp $(HEADERS)
		g++ $(CXXFLAGS) remotepluginserver.cpp -c

dssi-vst.so:	dssi-vst.cpp libremoteplugin.a remotevstclient.o $(HEADERS)
		g++ -shared -Wl,-Bsymbolic -g3 $(CXXFLAGS) -o dssi-vst.so dssi-vst.cpp remotevstclient.o $(LDFLAGS) -L. -lremoteplugin -lasound

vsthost:	vsthost.cpp libremoteplugin.a remotevstclient.o $(HEADERS)
		g++ $(CXXFLAGS) vsthost.cpp remotevstclient.o -o vsthost $(LDFLAGS) -L. -lremoteplugin -ljack -lasound

dssi-vst_gui:	dssi-vst_gui.cpp rdwrops.h
		g++ $(CXXFLAGS) dssi-vst_gui.cpp rdwrops.o -o dssi-vst_gui $(LDFLAGS) -llo

