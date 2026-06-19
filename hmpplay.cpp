/*
 * hmpplay - Standalone HMP file player for Linux via ALSA sequencer
 *
 * HMP (HMI MIDI Protocol) is the music format used by Descent 1 & 2.
 * This player reads .hmp files directly and sends MIDI events to any
 * ALSA sequencer port — hardware synths, virtual synths, etc.
 *
 * Usage:
 *   hmpplay [-p client:port] [-l] [-t tempo_scale] file.hmp [file2.hmp ...]
 *
 *   -p client:port   ALSA sequencer destination (default: list ports and ask)
 *   -l               Loop the file indefinitely
 *   -t scale         Tempo multiplier as float (default: 1.0)
 *   -v               Verbose: print events as they play
 *   --list           List available ALSA MIDI ports and exit
 *
 * Build:
 *   g++ -std=c++17 -O2 -o hmpplay hmpplay.cpp -lasound
 *
 * HMP format based on DXX-Rebirth source (common/misc/hmp.cpp),
 * originally by Arne de Bruijn and the JFFEE project.
 * ALSA playback is original work for this tool.
 */

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <csignal>
#include <atomic>
#include <dirent.h>
#include <sys/stat.h>
#include <termios.h>
#include <poll.h>

// ─────────────────────────────────────────────────────────────────────────────
// HMP format constants (from DXX-Rebirth hmp.h / hmp.cpp)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int HMP_MAX_TRACKS = 32;

// HMI uses little-endian variable-length encoding (unlike standard MIDI).
// HMP control-change values for loop markers:
static constexpr uint8_t HMP_LOOP_START = 0x6E;
static constexpr uint8_t HMP_LOOP_END   = 0x6F;

// ─────────────────────────────────────────────────────────────────────────────
// Data structures
// ─────────────────────────────────────────────────────────────────────────────

struct HmpTrack {
    std::vector<uint8_t> data;
    size_t pos{0};          // current read position
    uint32_t cur_time{0};   // track's current tick time
};

struct HmpFile {
    int num_tracks{0};
    int tempo{0};           // raw value from header; time_div = tempo * 1.6
    HmpTrack tracks[HMP_MAX_TRACKS];
    long filesize{0};
};

// A decoded MIDI event ready for sequencer dispatch
struct MidiEvent {
    uint32_t tick;          // absolute tick time
    uint8_t  status;        // MIDI status byte
    uint8_t  data1{0};
    uint8_t  data2{0};
    uint8_t  type;          // 0=short, 1=meta, 2=sysex
    std::vector<uint8_t> meta_data; // for meta/sysex events
    int meta_type{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// HMP file parsing (ported from DXX-Rebirth without PhysFS / engine deps)
// ─────────────────────────────────────────────────────────────────────────────

// HMI variable-length number: little-endian, MSB-terminated (opposite of MIDI)
// Returns number of bytes consumed, or 0 on error.
static int read_hmi_var(const uint8_t *data, int len, uint32_t *out) {
    uint32_t v = 0;
    int shift = 0;
    const uint8_t *p = data;

    while (len > 0 && !(*p & 0x80)) {
        v += (uint32_t)(*p++) << shift;
        shift += 7;
        len--;
    }
    if (!len) return 0;
    v += (uint32_t)(*p++ & 0x7f) << shift;
    if (out) *out = v;
    return (int)(p - data);
}

std::unique_ptr<HmpFile> hmp_open(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f)
        throw std::runtime_error(std::string("Cannot open: ") + filename);

    fseek(f, 0, SEEK_END);
    long filesize = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Read header magic
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "HMIMIDIP", 8) != 0) {
        fclose(f);
        throw std::runtime_error(std::string("Not a valid HMP file: ") + filename);
    }

    // Track count at offset 0x30
    if (fseek(f, 0x30, SEEK_SET) != 0) {
        fclose(f);
        throw std::runtime_error("Seek error (track count)");
    }
    uint32_t num_tracks = 0;
    if (fread(&num_tracks, 4, 1, f) != 1 || num_tracks < 1 || num_tracks > HMP_MAX_TRACKS) {
        fclose(f);
        throw std::runtime_error("Invalid track count in HMP file");
    }

    // Tempo at offset 0x38
    if (fseek(f, 0x38, SEEK_SET) != 0) {
        fclose(f);
        throw std::runtime_error("Seek error (tempo)");
    }
    int32_t raw_tempo = 0;
    if (fread(&raw_tempo, 4, 1, f) != 1) {
        fclose(f);
        throw std::runtime_error("Read error (tempo)");
    }

    // Tracks begin at offset 0x308
    if (fseek(f, 0x308, SEEK_SET) != 0) {
        fclose(f);
        throw std::runtime_error("Seek error (tracks)");
    }

    auto hmp = std::make_unique<HmpFile>();
    hmp->num_tracks = (int)num_tracks;
    hmp->tempo = raw_tempo; // little-endian, already native
    hmp->filesize = filesize;

    for (int i = 0; i < hmp->num_tracks; i++) {
        // Each track header: [unknown_4][length_4][unknown_4] then data
        int32_t tdata[3];
        if (fread(tdata, 4, 3, f) != 3) {
            fclose(f);
            throw std::runtime_error("Read error (track header)");
        }
        int data_len = tdata[1] - 12; // subtract the 12-byte track header
        if (data_len <= 0) {
            fclose(f);
            throw std::runtime_error("Invalid track data length");
        }
        hmp->tracks[i].data.resize(data_len);
        if ((int)fread(hmp->tracks[i].data.data(), 1, data_len, f) != data_len) {
            fclose(f);
            throw std::runtime_error("Read error (track data)");
        }
        hmp->tracks[i].pos = 0;
        hmp->tracks[i].cur_time = 0;
    }

    fclose(f);
    return hmp;
}

// ─────────────────────────────────────────────────────────────────────────────
// HMP → MIDI event decoding
//
// The HMP track format is similar to SMF but:
//   - Variable-length deltas are HMI-style (little-endian, MSB-terminated)
//   - Running status is supported the same as MIDI
//   - Track 0 is a meta/setup track; actual music starts at track 1
// ─────────────────────────────────────────────────────────────────────────────

// Decode all events from all music tracks into a flat, time-sorted list.
// This mirrors what hmptrk2mid() does but keeps events as structures
// rather than re-encoding to SMF bytes — we want them for direct ALSA dispatch.
std::vector<MidiEvent> decode_hmp(const HmpFile &hmp,
        const std::vector<bool> &track_active = {}) {
    // MIDI command lengths (2 or 3 bytes total including status)
    static const int cmdlen[8] = {3,3,3,3,2,2,3,0}; // 8x, 9x, Ax, Bx, Cx, Dx, Ex, Fx

    std::vector<MidiEvent> events;

    for (int trk_idx = 1; trk_idx < hmp.num_tracks; trk_idx++) {
        // Skip tracks not active for this device
        if (!track_active.empty() && trk_idx < (int)track_active.size()
                && !track_active[trk_idx]) continue;
        const HmpTrack &trk = hmp.tracks[trk_idx];
        const uint8_t *data = trk.data.data();
        const int      size = (int)trk.data.size();
        int pos = 0;
        uint32_t cur_tick = 0;
        uint8_t last_status = 0;

        while (pos < size) {
            // 1) Read HMI variable-length delta
            uint32_t delta = 0;
            int consumed = read_hmi_var(data + pos, size - pos, &delta);
            if (consumed <= 0) break;
            pos += consumed;
            cur_tick += delta;

            if (pos >= size) break;

            // 2) Read event
            uint8_t status = data[pos];

            if (status == 0xFF) {
                // Meta event
                if (pos + 2 >= size) break;
                uint8_t meta_type = data[pos + 1];
                pos += 2;

                // Meta length as standard MIDI var-len (big-endian MSB-first)
                uint32_t meta_len = 0;
                while (pos < size) {
                    uint8_t b = data[pos++];
                    meta_len = (meta_len << 7) | (b & 0x7f);
                    if (!(b & 0x80)) break;
                }

                MidiEvent ev;
                ev.tick = cur_tick;
                ev.status = 0xFF;
                ev.type = 1;
                ev.meta_type = meta_type;
                if (meta_len > 0 && pos + (int)meta_len <= size) {
                    ev.meta_data.assign(data + pos, data + pos + meta_len);
                    pos += meta_len;
                }
                if (meta_type == 0x2F) { // end of track
                    events.push_back(std::move(ev));
                    break;
                }
                events.push_back(std::move(ev));
            } else {
                // Short MIDI event
                if (status & 0x80) {
                    last_status = status;
                    pos++;
                } else {
                    // Running status: re-use last status byte, data[pos] is data1
                    status = last_status;
                    // don't advance pos; data1 is at current pos
                }

                if (!last_status) break;

                int cmd = (last_status >> 4) & 0x0F;
                int ndata = (cmd < 0xC) ? ((cmd == 0xC || cmd == 0xD) ? 1 : 2) : 0;

                // Use the table: 8x=3, 9x=3, Ax=3, Bx=3, Cx=2, Dx=2, Ex=3
                if (cmd >= 8 && cmd <= 14)
                    ndata = cmdlen[cmd - 8] - 1;
                else
                    break; // unknown

                MidiEvent ev;
                ev.tick = cur_tick;
                ev.status = last_status;
                ev.type = 0;

                if (ndata >= 1 && pos < size) ev.data1 = data[pos++];
                if (ndata >= 2 && pos < size) ev.data2 = data[pos++];

                events.push_back(std::move(ev));
            }
        }
    }

    // Sort by tick (stable to preserve track ordering within same tick)
    std::stable_sort(events.begin(), events.end(),
        [](const MidiEvent &a, const MidiEvent &b){ return a.tick < b.tick; });

    return events;
}

// ─────────────────────────────────────────────────────────────────────────────
// ALSA sequencer helpers
// ─────────────────────────────────────────────────────────────────────────────

struct AlsaSeq {
    snd_seq_t *seq{nullptr};
    int port{-1};

    ~AlsaSeq() {
        if (seq) snd_seq_close(seq);
    }

    void open(const char *client_name = "hmpplay") {
        if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0)
            throw std::runtime_error("Cannot open ALSA sequencer");
        snd_seq_set_client_name(seq, client_name);
        port = snd_seq_create_simple_port(seq, "Output",
            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (port < 0)
            throw std::runtime_error("Cannot create ALSA sequencer port");
    }

    void connect(int dest_client, int dest_port) {
        snd_seq_addr_t sender, dest;
        sender.client = snd_seq_client_id(seq);
        sender.port   = port;
        dest.client   = dest_client;
        dest.port     = dest_port;

        snd_seq_port_subscribe_t *sub;
        snd_seq_port_subscribe_alloca(&sub);
        snd_seq_port_subscribe_set_sender(sub, &sender);
        snd_seq_port_subscribe_set_dest(sub, &dest);
        if (snd_seq_subscribe_port(seq, sub) < 0)
            throw std::runtime_error("Cannot connect to destination port");
    }

    void send_short(uint8_t status, uint8_t d1, uint8_t d2) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_source(&ev, port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);

        int type = status >> 4;
        int ch   = status & 0x0f;

        switch (type) {
            case 0x9:
                snd_seq_ev_set_noteon(&ev, ch, d1, d2);
                break;
            case 0x8:
                snd_seq_ev_set_noteoff(&ev, ch, d1, d2);
                break;
            case 0xA:
                snd_seq_ev_set_keypress(&ev, ch, d1, d2);
                break;
            case 0xB:
                snd_seq_ev_set_controller(&ev, ch, d1, d2);
                break;
            case 0xC:
                snd_seq_ev_set_pgmchange(&ev, ch, d1);
                break;
            case 0xD:
                snd_seq_ev_set_chanpress(&ev, ch, d1);
                break;
            case 0xE: {
                int pitchbend = (int)(d1 | (d2 << 7)) - 8192;
                snd_seq_ev_set_pitchbend(&ev, ch, pitchbend);
                break;
            }
            default:
                return; // skip unknown
        }
        snd_seq_event_output_direct(seq, &ev);
    }

    // Send a raw SysEx message
    void send_sysex(const uint8_t *data, size_t len) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_source(&ev, port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_ev_set_sysex(&ev, len, const_cast<uint8_t*>(data));
        snd_seq_event_output_direct(seq, &ev);
        snd_seq_drain_output(seq);
    }

    // Send all-notes-off + all-sounds-off on all channels (panic reset)
    void panic() {
        for (int ch = 0; ch < 16; ch++) {
            send_short(0xB0 | ch, 120, 0); // all sounds off
            send_short(0xB0 | ch, 123, 0); // all notes off
            send_short(0xB0 | ch, 121, 0); // reset all controllers
        }
        snd_seq_drain_output(seq);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Port listing
// ─────────────────────────────────────────────────────────────────────────────

void list_ports(snd_seq_t *seq) {
    printf("Available ALSA MIDI output ports:\n");
    printf("  %-6s %-30s %s\n", "Port", "Client Name", "Port Name");
    printf("  %-6s %-30s %s\n", "------", "------------------------------", "----------");

    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t   *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            // Show only ports that can receive MIDI (WRITE capability)
            if ((caps & SND_SEQ_PORT_CAP_WRITE) &&
                !(caps & SND_SEQ_PORT_CAP_NO_EXPORT)) {
                printf("  %d:%-4d %-30s %s\n",
                    client,
                    snd_seq_port_info_get_port(pinfo),
                    snd_seq_client_info_get_name(cinfo),
                    snd_seq_port_info_get_name(pinfo));
            }
        }
    }
}

// Parse "client:port" string
static bool parse_port(const char *s, int *client, int *port) {
    return sscanf(s, "%d:%d", client, port) == 2;
}

// ─────────────────────────────────────────────────────────────────────────────
// Playback engine
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_skip{false};   // skip to next song
static std::atomic<bool> g_prev{false};   // go to previous song
static std::atomic<bool> g_loop{false};   // runtime loop toggle mirror

void signal_handler(int) {
    g_stop = true;
}

struct PlayOptions {
    bool loop{false};
    double tempo_scale{1.0};
    bool verbose{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// Terminal raw-mode keyboard input
// ─────────────────────────────────────────────────────────────────────────────

struct RawTerm {
    struct termios saved{};
    bool active{false};

    void enable() {
        if (!isatty(STDIN_FILENO)) return;
        tcgetattr(STDIN_FILENO, &saved);
        struct termios raw = saved;
        raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
        raw.c_cc[VMIN]  = 0;   // non-blocking
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        active = true;
    }

    void disable() {
        if (active) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved);
            active = false;
        }
    }

    ~RawTerm() { disable(); }
};

// Key-reader thread: polls stdin every 50 ms, sets atomic flags on keypress.
// Runs until g_stop is set.
void key_thread_fn(PlayOptions *opts) {
    struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
    printf("  Keys: [n/Space] next  [p] prev  [l] toggle loop  "
           "[+/-] tempo  [q] quit\n");
    fflush(stdout);

    while (!g_stop) {
        if (poll(&pfd, 1, 50) <= 0) continue;
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) continue;

        switch (c) {
            case 'n': case ' ':
                printf("\n  >> Next\n");
                g_skip = true;
                break;
            case 'p':
                printf("\n  >> Previous\n");
                g_prev = true;
                g_skip = true;   // also breaks current playback loop
                break;
            case 'l':
                opts->loop = !opts->loop;
                g_loop = opts->loop;
                printf("\n  >> Loop %s\n", opts->loop ? "ON" : "OFF");
                break;
            case '+': case '=':
                opts->tempo_scale = std::min(opts->tempo_scale * 1.25, 4.0);
                printf("\n  >> Tempo scale: %.2fx\n", opts->tempo_scale);
                break;
            case '-':
                opts->tempo_scale = std::max(opts->tempo_scale / 1.25, 0.25);
                printf("\n  >> Tempo scale: %.2fx\n", opts->tempo_scale);
                break;
            case 'q': case 3:   // 3 = Ctrl-C in raw mode
                printf("\n  >> Quit\n");
                g_stop = true;
                break;
            default:
                break;
        }
        fflush(stdout);
    }
}


// Wrapper that handles tempo change meta events properly
void play_hmp(AlsaSeq &alsa, const HmpFile &hmp, const PlayOptions &opts) {
    // Play all music tracks for a General MIDI device (e.g. SC-55, or an MT-32pi
    // in GM mode). Descent's per-device track map isn't usable from these files
    // (the OPL/MT-32 subsets aren't cleanly encoded), and the .hmp arrangement
    // doesn't fit a real MT-32's 9 parts — so we target GM, which voices every
    // channel. The OPL3 route is handled by hmpplay_opl3.
    (void)opts;
    std::vector<bool> track_active(hmp.num_tracks, true);
    track_active[0] = false;  // track 0 is always the conductor track

    auto events = decode_hmp(hmp, track_active);
    if (events.empty()) {
        fprintf(stderr, "Warning: no MIDI events decoded from HMP file\n");
        return;
    }

    // HMP time_div: DXX-Rebirth derives this as hmp->tempo * 1.6 (from midhdr ctor).
    // This value goes into the SMF header as ticks-per-beat.
    int time_div = (int)(hmp.tempo * 1.6);
    if (time_div <= 0) time_div = 60;

    // The DXX-generated SMF hardcodes this Set Tempo value in its tempo track:
    //   FF 51 03  18 80 00  = 0x188000 = 1,605,632 µs/beat (~37.4 BPM)
    // Using the standard MIDI default of 500,000 instead causes 3.2x-too-fast playback.
    static constexpr double HMP_TEMPO_US = 1605632.0;
    double tempo_us    = HMP_TEMPO_US;
    double us_per_tick = tempo_us / time_div;

    printf("  Tracks: %d  |  Time division: %d ticks/beat  |  "
           "Tempo: %.0f µs/beat (%.2f BPM)  |  Tick: %.2f ms\n",
        hmp.num_tracks, time_div, tempo_us,
        60000000.0 / tempo_us, us_per_tick / 1000.0);

    using clock = std::chrono::steady_clock;
    using us_t  = std::chrono::microseconds;

    do {
        auto start = clock::now();

        for (const auto &ev : events) {
            if (g_stop || g_skip) break;

            // Update tempo if we hit a Set Tempo meta event
            if (ev.type == 1 && ev.meta_type == 0x51 && ev.meta_data.size() >= 3) {
                uint32_t new_tempo_us =
                    ((uint32_t)ev.meta_data[0] << 16) |
                    ((uint32_t)ev.meta_data[1] <<  8) |
                     (uint32_t)ev.meta_data[2];
                if (new_tempo_us > 0) {
                    // Adjust start so timing stays consistent after tempo change
                    double elapsed_ticks = ev.tick * us_per_tick;
                    double elapsed_ticks_new = ev.tick * (new_tempo_us / (double)time_div);
                    // shift start point to compensate
                    start -= us_t((long long)(elapsed_ticks_new - elapsed_ticks));
                    tempo_us    = new_tempo_us;
                    us_per_tick = tempo_us / time_div;
                    if (opts.verbose)
                        printf("  tick=%6u  Tempo → %u µs/beat (%.1f BPM)\n",
                            ev.tick, new_tempo_us, 60000000.0 / new_tempo_us);
                }
                continue;
            }

            if (ev.type != 0) continue; // skip other meta events

            // Time this event should play at
            double elapsed_us = ev.tick * us_per_tick / opts.tempo_scale;
            auto target = start + us_t((long long)elapsed_us);
            auto now = clock::now();
            if (target > now)
                std::this_thread::sleep_until(target);

            if (g_stop || g_skip) break;

            if (opts.verbose)
                printf("  tick=%6u  %02X %02X %02X\n",
                    ev.tick, ev.status, ev.data1, ev.data2);

            alsa.send_short(ev.status, ev.data1, ev.data2);
        }

        if (opts.loop && !g_stop && !g_skip)
            printf("  [Looping...]\n");

    } while (opts.loop && !g_stop && !g_skip);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] file.hmp [file2.hmp ...]\n"
        "\n"
        "Options:\n"
        "  -p client:port   ALSA destination port (e.g. 128:0)\n"
        "  -l               Loop the file indefinitely (Ctrl-C to stop)\n"
        "  -t scale         Tempo multiplier (e.g. 0.5 = half speed, 2.0 = double)\n"
        "  -v               Verbose: print each MIDI event\n"
        "  --list           List available ALSA MIDI output ports and exit\n"
        "  -h, --help       Show this help\n"
        "\n"
        "Plays to a General MIDI device (e.g. Roland SC-55, or an MT-32pi in GM/\n"
        "soundfont mode). For OPL3 FM hardware use hmpplay_opl3 instead.\n"
        "Note: native MT-32 mode is not supported — Descent's .hmp files carry the\n"
        "dense OPL arrangement (13-16 channels), which a real MT-32 (9 parts) can't\n"
        "voice; use GM mode, which plays every channel.\n"
        "\n"
        "Example (mt32pi/SC-55 in GM mode):\n"
        "  %s -p 20:0 ./music/\n"
        "\n"
        "Build:  g++ -std=c++17 -O2 -o hmpplay hmpplay.cpp -lasound\n",
        prog, prog);
}

int main(int argc, char *argv[]) {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Parse arguments ──────────────────────────────────────────────────────
    int dest_client = -1, dest_port = -1;
    PlayOptions opts;
    std::vector<const char *> files;
    bool do_list = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) {
            do_list = true;
        } else if (!strcmp(argv[i], "-l")) {
            opts.loop = true;
        } else if (!strcmp(argv[i], "-v")) {
            opts.verbose = true;
        } else if (!strcmp(argv[i], "--gm")) {
            // Accepted for backward compatibility; GM is the only/default mode.
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            if (!parse_port(argv[++i], &dest_client, &dest_port)) {
                fprintf(stderr, "Error: invalid port spec '%s' (expected client:port)\n", argv[i]);
                return 1;
            }
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            opts.tempo_scale = atof(argv[++i]);
            if (opts.tempo_scale <= 0.0) {
                fprintf(stderr, "Error: tempo scale must be > 0\n");
                return 1;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else {
            files.push_back(argv[i]);
        }
    }

    // ── Expand any directory args into sorted lists of .hmp files ────────────
    std::vector<std::string> expanded;
    for (const char *arg : files) {
        struct stat st;
        if (stat(arg, &st) != 0) {
            // Let the play loop report the error
            expanded.push_back(arg);
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            expanded.push_back(arg);
            continue;
        }
        // It's a directory — collect all .hmp / .HMP files inside
        DIR *dir = opendir(arg);
        if (!dir) {
            fprintf(stderr, "Warning: cannot open directory '%s': %s\n",
                    arg, strerror(errno));
            continue;
        }
        std::vector<std::string> found;
        struct dirent *ent;
        while ((ent = readdir(dir)) != nullptr) {
            const char *name = ent->d_name;
            size_t nlen = strlen(name);
            if (nlen < 4) continue;
            const char *ext = name + nlen - 4;
            if (strcasecmp(ext, ".hmp") == 0) {
                std::string path = std::string(arg) + "/" + name;
                found.push_back(path);
            }
        }
        closedir(dir);
        if (found.empty()) {
            fprintf(stderr, "Warning: no .hmp files found in '%s'\n", arg);
            continue;
        }
        std::sort(found.begin(), found.end());
        for (auto &p : found)
            expanded.push_back(p);
    }

    if (expanded.empty() && !files.empty()) {
        fprintf(stderr, "Error: no .hmp files to play\n");
        return 1;
    }

    // ── Open ALSA sequencer ──────────────────────────────────────────────────
    AlsaSeq alsa;
    try {
        alsa.open("hmpplay");
    } catch (const std::exception &e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    if (do_list) {
        list_ports(alsa.seq);
        return 0;
    }

    if (files.empty()) {
        usage(argv[0]);
        return 1;
    }

    // ── If no port given, list and prompt ────────────────────────────────────
    if (dest_client < 0) {
        list_ports(alsa.seq);
        printf("\nEnter destination port (client:port): ");
        fflush(stdout);
        char buf[64];
        if (!fgets(buf, sizeof(buf), stdin) ||
            !parse_port(buf, &dest_client, &dest_port)) {
            fprintf(stderr, "Invalid port\n");
            return 1;
        }
    }

    try {
        alsa.connect(dest_client, dest_port);
    } catch (const std::exception &e) {
        fprintf(stderr, "Error connecting to %d:%d — %s\n",
            dest_client, dest_port, e.what());
        return 1;
    }
    printf("Connected to ALSA port %d:%d\n", dest_client, dest_port);
    printf("Playlist: %d file(s)\n", (int)expanded.size());

    // ── Enable raw terminal + start key-reader thread ────────────────────────
    RawTerm rawterm;
    rawterm.enable();
    g_loop = opts.loop;
    std::thread key_thread(key_thread_fn, &opts);

    // ── Play loop with prev/next support ─────────────────────────────────────
    int idx = 0;
    while (!g_stop && idx < (int)expanded.size()) {
        const char *f = expanded[idx].c_str();

        printf("\n[%d/%d] Playing: %s\n",
               idx + 1, (int)expanded.size(), f);

        std::unique_ptr<HmpFile> hmp;
        try {
            hmp = hmp_open(f);
        } catch (const std::exception &e) {
            fprintf(stderr, "  Error: %s\n", e.what());
            idx++;
            continue;
        }

        g_skip = false;
        g_prev = false;
        play_hmp(alsa, *hmp, opts);
        alsa.panic();   // silence any held notes between tracks

        if (g_stop) break;

        if (g_prev)
            idx = std::max(0, idx - 1);
        else
            idx++;
    }

    // ── Cleanup ──────────────────────────────────────────────────────────────
    g_stop = true;          // signal key thread to exit
    key_thread.join();
    rawterm.disable();

    printf("\nAll done.\n");
    return 0;
}
