// -*- c-basic-offset: 4 -*-

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004-2010 Chris Cannam
*/

#include "remotepluginserver.h"

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>

#include <time.h>
#include <iostream>
#include <cstdio>
#include <stdlib.h>
#include <string.h>

#include <errno.h>

RemotePluginServer::RemotePluginServer(std::string fileIdentifiers) :
    m_bufferSize(-1),
    m_numInputs(-1),
    m_numOutputs(-1),
    m_controlRequestFd(-1),
    m_controlResponseFd(-1),
    m_shmFd(-1),
    m_shmControlFd(-1),
    m_controlRequestFileName(0),
    m_controlResponseFileName(0),
    m_shmFileName(0),
    m_shmControlFileName(0),
    m_shm(0),
    m_shmSize(0),
    m_shmControl(0),
    m_inputs(0),
    m_outputs(0)
{
    char tmpFileBase[60];
    
    sprintf(tmpFileBase, "/tmp/rplugin_crq_%s",
	    fileIdentifiers.substr(0, 6).c_str());
    m_controlRequestFileName = strdup(tmpFileBase);

    if ((m_controlRequestFd = open(m_controlRequestFileName, O_RDONLY)) < 0) {
	cleanup();
	throw((std::string)"Failed to open FIFO");
    }
    
    sprintf(tmpFileBase, "/tmp/rplugin_crs_%s",
	    fileIdentifiers.substr(6, 6).c_str());
    m_controlResponseFileName = strdup(tmpFileBase);

    if ((m_controlResponseFd = open(m_controlResponseFileName, O_WRONLY)) < 0) {
	cleanup();
	throw((std::string)"Failed to open FIFO");
    }

    bool b = false;
    sprintf(tmpFileBase, "/dssi-vst-rplugin_shc_%s",
	    fileIdentifiers.substr(12, 6).c_str());
    m_shmControlFileName = strdup(tmpFileBase);

    m_shmControlFd = shm_open(m_shmControlFileName, O_RDWR, 0);
    if (m_shmControlFd < 0) {
        tryWrite(m_controlResponseFd, &b, sizeof(bool));
	cleanup();
	throw((std::string)"Failed to open or create shared memory file");
    }

    m_shmControl = static_cast<ShmControl *>(mmap(0, sizeof(ShmControl), PROT_READ | PROT_WRITE, MAP_SHARED, m_shmControlFd, 0));
    if (!m_shmControl) {
        tryWrite(m_controlResponseFd, &b, sizeof(bool));
        cleanup();
        throw((std::string)"Failed to mmap shared memory file");
    }

    sprintf(tmpFileBase, "/dssi-vst-rplugin_shm_%s",
	    fileIdentifiers.substr(18, 6).c_str());
    m_shmFileName = strdup(tmpFileBase);

    if ((m_shmFd = shm_open(m_shmFileName, O_RDWR, 0)) < 0) {
	tryWrite(m_controlResponseFd, &b, sizeof(bool));
	cleanup();
	throw((std::string)"Failed to open shared memory file");
    }

    b = true;
    tryWrite(m_controlResponseFd, &b, sizeof(bool));
}

RemotePluginServer::~RemotePluginServer()
{
    cleanup();
}

void
RemotePluginServer::cleanup()
{
    if (m_shm) {
	munmap(m_shm, m_shmSize);
	m_shm = 0;
    }
    if (m_shmControl) {
        munmap(m_shmControl, sizeof(ShmControl));
        m_shmControl = 0;
    }
    if (m_controlRequestFd >= 0) {
	close(m_controlRequestFd);
	m_controlRequestFd = -1;
    }
    if (m_controlResponseFd >= 0) {
	close(m_controlResponseFd);
	m_controlResponseFd = -1;
    }
    if (m_shmFd >= 0) {
	close(m_shmFd);
	m_shmFd = -1;
    }
    if (m_shmControlFd >= 0) {
        close(m_shmControlFd);
        m_shmControlFd = -1;
    }
    if (m_controlRequestFileName) {
	free(m_controlRequestFileName);
	m_controlRequestFileName = 0;
    }
    if (m_controlResponseFileName) {
	free(m_controlResponseFileName);
	m_controlResponseFileName = 0;
    }
    if (m_shmFileName) {
	free(m_shmFileName);
	m_shmFileName = 0;
    }
    if (m_shmControlFileName) {
        free(m_shmControlFileName);
        m_shmControlFileName = 0;
    }

    delete m_inputs;
    m_inputs = 0;

    delete m_outputs;
    m_outputs = 0;
}

void
RemotePluginServer::sizeShm()
{
    if (m_numInputs < 0 || m_numOutputs < 0 || m_bufferSize < 0) return;

    delete m_inputs;
    delete m_outputs;
    m_inputs = 0;
    m_outputs = 0;

    size_t sz = (m_numInputs + m_numOutputs) * m_bufferSize * sizeof(float);

    if (m_shm) {
	m_shm = (char *)mremap(m_shm, m_shmSize, sz, MREMAP_MAYMOVE);
    } else {
	m_shm = (char *)mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, m_shmFd, 0);
    }
    if (!m_shm) {
	std::cerr << "RemotePluginServer::sizeShm: ERROR: mmap or mremap for failed for " << sz
		  << " bytes from fd " << m_shmFd << "!" << std::endl;
	m_shmSize = 0;
    } else {
	m_shmSize = sz;
	if (m_numInputs > 0) {
	    m_inputs = new float*[m_numInputs];
	}
	if (m_numOutputs > 0) {
	    m_outputs = new float*[m_numOutputs];
	}
	std::cerr << "sized shm to " << sz << ", " << m_numInputs << " inputs and " << m_numOutputs << " outputs" << std::endl;
    }
}    

void
RemotePluginServer::dispatchControl(int timeout)
{
    struct pollfd pfd;
    
    pfd.fd = m_controlRequestFd;
    pfd.events = POLLIN | POLLPRI | POLLERR | POLLHUP | POLLNVAL;

    if (poll(&pfd, 1, timeout) < 0) {
	throw RemotePluginClosedException();
    }
    
    if ((pfd.revents & POLLIN) || (pfd.revents & POLLPRI)) {
	dispatchControlEvents();
    } else if (pfd.revents) {
	throw RemotePluginClosedException();
    }
}

void
RemotePluginServer::dispatchProcess(int timeout)
{
    timespec ts_timeout;
    clock_gettime(CLOCK_REALTIME, &ts_timeout);
    time_t seconds = timeout / 1000;
    ts_timeout.tv_sec += seconds;
    ts_timeout.tv_nsec += (timeout - seconds * 1000) * 1000000;
    if (ts_timeout.tv_nsec >= 1000000000) {
        ts_timeout.tv_nsec -= 1000000000;
        ts_timeout.tv_sec++;
    }

    if (sem_timedwait(&m_shmControl->runServer, &ts_timeout)) {
        if (errno == ETIMEDOUT) {
            return;
        } else {
            throw RemotePluginClosedException();
        }
    }

    while (dataAvailable(&m_shmControl->ringBuffer)) {
        dispatchProcessEvents();
    }

    if (sem_post(&m_shmControl->runClient)) {
        std::cerr << "Could not post to semaphore\n";
    }
}

void
RemotePluginServer::dispatchProcessEvents()
{    
    RemotePluginOpcode opcode = RemotePluginNoOpcode;

    tryRead(&m_shmControl->ringBuffer, &opcode, sizeof(RemotePluginOpcode));

//    std::cerr << "read opcode: " << opcode << std::endl;

    switch (opcode) {

    case RemotePluginProcess:
    {
	if (m_bufferSize < 0) {
	    std::cerr << "ERROR: RemotePluginServer: buffer size must be set before process" << std::endl;
	    return;
	}
	if (m_numInputs < 0) {
	    std::cerr << "ERROR: RemotePluginServer: input count must be tested before process" << std::endl;
	    return;
	}
	if (m_numOutputs < 0) {
	    std::cerr << "ERROR: RemotePluginServer: output count must be tested before process" << std::endl;
	    return;
	}
	if (!m_shm) {
	    sizeShm();
	    if (!m_shm) {
		std::cerr << "ERROR: RemotePluginServer: no shared memory region available" << std::endl;
		return;
	    }
	}

//	std::cerr << "server process: entering" << std::endl;

	size_t blocksz = m_bufferSize * sizeof(float);

	for (int i = 0; i < m_numInputs; ++i) {
	    m_inputs[i] = (float *)(m_shm + i * blocksz);
	}
	for (int i = 0; i < m_numOutputs; ++i) {
	    m_outputs[i] = (float *)(m_shm + (i + m_numInputs) * blocksz);
	}

	process(m_inputs, m_outputs);

//	std::cerr << "server process: written" << std::endl;
	break;
    }
	
    case RemotePluginSetParameter:
    {
        int pn(readInt(&m_shmControl->ringBuffer));
        setParameter(pn, readFloat(&m_shmControl->ringBuffer));
	break;
    }

    case RemotePluginSetCurrentProgram:
	setCurrentProgram(readInt(&m_shmControl->ringBuffer));
	break;

    case RemotePluginSendMIDIData:
    {
	int events = 0;
	int *frameoffsets = 0;
	unsigned char *data = readMIDIData(&m_shmControl->ringBuffer, &frameoffsets, events);
	if (events && data && frameoffsets) {
//    std::cerr << "RemotePluginServer::sendMIDIData(" << events << ")" << std::endl;

	    sendMIDIData(data, frameoffsets, events);
	}
	break;
    }

    case RemotePluginSetBufferSize:
    {
	int newSize = readInt(&m_shmControl->ringBuffer);
	setBufferSize(newSize);
	m_bufferSize = newSize;
	break;
    }

    case RemotePluginSetSampleRate:
	setSampleRate(readInt(&m_shmControl->ringBuffer));
	break;

    default:
	std::cerr << "WARNING: RemotePluginServer::dispatchProcessEvents: unexpected opcode "
		  << opcode << std::endl;
    }
}

void
RemotePluginServer::dispatchControlEvents()
{    
    RemotePluginOpcode opcode = RemotePluginNoOpcode;
    static float *parameterBuffer = 0;

    tryRead(m_controlRequestFd, &opcode, sizeof(RemotePluginOpcode));

    switch (opcode) {

    case RemotePluginGetVersion:
	writeFloat(m_controlResponseFd, getVersion());
	break;

    case RemotePluginGetName:
	writeString(m_controlResponseFd, getName());
	break;

    case RemotePluginGetMaker:
	writeString(m_controlResponseFd, getMaker());
	break;
    
    case RemotePluginTerminate:
	terminate();
	break;
    
    case RemotePluginGetInputCount:
	m_numInputs = getInputCount();
	writeInt(m_controlResponseFd, m_numInputs);
	break;

    case RemotePluginGetOutputCount:
	m_numOutputs = getOutputCount();
	writeInt(m_controlResponseFd, m_numOutputs);
	break;

    case RemotePluginGetParameterCount:
	writeInt(m_controlResponseFd, getParameterCount());
	break;
	
    case RemotePluginGetParameterName:
	writeString(m_controlResponseFd, getParameterName(readInt(m_controlRequestFd)));
	break;
    
    case RemotePluginGetParameter:
	writeFloat(m_controlResponseFd, getParameter(readInt(m_controlRequestFd)));
	break;
    
    case RemotePluginGetParameterDefault:
	writeFloat(m_controlResponseFd, getParameterDefault(readInt(m_controlRequestFd)));
	break;

    case RemotePluginGetParameters:
    {
	if (!parameterBuffer) {
	    parameterBuffer = new float[getParameterCount()];
	}
	int p0 = readInt(m_controlRequestFd);
	int pn = readInt(m_controlRequestFd);
	getParameters(p0, pn, parameterBuffer);
	tryWrite(m_controlResponseFd, parameterBuffer, (pn - p0 + 1) * sizeof(float));
	break;
    }

    case RemotePluginHasMIDIInput:
    {
	bool m = hasMIDIInput();
	tryWrite(m_controlResponseFd, &m, sizeof(bool));
	break;
    }
       
    case RemotePluginGetProgramCount:
	writeInt(m_controlResponseFd, getProgramCount());
	break;

    case RemotePluginGetProgramName:
	writeString(m_controlResponseFd, getProgramName(readInt(m_controlRequestFd)));
	break;

    case RemotePluginIsReady:
    {
	if (!m_shm) sizeShm();
	bool b(isReady());
	std::cerr << "isReady: returning " << b << std::endl;
	tryWrite(m_controlResponseFd, &b, sizeof(bool));
    }

    case RemotePluginSetDebugLevel:
    {
	RemotePluginDebugLevel newLevel = m_debugLevel;
	tryRead(m_controlRequestFd, &newLevel, sizeof(RemotePluginDebugLevel));
	setDebugLevel(newLevel);
	m_debugLevel = newLevel;
	break;
    }

    case RemotePluginWarn:
    {
	bool b = warn(readString(m_controlRequestFd));
	tryWrite(m_controlResponseFd, &b, sizeof(bool));
	break;
    }

    case RemotePluginShowGUI:
    {
	showGUI(readString(m_controlRequestFd));
	break;
    }

    case RemotePluginHideGUI:
    {
	hideGUI();
	break;
    }

    //Deryabin Andrew: vst chunks support
    case RemotePluginGetVSTChunk:
    {
        std::vector<char> chunk = getVSTChunk();
        writeRaw(m_controlResponseFd, chunk);
        break;
    }

    case RemotePluginSetVSTChunk:
    {
        std::vector<char> chunk = readRaw(m_controlRequestFd);
        setVSTChunk(chunk);
        break;
    }
    //Deryabin Andrew: vst chunks support: end code

    case RemotePluginNoOpcode:
	break;

    case RemotePluginReset:
        reset();
        break;

    default:
	std::cerr << "WARNING: RemotePluginServer::dispatchControlEvents: unexpected opcode "
		  << opcode << std::endl;
    }
}


