/*
 * hmpplay_opl3 - Direct OPL3 player for Descent 1 & 2 HMP music files
 *                via RetroWave OPL3 Express (/dev/ttyACM0)
 *
 * Uses Descent's own MELODIC.BNK / DRUMS.BNK instrument patches extracted
 * from the game's HOG archive, so synthesis is identical to the original
 * Sound Blaster experience — no MIDI proxy, no third-party sound font.
 *
 * Architecture:
 *   HMP file  →  HMI event decoder  →  OPL3 register writes
 *                                    →  RetroWave serial framer
 *                                    →  /dev/ttyACM0 @ 2 Mbaud
 *
 * Usage:
 *   hmpplay_opl3 [-d /dev/ttyACM0] [-m melodic.bnk] [-r drums.bnk]
 *                [-l] [-t scale] [-v] [--list-channels]
 *                <file.hmp | directory> [...]
 *
 * Build:
 *   g++ -std=c++17 -O2 -o hmpplay_opl3 hmpplay_opl3.cpp
 *   (no external library dependencies — serial I/O and OPL3 are self-contained)
 *
 * BNK files are in the game HOG archive. Extract with hogtool:
 *   ./hogtool extract descent.hog -o ./banks/ melodic.bnk drums.bnk
 *   ./hogtool extract descent2.hog -o ./banks/ melodic.bnk drums.bnk
 *
 * Wire protocol implemented from:
 *   SudoMaker/RetroWave RetroWaveLib/Protocol/Serial.c  (AGPLv3)
 *   SudoMaker/RetroWave RetroWaveLib/Board/OPL3.c       (AGPLv3)
 *   SudoMaker/RetroWave RetroWaveLib/Platform/POSIX_SerialPort.c (AGPLv3)
 *
 * HMP parsing from DXX-Rebirth common/misc/hmp.cpp (GPL v2+)
 * BNK format from ModdingWiki AdLib Instrument Bank Format
 * OPL3 register layout from Yamaha YMF262 Application Manual
 */

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// RetroWave serial wire protocol
// Implemented from SudoMaker/RetroWave RetroWaveLib/Protocol/Serial.c
// ─────────────────────────────────────────────────────────────────────────────

// The protocol encodes raw bytes so that no data byte is ever 0x00 or 0x02,
// reserving 0x00 as a frame-start marker and 0x02 as frame-end.
// It is a 7-of-8 bit encoding: every 7 input bytes become 8 output bytes,
// each with bit 0 forced to 1.  The frame is wrapped: [0x00][encoded...][0x02].

static uint32_t rw_pack(const uint8_t *in, uint32_t len, uint8_t *out) {
    uint32_t ic = 0, oc = 0;
    out[oc++] = 0x00;           // frame start
    uint8_t shift = 0;
    while (ic < len) {
        uint8_t b = in[ic] >> shift;
        if (ic > 0) b |= in[ic-1] << (8 - shift);
        b |= 0x01;
        out[oc++] = b;
        shift++;
        ic++;
        if (shift > 7) { shift = 0; ic--; }
    }
    if (shift) {
        out[oc++] = (in[ic-1] << (8 - shift)) | 0x01;
    }
    out[oc++] = 0x02;           // frame end
    return oc;
}

// ─────────────────────────────────────────────────────────────────────────────
// RetroWave OPL3 board commands
// Board type byte for OPL3: 0x21 << 1 = 0x42
// Implemented from SudoMaker/RetroWave RetroWaveLib/Board/OPL3.c
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint8_t RW_BOARD_OPL3 = 0x42;
static constexpr uint32_t RW_SPEED     = 2000000;

// Raw command buffer for one OPL3 register write (8 bytes payload)
// Port 0 (OPL3 primary):  { 0x42, 0x12, 0xe1, reg, 0xe3, val, 0xfb, val }
// Port 1 (OPL3 secondary): { 0x42, 0x12, 0xe5, reg, 0xe7, val, 0xfb, val }
// Reset:                   { 0x42, 0x12, 0xfe, 0x00 }  then  { 0x42, 0x12, 0xff, 0x00 }

struct OPL3Cmd {
    uint8_t buf[8];
    uint8_t len;
};

static OPL3Cmd opl3_cmd_port0(uint8_t reg, uint8_t val) {
    return {{ RW_BOARD_OPL3, 0x12, 0xe1, reg, 0xe3, val, 0xfb, val }, 8};
}
static OPL3Cmd opl3_cmd_port1(uint8_t reg, uint8_t val) {
    return {{ RW_BOARD_OPL3, 0x12, 0xe5, reg, 0xe7, val, 0xfb, val }, 8};
}
static OPL3Cmd opl3_cmd_reset_low()  { return {{ RW_BOARD_OPL3, 0x12, 0xfe, 0x00 }, 4}; }
static OPL3Cmd opl3_cmd_reset_high() { return {{ RW_BOARD_OPL3, 0x12, 0xff, 0x00 }, 4}; }

// ─────────────────────────────────────────────────────────────────────────────
// Serial port — /dev/ttyACM0 @ 2 Mbaud
// Implemented from SudoMaker/RetroWave RetroWaveLib/Platform/POSIX_SerialPort.c
// ─────────────────────────────────────────────────────────────────────────────

struct SerialPort {
    int fd{-1};

    void open(const char *path) {
        fd = ::open(path, O_RDWR | O_NOCTTY);
        if (fd < 0)
            throw std::runtime_error(std::string("Cannot open ") + path + ": " + strerror(errno));

        struct termios tio{};
        if (ioctl(fd, TCGETS, &tio) < 0)
            throw std::runtime_error("TCGETS failed");

        tio.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
        tio.c_oflag &= ~OPOST;
        tio.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
        tio.c_cflag &= ~(CSIZE|PARENB|CBAUD);
        tio.c_cflag |= CS8 | B2000000;

        if (ioctl(fd, TCSETS, &tio) < 0)
            throw std::runtime_error("TCSETS failed");
    }

    void write_cmd(const OPL3Cmd &cmd) {
        uint8_t packed[32];
        uint32_t plen = rw_pack(cmd.buf, cmd.len, packed);
        size_t written = 0;
        while (written < plen) {
            ssize_t rc = ::write(fd, packed + written, plen - written);
            if (rc > 0) written += rc;
            else throw std::runtime_error("Serial write failed");
        }
    }

    void write_reg(bool port1, uint8_t reg, uint8_t val) {
        write_cmd(port1 ? opl3_cmd_port1(reg, val) : opl3_cmd_port0(reg, val));
    }

    void reset() {
        write_cmd(opl3_cmd_reset_low());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        write_cmd(opl3_cmd_reset_high());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ~SerialPort() { if (fd >= 0) ::close(fd); }
};

// ─────────────────────────────────────────────────────────────────────────────
// OPL3 register layout
//
// 18 2-operator channels total:
//   Channels 0-8  → port 0 registers (0x000-0x0FF)
//   Channels 9-17 → port 1 registers (0x100-0x1FF) — same reg offsets, port1=true
//
// Operator (slot) offsets within a channel (YMF262 datasheet table):
//   Ch:    0  1  2  3  4  5  6  7  8
//   Slot0: 0  1  2  8  9  A 10 11 12   (op0, modulator)
//   Slot1: 3  4  5  B  C  D 13 14 15   (op1, carrier)
// ─────────────────────────────────────────────────────────────────────────────

static const uint8_t opl3_op_offset[9][2] = {
    {0x00, 0x03}, {0x01, 0x04}, {0x02, 0x05},
    {0x08, 0x0B}, {0x09, 0x0C}, {0x0A, 0x0D},
    {0x10, 0x13}, {0x11, 0x14}, {0x12, 0x15},
};

// Frequency number and block for a MIDI note
static void note_to_fnum(int midi_note, uint8_t *block_out, uint16_t *fnum_out) {
    double freq = 440.0 * std::pow(2.0, (midi_note - 69) / 12.0);
    int block = 0;
    while (block < 7 && freq * (1 << (20 - block)) / 49716.0 >= 512.0)
        block++;
    uint16_t fnum = (uint16_t)(freq * (1 << (20 - block)) / 49716.0);
    if (fnum > 0x3FF) fnum = 0x3FF;
    *block_out  = (uint8_t)block;
    *fnum_out   = fnum;
}

// ─────────────────────────────────────────────────────────────────────────────
// HMI BNK instrument bank parser
//
// Format from ModdingWiki "AdLib Instrument Bank Format"
// The HMI/Descent variant has version bytes 0,0 and 128 instruments.
// ─────────────────────────────────────────────────────────────────────────────

// Packed OPL register fields for one operator (from BNK file)
struct BnkOpRegs {
    uint8_t ksl;        // key scaling level     → reg 0x40 bits 6-7
    uint8_t multiple;   // frequency multiplier  → reg 0x20 bits 0-3
    uint8_t feedback;   // feedback (op0 only)   → reg 0xC0 bits 1-3
    uint8_t attack;     // attack rate           → reg 0x60 upper nibble
    uint8_t sustain;    // sustain level         → reg 0x80 upper nibble
    uint8_t eg;         // envelope gain         → reg 0x20 bit 5
    uint8_t decay;      // decay rate            → reg 0x60 lower nibble
    uint8_t releaseRate;// release rate          → reg 0x80 lower nibble
    uint8_t totalLevel; // total output level    → reg 0x40 bits 0-5
    uint8_t am;         // amplitude modulation  → reg 0x20 bit 7
    uint8_t vib;        // vibrato               → reg 0x20 bit 6
    uint8_t ksr;        // key scaling rate      → reg 0x20 bit 4
    uint8_t con;        // connection (op0 only) → reg 0xC0 bit 0 (inverted)
};

struct BnkInstrument {
    bool     valid{false};
    bool     percussive{false};
    uint8_t  voice_num{0};          // MIDI percussion key (drums only)
    BnkOpRegs op[2];                // op[0]=modulator, op[1]=carrier
    uint8_t  mod_wave{0};
    uint8_t  car_wave{0};
};

// Parse HMI BNK file.  Returns array of 128 instruments indexed by program number.
static std::vector<BnkInstrument> load_bnk(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) throw std::runtime_error(std::string("Cannot open BNK: ") + path);

    // Header: 2 version bytes, "ADLIB-", uint16 numUsed, uint16 numInstruments,
    //         uint32 offsetName, uint32 offsetData, 8 pad bytes
    uint8_t hdr[24];
    if (fread(hdr, 1, 24, f) != 24) { fclose(f); throw std::runtime_error("BNK header read error"); }
    if (memcmp(hdr+2, "ADLIB-", 6) != 0) { fclose(f); throw std::runtime_error("Not an AdLib BNK file"); }

    uint16_t num_instruments = hdr[10] | (hdr[11] << 8);
    uint32_t offset_name     = hdr[12] | (hdr[13]<<8) | (hdr[14]<<16) | (hdr[15]<<24);
    uint32_t offset_data     = hdr[16] | (hdr[17]<<8) | (hdr[18]<<16) | (hdr[19]<<24);

    // Read name entries: uint16 index, uint8 flags, char[9] name
    fseek(f, offset_name, SEEK_SET);
    std::vector<BnkInstrument> bank(128);

    for (int i = 0; i < num_instruments; i++) {
        uint8_t ne[12];
        if (fread(ne, 1, 12, f) != 12) break;
        uint16_t data_idx = ne[0] | (ne[1] << 8);
        // uint8_t flags  = ne[2];  (HMI variant: any non-zero = used)
        // name = ne[3..11]
        if (data_idx >= 128) continue;

        // Read instrument data at offset_data + data_idx * sizeof(PackedTimbre)
        // PackedTimbre = 2 + 13 + 13 + 2 = 30 bytes
        long saved = ftell(f);
        fseek(f, offset_data + data_idx * 30, SEEK_SET);

        uint8_t d[30];
        if (fread(d, 1, 30, f) != 30) { fseek(f, saved, SEEK_SET); continue; }
        fseek(f, saved, SEEK_SET);

        BnkInstrument &ins = bank[data_idx];
        ins.valid      = true;
        ins.percussive = d[0] != 0;
        ins.voice_num  = d[1];

        // op[0] = modulator (bytes 2..14), op[1] = carrier (bytes 15..27)
        for (int op = 0; op < 2; op++) {
            const uint8_t *b = d + 2 + op * 13;
            ins.op[op].ksl        = b[0];
            ins.op[op].multiple   = b[1];
            ins.op[op].feedback   = b[2];
            ins.op[op].attack     = b[3];
            ins.op[op].sustain    = b[4];
            ins.op[op].eg         = b[5];
            ins.op[op].decay      = b[6];
            ins.op[op].releaseRate= b[7];
            ins.op[op].totalLevel = b[8];
            ins.op[op].am         = b[9];
            ins.op[op].vib        = b[10];
            ins.op[op].ksr        = b[11];
            ins.op[op].con        = b[12];
        }
        ins.mod_wave = d[28];
        ins.car_wave = d[29];
    }

    fclose(f);
    return bank;
}

// ─────────────────────────────────────────────────────────────────────────────
// OPL3 voice manager
// Manages 18 hardware voices across 2 OPL3 register sets (ports 0 and 1)
// ─────────────────────────────────────────────────────────────────────────────

struct Voice {
    bool     active{false};
    int      channel{-1};      // MIDI channel
    int      note{-1};         // MIDI note
    uint32_t age{0};           // for LRU eviction
};

struct OPL3 {
    SerialPort &serial;
    Voice       voices[18];
    uint32_t    tick{0};

    // Per-MIDI-channel state
    uint8_t     program[16]{};
    uint8_t     volume[16];
    int8_t      pan[16]{};     // -64..+63
    int         pitch_bend[16]{};

    // Instrument banks
    const std::vector<BnkInstrument> *melodic{nullptr};
    const std::vector<BnkInstrument> *drums{nullptr};

    OPL3(SerialPort &s) : serial(s) {
        for (int i = 0; i < 16; i++) volume[i] = 100;
    }

    // ── Low-level register writes ────────────────────────────────────────────

    void write(int ch, uint8_t reg, uint8_t val) {
        // Channels 0-8 → port 0, channels 9-17 → port 1
        bool port1 = (ch >= 9);
        serial.write_reg(port1, reg, val);
    }

    void write_op(int ch, int op_slot, uint8_t base_reg, uint8_t val) {
        // base_reg is the register family (0x20, 0x40, 0x60, 0x80, 0xE0)
        int phys_ch = ch % 9;
        uint8_t off = opl3_op_offset[phys_ch][op_slot];
        write(ch, base_reg + off, val);
    }

    // ── Program an OPL3 voice with a BNK instrument ──────────────────────────

    void program_voice(int voice_idx, const BnkInstrument &ins, uint8_t vol) {
        int ch = voice_idx;

        for (int op = 0; op < 2; op++) {
            const BnkOpRegs &r = ins.op[op];

            // 0x20: AM | VIB | EG | KSR | MULTIPLE
            write_op(ch, op, 0x20,
                (r.am  ? 0x80 : 0) |
                (r.vib ? 0x40 : 0) |
                (r.eg  ? 0x20 : 0) |
                (r.ksr ? 0x10 : 0) |
                (r.multiple & 0x0F));

            // 0x40: KSL | TOTAL_LEVEL (carrier volume scaled by MIDI velocity)
            uint8_t tl = r.totalLevel & 0x3F;
            if (op == 1) {
                // Scale carrier total level by volume (lower tl = louder)
                // tl=0 is max, tl=63 is silent
                int scaled = tl + (int)(((63 - tl) * (127 - vol)) / 127);
                tl = (uint8_t)std::min(63, scaled);
            }
            write_op(ch, op, 0x40, (r.ksl << 6) | tl);

            // 0x60: ATTACK | DECAY
            write_op(ch, op, 0x60, (r.attack << 4) | (r.decay & 0x0F));

            // 0x80: SUSTAIN | RELEASE
            write_op(ch, op, 0x80, (r.sustain << 4) | (r.releaseRate & 0x0F));

            // 0xE0: WAVE SELECT
            write_op(ch, op, 0xE0, op == 0 ? ins.mod_wave : ins.car_wave);
        }

        // 0xC0: FEEDBACK | CONNECTION | OPL3 output enable (both DACs)
        // connection: 0 = FM (carrier output only), 1 = additive (both ops)
        // con field: 0 in BNK means OPL bit = 1 (FM); nonzero = OPL bit = 0 (additive)
        uint8_t con_bit = (ins.op[0].con == 0) ? 1 : 0;
        uint8_t fb  = (ins.op[0].feedback & 0x07) << 1;
        write(ch, 0xC0 + (ch % 9), fb | con_bit | 0x30); // 0x30 = both OPL3 outputs
    }

    // ── Note on ─────────────────────────────────────────────────────────────

    void note_on(int midi_ch, int note, int velocity) {
        if (velocity == 0) { note_off(midi_ch, note); return; }

        bool is_drum = (midi_ch == 9);
        const std::vector<BnkInstrument> *bank = is_drum ? drums : melodic;
        if (!bank) return;

        // Select instrument
        int prog = is_drum ? note : program[midi_ch];
        if (prog < 0 || prog >= (int)bank->size() || !(*bank)[prog].valid) return;
        const BnkInstrument &ins = (*bank)[prog];

        // Find a free voice, or steal the oldest active one
        int voice = -1;
        uint32_t oldest_age = UINT32_MAX;
        for (int i = 0; i < 18; i++) {
            if (!voices[i].active) { voice = i; break; }
            if (voices[i].age < oldest_age) {
                oldest_age = voices[i].age;
                voice = i;
            }
        }

        // If stealing, silence the old note first
        if (voices[voice].active)
            key_off(voice);

        uint8_t vol = (uint8_t)((volume[midi_ch] * velocity) / 127);
        program_voice(voice, ins, vol);

        // Set frequency
        uint8_t block; uint16_t fnum;
        note_to_fnum(note, &block, &fnum);

        // Apply pitch bend (±2 semitones default range)
        if (pitch_bend[midi_ch] != 0) {
            double semis = pitch_bend[midi_ch] * 2.0 / 8192.0;
            double freq  = 440.0 * std::pow(2.0, (note - 69 + semis) / 12.0);
            block = 0;
            while (block < 7 && freq * (1 << (20 - block)) / 49716.0 >= 512.0) block++;
            fnum = (uint16_t)std::min(0x3FF, (int)(freq * (1 << (20 - block)) / 49716.0));
        }

        int phys_ch = voice % 9;
        // 0xA0: F-num low byte
        write(voice, 0xA0 + phys_ch, fnum & 0xFF);
        // 0xB0: KEY-ON | BLOCK | F-num high 2 bits
        write(voice, 0xB0 + phys_ch, 0x20 | ((block & 0x07) << 2) | ((fnum >> 8) & 0x03));

        voices[voice] = { true, midi_ch, note, tick++ };
    }

    // ── Note off ─────────────────────────────────────────────────────────────

    void key_off(int voice) {
        int phys_ch = voice % 9;
        // Read back current B0 value (we don't store it, so reconstruct)
        // Reconstruct: clear KEY-ON bit — keep block/fnum as 0 (silence)
        write(voice, 0xB0 + phys_ch, 0x00);
        voices[voice].active = false;
    }

    void note_off(int midi_ch, int note) {
        for (int i = 0; i < 18; i++) {
            if (voices[i].active && voices[i].channel == midi_ch && voices[i].note == note) {
                key_off(i);
            }
        }
    }

    // ── CC / program change ───────────────────────────────────────────────────

    void control_change(int ch, int cc, int val) {
        if (cc == 7) volume[ch] = (uint8_t)val;      // channel volume
    }

    void program_change(int ch, int prog) {
        program[ch] = (uint8_t)(prog & 127);
    }

    void pitch_bend_change(int ch, int d1, int d2) {
        pitch_bend[ch] = ((d2 << 7) | d1) - 8192;
    }

    // ── Silence all voices ────────────────────────────────────────────────────

    void all_notes_off() {
        for (int i = 0; i < 18; i++) {
            key_off(i);
        }
        // Also write silence to all operator totalLevel registers
        for (int i = 0; i < 18; i++) {
            for (int op = 0; op < 2; op++) {
                write_op(i, op, 0x40, 0x3F);   // max attenuation
            }
        }
    }

    // ── Initialize OPL3 ──────────────────────────────────────────────────────

    void init() {
        serial.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Enable OPL3 mode (register 0x105 on port 1)
        serial.write_reg(true,  0x05, 0x01);   // OPL3 enable
        // Enable all 4 OPL3 output channels  (register 0x04 on port 1)
        serial.write_reg(true,  0x04, 0x00);   // CSM off, note-sel 0

        // Silence everything
        all_notes_off();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// HMP file parser (same as hmpplay.cpp, without PhysFS)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int HMP_MAX_TRACKS = 32;

struct HmpTrack { std::vector<uint8_t> data; };

// HMI SOS device IDs (index into deviceTrackMappings)
// Each maps to a set of tracks intended for that hardware
static constexpr int HMP_DEVICE_OPL    = 0;  // AdLib/OPL2/OPL3  -> intmelo.bnk + intdrum.bnk
static constexpr int HMP_DEVICE_MT32   = 1;  // Roland MT-32      -> melodic.bnk + drum.bnk
static constexpr int HMP_DEVICE_GM     = 2;  // General MIDI      -> melodic.bnk + drum.bnk
static constexpr int HMP_DEVICE_GS     = 3;  // Roland GS/SC      -> hammelo.bnk + hamdrum.bnk
static constexpr int HMP_DEVICE_TANDY  = 4;  // Tandy/PS1

// Suggested banks per device
static const char *HMP_DEVICE_NAMES[5] = { "OPL/AdLib", "MT-32", "General MIDI", "Roland GS", "Tandy" };
// Suggested banks per device: {intmelo,melodic,melodic,hammelo,?} for melodic
//                              {intdrum,drum,drum,hamdrum,?} for percussion

struct HmpFile {
    int num_tracks{0};
    int tempo{0};
    HmpTrack tracks[HMP_MAX_TRACKS];
    long filesize{0};
    // deviceTrackMappings[device][track_slot]: which track index to use per device
    // 0 = not mapped for this device. Read from HMP header at offset 0x80.
    uint32_t device_track_map[5][32]{};
    bool has_device_map{false};
};

static int read_hmi_var(const uint8_t *data, int len, uint32_t *out) {
    uint32_t v = 0; int shift = 0;
    const uint8_t *p = data;
    while (len > 0 && !(*p & 0x80)) { v += (uint32_t)(*p++) << shift; shift += 7; len--; }
    if (!len) return 0;
    v += (uint32_t)(*p++ & 0x7f) << shift;
    if (out) *out = v;
    return (int)(p - data);
}

// Silences warn_unused_result on fread without losing error semantics we handle elsewhere
static size_t xfread(void *ptr, size_t sz, size_t n, FILE *f) { return fread(ptr, sz, n, f); }

static std::unique_ptr<HmpFile> hmp_open(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) throw std::runtime_error(std::string("Cannot open: ") + filename);
    fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "HMIMIDIP", 8)) {
        fclose(f); throw std::runtime_error(std::string("Not a HMP file: ") + filename); }
    if (fseek(f, 0x30, SEEK_SET)) { fclose(f); throw std::runtime_error("seek fail"); }
    uint32_t num_tracks = 0;
    xfread(&num_tracks, 4, 1, f);
    if (num_tracks < 1 || num_tracks > HMP_MAX_TRACKS) {
        fclose(f); throw std::runtime_error("Bad track count"); }
    int32_t raw_tempo = 0;
    fseek(f, 0x38, SEEK_SET); xfread(&raw_tempo, 4, 1, f);
    // Read deviceTrackMappings[5][32] from offset 0x80
    // Layout: 5 devices x 32 track slots (outer=device, inner=track)
    // Entry = track index to play for this device; 0 = unused slot
    int32_t dtm[5][32]{};
    bool has_map = false;
    if (fseek(f, 0x80, SEEK_SET) == 0) {
        if (fread(dtm, sizeof(dtm), 1, f) == 1)
            has_map = true;
    }

    fseek(f, 0x308, SEEK_SET);
    auto hmp = std::make_unique<HmpFile>();
    hmp->num_tracks = (int)num_tracks; hmp->tempo = raw_tempo; hmp->filesize = fsize;
    hmp->has_device_map = has_map;
    if (has_map) {
        for (int d = 0; d < 5; d++)
            for (int t = 0; t < 32; t++)
                hmp->device_track_map[d][t] = (uint32_t)dtm[d][t];
    }
    for (int i = 0; i < hmp->num_tracks; i++) {
        int32_t td[3]; xfread(td, 4, 3, f);
        int dlen = td[1] - 12; if (dlen <= 0) { fclose(f); throw std::runtime_error("bad track len"); }
        hmp->tracks[i].data.resize(dlen);
        xfread(hmp->tracks[i].data.data(), 1, dlen, f);
    }
    fclose(f);
    return hmp;
}

// ─────────────────────────────────────────────────────────────────────────────
// Playback engine — drives OPL3 directly from HMP events
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_skip{false};
static std::atomic<bool> g_prev{false};

void signal_handler(int) { g_stop = true; }

struct PlayOptions {
    bool   loop{false};
    double tempo_scale{1.0};
    bool   verbose{false};
    int    device{HMP_DEVICE_OPL};   // HMP device index
};

// Terminal raw mode (same as hmpplay.cpp)
struct RawTerm {
    struct termios saved{}; bool active{false};
    void enable() {
        if (!isatty(STDIN_FILENO)) return;
        tcgetattr(STDIN_FILENO, &saved);
        struct termios r = saved;
        r.c_lflag &= ~(unsigned)(ICANON|ECHO);
        r.c_cc[VMIN] = 0; r.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &r);
        active = true;
    }
    void disable() { if (active) { tcsetattr(STDIN_FILENO, TCSANOW, &saved); active = false; } }
    ~RawTerm() { disable(); }
};

void key_thread_fn(PlayOptions *opts) {
    struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
    printf("  Keys: [n/Space] next  [p] prev  [l] loop  [+/-] tempo  [q] quit\n");
    fflush(stdout);
    while (!g_stop) {
        if (poll(&pfd, 1, 50) <= 0) continue;
        char c; if (read(STDIN_FILENO, &c, 1) != 1) continue;
        switch (c) {
            case 'n': case ' ': printf("\n  >> Next\n"); g_skip = true; break;
            case 'p': printf("\n  >> Previous\n"); g_prev = true; g_skip = true; break;
            case 'l': opts->loop = !opts->loop;
                      printf("\n  >> Loop %s\n", opts->loop ? "ON" : "OFF"); break;
            case '+': case '=':
                opts->tempo_scale = std::min(opts->tempo_scale * 1.25, 4.0);
                printf("\n  >> Tempo scale: %.2fx\n", opts->tempo_scale); break;
            case '-':
                opts->tempo_scale = std::max(opts->tempo_scale / 1.25, 0.25);
                printf("\n  >> Tempo scale: %.2fx\n", opts->tempo_scale); break;
            case 'q': case 3: printf("\n  >> Quit\n"); g_stop = true; break;
            default: break;
        }
        fflush(stdout);
    }
}

void play_hmp(OPL3 &opl, const HmpFile &hmp, const PlayOptions &opts) {
    // Timing: same constants as hmpplay.cpp
    // HMP_TEMPO_US = 1,605,632 µs/beat (hardcoded in DXX's generated SMF)
    static constexpr double HMP_TEMPO_US = 1605632.0;
    int time_div = (int)(hmp.tempo * 1.6);
    if (time_div <= 0) time_div = 60;
    double us_per_tick = HMP_TEMPO_US / time_div;

    printf("  Tracks: %d  time_div: %d  tick: %.2f ms\n",
           hmp.num_tracks, time_div, us_per_tick / 1000.0);

    using clock = std::chrono::steady_clock;
    using us_t  = std::chrono::microseconds;

    // cmdlen table: bytes per MIDI event (excluding status), indexed by (status>>4)-8
    static const int cmdlen[8] = {3,3,3,3,2,2,3,0};

    do {
        // Build set of active tracks for this device
        // deviceTrackMappings: non-zero entries are the track indices to play
        // Track 0 is always the conductor track and is always skipped.
        // If no device map is present, play all tracks (legacy behaviour).
        std::vector<bool> track_active(hmp.num_tracks, false);
        if (hmp.has_device_map) {
            int dev = opts.device;
            int mapped = 0;
            for (int slot = 0; slot < 32; slot++) {
                uint32_t trk_idx = hmp.device_track_map[dev][slot];
                if (trk_idx > 0 && (int)trk_idx < hmp.num_tracks) {
                    track_active[trk_idx] = true;
                    mapped++;
                }
            }
            if (opts.verbose || mapped == 0)
                printf("  Device %d (%s): %d mapped tracks\n", dev, HMP_DEVICE_NAMES[dev], mapped);
            // If nothing mapped for this device, fall back to all tracks
            if (mapped == 0) {
                printf("  Warning: no tracks mapped for device %d, playing all tracks\n", dev);
                for (int i = 1; i < hmp.num_tracks; i++) track_active[i] = true;
            }
        } else {
            // No device map in header — play all tracks
            for (int i = 1; i < hmp.num_tracks; i++) track_active[i] = true;
        }

        // Per-track read state
        struct TrackState {
            const uint8_t *data; int size, pos;
            uint32_t cur_tick;
            uint8_t last_status;
            bool done;
        };
        std::vector<TrackState> trks(hmp.num_tracks);
        for (int i = 0; i < hmp.num_tracks; i++) {
            trks[i] = { hmp.tracks[i].data.data(),
                        (int)hmp.tracks[i].data.size(), 0, 0, 0,
                        !track_active[i] };  // pre-mark inactive tracks as done
        }

        auto start = clock::now();

        while (!g_stop && !g_skip) {
            // Find the track with the earliest next event (merge sort play)
            // Each track cursor sits right before the next event's delta.
            // Peek at each track's next event time.
            uint32_t min_tick = UINT32_MAX;
            int      min_trk  = -1;

            for (int t = 0; t < hmp.num_tracks; t++) {  // track 0 pre-marked done
                TrackState &ts = trks[t];
                if (ts.done || ts.pos >= ts.size) continue;
                uint32_t delta = 0;
                int got = read_hmi_var(ts.data + ts.pos, ts.size - ts.pos, &delta);
                if (got <= 0) { ts.done = true; continue; }
                uint32_t abs_tick = ts.cur_tick + delta;
                if (abs_tick < min_tick) { min_tick = abs_tick; min_trk = t; }
            }

            if (min_trk < 0) break;  // all tracks exhausted

            // Consume the event from min_trk
            TrackState &ts = trks[min_trk];
            uint32_t delta = 0;
            int got = read_hmi_var(ts.data + ts.pos, ts.size - ts.pos, &delta);
            ts.pos += got;
            ts.cur_tick += delta;

            // Wait until this tick's time
            double elapsed_us = ts.cur_tick * us_per_tick / opts.tempo_scale;
            auto target = start + us_t((long long)elapsed_us);
            auto now = clock::now();
            if (target > now) std::this_thread::sleep_until(target);
            if (g_stop || g_skip) break;

            if (ts.pos >= ts.size) { ts.done = true; continue; }

            // Read event
            uint8_t status = ts.data[ts.pos];
            if (status == 0xFF) {
                // Meta event
                ts.pos++;
                if (ts.pos >= ts.size) { ts.done = true; continue; }
                uint8_t meta_type = ts.data[ts.pos++];
                uint32_t meta_len = 0;
                while (ts.pos < ts.size) {
                    uint8_t b = ts.data[ts.pos++];
                    meta_len = (meta_len << 7) | (b & 0x7f);
                    if (!(b & 0x80)) break;
                }
                // Set Tempo (0x51): update us_per_tick
                if (meta_type == 0x51 && meta_len >= 3 && ts.pos + (int)meta_len <= ts.size) {
                    uint32_t new_tempo =
                        ((uint32_t)ts.data[ts.pos] << 16) |
                        ((uint32_t)ts.data[ts.pos+1] << 8) |
                         (uint32_t)ts.data[ts.pos+2];
                    if (new_tempo > 0) {
                        double old_upt = us_per_tick;
                        us_per_tick = new_tempo / (double)time_div;
                        // Adjust start to keep timing consistent across tempo change
                        double now_tick_old = ts.cur_tick * old_upt / opts.tempo_scale;
                        double now_tick_new = ts.cur_tick * us_per_tick / opts.tempo_scale;
                        start -= us_t((long long)(now_tick_new - now_tick_old));
                        if (opts.verbose)
                            printf("  tick=%6u  Tempo → %u µs/beat\n", ts.cur_tick, new_tempo);
                    }
                }
                if (meta_type == 0x2F) ts.done = true;  // end of track
                ts.pos += meta_len;
                continue;
            }

            // Short MIDI event (with running status)
            if (status & 0x80) { ts.last_status = status; ts.pos++; }
            else                { status = ts.last_status; }
            if (!ts.last_status) { ts.done = true; continue; }

            int cmd    = (ts.last_status >> 4) & 0x0F;
            int midi_ch = ts.last_status & 0x0F;
            int ndata  = (cmd >= 8 && cmd <= 14) ? cmdlen[cmd-8]-1 : 0;

            uint8_t d1 = 0, d2 = 0;
            if (ndata >= 1 && ts.pos < ts.size) d1 = ts.data[ts.pos++];
            if (ndata >= 2 && ts.pos < ts.size) d2 = ts.data[ts.pos++];

            if (opts.verbose)
                printf("  tick=%6u  ch=%2d  %02X %02X %02X\n",
                       ts.cur_tick, midi_ch, ts.last_status, d1, d2);

            switch (cmd) {
                case 0x9: opl.note_on(midi_ch, d1, d2);                 break;
                case 0x8: opl.note_off(midi_ch, d1);                    break;
                case 0xB: opl.control_change(midi_ch, d1, d2);          break;
                case 0xC: opl.program_change(midi_ch, d1);              break;
                case 0xE: opl.pitch_bend_change(midi_ch, d1, d2);       break;
                default:  break;
            }
        }

        if (opts.loop && !g_stop && !g_skip)
            printf("  [Looping...]\n");

    } while (opts.loop && !g_stop && !g_skip);
}

// ─────────────────────────────────────────────────────────────────────────────
// Directory expansion
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<std::string> expand_args(const std::vector<const char*> &args) {
    std::vector<std::string> result;
    for (const char *arg : args) {
        struct stat st;
        if (stat(arg, &st) != 0) { result.push_back(arg); continue; }
        if (!S_ISDIR(st.st_mode)) { result.push_back(arg); continue; }
        DIR *dir = opendir(arg);
        if (!dir) { fprintf(stderr, "Warning: cannot open dir '%s'\n", arg); continue; }
        std::vector<std::string> found;
        struct dirent *ent;
        while ((ent = readdir(dir))) {
            size_t nlen = strlen(ent->d_name);
            if (nlen < 4) continue;
            if (strcasecmp(ent->d_name + nlen - 4, ".hmp") == 0)
                found.push_back(std::string(arg) + "/" + ent->d_name);
        }
        closedir(dir);
        if (found.empty()) { fprintf(stderr, "Warning: no .hmp files in '%s'\n", arg); continue; }
        std::sort(found.begin(), found.end());
        for (auto &p : found) result.push_back(p);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <file.hmp | directory> [...]\n"
        "\n"
        "Options:\n"
        "  -d device        RetroWave serial device (default: /dev/ttyACM0)\n"
        "  -m melodic.bnk   Melodic bank (default: intmelo.bnk for OPL)\n"
        "  -r drums.bnk     Percussion bank (default: intdrum.bnk for OPL)\n"
        "  -D N             HMI device index for track selection (default: 0)\n"
        "                     0=OPL/AdLib  1=MT-32  2=GM  3=Roland GS  4=Tandy\n"
        "  -l               Loop playlist indefinitely\n"
        "  -t scale         Tempo multiplier (default: 1.0)\n"
        "  -v               Verbose: print track mapping and every MIDI event\n"
        "  -h, --help       Show this help\n"
        "\n"
        "Extract BNK files from the game archive with hogtool:\n"
        "  ./hogtool extract descent.hog  -o ./banks/ intmelo.bnk intdrum.bnk\n"
        "  ./hogtool extract descent2.hog -o ./banks/ intmelo.bnk intdrum.bnk\n"
        "  (also available: melodic.bnk drum.bnk hammelo.bnk hamdrum.bnk)\n"
        "\n"
        "Keys during playback:\n"
        "  n / Space  next   p  previous   l  loop   +/-  tempo   q  quit\n"
        "\n"
        "Build:  g++ -std=c++17 -O2 -o hmpplay_opl3 hmpplay_opl3.cpp\n",
        prog);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    const char *device   = "/dev/ttyACM0";
    const char *mel_path = nullptr;
    const char *drm_path = nullptr;
    PlayOptions opts;
    std::vector<const char*> raw_files;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i+1 < argc) { device   = argv[++i]; }
        else if (!strcmp(argv[i], "-m") && i+1 < argc) { mel_path = argv[++i]; }
        else if (!strcmp(argv[i], "-r") && i+1 < argc) { drm_path = argv[++i]; }
        else if (!strcmp(argv[i], "-l"))  { opts.loop = true; }
        else if (!strcmp(argv[i], "-v"))  { opts.verbose = true; }
        else if (!strcmp(argv[i], "-D") && i+1 < argc) {
            opts.device = atoi(argv[++i]);
            if (opts.device < 0 || opts.device > 4) {
                fprintf(stderr, "Device index must be 0-4\n"); return 1; }
        }
        else if (!strcmp(argv[i], "-t") && i+1 < argc) {
            opts.tempo_scale = atof(argv[++i]);
            if (opts.tempo_scale <= 0) { fprintf(stderr, "tempo must be > 0\n"); return 1; }
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else if (argv[i][0] == '-') { fprintf(stderr, "Unknown option: %s\n", argv[i]); usage(argv[0]); return 1; }
        else raw_files.push_back(argv[i]);
    }

    if (raw_files.empty()) { usage(argv[0]); return 1; }

    // Load instrument banks
    std::vector<BnkInstrument> melodic_bank, drums_bank;
    if (mel_path) {
        try { melodic_bank = load_bnk(mel_path); printf("Loaded melodic bank: %s\n", mel_path); }
        catch (const std::exception &e) { fprintf(stderr, "Warning: %s\n", e.what()); }
    }
    if (drm_path) {
        try { drums_bank = load_bnk(drm_path); printf("Loaded drums bank: %s\n", drm_path); }
        catch (const std::exception &e) { fprintf(stderr, "Warning: %s\n", e.what()); }
    }
    if (melodic_bank.empty() && drums_bank.empty()) {
        fprintf(stderr, "Warning: no instrument banks loaded — use -m and -r\n"
                        "         Sound will be silent. Extract banks from the game HOG first.\n");
    }

    // Open serial device
    SerialPort serial;
    try { serial.open(device); printf("Opened: %s\n", device); }
    catch (const std::exception &e) { fprintf(stderr, "Error: %s\n", e.what()); return 1; }

    // Init OPL3
    OPL3 opl(serial);
    if (!melodic_bank.empty()) opl.melodic = &melodic_bank;
    if (!drums_bank.empty())   opl.drums   = &drums_bank;
    opl.init();
    printf("OPL3 initialized\n");

    // Expand directories → HMP file list
    auto files = expand_args(raw_files);
    if (files.empty()) { fprintf(stderr, "Error: no .hmp files to play\n"); return 1; }
    printf("Playlist: %d file(s)\n", (int)files.size());

    // Start keyboard thread
    RawTerm rawterm; rawterm.enable();
    std::thread key_thread(key_thread_fn, &opts);

    // Play loop
    int idx = 0;
    while (!g_stop && idx < (int)files.size()) {
        printf("\n[%d/%d] %s\n", idx+1, (int)files.size(), files[idx].c_str());

        std::unique_ptr<HmpFile> hmp;
        try { hmp = hmp_open(files[idx].c_str()); }
        catch (const std::exception &e) {
            fprintf(stderr, "  Error: %s\n", e.what());
            idx++; continue;
        }

        g_skip = false; g_prev = false;
        play_hmp(opl, *hmp, opts);
        opl.all_notes_off();     // silence between tracks

        if (g_stop) break;
        if (g_prev) idx = std::max(0, idx-1);
        else        idx++;
    }

    g_stop = true;
    key_thread.join();
    rawterm.disable();
    opl.all_notes_off();
    printf("\nDone.\n");
    return 0;
}
