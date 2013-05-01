
/* -*- c-basic-offset: 4 -*- */

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004-2010 Chris Cannam
*/

#include "remotevstclient.h"
#include "rdwrops.h"

#include "dssi/ladspa.h"
#include "dssi/dssi.h"
#include <alsa/seq_event.h>
#include <alsa/seq_midi_event.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <iostream>
#include <string.h>
#include <stdlib.h>

class DSSIVSTPluginInstance
{
public:
    static void freeFields(DSSI_Descriptor &descriptor);

    DSSIVSTPluginInstance(std::string dllName,
			unsigned long sampleRate);
    virtual ~DSSIVSTPluginInstance();

    bool isOK() { return m_ok; }

    // LADSPA methods:

    void activate();
    void deactivate();
    void connectPort(unsigned long port, LADSPA_Data *location);
    void run(unsigned long sampleCount);

    // DSSI methods:

    const DSSI_Program_Descriptor *getProgram(unsigned long index);
    void selectProgram(unsigned long bank, unsigned long program);
    void runSynth(unsigned long sampleCount,
		  snd_seq_event_t *events, unsigned long eventCount);
    std::string configure(std::string key, std::string value);

protected:
    static RemotePluginClient *load(std::string name);

    unsigned long              m_sampleRate;
    unsigned long              m_lastSampleCount;

    LADSPA_Data              **m_controlPorts;
    LADSPA_Data               *m_controlPortsSaved;
    unsigned long              m_controlPortCount;

    LADSPA_Data              **m_audioIns;
    unsigned long              m_audioInCount;

    LADSPA_Data              **m_audioOuts;
    unsigned long              m_audioOutCount;

    LADSPA_Data               *m_latencyOut;

    DSSI_Program_Descriptor   *m_programs;
    unsigned long              m_programCount;

    unsigned char              m_decodeBuffer[MIDI_BUFFER_SIZE];
    int                        m_frameOffsetsBuffer[MIDI_BUFFER_SIZE / 3];
    snd_midi_event_t          *m_alsaDecoder;

    bool m_pendingProgram;

    RemotePluginClient        *m_plugin;
    bool                       m_ok;

    //Andrew Deryabin: VST chunks support
    char *m_chunkdata;
    friend class DSSIVSTPlugin;
    //Andrew Deryabin: VST chunks support: end code
};

class DSSIVSTPlugin
{
public:
    DSSIVSTPlugin();
    virtual ~DSSIVSTPlugin();

    DSSI_Descriptor *queryDescriptor(unsigned long index);

    // LADSPA methods:

    static LADSPA_Handle instantiate(const LADSPA_Descriptor *descriptor,
				     unsigned long sampleRate);
    
    static void connect_port(LADSPA_Handle instance,
			     unsigned long port,
			     LADSPA_Data *location);

    static void activate(LADSPA_Handle instance);
    
    static void run(LADSPA_Handle instance,
		    unsigned long sampleCount);

    static void deactivate(LADSPA_Handle instance);

    static void cleanup(LADSPA_Handle instance);

    // DSSI methods:

    static const DSSI_Program_Descriptor *get_program(LADSPA_Handle instance,
						      unsigned long index);

    static void select_program(LADSPA_Handle instance,
			       unsigned long bank, unsigned long program);

    static void run_synth(LADSPA_Handle instance, unsigned long sampleCount,
			  snd_seq_event_t *events, unsigned long eventCount);

    static char *configure(LADSPA_Handle instance, const char *key,
			   const char *value);

    static int set_custom_data(LADSPA_Handle Instance, void *Data, unsigned long DataLength);
    static int get_custom_data(LADSPA_Handle Instance, void **Data, unsigned long *DataLength);

private:
    typedef std::pair<std::string, DSSI_Descriptor *> PluginPair;
    typedef std::vector<PluginPair> PluginList;
    PluginList m_descriptors;
};


#define NO_CONTROL_DATA -10000000000000.0

DSSIVSTPluginInstance::DSSIVSTPluginInstance(std::string dllName,
					 unsigned long sampleRate) :
    m_sampleRate(sampleRate),
    m_lastSampleCount(0),
    m_controlPorts(0),
    m_controlPortsSaved(0),
    m_controlPortCount(0),
    m_audioIns(0),
    m_audioInCount(0),
    m_audioOuts(0),
    m_audioOutCount(0),
    m_programs(0),
    m_programCount(0),
    m_alsaDecoder(0),
    m_pendingProgram(false),
    m_plugin(0),
    m_ok(false),
    m_chunkdata(0)
{
    std::cerr << "DSSIVSTPluginInstance::DSSIVSTPluginInstance(" << dllName << ")" << std::endl;

    try {
	m_plugin = new RemoteVSTClient(dllName);

    } catch (RemotePluginClosedException) {
	std::cerr << "DSSIVSTPluginInstance::DSSIVSTPluginInstance("
		  << dllName << "): startup failed" << std::endl;

	m_ok = false;
	delete m_plugin; m_plugin = 0;
	return;

    } catch (std::string message) {
	std::cerr << "DSSIVSTPluginInstance::DSSIVSTPluginInstance("
		  << dllName << "): startup failed: " << message << std::endl;
	
	m_ok = false;
	delete m_plugin; m_plugin = 0;
	return;
    }

    m_controlPortCount = m_plugin->getParameterCount();
    m_controlPorts = new LADSPA_Data*[m_controlPortCount];
    m_controlPortsSaved = new LADSPA_Data[m_controlPortCount];

    for (unsigned long i = 0; i < m_controlPortCount; ++i) {
	m_controlPortsSaved[i] = NO_CONTROL_DATA;
    }

    m_audioInCount = m_plugin->getInputCount();
    m_audioIns = new LADSPA_Data*[m_audioInCount];

    m_audioOutCount = m_plugin->getOutputCount();
    m_audioOuts = new LADSPA_Data*[m_audioOutCount];

    m_programCount = m_plugin->getProgramCount();
    m_programs = new DSSI_Program_Descriptor[m_programCount];
    for (unsigned long i = 0; i < m_programCount; ++i) {
	m_programs[i].Bank = 0;
	m_programs[i].Program = i;
	m_programs[i].Name = strdup(m_plugin->getProgramName(i).c_str());
    }

    snd_midi_event_new(MIDI_BUFFER_SIZE, &m_alsaDecoder);
    if (!m_alsaDecoder) {
	std::cerr << "DSSIVSTPluginInstance::DSSIVSTPluginInstance("
		  << dllName << "): failed to initialize ALSA MIDI decoder"
		  << std::endl;
    } else {
	snd_midi_event_no_status(m_alsaDecoder, 1);
    }

    m_ok = true;
    std::cerr << "DSSIVSTPluginInstance::DSSIVSTPluginInstance(" << dllName << ") construction complete" << std::endl;
}

DSSIVSTPluginInstance::~DSSIVSTPluginInstance()
{
    std::cerr << "DSSIVSTPluginInstance::~DSSIVSTPluginInstance" << std::endl;

    if (m_ok) {
	try {
	    std::cerr << "DSSIVSTPluginInstance::~DSSIVSTPluginInstance: asking plugin to terminate" << std::endl;
	    m_plugin->terminate();
	} catch (RemotePluginClosedException) { }
    }

    if (!m_plugin) {
	return;
    }

    delete m_plugin;
    delete m_chunkdata;

    if (m_alsaDecoder) {
	snd_midi_event_free(m_alsaDecoder);
    }

    delete[] m_controlPorts;
    delete[] m_controlPortsSaved;
    delete[] m_audioIns;
    delete[] m_audioOuts;

    for (unsigned long i = 0; i < m_programCount; ++i) {
	free((void *)m_programs[i].Name);
    }
    delete[] m_programs;
}

void
DSSIVSTPluginInstance::activate()
{
    if (m_ok) {
	try {
	    m_plugin->setSampleRate(m_sampleRate);
	} catch (RemotePluginClosedException) {
	    m_ok = false;
	}
    }
}

void
DSSIVSTPluginInstance::deactivate()
{
    if (m_ok) {
	try {
	    m_plugin->reset();
	} catch (RemotePluginClosedException) {
	    m_ok = false;
	}
    }
}

void
DSSIVSTPluginInstance::connectPort(unsigned long port, LADSPA_Data *location)
{
//    std::cerr << "connectPort(" << port << "," << location << ")" << std::endl;

    if (!m_ok) return;

    if (port < m_controlPortCount) {
//	std::cerr << "(control port)" << std::endl;
	m_controlPorts[port] = location;
	return;
    }
    port -= m_controlPortCount;

    if (port < m_audioInCount) {
//	std::cerr << "(audio in port)" << std::endl;
	m_audioIns[port] = location;
	return;
    }
    port -= m_audioInCount;

    if (port < m_audioOutCount) {
//	std::cerr << "(audio out port)" << std::endl;
	m_audioOuts[port] = location;
	return;
    }
    port -= m_audioOutCount;

    if (port < 1) { // latency
//	std::cerr << "(latency output port)" << std::endl;
	m_latencyOut = location;
	if (m_latencyOut) *m_latencyOut = 0;
	return;
    }
}

const DSSI_Program_Descriptor *
DSSIVSTPluginInstance::getProgram(unsigned long index)
{
    if (index >= m_programCount) return 0;
    return &m_programs[index];
}

void
DSSIVSTPluginInstance::selectProgram(unsigned long bank, unsigned long program)
{
    if (bank != 0 || program >= m_programCount) return;

    try {
	m_plugin->setCurrentProgram(program);
	m_plugin->getParameters(0, m_controlPortCount - 1, m_controlPortsSaved);

	for (unsigned long i = 0; i < m_controlPortCount; ++i) {
	    if (!m_controlPorts[i]) continue;
	    *m_controlPorts[i] = m_controlPortsSaved[i];
	}

    } catch (RemotePluginClosedException) {
	m_ok = false;
	return;
    }
}

void
DSSIVSTPluginInstance::run(unsigned long sampleCount)
{
    if (!m_ok) return;

    try {
	if (sampleCount != m_lastSampleCount) {
	    m_plugin->setBufferSize(sampleCount);
	    m_lastSampleCount = sampleCount;
	}
	
	int modifiedCount = 0;
	
	for (unsigned long i = 0; i < m_controlPortCount; ++i) {
	    
	    if (!m_controlPorts[i]) continue;
	    
	    if (m_controlPortsSaved[i] != *m_controlPorts[i]) {
//		    std::cout << "Sending new value " << *m_controlPorts[i]
//			      << " for control port " << i << std::endl;
		m_plugin->setParameter(i, *m_controlPorts[i]);
		m_controlPortsSaved[i] =  *m_controlPorts[i];
		if (++modifiedCount > 10) break;
	    }
	}
	
	m_plugin->process(m_audioIns, m_audioOuts);
	
    } catch (RemotePluginClosedException) {
	m_ok = false;
    }
}

void
DSSIVSTPluginInstance::runSynth(unsigned long sampleCount,
				snd_seq_event_t *events, unsigned long eventCount)
{
    if (!m_ok) return;

    try {
	if (m_alsaDecoder) {

	    unsigned long index = 0;
	    unsigned long i;
	    
	    for (i = 0; i < eventCount; ++i) {
		
		snd_seq_event_t *ev = &events[i];

		if (index >= MIDI_BUFFER_SIZE - 4) break;

//		std::cerr << "MIDI event at frame " << ev->time.tick
//			  << ", channel " << int(ev->data.note.channel) << std::endl;

		m_frameOffsetsBuffer[i] = ev->time.tick;
		ev->time.tick = 0;
		
		long count = snd_midi_event_decode(m_alsaDecoder,
						   m_decodeBuffer + index,
						   MIDI_BUFFER_SIZE - index,
						   ev);
		if (count < 0) {
		    std::cerr << "WARNING: MIDI decoder error " << count
			      << " for event type " << ev->type << std::endl;
		} else if (count == 0 || count > 3) {
		    std::cerr << "WARNING: MIDI event of type " << ev->type
			      << " decoded to " << count << " bytes, discarding" << std::endl;
		} else {
		    index += count;
		    while (count++ < 3) {
			m_decodeBuffer[index++] = '\0';
		    }
		}
	    }
	    
	    if (index > 0) {
		m_plugin->sendMIDIData(m_decodeBuffer, m_frameOffsetsBuffer, i);
	    }
	}
    } catch (RemotePluginClosedException) {
	m_ok = false;
    }

    run(sampleCount);
}

std::string
DSSIVSTPluginInstance::configure(std::string key, std::string value)
{
    std::cerr << "DSSIVSTPluginInstance::configure(" << key << "," << value <<")" << std::endl;

    try {
	if (key == "guiVisible") {
	    if (value.length() > 0) {
		std::cerr << "DSSIVSTPluginInstance::configure: show gui: value " << value << std::endl;
		m_plugin->showGUI(value);
	    } else {
		std::cerr << "DSSIVSTPluginInstance::configure: hide gui" << std::endl;
		m_plugin->hideGUI();
	    }
	}
    } catch (RemotePluginClosedException) {
	m_ok = false;
    }

    return "";
}

void
DSSIVSTPluginInstance::freeFields(DSSI_Descriptor &descriptor)
{
    LADSPA_Descriptor &ldesc = (LADSPA_Descriptor &)*descriptor.LADSPA_Plugin;

    if (ldesc.Name)      free((char *)ldesc.Name);
    if (ldesc.Maker)     free((char *)ldesc.Maker);
    if (ldesc.Copyright) free((char *)ldesc.Copyright);

    if (ldesc.PortDescriptors) {
	delete[] ldesc.PortDescriptors;
    }

    if (ldesc.PortNames) {
	for (unsigned long i = 0; i < ldesc.PortCount; ++i) {
	    free((char *)ldesc.PortNames[i]);
	}
	delete[] ldesc.PortNames;
    }

    if (ldesc.PortRangeHints) {
	delete[] ldesc.PortRangeHints;
    }
}


DSSIVSTPlugin::DSSIVSTPlugin()
{
    std::vector<RemoteVSTClient::PluginRecord> plugins;

    try {
	RemoteVSTClient::queryPlugins(plugins);
    } catch (std::string error) {
	std::cerr << "DSSIVSTPlugin: Error on plugin query: " << error << std::endl;
	return;
    }
    
    for (unsigned int p = 0; p < plugins.size(); ++p) {

	DSSI_Descriptor *descriptor = new DSSI_Descriptor;
	LADSPA_Descriptor *ldesc = new LADSPA_Descriptor;
	descriptor->LADSPA_Plugin = ldesc;

	RemoteVSTClient::PluginRecord &rec = plugins[p];

	// LADSPA labels mustn't contain spaces.  We replace them with
	// asterisks here and restore them when used to indicate DLL name
	// again.
	char *label = strdup(rec.dllName.c_str());
	for (int i = 0; label[i]; ++i) {
	    if (label[i] == ' ') label[i] = '*';
	}

	ldesc->UniqueID = 6666 + p;
	ldesc->Label = label;
	ldesc->Properties = LADSPA_PROPERTY_HARD_RT_CAPABLE;
	ldesc->Name = strdup(std::string(rec.pluginName + " VST").c_str());
	ldesc->Maker = strdup(rec.vendorName.c_str());
	ldesc->Copyright = strdup(ldesc->Maker);

//	std::cerr << "Plugin name: " << ldesc->Name << std::endl;

	int parameters = rec.parameters;
	int inputs = rec.inputs;
	int outputs = rec.outputs;
	int portCount = parameters + inputs + outputs + 1; // 1 for latency output

	LADSPA_PortDescriptor *ports = new LADSPA_PortDescriptor[portCount];
        char **names = new char *[portCount];
	LADSPA_PortRangeHint *hints = new LADSPA_PortRangeHint[portCount];

	for (int i = 0; i < parameters; ++i) {
	    ports[i] = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
	    names[i] = strdup(rec.parameterNames[i].c_str());
	    hints[i].LowerBound = 0.0f;
	    hints[i].UpperBound = 1.0f;
	    hints[i].HintDescriptor =
		LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
	    float deflt = rec.parameterDefaults[i];
	    if (deflt < 0.0001) {
		hints[i].HintDescriptor |= LADSPA_HINT_DEFAULT_MINIMUM;
	    } else if (deflt > 0.999) {
		hints[i].HintDescriptor |= LADSPA_HINT_DEFAULT_MAXIMUM;
	    } else if (deflt < 0.35) {
		hints[i].HintDescriptor |= LADSPA_HINT_DEFAULT_LOW;
	    } else if (deflt > 0.65) {
		hints[i].HintDescriptor |= LADSPA_HINT_DEFAULT_HIGH;
	    } else {
		hints[i].HintDescriptor |= LADSPA_HINT_DEFAULT_MIDDLE;
	    }
	}

	for (int i = 0; i < inputs; ++i) {
	    int j = i + parameters;
	    ports[j] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
	    char buf[20];
	    snprintf(buf, 19, "in%d", i + 1);
	    names[j] = strdup(buf);
	    hints[j].HintDescriptor = 0;
	}

	for (int i = 0; i < outputs; ++i) {
	    int j = i + inputs + parameters;
	    ports[j] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
	    char buf[20];
	    snprintf(buf, 19, "out%d", i + 1);
	    names[j] = strdup(buf);
	    hints[j].HintDescriptor = 0;
	}

	ports[portCount-1] = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
	names[portCount-1] = strdup("_latency");
	hints[portCount-1].HintDescriptor = 0;

	ldesc->PortCount = portCount;
	ldesc->PortDescriptors = ports;
	ldesc->PortNames = names;
	ldesc->PortRangeHints = hints;
	ldesc->ImplementationData = 0;

	ldesc->instantiate = DSSIVSTPlugin::instantiate;
	ldesc->connect_port = DSSIVSTPlugin::connect_port;
	ldesc->activate = DSSIVSTPlugin::activate;
	ldesc->run = DSSIVSTPlugin::run;
	ldesc->run_adding = 0;
	ldesc->set_run_adding_gain = 0;
	ldesc->deactivate = DSSIVSTPlugin::deactivate;
	ldesc->cleanup = DSSIVSTPlugin::cleanup;
	
	descriptor->DSSI_API_Version = 1;
	descriptor->configure = DSSIVSTPlugin::configure;
	descriptor->get_program = DSSIVSTPlugin::get_program;
	descriptor->select_program = DSSIVSTPlugin::select_program;
	descriptor->get_midi_controller_for_port = 0;

       //Andrew Deryabin: VST chunks support
       descriptor->set_custom_data = DSSIVSTPlugin::set_custom_data;
       descriptor->get_custom_data = DSSIVSTPlugin::get_custom_data;
       //Andrew Deryabin: VST chunks support: end code

	if (rec.isSynth) {
	    descriptor->run_synth = DSSIVSTPlugin::run_synth;
	} else {
	    descriptor->run_synth = 0;
	}

	descriptor->run_synth_adding = 0;
	descriptor->run_multiple_synths = 0;
	descriptor->run_multiple_synths_adding = 0;

	m_descriptors.push_back(PluginPair(rec.dllName, descriptor));
    }
}
    
DSSIVSTPlugin::~DSSIVSTPlugin()
{
    for (PluginList::iterator i = m_descriptors.begin(); i != m_descriptors.end(); ++i) {
	DSSIVSTPluginInstance::freeFields(*i->second);
	delete i->second->LADSPA_Plugin;
	delete i->second;
    }
}


DSSI_Descriptor *
DSSIVSTPlugin::queryDescriptor(unsigned long index)
{
    if (index < m_descriptors.size()) {
//	std::cerr << "DSSIVSTPlugin::queryDescriptor: index is " << index
//		  << ", returning " << m_descriptors[index].second->LADSPA_Plugin->Name
//		  << std::endl;
	return m_descriptors[index].second;
    } else {
	return 0;
    }
}

LADSPA_Handle
DSSIVSTPlugin::instantiate(const LADSPA_Descriptor *descriptor,
			 unsigned long sampleRate)
{
    std::cerr << "DSSIVSTPlugin::instantiate(" << descriptor->Label << ")" << std::endl;

    try {
	return (LADSPA_Handle)
	    (new DSSIVSTPluginInstance(descriptor->Label, sampleRate));
    } catch (std::string e) {
	perror(e.c_str());
    } catch (RemotePluginClosedException) {
	std::cerr << "Remote plugin closed." << std::endl;
    }
    return 0;
}

void
DSSIVSTPlugin::connect_port(LADSPA_Handle instance,
			  unsigned long port,
			  LADSPA_Data *location)
{
    ((DSSIVSTPluginInstance *)instance)->connectPort(port, location);
}

void
DSSIVSTPlugin::activate(LADSPA_Handle instance)
{
    ((DSSIVSTPluginInstance *)instance)->activate();
}

void
DSSIVSTPlugin::run(LADSPA_Handle instance, unsigned long sampleCount)
{
    ((DSSIVSTPluginInstance *)instance)->run(sampleCount);
}

void
DSSIVSTPlugin::deactivate(LADSPA_Handle instance)
{
    ((DSSIVSTPluginInstance *)instance)->deactivate();
}

void
DSSIVSTPlugin::cleanup(LADSPA_Handle instance)
{
    std::cerr << "DSSIVSTPlugin::cleanup" << std::endl;
    delete ((DSSIVSTPluginInstance *)instance);
}

const DSSI_Program_Descriptor *
DSSIVSTPlugin::get_program(LADSPA_Handle instance, unsigned long index)
{
    return ((DSSIVSTPluginInstance *)instance)->getProgram(index);
}

void
DSSIVSTPlugin::select_program(LADSPA_Handle instance, unsigned long bank,
			    unsigned long program)
{
    ((DSSIVSTPluginInstance *)instance)->selectProgram(bank, program);
}

void
DSSIVSTPlugin::run_synth(LADSPA_Handle instance, unsigned long sampleCount,
		       snd_seq_event_t *events, unsigned long eventCount)
{
    ((DSSIVSTPluginInstance *)instance)->runSynth(sampleCount, events,
						eventCount);
}

char *
DSSIVSTPlugin::configure(LADSPA_Handle instance, const char *key,
			 const char *value)
{
    std::cerr << "DSSIVSTPlugin::configure(" << key << "," << value << ")" << std::endl;

    std::string rv = ((DSSIVSTPluginInstance *)instance)->configure(key, value);
    if (rv == "") {
	return NULL;
    } else {
	return strdup(rv.c_str());
    }
}


static DSSIVSTPlugin *_plugin = 0;
static std::vector<int> _ladspaDescriptors;

static void
_makeLADSPADescriptorMap()
{
    int i = 0;
    const DSSI_Descriptor *dssiDescriptor = 0;
    while ((dssiDescriptor = dssi_descriptor(i))) {
	if (!dssiDescriptor->run_synth &&
	    !dssiDescriptor->run_synth_adding &&
	    !dssiDescriptor->run_multiple_synths &&
	    !dssiDescriptor->run_multiple_synths_adding) {
	    _ladspaDescriptors.push_back(i);
	}
	++i;
    }
}

extern "C" __attribute__ ((visibility("default")))
const LADSPA_Descriptor *
ladspa_descriptor(unsigned long index)
{
    if (!_plugin) {
	_plugin = new DSSIVSTPlugin;
	_makeLADSPADescriptorMap();
    }
    if (index < _ladspaDescriptors.size()) {
	const DSSI_Descriptor *dssiDescriptor =
	    dssi_descriptor(_ladspaDescriptors[index]);
	if (!dssiDescriptor) return 0;
	return dssiDescriptor->LADSPA_Plugin;
    }
    return 0;
}

extern "C" __attribute__ ((visibility("default")))
const DSSI_Descriptor *
dssi_descriptor(unsigned long index)
{
    if (!_plugin) {
	_plugin = new DSSIVSTPlugin;
	_makeLADSPADescriptorMap();
    }
    return _plugin->queryDescriptor(index);
}

//Andrew Deryabin: VST chunks support
int DSSIVSTPlugin::set_custom_data(LADSPA_Handle Instance, void *Data, unsigned long  DataLength)
{
    DSSIVSTPluginInstance *instance = ((DSSIVSTPluginInstance *)Instance);
    if(DataLength == 0 || Data == 0)
        return 0;
    std::vector<char> chunk;
    for(unsigned long i = 0; i < DataLength; i++)
        chunk.push_back(((char *)Data) [i]);
    instance->m_plugin->setVSTChunk(chunk);
    return 1;
}

int DSSIVSTPlugin::get_custom_data(LADSPA_Handle Instance, void **Data, unsigned long  *DataLength)
{
    DSSIVSTPluginInstance *instance = ((DSSIVSTPluginInstance *)Instance);
    std::vector<char> chunk = instance->m_plugin->getVSTChunk();
    unsigned long chunksize = chunk.size();
    instance->m_chunkdata = new char [chunksize];
    if(instance->m_chunkdata)
    {
        std::vector<char>::pointer ptr = &chunk [0];
        memcpy(instance->m_chunkdata, ptr, chunksize);
        *Data = instance->m_chunkdata;
        *DataLength = chunksize;
        return 1;
    }
    *Data = 0;
    *DataLength = 0;
    return 0;
}
//Andrew Deryabin: VST chunks support: end code
