/*
 *  DOSBox OPL2/OPL3 emulator interface (compiled here with OPLTYPE_IS_OPL3).
 *  Copyright (C) 2002-2010 The DOSBox Team.  GNU LGPL 2.1+.
 *  Based on ADLIBEMU.C by Ken Silverman (C) 1998-2001.
 *
 *  Used as a software "tee": the descent-hmp-player visualizer mirrors the OPL
 *  register stream that libADLMIDI sends to the board into this emulator, to
 *  recover per-channel output levels (and optional PC audio).
 */
#ifndef VIZ_OPL_H
#define VIZ_OPL_H

#include <stdint.h>

typedef uintptr_t Bitu;
typedef intptr_t  Bits;
typedef uint32_t  Bit32u;
typedef int32_t   Bit32s;
typedef uint16_t  Bit16u;
typedef int16_t   Bit16s;
typedef uint8_t   Bit8u;
typedef int8_t    Bit8s;

#ifdef __cplusplus
extern "C" {
#endif

void adlib_init(Bit32u samplerate);
void adlib_write(Bitu idx, Bit8u val);
void adlib_getsample(Bit16s *sndptr, Bits numsamples);   // OPL3 build: stereo, interleaved

// Per-channel output level (~0..1, peak-to-peak since last call) for 2-op
// channel ch (0..17).  A silent/keyed-but-inaudible channel reads ~0.
double opl_channel_level(int ch);

#ifdef __cplusplus
}
#endif

#endif /* VIZ_OPL_H */
