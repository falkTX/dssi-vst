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
#include <dirent.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "aeffectx.h"
#include "AEffEditor.hpp"

#include "remotepluginserver.h"

#define APPLICATION_CLASS_NAME "dssi_vst"
#define PLUGIN_ENTRY_POINT "main"

using namespace std;


long VSTCALLBACK
hostCallback(AEffect *plugin, long opcode, long index,
	     long value, void *ptr, float opt)
{
    static VstTimeInfo timeInfo;

    switch (opcode) {

    case audioMasterVersion:
	return 2300;

    case audioMasterGetVendorString:
	strcpy((char *)ptr, "Chris Cannam");
	break;

    case audioMasterGetProductString:
	strcpy((char *)ptr, "DSSI VST Wrapper Plugin Scanner");
	break;

    case audioMasterGetVendorVersion:
	return long(RemotePluginVersion * 100);

    case audioMasterGetLanguage:
	return kVstLangEnglish;

    case audioMasterCanDo:
	if (!strcmp((char*)ptr, "sendVstEvents") ||
	    !strcmp((char*)ptr, "sendVstMidiEvent") ||
	    !strcmp((char*)ptr, "sendVstTimeInfo") ||
	    !strcmp((char*)ptr, "sizeWindow") ||
	    !strcmp((char*)ptr, "supplyIdle")) {
	    return 1;
	}
	break;

    case audioMasterGetTime:
	timeInfo.samplePos = 0;
	timeInfo.sampleRate = 48000;
	timeInfo.flags = 0; // don't mark anything valid except default samplePos/Rate
	return (long)&timeInfo;

    case audioMasterTempoAt:
	// can't support this, return 120bpm
	return 120 * 10000;

    case audioMasterGetSampleRate:
	plugin->dispatcher(plugin, effSetSampleRate,
			   0, 0, NULL, 48000.0);
	break;

    case audioMasterGetBlockSize:
	plugin->dispatcher(plugin, effSetBlockSize,
			   0, 1024, NULL, 0);
	break;

    case audioMasterWillReplaceOrAccumulate:
	// 0 -> unsupported, 1 -> replace, 2 -> accumulate
	return 1;

    case audioMasterGetCurrentProcessLevel:
	// 0 -> unsupported, 1 -> gui, 2 -> process, 3 -> midi/timer, 4 -> offline
	return 1;

    case audioMasterGetParameterQuantization:
	return 1;

    case audioMasterNeedIdle:
	// might be nice to handle this better
	return 1;

    case audioMasterWantMidi:
	// happy to oblige
	return 1;

    default:
	;
    }
    
    return 0;
};

int WINAPI
WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR cmdline, int cmdshow)
{
    char *destFile = 0;

    cout << "DSSI VST plugin scanner v0.2" << endl;
    cout << "Copyright (c) 2004 Chris Cannam" << endl;

    if (cmdline && cmdline[0]) destFile = strdup(cmdline);
    
    int targetfd = 0;
    if (destFile) {
	if ((targetfd = open(destFile, O_WRONLY)) < 0) {
	    cerr << "dssi-vst-scanner: Failed to open output file " << destFile;
	    perror(" ");
	    cerr << "dssi-vst-scanner: Defaulting to stdout" << endl;
	    targetfd = 0;
	}
    }

    //!!! could do with an option for vst/vsti path, for the moment
    // we'll deal only with effects

    char libPath[1024];
    HINSTANCE libHandle = 0;
    char *vstDir = getenv("VST_DIR");

    if (!vstDir) {
	cerr << "dssi-vst-scanner: $VST_DIR not set" << endl;
	exit(1);
    }

    DIR *directory = opendir(vstDir);
    if (!directory) {
	cerr << "dssi-vst-scanner: couldn't read VST directory \""
		  << vstDir << "\" (from $VST_DIR)" << std::endl;
	if (targetfd != 0) close(targetfd);
	return 1;
    }

    struct dirent *entry;
    int count = 0;

    char *home = getenv("HOME");
    std::string cacheDir = std::string(home) + "/.dssi-vst";
    bool haveCacheDir = false;

    DIR *test = opendir(cacheDir.c_str());
    if (!test) {
	if (mkdir(cacheDir.c_str(), 0755)) {
	    cerr << "dssi-vst-scanner: failed to create cache directory " << cacheDir;
	    perror(0);
	} else {
	    haveCacheDir = true;
	}
    } else {
	haveCacheDir = true;
	closedir(test);
    }

    while ((entry = readdir(directory))) {

	// For each plugin, we write:
	//
	// unique id (unsigned long)
	// dll name (64 chars)
	// name (64 chars)
	// vendor (64 chars)
	// is synth (bool)
	// have editor (bool)
	// input count (int)
	// output count (int)
	// 
	// parameter count (int)
	// then for each parameter:
	// name (64 chars)
	//
	// program count (int)
	// then for each program:
	// name (64 chars)

	std::string libname = entry->d_name;

	if (libname[0] == '.' ||
	    libname.length() < 5 ||
	    (libname.substr(libname.length() - 4) != ".dll" &&
	     libname.substr(libname.length() - 4) != ".DLL")) {
	    continue;
	}

	if (vstDir[strlen(vstDir) - 1] == '/') {
	    snprintf(libPath, 1024, "%s%s", vstDir, libname.c_str());
	} else {
	    snprintf(libPath, 1024, "%s/%s", vstDir, libname.c_str());
	}

	std::string libpathstr(libPath);

	if (home && home[0] != '\0') {
	    if (libpathstr.substr(0, strlen(home)) == std::string(home)) {
		libpathstr = libpathstr.substr(strlen(home) + 1);
	    }
	}

	int fd = targetfd;
	bool haveCache = false;
	bool writingCache = false;
	std::string cacheFileName = cacheDir + "/" + libname + ".cache";

	if (haveCacheDir) {
	
	    struct stat st;
	    if (!stat(cacheFileName.c_str(), &st)) {
		haveCache = true;
	    } else {
		if ((fd = open(cacheFileName.c_str(), O_WRONLY | O_CREAT, 0644)) < 0) {
		    cerr << "dssi-vst-scanner: Failed to open cache file " << cacheFileName;
		    perror("for writing");
		    fd = targetfd;
		} else {
		    writingCache = true;
		}
	    }
	}

	if (!haveCache) {

	    int inputs = 0, outputs = 0, params = 0, programs = 0;
	    char buffer[65];
	    unsigned long uniqueId = 0;
	    bool synth = false, gui = false;
	    int i = 0;
	    AEffect *(__stdcall* getInstance)(audioMasterCallback) = 0;
	    AEffect *plugin = 0;

	    libHandle = LoadLibrary(libpathstr.c_str());

	    if (!libHandle) {
		cerr << "dssi-vst-scanner: Couldn't load DLL " << libpathstr << endl;
		goto done;
	    }

	    getInstance = (AEffect*(__stdcall*)(audioMasterCallback))
		GetProcAddress(libHandle, PLUGIN_ENTRY_POINT);

	    if (!getInstance) {
		cerr << "dssi-vst-scanner: VST entrypoint \"" << PLUGIN_ENTRY_POINT
		     << "\" not found in DLL \"" << libpathstr << "\"" << endl;
		goto done;
	    }

	    plugin = getInstance(hostCallback);

	    if (!plugin) {
		cerr << "dssi-vst-scanner: Failed to instantiate plugin in VST DLL \""
		     << libpathstr << "\"" << endl;
		goto done;
	    }

	    if (plugin->magic != kEffectMagic) {
		cerr << "dssi-vst-scanner: Not a VST effect in DLL \""
		     << libpathstr << "\"" << endl;
		goto done;
	    }

	    if (!plugin->flags & effFlagsCanReplacing) {
		cerr << "dssi-vst-scanner: Effect does not support processReplacing (required)"
		     << endl;
		goto done;
	    }

	    uniqueId = 6666 + count;
	    write(fd, &uniqueId, sizeof(unsigned long));

	    memset(buffer, 0, 65);
	    snprintf(buffer, 64, "%s", libname.c_str());
	    write(fd, buffer, 64);

	    memset(buffer, 0, 65);
	    plugin->dispatcher(plugin, effGetEffectName, 0, 0, buffer, 0);
	    if (buffer[0] == '\0') {
		snprintf(buffer, 64, "%s", libname.c_str());
	    }
	    write(fd, buffer, 64);

	    memset(buffer, 0, 65);
	    plugin->dispatcher(plugin, effGetVendorString, 0, 0, buffer, 0);
	    write(fd, buffer, 64);

	    synth = false;
	    if (plugin->flags & effFlagsIsSynth) synth = true;
	    write(fd, &synth, sizeof(bool));

	    gui = false;
	    if (plugin->flags & effFlagsHasEditor) gui = true;
	    write(fd, &gui, sizeof(bool));

	    inputs = plugin->numInputs;
	    write(fd, &inputs, sizeof(int));

	    outputs = plugin->numOutputs;
	    write(fd, &outputs, sizeof(int));

	    params = plugin->numParams;
	    write(fd, &params, sizeof(int));

	    for (i = 0; i < params; ++i) {
		memset(buffer, 0, 65);
		plugin->dispatcher(plugin, effGetParamName, i, 0, buffer, 0);
		write(fd, buffer, 64);
	    }

	    programs = plugin->numPrograms;
	    write(fd, &programs, sizeof(int));

	    for (i = 0; i < programs; ++i) {
		memset(buffer, 0, 65);
		// effGetProgramName appears to return the name of the
		// current program, not program <index> -- though we
		// pass in <index> as well, just in case
		plugin->dispatcher(plugin, effSetProgram, 0, i, NULL, 0);
		plugin->dispatcher(plugin, effGetProgramName, i, 0, buffer, 0);
		write(fd, buffer, 64);
	    }

	done:
	    if (plugin) plugin->dispatcher(plugin, effClose, 0, 0, NULL, 0);
	    FreeLibrary(libHandle);
	}

	if (writingCache) {
	    close(fd);
	}

	if (haveCache || writingCache) {
	    // need to read from cache as well
	    if ((fd = open(cacheFileName.c_str(), O_RDONLY)) < 0) {
		cerr << "dssi-vst-scanner: Failed to open cache file " << cacheFileName;
		perror("for reading");
	    } else {
		unsigned char c;
		while (read(fd, &c, 1) == 1) {
		    write(targetfd, &c, 1);
		}
		close(fd);
	    }
	}

	++count;
    }

    closedir(directory);
    if (targetfd != 0) {
	close(targetfd);
    }

    return 0;
}

