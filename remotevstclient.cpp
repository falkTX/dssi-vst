// -*- c-basic-offset: 4 -*-

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004 Chris Cannam
*/

#include "remotevstclient.h"

#include <sys/un.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <iostream>
#include <dirent.h>
#include <unistd.h>

#include "rdwrops.h"
#include "paths.h"

RemoteVSTClient::RemoteVSTClient(std::string dllName) :
    RemotePluginClient()
{
    pid_t child;
    std::string arg = dllName + "," + getFileIdentifiers();
    const char *argStr = arg.c_str();

    // We want to run the dssi-vst-server script, which runs wine
    // dssi-vst-server.exe.so.  We expect to find this script in the
    // same subdirectory of a directory in the DSSI_PATH as a host
    // would look for the GUI for this plugin: one called dssi-vst.
    // See also RemoteVSTClient::queryPlugins below.

    std::vector<std::string> dssiPath = Paths::getPath
	("DSSI_PATH", "/usr/local/lib/dssi:/usr/lib/dssi", "/.dssi");

    bool found = false;

    for (size_t i = 0; i < dssiPath.size(); ++i) {

	std::string subDir = dssiPath[i] + "/dssi-vst";

	DIR *directory = opendir(subDir.c_str());
	if (!directory) {
	    continue;
	}
	closedir(directory);

	struct stat st;
	std::string fileName = subDir + "/dssi-vst-server";

	if (stat(fileName.c_str(), &st)) {
	    continue;
	}

	if (!(S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) ||
	    !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {

	    std::cerr << "RemoteVSTClient: file " << fileName
		      << " exists but can't be executed" << std::endl;
	    continue;
	}

	found = true;

	std::cerr << "RemoteVSTClient: executing "
		  << fileName << " " << argStr << std::endl;

	if ((child = fork()) < 0) {
	    cleanup();
	    throw((std::string)"Fork failed");
	} else if (child == 0) { // child process
	    if (execlp(fileName.c_str(), fileName.c_str(), argStr, 0)) {
		perror("Exec failed");
		exit(1);
	    }
	}
    }

    if (!found) {
	cleanup();
	throw((std::string)"Failed to find dssi-vst-server executable");
    } else {
	syncStartup();
    }
}

RemoteVSTClient::~RemoteVSTClient()
{
}

void
RemoteVSTClient::queryPlugins(std::vector<PluginRecord> &plugins)
{
    // First check whether there are any DLLs in the same VST path as
    // the scanner uses.  If not, we know immediately there are no
    // plugins and we don't need to run the (Wine-based) scanner.
    
    std::vector<std::string> vstPath = Paths::getPath
	("VST_PATH", "/usr/local/lib/vst:/usr/lib/vst", "/vst");

    bool haveDll = false;

    for (size_t i = 0; i < vstPath.size(); ++i) {
	
	std::string vstDir = vstPath[i];
	DIR *directory = opendir(vstDir.c_str());
	if (!directory) continue;
	struct dirent *entry;

	while ((entry = readdir(directory))) {
	    
	    std::string libname = entry->d_name;

	    if (libname[0] != '.' &&
		libname.length() >= 5 &&
		(libname.substr(libname.length() - 4) == ".dll" ||
		 libname.substr(libname.length() - 4) == ".DLL")) {
		haveDll = true;
		break;
	    }
	}

	closedir(directory);
	if (haveDll) break;
    }

    if (!haveDll) return;

    char fifoFile[60];

    sprintf(fifoFile, "/tmp/rplugin_qry_XXXXXX");
    if (mkstemp(fifoFile) < 0) {
	throw((std::string)"Failed to obtain temporary filename");
    }

    unlink(fifoFile);
    if (mkfifo(fifoFile, 0666)) { //!!! what permission is correct here?
	perror(fifoFile);
	throw((std::string)"Failed to create FIFO");
    }

    // We open the fd nonblocking, then start the scanner, then wait
    // to see whether the scanner starts sending anything on it.  If
    // no input is available after a certain time, give up.

    int fd = -1;

    if ((fd = open(fifoFile, O_RDONLY | O_NONBLOCK)) < 0) {
	unlink(fifoFile);
	throw((std::string)"Failed to open FIFO");
    }

    // We want to run the dssi-vst-scanner script, which runs wine
    // dssi-vst-scanner.exe.so.  We expect to find this script in the
    // same subdirectory of a directory in the DSSI_PATH as a host
    // would look for the GUI for this plugin: one called dssi-vst.
    // See also the RemoteVSTClient constructor above.

    std::vector<std::string> dssiPath = Paths::getPath
	("DSSI_PATH", "/usr/local/lib/dssi:/usr/lib/dssi", "/.dssi");

    bool found = false;
    pid_t child;

    for (size_t i = 0; i < dssiPath.size(); ++i) {

	std::string subDir = dssiPath[i] + "/dssi-vst";

	DIR *directory = opendir(subDir.c_str());
	if (!directory) {
	    continue;
	}
	closedir(directory);

	struct stat st;
	std::string fileName = subDir + "/dssi-vst-scanner";

	if (stat(fileName.c_str(), &st)) {
	    continue;
	}

	if (!(S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) ||
	    !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {

	    std::cerr << "RemoteVSTClient: file " << fileName
		      << " exists but can't be executed" << std::endl;
	    continue;
	}

	found = true;

	std::cerr << "RemoteVSTClient: executing "
		  << fileName << " " << fifoFile << std::endl;

	if ((child = fork()) < 0) {
	    unlink(fifoFile);
	    throw((std::string)"Fork failed");
	} else if (child == 0) { // child process
	    if (execlp(fileName.c_str(), fileName.c_str(), fifoFile, 0)) {
		perror("Exec failed");
		unlink(fifoFile);
		exit(1);
	    }
	}
    }

    if (!found) {
	unlink(fifoFile);
	throw((std::string)"Failed to find dssi-vst-scanner executable");
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    int sec;

    for (sec = 0; sec < 6; ++sec) {

	int rv = poll(&pfd, 1, 1000);

	if (rv < 0) {
	    if (errno == EINTR || errno == EAGAIN) {
		// try again
		sleep(1);
		continue;
	    } else {
		close(fd);
		unlink(fifoFile);
		throw ((std::string)"Plugin scanner startup failed.");
	    }
	} else if (rv > 0) {
	    break;
	}
    }

    if (sec >= 6) {
	close(fd);
	unlink(fifoFile);
	throw ((std::string)"Plugin scanner timed out on startup.");
    }

    try {
	char buffer[64];
	int version = 0;

	tryRead(fd, &version, sizeof(int));
	if (version != int(RemotePluginVersion * 1000)) {
	    throw ((std::string)"Plugin scanner version mismatch");
	}

	while (1) {

	    PluginRecord rec;
	    
	    try {
		tryRead(fd, buffer, 64);
		rec.dllName = buffer;
	    } catch (RemotePluginClosedException) {
		// This is acceptable here; it just means we're done
		break;
	    }

	    tryRead(fd, buffer, 64);
	    rec.pluginName = buffer;
	    
	    std::cerr << "Plugin " << rec.pluginName << std::endl;
	    
	    tryRead(fd, buffer, 64);
	    rec.vendorName = buffer;
	    
	    tryRead(fd, &rec.isSynth, sizeof(bool));
	    tryRead(fd, &rec.hasGUI, sizeof(bool));
	    tryRead(fd, &rec.inputs, sizeof(int));
	    tryRead(fd, &rec.outputs, sizeof(int));
	    tryRead(fd, &rec.parameters, sizeof(int));
	    
	    std::cerr << rec.parameters << " parameters" << std::endl;
	    
	    for (int i = 0; i < rec.parameters; ++i) {
		tryRead(fd, buffer, 64);
		rec.parameterNames.push_back(std::string(buffer));
		float f;
		tryRead(fd, &f, sizeof(float));
		rec.parameterDefaults.push_back(f);
		std::cerr << "Parameter " << i << ": name " << buffer << ", default " << f << std::endl;
	    }
	    
	    tryRead(fd, &rec.programs, sizeof(int));
	    
	    std::cerr << rec.programs << " programs" << std::endl;
	    
	    for (int i = 0; i < rec.programs; ++i) {
		tryRead(fd, buffer, 64);
		rec.programNames.push_back(std::string(buffer));
		std::cerr << "Program " << i << ": name " << buffer << std::endl;
	    }	    
	    
	    plugins.push_back(rec);
	}
    } catch (std::string s) {
	std::cerr << s << std::endl;
    } catch (RemotePluginClosedException) {
	std::cerr << "Plugin scanner exited unexpectedly." << std::endl;
    }

    close(fd);
    unlink(fifoFile);
}

