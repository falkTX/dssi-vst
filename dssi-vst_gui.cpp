// -*- c-basic-offset: 4 -*-

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004-2010 Chris Cannam
*/

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <map>

#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>

#include <lo/lo.h>
#include <lo/lo_lowlevel.h>

#include "rdwrops.h"

static int debugLevel = 3;
static bool ready = false;
static bool exiting = false;

static lo_server oscserver = 0;

static char *hosturl = 0;
static lo_address hostaddr = 0;
static char *hosthostname = 0;
static char *hostport = 0;
static char *hostpath = 0;

static char *fifoFile = 0;
static int fifoFd = -1;

using std::cout;
using std::cerr;
using std::endl;

#include "remoteplugin.h" // for RemotePluginVersion

void
osc_error(int num, const char *msg, const char *path)
{
    cerr << "Error: liblo server error " << num
	 << " in path \"" << (path ? path : "(null)")
	 << "\": " << msg << endl;
}

int
debug_handler(const char *path, const char *types, lo_arg **argv,
	      int argc, void *data, void *user_data)
{
    int i;

    cerr << "Warning: unhandled OSC message in GUI:" << endl;

    for (i = 0; i < argc; ++i) {
	cerr << "arg " << i << ": type '" << types[i] << "': ";
        lo_arg_pp((lo_type)types[i], argv[i]);
	cerr << endl;
    }

    cerr << "(path is <" << path << ">)" << endl;
    return 1;
}

int
program_handler(const char *path, const char *types, lo_arg **argv,
	       int argc, void *data, void *user_data)
{
    cerr << "dssi-vst_gui: program_handler" << endl;
    return 0;
}

int
configure_handler(const char *path, const char *types, lo_arg **argv,
		  int argc, void *data, void *user_data)
{
    cerr << "dssi-vst_gui: configure_handler, returning 0" << endl;
    return 0;
}

int
show_handler(const char *path, const char *types, lo_arg **argv,
	     int argc, void *data, void *user_data)
{
    cerr << "dssi-vst_gui: show_handler" << endl;

    lo_send(hostaddr,
	    (std::string(hostpath) + "/configure").c_str(),
	    "ss",
	    "guiVisible",
	    fifoFile);

    return 0;
}

int
hide_handler(const char *path, const char *types, lo_arg **argv,
	     int argc, void *data, void *user_data)
{
    cerr << "dssi-vst_gui: hide_handler" << endl;

    lo_send(hostaddr,
	    (std::string(hostpath) + "/configure").c_str(),
	    "ss",
	    "guiVisible",
	    "");

    return 0;
}

int
quit_handler(const char *path, const char *types, lo_arg **argv,
	     int argc, void *data, void *user_data)
{
    cerr << "quit_handler" << endl;
    exiting = true;
    return 0;
}

int
control_handler(const char *path, const char *types, lo_arg **argv,
		int argc, void *data, void *user_data)
{
    static int count = 0;
    cerr << "dssi-vst_gui: control_handler " << count++ << endl;
    return 0;
}

void
readFromPlugin()
{
    cerr << "dssi-vst_gui: something to read from plugin" << endl;

    try {
	RemotePluginOpcode opcode = RemotePluginNoOpcode;
	tryRead(fifoFd, &opcode, sizeof(RemotePluginOpcode));

	switch (opcode) {
	    
	case RemotePluginIsReady:
	    ready = true;
	    break;

	case RemotePluginSetParameter:
	{
	    int port = readInt(fifoFd);
	    float value = readFloat(fifoFd);

	    cerr << "dssi-vst_gui: sending (" << port << "," << value << ") to host" << endl;

	    lo_send(hostaddr,
		    (std::string(hostpath) + "/control").c_str(),
		    "if", port, value);
	    break;
	}

	case RemotePluginTerminate:
	    cerr << "dssi-vst_gui: asked to terminate" << endl;
	    lo_send(hostaddr,
		    (std::string(hostpath) + "/exiting").c_str(),
		    "");
	    exiting = true;
	    break;

	default:
	    std::cerr << "WARNING: dssi-vst_gui: unexpected opcode "
		      << opcode << std::endl;
	    break;
	}
    } catch (...) { }
}

int
main(int argc, char **argv)
{
    cout << "DSSI VST plugin GUI controller v" << RemotePluginVersion << endl;
    cout << "Copyright (c) 2004-2010 Chris Cannam" << endl;

    char *pluginlibname = 0;
    char *label = 0;
    char *friendlyname = 0;

    if (argc != 5) {
	cerr << "Usage: dssi-vst_gui <osc url> <plugin.so> <label> <friendlyname>" << endl;
	exit(2);
    }

    hosturl = argv[1];
    pluginlibname = argv[2];
    label = argv[3];
    friendlyname = argv[4];

    if (!hosturl || !hosturl[0] ||
	!pluginlibname || !pluginlibname[0] ||
	!label || !label[0] ||
	!friendlyname || !friendlyname[0]) {
	cerr << "Usage: dssi-vst_gui <osc url> <plugin.so> <label> <friendlyname>" << endl;
	exit(2);
    }

    char tmpFileBase[60];

    sprintf(tmpFileBase, "/tmp/rplugin_gui_XXXXXX");
    if (mkstemp(tmpFileBase) < 0) {
	cerr << "Failed to obtain temporary filename" << endl;
	exit(1);
    }
    fifoFile = strdup(tmpFileBase);

    unlink(fifoFile);
    if (mkfifo(fifoFile, 0666)) { //!!! what permission is correct here?
	perror(fifoFile);
	cerr << "Failed to create FIFO" << endl;
	exit(1);
    }

    if ((fifoFd = open(fifoFile, O_RDONLY | O_NONBLOCK)) < 0) {
	perror(fifoFile);
	cerr << "Failed to open FIFO" << endl;
	unlink(fifoFile);
	exit(1);
    }

    oscserver = lo_server_new(NULL, osc_error);
    lo_server_add_method(oscserver, "/dssi/control", "if", control_handler, 0);
    lo_server_add_method(oscserver, "/dssi/program", "ii", program_handler, 0);
    lo_server_add_method(oscserver, "/dssi/configure", "ss", configure_handler, 0);
    lo_server_add_method(oscserver, "/dssi/show", "", show_handler, 0);
    lo_server_add_method(oscserver, "/dssi/hide", "", hide_handler, 0);
    lo_server_add_method(oscserver, "/dssi/quit", "", quit_handler, 0);
    lo_server_add_method(oscserver, NULL, NULL, debug_handler, 0);

    hosthostname = lo_url_get_hostname(hosturl);
    hostport = lo_url_get_port(hosturl);
    hostpath = lo_url_get_path(hosturl);

    cout << "created lo server (url is " << lo_server_get_url(oscserver) << ") - update path is " << std::string(hostpath) << "/update" << endl;

    hostaddr = lo_address_new(hosthostname, hostport);
    lo_send(hostaddr,
	    (std::string(hostpath) + "/update").c_str(),
	    "s",
	    (std::string(lo_server_get_url(oscserver)) + "dssi").c_str());

    exiting = false;
    bool idle = true;
    struct timeval tv;
    gettimeofday(&tv, NULL);

    while (!exiting) {

	bool idleHere = true;

	struct pollfd pfd;
	pfd.fd = fifoFd;
	pfd.events = POLLIN;

	if (poll(&pfd, 1, 0) > 0) {
	    readFromPlugin();
	    idleHere = false;
	}

	if (lo_server_recv_noblock(oscserver, idle ? 30 : 0)) {
	    idleHere = false;
	}

	idle = idleHere;

	if (!ready) {
	    struct timeval tv1;
	    gettimeofday(&tv1, NULL);
	    if (tv1.tv_sec > tv.tv_sec + 40) {
		cerr << "dssi-vst_gui: No contact from plugin -- timed out on startup" << endl;
		lo_send(hostaddr,
			(std::string(hostpath) + "/exiting").c_str(),
			"");
		break;
	    }
	}
    }

    if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: exiting" << endl;
    }

    close(fifoFd);
    unlink(fifoFile);

    return 0;
}

