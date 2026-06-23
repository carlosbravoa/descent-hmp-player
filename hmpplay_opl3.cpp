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
 * Instrument banks:
 *   By default the bank is chosen PER SONG from descent.sng (Descent's own
 *   song→bank list), mapping each song's melodic .bnk to the matching built-in
 *   Descent bank. This is what makes a mixed soundtrack folder sound right —
 *   game12 wants melodic, the intro wants ham, etc. Pass --bank/-b to override
 *   and force one bank for everything.
 *
 *   --bank int   →  "HMI (Descent:: Int)" — intmelo/intdrum (OPL/AdLib)
 *   --bank ham   →  "HMI (Descent:: Ham)" — hammelo/hamdrum
 *   --bank rick  →  "HMI (Descent:: Rick)"
 *   --bank d2    →  "HMI (Descent 2)"
 *   --bank gm    →  "HMI (Descent, Asterix)" — melodic/drum (GM-ish)
 *   -b file.wopl →  load an external WOPL bank file
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
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <map>
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

#ifdef HAVE_GUI
#include "viz/hmpviz.h"
// libADLMIDI register-write tap (added to its serial backend) -> our tee.
extern "C" { extern void (*g_retrowave_opl_tap)(uint16_t addr, uint8_t data); }
#endif

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

static std::vector<uint8_t> hmp_to_smf(const HmpFile &hmp, int /*device*/) {
    // Include all music tracks (1..N), exactly as DXX-Rebirth hmp2mid() does.
    // The deviceTrackMappings in the HMP header is only relevant when sending
    // raw MIDI to hardware (e.g. MT-32 needs MT-32-specific tracks). For OPL
    // synthesis via libADLMIDI, all tracks should play — libADLMIDI handles
    // the OPL register generation from whatever MIDI events it receives.
    std::vector<bool> active(hmp.num_tracks, true);
    active[0] = false; // track 0 is the HMP conductor track, always skip

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
static std::atomic<bool> g_pause{false};   // GUI pause

void signal_handler(int) { g_stop = true; }

struct PlayOptions {
    bool        loop{false};
    double      tempo_scale{1.0};
    bool        verbose{false};
    int         device{HMP_DEVICE_OPL};
    int         bank_no{-1};         // built-in fallback; resolved to Int by name
    bool        explicit_bank{false};// user forced a bank → disable per-song auto
    bool        force_hmp{false};    // --hmp: force OPL2 .hmp even when .hmq exists
    std::string bank_file;           // external .wopl (overrides bank_no)
    std::string serial_dev{"ttyACM0"};
    bool        gui{false};           // --gui: SDL channel-activity visualizer
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

#ifdef HAVE_GUI
// Apply a tempo scale from the GUI (runs on the main thread).
static void gui_apply_tempo(double t) { if (g_adl) adl_setTempo(g_adl, t); }
#endif

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
            while (g_pause && !g_stop && !g_skip)   // GUI pause: hold without ticking
                std::this_thread::sleep_for(std::chrono::milliseconds(15));

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
// Per-song instrument bank selection (descent.sng)
// ─────────────────────────────────────────────────────────────────────────────
//
// Descent ships descent.sng: a tab-separated list  <song.hmp> <melodic.bnk>
// <drum.bnk>  pairing every song with the instrument bank it was authored for.
// Each of the four melodic banks has an exact counterpart already embedded in
// libADLMIDI's bank DB (Wohlstand extracted them from these very .bnk files),
// so we honor the per-song mapping without shipping any bank data:
//
//   melodic.bnk  -> #4  "HMI (Descent, Asterix)"   (GM-complete)
//   intmelo.bnk  -> #6  "HMI (Descent:: Int)"
//   hammelo.bnk  -> #8  "HMI (Descent:: Ham)"
//   rickmelo.bnk -> #10 "HMI (Descent:: Rick)"
//
// DXX-Rebirth and the stock libADLMIDI path ignore these columns and use ONE
// bank for the whole soundtrack — the main reason mixed folders sound off.

static std::string lower_str(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Basename without directory or extension, lowercased:  a/b/GAME01.hmp -> game01
static std::string song_key(const std::string &path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return lower_str(base);
}

static std::string dir_of(const std::string &path) {
    size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash);
}

// Resolve a built-in bank index by a distinctive substring of its name. The
// bank id passed to adl_setBank() is the ORDINAL position in adl_getBankNames()
// — NOT the data-offset numbers in inst_db.cpp. Looking it up by name avoids
// the off-by-data-offset bug that had --bank int actually loading Descent 2.
static int find_bank(const char *needle) {
    const char *const *names = adl_getBankNames();
    int n = adl_getBanksCount();
    for (int i = 0; i < n; i++)
        if (names[i] && strstr(names[i], needle)) return i;
    return -1;
}

static int bank_no_for_melo(const std::string &melo) {
    std::string n = lower_str(melo);
    if (n.find("intmelo")  != std::string::npos) return find_bank("Descent:: Int");
    if (n.find("hammelo")  != std::string::npos) return find_bank("Descent:: Ham");
    if (n.find("rickmelo") != std::string::npos) return find_bank("Descent:: Rick");
    if (n.find("melodic")  != std::string::npos) return find_bank("Descent, Asterix");
    return -1;
}

// Load descent.sng from `dir` into  song_key -> built-in bank number.
static std::map<std::string,int> load_song_banks(const std::string &dir) {
    std::map<std::string,int> m;
    std::string path = (dir.empty() ? std::string(".") : dir) + "/descent.sng";
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return m;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char song[128] = {0}, melo[128] = {0};
        // %s stops at any whitespace incl. tab and the DOS CR — no \r cleanup needed
        if (sscanf(line, "%127s %127s", song, melo) < 2) continue;
        int bn = bank_no_for_melo(melo);
        if (bn >= 0) m[song_key(song)] = bn;
    }
    fclose(f);
    return m;
}

// Prefer the OPL3 (.hmq) arrangement when present — this is what Descent itself
// loads on an OPL3 sound card (the RetroWave is one). It's a genuinely different,
// richer arrangement than the OPL2 .hmp: more tracks, and channel 9 is real GM
// percussion (matching libADLMIDI's default), whereas .hmp puts a melodic voice
// on ch9 that libADLMIDI would wrongly render as drums. Same per-song bank either
// way. Use --hmp to force the OPL2 .hmp instead.
static std::string prefer_hmq(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && lower_str(path.substr(dot)) == ".hmp") {
        std::string hmq = path.substr(0, dot) + ".hmq";
        struct stat st;
        if (stat(hmq.c_str(), &st) == 0) return hmq;
    }
    return path;
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
        "  -b file.wopl     Force a custom WOPL bank for every song\n"
        "  --bank NAME      Force one built-in bank for every song (overrides\n"
        "                   the per-song default; useful for A/B testing):\n"
        "                     int   HMI (Descent Int)   [OPL/AdLib]\n"
        "                     ham   HMI (Descent Ham)\n"
        "                     rick  HMI (Descent Rick)\n"
        "                     d2    HMI (Descent 2)\n"
        "                     gm    HMI (Descent, Asterix)  [GM-ish]\n"
        "  -D N             HMP device track selection (default: 0=OPL)\n"
        "                     0=OPL  1=MT-32  2=GM  3=Roland GS\n"
        "  --hmp            Force the OPL2 .hmp arrangement even when a richer\n"
        "                     .hmq (OPL3) version exists. By default .hmq is\n"
        "                     preferred — that's what Descent loads on OPL3.\n"
        "  -l               Loop playlist indefinitely\n"
        "  -t scale         Tempo multiplier (default: 1.0)\n"
        "  --gui            Open an SDL window with an OPL3 channel-activity\n"
        "                     visualizer (18 bars) and clickable transport\n"
        "  -v               Verbose\n"
        "  -h, --help       This help\n"
        "\n"
        "By default each song's bank is chosen from descent.sng (must be in the\n"
        "same folder as the songs), and the .hmq (OPL3) arrangement is preferred\n"
        "over .hmp when present — as Descent does on an OPL3 card.\n"
        "\n"
        "Examples:\n"
        "  %s ./music/                           # per-song banks from descent.sng\n"
        "  %s -l ./music/                        # loop the whole soundtrack\n"
        "  %s --bank d2 ./music/                 # force the Descent 2 bank\n"
        "  %s -b mybank.wopl ./music/            # force a custom WOPL bank\n"
        "\n"
        "Build: see build.sh\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    PlayOptions opts;
    std::vector<const char *> raw_files;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i+1 < argc) {
            opts.serial_dev = argv[++i];
        } else if (!strcmp(argv[i], "--bank") && i+1 < argc) {
            const char *n = argv[++i];
            if      (!strcmp(n,"int"))  opts.bank_no = find_bank("Descent:: Int");
            else if (!strcmp(n,"ham"))  opts.bank_no = find_bank("Descent:: Ham");
            else if (!strcmp(n,"rick")) opts.bank_no = find_bank("Descent:: Rick");
            else if (!strcmp(n,"d2"))   opts.bank_no = find_bank("Descent 2");
            else if (!strcmp(n,"gm"))   opts.bank_no = find_bank("Descent, Asterix");
            else { fprintf(stderr,"Unknown bank '%s'\n",n); return 1; }
            if (opts.bank_no < 0) { fprintf(stderr,"Bank '%s' not in this libADLMIDI build\n",n); return 1; }
            opts.explicit_bank = true;   // force this bank for every song
        } else if (!strcmp(argv[i], "-b") && i+1 < argc) {
            opts.bank_file = argv[++i];
            opts.explicit_bank = true;
        } else if (!strcmp(argv[i], "-D") && i+1 < argc) {
            opts.device = atoi(argv[++i]);
            if (opts.device < 0 || opts.device > 4) {
                fprintf(stderr,"Device must be 0-4\n"); return 1;
            }
        } else if (!strcmp(argv[i], "--gui")) {
            opts.gui = true;
        } else if (!strcmp(argv[i], "--hmp")) {
            opts.force_hmp = true;
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

    // Fallback bank (songs with no descent.sng match) = Descent Int.
    if (opts.bank_no < 0) opts.bank_no = find_bank("Descent:: Int");
    if (opts.bank_no < 0) opts.bank_no = 0; // last resort if names ever change

    // ── Init libADLMIDI ───────────────────────────────────────────────────────
    ADL_MIDIPlayer *adl = adl_init(49716); // native OPL sample rate
    if (!adl) { fprintf(stderr,"adl_init failed\n"); return 1; }
    g_adl = adl;

    // ── Instrument bank ───────────────────────────────────────────────────────
    // Use libADLMIDI's built-in, properly-tuned Descent banks. These embed the
    // correct HMI volume model (VOLUME_MODEL=9), deep tremolo/vibrato, and the
    // instrument data the libADLMIDI author tuned against the real game — the
    // same data chip-player-js and DXX-Rebirth use. A custom .wopl can override.
    if (!opts.bank_file.empty()) {
        if (adl_openBankFile(adl, opts.bank_file.c_str()) != 0) {
            fprintf(stderr, "Bank file error: %s\n", adl_errorInfo(adl));
            adl_close(adl); return 1;
        }
        printf("Bank: %s\n", opts.bank_file.c_str());
    } else {
        if (adl_setBank(adl, opts.bank_no) != 0) {
            fprintf(stderr, "Bank %d error: %s\n", opts.bank_no, adl_errorInfo(adl));
            adl_close(adl); return 1;
        }
        if (opts.explicit_bank)
            printf("Bank: built-in #%d — %s  (forced for all songs)\n",
                   opts.bank_no, adl_getBankNames()[opts.bank_no]);
        else
            printf("Bank: per-song from descent.sng "
                   "(fallback #%d — %s)\n",
                   opts.bank_no, adl_getBankNames()[opts.bank_no]);
    }

    // Volume model: AUTO honors the bank's own VOLUME_MODEL (HMI for Descent),
    // which is the fix for the "notes sustain too long" symptom (libADLMIDI #223).
    adl_setVolumeRangeModel(adl, ADLMIDI_VolumeModel_AUTO);

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

    // ── Play loop (runs headless on the main thread, or on a worker in GUI) ────
    // Per-song banks come from descent.sng (unless --bank/-b forced one). Cache
    // the parsed list per directory; track the live bank to avoid redundant
    // adl_setBank() calls.
    auto play_all = [&]() {
        const bool auto_bank = !opts.explicit_bank && opts.bank_file.empty();
        std::map<std::string, std::map<std::string,int>> sng_cache;
        int cur_bank = opts.bank_no;

        int idx = 0;
        while (!g_stop && idx < (int)files.size()) {
            std::string playpath = opts.force_hmp ? files[idx] : prefer_hmq(files[idx]);
            const char *nm = strrchr(playpath.c_str(), '/');
            nm = nm ? nm + 1 : playpath.c_str();
            printf("\n[%d/%d] %s\n", idx+1, (int)files.size(), playpath.c_str());
#ifdef HAVE_GUI
            if (opts.gui) hmpviz_set_status(idx+1, (int)files.size(), nm);
#else
            (void)nm;
#endif

            if (auto_bank) {
                std::string d = dir_of(files[idx]);
                auto it = sng_cache.find(d);
                if (it == sng_cache.end())
                    it = sng_cache.emplace(d, load_song_banks(d)).first;
                auto bit = it->second.find(song_key(files[idx]));
                int bn = (bit != it->second.end()) ? bit->second : opts.bank_no;
                printf("  Bank: #%d — %s%s\n", bn, adl_getBankNames()[bn],
                       (bit == it->second.end()) ? "  (no descent.sng match — fallback)" : "");
                fflush(stdout);
                if (bn != cur_bank && adl_setBank(adl, bn) == 0) {
                    adl_setVolumeRangeModel(adl, ADLMIDI_VolumeModel_AUTO);
                    adl_setNumChips(adl, 1);
                    cur_bank = bn;
                }
            }

            HmpFile *hmp = nullptr;
            try { hmp = hmp_open(playpath.c_str()); }
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
        g_stop = true;   // playlist ended -> signal the GUI / keyboard thread
    };

    if (opts.gui) {
#ifdef HAVE_GUI
        g_retrowave_opl_tap = hmpviz_opl_tap;   // feed the software tee
        HmpVizHooks hooks{ &g_stop, &g_skip, &g_prev, &g_pause,
                           &opts.loop, &opts.tempo_scale, gui_apply_tempo };
        if (hmpviz_init(&hooks)) {
            std::thread player(play_all);
            while (hmpviz_frame()) { /* paced by vsync */ }
            g_stop = true;
            player.join();
            hmpviz_shutdown();
            g_retrowave_opl_tap = nullptr;
            adl_panic(adl); adl_close(adl);
            printf("\nDone.\n");
            return 0;
        }
        fprintf(stderr, "GUI init failed; falling back to terminal.\n");
#else
        fprintf(stderr, "--gui not available: built without SDL2. "
                        "Install libsdl2-dev and rebuild. Playing in terminal.\n");
#endif
    }

    // ── Headless (terminal) ────────────────────────────────────────────────────
    RawTerm rawterm; rawterm.enable();
    std::thread key_thread(key_thread_fn_real, &opts);
    play_all();
    g_stop = true;
    key_thread.join();
    rawterm.disable();
    adl_panic(adl);
    adl_close(adl);
    printf("\nDone.\n");
    return 0;
}
