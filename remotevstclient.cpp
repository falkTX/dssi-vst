// -*- c-basic-offset: 4 -*-

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004 Chris Cannam
*/

#include "remotevstclient.h"

#include <sys/un.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <iostream>

#include "rdwrops.h"

RemoteVSTClient::RemoteVSTClient(std::string dllName) :
    RemotePluginClient()
{
    pid_t child;
    std::string arg = dllName + "," + getFileIdentifiers();
    const char *argStr = arg.c_str();

    std::cerr << "RemoteVSTClient: executing wine dssi-vst-server.exe.so " << argStr << std::endl;
    
    if ((child = fork()) < 0) {
	cleanup();
	throw((std::string)"Fork failed");
    } else if (child == 0) { // child process
	if (execlp("wine", "wine", "dssi-vst-server.exe.so", argStr, 0)) {
	    perror("Exec failed");
	    exit(1);
	}
    }

    syncStartup();
}

RemoteVSTClient::~RemoteVSTClient()
{
}

void
RemoteVSTClient::queryPlugins(std::vector<PluginRecord> &plugins)
{
    char tmpFileBase[60];

    sprintf(tmpFileBase, "/tmp/rplugin_qry_XXXXXX");
    if (mkstemp(tmpFileBase) < 0) {
	throw((std::string)"Failed to obtain temporary filename");
    }

    unlink(tmpFileBase);
    if (mkfifo(tmpFileBase, 0666)) { //!!! what permission is correct here?
	perror(tmpFileBase);
	throw((std::string)"Failed to create FIFO");
    }

    pid_t child;
    
    if ((child = fork()) < 0) {
	unlink(tmpFileBase);
	throw((std::string)"Fork failed");
    } else if (child == 0) { // child process
	if (execlp("wine", "wine", "dssi-vst-scanner.exe.so", tmpFileBase, 0)) {
	    perror("Exec failed");
	    exit(1);
	}
    }

    //!!! again, should do this via nonblocking loop

    int fd = -1;
    if ((fd = open(tmpFileBase, O_RDONLY)) < 0) {
	unlink(tmpFileBase);
	throw((std::string)"Failed to open FIFO");
    }

    try {
	unsigned long uniqueId;
	char buffer[64];

	while (1) {

	    try {
		tryRead(fd, &uniqueId, sizeof(unsigned long));
	    } catch (RemotePluginClosedException) {
		// This is acceptable here; it just means we're done
		break;
	    }

	    PluginRecord rec;
	    rec.uniqueId = uniqueId;
	    
	    tryRead(fd, buffer, 64);
	    rec.dllName = buffer;
	    
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
		std::cerr << "Parameter " << i << ": name " << buffer << std::endl;
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
    unlink(tmpFileBase);
}

