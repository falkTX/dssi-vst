// -*- c-basic-offset: 4 -*-

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004 Chris Cannam
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

#include "rdwrops.h"

RemotePluginServer::RemotePluginServer(std::string fileIdentifiers) :
    m_bufferSize(-1),
    m_numInputs(-1),
    m_numOutputs(-1),
    m_controlRequestFd(-1),
    m_controlResponseFd(-1),
    m_processFd(-1),
    m_shmFd(-1),
    m_controlRequestFileName(0),
    m_controlResponseFileName(0),
    m_processFileName(0),
    m_shmFileName(0),
    m_shm(0),
    m_shmSize(0),
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
    
    sprintf(tmpFileBase, "/tmp/rplugin_prc_%s",
	    fileIdentifiers.substr(12, 6).c_str());
    m_processFileName = strdup(tmpFileBase);

    if ((m_processFd = open(m_processFileName, O_RDONLY)) < 0) {
	cleanup();
	throw((std::string)"Failed to open FIFO");
    }
    
    sprintf(tmpFileBase, "/tmp/rplugin_shm_%s",
	    fileIdentifiers.substr(18, 6).c_str());
    m_shmFileName = strdup(tmpFileBase);

    bool b = false;

    if ((m_shmFd = open(m_shmFileName, O_RDWR)) < 0) {
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
    if (m_controlRequestFd >= 0) {
	close(m_controlRequestFd);
	m_controlRequestFd = -1;
    }
    if (m_controlResponseFd >= 0) {
	close(m_controlResponseFd);
	m_controlResponseFd = -1;
    }
    if (m_processFd >= 0) {
	close(m_processFd);
	m_processFd = -1;
    }
    if (m_shmFd >= 0) {
	close(m_shmFd);
	m_shmFd = -1;
    }
    if (m_controlRequestFileName) {
	free(m_controlRequestFileName);
	m_controlRequestFileName = 0;
    }
    if (m_controlResponseFileName) {
	free(m_controlResponseFileName);
	m_controlResponseFileName = 0;
    }
    if (m_processFileName) {
	free(m_processFileName);
	m_processFileName = 0;
    }
    if (m_shmFileName) {
	free(m_shmFileName);
	m_shmFileName = 0;
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
RemotePluginServer::dispatch()
{
    struct pollfd pfd[2];
    
    pfd[0].fd = m_controlRequestFd;
    pfd[1].fd = m_processFd;
    pfd[0].events = pfd[1].events = POLLIN | POLLERR | POLLHUP;

    if (poll(pfd, 2, -1) < 0) {
	throw RemotePluginClosedException();
    }
    
    if (pfd[0].revents & POLLIN) {
	dispatchControl();
    }
    
    if (pfd[1].revents & POLLIN) {
	dispatchProcess();
    }
}

void
RemotePluginServer::dispatchProcess()
{    
    RemotePluginOpcode opcode = RemotePluginNoOpcode;

    tryRead(m_processFd, &opcode, sizeof(RemotePluginOpcode));

//    std::cerr << "read opcode: " << opcode << std::endl;

    if (opcode == RemotePluginProcess) {

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

    } else if (opcode == RemotePluginSetCurrentProgram) {

	setCurrentProgram(readInt(m_processFd));

    } else if (opcode == RemotePluginSendMIDIData) {

	int events = 0;
	int *frameoffsets = 0;
	unsigned char *data = readMIDIData(m_processFd, &frameoffsets, events);
	if (events && data && frameoffsets) {
    std::cerr << "RemotePluginServer::sendMIDIData(" << events << ")" << std::endl;

	    sendMIDIData(data, frameoffsets, events);
	}
    }
}

void
RemotePluginServer::dispatchControl()
{    
    RemotePluginOpcode opcode = RemotePluginNoOpcode;

    tryRead(m_controlRequestFd, &opcode, sizeof(RemotePluginOpcode));

    switch (opcode) {

    case RemotePluginProcess:
    case RemotePluginSetCurrentProgram:
    case RemotePluginSendMIDIData:
	std::cerr << "WARNING: RemotePluginServer: got opcode " << opcode
		  << " from control fd, should be a process fd opcode" << std::endl;
	break;

    case RemotePluginGetVersion:
	writeFloat(m_controlResponseFd, getVersion());
	break;

    case RemotePluginGetName:
	writeString(m_controlResponseFd, getName());
	break;

    case RemotePluginGetMaker:
	writeString(m_controlResponseFd, getMaker());
	break;

    case RemotePluginSetBufferSize:
    {
	int newSize = readInt(m_controlRequestFd);
	setBufferSize(newSize);
	m_bufferSize = newSize;
	break;
    }

    case RemotePluginSetSampleRate:
	setSampleRate(readInt(m_controlRequestFd));
	break;
    
    case RemotePluginReset:
	reset();
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
	
    case RemotePluginSetParameter:
    {
	int pn(readInt(m_controlRequestFd));
	setParameter(pn, readFloat(m_controlRequestFd));
	break;
    }
    
    case RemotePluginGetParameter:
	writeFloat(m_controlResponseFd, getParameter(readInt(m_controlRequestFd)));
	break;
    
    case RemotePluginGetParameterDefault:
	writeFloat(m_controlResponseFd, getParameterDefault(readInt(m_controlRequestFd)));
	break;

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

    case RemotePluginNoOpcode:
	break;

    default:
	std::cerr << "WARNING: RemotePluginServer: unknown opcode "
		  << opcode << std::endl;
    }
}

