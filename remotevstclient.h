/* -*- c-basic-offset: 4 -*- */

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004-2010 Chris Cannam
*/

#ifndef REMOTE_VST_CLIENT_H
#define REMOTE_VST_CLIENT_H

#include "remotepluginclient.h"

class RemoteVSTClient : public RemotePluginClient
{
public:
    // may throw a string exception
    RemoteVSTClient(std::string dllName, bool showGUI = false);

    virtual ~RemoteVSTClient();

    // lightweight bulk query mechanism

    struct PluginRecord {
	std::string dllName;
	std::string pluginName;
	std::string vendorName;
	bool isSynth;
	bool hasGUI;
	int inputs;
	int outputs;
	int parameters;
	std::vector<std::string> parameterNames;
	std::vector<float> parameterDefaults;
	int programs;
	std::vector<std::string> programNames;
    };

    static void queryPlugins(std::vector<PluginRecord> &plugins);

protected:
    static bool addFromFd(int fd, PluginRecord &rec);

private:
    RemoteVSTClient(const RemoteVSTClient &); // not provided
    RemoteVSTClient &operator=(const RemoteVSTClient &); // not provided
};    

#endif
