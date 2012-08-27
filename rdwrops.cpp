

// -*- c-basic-offset: 4 -*-

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004-2010 Chris Cannam
*/

#include "rdwrops.h"

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <zlib.h>
#include <cstdio>
#include <iostream>

//#define DEBUG_RDWR 1

void
rdwr_tryRead(int fd, void *buf, size_t count, const char *file, int line)
{
    ssize_t r = 0;

    while ((r = read(fd, buf, count)) < (ssize_t)count) {

	if (r == 0) {
	    // end of file
	    throw RemotePluginClosedException();
	} else if (r < 0) {
	    if (errno != EAGAIN) {
		char message[100];
		sprintf(message, "Read failed on fd %d at %s:%d", fd, file, line);
		perror(message);
		throw RemotePluginClosedException();
	    }
	    r = 0;
	}

	buf = (void *)(((char *)buf) + r);
	count -= r;

	if (count > 0) {
	    usleep(20000);
	}
    }

#ifdef DEBUG_RDWR
    if (r >= count) {
	fprintf(stderr, "read succeeded at %s:%d (%d bytes)\n",
		file, line, r);
    }
#endif
}

void
rdwr_tryWrite(int fd, const void *buf, size_t count, const char *file, int line)
{
    ssize_t w = write(fd, buf, count);

    if (w < 0) {
	char message[100];
	sprintf(message, "Write failed on fd %d at %s:%d", fd, file, line);
	perror(message);
	throw RemotePluginClosedException();
    }

    if (w < (ssize_t)count) {
	fprintf(stderr, "Failed to complete write on fd %d (have %d, put %d) at %s:%d\n",
		fd, (int)count, (int)w, file, line);
	throw RemotePluginClosedException();
    }

#ifdef DEBUG_RDWR
    fprintf(stderr, "write succeeded at %s:%d (%d bytes)\n",
	    file, line, w);
#endif
}

void
rdwr_tryRead(RingBuffer *ringbuf, void *buf, size_t count, const char *file, int line)
{
    char *charbuf = static_cast<char *>(buf);
    size_t tail = ringbuf->tail;
    size_t head = ringbuf->head;
    size_t wrap = 0;

    if (head <= tail) {
        wrap = SHM_RING_BUFFER_SIZE;
    }
    if (head - tail + wrap < count) {
        throw RemotePluginClosedException();
    }
    size_t readto = tail + count;
    if (readto >= SHM_RING_BUFFER_SIZE) {
        readto -= SHM_RING_BUFFER_SIZE;
        size_t firstpart = SHM_RING_BUFFER_SIZE - tail;
        memcpy(charbuf, ringbuf->buf + tail, firstpart);
        memcpy(charbuf + firstpart, ringbuf->buf, readto);
    } else {
        memcpy(charbuf, ringbuf->buf + tail, count);
    }
    ringbuf->tail = readto;
}

void
rdwr_tryWrite(RingBuffer *ringbuf, const void *buf, size_t count, const char *file, int line)
{
    const char *charbuf = static_cast<const char *>(buf);
    size_t written = ringbuf->written;
    size_t tail = ringbuf->tail;
    size_t wrap = 0;
    if (tail <= written) {
        wrap = SHM_RING_BUFFER_SIZE;
    }
    if (tail - written + wrap < count) {
        std::cerr << "Operation ring buffer full! Dropping events." << std::endl;
        ringbuf->invalidateCommit = true;
        return;
    }

    size_t writeto = written + count;
    if (writeto >= SHM_RING_BUFFER_SIZE) {
        writeto -= SHM_RING_BUFFER_SIZE;
        size_t firstpart = SHM_RING_BUFFER_SIZE - written;
        memcpy(ringbuf->buf + written, charbuf, firstpart);
        memcpy(ringbuf->buf, charbuf + firstpart, writeto);
    } else {
        memcpy(ringbuf->buf + written, charbuf, count);
    }
    ringbuf->written = writeto;
}

void
rdwr_commitWrite(RingBuffer *ringbuf, const char *file, int line)
{
    if (ringbuf->invalidateCommit) {
        ringbuf->written = ringbuf->head;
        ringbuf->invalidateCommit = false;
    } else {
        ringbuf->head = ringbuf->written;
    }
}

bool dataAvailable(RingBuffer *ringbuf)
{
    return ringbuf->tail != ringbuf->head;
}

template <typename T> void
rdwr_writeOpcode(T fd, RemotePluginOpcode opcode, const char *file, int line)
{
    rdwr_tryWrite(fd, &opcode, sizeof(RemotePluginOpcode), file, line);
}

template <typename T> void
rdwr_writeString(T fd, const std::string &str, const char *file, int line)
{
    int len = str.length();
    rdwr_tryWrite(fd, &len, sizeof(int), file, line);
    rdwr_tryWrite(fd, str.c_str(), len, file, line);
}

template <typename T> std::string
rdwr_readString(T fd, const char *file, int line)
{
    int len;
    static char *buf = 0;
    static int bufLen = 0;
    rdwr_tryRead(fd, &len, sizeof(int), file, line);
    if (len + 1 > bufLen) {
	delete buf;
	buf = new char[len + 1];
	bufLen = len + 1;
    }
    rdwr_tryRead(fd, buf, len, file, line);
    buf[len] = '\0';
    return std::string(buf);
}

template <typename T> void
rdwr_writeInt(T fd, int i, const char *file, int line)
{
    rdwr_tryWrite(fd, &i, sizeof(int), file, line);
}

template <typename T> int
rdwr_readInt(T fd, const char *file, int line)
{
    int i = 0;
    rdwr_tryRead(fd, &i, sizeof(int), file, line);
    return i;
}

template <typename T> void
rdwr_writeFloat(T fd, float f, const char *file, int line)
{
    rdwr_tryWrite(fd, &f, sizeof(float), file, line);
}

template <typename T> float
rdwr_readFloat(T fd, const char *file, int line)
{
    float f = 0;
    rdwr_tryRead(fd, &f, sizeof(float), file, line);
    return f;
}

template <typename T> unsigned char *
rdwr_readMIDIData(T fd, int **frameoffsets, int &events, const char *file, int line)
{
    static unsigned char buf[MIDI_BUFFER_SIZE * 3];
    static int frameoffbuf[MIDI_BUFFER_SIZE];

    rdwr_tryRead(fd, &events, sizeof(int), file, line);

    rdwr_tryRead(fd, buf, events * 3, file, line);
    rdwr_tryRead(fd, frameoffbuf, events * sizeof(int), file, line);

    if (frameoffsets) *frameoffsets = frameoffbuf;
    return buf;
}

//Deryabin Andrew: vst chunks support
template <typename T> void
rdwr_writeRaw(T fd, std::vector<char> rawdata, const char *file, int line)
{
    unsigned long complen = compressBound(rawdata.size());
    char *compressed = new char [complen];
    if(!compressed)
    {
        fprintf(stderr, "Failed to allocate %lu bytes of memory at %s:%d\n", complen, file, line);
        throw RemotePluginClosedException();
    }

    std::vector<char>::pointer ptr = &rawdata [0];

    if(compress2((Bytef *)compressed, &complen, (Bytef *)ptr, rawdata.size(), 9) != Z_OK)
    {
        delete compressed;
        fprintf(stderr, "Failed to compress source buffer at %s:%d\n", file, line);
        throw RemotePluginClosedException();
    }

    fprintf(stderr, "compressed source buffer. size=%lu bytes\n", complen);

    int len = complen;
    rdwr_tryWrite(fd, &len, sizeof(int), file, line);
    len = rawdata.size();
    rdwr_tryWrite(fd, &len, sizeof(int), file, line);    
    rdwr_tryWrite(fd, compressed, complen, file, line);

    delete [] compressed;
}

template <typename T> std::vector<char>
rdwr_readRaw(T fd, const char *file, int line)
{
    int complen, len;
    static char *rawbuf = 0;
    static int bufLen = 0;
    rdwr_tryRead(fd, &complen, sizeof(int), file, line);
    rdwr_tryRead(fd, &len, sizeof(int), file, line);
    if (complen > bufLen) {
    delete rawbuf;
    rawbuf = new char[complen];
    bufLen = complen;
    }
    rdwr_tryRead(fd, rawbuf, complen, file, line);

    char *uncompressed = new char [len];

    if(!uncompressed)
    {
        fprintf(stderr, "Failed to allocate %d bytes of memory at %s:%d\n", len, file, line);
        throw RemotePluginClosedException();
    }

    unsigned long destlen = len;

    if(uncompress((Bytef *)uncompressed, &destlen, (Bytef *)rawbuf, complen) != Z_OK)
    {
        delete uncompressed;
        fprintf(stderr, "Failed to uncompress source buffer at %s:%d\n", file, line);
        throw RemotePluginClosedException();   
    }

    fprintf(stderr, "uncompressed source buffer. size=%lu bytes, complen=%d\n", destlen, complen);

    std::vector<char> rawout;
    for(unsigned long i = 0; i < destlen; i++)
    {
        rawout.push_back(uncompressed [i]);
    }

    delete uncompressed;

    return rawout;
}
//Deryabin Andrew: vst chunks support: end code

template
void rdwr_writeOpcode(int fd, RemotePluginOpcode opcode, const char *file, int line);
template
void rdwr_writeString(int fd, const std::string &str, const char *file, int line);
template
std::string rdwr_readString(int fd, const char *file, int line);
template
void rdwr_writeInt(int fd, int i, const char *file, int line);
template
int rdwr_readInt(int fd, const char *file, int line);
template
void rdwr_writeFloat(int fd, float f, const char *file, int line);
template
float rdwr_readFloat(int fd, const char *file, int line);
template
unsigned char *rdwr_readMIDIData(int fd, int **frameoffsets, int &events, const char *file, int line);
template
void rdwr_writeRaw(int fd, std::vector<char> rawdata, const char *file, int line);
template
std::vector<char> rdwr_readRaw(int fd, const char *file, int line);

template
void rdwr_writeOpcode(RingBuffer *ringbuf, RemotePluginOpcode opcode, const char *file, int line);
template
void rdwr_writeString(RingBuffer *ringbuf, const std::string &str, const char *file, int line);
template
std::string rdwr_readString(RingBuffer *ringbuf, const char *file, int line);
template
void rdwr_writeInt(RingBuffer *ringbuf, int i, const char *file, int line);
template
int rdwr_readInt(RingBuffer *ringbuf, const char *file, int line);
template
void rdwr_writeFloat(RingBuffer *ringbuf, float f, const char *file, int line);
template
float rdwr_readFloat(RingBuffer *ringbuf, const char *file, int line);
template
unsigned char *rdwr_readMIDIData(RingBuffer *ringbuf, int **frameoffsets, int &events, const char *file, int line);
template
void rdwr_writeRaw(RingBuffer *ringbuf, std::vector<char> rawdata, const char *file, int line);
template
std::vector<char> rdwr_readRaw(RingBuffer *ringbuf, const char *file, int line);
