/* -*- c-basic-offset: 4 -*- */

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004-2010 Chris Cannam
*/

#ifndef REMOTE_PLUGIN_SERVER_H
#define REMOTE_PLUGIN_SERVER_H

#include "remoteplugin.h"
#include "rdwrops.h"
#include <string>
#include <vector>

class RemotePluginServer
{
public:
    virtual ~RemotePluginServer();
    
    virtual bool         isReady() = 0;

    virtual float        getVersion() { return RemotePluginVersion; }
    virtual std::string  getName() = 0;
    virtual std::string  getMaker() = 0;

    virtual void         setBufferSize(int) = 0;
    virtual void         setSampleRate(int) = 0;

    virtual void         reset() = 0;
    virtual void         terminate() = 0;
    
    virtual int          getInputCount() = 0;
    virtual int          getOutputCount() = 0;

    virtual int          getParameterCount()                  { return 0; }
    virtual std::string  getParameterName(int)                { return ""; }
    virtual void         setParameter(int, float)             { return; }
    virtual float        getParameter(int)                    { return 0.0f; }
    virtual float        getParameterDefault(int)             { return 0.0f; }
    virtual void         getParameters(int p0, int pn, float *v) {
	for (int i = p0; i <= pn; ++i) v[i - p0] = 0.0f;
    }

    virtual int          getProgramCount()                    { return 0; }
    virtual std::string  getProgramName(int)                  { return ""; }
    virtual void         setCurrentProgram(int)               { return; }

    virtual bool         hasMIDIInput()                       { return false; }
    virtual void         sendMIDIData(unsigned char *data,
				      int *frameOffsets,
				      int events)             { return; }

    virtual void         process(float **inputs, float **outputs) = 0;

    virtual void         setDebugLevel(RemotePluginDebugLevel) { return; } 
    virtual bool         warn(std::string) = 0;

    virtual void         showGUI(std::string guiData) { } 
    virtual void         hideGUI() { }

    //Deryabin Andrew: vst chunks support
    virtual std::vector<char> getVSTChunk() = 0;
    virtual bool setVSTChunk(std::vector<char>) = 0;
    //Deryabin Andrew: vst chunks support: end code

    void dispatchControl(int timeout = -1); // may throw RemotePluginClosedException
    void dispatchProcess(int timeout = -1); // may throw RemotePluginClosedException

protected:
    RemotePluginServer(std::string fileIdentifiers);

    void cleanup();

private:
    RemotePluginServer(const RemotePluginServer &); // not provided
    RemotePluginServer &operator=(const RemotePluginServer &); // not provided

    void dispatchControlEvents();
    void dispatchProcessEvents();

    int m_bufferSize;
    int m_numInputs;
    int m_numOutputs;

    int m_controlRequestFd;
    int m_controlResponseFd;
    int m_shmFd;
    int m_shmControlFd;

    char *m_controlRequestFileName;
    char *m_controlResponseFileName;
    char *m_shmFileName;
    char *m_shmControlFileName;

    char *m_shm;
    size_t m_shmSize;
    ShmControl *m_shmControl;

    float **m_inputs;
    float **m_outputs;

    RemotePluginDebugLevel m_debugLevel;

    void sizeShm();
};

#endif
