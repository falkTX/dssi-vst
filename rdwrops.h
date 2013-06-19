// -*- c-basic-offset: 4 -*-

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004-2010 Chris Cannam
*/

#ifndef _RD_WR_OPS_H_
#define _RD_WR_OPS_H_

#include <string>
#include <vector>
#include "remoteplugin.h"

#include <semaphore.h>

// Should be divisible by three
#define MIDI_BUFFER_SIZE 1023

#define SHM_RING_BUFFER_SIZE 2048

struct RingBuffer
{
    int head;
    int tail;
    int written;
    bool invalidateCommit;
    char buf[SHM_RING_BUFFER_SIZE];
};

struct ShmControl
{
    // 32 and 64-bit binaries align semaphores differently.
    // Let's make sure there's plenty of room for either one.
    union {
        sem_t runServer;
        char alignServerSem[32];
    };
    union {
        sem_t runClient;
        char alignClientSem[32];
    };
    RingBuffer ringBuffer;
};

void rdwr_tryRead(int fd, void *buf, size_t count, const char *file, int line);
void rdwr_tryWrite(int fd, const void *buf, size_t count, const char *file, int line);
void rdwr_tryRead(RingBuffer *ringbuf, void *buf, size_t count, const char *file, int line);
void rdwr_tryWrite(RingBuffer *ringbuf, const void *buf, size_t count, const char *file, int line);
void rdwr_commitWrite(RingBuffer *ringbuf, const char *file, int line);
bool dataAvailable(RingBuffer *ringbuf);

template <typename T>
void rdwr_writeOpcode(T fd, RemotePluginOpcode opcode, const char *file, int line);
template <typename T>
void rdwr_writeString(T fd, const std::string &str, const char *file, int line);
template <typename T>
std::string rdwr_readString(T fd, const char *file, int line);
template <typename T>
void rdwr_writeInt(T fd, int i, const char *file, int line);
template <typename T>
int rdwr_readInt(T fd, const char *file, int line);
template <typename T>
void rdwr_writeFloat(T fd, float f, const char *file, int line);
template <typename T>
float rdwr_readFloat(T fd, const char *file, int line);
template <typename T>
unsigned char *rdwr_readMIDIData(T fd, int **frameoffsets, int &events, const char *file, int line);
template <typename T>
void rdwr_writeRaw(T fd, std::vector<char> rawdata, const char *file, int line);
template <typename T>
std::vector<char> rdwr_readRaw(T fd, const char *file, int line);

#define tryRead(a, b, c) rdwr_tryRead(a, b, c, __FILE__, __LINE__)
#define tryWrite(a, b, c) rdwr_tryWrite(a, b, c, __FILE__, __LINE__)
#define writeOpcode(a, b) rdwr_writeOpcode(a, b, __FILE__, __LINE__)
#define writeString(a, b) rdwr_writeString(a, b, __FILE__, __LINE__)
#define readString(a) rdwr_readString(a, __FILE__, __LINE__)
#define writeInt(a, b) rdwr_writeInt(a, b, __FILE__, __LINE__)
#define readInt(a) rdwr_readInt(a, __FILE__, __LINE__)
#define writeFloat(a, b) rdwr_writeFloat(a, b, __FILE__, __LINE__)
#define readFloat(a) rdwr_readFloat(a, __FILE__, __LINE__)
#define readMIDIData(a, b, c) rdwr_readMIDIData(a, b, c, __FILE__, __LINE__)
#define commitWrite(a) rdwr_commitWrite(a, __FILE__, __LINE__)
#define purgeRead(a) rdwr_purgeRead(a, __FILE__, __LINE__)

//Deryabin Andrew: chunks support
#define writeRaw(a, b) rdwr_writeRaw(a, b, __FILE__, __LINE__)
#define readRaw(a) rdwr_readRaw(a, __FILE__, __LINE__)
//Deryabin Andrew: vst chunks support: end code

#endif
