/*
 * hmpviz.h — SDL2 visualizer front-end for hmpplay_opl3.
 *
 * Mirrors the OPL register stream that libADLMIDI sends to the RetroWave board
 * into a software OPL3 emulator ("tee", viz/opl.c) and renders a per-channel
 * activity visualizer (18 OPL3 channels) with transport controls. Ported from
 * the tyrian-retrowave GUI.
 *
 * Threading: the player runs libADLMIDI's tick loop on a worker thread (it
 * calls hmpviz_opl_tap on every register write); the GUI runs on the main
 * thread (hmpviz_init / repeated hmpviz_frame / hmpviz_shutdown).
 */
#ifndef HMPVIZ_H
#define HMPVIZ_H

#include <atomic>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif
// Wire libADLMIDI's g_retrowave_opl_tap to this (feeds the software tee).
void hmpviz_opl_tap(uint16_t addr, uint8_t data);
#ifdef __cplusplus
}
#endif

// Transport handles into the player (set/read by the GUI).
struct HmpVizHooks
{
	std::atomic<bool> *stop;    // GUI -> player: quit
	std::atomic<bool> *skip;    // GUI -> player: advance current song
	std::atomic<bool> *prev;    // GUI -> player: go to previous
	std::atomic<bool> *pause;   // GUI <-> player: pause ticking
	bool   *loop;               // GUI -> player: loop toggle
	double *tempo;              // GUI -> player: tempo scale
	void  (*apply_tempo)(double);
};

bool hmpviz_init(const HmpVizHooks *hooks);            // main thread; false if no GUI
void hmpviz_set_status(int idx, int total, const char *name);  // player thread
bool hmpviz_frame(void);                               // main thread; false => quit
void hmpviz_shutdown(void);

#endif /* HMPVIZ_H */
