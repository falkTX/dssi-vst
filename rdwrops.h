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

// Should be divisible by three
#define MIDI_BUFFER_SIZE 1023

extern void rdwr_tryRead(int fd, void *buf, size_t count, const char *file, int line);
extern void rdwr_tryWrite(int fd, const void *buf, size_t count, const char *file, int line);
extern void rdwr_writeOpcode(int fd, RemotePluginOpcode opcode, const char *file, int line);
extern void rdwr_writeString(int fd, const std::string &str, const char *file, int line);
extern std::string rdwr_readString(int fd, const char *file, int line);
extern void rdwr_writeInt(int fd, int i, const char *file, int line);
extern int rdwr_readInt(int fd, const char *file, int line);
extern void rdwr_writeFloat(int fd, float f, const char *file, int line);
extern float rdwr_readFloat(int fd, const char *file, int line);
extern unsigned char *rdwr_readMIDIData(int fd, int **frameoffsets, int &events, const char *file, int line);

//Deryabin Andrew: vst chunks support
extern void rdwr_writeRaw(int fd, std::vector<char>, const char *file, int line);
extern std::vector<char> rdwr_readRaw(int fd, const char *file, int line);
//Deryabin Andrew: vst chunks support: end code

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

//Deryabin Andrew: chunks support
#define writeRaw(a, b) rdwr_writeRaw(a, b, __FILE__, __LINE__)
#define readRaw(a) rdwr_readRaw(a, __FILE__, __LINE__)
//Deryabin Andrew: vst chunks support: end code

#endif
