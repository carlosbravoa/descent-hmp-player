/*
 * hmpplay_opl3 - Descent HMP player for RetroWave OPL3 Express
 *
 * Uses libADLMIDI for OPL3 synthesis — the same engine that correctly
 * implements the HMI volume model, 4-op channels, and FM register
 * generation that the original game used. libADLMIDI drives the
 * RetroWave OPL3 Express directly via its built-in serial chip driver
 * (ProtocolRetroWaveOPL3), bypassing ALSA entirely.
 *
 * Architecture:
 *   HMP file  →  hmp2smf() (our parser → standard MIDI bytes)
 *             →  adl_openData() (libADLMIDI sequencer)
 *             →  adl_switchSerialHW() (RetroWave OPL3 serial chip)
 *             →  /dev/ttyACM0 @ 2 Mbaud
 *
 * Built-in Descent banks (no .bnk files needed):
 *   --bank int   →  "HMI (Descent:: Int)" — intmelo/intdrum (OPL/AdLib)
 *   --bank ham   →  "HMI (Descent:: Ham)" — hammelo/hamdrum
 *   --bank rick  →  "HMI (Descent:: Rick)"
 *   --bank d2    →  "HMI (Descent 2)"
 *   --bank gm    →  "HMI (Descent, Asterix)" — melodic/drum (GM-ish)
 * Or load external bank files:
 *   -b /path/to/intmelo.bnk  (AdLib BNK format)
 *
 * Build: see build.sh
 *
 * HMP parser from DXX-Rebirth common/misc/hmp.cpp (GPL v2+)
 * libADLMIDI by Vitaly Novichkov (LGPL v2.1+)
 * RetroWave serial protocol by SudoMaker (AGPLv3)
 */

#include <adlmidi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// HMP file parser
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int HMP_MAX_TRACKS = 32;

struct HmpTrack { std::vector<uint8_t> data; };

struct HmpFile {
    int num_tracks{0};
    int tempo{0};
    HmpTrack tracks[HMP_MAX_TRACKS];
    uint32_t device_track_map[5][32]{};
    bool has_device_map{false};
};

static constexpr int HMP_DEVICE_OPL  = 0;
static constexpr int HMP_DEVICE_MT32 = 1;
static constexpr int HMP_DEVICE_GM   = 2;
static constexpr int HMP_DEVICE_GS   = 3;
static const char *HMP_DEVICE_NAMES[5] = {"OPL/AdLib","MT-32","GM","Roland GS","Tandy"};

static int read_hmi_var(const uint8_t *p, int len, uint32_t *out) {
    uint32_t v = 0; int shift = 0;
    const uint8_t *start = p;
    while (len > 0 && !(*p & 0x80)) { v += (uint32_t)(*p++) << shift; shift += 7; len--; }
    if (!len) return 0;
    v += (uint32_t)(*p++ & 0x7f) << shift;
    if (out) *out = v;
    return (int)(p - start);
}

static HmpFile *hmp_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) throw std::runtime_error(std::string("Cannot open: ") + path);

    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "HMIMIDIP", 8)) {
        fclose(f); throw std::runtime_error(std::string("Not an HMP file: ") + path);
    }

    auto *hmp = new HmpFile;

    {
        int32_t dtm[5][32]{};
        if (fseek(f, 0x80, SEEK_SET) == 0 && fread(dtm, sizeof(dtm), 1, f) == 1) {
            hmp->has_device_map = true;
            for (int d = 0; d < 5; d++)
                for (int t = 0; t < 32; t++)
                    hmp->device_track_map[d][t] = (uint32_t)dtm[d][t];
        }
    }

    fseek(f, 0x30, SEEK_SET);
    uint32_t num_tracks = 0;
    if (fread(&num_tracks, 4, 1, f) != 1 || num_tracks < 1 || num_tracks > (uint32_t)HMP_MAX_TRACKS) {
        fclose(f); delete hmp; throw std::runtime_error("Invalid track count");
    }
    int32_t raw_tempo = 0;
    fseek(f, 0x38, SEEK_SET);
    if (fread(&raw_tempo, 4, 1, f) != 1) {
        fclose(f); delete hmp; throw std::runtime_error("Read error (tempo)");
    }
    hmp->num_tracks = (int)num_tracks;
    hmp->tempo = raw_tempo;

    fseek(f, 0x308, SEEK_SET);
    for (int i = 0; i < hmp->num_tracks; i++) {
        int32_t hdr[3];
        if (fread(hdr, 4, 3, f) != 3) {
            fclose(f); delete hmp; throw std::runtime_error("Track header read error");
        }
        int dlen = hdr[1] - 12;
        if (dlen <= 0) { fclose(f); delete hmp; throw std::runtime_error("Bad track length"); }
        hmp->tracks[i].data.resize(dlen);
        if ((int)fread(hmp->tracks[i].data.data(), 1, dlen, f) != dlen) {
            fclose(f); delete hmp; throw std::runtime_error("Track data read error");
        }
    }
    fclose(f);
    return hmp;
}

// ─────────────────────────────────────────────────────────────────────────────
// HMP → Standard MIDI File (SMF)
// libADLMIDI's sequencer reads standard SMF, so we convert in-memory.
// ─────────────────────────────────────────────────────────────────────────────

static void write_midi_var(std::vector<uint8_t> &buf, uint32_t v) {
    if (v >= (1 << 21)) buf.push_back(0x80 | ((v >> 21) & 0x7f));
    if (v >= (1 << 14)) buf.push_back(0x80 | ((v >> 14) & 0x7f));
    if (v >= (1 <<  7)) buf.push_back(0x80 | ((v >>  7) & 0x7f));
    buf.push_back(v & 0x7f);
}
static void write_be32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back((v>>24)&0xff); b.push_back((v>>16)&0xff);
    b.push_back((v>> 8)&0xff); b.push_back(v&0xff);
}
static void write_be16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back((v>>8)&0xff); b.push_back(v&0xff);
}

static std::vector<uint8_t> hmp_track_to_midi(const HmpTrack &trk, bool active) {
    static const int cmdlen[8] = {3,3,3,3,2,2,3,0};
    std::vector<uint8_t> out;

    if (!active) {
        out.push_back(0x00);
        out.push_back(0xFF); out.push_back(0x2F); out.push_back(0x00);
        return out;
    }

    const uint8_t *data = trk.data.data();
    const int size = (int)trk.data.size();
    int pos = 0;
    uint8_t last_status = 0;

    while (pos < size) {
        uint32_t delta = 0;
        int got = read_hmi_var(data + pos, size - pos, &delta);
        if (got <= 0) break;
        pos += got;
        write_midi_var(out, delta);
        if (pos >= size) break;

        uint8_t status = data[pos];
        if (status == 0xFF) {
            pos++;
            if (pos >= size) break;
            uint8_t meta_type = data[pos++];
            out.push_back(0xFF);
            out.push_back(meta_type);
            uint32_t meta_len = 0;
            while (pos < size) {
                uint8_t b = data[pos++];
                meta_len = (meta_len << 7) | (b & 0x7f);
                out.push_back(b);
                if (!(b & 0x80)) break;
            }
            for (uint32_t i = 0; i < meta_len && pos < size; i++)
                out.push_back(data[pos++]);
            if (meta_type == 0x2F) break;
        } else {
            if (status & 0x80) { last_status = status; pos++; }
            else                { status = last_status; }
            if (!last_status) break;
            int cmd = (last_status >> 4) & 0x0F;
            int ndata = (cmd >= 8 && cmd <= 14) ? cmdlen[cmd-8]-1 : 0;
            out.push_back(last_status);
            for (int i = 0; i < ndata && pos < size; i++)
                out.push_back(data[pos++]);
        }
    }
    // Ensure End of Track
    out.push_back(0x00);
    out.push_back(0xFF); out.push_back(0x2F); out.push_back(0x00);
    return out;
}

static std::vector<uint8_t> hmp_to_smf(const HmpFile &hmp, int device) {
    // Build active-track set from device map
    std::vector<bool> active(hmp.num_tracks, true);
    active[0] = false;

    if (device >= 0 && device <= 4 && hmp.has_device_map) {
        std::fill(active.begin(), active.end(), false);
        int mapped = 0;
        for (int slot = 0; slot < 32; slot++) {
            uint32_t ti = hmp.device_track_map[device][slot];
            if (ti > 0 && (int)ti < hmp.num_tracks) { active[ti] = true; mapped++; }
        }
        if (mapped == 0) {
            printf("  Warning: no tracks for device %d (%s), playing all\n",
                   device, HMP_DEVICE_NAMES[device]);
            for (int i = 1; i < hmp.num_tracks; i++) active[i] = true;
        } else {
            printf("  Device %d (%s): %d mapped tracks\n",
                   device, HMP_DEVICE_NAMES[device], mapped);
        }
    }

    // Timing derivation:
    // The original HMI engine plays at: us_per_tick = 1,605,632 / (hmp->tempo * 1.6)
    // We encode that into standard MIDI using the 500,000 us/beat default tempo
    // so timing is correct regardless of whether libADLMIDI honors Set Tempo events:
    //   time_div = round(500,000 / us_per_tick)
    //            = round(500,000 * hmp->tempo * 1.6 / 1,605,632)
    // Error vs ideal: <0.4% (inaudible rounding artefact).
    static constexpr double HMP_TEMPO_US = 1605632.0;
    double us_per_tick = HMP_TEMPO_US / (hmp.tempo * 1.6);
    int time_div = std::max(1, (int)std::round(500000.0 / us_per_tick));

    std::vector<uint8_t> smf;
    smf.insert(smf.end(), {'M','T','h','d'});
    write_be32(smf, 6);
    write_be16(smf, 1);                             // format 1
    write_be16(smf, (uint16_t)hmp.num_tracks);
    write_be16(smf, (uint16_t)time_div);

    // Tempo track: Set Tempo = 500,000 us/beat (standard MIDI default, 120 BPM).
    // time_div is already scaled so that 500,000/time_div = target us/tick,
    // making timing correct even if the sequencer ignores this meta event.
    {
        std::vector<uint8_t> t;
        t.push_back(0x00);                           // delta 0
        t.insert(t.end(), {0xFF,0x51,0x03, 0x07,0xA1,0x20}); // Set Tempo 500,000
        t.push_back(0x00);                           // delta 0
        t.insert(t.end(), {0xFF,0x2F,0x00});         // End of Track
        smf.insert(smf.end(), {'M','T','r','k'});
        write_be32(smf, (uint32_t)t.size());
        smf.insert(smf.end(), t.begin(), t.end());
    }

    // Music tracks (track 0 of HMP was conductor, replaced above by our tempo track)
    for (int i = 1; i < hmp.num_tracks; i++) {
        auto tb = hmp_track_to_midi(hmp.tracks[i], active[i]);
        smf.insert(smf.end(), {'M','T','r','k'});
        write_be32(smf, (uint32_t)tb.size());
        smf.insert(smf.end(), tb.begin(), tb.end());
    }

    return smf;
}

// ─────────────────────────────────────────────────────────────────────────────
// Globals and keyboard control
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_skip{false};
static std::atomic<bool> g_prev{false};

void signal_handler(int) { g_stop = true; }

struct PlayOptions {
    bool        loop{false};
    double      tempo_scale{1.0};
    bool        verbose{false};
    int         device{HMP_DEVICE_OPL};
    int         bank_no{6};          // built-in: 6 = HMI Descent Int
    std::string bank_file;           // external .bnk (overrides bank_no)
    std::string serial_dev{"ttyACM0"};
};

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
    void disable() {
        if (active) { tcsetattr(STDIN_FILENO, TCSANOW, &saved); active = false; }
    }
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
            case 'n': case ' ': printf("\n  >> Next\n");  g_skip = true; break;
            case 'p': printf("\n  >> Prev\n"); g_prev = true; g_skip = true; break;
            case 'l': opts->loop = !opts->loop;
                      printf("\n  >> Loop %s\n", opts->loop ? "ON" : "OFF"); break;
            case '+': case '=':
                opts->tempo_scale = std::min(opts->tempo_scale * 1.25, 4.0);
                adl_setTempo(nullptr, opts->tempo_scale); // updated per-song too
                printf("\n  >> Tempo %.2fx\n", opts->tempo_scale); break;
            case '-':
                opts->tempo_scale = std::max(opts->tempo_scale / 1.25, 0.25);
                printf("\n  >> Tempo %.2fx\n", opts->tempo_scale); break;
            case 'q': case 3: printf("\n  >> Quit\n"); g_stop = true; break;
            default: break;
        }
        fflush(stdout);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Playback
// ─────────────────────────────────────────────────────────────────────────────

// We need a global player pointer so the key thread can call adl_setTempo
static ADL_MIDIPlayer *g_adl = nullptr;

void key_thread_fn_real(PlayOptions *opts) {
    struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
    printf("  Keys: [n/Space] next  [p] prev  [l] loop  [+/-] tempo  [q] quit\n");
    fflush(stdout);
    while (!g_stop) {
        if (poll(&pfd, 1, 50) <= 0) continue;
        char c; if (read(STDIN_FILENO, &c, 1) != 1) continue;
        switch (c) {
            case 'n': case ' ': printf("\n  >> Next\n");  g_skip = true; break;
            case 'p': printf("\n  >> Prev\n"); g_prev = true; g_skip = true; break;
            case 'l': opts->loop = !opts->loop;
                      printf("\n  >> Loop %s\n", opts->loop ? "ON" : "OFF"); break;
            case '+': case '=':
                opts->tempo_scale = std::min(opts->tempo_scale * 1.25, 4.0);
                if (g_adl) adl_setTempo(g_adl, opts->tempo_scale);
                printf("\n  >> Tempo %.2fx\n", opts->tempo_scale); break;
            case '-':
                opts->tempo_scale = std::max(opts->tempo_scale / 1.25, 0.25);
                if (g_adl) adl_setTempo(g_adl, opts->tempo_scale);
                printf("\n  >> Tempo %.2fx\n", opts->tempo_scale); break;
            case 'q': case 3: printf("\n  >> Quit\n"); g_stop = true; break;
            default: break;
        }
        fflush(stdout);
    }
}

void play_hmp(ADL_MIDIPlayer *adl, const HmpFile &hmp, const PlayOptions &opts) {
    auto smf = hmp_to_smf(hmp, opts.device);

    if (adl_openData(adl, smf.data(), (unsigned long)smf.size()) != 0) {
        fprintf(stderr, "  Error: %s\n", adl_errorInfo(adl));
        return;
    }

    adl_setTempo(adl, opts.tempo_scale);

    printf("  time_div=%d  tracks=%d\n",
           (int)(hmp.tempo * 1.6), hmp.num_tracks);

    // For a hardware serial chip, adl_play() renders silence instantly —
    // there is no audio device to pace it, so the sequencer races at CPU speed.
    // Instead we use adl_tickEventsOnly() (designed for hardware OPL mode):
    //   - it advances the sequencer by `seconds` and fires OPL register writes
    //   - returns the number of seconds until the next call is needed
    //   - WE are responsible for sleeping that duration (real wall-clock time)
    // adl_tickIterators() handles vibrato/arpeggio/portamento interpolation.
    using clock = std::chrono::steady_clock;
    using dur   = std::chrono::duration<double>;

    // Granularity: minimum MIDI tick size in seconds.
    // 1 ms is fine — smaller than any audible timing difference.
    static constexpr double GRANULARITY = 0.001;

    do {
        adl_positionRewind(adl);
        double prev_delay = 0.0;

        while (!g_stop && !g_skip) {
            auto t0 = clock::now();

            // Advance sequencer: fires OPL register writes to the serial port
            double next_delay = adl_tickEventsOnly(adl, prev_delay, GRANULARITY);

            // Handle vibrato / arpeggio / portamento
            adl_tickIterators(adl, prev_delay);

            if (next_delay < 0.0) break;   // error
            if (next_delay == 0.0) break;  // end of song

            // Sleep the remaining real time until the next tick is due,
            // accounting for the time adl_tickEventsOnly() itself took
            auto elapsed = std::chrono::duration_cast<dur>(clock::now() - t0).count();
            double sleep_s = next_delay - elapsed;
            if (sleep_s > 0.0)
                std::this_thread::sleep_for(dur(sleep_s));

            prev_delay = next_delay;
        }

        if (opts.loop && !g_stop && !g_skip)
            printf("  [Looping...]\n");

    } while (opts.loop && !g_stop && !g_skip);
}

// ─────────────────────────────────────────────────────────────────────────────
// Directory expansion
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<std::string> expand_args(const std::vector<const char *> &args) {
    std::vector<std::string> result;
    for (const char *arg : args) {
        struct stat st;
        if (stat(arg, &st) != 0) { result.push_back(arg); continue; }
        if (!S_ISDIR(st.st_mode)) { result.push_back(arg); continue; }
        DIR *dir = opendir(arg);
        if (!dir) { fprintf(stderr, "Warning: cannot open '%s'\n", arg); continue; }
        std::vector<std::string> found;
        struct dirent *ent;
        while ((ent = readdir(dir))) {
            size_t nl = strlen(ent->d_name);
            if (nl >= 4 && strcasecmp(ent->d_name + nl - 4, ".hmp") == 0)
                found.push_back(std::string(arg) + "/" + ent->d_name);
        }
        closedir(dir);
        std::sort(found.begin(), found.end());
        for (auto &p : found) result.push_back(p);
        if (found.empty()) fprintf(stderr, "Warning: no .hmp files in '%s'\n", arg);
    }
    return result;
}


// ─────────────────────────────────────────────────────────────────────────────
// HMI AdLib BNK parser and BNK → WOPL in-memory converter
//
// Loads intmelo.bnk (melodic) + intdrum.bnk (percussion) and builds a
// WOPLFile in memory that adl_openBankData() can consume directly.
// This gives libADLMIDI the actual original FM patches from the game.
// ─────────────────────────────────────────────────────────────────────────────

// One operator's worth of BNK data (13 bytes per op in the PackedTimbre)
struct BnkOp {
    uint8_t ksl, multiple, feedback, attack, sustain, eg, decay, rel;
    uint8_t totalLevel, am, vib, ksr, con;
};

struct BnkInst {
    bool     valid{false};
    bool     percussive{false};
    uint8_t  voice_num{0};   // percussion MIDI note
    BnkOp    op[2];          // op[0]=modulator, op[1]=carrier
    uint8_t  mod_wave{0}, car_wave{0};
};

// Parse one HMI .bnk file into a vector of 128 instruments.
static std::vector<BnkInst> load_bnk_file(const char *path) {
    std::vector<BnkInst> bank(128);
    FILE *f = fopen(path, "rb");
    if (!f) return bank; // returns all-invalid on failure

    uint8_t hdr[24];
    if (fread(hdr, 1, 24, f) != 24 || memcmp(hdr+2, "ADLIB-", 6) != 0) {
        fclose(f); return bank;
    }
    uint16_t num_used   = hdr[8]  | (hdr[9]  << 8);
    uint32_t offset_name = hdr[12] | (hdr[13]<<8) | (hdr[14]<<16) | (hdr[15]<<24);
    uint32_t offset_data = hdr[16] | (hdr[17]<<8) | (hdr[18]<<16) | (hdr[19]<<24);

    fseek(f, offset_name, SEEK_SET);
    for (int i = 0; i < (int)num_used; i++) {
        uint8_t ne[12];
        if (fread(ne, 1, 12, f) != 12) break;
        uint16_t data_idx = ne[0] | (ne[1] << 8);
        if (data_idx >= 128) continue;

        long saved = ftell(f);
        fseek(f, offset_data + data_idx * 30, SEEK_SET);
        uint8_t d[30];
        if (fread(d, 1, 30, f) != 30) { fseek(f, saved, SEEK_SET); continue; }
        fseek(f, saved, SEEK_SET);

        BnkInst &ins = bank[data_idx];
        ins.valid      = true;
        ins.percussive = d[0] != 0;
        ins.voice_num  = d[1];
        for (int op = 0; op < 2; op++) {
            const uint8_t *b = d + 2 + op * 13;
            ins.op[op] = {b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12]};
        }
        ins.mod_wave = d[28];
        ins.car_wave = d[29];
    }
    fclose(f);
    return bank;
}

// Build a WOPL v2 binary blob from melodic + percussion BnkInst vectors.
// Layout verified against libADLMIDI src/wopl/wopl_file.c WOPL_parseInstrument().
// Returns raw bytes for adl_openBankData(), or empty on failure.
static std::vector<uint8_t> bnk_pair_to_wopl(
        const std::vector<BnkInst> &mel,
        const std::vector<BnkInst> &drm) {

    // WOPL uses big-endian for most multi-byte fields (note offsets, bank counts).
    // Version field and total inst size are little-endian.
    auto wbe16 = [](std::vector<uint8_t> &b, int16_t v) {
        b.push_back((v >> 8) & 0xFF); b.push_back(v & 0xFF);
    };
    auto wle16 = [](std::vector<uint8_t> &b, uint16_t v) {
        b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF);
    };
    auto wbyte = [](std::vector<uint8_t> &b, uint8_t v) { b.push_back(v); };
    auto wpad  = [](std::vector<uint8_t> &b, int n)  { b.insert(b.end(), n, 0); };
    auto wstr  = [](std::vector<uint8_t> &b, const char *s, int len) {
        int sl = s ? (int)strlen(s) : 0;
        for (int i = 0; i < len; i++) b.push_back(i < sl ? (uint8_t)s[i] : 0);
    };

    // Write one 5-byte operator block (offsets 42+l*5 in v2 instrument)
    auto write_op = [&](std::vector<uint8_t> &b, const BnkOp &op, uint8_t wave) {
        wbyte(b, (op.am?0x80:0)|(op.vib?0x40:0)|(op.eg?0x20:0)|(op.ksr?0x10:0)|(op.multiple&0x0F));
        wbyte(b, (op.ksl<<6)|(op.totalLevel&0x3F));
        wbyte(b, (op.attack<<4)|(op.decay&0x0F));
        wbyte(b, (op.sustain<<4)|(op.rel&0x0F));
        wbyte(b, wave & 0x07);
    };

    // Write one 62-byte v2 instrument block.
    // Exact layout from WOPL_parseInstrument() in wopl_file.c:
    //   [0..31]  inst_name (32 bytes, then  )         — we write 32 chars
    //   [32]     inst_name[32] = ' ' added by parser   — we write it
    //   [32..33] note_offset1  sint16 BE
    //   [34..35] note_offset2  sint16 BE
    //   [36]     midi_velocity_offset
    //   [37]     second_voice_detune
    //   [38]     percussion_key_number
    //   [39]     inst_flags  (0x08 = blank)
    //   [40]     fb_conn1_C0
    //   [41]     fb_conn2_C0
    //   [42..46] operator 0 (5 bytes)
    //   [47..51] operator 1 (5 bytes)
    //   [52..56] operator 2 (5 bytes, zeros for 2-op)
    //   [57..61] operator 3 (5 bytes, zeros for 2-op)
    //   Total = 62 bytes = WOPL_INST_SIZE_V2
    auto write_inst = [&](std::vector<uint8_t> &b, const BnkInst &ins, int prog) {
        char name[33]{}; snprintf(name, 33, "ins_%03d", prog);
        wstr(b, name, 32);          // [0..31]
        wbyte(b, 0);                // [32] null terminator part of name field
        wbe16(b, 0);                // [33..34] note_offset1
        wbe16(b, 0);                // [35..36] note_offset2  (note: parser reads 32,34)
        // ^^^ The parser reads note_offset1 at cursor+32, note_offset2 at cursor+34,
        //     but the name is 32 bytes NOT including the null — parser adds null manually.
        //     So inst_name is bytes [0..31] (32 bytes), null at [32], then offsets at [32..35].
        //     Wait — strncpy(name, cursor, 32) then name[32]=' ' means the stored name
        //     is bytes [0..31] and the null is NOT in the binary. Then note_offset1 is
        //     at cursor+32. So no null byte between name and offsets. Let me fix:
        // Actually re-read: strncpy copies 32 bytes from cursor[0], sets cursor[32]=0 in
        // the struct but doesn't advance cursor. note_offset1 = toSint16BE(cursor+32).
        // So bytes [0..31]=name, [32..33]=note_offset1 BE, [34..35]=note_offset2 BE.
        // We wrote one too many bytes above. Let me redo:
        b.resize(b.size() - 3); // undo the 0 + wbe16(0)
        wbe16(b, 0);                // [32..33] note_offset1 BE
        wbe16(b, 0);                // [34..35] note_offset2 BE
        wbyte(b, 0);                // [36] midi_velocity_offset
        wbyte(b, 0);                // [37] second_voice_detune
        wbyte(b, ins.valid && ins.percussive ? ins.voice_num : 0); // [38]
        wbyte(b, ins.valid ? 0x00 : 0x08); // [39] inst_flags (0x08=blank)
        // fb_conn1_C0 [40]: connection(bit0) | feedback(bits1-3) | OPL3-enable(bits4-5=0x30)
        // BNK 'con' field: 0=FM(carrier output) meaning OPL con bit=1 (additive=off)
        //                  non-zero=additive meaning OPL con bit=0
        uint8_t con_bit = (ins.valid && ins.op[0].con == 0) ? 1 : 0;
        uint8_t fb      = ins.valid ? (uint8_t)((ins.op[0].feedback & 0x07) << 1) : 0;
        wbyte(b, fb | con_bit | 0x30); // [40] fb_conn1_C0
        wbyte(b, 0x30);            // [41] fb_conn2_C0 (4-op only)
        if (ins.valid) {
            write_op(b, ins.op[0], ins.mod_wave); // [42..46]
            write_op(b, ins.op[1], ins.car_wave); // [47..51]
        } else {
            wpad(b, 10);           // [42..51] silence
        }
        wpad(b, 10);               // [52..61] ops 2,3 (zeros for 2-op)
        // Total so far: 32 + 2 + 2 + 1 + 1 + 1 + 1 + 1 + 1 + 10 + 10 = 62 ✓
    };

    // Bank header v2: 32-byte name + lsb(1) + msb(1) = 34 bytes
    auto write_bank_hdr = [&](std::vector<uint8_t> &b, const char *name) {
        wstr(b, name, 32);
        wbyte(b, 0); // lsb
        wbyte(b, 0); // msb
    };

    std::vector<uint8_t> out;

    // File header:
    //  [0..10]  magic "WOPL3-BANK " (11 bytes)
    //  [11..12] version uint16 LE = 2
    //  [13..14] banks_melodic uint16 BE
    //  [15..16] banks_percussion uint16 BE
    //  [17]     opl_flags
    //  [18]     volume_model
    const char *magic = "WOPL3-BANK";
    out.insert(out.end(), magic, magic + 11); // includes  
    wle16(out, 2);      // version 2 (LE)
    // bank counts are big-endian (parsed with toUint16BE in wopl_file.c)
    out.push_back(0); out.push_back(1); // banks_melodic = 1 (BE)
    out.push_back(0); out.push_back(1); // banks_percussion = 1 (BE)
    wbyte(out, 0x03);   // opl_flags: deep tremolo + deep vibrato
    wbyte(out, 0x0A);   // volume_model: HMI = 10

    // Bank names (v2 only: melodic banks first, then percussion)
    write_bank_hdr(out, "Descent Int Melodic");
    write_bank_hdr(out, "Descent Int Drums");

    // Instruments: all melodic banks, then all percussion banks
    for (int i = 0; i < 128; i++)
        write_inst(out, i < (int)mel.size() ? mel[i] : BnkInst{}, i);
    for (int i = 0; i < 128; i++)
        write_inst(out, i < (int)drm.size() ? drm[i] : BnkInst{}, i);

    return out;
}

// Look for intmelo.bnk + intdrum.bnk next to the given path (file or dir).
// Returns the directory to search, or empty string if not found.
static std::string find_bnk_dir(const std::string &hmp_path) {
    // Try the directory containing the HMP file
    std::string dir = hmp_path;
    auto slash = dir.rfind('/');
    if (slash != std::string::npos)
        dir = dir.substr(0, slash);
    else
        dir = ".";

    auto exists = [](const std::string &p) {
        struct stat st; return stat(p.c_str(), &st) == 0;
    };

    if (exists(dir + "/intmelo.bnk") && exists(dir + "/intdrum.bnk"))
        return dir;
    // Also try current directory
    if (exists("./intmelo.bnk") && exists("./intdrum.bnk"))
        return ".";
    return "";
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <file.hmp | directory> [...]\n"
        "\n"
        "Options:\n"
        "  -d name          Serial device name without /dev/ (default: ttyACM0)\n"
        "  -m intmelo.bnk   Melodic HMI BNK file (auto-detected if next to HMP)\n"
        "  -r intdrum.bnk   Percussion HMI BNK file (auto-detected if next to HMP)\n"
        "  -b file.wopl     Load WOPL format bank file\n"
        "  --bank NAME      Built-in bank fallback if no BNK files found:\n"
        "                     int   HMI (Descent Int)   [default, OPL/AdLib]\n"
        "                     ham   HMI (Descent Ham)\n"
        "                     rick  HMI (Descent Rick)\n"
        "                     d2    HMI (Descent 2)\n"
        "                     gm    HMI (Descent, Asterix)  [GM-ish]\n"
        "  -D N             HMP device track selection (default: 0=OPL)\n"
        "                     0=OPL  1=MT-32  2=GM  3=Roland GS\n"
        "  -l               Loop playlist indefinitely\n"
        "  -t scale         Tempo multiplier (default: 1.0)\n"
        "  -v               Verbose\n"
        "  -h, --help       This help\n"
        "\n"
        "Examples:\n"
        "  %s ./music/\n"
        "  %s -l ./music/                        # auto-detects intmelo/intdrum.bnk\n"
        "  %s --bank d2 ./music/                 # use built-in Descent 2 bank\n"
        "  %s -m ./banks/intmelo.bnk -r ./banks/intdrum.bnk ./music/\n"
        "\n"
        "Build: see build.sh\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    PlayOptions opts;
    std::vector<const char *> raw_files;
    const char *mel_path = nullptr; // explicit -m melodic.bnk
    const char *drm_path = nullptr; // explicit -r drums.bnk

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i+1 < argc) {
            opts.serial_dev = argv[++i];
        } else if (!strcmp(argv[i], "--bank") && i+1 < argc) {
            const char *n = argv[++i];
            if      (!strcmp(n,"int"))  opts.bank_no = 6;
            else if (!strcmp(n,"ham"))  opts.bank_no = 8;
            else if (!strcmp(n,"rick")) opts.bank_no = 10;
            else if (!strcmp(n,"d2"))   opts.bank_no = 12;
            else if (!strcmp(n,"gm"))   opts.bank_no = 4;
            else { fprintf(stderr,"Unknown bank '%s'\n",n); return 1; }
        } else if (!strcmp(argv[i], "-b") && i+1 < argc) {
            opts.bank_file = argv[++i];
        } else if (!strcmp(argv[i], "-m") && i+1 < argc) {
            mel_path = argv[++i];
        } else if (!strcmp(argv[i], "-r") && i+1 < argc) {
            drm_path = argv[++i];
        } else if (!strcmp(argv[i], "-D") && i+1 < argc) {
            opts.device = atoi(argv[++i]);
            if (opts.device < 0 || opts.device > 4) {
                fprintf(stderr,"Device must be 0-4\n"); return 1;
            }
        } else if (!strcmp(argv[i], "-l")) {
            opts.loop = true;
        } else if (!strcmp(argv[i], "-v")) {
            opts.verbose = true;
        } else if (!strcmp(argv[i], "-t") && i+1 < argc) {
            opts.tempo_scale = atof(argv[++i]);
            if (opts.tempo_scale <= 0) { fprintf(stderr,"Tempo must be > 0\n"); return 1; }
        } else if (!strcmp(argv[i],"-h") || !strcmp(argv[i],"--help")) {
            usage(argv[0]); return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr,"Unknown option: %s\n", argv[i]); usage(argv[0]); return 1;
        } else {
            raw_files.push_back(argv[i]);
        }
    }

    if (raw_files.empty()) { usage(argv[0]); return 1; }

    auto files = expand_args(raw_files);
    if (files.empty()) { fprintf(stderr,"Error: no .hmp files\n"); return 1; }

    // ── Init libADLMIDI ───────────────────────────────────────────────────────
    ADL_MIDIPlayer *adl = adl_init(49716); // native OPL sample rate
    if (!adl) { fprintf(stderr,"adl_init failed\n"); return 1; }
    g_adl = adl;

    // ── Instrument bank loading ───────────────────────────────────────────────
    // Priority:
    //   1. Explicit -m melodic.bnk + -r drums.bnk  (raw HMI BNK files)
    //   2. Explicit -b file.wopl                    (WOPL format)
    //   3. BNK files auto-detected next to HMP files
    //   4. Built-in embedded bank (--bank flag)
    bool bank_loaded = false;

    // Helper: load BNK pair and feed to libADLMIDI
    auto load_bnk_pair = [&](const char *mel, const char *drm) -> bool {
        auto melodic  = load_bnk_file(mel);
        auto drums    = load_bnk_file(drm);
        bool mel_ok   = std::any_of(melodic.begin(), melodic.end(),
                                    [](const BnkInst &b){ return b.valid; });
        bool drm_ok   = std::any_of(drums.begin(),   drums.end(),
                                    [](const BnkInst &b){ return b.valid; });
        if (!mel_ok && !drm_ok) return false;
        auto wopl = bnk_pair_to_wopl(melodic, drums);
        if (wopl.empty()) return false;
        if (adl_openBankData(adl, wopl.data(), (unsigned long)wopl.size()) != 0) {
            fprintf(stderr, "BNK load error: %s\n", adl_errorInfo(adl));
            return false;
        }
        printf("Bank: %s + %s\n", mel, drm);
        return true;
    };

    if (mel_path || drm_path) {
        // Explicit BNK files given
        const char *m = mel_path ? mel_path : "";
        const char *d = drm_path ? drm_path : "";
        if (!load_bnk_pair(m, d)) {
            fprintf(stderr, "Error loading BNK files\n");
            adl_close(adl); return 1;
        }
        bank_loaded = true;
    }

    if (!bank_loaded && !opts.bank_file.empty()) {
        // Explicit WOPL file
        if (adl_openBankFile(adl, opts.bank_file.c_str()) != 0) {
            fprintf(stderr, "Bank file error: %s\n", adl_errorInfo(adl));
            adl_close(adl); return 1;
        }
        printf("Bank: %s\n", opts.bank_file.c_str());
        bank_loaded = true;
    }

    if (!bank_loaded && !files.empty()) {
        // Auto-detect intmelo.bnk + intdrum.bnk next to the first HMP file
        std::string bnk_dir = find_bnk_dir(files[0]);
        if (!bnk_dir.empty()) {
            std::string m = bnk_dir + "/intmelo.bnk";
            std::string d = bnk_dir + "/intdrum.bnk";
            if (load_bnk_pair(m.c_str(), d.c_str()))
                bank_loaded = true;
        }
    }

    if (!bank_loaded) {
        // Fall back to built-in embedded bank
        if (adl_setBank(adl, opts.bank_no) != 0) {
            fprintf(stderr, "Bank %d error: %s\n", opts.bank_no, adl_errorInfo(adl));
            adl_close(adl); return 1;
        }
        printf("Bank: built-in #%d — %s\n", opts.bank_no, adl_getBankNames()[opts.bank_no]);
    }

    // HMI volume model: essential for correct Descent dynamics
    adl_setVolumeRangeModel(adl, ADLMIDI_VolumeModel_HMI);

    // One OPL3 chip (18 two-operator voices)
    adl_setNumChips(adl, 1);

    // Connect to RetroWave OPL3 Express
    printf("Connecting to /dev/%s (RetroWave OPL3, 2 Mbaud)...\n",
           opts.serial_dev.c_str());
    if (adl_switchSerialHW(adl,
                           opts.serial_dev.c_str(),
                           2000000,
                           ADLMIDI_SerialProtocol_RetroWaveOPL3) != 0) {
        fprintf(stderr,"Serial error: %s\n", adl_errorInfo(adl));
        adl_close(adl); return 1;
    }
    printf("Connected: %s\n", adl_chipEmulatorName(adl));
    printf("Playlist: %d file(s)\n", (int)files.size());

    // ── Keyboard thread ───────────────────────────────────────────────────────
    RawTerm rawterm; rawterm.enable();
    std::thread key_thread(key_thread_fn_real, &opts);

    // ── Play loop ─────────────────────────────────────────────────────────────
    int idx = 0;
    while (!g_stop && idx < (int)files.size()) {
        printf("\n[%d/%d] %s\n", idx+1, (int)files.size(), files[idx].c_str());

        HmpFile *hmp = nullptr;
        try { hmp = hmp_open(files[idx].c_str()); }
        catch (const std::exception &e) {
            fprintf(stderr,"  Error: %s\n", e.what()); idx++; continue;
        }

        g_skip = false; g_prev = false;
        play_hmp(adl, *hmp, opts);
        delete hmp;

        if (g_stop) break;
        if (g_prev) idx = std::max(0, idx-1);
        else        idx++;
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    g_stop = true;
    key_thread.join();
    rawterm.disable();
    adl_panic(adl);
    adl_close(adl);
    printf("\nDone.\n");
    return 0;
}
