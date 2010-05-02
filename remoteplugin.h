/* -*- c-basic-offset: 4 -*- */

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004-2010 Chris Cannam
*/

#ifndef REMOTE_PLUGIN_H
#define REMOTE_PLUGIN_H

static const float RemotePluginVersion = 0.986;

enum RemotePluginDebugLevel {
    RemotePluginDebugNone,
    RemotePluginDebugSetup,
    RemotePluginDebugEvents,
    RemotePluginDebugData
};

enum RemotePluginOpcode {

    RemotePluginGetVersion = 0,
    RemotePluginGetName,
    RemotePluginGetMaker,

    RemotePluginSetBufferSize = 100,
    RemotePluginSetSampleRate,
    RemotePluginReset,
    RemotePluginTerminate,

    RemotePluginGetInputCount = 200,
    RemotePluginGetOutputCount,

    RemotePluginGetParameterCount = 300,
    RemotePluginGetParameterName,
    RemotePluginSetParameter,
    RemotePluginGetParameter,
    RemotePluginGetParameterDefault,
    RemotePluginGetParameters,

    RemotePluginGetProgramCount = 350,
    RemotePluginGetProgramName,
    RemotePluginSetCurrentProgram,

    RemotePluginHasMIDIInput = 400,
    RemotePluginSendMIDIData,

    RemotePluginProcess = 500,
    RemotePluginIsReady,

    RemotePluginSetDebugLevel = 600,
    RemotePluginWarn,

    RemotePluginShowGUI = 700,
    RemotePluginHideGUI,

    RemotePluginNoOpcode = 9999

};

class RemotePluginClosedException { };

#endif
