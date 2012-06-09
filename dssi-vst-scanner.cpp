// -*- c-basic-offset: 4 -*-

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004-2010 Chris Cannam
*/

#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define VST_FORCE_DEPRECATED 0
#include "aeffectx.h"

#include "remotepluginserver.h"
#include "paths.h"

#define APPLICATION_CLASS_NAME "dssi_vst"
#define OLD_PLUGIN_ENTRY_POINT "main"
#define NEW_PLUGIN_ENTRY_POINT "VSTPluginMain"

#if VST_FORCE_DEPRECATED
#define DEPRECATED_VST_SYMBOL(x) __##x##Deprecated
#else
#define DEPRECATED_VST_SYMBOL(x) x
#endif

using namespace std;


#if 1 // vestige header
#define kVstVersion 2400
struct VstTimeInfo_R {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};
intptr_t
hostCallback(AEffect *plugin, int32_t opcode, int32_t index,
             intptr_t value, void *ptr, float opt)
#elif VST_2_4_EXTENSIONS
typedef VstTimeInfo VstTimeInfo_R;
VstIntPtr VSTCALLBACK
hostCallback(AEffect *plugin, VstInt32 opcode, VstInt32 index,
	     VstIntPtr value, void *ptr, float opt)
#else
typedef VstTimeInfo VstTimeInfo_R;
long VSTCALLBACK
hostCallback(AEffect *plugin, long opcode, long index,
	     long value, void *ptr, float opt)
#endif
{
    static VstTimeInfo_R timeInfo;

    switch (opcode) {

    case audioMasterAutomate:
        if (plugin)
            plugin->setParameter(plugin, index, opt);
        break;

    case audioMasterVersion:
	return kVstVersion;

    case audioMasterIdle:
        if (plugin)
            plugin->dispatcher(plugin, effEditIdle, 0, 0, 0, 0.0f);
        break;

    case audioMasterGetVendorString:
	strcpy((char *)ptr, "Chris Cannam");
	break;

    case audioMasterGetProductString:
	strcpy((char *)ptr, "DSSI-VST Scanner");
	break;

    case audioMasterGetVendorVersion:
	return intptr_t(RemotePluginVersion * 100);

    case audioMasterGetLanguage:
	return kVstLangEnglish;

    case audioMasterCanDo:
	if (!strcmp((char*)ptr, "sendVstEvents") ||
	    !strcmp((char*)ptr, "sendVstMidiEvent") ||
	    !strcmp((char*)ptr, "sendVstTimeInfo") ||
	    !strcmp((char*)ptr, "sizeWindow") ||
	    !strcmp((char*)ptr, "supplyIdle") ||
            !strcmp((char*)ptr, "receiveVstEvents") ||
            !strcmp((char*)ptr, "receiveVstMidiEvent")) {
	    return 1;
	}
	break;

    case audioMasterGetTime:
        memset(&timeInfo, 0, sizeof(VstTimeInfo_R));
	timeInfo.samplePos = 0;
	timeInfo.sampleRate = 48000;
	timeInfo.flags = 0; // don't mark anything valid except default samplePos/Rate
	return (intptr_t)&timeInfo;

    case DEPRECATED_VST_SYMBOL(audioMasterTempoAt):
	// can't support this, return 120bpm
	return 120 * 10000;

    case audioMasterGetSampleRate:
	return 48000; // fake value
	break;

    case audioMasterGetBlockSize:
	return 1024; // fake value
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterWillReplaceOrAccumulate):
	// 0 -> unsupported, 1 -> replace, 2 -> accumulate
	return 1;

    case audioMasterGetCurrentProcessLevel:
	// 0 -> unsupported, 1 -> gui, 2 -> process, 3 -> midi/timer, 4 -> offline
	return 1;

    case DEPRECATED_VST_SYMBOL(audioMasterGetParameterQuantization):
	return 1;
	
    case DEPRECATED_VST_SYMBOL(audioMasterNeedIdle):
	return 1;

    case DEPRECATED_VST_SYMBOL(audioMasterWantMidi):
	return 1;

    default:
	break;
    }

    return 0;
};

int WINAPI
WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR cmdline, int cmdshow)
{
    char *destFile = 0;

    cout << "DSSI VST plugin scanner v0.3" << endl;
    cout << "Copyright (c) 2004-2010 Chris Cannam" << endl;

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

    int version = int(RemotePluginVersion * 1000);
    write(targetfd, &version, sizeof(int));

    HINSTANCE libHandle = 0;

    std::vector<std::string> vstPath = Paths::getPath
	("VST_PATH", "/usr/local/lib/vst:/usr/lib/vst", "/vst");

    for (size_t i = 0; i < vstPath.size(); ++i) {
	
	std::string vstDir = vstPath[i];

	DIR *directory = opendir(vstDir.c_str());
	if (!directory) {
//	    cerr << "dssi-vst-scanner: couldn't read VST directory \""
//		 << vstDir << "\"" << std::endl;
	    continue;
	}

	struct dirent *entry;
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
	    // default value (float)
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

	    int fd = targetfd;
	    bool haveCache = false;
	    bool writingCache = false;
	    std::string cacheFileName = cacheDir + "/" + libname + ".cache";

	    if (haveCacheDir) {
	
		struct stat st;
		if (!stat(cacheFileName.c_str(), &st)) {
		    int testfd = open(cacheFileName.c_str(), O_RDONLY);
		    if (testfd >= 0) {
			int testVersion = 0;
			if (read(testfd, &testVersion, sizeof(int)) == sizeof(int) &&
			    testVersion == version) {
			    haveCache = true;
			} else {
			    cerr << "dssi-vst-scanner: Cache version mismatch for file "
				 << cacheFileName << " (" << testVersion << ", wanted "
				 << version << ") - rewriting" << endl;
			}
			close(testfd);
		    }
		}
		if (!haveCache) {
		    if ((fd = open(cacheFileName.c_str(), O_WRONLY | O_CREAT, 0644)) < 0) {
			cerr << "dssi-vst-scanner: Failed to open cache file " << cacheFileName;
			perror(" for writing");
			fd = targetfd;
		    } else {
			writingCache = true;
			write(fd, &version, sizeof(int));
		    }
		}
	    }

	    if (!haveCache) {

		int inputs = 0, outputs = 0, params = 0, programs = 0;
		char buffer[65];
		bool synth = false, gui = false;
		int i = 0;
		AEffect *(__stdcall* getInstance)(audioMasterCallback) = 0;
		AEffect *plugin = 0;
		std::string libPath;

		if (vstDir[vstDir.length()-1] == '/') {
		    libPath = vstDir + libname;
		} else {
		    libPath = vstDir + "/" + libname;
		}
		
		libHandle = LoadLibrary(libPath.c_str());
		cerr << "dssi-vst-scanner: " << (libHandle ? "" : "not ")
		     << "found in " << libPath << endl;
		
		if (!libHandle) {
		    if (home && home[0] != '\0') {
			if (libPath.substr(0, strlen(home)) == home) {
			    libPath = libPath.substr(strlen(home) + 1);
			}
			libHandle = LoadLibrary(libPath.c_str());
			cerr << "dssi-vst-scanner: " << (libHandle ? "" : "not ")
			     << "found in " << libPath << endl;
		    }
		}
		
		if (!libHandle) {
		    cerr << "dssi-vst-scanner: Couldn't load DLL " << libPath << endl;
		    goto done;
		}

		getInstance = (AEffect*(__stdcall*)(audioMasterCallback))
		    GetProcAddress(libHandle, NEW_PLUGIN_ENTRY_POINT);

		if (!getInstance) {
		    getInstance = (AEffect*(__stdcall*)(audioMasterCallback))
			GetProcAddress(libHandle, OLD_PLUGIN_ENTRY_POINT);

		    if (!getInstance) {
			cerr << "dssi-vst-scanner: VST entrypoints \""
			     << NEW_PLUGIN_ENTRY_POINT << "\" or \"" 
			     << OLD_PLUGIN_ENTRY_POINT << "\" not found in DLL \""
			     << libname << "\"" << endl;
			goto done;
		    }
		}

		plugin = getInstance(hostCallback);

		if (!plugin) {
		    cerr << "dssi-vst-scanner: Failed to instantiate plugin in VST DLL \""
			 << libPath << "\"" << endl;
		    goto done;
		}

		if (plugin->magic != kEffectMagic) {
		    cerr << "dssi-vst-scanner: Not a VST effect in DLL \""
			 << libPath << "\"" << endl;
		    goto done;
		}

		if (!plugin->flags & effFlagsCanReplacing) {
		    cerr << "dssi-vst-scanner: Effect does not support processReplacing (required)"
			 << endl;
		    goto done;
		}

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
		    float f = plugin->getParameter(plugin, i);
		    write(fd, &f, sizeof(float));
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
		    int testVersion = 0;
		    if (read(fd, &testVersion, sizeof(int)) != sizeof(int) ||
			testVersion != version) {
			cerr << "dssi-vst-scanner: Internal error: cache file " << cacheFileName << " verified earlier, but now fails version test (" << testVersion << " != " << version << ")" << endl;
		    } else {
			unsigned char c;
			while (read(fd, &c, 1) == 1) {
			    write(targetfd, &c, 1);
			}
		    }
		    close(fd);
		}
	    }
	}

	closedir(directory);
    }

    if (targetfd != 0) {
	close(targetfd);
    }
    
    return 0;
}
