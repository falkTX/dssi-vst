// -*- c-basic-offset: 4 -*-

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004 Chris Cannam
*/

#include <iostream>
#include <fstream>
#include <string>

#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/un.h>

#include <lo/lo.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "aeffectx.h"
#include "AEffEditor.hpp"

#define APPLICATION_CLASS_NAME "dssi_vst"
#define PLUGIN_ENTRY_POINT "main"

static bool inProcessThread = false;
static bool exiting = false;
static HWND hWnd = 0;
static double currentSamplePosition = 0.0;

static bool ready = false;
static int bufferSize = 0;
static int sampleRate = 0;

static int debugLevel = 3;

static AEffect *plugin = 0;
static char *serverurl = 0;

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
	lo_address hostaddr = lo_address_new(lo_url_get_hostname(serverurl),
					     lo_url_get_port(serverurl));
	lo_send(hostaddr,
		(std::string(lo_url_get_path(serverurl)) + "/control").c_str(),
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
	if (debugLevel > 1) {
	    cerr << "dssi-vst_gui[2]: audioMasterGetCurrentProcessLevel requested (level is " << (inProcessThread ? 2 : 1) << ")" << endl;
	}
	// 0 -> unsupported, 1 -> gui, 2 -> process, 3 -> midi/timer, 4 -> offline
	if (inProcessThread) return 2;
	else return 1;

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

DWORD WINAPI
AudioThreadMain(LPVOID parameter)
{
    while (1) {
	    //!!!
//	    if (plugin) plugin->processReplacing(plugin, inputs, outputs, 1024);
//	    remoteVSTServerInstance->dispatch();
	
	//!!! need to at least maintain the pretence of running the thing

	sleep(1); //!!!

	if (exiting) return 0;
    }
}

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

    //!!! wrong thread I think -- queue these

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

    //!!! wrong thread I think -- queue these

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
		if (!serverurl) serverurl = strndup(cmdline + offset, ci - offset);
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

    if (!serverurl || !serverurl[0] ||
	!pluginlibname || !pluginlibname[0] ||
	!label || !label[0] ||
	!friendlyname || !friendlyname[0]) {
	cerr << "Usage: dssi-vst_gui <oscurl> <plugin.so> <label> <friendlyname>" << endl;
	cerr << "(Command line was: " << cmdline << ")" << endl;
	exit(2);
    }

    char *libname = label; // VST libname is in label, DSSI libname in pluginlibname

    cout << "Loading \"" << libname << "\"... ";
    if (debugLevel > 0) cout << endl;

    char libPath[1024];
    HINSTANCE libHandle = 0;
    char *vstDir = getenv("VSTI_DIR");

    if (vstDir) {
	if (vstDir[strlen(vstDir) - 1] == '/') {
	    snprintf(libPath, 1024, "%s%s", vstDir, libname);
	} else {
	    snprintf(libPath, 1024, "%s/%s", vstDir, libname);
	}
	std::string libpathstr(libPath);

	if (home && home[0] != '\0') {
	    if (libpathstr.substr(0, strlen(home)) == std::string(home)) {
		libpathstr = libpathstr.substr(strlen(home) + 1);
	    }
	}

	libHandle = LoadLibrary(libpathstr.c_str());

	libHandle = LoadLibrary(libPath);
	if (debugLevel > 0) {
	    cerr << "dssi-vst_gui[1]: " << (libHandle ? "" : "not ")
		 << "found in " << vstDir << " (from $VSTI_DIR)" << endl;
	}
    } else if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: $VSTI_DIR not set" << endl;
    }

    std::cerr << "home is " << getenv("HOME") << std::endl;

    if (!libHandle) {
	vstDir = getenv("VST_DIR");
	if (vstDir) {
	    snprintf(libPath, 1024, "%s/%s", vstDir, libname);
	    std::string libpathstr(libPath);

	    if (home && home[0] != '\0') {
		if (libpathstr.substr(0, strlen(home)) == std::string(home)) {
		    libpathstr = libpathstr.substr(strlen(home) + 1);
		}
	    }
	    
	    libHandle = LoadLibrary(libpathstr.c_str());

	    if (debugLevel > 0) {
		cerr << "dssi-vst_gui[1]: " << (libHandle ? "" : "not ")
		     << "found in " << libPath << " (from $VST_DIR)" << endl;

		LPVOID lpMsgBuf;
		if (FormatMessage( 
			FORMAT_MESSAGE_ALLOCATE_BUFFER | 
			FORMAT_MESSAGE_FROM_SYSTEM | 
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			GetLastError(),
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
			(LPTSTR) &lpMsgBuf,
			0,
			NULL )) {
		    std::cerr << (const char *)lpMsgBuf << std::endl;
		}

		LocalFree( lpMsgBuf );
	    }
	} else if (debugLevel > 0) {
	    cerr << "dssi-vst_gui[1]: $VST_DIR not set" << endl;
	}
    }

    if (!libHandle) {
	libHandle = LoadLibrary(libname);
	if (debugLevel > 0) {
	    cerr << "dssi-vst_gui[1]: " << (libHandle ? "" : "not ")
		 << "found in DLL path" << endl;
	}
    }

    if (!libHandle) {
	cerr << "dssi-vst_gui: ERROR: Couldn't load VST DLL \"" << libname << "\"" << endl;
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
	cerr << "dssi-vst_gui: ERROR: VST entrypoint \"" << PLUGIN_ENTRY_POINT
	     << "\" not found in DLL \"" << libname << "\"" << endl;
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
	cerr << "dssi-vst_gui: ERROR: Not a VST effect in DLL \"" << libname << "\"" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: plugin is a VST" << endl;
    }

    if (!(plugin->flags & effFlagsHasEditor)) {
	cerr << "dssi-vst_gui: ERROR: Instrument has no GUI (required)" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: synth has a GUI" << endl;
    }

    if (!plugin->flags & effFlagsCanReplacing) {
	cerr << "dssi-vst_gui: ERROR: Instrument does not support processReplacing (required)"
	     << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: synth supports processReplacing" << endl;
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

    lo_server_thread thread = lo_server_thread_new(NULL, osc_error);
    lo_server_thread_add_method(thread, "/dssi/control", "if", control_handler, 0);
    lo_server_thread_add_method(thread, "/dssi/program", "ii", program_handler, 0);
    lo_server_thread_add_method(thread, "/dssi/configure", "ss", configure_handler, 0);
    lo_server_thread_add_method(thread, "/dssi/show", "", show_handler, 0);
    lo_server_thread_add_method(thread, "/dssi/hide", "", hide_handler, 0);
    lo_server_thread_add_method(thread, "/dssi/quit", "", quit_handler, 0);
    lo_server_thread_add_method(thread, NULL, NULL, debug_handler, 0);
    lo_server_thread_start(thread);

    cout << "started lo thread (url is " << lo_server_thread_get_url(thread) << ")" << endl;

    lo_address hostaddr = lo_address_new(lo_url_get_hostname(serverurl),
					 lo_url_get_port(serverurl));
    lo_send(hostaddr,
	    (std::string(lo_url_get_path(serverurl)) + "/update").c_str(),
	    "s",
	    (std::string(lo_server_thread_get_url(thread)) + "dssi").c_str());

    DWORD threadId = 0;
    HANDLE threadHandle = CreateThread(0, 0, AudioThreadMain, 0, 0, &threadId);
    if (!threadHandle) {
	cerr << "Failed to create audio thread!" << endl;
	return 1; //!!!tidy
    } else if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: created audio thread" << endl;
    }

    ready = true;

    MSG msg;
    exiting = false;
    while (GetMessage(&msg, 0, 0, 0)) {
	DispatchMessage(&msg);
	if (exiting) break;
    }

    if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: cleaning up" << endl;
    }

    CloseHandle(threadHandle);
    if (debugLevel > 0) {
	cerr << "dssi-vst_gui[1]: closed audio thread" << endl;
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

