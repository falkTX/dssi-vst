// -*- c-basic-offset: 4 -*-

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004 Chris Cannam
*/

#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/un.h>
#include <unistd.h>

#include <lo/lo.h>
#include <lo/lo_lowlevel.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "aeffectx.h"
#include "AEffEditor.hpp"

#include "paths.h"

#define APPLICATION_CLASS_NAME "dssi_vst"
#define PLUGIN_ENTRY_POINT "main"

static bool exiting = false;
static HWND hWnd = 0;
static double currentSamplePosition = 0.0;

static int bufferSize = 0;
static int sampleRate = 0;

static int debugLevel = 3;

static AEffect *plugin = 0;
static lo_server oscserver = 0;

static char *hosturl = 0;
static char *hosthostname = 0;
static char *hostport = 0;
static char *hostpath = 0;

using std::cout;
using std::cerr;
using std::endl;

#include "remoteplugin.h" // for RemotePluginVersion


long VSTCALLBACK
hostCallback(AEffect *plugin, long opcode, long index,
	     long value, void *ptr, float opt)
{
    static VstTimeInfo timeInfo;

    switch (opcode) {

    case audioMasterAutomate:
    {
	float actual = plugin->getParameter(plugin, index);
	if (debugLevel > 1) {
	    cerr << "dssi-vst_gui[2]: audioMasterAutomate(" << index << "," << value << ")" << endl;
	    cerr << "dssi-vst_gui[2]: actual value " << actual << endl;
	}
	lo_address hostaddr = lo_address_new(hosthostname, hostport);
	lo_send(hostaddr,
		(std::string(hostpath) + "/control").c_str(),
		"if",
		index,
		actual);
	break;
    }

    case audioMasterVersion:
	if (debugLevel > 1)
	    cerr << "dssi-vst_gui[2]: audioMasterVersion requested" << endl;
	return 2300;

    case audioMasterGetVendorString:
	if (debugLevel > 1)
	    cerr << "dssi-vst_gui[2]: audioMasterGetVendorString requested" << endl;
	strcpy((char *)ptr, "Chris Cannam");
	break;

    case audioMasterGetProductString:
	if (debugLevel > 1)
	    cerr << "dssi-vst_gui[2]: audioMasterGetProductString requested" << endl;
	strcpy((char *)ptr, "DSSI VST Wrapper Plugin GUI");
	break;

    case audioMasterGetVendorVersion:
	if (debugLevel > 1)
	    cerr << "dssi-vst_gui[2]: audioMasterGetVendorVersion requested" << endl;
	return long(RemotePluginVersion * 100);

    case audioMasterGetLanguage:
	if (debugLevel > 1)
	    cerr << "dssi-vst_gui[2]: audioMasterGetLanguage requested" << endl;
	return kVstLangEnglish;

    case audioMasterCanDo:
	if (debugLevel > 1)
	    cerr << "dssi-vst_gui[2]: audioMasterCanDo(" << (char *)ptr
		 << ") requested" << endl;
	if (!strcmp((char*)ptr, "sendVstEvents") ||
	    !strcmp((char*)ptr, "sendVstMidiEvent") ||
	    !strcmp((char*)ptr, "sendVstTimeInfo") ||
	    !strcmp((char*)ptr, "sizeWindow") ||
	    !strcmp((char*)ptr, "supplyIdle")) {
	    return 1;
	}
	break;

    case audioMasterGetTime:
	if (debugLevel > 1)
	    cerr << "dssi-vst_gui[2]: audioMasterGetTime requested" << endl;
	timeInfo.samplePos = currentSamplePosition;
	timeInfo.sampleRate = sampleRate;
	timeInfo.flags = 0; // don't mark anything valid except default samplePos/Rate
	return (long)&timeInfo;

    case audioMasterTempoAt:
	if (debugLevel > 1)
	    cerr << "dssi-vst_gui[2]: audioMasterTempoAt requested" << endl;
	// can't support this, return 120bpm
	return 120 * 10000;

    case audioMasterGetSampleRate:
	if (debugLevel > 1)
	    cerr << "dssi-vst_gui[2]: audioMasterGetSampleRate requested" << endl;
	if (!sampleRate) {
	    cerr << "WARNING: Sample rate requested but not yet set" << endl;
	}
	plugin->dispatcher(plugin, effSetSampleRate,
			   0, 0, NULL, (float)sampleRate);
	break;

    case audioMasterGetBlockSize:
	if (debugLevel > 1)
	    cerr << "dssi-vst_gui[2]: audioMasterGetBlockSize requested" << endl;
	if (!bufferSize) {
	    cerr << "WARNING: Buffer size requested but not yet set" << endl;
	}
	plugin->dispatcher(plugin, effSetBlockSize,
			   0, bufferSize, NULL, 0);
	break;

    case audioMasterWillReplaceOrAccumulate:
	if (debugLevel > 1)
	    cerr << "dssi-vst_gui[2]: audioMasterWillReplaceOrAccumulate requested" << endl;
	// 0 -> unsupported, 1 -> replace, 2 -> accumulate
	return 1;

    case audioMasterGetCurrentProcessLevel:
	// 0 -> unsupported, 1 -> gui, 2 -> process, 3 -> midi/timer, 4 -> offline
	return 2;

    case audioMasterGetParameterQuantization:
	if (debugLevel > 1) {
	    cerr << "dssi-vst_gui[2]: audioMasterGetParameterQuantization requested" << endl;
	}
	return 1;

    case audioMasterNeedIdle:
	if (debugLevel > 1) {
	    cerr << "dssi-vst_gui[2]: audioMasterNeedIdle requested" << endl;
	}
	// might be nice to handle this better
	return 1;

    case audioMasterWantMidi:
	if (debugLevel > 1) {
	    cerr << "dssi-vst_gui[2]: audioMasterWantMidi requested" << endl;
	}
	// happy to oblige
	return 1;

    case audioMasterSizeWindow:
	if (debugLevel > 1) {
	    cerr << "dssi-vst_gui[2]: audioMasterSizeWindow requested" << endl;
	}
	SetWindowPos(hWnd, 0, 0, 0,
		     index + 6,
		     value + 25,
		     SWP_NOACTIVATE | SWP_NOMOVE |
		     SWP_NOOWNERZORDER | SWP_NOZORDER);
	return 1;

    default:
	if (debugLevel > 0) {
	    cerr << "dssi-vst_gui[0]: unsupported audioMaster callback opcode "
		 << opcode << endl;
	}
    }

    return 0;
};

LRESULT WINAPI
MainProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_DESTROY:
	PostQuitMessage(0);
	exiting = true;
	return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

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
    if (argc < 2) {
	cerr << "Error: too few arguments to program_handler" << endl;
	return 1;
    }

    const int bank = argv[0]->i;
    const int program = argv[1]->i;

    cerr << "program_handler(" << bank << "," << program << ")" << endl;

    plugin->dispatcher(plugin, effSetProgram, 0, program, NULL, 0);

    return 0;
}

int
configure_handler(const char *path, const char *types, lo_arg **argv,
		  int argc, void *data, void *user_data)
{
    cerr << "configure_handler" << endl;
    return 0;
}

int
show_handler(const char *path, const char *types, lo_arg **argv,
	     int argc, void *data, void *user_data)
{
    cerr << "show_handler" << endl;

    if (hWnd) {
	ShowWindow(hWnd, SW_SHOWNORMAL);
	UpdateWindow(hWnd);
    }

    if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: showed window" << endl;
    }

    return 0;
}

int
hide_handler(const char *path, const char *types, lo_arg **argv,
	     int argc, void *data, void *user_data)
{
    cerr << "hide_handler" << endl;
    ShowWindow(hWnd, SW_HIDE);

    if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: hid window" << endl;
    }

    return 0;
}

int
quit_handler(const char *path, const char *types, lo_arg **argv,
	     int argc, void *data, void *user_data)
{
    cerr << "quit_handler" << endl;
    PostQuitMessage(0);
    exiting = true;
    return 0;
}

int
control_handler(const char *path, const char *types, lo_arg **argv,
		int argc, void *data, void *user_data)
{
    if (argc < 2) {
	cerr << "Error: too few arguments to control_handler" << endl;
	return 1;
    }

    const int port = argv[0]->i;
    const float value = argv[1]->f;

    cerr << "control_handler(" << port << "," << value << ")" << endl;

    plugin->setParameter(plugin, port, value);

    return 0;
}

int WINAPI
WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR cmdline, int cmdshow)
{
    cout << "DSSI VST plugin GUI host v" << RemotePluginVersion << endl;
    cout << "Copyright (c) 2004 Chris Cannam" << endl;

    char *home = getenv("HOME");

    char *pluginlibname = 0;
    char *label = 0;
    char *friendlyname = 0;

    if (cmdline) {
	int offset = 0;
	if (cmdline[0] == '"' || cmdline[0] == '\'') offset = 1;
	for (int ci = offset; ; ++ci) {
	    if (isspace(cmdline[ci]) || !cmdline[ci]) {
		if (!hosturl) hosturl = strndup(cmdline + offset, ci - offset);
		else if (!pluginlibname) pluginlibname = strndup(cmdline + offset, ci - offset);
		else if (!label) {
		    label = strndup(cmdline + offset, ci - offset);
		    friendlyname = strdup(cmdline + ci + 1);
		    break;
		}
		while (isspace(cmdline[ci])) ++ci;
		if (!cmdline[ci]) break;
		offset = ci;
	    }
	}
    }
    
    if (friendlyname) {
	int l = strlen(friendlyname);
	if (friendlyname[l-1] == '"' ||
	    friendlyname[l-1] == '\'') {
	    friendlyname[l-1] = '\0';
	}
    }

    if (!hosturl || !hosturl[0] ||
	!pluginlibname || !pluginlibname[0] ||
	!label || !label[0] ||
	!friendlyname || !friendlyname[0]) {
	cerr << "Usage: dssi-vst_gui <oscurl>,<plugin.so>,<label>,<friendlyname>" << endl;
	cerr << "(Command line was: " << cmdline << ")" << endl;
	exit(2);
    }

    // LADSPA labels can't contain spaces (good thing too, as they'd
    // confuse our command line parsing above!) so dssi-vst replaces
    // spaces with asterisks.
    for (int ci = 0; label[ci]; ++ci) {
	if (label[ci] == '*') label[ci] = ' ';
    }

    char *libname = label; // VST libname is in label, DSSI libname in pluginlibname

    cout << "Loading \"" << libname << "\"... ";
    if (debugLevel > 0) cout << endl;

    HINSTANCE libHandle = 0;

    std::vector<std::string> vstPath = Paths::getPath
	("VST_PATH", "/usr/local/lib/vst:/usr/lib/vst", "/vst");

    for (size_t i = 0; i < vstPath.size(); ++i) {
	
	std::string vstDir = vstPath[i];
	std::string libPath;

	if (vstDir[vstDir.length()-1] == '/') {
	    libPath = vstDir + libname;
	} else {
	    libPath = vstDir + "/" + libname;
	}

	libHandle = LoadLibrary(libPath.c_str());
	if (debugLevel > 0) {
	    cerr << "dssi-vst_gui[1]: " << (libHandle ? "" : "not ")
		 << "found in " << libPath << endl;
	}

	if (!libHandle) {
	    if (home && home[0] != '\0') {
		if (libPath.substr(0, strlen(home)) == home) {
		    libPath = libPath.substr(strlen(home) + 1);
		}
		libHandle = LoadLibrary(libPath.c_str());
		if (debugLevel > 0) {
		    cerr << "dssi-vst_gui[1]: " << (libHandle ? "" : "not ")
			 << "found in " << libPath << endl;
		}
	    }
	}

	if (libHandle) break;
    }	

    if (!libHandle) {
	libHandle = LoadLibrary(libname);
	if (debugLevel > 0) {
	    cerr << "dssi-vst_gui[1]: " << (libHandle ? "" : "not ")
		 << "found in DLL path" << endl;
	}
    }

    if (!libHandle) {
	std::string message = std::string("Failed to load VST DLL \"") + libname + "\"";
	cerr << "dssi-vst_gui: ERROR: " << message << endl;
	MessageBox(NULL, message.c_str(), NULL, MB_OK);
	return 1;
    }

    cout << "done" << endl;

    cout << "Testing VST compatibility... ";
    if (debugLevel > 0) cout << endl;

//!!! better debug level support
    
    AEffect *(__stdcall* getInstance)(audioMasterCallback);
    getInstance = (AEffect*(__stdcall*)(audioMasterCallback))
	GetProcAddress(libHandle, PLUGIN_ENTRY_POINT);

    if (!getInstance) {
	std::string message = std::string("Bad VST DLL \"") + libname + "\" (entrypoint \""
	    + PLUGIN_ENTRY_POINT + "\" not found)";
	cerr << "dssi-vst_gui: ERROR: " << message << endl;
	MessageBox(NULL, message.c_str(), NULL, MB_OK);
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: VST entrypoint \"" << PLUGIN_ENTRY_POINT
	     << "\" found" << endl;
    }

    plugin = getInstance(hostCallback);

    if (!plugin) {
	cerr << "dssi-vst_gui: ERROR: Failed to instantiate plugin in VST DLL \""
	     << libname << "\"" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: plugin instantiated" << endl;
    }

    if (plugin->magic != kEffectMagic) {
	cerr << "dssi-vst_gui: ERROR: Not a VST plugin in DLL \"" << libname << "\"" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: plugin is a VST" << endl;
    }

    if (!(plugin->flags & effFlagsHasEditor)) {
	std::string message = "No GUI available for this plugin";
	cerr << "dssi-vst_gui: ERROR: " << message << endl;
	MessageBox(NULL, message.c_str(), NULL, MB_OK);
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: plugin has a GUI" << endl;
    }

    cout << "Initialising Windows subsystem... ";
    if (debugLevel > 0) cout << endl;

    WNDCLASSEX wclass;
    wclass.cbSize = sizeof(WNDCLASSEX);
    wclass.style = 0;
    wclass.lpfnWndProc = MainProc;
    wclass.cbClsExtra = 0;
    wclass.cbWndExtra = 0;
    wclass.hInstance = hInst;
    wclass.hIcon = LoadIcon(hInst, APPLICATION_CLASS_NAME);
    wclass.hCursor = LoadCursor(0, IDI_APPLICATION);
    wclass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wclass.lpszMenuName = "MENU_DSSI_VST";
    wclass.lpszClassName = APPLICATION_CLASS_NAME;
    wclass.hIconSm = 0;
    
    if (!RegisterClassEx(&wclass)) {
	cerr << "dssi-vst_gui: ERROR: Failed to register Windows application class!\n" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: registered Windows application class \"" << APPLICATION_CLASS_NAME << "\"" << endl;
    }
    
    hWnd = CreateWindow
	(APPLICATION_CLASS_NAME, friendlyname,
	 WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
	 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
	 0, 0, hInst, 0);
    if (!hWnd) {
	cerr << "dssi-vst_gui: ERROR: Failed to create window!\n" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: created main window" << endl;
    }

    plugin->dispatcher(plugin, effEditOpen, 0, 0, hWnd, 0);
    ERect *rect = 0;
    plugin->dispatcher(plugin, effEditGetRect, 0, 0, &rect, 0);
    if (!rect) {
	cerr << "dssi-vst_gui: ERROR: Plugin failed to report window size\n" << endl;
	return 1;
    }

    // Seems we need to provide space in here for the titlebar and frame,
    // even though we don't know how big they'll be!  How crap.
    SetWindowPos(hWnd, 0, 0, 0,
		 rect->right - rect->left + 6,
		 rect->bottom - rect->top + 25,
		 SWP_NOACTIVATE | SWP_NOMOVE |
		 SWP_NOOWNERZORDER | SWP_NOZORDER);

    if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: sized window" << endl;
    }

    cout << "done" << endl;

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

    lo_address hostaddr = lo_address_new(hosthostname, hostport);
    lo_send(hostaddr,
	    (std::string(hostpath) + "/update").c_str(),
	    "s",
	    (std::string(lo_server_get_url(oscserver)) + "dssi").c_str());

    float **inputs, **outputs;

    if (plugin && plugin->numInputs > 0) {
	inputs = new float *[plugin->numInputs];
	for (int i = 0; i < plugin->numInputs; ++i) {
	    inputs[i] = new float[1024];
	}
    } else {
	inputs = 0;
    }

    if (plugin && plugin->numOutputs > 0) {
	outputs = new float *[plugin->numOutputs];
	for (int i = 0; i < plugin->numOutputs; ++i) {
	    outputs[i] = new float[1024];
	}
    } else {
	outputs = 0;
    }

    MSG msg;
    exiting = false;
    int count = 0;

    while (!exiting) {

	bool idle = true;
	
	if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
	    DispatchMessage(&msg);
	    idle = false;
	}

	if (lo_server_recv_noblock(oscserver, idle ? 30 : 0)) {
	    idle = false;
	}

	++count;

	if (idle || count == 50) {
	    plugin->processReplacing(plugin, inputs, outputs, 1024);
	    usleep(50);
	    count = 0;
	}
    }

    if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: cleaning up" << endl;
    }

    FreeLibrary(libHandle);
    if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: freed dll" << endl;
    }

    if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: exiting" << endl;
    }

    return 0;
}

