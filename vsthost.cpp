/* -*- c-basic-offset: 4 -*- */

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004-2010 Chris Cannam
*/

#include <ctype.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <sys/time.h>

#include <alsa/asoundlib.h>
#include <alsa/seq.h>
#include <jack/jack.h>

#include "remotevstclient.h"

static RemotePluginClient *plugin = 0;

#define MIDI_DECODED_SIZE 3
#define MIDI_BUFFER_SIZE 3072

static unsigned char midiStreamBuffer[(MIDI_BUFFER_SIZE + 1) * MIDI_DECODED_SIZE];
static struct timeval midiTimeBuffer[MIDI_BUFFER_SIZE];
static int midiFrameOffsets[MIDI_BUFFER_SIZE];
static int midiReadIndex = 0, midiWriteIndex = 0;

static snd_midi_event_t *alsaDecoder = 0;

static pthread_mutex_t pluginMutex = PTHREAD_MUTEX_INITIALIZER;

static bool ready = false;
static bool exiting = false;

static snd_seq_t *alsaSeqHandle = 0;

struct JackData {
    jack_client_t *client;
    int            input_count;
    int            output_count;
    jack_port_t  **input_ports;
    jack_port_t  **output_ports;
    float        **input_buffers;
    float        **output_buffers;
    jack_nframes_t sample_rate;
    jack_nframes_t buffer_size;
};

static JackData jackData;

void closeJack();


void
bail(int sig)
{
    static bool bailing = false;

    // can't call pthread_mutex functions safely from a signal handler
    // -- this means we don't get a mutex lock in closeJack either.
    // Do this safer but still racy thing:

    if (bailing) return;
    bailing = true;

    if (sig != 0) {
	fprintf(stderr, "vsthost: signal %d received, exiting\n", sig);
    } else {
	fprintf(stderr, "vsthost: bailing out\n");
    }

    if (jackData.client) {
	closeJack();
    }

    if (alsaSeqHandle) {
	snd_seq_close(alsaSeqHandle);
	alsaSeqHandle = 0;
    }

    if (plugin) {
	delete plugin;
    }

    // ignore term signals, then send one to the process group
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, 0);
    kill(0, SIGTERM);

    exit(1);
}

int
bufferSizeChanged(jack_nframes_t nframes, void *i)
{
    if (nframes == jackData.buffer_size) return 0;

    pthread_mutex_lock(&pluginMutex);

    jackData.buffer_size = nframes;
    plugin->setBufferSize(nframes);

    pthread_mutex_unlock(&pluginMutex);
    return 0;
}

int
sampleRateChanged(jack_nframes_t nframes, void *i)
{
    if (nframes == jackData.sample_rate) return 0;

    pthread_mutex_lock(&pluginMutex);

    jackData.sample_rate = nframes;
    plugin->setSampleRate(nframes);

    pthread_mutex_unlock(&pluginMutex);
    return 0;
}

void
alsaSeqCallback(snd_seq_t *alsaSeqHandle)
{
    snd_seq_event_t *ev = 0;

    if (!ready) return;

    do {

	if (snd_seq_event_input(alsaSeqHandle, &ev) > 0) {

	    if (ev->type >= SND_SEQ_EVENT_ECHO) {
		fprintf(stderr, "Ignoring incoming sequencer event of type %d\n",
			ev->type);
		continue;
	    }

	    if (midiReadIndex == midiWriteIndex + 1) {
		fprintf(stderr, "WARNING: MIDI stream buffer overflow\n");
		continue;
	    }

	    long count = snd_midi_event_decode
		(alsaDecoder,
		 midiStreamBuffer + (midiWriteIndex * MIDI_DECODED_SIZE),
		 (MIDI_BUFFER_SIZE - midiWriteIndex) * MIDI_DECODED_SIZE,
		 ev);

	    if (count > 0 && count <= 3) {

		while (count < 3) {
		    midiStreamBuffer[midiWriteIndex * MIDI_DECODED_SIZE + count]
			= '\0';
		    ++count;
		}

		gettimeofday(midiTimeBuffer + midiWriteIndex, NULL);
		midiWriteIndex = (midiWriteIndex + 1) % MIDI_BUFFER_SIZE;
		
	    } else if (count > 3) {
		fprintf(stderr, "WARNING: MIDI event of type %d"
			" decoded to >3 bytes, discarding\n", ev->type);
	    } else if (count == 0) {
		fprintf(stderr, "WARNING: MIDI event of type %d"
			" decoded to zero bytes, discarding\n", ev->type);
	    } else { // count < 0
		fprintf(stderr, "WARNING: MIDI decoder error %ld"
			" for event type %d\n", count, ev->type);
	    }
	}
	
    } while (snd_seq_event_input_pending(alsaSeqHandle, 0) > 0);
}

int
openAlsaSeq(const char *pluginName)
{
    int portid;
    char alsaName[75];

    if (pluginName[0]) {
	sprintf(alsaName, "%s VST", pluginName);
    } else {
	sprintf(alsaName, "VST Host");
    }

    alsaSeqHandle = 0;

    if (snd_seq_open(&alsaSeqHandle, "hw", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
	fprintf(stderr, "ERROR: Failed to open ALSA sequencer interface\n");
	return 1;
    }

    snd_seq_set_client_name(alsaSeqHandle, alsaName);

    if ((portid = snd_seq_create_simple_port
	 (alsaSeqHandle, alsaName,
	  SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
	  SND_SEQ_PORT_TYPE_APPLICATION)) < 0) {
	fprintf(stderr, "ERROR: Failed to create ALSA sequencer port\n");
	return 1;
    }

    snd_midi_event_new(MIDI_BUFFER_SIZE, &alsaDecoder);
    if (!alsaDecoder) {
	fprintf(stderr, "ERROR: Failed to initialize ALSA MIDI decoder\n");
	return 1;
    }
    snd_midi_event_no_status(alsaDecoder, 1);

    return 0;
}

int
jackProcess(jack_nframes_t nframes, void *arg)
{
    int ri = midiReadIndex, wi = midiWriteIndex;

    if (nframes != jackData.buffer_size) {
	// This is apparently legal, though it will never happen with
	// current JACK versions.  In theory nframes can be anywhere
	// in the range 0 -> buffersize.  Let's not handle that yet.
	fprintf(stderr, "ERROR: Internal JACK error: process() called with incorrect buffer size (was %d, should be %d)\n", nframes, jackData.buffer_size);
	return 0;
    }

    if (sizeof(float) != sizeof(jack_default_audio_sample_t)) {
	fprintf(stderr, "ERROR: The JACK audio sample type is not \"float\"; can't proceed\n");
	exiting = true;
	return 0;
    }

    for (int i = 0; i < jackData.input_count; ++i) {
	jackData.input_buffers[i] = (float *)jack_port_get_buffer
	    (jackData.input_ports[i], jackData.buffer_size);
    }
    for (int i = 0; i < jackData.output_count; ++i) {
	jackData.output_buffers[i] = (float *)jack_port_get_buffer
	    (jackData.output_ports[i], jackData.buffer_size);
    }

    if (exiting) {
	return 0;
    }

    if (!ready || pthread_mutex_trylock(&pluginMutex)) {
	for (int i = 0; i < jackData.output_count; ++i) {
	    memset(jackData.output_buffers[i], 0, jackData.buffer_size * sizeof(float));
	}
	return 0;
    }

    struct timeval tv, diff;
    long framediff;
    jack_nframes_t rate;

    gettimeofday(&tv, NULL);
    rate = jack_get_sample_rate(jackData.client);

    while (ri != wi) {

	diff.tv_sec = tv.tv_sec - midiTimeBuffer[ri].tv_sec;

	if (tv.tv_usec < midiTimeBuffer[ri].tv_usec) {
	    --diff.tv_sec;
	    diff.tv_usec = tv.tv_usec + 1000000 - midiTimeBuffer[ri].tv_usec;
	} else {
	    diff.tv_usec = tv.tv_usec - midiTimeBuffer[ri].tv_usec;
	}

	framediff =
	    diff.tv_sec * rate +
	    ((diff.tv_usec / 1000) * rate) / 1000 +
	    ((diff.tv_usec - 1000 * (diff.tv_usec / 1000)) * rate) / 1000000;

	if (framediff >= (long)nframes) framediff = nframes - 1;
	else if (framediff < 0) framediff = 0;

	midiFrameOffsets[ri] = (int)(nframes - framediff - 1);
	ri = (ri + 1) % MIDI_BUFFER_SIZE;
    }

    if (midiReadIndex > wi) {
	plugin->sendMIDIData(midiStreamBuffer + midiReadIndex * MIDI_DECODED_SIZE,
			     midiFrameOffsets + midiReadIndex,
			     MIDI_BUFFER_SIZE - midiReadIndex);
	midiReadIndex = 0;
    }

    if (midiReadIndex < wi) {
	plugin->sendMIDIData(midiStreamBuffer + midiReadIndex * MIDI_DECODED_SIZE,
			     midiFrameOffsets + midiReadIndex,
			     wi - midiReadIndex);
	midiReadIndex = wi;
    }	

    try {
	plugin->process(jackData.input_buffers, jackData.output_buffers);
    } catch (RemotePluginClosedException) {
	pthread_mutex_unlock(&pluginMutex);
	exiting = true;
	return 0;
    }

    pthread_mutex_unlock(&pluginMutex);
    return 0;      
}

void
shutdownJack(void *arg)
{
    jackData.client = 0;
    bail(0);
}

int
openJack(const char *pluginName)
{
    const char **ports = 0;
    char jackName[26];
    char tmpbuf[21];
    int i = 0, j = 0;

    for (i = 0; i < 20 && pluginName[i]; ++i) {
	if (isalpha(pluginName[i])) {
	    tmpbuf[j] = tolower(pluginName[i]);
	    ++j;
	}
    }
    tmpbuf[j] = '\0';
    snprintf(jackName, 26, "vst_%s", tmpbuf);

    if ((jackData.client = jack_client_open(jackName, JackNullOption, NULL)) == 0) {
	fprintf(stderr, "ERROR: Failed to connect to JACK server -- jackd not running?\n");
	return 1;
    }

    jack_set_process_callback(jackData.client, jackProcess, 0);
    jack_on_shutdown(jackData.client, shutdownJack, 0);
  
    jackData.sample_rate = jack_get_sample_rate(jackData.client);
    jackData.buffer_size = jack_get_buffer_size(jackData.client);

    plugin->setSampleRate(jackData.sample_rate);
    plugin->setBufferSize(jackData.buffer_size);

    ports = jack_get_ports
	 (jackData.client, NULL, NULL, JackPortIsPhysical | JackPortIsInput);

    jackData.input_count = plugin->getInputCount();
    jackData.output_count = plugin->getOutputCount();

    static char portName[100];

    if (jackData.input_count > 0) {

	jackData.input_ports = new jack_port_t*[jackData.input_count];
	jackData.input_buffers = new float*[jackData.input_count];

	for (int i = 0; i < jackData.input_count; ++i) {
	    jackData.input_buffers[i] = 0;
	    snprintf(portName, 100, "in_%d", i+1);
	    jackData.input_ports[i] = jack_port_register
		(jackData.client, portName,
		 JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	}

    } else {
	jackData.input_ports = 0;
	jackData.input_buffers = 0;
    }

    if (jackData.output_count > 0) {

	jackData.output_ports = new jack_port_t*[jackData.output_count];
	jackData.output_buffers = new float*[jackData.output_count];

	for (int i = 0; i < jackData.output_count; ++i) {
	    jackData.output_buffers[i] = 0;
	    snprintf(portName, 100, "out_%d", i+1);
	    jackData.output_ports[i] = jack_port_register
		(jackData.client, portName,
		 JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
	}

    } else {
	jackData.output_ports = 0;
	jackData.output_buffers = 0;
    }

    jack_set_sample_rate_callback(jackData.client, sampleRateChanged, 0);
    jack_set_buffer_size_callback(jackData.client, bufferSizeChanged, 0);

    if (jack_activate(jackData.client)) {
	fprintf(stderr, "ERROR: Failed to activate JACK client -- some internal error?\n");
	return 1;
    }
    
    bool portsLeft = true;
    for (int i = 0; i < jackData.output_count; ++i) {
	if (portsLeft) {
	    if (ports && ports[i]) {
		if (jack_connect
		    (jackData.client,
		     jack_port_name(jackData.output_ports[i]),
		     ports[i])) {
		    fprintf(stderr, "WARNING: Failed to connect output port %d\n", i);
		}
	    } else {
		portsLeft = false;
	    }
	}
    }

    free(ports);
    return 0;
}

void
closeJack()
{
    if (!jackData.client) return;

    for (int i = 0; i < jackData.input_count; ++i) {
	jack_port_unregister(jackData.client, jackData.input_ports[i]);
    }

    for (int i = 0; i < jackData.output_count; ++i) {
	jack_port_unregister(jackData.client, jackData.output_ports[i]);
    }

    jack_client_close(jackData.client);

    jackData.client = 0;
}

void
usage()
{
    fprintf(stderr, "Usage: vsthost [-n] [-d<x>] <dll>\n    -n  No GUI\n    -d  Debug level (0, 1, 2 or 3)\n");
    exit(2);
}    

int
main(int argc, char **argv)
{
    char *dllname = 0;
    int   debugLevel = 0;
    bool  gui = true;

    int npfd;
    struct pollfd *pfd;

    while (1) {
	int c = getopt(argc, argv, "nd:");
	
	if (c == -1) break;
	else if (c == 'n') {
	    gui = false;
	} else if (c == 'd') {
	    int dl = atoi(optarg);
	    if (dl >= 0 && dl < 4) debugLevel = dl;
	    else {
		usage();
	    }
	} else {
	    usage();
	}
    }

    if (optind >= argc) usage();

    dllname = argv[optind];

    if (!dllname) usage();

    setsid();

    struct sigaction sa;
    sa.sa_handler = bail;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGHUP,  &sa, 0);
    sigaction(SIGQUIT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGPIPE, &sa, 0);

    jackData.client = 0;

    try {
	plugin = new RemoteVSTClient(dllname, gui);
    } catch (std::string e) {
	perror(e.c_str());
	bail(0);
    }

    std::string pluginName = plugin->getName();

    // prevent child threads from wanting to handle signals
    sigset_t _signals;
    sigemptyset(&_signals);
    sigaddset(&_signals, SIGHUP);
    sigaddset(&_signals, SIGINT);
    sigaddset(&_signals, SIGQUIT);
    sigaddset(&_signals, SIGPIPE);
    sigaddset(&_signals, SIGTERM);
    sigaddset(&_signals, SIGUSR1);
    sigaddset(&_signals, SIGUSR2);
    sigaddset(&_signals, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &_signals, 0);

    bool hasMIDI = plugin->hasMIDIInput();

    if (hasMIDI) {
	if (openAlsaSeq(pluginName.c_str())) {
	    plugin->warn("Failed to connect to ALSA sequencer MIDI interface");
	    bail(0);
	}
    }
    if (openJack(pluginName.c_str())) {
	plugin->warn("Failed to connect to JACK audio server (jackd not running?)");
	bail(0);
    }

    // restore signal handling
    pthread_sigmask(SIG_UNBLOCK, &_signals, 0);

    ready = true;

    if (hasMIDI) {

	npfd = snd_seq_poll_descriptors_count(alsaSeqHandle, POLLIN);
	pfd = (struct pollfd *)alloca(npfd * sizeof(struct pollfd));
	snd_seq_poll_descriptors(alsaSeqHandle, pfd, npfd, POLLIN);
	
	while (1) {
	    if (poll(pfd, npfd, 1000) > 0) {
		alsaSeqCallback(alsaSeqHandle);
	    }
	    if (exiting) bail(0);
	}
    } else {

	while (1) {
	    sleep (1);
	    if (exiting) bail(0);
	}
    }
}
