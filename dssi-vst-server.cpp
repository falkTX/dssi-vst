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

#include <stdlib.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/poll.h>
#include <time.h>

#include <unistd.h>
#include <sched.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <jack/jack.h>

#define VST_FORCE_DEPRECATED 0
#include "aeffectx.h"

#include "remotepluginserver.h"

#include "paths.h"
#include "rdwrops.h"

#define APPLICATION_CLASS_NAME "dssi_vst"
#define OLD_PLUGIN_ENTRY_POINT "main"
#define NEW_PLUGIN_ENTRY_POINT "VSTPluginMain"

#if VST_FORCE_DEPRECATED
#define DEPRECATED_VST_SYMBOL(x) __##x##Deprecated
#else
#define DEPRECATED_VST_SYMBOL(x) x
#endif

#define effGetProgramNameIndexed 29

struct Rect {
    short top;
    short left;
    short bottom;
    short right;
};

static bool inProcessThread = false;
static HANDLE audioThreadHandle = 0;
static bool exiting = false;
static HWND hWnd = 0;
static double currentSamplePosition = 0.0;

static bool ready = false;
static bool alive = false;
static int bufferSize = 0;
static int sampleRate = 0;
static bool guiVisible = false;
static bool needIdle = false;

static RemotePluginDebugLevel debugLevel = RemotePluginDebugNone;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static jack_client_t* jack_client = 0;
static int jack_process_callback(jack_nframes_t, void*)
{ return 0; }

using namespace std;

class RemoteVSTServer : public RemotePluginServer
{
public:
    RemoteVSTServer(std::string fileIdentifiers, AEffect *plugin, std::string fallbackName);
    virtual ~RemoteVSTServer();
 
    virtual bool         isReady() { return ready; }
   
    virtual std::string  getName() { return m_name; }
    virtual std::string  getMaker() { return m_maker; }
    virtual void         setBufferSize(int);
    virtual void         setSampleRate(int);
    virtual void         reset();
    virtual void         terminate();
    
    virtual int          getInputCount() { return m_plugin->numInputs; }
    virtual int          getOutputCount() { return m_plugin->numOutputs; }

    virtual int          getParameterCount() { return m_plugin->numParams; }
    virtual std::string  getParameterName(int);
    virtual void         setParameter(int, float);
    virtual float        getParameter(int);
    virtual float        getParameterDefault(int);
    virtual void         getParameters(int, int, float *);

    virtual int          getProgramCount() { return m_plugin->numPrograms; }
    virtual std::string  getProgramName(int);
    virtual void         setCurrentProgram(int);

    virtual bool         hasMIDIInput() { return m_hasMIDI; }
    virtual void         sendMIDIData(unsigned char *data,
				      int *frameOffsets,
				      int events);

    virtual void         showGUI(std::string);
    virtual void         hideGUI();

    //Deryabin Andrew: vst chunks support
    virtual std::vector<char> getVSTChunk();
    virtual bool setVSTChunk(std::vector<char>);
    //Deryabin Andrew: vst chunks support: end code

    virtual void process(float **inputs, float **outputs);

    virtual void setDebugLevel(RemotePluginDebugLevel level) {
	debugLevel = level;
    }

    virtual bool warn(std::string);

    void startEdit();
    void endEdit();
    void monitorEdits();
    void scheduleGUINotify(int index, float value);
    void notifyGUI(int index, float value);
    void checkGUIExited();
    void terminateGUIProcess();

private:
    AEffect *m_plugin;

    std::string m_name;
    std::string m_maker;

    // These should be referred to from the GUI thread only
    std::string m_guiFifoFile;
    int m_guiFifoFd;
    int m_guiEventsExpected;
    struct timeval m_lastGuiComms;

    // To be written by the audio thread and read by the GUI thread
#define PARAMETER_CHANGE_COUNT 200
    int m_paramChangeIndices[PARAMETER_CHANGE_COUNT];
    float m_paramChangeValues[PARAMETER_CHANGE_COUNT];
    int m_paramChangeReadIndex;
    int m_paramChangeWriteIndex;

    enum {
	EditNone,
	EditStarted,
	EditFinished
    } m_editLevel;
    
    float *m_defaults;
    float *m_values;
    bool m_hasMIDI;
};

static RemoteVSTServer *remoteVSTServerInstance = 0;

RemoteVSTServer::RemoteVSTServer(std::string fileIdentifiers,
				 AEffect *plugin, std::string fallbackName) :
    RemotePluginServer(fileIdentifiers),
    m_plugin(plugin),
    m_name(fallbackName),
    m_maker(""),
    m_guiFifoFile(""),
    m_guiFifoFd(-1),
    m_guiEventsExpected(0),
    m_paramChangeReadIndex(0),
    m_paramChangeWriteIndex(0),
    m_editLevel(EditNone)
{
    pthread_mutex_lock(&mutex);

    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: opening plugin" << endl;
    }

    m_plugin->dispatcher(m_plugin, effOpen, 0, 0, NULL, 0);
    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 0, NULL, 0);

    m_hasMIDI = false;

    if (m_plugin->dispatcher(m_plugin, effGetVstVersion, 0, 0, NULL, 0) < 2) {
	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: plugin is VST 1.x" << endl;
	}
    } else {
	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: plugin is VST 2.0 or newer" << endl;
	}
	if ((m_plugin->flags & effFlagsIsSynth)) {
	    if (debugLevel > 0) {
		cerr << "dssi-vst-server[1]: plugin is a synth" << endl;
	    }
	    m_hasMIDI = true;
	} else {
	    if (debugLevel > 0) {
		cerr << "dssi-vst-server[1]: plugin is not a synth" << endl;
	    }
	    if (m_plugin->dispatcher(m_plugin, effCanDo, 0, 0, (void *)"receiveVstMidiEvent", 0) > 0) {
		if (debugLevel > 0) {
		    cerr << "dssi-vst-server[1]: plugin can receive MIDI anyway" << endl;
		}
		m_hasMIDI = true;
	    }
	}
    }

    char buffer[65];
    buffer[0] = '\0';
    m_plugin->dispatcher(m_plugin, effGetEffectName, 0, 0, buffer, 0);
    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: plugin name is \"" << buffer
	     << "\"" << endl;
    }
    if (buffer[0]) m_name = buffer;

    buffer[0] = '\0';
    m_plugin->dispatcher(m_plugin, effGetVendorString, 0, 0, buffer, 0);
    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: vendor string is \"" << buffer
	     << "\"" << endl;
    }
    if (buffer[0]) m_maker = buffer;

    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 1, NULL, 0);

    m_defaults = new float[m_plugin->numParams];
    m_values = new float[m_plugin->numParams];
    for (int i = 0; i < m_plugin->numParams; ++i) {
	m_defaults[i] = m_plugin->getParameter(m_plugin, i);
	m_values[i] = m_defaults[i];
    }

    pthread_mutex_unlock(&mutex);
}

RemoteVSTServer::~RemoteVSTServer()
{
    pthread_mutex_lock(&mutex);

    if (m_guiFifoFd >= 0) {
	try {
	    writeOpcode(m_guiFifoFd, RemotePluginTerminate);
	} catch (...) { }
	close(m_guiFifoFd);
    }

    if (guiVisible) {
	ShowWindow(hWnd, SW_HIDE);
	UpdateWindow(hWnd);
	m_plugin->dispatcher(m_plugin, effEditClose, 0, 0, 0, 0);
	guiVisible = false;
    }

    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 0, NULL, 0);
    m_plugin->dispatcher(m_plugin, effClose, 0, 0, NULL, 0);
    delete[] m_defaults;

    pthread_mutex_unlock(&mutex);
}

void
RemoteVSTServer::process(float **inputs, float **outputs)
{
    if (pthread_mutex_trylock(&mutex)) {
	for (int i = 0; i < m_plugin->numOutputs; ++i) {
	    memset(outputs[i], 0, bufferSize * sizeof(float));
	}
	currentSamplePosition += bufferSize;
	return;
    }
    
    inProcessThread = true;
    
    // superclass guarantees setBufferSize will be called before this
    m_plugin->processReplacing(m_plugin, inputs, outputs, bufferSize);
    currentSamplePosition += bufferSize;
    
    inProcessThread = false;
    pthread_mutex_unlock(&mutex);
}

void
RemoteVSTServer::setBufferSize(int sz)
{
    pthread_mutex_lock(&mutex);

    if (bufferSize != sz) {
	m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 0, NULL, 0);
	m_plugin->dispatcher(m_plugin, effSetBlockSize, 0, sz, NULL, 0);
	m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 1, NULL, 0);
	bufferSize = sz;
    }

    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: set buffer size to " << sz << endl;
    }

    pthread_mutex_unlock(&mutex);
}

void
RemoteVSTServer::setSampleRate(int sr)
{
    pthread_mutex_lock(&mutex);

    if (sampleRate != sr) {
	m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 0, NULL, 0);
	m_plugin->dispatcher(m_plugin, effSetSampleRate, 0, 0, NULL, (float)sr);
	m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 1, NULL, 0);
	sampleRate = sr;
    }

    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: set sample rate to " << sr << endl;
    }

    pthread_mutex_unlock(&mutex);
}

void
RemoteVSTServer::reset()
{
    pthread_mutex_lock(&mutex);

    cerr << "dssi-vst-server[1]: reset" << endl;

    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 0, NULL, 0);
    m_plugin->dispatcher(m_plugin, effMainsChanged, 0, 1, NULL, 0);

    pthread_mutex_unlock(&mutex);
}

void
RemoteVSTServer::terminate()
{
    cerr << "RemoteVSTServer::terminate: setting exiting flag" << endl;
    exiting = true;
}

std::string
RemoteVSTServer::getParameterName(int p)
{
    char name[24];
    m_plugin->dispatcher(m_plugin, effGetParamName, p, 0, name, 0);
    return name;
}

void
RemoteVSTServer::setParameter(int p, float v)
{
    if (debugLevel > 1) {
	cerr << "dssi-vst-server[2]: setParameter (" << p << "," << v << ")" << endl;
    }

    pthread_mutex_lock(&mutex);
    
    if (debugLevel > 1)
        cerr << "RemoteVSTServer::setParameter (" << p << "," << v << "): " << m_guiEventsExpected << " events expected" << endl;
    
    if (m_guiFifoFd < 0) {
	m_guiEventsExpected = 0;
    }
    
    if (m_guiEventsExpected > 0) {
	
	//!!! should be per-parameter of course!
	
	struct timeval tv;
	gettimeofday(&tv, NULL);
    
	if (tv.tv_sec > m_lastGuiComms.tv_sec + 10) {
	    m_guiEventsExpected = 0;
	} else {
	    --m_guiEventsExpected;
	    cerr << "Reduced to " << m_guiEventsExpected << endl;
	    pthread_mutex_unlock(&mutex);
	    return;
	}
    }
    
    pthread_mutex_unlock(&mutex);
    
    m_plugin->setParameter(m_plugin, p, v);
}

float
RemoteVSTServer::getParameter(int p)
{
    return m_plugin->getParameter(m_plugin, p);
}

float
RemoteVSTServer::getParameterDefault(int p)
{
    return m_defaults[p];
}

void
RemoteVSTServer::getParameters(int p0, int pn, float *v)
{
    for (int i = p0; i <= pn; ++i) {
	v[i - p0] = m_plugin->getParameter(m_plugin, i);
    }
}

std::string
RemoteVSTServer::getProgramName(int p)
{
    if (debugLevel > 1) {
	cerr << "dssi-vst-server[2]: getProgramName(" << p << ")" << endl;
    }

    pthread_mutex_lock(&mutex);

    char name[24];

    if (m_plugin->dispatcher(m_plugin, effGetVstVersion, 0, 0, NULL, 0) < 2) {

        long prevProgram =
            m_plugin->dispatcher(m_plugin, effGetProgram, 0, 0, NULL, 0);

        m_plugin->dispatcher(m_plugin, effSetProgram, 0, p, NULL, 0);
        m_plugin->dispatcher(m_plugin, effGetProgramName, p, 0, name, 0);
        m_plugin->dispatcher(m_plugin, effSetProgram, 0, prevProgram, NULL, 0);

    } else {
        m_plugin->dispatcher(m_plugin, effGetProgramNameIndexed, p, 0, name, 0);
    }

    pthread_mutex_unlock(&mutex);
    return name;
}

void
RemoteVSTServer::setCurrentProgram(int p)
{
    if (debugLevel > 1) {
	cerr << "dssi-vst-server[2]: setCurrentProgram(" << p << ")" << endl;
    }

    pthread_mutex_lock(&mutex);

    m_plugin->dispatcher(m_plugin, effSetProgram, 0, p, 0, 0);

    pthread_mutex_unlock(&mutex);
}

void
RemoteVSTServer::sendMIDIData(unsigned char *data, int *frameOffsets, int events)
{
#define MIDI_EVENT_BUFFER_COUNT 1024
    static VstMidiEvent vme[MIDI_EVENT_BUFFER_COUNT];
    static char evbuf[sizeof(VstMidiEvent *) * MIDI_EVENT_BUFFER_COUNT +
		      sizeof(VstEvents)];
    
    VstEvents *vstev = (VstEvents *)evbuf;
    vstev->reserved = 0;

    int ix = 0;

    if (events > MIDI_EVENT_BUFFER_COUNT) {
	std::cerr << "vstserv: WARNING: " << events << " MIDI events received "
		  << "for " << MIDI_EVENT_BUFFER_COUNT << "-event buffer"
		  << std::endl;
	events = MIDI_EVENT_BUFFER_COUNT;
    }

    while (ix < events) {

	vme[ix].type = kVstMidiType;
	vme[ix].byteSize = 24;
	vme[ix].deltaFrames = (frameOffsets ? frameOffsets[ix] : 0);
	vme[ix].flags = 0;
	vme[ix].noteLength = 0;
	vme[ix].noteOffset = 0;
	vme[ix].detune = 0;
	vme[ix].noteOffVelocity = 0;
	vme[ix].reserved1 = 0;
	vme[ix].reserved2 = 0;
	vme[ix].midiData[0] = data[ix*3];
	vme[ix].midiData[1] = data[ix*3+1];
	vme[ix].midiData[2] = data[ix*3+2];
	vme[ix].midiData[3] = 0;
	
	vstev->events[ix] = (VstEvent *)&vme[ix];
	
	if (debugLevel > 1) {
	    cerr << "dssi-vst-server[2]: MIDI event in: "
		 << (int)data[ix*3]   << " "
		 << (int)data[ix*3+1] << " "
		 << (int)data[ix*3+2] << endl;
	}
	
	++ix;
    }

    pthread_mutex_lock(&mutex);

    vstev->numEvents = events;
    m_plugin->dispatcher(m_plugin, effProcessEvents, 0, 0, vstev, 0);

    pthread_mutex_unlock(&mutex);
}

bool
RemoteVSTServer::warn(std::string warning)
{
    if (hWnd) MessageBox(hWnd, warning.c_str(), "Error", 0);
    return true;
}

void
RemoteVSTServer::showGUI(std::string guiData)
{
    if (debugLevel > 0) {
	cerr << "RemoteVSTServer::showGUI(" << guiData << "): guiVisible is " << guiVisible << endl;
    }

    if (guiVisible) return;

    if (guiData != m_guiFifoFile || m_guiFifoFd < 0) {

	if (m_guiFifoFd >= 0) {
	    close(m_guiFifoFd);
	    m_guiFifoFd = -1;
	}

	m_guiFifoFile = guiData;

	if ((m_guiFifoFd = open(m_guiFifoFile.c_str(), O_WRONLY | O_NONBLOCK)) < 0) {
	    perror(m_guiFifoFile.c_str());
	    cerr << "WARNING: Failed to open FIFO to GUI manager process" << endl;
	    pthread_mutex_unlock(&mutex);
	    return;
	}

	writeOpcode(m_guiFifoFd, RemotePluginIsReady);
    }

    m_plugin->dispatcher(m_plugin, effEditOpen, 0, 0, hWnd, 0);
    Rect *rect = 0;
    m_plugin->dispatcher(m_plugin, effEditGetRect, 0, 0, &rect, 0);
    if (!rect) {
	cerr << "dssi-vst-server: ERROR: Plugin failed to report window size\n" << endl;
    } else {
	// Seems we need to provide space in here for the titlebar
	// and frame, even though we don't know how big they'll
	// be!  How crap.
	SetWindowPos(hWnd, 0, 0, 0,
		     rect->right - rect->left + 6,
		     rect->bottom - rect->top + 25,
		     SWP_NOACTIVATE | SWP_NOMOVE |
		     SWP_NOOWNERZORDER | SWP_NOZORDER);
	
	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: sized window" << endl;
	}

	ShowWindow(hWnd, SW_SHOWNORMAL);
	UpdateWindow(hWnd);
	guiVisible = true;
    }

    m_paramChangeReadIndex = m_paramChangeWriteIndex;
}

void
RemoteVSTServer::hideGUI()
{
    if (!guiVisible) return;

    if (m_guiFifoFd >= 0) {
	int fd = m_guiFifoFd;
	m_guiFifoFd = -1;
	close(fd);
    }

    ShowWindow(hWnd, SW_HIDE);
    UpdateWindow(hWnd);
    m_plugin->dispatcher(m_plugin, effEditClose, 0, 0, 0, 0);
    guiVisible = false;
}

//Deryabin Andrew: vst chunks support
std::vector<char> RemoteVSTServer::getVSTChunk()
{
    cerr << "dssi-vst-server: Getting vst chunk from plugin.." << endl;
    char * chunkraw = 0;
    int len = m_plugin->dispatcher(m_plugin, 23, 0, 0, (void **)&chunkraw, 0);
    std::vector<char> chunk;
    for(int i = 0; i < len; i++)
    {
        chunk.push_back(chunkraw [i]);
    }

    if (len > 0)
    {
        cerr << "Got " << len << " bytes chunk." << endl;
    }

    return chunk;
}

bool RemoteVSTServer::setVSTChunk(std::vector<char> chunk)
{
    cerr << "dssi-vst-server: Sending vst chunk to plugin. Size=" << chunk.size() << endl;
    std::vector<char>::pointer ptr = &chunk [0];

    pthread_mutex_lock(&mutex);
    m_plugin->dispatcher(m_plugin, 24, 0, chunk.size(), (void *)ptr, 0);
    pthread_mutex_unlock(&mutex);

    return true;
}
//Deryabin Andrew: vst chunks support: end code

void
RemoteVSTServer::startEdit()
{
    m_editLevel = EditStarted;
}

void
RemoteVSTServer::endEdit()
{
    m_editLevel = EditFinished;
}

void
RemoteVSTServer::monitorEdits()
{
    if (m_editLevel != EditNone) {
	
	if (m_editLevel == EditFinished) m_editLevel = EditNone;

	for (int i = 0; i < m_plugin->numParams; ++i) {
	    float actual = m_plugin->getParameter(m_plugin, i);
	    if (actual != m_values[i]) {
		m_values[i] = actual;
		notifyGUI(i, actual);
	    }
	}
    }

    while (m_paramChangeReadIndex != m_paramChangeWriteIndex) {
	int index = m_paramChangeIndices[m_paramChangeReadIndex];
	float value = m_paramChangeValues[m_paramChangeReadIndex];
	if (value != m_values[index]) {
	    m_values[index] = value;
	    notifyGUI(index, value);
	}
	m_paramChangeReadIndex =
	    (m_paramChangeReadIndex + 1) % PARAMETER_CHANGE_COUNT;
    }
}

void
RemoteVSTServer::scheduleGUINotify(int index, float value)
{
    int ni = (m_paramChangeWriteIndex + 1) % PARAMETER_CHANGE_COUNT;
    if (ni == m_paramChangeReadIndex) return;

    m_paramChangeIndices[m_paramChangeWriteIndex] = index;
    m_paramChangeValues[m_paramChangeWriteIndex] = value;
    
    m_paramChangeWriteIndex = ni;
}

void
RemoteVSTServer::notifyGUI(int index, float value)
{
    if (m_guiFifoFd >= 0) {

	if (debugLevel > 1) {
	    cerr << "RemoteVSTServer::notifyGUI(" << index << "," << value << "): about to lock" << endl;
	}

	try {
	    writeOpcode(m_guiFifoFd, RemotePluginSetParameter);
	    int i = (int)index;
	    writeInt(m_guiFifoFd, i);
	    writeFloat(m_guiFifoFd, value);

	    gettimeofday(&m_lastGuiComms, NULL);
	    ++m_guiEventsExpected;

	} catch (RemotePluginClosedException e) {
	    hideGUI();
	}

	if (debugLevel > 1) {
	    cerr << "wrote (" << index << "," << value << ") to gui (" << m_guiEventsExpected << " events expected now)" << endl;
	}
    }
}

void
RemoteVSTServer::checkGUIExited()
{
    if (m_guiFifoFd >= 0) {

	struct pollfd pfd;
	pfd.fd = m_guiFifoFd;
	pfd.events = POLLHUP;

	if (poll(&pfd, 1, 0) != 0) {
	    m_guiFifoFd = -1;
	}

    }
}

void
RemoteVSTServer::terminateGUIProcess()
{
    if (m_guiFifoFd >= 0) {
	writeOpcode(m_guiFifoFd, RemotePluginTerminate);
	m_guiFifoFd = -1;
    }
}

#if 1 // vestige header
#define kVstTransportChanged 1
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
    int rv = 0;

    switch (opcode) {

    case audioMasterAutomate:
    {
	/*!!! Automation:

	When something changes here, we send it straight to the GUI
	via our back channel.  The GUI sends it back to the host via
	configure; that comes to us; and we somehow need to know to
	ignore it.  Checking whether it's the same as the existing
	param value won't cut it, as we might be changing that
	continuously.  (Shall we record that we're expecting the
	configure call because we just sent to the GUI?)

	*/

	float v = plugin->getParameter(plugin, index);

	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterAutomate(" << index << "," << v << ")" << endl;

	if (remoteVSTServerInstance)
	    remoteVSTServerInstance->scheduleGUINotify(index, v);

	break;
    }

    case audioMasterVersion:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterVersion requested" << endl;
	rv = kVstVersion;
	break;

    case audioMasterCurrentId:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterCurrentId requested" << endl;
	rv = 0;
	break;

    case audioMasterIdle:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterIdle requested" << endl;
	if (plugin)
            plugin->dispatcher(plugin, effEditIdle, 0, 0, 0, 0);
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterPinConnected):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterPinConnected requested" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterWantMidi):
	if (debugLevel > 1) {
	    cerr << "dssi-vst-server[2]: audioMasterWantMidi requested" << endl;
	}
	// happy to oblige
	rv = 1;
	break;

    case audioMasterGetTime:
//	if (debugLevel > 1)
//	    cerr << "dssi-vst-server[2]: audioMasterGetTime requested" << endl;
        memset(&timeInfo, 0, sizeof(VstTimeInfo_R));
        timeInfo.sampleRate = sampleRate;
        timeInfo.samplePos  = currentSamplePosition;

        if (jack_client)
        {
            static jack_position_t jack_pos;
            static jack_transport_state_t jack_state;

            jack_state = jack_transport_query(jack_client, &jack_pos);

            if (jack_pos.unique_1 == jack_pos.unique_2)
            {
                timeInfo.sampleRate  = jack_pos.frame_rate;
                timeInfo.samplePos   = jack_pos.frame;
                timeInfo.nanoSeconds = jack_pos.usecs*1000;

                timeInfo.flags |= kVstTransportChanged;
                timeInfo.flags |= kVstNanosValid;

                if (jack_state != JackTransportStopped)
                    timeInfo.flags |= kVstTransportPlaying;

                if (jack_pos.valid & JackPositionBBT)
                {
                    double ppqBar  = double(jack_pos.bar - 1) * jack_pos.beats_per_bar;
                    double ppqBeat = double(jack_pos.beat - 1);
                    double ppqTick = double(jack_pos.tick) / jack_pos.ticks_per_beat;

                    // PPQ Pos
                    timeInfo.ppqPos = ppqBar + ppqBeat + ppqTick;
                    timeInfo.flags |= kVstPpqPosValid;

                    // Tempo
                    timeInfo.tempo  = jack_pos.beats_per_minute;
                    timeInfo.flags |= kVstTempoValid;

                    // Bars
                    timeInfo.barStartPos = ppqBar;
                    timeInfo.flags |= kVstBarsValid;

                    // Time Signature
                    timeInfo.timeSigNumerator   = jack_pos.beats_per_bar;
                    timeInfo.timeSigDenominator = jack_pos.beat_type;
                    timeInfo.flags |= kVstTimeSigValid;
                }
            }
        }
	rv = (intptr_t)&timeInfo;
	break;

    case audioMasterProcessEvents:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterProcessEvents requested" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterSetTime):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterSetTime requested" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterTempoAt):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterTempoAt requested" << endl;
	// can't support this, return 120bpm
	rv = 120 * 10000;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterGetNumAutomatableParameters):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetNumAutomatableParameters requested" << endl;
	rv = 5000;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterGetParameterQuantization):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetParameterQuantization requested" << endl;
	rv = 1;
	break;

    case audioMasterIOChanged:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterIOChanged requested" << endl;
	cerr << "WARNING: Plugin inputs and/or outputs changed: NOT SUPPORTED" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterNeedIdle):
	if (debugLevel > 1) {
	    cerr << "dssi-vst-server[2]: audioMasterNeedIdle requested" << endl;
	}
	needIdle=true;
	rv = 1;
	break;

    case audioMasterSizeWindow:
	if (debugLevel > 1) {
	    cerr << "dssi-vst-server[2]: audioMasterSizeWindow requested" << endl;
	}
	if (hWnd) {
	    SetWindowPos(hWnd, 0, 0, 0,
			 index + 6,
			 value + 25,
			 SWP_NOACTIVATE | SWP_NOMOVE |
			 SWP_NOOWNERZORDER | SWP_NOZORDER);
	}
	rv = 1;
	break;

    case audioMasterGetSampleRate:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetSampleRate requested" << endl;
	if (!sampleRate) {
	    cerr << "WARNING: Sample rate requested but not yet set" << endl;
	}
	plugin->dispatcher(plugin, effSetSampleRate,
			   0, 0, NULL, (float)sampleRate);
        rv = sampleRate;
	break;

    case audioMasterGetBlockSize:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetBlockSize requested" << endl;
	if (!bufferSize) {
	    cerr << "WARNING: Buffer size requested but not yet set" << endl;
	}
	plugin->dispatcher(plugin, effSetBlockSize,
			   0, bufferSize, NULL, 0);
        rv = bufferSize;
	break;

    case audioMasterGetInputLatency:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetInputLatency requested" << endl;
	break;

    case audioMasterGetOutputLatency:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetOutputLatency requested" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterGetPreviousPlug):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetPreviousPlug requested" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterGetNextPlug):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetNextPlug requested" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterWillReplaceOrAccumulate):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterWillReplaceOrAccumulate requested" << endl;
	// 0 -> unsupported, 1 -> replace, 2 -> accumulate
	rv = 1;
	break;

    case audioMasterGetCurrentProcessLevel:
	if (debugLevel > 1) {
	    cerr << "dssi-vst-server[2]: audioMasterGetCurrentProcessLevel requested (level is " << (inProcessThread ? 2 : 1) << ")" << endl;
	}
	// 0 -> unsupported, 1 -> gui, 2 -> process, 3 -> midi/timer, 4 -> offline
	if (inProcessThread) rv = 2;
	else rv = 1;
	break;

    case audioMasterGetAutomationState:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetAutomationState requested" << endl;
	rv = 4; // read/write
	break;

    case audioMasterOfflineStart:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOfflineStart requested" << endl;
	break;

    case audioMasterOfflineRead:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOfflineRead requested" << endl;
	break;

    case audioMasterOfflineWrite:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOfflineWrite requested" << endl;
	break;

    case audioMasterOfflineGetCurrentPass:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOfflineGetCurrentPass requested" << endl;
	break;

    case audioMasterOfflineGetCurrentMetaPass:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOfflineGetCurrentMetaPass requested" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterSetOutputSampleRate):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterSetOutputSampleRate requested" << endl;
	break;

/* Deprecated in VST 2.4 and also (accidentally?) renamed in the SDK header,
   so we won't retain it here
    case audioMasterGetSpeakerArrangement:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetSpeakerArrangement requested" << endl;
	break;
*/
    case audioMasterGetVendorString:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetVendorString requested" << endl;
	strcpy((char *)ptr, "Chris Cannam");
	break;

    case audioMasterGetProductString:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetProductString requested" << endl;
	strcpy((char *)ptr, "DSSI-VST Plugin");
	break;

    case audioMasterGetVendorVersion:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetVendorVersion requested" << endl;
	rv = long(RemotePluginVersion * 100);
	break;

    case audioMasterVendorSpecific:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterVendorSpecific requested" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterSetIcon):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterSetIcon requested" << endl;
	break;

    case audioMasterCanDo:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterCanDo(" << (char *)ptr
		 << ") requested" << endl;
        if (!strcmp((char*)ptr, "sendVstEvents") ||
            !strcmp((char*)ptr, "sendVstMidiEvent") ||
            !strcmp((char*)ptr, "sendVstTimeInfo") ||
            !strcmp((char*)ptr, "sizeWindow") ||
            !strcmp((char*)ptr, "supplyIdle") ||
            !strcmp((char*)ptr, "receiveVstEvents") ||
            !strcmp((char*)ptr, "receiveVstMidiEvent")) {
	    rv = 1;
	}
	break;

    case audioMasterGetLanguage:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetLanguage requested" << endl;
	rv = kVstLangEnglish;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterOpenWindow):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOpenWindow requested" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterCloseWindow):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterCloseWindow requested" << endl;
	break;

    case audioMasterGetDirectory:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetDirectory requested" << endl;
	break;

    case audioMasterUpdateDisplay:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterUpdateDisplay requested" << endl;
	if (plugin)
            plugin->dispatcher(plugin, effEditIdle, 0, 0, NULL, 0);
	break;

    case audioMasterBeginEdit:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterBeginEdit requested" << endl;
	remoteVSTServerInstance->startEdit();
	break;

    case audioMasterEndEdit:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterEndEdit requested" << endl;
	remoteVSTServerInstance->endEdit();
	break;

    case audioMasterOpenFileSelector:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterOpenFileSelector requested" << endl;
	break;

    case audioMasterCloseFileSelector:
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterCloseFileSelector requested" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterEditFile):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterEditFile requested" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterGetChunkFile):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetChunkFile requested" << endl;
	break;

    case DEPRECATED_VST_SYMBOL(audioMasterGetInputSpeakerArrangement):
	if (debugLevel > 1)
	    cerr << "dssi-vst-server[2]: audioMasterGetInputSpeakerArrangement requested" << endl;
	break;

    default:
	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[0]: unsupported audioMaster callback opcode "
		 << opcode << endl;
	}
    }

    return rv;
};

DWORD WINAPI
WatchdogThreadMain(LPVOID parameter)
{
    struct sched_param param;
    param.sched_priority = 2;
    int result = sched_setscheduler(0, SCHED_FIFO, &param);
    if (result < 0) {
	perror("Failed to set realtime priority for watchdog thread");
    }

    int count = 0;

    while (!exiting) {
	if (!alive) {
	    ++count;
	}
	if (count == 20) {
	    cerr << "Remote VST plugin watchdog: terminating audio thread" << endl;
	    // bam
	    TerminateThread(audioThreadHandle, 0);
	    exiting = 1;
	    break;
	} else {
//	    cerr << "Remote VST plugin watchdog: OK, count is " << count << endl;
	}
	sleep(1);
    }

    cerr << "Remote VST plugin watchdog thread: returning" << endl;

    param.sched_priority = 0;
    (void)sched_setscheduler(0, SCHED_OTHER, &param);
    return 0;
}

DWORD WINAPI
AudioThreadMain(LPVOID parameter)
{
    struct sched_param param;
    param.sched_priority = 1;
    HANDLE watchdogThreadHandle;

    int result = sched_setscheduler(0, SCHED_FIFO, &param);

    if (result < 0) {
	perror("Failed to set realtime priority for audio thread");
    } else {
	// Start a watchdog thread as well
	DWORD watchdogThreadId = 0;
	watchdogThreadHandle =
	    CreateThread(0, 0, WatchdogThreadMain, 0, 0, &watchdogThreadId);
	if (!watchdogThreadHandle) {
	    cerr << "Failed to create watchdog thread -- not using RT priority for audio thread" << endl;
	    param.sched_priority = 0;
	    (void)sched_setscheduler(0, SCHED_OTHER, &param);
	}
    }

    while (!exiting) {
	alive = true;
	try {
	    // This can call sendMIDIData, setCurrentProgram, process
	    remoteVSTServerInstance->dispatchProcess(50);
	} catch (std::string message) {
	    cerr << "ERROR: Remote VST server instance failed: " << message << endl;
	    exiting = true;
	} catch (RemotePluginClosedException) {
	    cerr << "ERROR: Remote VST plugin communication failure in audio thread" << endl;
	    exiting = true;
	}
    }

    cerr << "Remote VST plugin audio thread: returning" << endl;

    param.sched_priority = 0;
    (void)sched_setscheduler(0, SCHED_OTHER, &param);

    if (watchdogThreadHandle) {
	TerminateThread(watchdogThreadHandle, 0);
	CloseHandle(watchdogThreadHandle);
    }
    return 0;
}

LRESULT WINAPI
MainProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CLOSE:
	remoteVSTServerInstance->terminateGUIProcess();
        remoteVSTServerInstance->hideGUI();
        return TRUE;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI
WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR cmdline, int cmdshow)
{
    char *libname = 0;
    char *fileInfo = 0;
    bool tryGui = false, haveGui = true;

    cout << "DSSI VST plugin server v" << RemotePluginVersion << endl;
    cout << "Copyright (c) 2004-2010 Chris Cannam" << endl;

    char *home = getenv("HOME");

    if (cmdline) {
	int offset = 0;
	if (cmdline[0] == '"' || cmdline[0] == '\'') offset = 1;
	if (!strncmp(&cmdline[offset], "-g ", 3)) {
	    tryGui = true;
	    offset += 3;
	}
	for (int ci = offset; cmdline[ci]; ++ci) {
	    if (cmdline[ci] == ',') {
		libname = strndup(cmdline + offset, ci - offset);
		++ci;
		if (cmdline[ci]) {
		    fileInfo = strdup(cmdline + ci);
		    int l = strlen(fileInfo);
		    if (fileInfo[l-1] == '"' ||
			fileInfo[l-1] == '\'') {
			fileInfo[l-1] = '\0';
		    }
		}
	    }
	}
    }

    if (!libname || !libname[0] || !fileInfo || !fileInfo[0]) {
	cerr << "Usage: dssi-vst-server <vstname.dll>,<tmpfilebase>" << endl;
	cerr << "(Command line was: " << cmdline << ")" << endl;
	exit(2);
    }

    // LADSPA labels can't contain spaces so dssi-vst replaces spaces
    // with asterisks.
    for (int ci = 0; libname[ci]; ++ci) {
	if (libname[ci] == '*') libname[ci] = ' ';
    }

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
	    cerr << "dssi-vst-server[1]: " << (libHandle ? "" : "not ")
		 << "found in " << libPath << endl;
	}

	if (!libHandle) {
	    if (home && home[0] != '\0') {
		if (libPath.substr(0, strlen(home)) == home) {
		    libPath = libPath.substr(strlen(home) + 1);
		}
		libHandle = LoadLibrary(libPath.c_str());
		if (debugLevel > 0) {
		    cerr << "dssi-vst-server[1]: " << (libHandle ? "" : "not ")
			 << "found in " << libPath << endl;
		}
	    }
	}

	if (libHandle) break;
    }	

    if (!libHandle) {
	libHandle = LoadLibrary(libname);
	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: " << (libHandle ? "" : "not ")
		 << "found in DLL path" << endl;
	}
    }

    if (!libHandle) {
	cerr << "dssi-vst-server: ERROR: Couldn't load VST DLL \"" << libname << "\"" << endl;
	return 1;
    }

    cout << "done" << endl;

    cout << "Testing VST compatibility... ";
    if (debugLevel > 0) cout << endl;

//!!! better debug level support
    
    AEffect *(__stdcall* getInstance)(audioMasterCallback);

    getInstance = (AEffect*(__stdcall*)(audioMasterCallback))
	GetProcAddress(libHandle, NEW_PLUGIN_ENTRY_POINT);

    if (!getInstance) {
	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: VST 2.4 entrypoint \""
		 << NEW_PLUGIN_ENTRY_POINT << "\" not found in DLL \""
		 << libname << "\", looking for \""
		 << OLD_PLUGIN_ENTRY_POINT << "\"" << endl;
	}

	getInstance = (AEffect*(__stdcall*)(audioMasterCallback))
	    GetProcAddress(libHandle, OLD_PLUGIN_ENTRY_POINT);

	if (!getInstance) {
	    cerr << "dssi-vst-server: ERROR: VST entrypoints \""
		 << NEW_PLUGIN_ENTRY_POINT << "\" or \"" 
		 << OLD_PLUGIN_ENTRY_POINT << "\" not found in DLL \""
		 << libname << "\"" << endl;
	    return 1;
	} else if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: VST entrypoint \""
		 << OLD_PLUGIN_ENTRY_POINT << "\" found" << endl;
	}

    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: VST entrypoint \""
	     << NEW_PLUGIN_ENTRY_POINT << "\" found" << endl;
    }

    AEffect *plugin = getInstance(hostCallback);

    if (!plugin) {
	cerr << "dssi-vst-server: ERROR: Failed to instantiate plugin in VST DLL \""
	     << libname << "\"" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: plugin instantiated" << endl;
    }

    if (plugin->magic != kEffectMagic) {
	cerr << "dssi-vst-server: ERROR: Not a VST plugin in DLL \"" << libname << "\"" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: plugin is a VST" << endl;
    }

    if (!(plugin->flags & effFlagsHasEditor)) {
	if (debugLevel > 0) {
	    cerr << "dssi-vst-server[1]: Plugin has no GUI" << endl;
	}
	haveGui = false;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: plugin has a GUI" << endl;
    }

    if (!(plugin->flags & effFlagsCanReplacing)) {
	cerr << "dssi-vst-server: ERROR: Plugin does not support processReplacing (required)"
	     << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: plugin supports processReplacing" << endl;
    }

    try {
	remoteVSTServerInstance =
	    new RemoteVSTServer(fileInfo, plugin, libname);
    } catch (std::string message) {
	cerr << "ERROR: Remote VST startup failed: " << message << endl;
	return 1;
    } catch (RemotePluginClosedException) {
	cerr << "ERROR: Remote VST plugin communication failure in startup" << endl;
	return 1;
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
//    wclass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wclass.lpszMenuName = "MENU_DSSI_VST";
    wclass.lpszClassName = APPLICATION_CLASS_NAME;
    wclass.hIconSm = 0;
	
    if (!RegisterClassEx(&wclass)) {
	cerr << "dssi-vst-server: ERROR: Failed to register Windows application class!\n" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: registered Windows application class \"" << APPLICATION_CLASS_NAME << "\"" << endl;
    }
    
    hWnd = CreateWindow
	(APPLICATION_CLASS_NAME, remoteVSTServerInstance->getName().c_str(),
	 WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
	 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
	 0, 0, hInst, 0);
    if (!hWnd) {
	cerr << "dssi-vst-server: ERROR: Failed to create window!\n" << endl;
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: created main window" << endl;
    }
    
    if (!haveGui) {
	cerr << "Should be showing message here" << endl;
    } else {
	
	if (tryGui) {
	
	    plugin->dispatcher(plugin, effEditOpen, 0, 0, hWnd, 0);
	    Rect *rect = 0;
	    plugin->dispatcher(plugin, effEditGetRect, 0, 0, &rect, 0);
	    if (!rect) {
		cerr << "dssi-vst-server: ERROR: Plugin failed to report window size\n" << endl;
		return 1;
	    }
	
	    // Seems we need to provide space in here for the titlebar
	    // and frame, even though we don't know how big they'll
	    // be!  How crap.
	    SetWindowPos(hWnd, 0, 0, 0,
			 rect->right - rect->left + 6,
			 rect->bottom - rect->top + 25,
			 SWP_NOACTIVATE | SWP_NOMOVE |
			 SWP_NOOWNERZORDER | SWP_NOZORDER);
	    
	    if (debugLevel > 0) {
		cerr << "dssi-vst-server[1]: sized window" << endl;
	    }

	    ShowWindow(hWnd, SW_SHOWNORMAL);
	    UpdateWindow(hWnd);

	    if (debugLevel > 0) {
		cerr << "dssi-vst-server[1]: showed window" << endl;
	    }

	    guiVisible = true;
	}
    }

    cout << "done" << endl;

    DWORD threadId = 0;
    audioThreadHandle = CreateThread(0, 0, AudioThreadMain, 0, 0, &threadId);
    if (!audioThreadHandle) {
	cerr << "Failed to create audio thread!" << endl;
	delete remoteVSTServerInstance;
	FreeLibrary(libHandle);
	return 1;
    } else if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: created audio thread" << endl;
    }

    /* create a dummy window for timer events - this bit based on fst
     * by Torben Hohn, patch worked out by Robert Jonsson - thanks! */
    if ((hInst = GetModuleHandleA (NULL)) == NULL) {
	cerr << "can't get module handle" << endl;
	return 1;
    }

    // create dummy jack client for transport info
    jack_client = jack_client_open("dssi-vst", JackNoStartServer, 0);

    if (jack_client)
    {
        jack_set_process_callback(jack_client, jack_process_callback, 0);
        jack_activate(jack_client);
    }

    HWND window;
    if ((window = CreateWindowExA
	 (0, "dssi-vst", "dummy",
	  WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
	  9999, 9999,
	  1, 1,
	  NULL, NULL,
	  hInst,
	  NULL )) == NULL) {
	cerr << "cannot create dummy timer window" << endl;
    }
    if (!SetTimer (window, 1000, 20, NULL)) {
	cerr << "cannot set timer on window" << endl;
    }

    ready = true;

    MSG msg;
    exiting = false;
    while (!exiting) {

	while (!exiting && PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
	    DispatchMessage(&msg);

	    /* this bit based on fst by Torben Hohn, patch worked out
	     * by Robert Jonsson - thanks! */
	    if (msg.message == WM_TIMER) {
		plugin->dispatcher (plugin, effEditIdle, 0, 0, NULL, 0);
		if (needIdle) {
		    if (plugin) {
			plugin->dispatcher(plugin, 53, 0, 0, NULL, 0);
		    }
		}
	    }
	}

	if (tryGui && haveGui && !guiVisible) {
	    // Running in GUI-always-on mode and GUI has exited: follow it
	    cerr << "dssi-vst-server: Running in GUI mode and GUI has exited: going with it" << endl;
	    exiting = true;
	}

	if (exiting) break;

	try {
	    if (guiVisible) {
		remoteVSTServerInstance->dispatchControl(10);
	    } else {
		remoteVSTServerInstance->dispatchControl(500);
	    }
	} catch (RemotePluginClosedException) {
	    cerr << "ERROR: Remote VST plugin communication failure in GUI thread" << endl;
	    exiting = true;
	    break;
	}

	remoteVSTServerInstance->checkGUIExited();
	remoteVSTServerInstance->monitorEdits();
    }

    // wait for audio thread to catch up
    sleep(1);

    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: cleaning up" << endl;
    }

    if (jack_client)
    {
        jack_deactivate(jack_client);
        jack_client_close(jack_client);
    }

    CloseHandle(audioThreadHandle);
    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: closed audio thread" << endl;
    }

    delete remoteVSTServerInstance;
    remoteVSTServerInstance = 0;

    FreeLibrary(libHandle);
    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: freed dll" << endl;
    }

    if (debugLevel > 0) {
	cerr << "dssi-vst-server[1]: exiting" << endl;
    }

    return 0;
}

