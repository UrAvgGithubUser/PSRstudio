// style.cpp — PRS style file parsing, fire-time cache, voice setup, CASM.
#include "style.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <ranges>
#include <thread>
#include <utility>

namespace prs {

// =========================================================
//  Constant tables
// =========================================================
const std::array<std::string_view, 35> kChordTypeNames = {
    "Maj",         "Maj6",          "Maj7",          "Maj7#11",
    "Maj(9)",      "Maj7(9)",       "Maj6(9)",       "aug",
    "min",         "min6",          "min7",          "min7b5",
    "min(9)",      "min7(9)",       "min7(11)",      "minMaj7",
    "minMaj7(9)",  "dim",           "dim7",          "7th",
    "7sus4",       "7b5",           "7(9)",          "7#11",
    "7(13)",       "7(b9)",         "7(b13)",        "7(#9)",
    "Maj7aug",     "7aug",          "1+8",           "1+5",
    "sus4",        "1+2+5",         "cancel",
};

const std::array<std::string_view, 12> kNoteNames = {
    "C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B"
};

// Chord mute bit-position lookup table.
// Maps CASM chord type index (0..34) to bit position in chord_mask.
//
// chord_mask layout (40 bits, MSB first):
//   Byte 13 low nibble → bits 39-36:
//     bit 39 = autostart (not a chord type — skip)
//     bit 38 = 1+2+5     (CASM index 0x21 = 33)
//     bit 37 = sus4      (CASM index 0x20 = 32)
//     bit 36 = unused
//   Byte 14 → bits 35-28:
//     35=1+5(0x1F=31) 34=1+8(0x1E=30) 33=7aug(0x1D=29) 32=Maj7aug(0x1C=28)
//     31=7#9(0x1B=27) 30=7b13(0x1A=26) 29=7b9(0x19=25) 28=7(13)(0x18=24)
//   Byte 15 → bits 27-20:
//     27=7#11(0x17=23) 26=7(9)(0x16=22) 25=7b5(0x15=21) 24=7sus4(0x14=20)
//     23=7th(0x13=19)  22=dim7(0x12=18)  21=dim(0x11=17)  20=minMaj7(9)(0x10=16)
//   Byte 16 → bits 19-12:
//     19=minMaj7(0x0F=15) 18=min7(11)(0x0E=14) 17=min7(9)(0x0D=13) 16=min(9)(0x0C=12)
//     15=m7b5(0x0B=11)    14=min7(0x0A=10)     13=min6(0x09=9)     12=min(0x08=8)
//   Byte 17 → bits 11-4:
//     11=aug(0x07=7)   10=Maj6(9)(0x06=6)  9=Maj7(9)(0x05=5)  8=Maj(9)(0x04=4)
//     7=Maj7#11(0x03=3) 6=Maj7(0x02=2)      5=Maj6(0x01=1)    4=Maj(0x00=0)
//   Bits 3-0 = unused
//
// Index = CASM chord type, value = bit position in chord_mask.
// -1 = not represented in the mask (always allowed).
const std::array<int, 35> kChordMuteBitPos = []() constexpr {
    std::array<int, 35> t{};
    t.fill(-1);  // default: not in mask → always allowed

    // Byte 17 (Part 5) → bits 11-4: Maj(0) through aug(7)
    t[0]  = 4;   // Maj
    t[1]  = 5;   // Maj6
    t[2]  = 6;   // Maj7
    t[3]  = 7;   // Maj7#11
    t[4]  = 8;   // Maj(9)
    t[5]  = 9;   // Maj7(9)
    t[6]  = 10;  // Maj6(9)
    t[7]  = 11;  // aug

    // Byte 16 (Part 4) → bits 19-12: min(8) through minMaj7(15)
    t[8]  = 12;  // min
    t[9]  = 13;  // min6
    t[10] = 14;  // min7
    t[11] = 15;  // min7b5
    t[12] = 16;  // min(9)
    t[13] = 17;  // min7(9)
    t[14] = 18;  // min7(11)
    t[15] = 19;  // minMaj7

    // Byte 15 (Part 3) → bits 27-20: minMaj7(9)(16) through 7#11(23)
    t[16] = 20;  // minMaj7(9)
    t[17] = 21;  // dim
    t[18] = 22;  // dim7
    t[19] = 23;  // 7th
    t[20] = 24;  // 7sus4
    t[21] = 25;  // 7b5
    t[22] = 26;  // 7(9)
    t[23] = 27;  // 7#11

    // Byte 14 (Part 2) → bits 35-28: 7(13)(24) through 1+5(31)
    t[24] = 28;  // 7(13)
    t[25] = 29;  // 7(b9)
    t[26] = 30;  // 7(b13)
    t[27] = 31;  // 7(#9)
    t[28] = 32;  // Maj7aug
    t[29] = 33;  // 7aug
    t[30] = 34;  // 1+8
    t[31] = 35;  // 1+5

    // Byte 13 (Part 1) → bits 38-37: sus4(32), 1+2+5(33)
    t[32] = 37;  // sus4
    t[33] = 38;  // 1+2+5
    t[34] = -1;  // cancel — not in mask (always mutes everything)

    return t;
}();

// =========================================================
//  MidiSink default implementations
// =========================================================
void MidiSink::all_notes_off() {
    for (int ch = 0; ch < 16; ++ch) {
        short_direct(static_cast<u8>(0xB0 | ch), 123, 0);  // All Notes Off
        short_direct(static_cast<u8>(0xB0 | ch), 120, 0);  // All Sound Off
    }
}

// =========================================================
//  String helpers
// =========================================================
namespace {

[[nodiscard]] constexpr char ascii_tolower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

bool iequals(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b,
        [](char x, char y) noexcept { return ascii_tolower(x) == ascii_tolower(y); });
}

std::string normalize_name(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != ' ') out.push_back(ascii_tolower(c));
    }
    return out;
}

// =========================================================
//  PrsStyle::parse — top-level entry point
// =========================================================
bool PrsStyle::parse(const std::filesystem::path& path) {
    // ----- Load file -----
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::cerr << std::format("Cannot open '{}'\n", path.string());
        return false;
    }
    const std::streamoff fsz = f.tellg();
    if (fsz <= 0) {
        std::cerr << "Empty file\n";
        return false;
    }
    raw_.resize(static_cast<std::size_t>(fsz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(raw_.data()), fsz);
    if (!f) {
        std::cerr << "fread incomplete\n";
        return false;
    }

    // ----- De-obfuscate protected files -----
    // Files whose last 4 bytes are exactly D4 C1 D0 CB are stored
    // byte-reversed with the high bit of every byte flipped. After
    // decoding, the trailer becomes the "KPAT" magic of a Ketron style.
    if (raw_.size() >= 4 &&
        raw_[raw_.size() - 4] == 0xD4 && raw_[raw_.size() - 3] == 0xC1 &&
        raw_[raw_.size() - 2] == 0xD0 && raw_[raw_.size() - 1] == 0xCB) {
        std::ranges::reverse(raw_);
        for (auto& b : raw_) b = static_cast<u8>(b ^ 0x80);
        std::cout << "[PRS] de-obfuscated protected style file\n";
    }

    // ----- Initialize defaults -----
    tempo_us_      = 500000;
    ts_num_        = 4;
    ts_den_        = 4;
    raw_hash_      = fnv1a(raw_);
    file_path_     = path.string();
    format_        = StyleFormat::Yamaha;

    for (auto& v : ch_voices_) {
        v.volume     = 100;
        v.pan        = 64;
        v.expression = 127;
        v.filter_res = 64;
        v.release    = 64;
        v.attack     = 64;
        v.brightness = 64;
    }

    // Pre-reserve reasonable capacities. We grow on demand.
    events_.reserve(1 << 16);
    sx_pool_.reserve(1 << 18);

    // ----- Format dispatch -----
    // Ketron ("KPAT") and Korg PA ("G8"/"EV") styles have their own
    // parsers (style_foreign.cpp); everything else must be an SMF.
    if (raw_.size() >= 4 && std::memcmp(raw_.data(), "KPAT", 4) == 0) {
        format_ = StyleFormat::Ketron;
        return parse_ketron();
    }
    if (raw_.size() >= 2 &&
        ((raw_[0] == 'G' && raw_[1] == '8') ||
         (raw_[0] == 'E' && raw_[1] == 'V'))) {
        format_ = StyleFormat::Korg;
        return parse_korg();
    }

    // ----- Validate SMF header -----
    // Layout: MThd(4) + len(4) + fmt(2) + ntrks(2) + div(2).
    if (raw_.size() < 14 || std::memcmp(raw_.data(), "MThd", 4) != 0) {
        std::cerr << "Not an SMF\n";
        return false;
    }
    ByteReader r(raw_);
    r.seek(8);
    [[maybe_unused]] const u16 fmt = r.read_be16();
    const u16 num_tracks           = r.read_be16();
    const u16 div                  = r.read_be16();   // ticks per quarter note
    ticks_per_qn_                  = div;
    if (num_tracks == 0) {
        std::cerr << "No tracks\n";
        return false;
    }

    // ----- Track 1 -----
    // First chunk header begins at offset 14.
    if (raw_.size() < 22) {
        std::cerr << "Truncated track header\n";
        return false;
    }
    if (std::memcmp(raw_.data() + 14, "MTrk", 4) != 0) {
        std::cerr << "Expected MTrk\n";
        return false;
    }
    r.seek(18);
    const u32 trk1_len             = r.read_be32();
    const std::size_t track1_start = 22;
    const std::size_t track1_end   = track1_start + trk1_len;

    parse_track1(r, track1_end);

    // ----- Remaining tracks -----
    std::size_t pos = track1_end;
    std::size_t last_trk_end = track1_end;
    for (int t = 1; t < num_tracks; ++t) {
        if (pos + 8 > raw_.size()) break;
        char chunk_tag[4];
        std::memcpy(chunk_tag, raw_.data() + pos, 4);
        if (std::memcmp(chunk_tag, "MTrk", 4) != 0) break;

        // Inline read_be32: chunk length at offset pos+4.
        const u8* q = raw_.data() + pos + 4;
        const u32 chunk_len = (static_cast<u32>(q[0]) << 24) |
                              (static_cast<u32>(q[1]) << 16) |
                              (static_cast<u32>(q[2]) <<  8) |
                               static_cast<u32>(q[3]);

        const std::size_t tk_start = pos + 8;
        const std::size_t tk_end   = tk_start + chunk_len;
        if (tk_end > raw_.size()) break;
        r.seek(tk_start);
        parse_extra_track(r, tk_end);
        pos = tk_end;
        last_trk_end = tk_end;
    }

    // ----- Sort by tick if more than one track -----
    if (num_tracks > 1) {
        std::ranges::sort(events_, {}, &Event::tick);
        rebuild_sections();
    }

    // ----- Diagnostic summary -----
    std::cout << std::format("[PRS] '{}'  {} events  {} sections  sysex_pool={} B\n",
        style_name_.empty() ? file_path_ : style_name_,
        events_.size(), sections_.size(), sx_pool_.size());
    std::cout << std::format("[PRS] Tempo {:.1f} BPM  Ticks/QN {}  Time sig {}/{}\n",
        60000000.0 / tempo_us_, ticks_per_qn_,
        static_cast<int>(ts_num_), static_cast<int>(ts_den_));

    std::cout << "[PRS] Ch  Bank/PC(raw)  ->  Bank/PC(remapped)  "
                 "Vol Pan Exp Rev Cho Var\n";
    // The remap display requires a RemapSource — but the style parser
    // used a global here. We skip the per-channel remap printout because
    // it's a diagnostic; users can call build_channel_setup() with their
    // own RemapSource. (Or call casm_dump() for the CASM view.)

    // ----- Parse CASM block (immediately after last MTrk) -----
    attach_casm(last_trk_end);

    // ----- Parse OTS block (after CASM, if present) -----
    attach_ots(last_trk_end);

    // ----- Decode header SysEx stream (XG init, reverb/chorus type, etc.) -----
    decoded_hdr_sysex_ = sysex::decode_stream(hdr_sx_);

    // ----- Decode section SysEx (post-SInt, from sx_pool_) -----
    // Each EV_SYSEX event references a slice of sx_pool_ by offset+length.
    // Walk events in order, decode each sysex payload.
    for (const Event& ev : events_) {
        if (ev.kind != EventKind::SysEx) continue;
        if (ev.sx_offset + ev.sx_len > sx_pool_.size()) continue;
        decoded_section_sysex_.push_back(sysex::decode(
            std::span<const u8>(sx_pool_.data() + ev.sx_offset, ev.sx_len)));
    }

    // ----- SFF-BASIC fallback -----
    // Yamaha styles without a CASM block get a synthetic one with the
    // standard per-channel roles, so their accompaniment follows chords
    // instead of playing untransposed.
    if (!casm_.present) {
        std::vector<std::string> names;
        names.reserve(sections_.size());
        for (const auto& s : sections_) names.push_back(s.name);
        synthesize_default_casm(names);
        std::cout << "[PRS] no CASM — synthesized default channel roles "
                     "(SFF-BASIC fallback)\n";
    }

    return true;
}

// =========================================================
//  Track 1 parser — sets up sections, header sysex, init CCs.
// =========================================================
void PrsStyle::parse_track1(ByteReader& r, std::size_t track_end) {
    u32 abs_tick = 0;
    u8  running  = 0;
    bool past_sint = false;
    bool in_init   = false;
    int  cur_section = -1;

    while (r.position() < track_end) {
        abs_tick += r.read_vlq();
        const u8 sb = r.read_u8();

        if (sb == 0xFF) {
            // ----- Meta event -----
            const u8  mt = r.read_u8();
            const u32 ml = r.read_vlq();
            const auto md = r.read_bytes(ml);

            if (mt == 0x51 && ml == 3) {
                const u32 t = (static_cast<u32>(md[0]) << 16) |
                              (static_cast<u32>(md[1]) <<  8) |
                               static_cast<u32>(md[2]);
                if (cur_section < 0) tempo_us_ = t;
                if (events_.size() < events_.capacity() + (1u << 20)) {
                    Event& ev = events_.emplace_back();
                    ev.tick = abs_tick; ev.kind = EventKind::Tempo;
                    ev.d0 = md[0]; ev.d1 = md[1]; ev.status = md[2];
                }
            } else if (mt == 0x58 && ml == 4 && cur_section < 0) {
                ts_num_ = md[0];
                ts_den_ = static_cast<u8>(1u << md[1]);
            } else if (mt == 0x06) {
                // ----- Marker -----
                std::string marker(reinterpret_cast<const char*>(md.data()), ml);
                if (marker == "SFF2") {
                    // ignored, same as original
                } else if (marker == "SInt") {
                    past_sint = true;
                    in_init   = true;
                } else if (past_sint) {
                    if (in_init) in_init = false;
                    if (cur_section >= 0)
                        sections_[cur_section].end_tick = abs_tick;

                    cur_section = static_cast<int>(sections_.size());
                    StyleSection& sec = sections_.emplace_back();
                    sec.name       = marker;
                    sec.start_tick = abs_tick;
                    sec.end_tick   = 0xFFFF'FFFFu;
                    sec.ev_start   = events_.size();
                    sec.ev_end     = events_.size();
                }
            } else if (mt == 0x03 && !past_sint) {
                style_name_.assign(reinterpret_cast<const char*>(md.data()), ml);
            } else if (mt == 0x2F) {
                if (cur_section >= 0) {
                    sections_[cur_section].end_tick = abs_tick;
                    sections_[cur_section].ev_end   = events_.size();
                }
                break;
            }
        } else if (sb == 0xF0 || sb == 0xF7) {
            // ----- SysEx -----
            const u32 sl = r.read_vlq();
            const auto sd = r.read_bytes(sl);

            if (!past_sint) {
                hdr_sx_.push_back(0xF0);
                hdr_sx_.insert(hdr_sx_.end(), sd.begin(), sd.end());
            }
            // Capture all post-SInt sysex (including the XG setup that
            // appears between SInt and the first section marker).
            if (past_sint) {
                Event& ev = events_.emplace_back();
                ev.tick      = abs_tick;
                ev.kind      = EventKind::SysEx;
                ev.sx_offset = static_cast<u32>(sx_pool_.size());
                ev.sx_len    = static_cast<u16>(std::min<u32>(sl, 65535));
                sx_pool_.insert(sx_pool_.end(), sd.begin(), sd.end());
            }
        } else {
            // ----- Channel message (with running status) -----
            u8 cmd;
            u8 status;
            if (sb & 0x80) {
                running = sb;
                cmd     = sb >> 4;
                status  = sb;
            } else {
                // Running status: data byte first; rewind one byte.
                r.seek(r.position() - 1);
                cmd    = running >> 4;
                status = running;
            }
            const u8 ch = status & 0x0F;
            u8 b0 = 0, b1 = 0;

            if (cmd == 0x8 || cmd == 0x9 || cmd == 0xA ||
                cmd == 0xB || cmd == 0xE) {
                b0 = r.read_u8();
                b1 = r.read_u8();
                if (in_init && cmd == 0xB) {
                    ChVoice& v = ch_voices_[ch];
                    switch (b0) {
                        case  0: v.bank_msb   = b1; break;
                        case  7: v.volume     = b1; break;
                        case 10: v.pan        = b1; break;
                        case 11: v.expression = b1; break;
                        case 32: v.bank_lsb   = b1; break;
                        case 71: v.filter_res = b1; break;
                        case 72: v.release    = b1; break;
                        case 73: v.attack     = b1; break;
                        case 74: v.brightness = b1; break;
                        case 91: v.reverb     = b1; break;
                        case 93: v.chorus     = b1; break;
                        case 94: v.variation  = b1; break;
                        default: break;
                    }
                }
            } else if (cmd == 0xC || cmd == 0xD) {
                b0 = r.read_u8();
                if (in_init && cmd == 0xC) {
                    ch_voices_[ch].pc    = static_cast<u8>(b0 + 1);
                    ch_voices_[ch].valid = true;
                }
            }

            if (past_sint && cur_section >= 0) {
                Event& ev = events_.emplace_back();
                ev.tick   = abs_tick;
                ev.kind   = EventKind::Short;
                ev.status = status;
                ev.d0     = b0;
                ev.d1     = b1;
            }
        }

        if (cur_section >= 0)
            sections_[cur_section].ev_end = events_.size();
    }

    if (cur_section >= 0 && !sections_.empty()) {
        sections_[cur_section].end_tick = abs_tick;
        sections_[cur_section].ev_end   = events_.size();
    }
}

// =========================================================
//  Extra-track parser (no sections, no header sysex)
// =========================================================
void PrsStyle::parse_extra_track(ByteReader& r, std::size_t track_end) {
    u32 tick = 0;
    u8  running = 0;

    while (r.position() < track_end) {
        tick += r.read_vlq();
        const u8 sb = r.read_u8();

        if (sb == 0xFF) {
            const u8  mt = r.read_u8();
            const u32 ml = r.read_vlq();
            const auto md = r.read_bytes(ml);
            if (mt == 0x51 && ml == 3) {
                Event& ev = events_.emplace_back();
                ev.tick   = tick;
                ev.kind   = EventKind::Tempo;
                ev.d0     = md[0];
                ev.d1     = md[1];
                ev.status = md[2];
            } else if (mt == 0x2F) {
                break;
            }
        } else if (sb == 0xF0 || sb == 0xF7) {
            const u32 sl = r.read_vlq();
            const auto sd = r.read_bytes(sl);
            Event& ev = events_.emplace_back();
            ev.tick      = tick;
            ev.kind      = EventKind::SysEx;
            ev.sx_offset = static_cast<u32>(sx_pool_.size());
            ev.sx_len    = static_cast<u16>(std::min<u32>(sl, 65535));
            sx_pool_.insert(sx_pool_.end(), sd.begin(), sd.end());
        } else {
            u8 cmd;
            u8 status;
            if (sb & 0x80) {
                running = sb;
                cmd     = sb >> 4;
                status  = sb;
            } else {
                r.seek(r.position() - 1);
                cmd    = running >> 4;
                status = running;
            }
            u8 b0 = 0, b1 = 0;
            if (cmd == 0x8 || cmd == 0x9 || cmd == 0xA ||
                cmd == 0xB || cmd == 0xE) {
                b0 = r.read_u8();
                b1 = r.read_u8();
            } else if (cmd == 0xC || cmd == 0xD) {
                b0 = r.read_u8();
            }
            Event& ev = events_.emplace_back();
            ev.tick   = tick;
            ev.kind   = EventKind::Short;
            ev.status = status;
            ev.d0     = b0;
            ev.d1     = b1;
        }
    }
}

// =========================================================
//  rebuild_sections — recompute ev_start/ev_end after sort
// =========================================================
void PrsStyle::rebuild_sections() {
    for (auto& sec : sections_) {
        const u32 st = sec.start_tick;
        const u32 et = sec.end_tick;
        std::size_t i = sec.ev_start;
        if (i >= events_.size()) i = 0;
        while (i < events_.size() && events_[i].tick < st) ++i;
        sec.ev_start = i;
        while (i < events_.size() && events_[i].tick < et) ++i;
        sec.ev_end = i;
    }
}

// =========================================================
//  Section helpers
// =========================================================
std::optional<std::size_t>
PrsStyle::find_section(std::string_view name) const {
    // 1) Exact case-insensitive match
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        if (iequals(sections_[i].name, name)) return i;
    }
    // 2) Spaces-ignored case-insensitive match
    const std::string w = normalize_name(name);
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        if (normalize_name(sections_[i].name) == w) return i;
    }
    // 3) Prefix match — foreign-format naming tolerance: "Intro" finds
    //    "Intro 1", "Fill In BA" finds "Fill In Break" (Ketron), etc.
    if (!w.empty()) {
        for (std::size_t i = 0; i < sections_.size(); ++i) {
            if (normalize_name(sections_[i].name).starts_with(w)) return i;
        }
    }
    return std::nullopt;
}

std::size_t PrsStyle::find_main_A() const {
    if (auto idx = find_section("MainA")) return *idx;
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        std::string lo;
        lo.reserve(sections_[i].name.size());
        for (char c : sections_[i].name)
            lo.push_back(static_cast<char>(ascii_tolower(c)));
        if (lo.find("main") != std::string::npos) return i;
    }
    return 0;
}

std::optional<std::size_t>
PrsStyle::find_corresponding_fill(std::string_view main_name) const {
    // Strip trailing spaces, take last char.
    std::size_t end = main_name.size();
    while (end > 0 && main_name[end - 1] == ' ') --end;
    if (end == 0) return std::nullopt;
    const char letter_raw = main_name[end - 1];
    if (!((letter_raw >= 'A' && letter_raw <= 'Z') ||
          (letter_raw >= 'a' && letter_raw <= 'z'))) return std::nullopt;
    const int letter_idx = ascii_tolower(letter_raw) - 'a';
    if (letter_idx < 0 || letter_idx > 3) return std::nullopt;  // Main A..D only

    // Foreign formats use their own fill naming.
    if (format_ == StyleFormat::Ketron) {
        // Observed mapping: Main A -> Fill In 1, B -> 2, C -> 2, D -> 3.
        static const char* kKetronFills[4] =
            { "Fill In 1", "Fill In 2", "Fill In 2", "Fill In 3" };
        if (auto idx = find_section(kKetronFills[letter_idx])) return idx;
    } else if (format_ == StyleFormat::Korg) {
        // Korg PA: upward fills per target main.
        static const char* kKorgFills[4] =
            { "Fill In 1U", "Fill In 2U", "Fill In 3U", "Fill In 3D" };
        if (auto idx = find_section(kKorgFills[letter_idx])) return idx;
    } else {
        const std::string fill_name =
            std::format("Fill In {}{}",
                        static_cast<char>('A' + letter_idx),
                        static_cast<char>('A' + letter_idx));
        if (auto idx = find_section(fill_name)) return idx;
    }

    // Fallback: the first Fill In section that exists.
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        if (section_is_fillin(sections_[i].name)) return i;
    }
    return std::nullopt;
}

bool PrsStyle::section_is_loop_only(std::string_view name, bool& auto_stop) {
    auto_stop = false;
    std::string lo;
    lo.reserve(name.size());
    for (char c : name) lo.push_back(static_cast<char>(ascii_tolower(c)));
    if (lo.starts_with("ending")) { auto_stop = true;  return true; }
    if (lo.starts_with("intro"))  { return true; }
    if (lo.starts_with("fill"))   { return true; }
    if (lo.starts_with("count"))  { return true; }   // Ketron "Count In"
    return false;
}

bool PrsStyle::section_is_fillin(std::string_view name) {
    std::string lo;
    lo.reserve(name.size());
    for (char c : name) lo.push_back(static_cast<char>(ascii_tolower(c)));
    return lo.starts_with("fill");
}

bool PrsStyle::section_is_ending(std::string_view name) {
    std::string lo;
    lo.reserve(name.size());
    for (char c : name) lo.push_back(static_cast<char>(ascii_tolower(c)));
    return lo.starts_with("ending");
}

// =========================================================
//  CASM parser — internal helpers
// =========================================================
void PrsStyle::parse_note_range_ctb2(std::span<const u8> p, NoteRange& r) {
    const u8 raw_ntr = p[0];
    const u8 raw_ntt = p[1];
    r.bass_on   = (raw_ntt & 0x80) != 0;
    r.high_key  = p[2];
    r.note_low  = p[3];
    r.note_high = p[4];
    r.rtr       = static_cast<Rtr>(p[5]);

    r.ntr = (raw_ntr <= 2) ? static_cast<Ntr>(raw_ntr) : Ntr::RootTrans;

    const u8 ntt_raw = raw_ntt & 0x7F;
    r.ntt = (ntt_raw <= static_cast<u8>(Ntt::Dorian5))
                ? static_cast<Ntt>(ntt_raw)
                : Ntt::Bypass;
}

void PrsStyle::parse_note_range_ctab(std::span<const u8> p, NoteRange& r) {
    const u8 raw_ntr = p[0];
    const u8 raw_ntt = p[1];

    r.ntr       = (raw_ntr <= 1) ? static_cast<Ntr>(raw_ntr) : Ntr::RootTrans;
    r.high_key  = p[2];
    r.note_low  = p[3];
    r.note_high = p[4];
    r.rtr       = static_cast<Rtr>(p[5]);
    r.bass_on   = false;

    switch (raw_ntt) {
        case 0: r.ntt = Ntt::Bypass;          break;
        case 1: r.ntt = Ntt::Melody;          break;
        case 2: r.ntt = Ntt::Chord;           break;
        case 3: r.ntt = Ntt::Melody; r.bass_on = true; break;
        case 4: r.ntt = Ntt::MelodicMinor;    break;
        case 5: r.ntt = Ntt::HarmonicMinor;   break;
        default: r.ntt = Ntt::Bypass;         break;
    }
}

bool PrsStyle::parse_casm_channel(std::span<const u8> data,
                                  bool is_ctb2, CasmChannel& cc) {
    if (data.size() < 20) return false;

    cc = CasmChannel{};
    cc.src_ch = data[0] & 0x0F;
    cc.dst_ch = data[9] & 0x0F;

    // Part name: 8 bytes, space-padded.
    cc.part_name.assign(reinterpret_cast<const char*>(data.data() + 1), 8);
    while (!cc.part_name.empty() && cc.part_name.back() == ' ')
        cc.part_name.pop_back();

    // Note mute: bytes 11-12.
    // Byte 11: bits 3-0 = B, Bb, A, G# (bit=1 → play)
    // Byte 12: bits 7-0 = G, F#, F, E, Eb, D, C#, C (bit=1 → play)
    // Pack into u16: bits 15-12 = byte[11] bits 3-0, bits 11-4 = byte[12], bits 3-0 = 1 (unused)
    cc.note_mute_mask = static_cast<u16>(
        (static_cast<u16>(data[11] & 0x0F) << 12) |
        (static_cast<u16>(data[12]) << 4) |
        static_cast<u16>(0x000F));  // unused bits 3-0 = always allowed

    // Chord mask: low nibble of byte 13 << 32 | be32 bytes 14..17
    cc.chord_mask =
        (static_cast<u64>(data[13] & 0x0F) << 32) |
        (static_cast<u32>(data[14]) << 24) |
        (static_cast<u32>(data[15]) << 16) |
        (static_cast<u32>(data[16]) <<  8) |
         static_cast<u32>(data[17]);

    // Optimization: if all chord-type bits (bits 4-38) are set,
    // mark as "all allowed" so the player can skip the per-chord check.
    // Bits 0-3 are unused and always 0 in the raw data.
    // Bits 4-38 = 35 chord types = 0x7FFFFFFF0
    cc.chord_mute_all = (cc.chord_mask & 0x7FFFFFFF0ULL) == 0x7FFFFFFF0ULL;

    cc.src_root       = data[18] & 0x0F;
    cc.src_chord_type = data[19];

    if (is_ctb2) {
        if (data.size() < 40) return false;
        cc.mid_low_pitch  = data[20];
        cc.mid_high_pitch = data[21];

        parse_note_range_ctb2(data.subspan(22, 6), cc.low);
        parse_note_range_ctb2(data.subspan(28, 6), cc.mid);
        parse_note_range_ctb2(data.subspan(34, 6), cc.high);
    } else {
        if (data.size() < 26) return false;
        cc.mid_low_pitch  = 0;
        cc.mid_high_pitch = 127;
        parse_note_range_ctab(data.subspan(20, 6), cc.mid);
        cc.low  = cc.mid;
        cc.high = cc.mid;

        // §5.2.9 special features (Ctab only): byte 26 = feature id
        // (01H = "extra break drum voice"), 29 = instrument,
        // 30 = volume 0-127. Stored on the channel; no playback effect
        // (the extra-break-voice injection is not implemented, and these
        // channels carry no notes anyway).
        if (data.size() >= 31) {
            cc.special_feature = data[26];
            cc.special_instr   = data[29];
            cc.special_volume  = data[30];
        }
    }

    cc.is_drums = (cc.mid.ntr == Ntr::RootFixed &&
                   cc.mid.ntt == Ntt::Bypass);
    return true;
}

void PrsStyle::parse_cseg(std::span<const u8> data, CasmCseg& out) {
    ByteReader r(data);

    // --- Sdec ---
    if (!r.can_read(8)) return;
    const auto tag_bytes = r.read_bytes(4);   // consume tag
    char tag[4];
    std::memcpy(tag, tag_bytes.data(), 4);
    if (std::memcmp(tag, "Sdec", 4) != 0) return;
    const u32 sdec_len = r.read_be32();
    if (!r.can_read(sdec_len)) return;

    // Parse comma-separated section names
    const auto names_bytes = r.read_bytes(sdec_len);
    std::size_t start = 0;
    for (std::size_t i = 0; i <= names_bytes.size(); ++i) {
        if (i == names_bytes.size() || names_bytes[i] == ',') {
            if (i > start) {
                out.sec_names.emplace_back(
                    reinterpret_cast<const char*>(names_bytes.data() + start),
                    i - start);
            }
            start = i + 1;
        }
    }

    // --- Ctab / Ctb2 / Cntt entries ---
    while (r.can_read(8)) {
        const auto tag_b = r.read_bytes(4);
        std::memcpy(tag, tag_b.data(), 4);
        const u32 chunk_len = r.read_be32();
        if (!r.can_read(chunk_len)) break;
        const auto chunk = r.read_bytes(chunk_len);

        if (std::memcmp(tag, "Ctb2", 4) == 0) {
            CasmChannel cc{};
            if (parse_casm_channel(chunk, true, cc))
                out.channels.push_back(std::move(cc));
        } else if (std::memcmp(tag, "Ctab", 4) == 0) {
            CasmChannel cc{};
            if (parse_casm_channel(chunk, false, cc))
                out.channels.push_back(std::move(cc));
        } else if (std::memcmp(tag, "Cntt", 4) == 0 && chunk.size() >= 2) {
            const u8 cntt_src = chunk[0] & 0x0F;
            const u8 cntt_raw = chunk[1];
            const bool bass_on = (cntt_raw & 0x80) != 0;
            const u8 ntt_raw   = cntt_raw & 0x7F;
            const Ntt ntt = (ntt_raw <= static_cast<u8>(Ntt::Dorian5))
                                ? static_cast<Ntt>(ntt_raw)
                                : Ntt::Bypass;

            for (auto& ch : out.channels) {
                if (ch.src_ch == cntt_src) {
                    ch.mid.ntt    = ntt;
                    ch.mid.bass_on = bass_on;
                    ch.low.ntt     = ntt;
                    ch.low.bass_on  = bass_on;
                    ch.high.ntt    = ntt;
                    ch.high.bass_on = bass_on;
                    ch.is_drums    = (ch.mid.ntr == Ntr::RootFixed &&
                                       ntt == Ntt::Bypass);
                    break;
                }
            }
        }
        // Unknown tags: silently skipped (spec-safe).
    }
}

void PrsStyle::parse_casm_block(std::size_t casm_offset) {
    // Scan forward up to 32 bytes for "CASM" tag
    std::size_t pos = casm_offset;
    bool found = false;
    for (int scan = 0; scan < 32 && pos + 4 <= raw_.size(); ++scan, ++pos) {
        if (std::memcmp(raw_.data() + pos, "CASM", 4) == 0) {
            found = true; break;
        }
    }
    if (!found) {
        std::cout << "[CASM] No CASM block found\n";
        return;
    }
    if (pos + 8 > raw_.size()) return;
    const u8* p = raw_.data() + pos + 4;
    const u32 casm_len = (static_cast<u32>(p[0]) << 24) |
                         (static_cast<u32>(p[1]) << 16) |
                         (static_cast<u32>(p[2]) <<  8) |
                          static_cast<u32>(p[3]);
    const std::size_t start = pos + 8;
    if (start + casm_len > raw_.size()) {
        std::cout << "[CASM] CASM block extends past end of file\n";
        return;
    }

    const std::size_t end = start + casm_len;
    std::size_t cpos = start;
    while (cpos + 8 <= end) {
        char tag[4];
        std::memcpy(tag, raw_.data() + cpos, 4);
        if (std::memcmp(tag, "CSEG", 4) != 0) break;
        const u8* q = raw_.data() + cpos + 4;
        const u32 cseg_len = (static_cast<u32>(q[0]) << 24) |
                             (static_cast<u32>(q[1]) << 16) |
                             (static_cast<u32>(q[2]) <<  8) |
                              static_cast<u32>(q[3]);
        const std::size_t cseg_start = cpos + 8;
        if (cseg_start + cseg_len > end) break;

        CasmCseg cseg{};
        parse_cseg(std::span<const u8>(raw_.data() + cseg_start, cseg_len), cseg);
        if (!cseg.sec_names.empty()) {
            casm_.csegs.push_back(std::move(cseg));
        }
        cpos = cseg_start + cseg_len;
    }

    casm_.present = !casm_.csegs.empty();
    std::cout << std::format("[CASM] Parsed {} CSEG(s)\n", casm_.csegs.size());
}

void PrsStyle::attach_casm(std::size_t last_trk_end) {
    parse_casm_block(last_trk_end);
}

// =========================================================
//  CASM lookup API
// =========================================================
const CasmChannel*
PrsStyle::casm_get_channel(std::string_view section_name, u8 src_ch) const {
    if (!casm_.present) return nullptr;

    for (const auto& cseg : casm_.csegs) {
        const bool name_match = std::ranges::any_of(
            cseg.sec_names,
            [&](std::string_view n) noexcept { return iequals(n, section_name); });
        if (!name_match) continue;

        for (const auto& ch : cseg.channels) {
            if (ch.src_ch == src_ch) return &ch;
        }
        // Found the right CSEG but channel not listed — stop here.
        return nullptr;
    }
    return nullptr;
}

const NoteRange*
PrsStyle::casm_get_range(const CasmChannel& cc, u8 pitch) {
    if (cc.mid_low_pitch > 0 && pitch < cc.mid_low_pitch) return &cc.low;
    if (cc.mid_high_pitch < 127 && pitch > cc.mid_high_pitch) return &cc.high;
    return &cc.mid;
}

bool PrsStyle::casm_is_drums(std::string_view section_name, u8 src_ch) const {
    const CasmChannel* cc = casm_get_channel(section_name, src_ch);
    return cc ? cc->is_drums : false;
}

// Note mute check: returns true if the note's pitch class is allowed.
// The note_mute_mask is a 16-bit value where each bit maps to a pitch class:
//   Bit 15 = B,    Bit 14 = Bb,   Bit 13 = A,    Bit 12 = G#
//   Bit 11 = G,    Bit 10 = F#,   Bit  9 = F,    Bit  8 = E
//   Bit  7 = Eb,   Bit  6 = D,    Bit  5 = C#,   Bit  4 = C
//   Bits 3-0 = unused (always 1 = allowed)
// Bit=1 → allowed, Bit=0 → muted.
// 0xFFFF = all allowed (default).
bool PrsStyle::casm_is_note_allowed(const CasmChannel& cc, u8 pitch) noexcept {
    if (cc.note_mute_mask == 0xFFFF) return true;  // fast path: all allowed
    const int pc = pitch % 12;
    // Map pitch class to bit position (15 = B down to 4 = C)
    const int bit = 15 - pc;
    return (cc.note_mute_mask >> bit) & 1;
}

// Chord mute check: returns true if the channel should play for this chord type.
// casm_chord_type: CASM index (0..34) of the detected chord.
bool PrsStyle::casm_is_chord_allowed(const CasmChannel& cc,
                                     int casm_chord_type) noexcept {
    if (cc.chord_mute_all) return true;  // fast path: all chord types allowed

    // "cancel" chord type (0x22 = 34) mutes all non-rhythm channels
    if (casm_chord_type == 34) return false;

    // Look up the bit position for this chord type
    if (casm_chord_type < 0 || casm_chord_type >= 35) return true;  // unknown → allow
    const int bit_pos = kChordMuteBitPos[static_cast<std::size_t>(casm_chord_type)];
    if (bit_pos < 0) return true;  // not in mask → always allowed

    // Check the bit in chord_mask
    return (cc.chord_mask >> bit_pos) & 1;
}

void PrsStyle::casm_dump() const {
    if (!casm_.present) {
        std::cout << "[CASM] Not present\n";
        return;
    }
    std::cout << std::format("[CASM] {} CSEG(s)\n", casm_.csegs.size());
    for (std::size_t ci = 0; ci < casm_.csegs.size(); ++ci) {
        const auto& cseg = casm_.csegs[ci];
        std::cout << std::format("  CSEG {}  sections:", ci);
        for (const auto& n : cseg.sec_names)
            std::cout << std::format(" \"{}\"", n);
        std::cout << '\n';

        for (const auto& cc : cseg.channels) {
            const char* src_ct = (cc.src_chord_type <= 0x22)
                                     ? kChordTypeNames[cc.src_chord_type].data()
                                     : "?";
            std::cout << std::format(
                "    ch{:02}->ch{:02}  {:<10}  src={}{}  drums={}  mask={:010X}\n",
                cc.src_ch + 1, cc.dst_ch + 1, cc.part_name,
                kNoteNames[cc.src_root], src_ct,
                cc.is_drums ? 1 : 0, cc.chord_mask);

            std::cout << std::format(
                "      mid [{:3}-{:3}] NTR={} NTT={:02X} bass={} hkey={} lo={} hi={} RTR={}\n",
                static_cast<int>(cc.mid_low_pitch),
                static_cast<int>(cc.mid_high_pitch),
                static_cast<int>(cc.mid.ntr),
                static_cast<unsigned>(cc.mid.ntt),
                cc.mid.bass_on ? 1 : 0,
                static_cast<int>(cc.mid.high_key),
                static_cast<int>(cc.mid.note_low),
                static_cast<int>(cc.mid.note_high),
                static_cast<int>(cc.mid.rtr));

            if (cc.mid_low_pitch > 0) {
                std::cout << std::format(
                    "      low [0  -{:3}] NTR={} NTT={:02X} RTR={}\n",
                    static_cast<int>(cc.mid_low_pitch - 1),
                    static_cast<int>(cc.low.ntr),
                    static_cast<unsigned>(cc.low.ntt),
                    static_cast<int>(cc.low.rtr));
            }
            if (cc.mid_high_pitch < 127) {
                std::cout << std::format(
                    "      high[{:3}-127] NTR={} NTT={:02X} RTR={}\n",
                    static_cast<int>(cc.mid_high_pitch + 1),
                    static_cast<int>(cc.high.ntr),
                    static_cast<unsigned>(cc.high.ntt),
                    static_cast<int>(cc.high.rtr));
            }
        }
    }
}

// =========================================================
//  Fire-time cache
// =========================================================
namespace {
    constexpr u32 kCacheMagic   = 0x5052'5343u;   // 'PRSC'
    constexpr u32 kCacheVersion = 1u;
}

std::filesystem::path PrsStyle::cache_path() const {
    auto name = std::format("prs_cache_{:08X}.bin", raw_hash_);
    return std::filesystem::temp_directory_path() / name;
}

bool PrsStyle::cache_load() {
    const auto path = cache_path();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    u32 hdr[4];
    f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (!f) return false;
    if (hdr[0] != kCacheMagic || hdr[1] != kCacheVersion) return false;
    if (hdr[2] != raw_hash_) return false;
    if (hdr[3] != events_.size()) return false;

    fire_ns_.resize(events_.size());
    f.read(reinterpret_cast<char*>(fire_ns_.data()),
           static_cast<std::streamsize>(fire_ns_.size() * sizeof(i64)));
    if (!f) return false;

    for (auto& sec : sections_) {
        i64 d = 0;
        f.read(reinterpret_cast<char*>(&d), sizeof(d));
        if (!f) return false;
        sec.dur = std::chrono::nanoseconds(d);
    }

    std::cout << std::format("[CACHE] Loaded from {}\n", path.string());
    return true;
}

void PrsStyle::cache_save() const {
    const auto path = cache_path();
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    const u32 hdr[4] = { kCacheMagic, kCacheVersion,
                         raw_hash_, static_cast<u32>(events_.size()) };
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(fire_ns_.data()),
            static_cast<std::streamsize>(fire_ns_.size() * sizeof(i64)));
    for (const auto& sec : sections_) {
        const i64 d = sec.dur.count();
        f.write(reinterpret_cast<const char*>(&d), sizeof(d));
    }
    std::cout << std::format("[CACHE] Saved to {}\n", path.string());
}

// Compute fire times for one section. Returns the section's total duration
// in nanoseconds. Output `out_qpc` is filled with the absolute fire time
// (also in nanoseconds) for each event in [sec.ev_start, sec.ev_end).
std::chrono::nanoseconds
PrsStyle::compute_section_fire_times(const StyleSection& sec,
                                     u32 base_tempo_us,
                                     u32 ticks_per_qn,
                                     u32 dur_ticks,
                                     std::span<const Event> events,
                                     std::span<i64> out_qpc,
                                     std::chrono::nanoseconds::rep qpc_freq)
{
    struct TempoSeg { u32 tick; u32 tempo_us; };
    std::vector<TempoSeg> segs;
    segs.reserve(128);
    segs.push_back({0u, base_tempo_us});

    const u32 base = sec.start_tick;
    for (std::size_t i = sec.ev_start; i < sec.ev_end && i < events.size(); ++i) {
        const Event& ev = events[i];
        if (ev.kind == EventKind::Tempo && segs.size() < 128) {
            const u32 t = (static_cast<u32>(ev.d0) << 16) |
                          (static_cast<u32>(ev.d1) <<  8) |
                           static_cast<u32>(ev.status);
            segs.push_back({ ev.tick - base, t });
        }
    }

    // Cumulative microseconds at each segment boundary
    std::vector<i64> cumul_us(segs.size());
    cumul_us[0] = 0;
    for (std::size_t s = 1; s < segs.size(); ++s) {
        const i64 span = static_cast<i64>(segs[s].tick) -
                         static_cast<i64>(segs[s - 1].tick);
        cumul_us[s] = cumul_us[s - 1] +
                      span * static_cast<i64>(segs[s - 1].tempo_us) /
                      static_cast<i64>(ticks_per_qn);
    }

    // Per-event fire time
    for (std::size_t i = sec.ev_start; i < sec.ev_end && i < events.size(); ++i) {
        const u32 rel = events[i].tick - base;
        std::size_t s = segs.size() - 1;
        while (s > 0 && segs[s].tick > rel) --s;
        const i64 us = cumul_us[s] +
                       (static_cast<i64>(rel) - static_cast<i64>(segs[s].tick)) *
                       static_cast<i64>(segs[s].tempo_us) /
                       static_cast<i64>(ticks_per_qn);
        out_qpc[i] = us * qpc_freq / 1'000'000LL;
    }

    // Section end time
    std::size_t s = segs.size() - 1;
    while (s > 0 && segs[s].tick > dur_ticks) --s;
    const i64 end_us = cumul_us[s] +
                       (static_cast<i64>(dur_ticks) - static_cast<i64>(segs[s].tick)) *
                       static_cast<i64>(segs[s].tempo_us) /
                       static_cast<i64>(ticks_per_qn);
    return std::chrono::nanoseconds(end_us * qpc_freq / 1'000'000LL);
}

void PrsStyle::cache_all_sections() {
    if (cache_load()) return;

    std::cout << std::format("[CACHE] Pre-computing fire-times for {} sections...\n",
                              sections_.size());

    fire_ns_.assign(events_.size(), 0);

    for (std::size_t i = 0; i < sections_.size(); ++i) {
        StyleSection& sec = sections_[i];

        u32 dur_ticks = sec.end_tick - sec.start_tick;
        if (dur_ticks == 0 || dur_ticks == 0xFFFF'FFFFu) {
            u32 last = 0;
            for (std::size_t j = sec.ev_start; j < sec.ev_end && j < events_.size(); ++j) {
                const u32 r = events_[j].tick - sec.start_tick;
                if (r > last) last = r;
            }
            dur_ticks = last + ticks_per_qn_ * 2u;
        }

        sec.dur = compute_section_fire_times(sec, tempo_us_, ticks_per_qn_,
                                             dur_ticks, events_, fire_ns_,
                                             kClockFreq);

        std::cout << std::format("  [{:2}] {:<16}  {} events  {:.2f} s\n",
            i, sec.name, sec.ev_end - sec.ev_start,
            static_cast<double>(sec.dur.count()) / kClockFreq);
    }

    cache_save();
}

// =========================================================
//  send_synth_init — one-time synth reset (static, style-independent)
// =========================================================
void PrsStyle::send_synth_init(MidiSink& midi) {
    constexpr u8 MIDI_CC = 0xB0;

    // XG System On
    {
        const u8 xg_on[] = {0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00};
        midi.sysex_nowait(std::span<const u8>(xg_on, sizeof(xg_on)));
    }

    // Per-channel CC reset block (all 16 channels)
    for (int ch = 0; ch < 16; ++ch) {
        const u8 cc = static_cast<u8>(MIDI_CC | ch);
        const u8 pb = static_cast<u8>(0xE0 | ch);
        const u8 cp = static_cast<u8>(0xD0 | ch);

        midi.short_direct(cc, 120, 0);   // All Sound Off
        midi.short_direct(cc, 123, 0);   // All Notes Off
        midi.short_direct(cc, 121, 0);   // Reset All Controllers
        midi.short_direct(cc, 119, 0);
        midi.short_direct(cc,  11, 127);
        midi.short_direct(cc,   1,   0);
        midi.short_direct(pb,   0,  64);
        midi.short_direct(cc,  64,   0);
        midi.short_direct(cc,  84,   0);
        midi.short_direct(cc,  85,  64);
        midi.short_direct(cc,  74,  64);
        midi.short_direct(cc,  71,  64);
        midi.short_direct(cc,  78,  64);
        midi.short_direct(cc,  76,  64);
        midi.short_direct(cc,  77,  64);

        midi.short_direct(cc, 99, 1);  midi.short_direct(cc, 98, 0x11);
        midi.short_direct(cc,  6, 0x40);
        midi.short_direct(cc, 99, 1);  midi.short_direct(cc, 98, 0x10);
        midi.short_direct(cc,  6, 0x40);
        midi.short_direct(cc, 99, 1);  midi.short_direct(cc, 98, 0x12);
        midi.short_direct(cc,  6, 0x40);
        midi.short_direct(cc, 99, 1);  midi.short_direct(cc, 98, 0x13);
        midi.short_direct(cc,  6, 0x40);

        midi.short_direct(cc, 73, 64);
        midi.short_direct(cc, 99, 1);  midi.short_direct(cc, 98, 0x64);
        midi.short_direct(cc,  6, 0x40);
        midi.short_direct(cc, 99, 1);  midi.short_direct(cc, 98, 0x6B);
        midi.short_direct(cc,  6, 0x40);

        midi.short_direct(cc, 72, 64);
        midi.short_direct(cc, 91,  0);
        midi.short_direct(cc, 93,  0);
        midi.short_direct(cc, 94,  0);
        midi.short_direct(cc, 10, 64);
        midi.short_direct(cc, 65,  0);
        midi.short_direct(cc,  5,  0);

        midi.short_direct(cc, 101, 0);  midi.short_direct(cc, 100, 1);
        midi.short_direct(cc,   6, 0x40);
        midi.short_direct(cc, 101, 0);  midi.short_direct(cc, 100, 0);
        midi.short_direct(cc,   6, 2);

        midi.short_direct(cp, 0, 0);
        midi.short_direct(cc, 127, 0);
    }

    // GM2 System On
    {
        const u8 gm2_on[] = {0x7F, 0x7F, 0x04, 0x01, 0x00, 0x7F};
        midi.sysex_nowait(std::span<const u8>(gm2_on, sizeof(gm2_on)));
    }

    midi.sysex_flush();
    midi.sleep_ms(10);
}

// =========================================================
//  send_header_sysex — replay the style's pre-SInt SysEx stream
//
//  Only the SysEx (XG effect types, drum setup, ...). The per-channel
//  voices / volumes / pans come from the channel model instead (see
//  build_channel_setup + ChannelModel::apply_setup), so the model and
//  the device can never disagree.
//
//  Batched flush: WinMidiOutput::sysex_nowait() uses a pool of 16 small
//  buffers that wraps around; flushing every 8 messages guarantees a
//  buffer is never reused while its message is still in flight.
// =========================================================
void PrsStyle::send_header_sysex(MidiSink& midi) const {
    const u8* p = hdr_sx_.data();
    int rem = static_cast<int>(hdr_sx_.size());
    int sent = 0;
    while (rem > 0) {
        if (*p != 0xF0) { ++p; --rem; continue; }
        const u8* eox = p + 1;
        while (eox < p + rem && *eox != 0xF7) ++eox;
        const int sl = static_cast<int>(eox - p);
        if (sl > 1) {
            midi.sysex_nowait(std::span<const u8>(p + 1, static_cast<std::size_t>(sl - 1)));
            if (++sent % 8 == 0) midi.sysex_flush();
        }
        p   += sl + 1;
        rem -= sl + 1;
    }
    midi.sysex_flush();
}

// =========================================================
//  build_channel_setup — the style's per-channel startup state
//
//  For every source channel the style defines a voice for:
//    1. apply the voice remap (bank/PC translation)
//    2. route it to its OUTPUT channel:
//         Montage off -> CASM dst_ch (first CSEG)
//         Montage on  -> CASM bypassed, folded into channels 1-8
//       (both decided by RemapSource::route_channel)
//    3. convert the internal 1-based PC to the 0-127 wire value
//
//  The results are NOT sent anywhere here — the caller hands them to
//  ChannelModel::apply_setup(), which stores them and syncs the device.
// =========================================================
std::vector<ChannelSetup>
PrsStyle::build_channel_setup(const RemapSource& remap) const {
    // CASM src_ch -> dst_ch map (first CSEG), identity by default.
    std::array<u8, 16> dst_ch_map{};
    for (int i = 0; i < 16; ++i) dst_ch_map[static_cast<std::size_t>(i)] = static_cast<u8>(i);
    if (!casm_.csegs.empty()) {
        for (const auto& cc : casm_.csegs[0].channels) {
            dst_ch_map[cc.src_ch & 0x0F] = static_cast<u8>(cc.dst_ch & 0x0F);
        }
    }

    std::vector<ChannelSetup> out;
    out.reserve(16);

    for (std::size_t ch = 0; ch < ch_voices_.size(); ++ch) {
        const ChVoice& v = ch_voices_[ch];
        if (!v.valid) continue;

        u8 msb = v.bank_msb, lsb = v.bank_lsb, pc = v.pc;
        u8 dm = msb, dl = lsb, dp = pc;
        if (remap.apply(msb, lsb, pc, dm, dl, dp)) {
            msb = dm; lsb = dl; pc = dp;
        }

        ChannelSetup s;
        s.ch         = static_cast<u8>(remap.route_channel(static_cast<u8>(ch), dst_ch_map[ch]) & 0x0F);
        s.msb        = static_cast<u8>(msb & 0x7F);
        s.lsb        = static_cast<u8>(lsb & 0x7F);
        s.program    = pc_to_wire(pc);
        s.volume     = v.volume;
        s.pan        = v.pan;
        s.expression = v.expression;
        s.reverb     = v.reverb;
        s.chorus     = v.chorus;
        s.variation  = v.variation;
        s.filter_res = v.filter_res;
        s.release    = v.release;
        s.attack     = v.attack;
        s.brightness = v.brightness;
        out.push_back(s);
    }
    return out;
}

// =========================================================
//  send_bass_legato_setup — configure bass channel for mono legato
// =========================================================
void PrsStyle::send_bass_legato_setup(MidiSink& midi, u8 ch) const {
    constexpr u8 MIDI_CC = 0xB0;
    const u8 cc = static_cast<u8>(MIDI_CC | ch);
    midi.short_direct(cc,   5,  1);    // Portamento Time = 1
    midi.short_direct(cc,  65, 127);   // Portamento = On
    midi.short_direct(cc, 126, 0);     // Mono Mode On
}

// =========================================================
//  SysEx decoder
// =========================================================
namespace {

// Reverb type names (XG spec, address 02 01 00).
constexpr std::string_view kReverbNames[] = {
    "Off",        "Hall1",      "Hall2",      "Hall3",
    "Room1",      "Room2",      "Room3",      "Stage1",
    "Stage2",     "Plate1",     "Plate2",     "White Room",
    "Tunnel",     "Canyon",     "Basement",   "",
};

// Chorus/variation type names (XG spec, address 02 02 00 / 02 03 00).
constexpr std::string_view kChorusNames[] = {
    "Off",        "Chorus1",    "Chorus2",    "Chorus3",
    "Chorus4",    "Celeste1",   "Celeste2",   "Celeste3",
    "Celeste4",   "Flanger1",   "Flanger2",   "Flanger3",
    "Flanger4",   "Phaser1",    "Phaser2",    "Phaser3",
    "Phaser4",    "Rotor",      "Distortion", "Echo",
    "AutoPan",    "AutoPan2",   "",
};

// Multi Part parameter names (XG spec, address 08 <part> <param>).
// Index is the param byte.
constexpr std::string_view kPartParamNames[] = {
    "bank_msb",       // 00
    "bank_lsb",       // 01
    "program",        // 02
    "rcv_channel",    // 03
    "mono_poly",      // 04
    "part_mode",      // 05  (0=Normal, 1=Drum, 2=Off)
    "volume",         // 06
    "pan",            // 07
    "reverb_send",    // 08
    "chorus_send",    // 09
    "variation_send", // 0A
    "vibrato_rate",   // 0B
    "vibrato_depth",  // 0C
    "vibrato_delay",  // 0D
    "filter_cutoff",  // 0E
    "filter_reson",   // 0F
    "eg_attack",      // 10
    "eg_decay",       // 11
    "eg_sustain",     // 12
    "eg_release",     // 13
    "pitch_offset",   // 14
};

[[nodiscard]] std::string_view part_param_name_impl(u8 param) noexcept {
    if (param < std::size(kPartParamNames)) return kPartParamNames[param];
    switch (param) {
        case 0x1E: return "pitch_offset_fine";
        case 0x20: return "eq_low_freq";
        case 0x21: return "eq_mid_freq";
        case 0x22: return "eq_high_freq";
        case 0x4E: return "key_follow";
        case 0x50: return "pitch_bend_range";
        case 0x51: return "pitch_bend_step";
        case 0x52: return "mod_pitch_range";
        case 0x5B: return "hpf_freq";
        case 0x5D: return "eq_low_gain";
        case 0x5E: return "eq_mid_gain";
        case 0x5F: return "eq_high_gain";
        case 0x6E: return "dry_level";
        case 0x72: return "chorus_send_alt";
        case 0x73: return "variation_send_alt";
        case 0x76: return "reverb_send_alt";
        case 0x77: return "dry_level_alt";
        default:   return "?";
    }
}

[[nodiscard]] std::string hex_bytes(std::span<const u8> data) {
    std::string s;
    s.reserve(data.size() * 3);
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i) s += ' ';
        s += std::format("{:02X}", data[i]);
    }
    return s;
}

} // namespace

namespace sysex {

std::string_view kind_name(Kind k) noexcept {
    switch (k) {
        case Kind::GM_System_On:        return "GM System On";
        case Kind::GM_Master_Volume:    return "GM Master Volume";
        case Kind::XG_System_On:        return "XG System On";
        case Kind::XG_Master_Volume:    return "XG Master Volume";
        case Kind::XG_Master_Tune:      return "XG Master Tune";
        case Kind::XG_Reverb_Type:      return "XG Reverb Type";
        case Kind::XG_Reverb_Param:     return "XG Reverb Param";
        case Kind::XG_Chorus_Type:      return "XG Chorus Type";
        case Kind::XG_Chorus_Param:     return "XG Chorus Param";
        case Kind::XG_Variation_Type:   return "XG Variation Type";
        case Kind::XG_Variation_Param:  return "XG Variation Param";
        case Kind::XG_Insertion_Type:   return "XG Insertion Type";
        case Kind::XG_Multi_EQ:         return "XG Multi EQ";
        case Kind::XG_Multi_Part_Param: return "XG Multi Part Param";
        case Kind::XG_Drum_Setup:       return "XG Drum Setup";
        case Kind::XG_Drum_Param:       return "XG Drum Param";
        case Kind::Yamaha_Song_Setup:   return "Yamaha Song Setup";
        case Kind::Yamaha_Style_Setup:  return "Yamaha Style Setup";
        case Kind::Yamaha_OTS_Setup:    return "Yamaha OTS Setup";
        case Kind::Unknown_Yamaha:      return "Yamaha (unknown)";
        case Kind::Unknown_Other:       return "Unknown";
    }
    return "?";
}

std::string_view reverb_type_name(u8 type) noexcept {
    return type < std::size(kReverbNames) ? kReverbNames[type] : "?";
}
std::string_view chorus_type_name(u8 type) noexcept {
    return type < std::size(kChorusNames) ? kChorusNames[type] : "?";
}
std::string_view part_param_name(u8 param) noexcept {
    return part_param_name_impl(param);
}

Message decode(std::span<const u8> payload) noexcept {
    Message m;
    m.raw.assign(payload.begin(), payload.end());

    if (payload.size() < 1) { m.kind = Kind::Unknown_Other; return m; }

    // --- GM System On: 7E 7F 09 01 ---
    if (payload.size() >= 4 &&
        payload[0] == 0x7E && payload[1] == 0x7F &&
        payload[2] == 0x09 && payload[3] == 0x01) {
        m.kind = Kind::GM_System_On;
        m.description = "GM System On";
        return m;
    }
    // --- GM Master Volume: 7F 7F 04 01 ll hh ---
    if (payload.size() >= 6 &&
        payload[0] == 0x7F && payload[1] == 0x7F &&
        payload[2] == 0x04 && payload[3] == 0x01) {
        m.kind = Kind::GM_Master_Volume;
        const u16 vol14 = static_cast<u16>(payload[4]) |
                          (static_cast<u16>(payload[5] & 0x7F) << 7);
        m.value = static_cast<u8>(vol14 & 0xFF);
        m.description = std::format("GM Master Volume = {} (14-bit: {})",
            vol14 >> 7, vol14);
        return m;
    }

    // --- Yamaha (manufacturer 0x43) ---
    if (payload[0] == 0x43) {
        // payload[1] = device number (high nibble) + sub-status (low nibble)
        // For XG: sub-status 0x0 → model ID follows at [2]
        const u8 sub = payload[1] & 0x0F;

        // Yamaha Song Setup: 43 76 1A ...
        if (payload.size() >= 3 && payload[1] == 0x76 && payload[2] == 0x1A) {
            m.kind = Kind::Yamaha_Song_Setup;
            m.description = std::format("Yamaha Song Setup ({} bytes)",
                payload.size());
            return m;
        }
        // Style Setup: 43 73 39 ...
        if (payload.size() >= 3 && payload[1] == 0x73 && payload[2] == 0x39) {
            m.kind = Kind::Yamaha_Style_Setup;
            m.description = std::format("Yamaha Style Setup ({} bytes)",
                payload.size());
            return m;
        }
        // OTS Setup: 43 73 01 51 ...
        if (payload.size() >= 4 && payload[1] == 0x73 &&
            payload[2] == 0x01 && payload[3] == 0x51) {
            m.kind = Kind::Yamaha_OTS_Setup;
            m.description = std::format("Yamaha OTS Setup ({} bytes)",
                payload.size());
            return m;
        }

        // XG: 43 10 4C <addr...> <data...>
        // sub-status 0x0, model ID 0x4C
        if (sub == 0x0 && payload.size() >= 3 && payload[2] == 0x4C) {
            const std::size_t after_model = 3;
            const u8* a = payload.data() + after_model;
            const std::size_t arem = payload.size() - after_model;

            // XG System On: 00 00 7E 00
            if (arem >= 4 && a[0] == 0x00 && a[1] == 0x00 &&
                a[2] == 0x7E && a[3] == 0x00) {
                m.kind = Kind::XG_System_On;
                m.description = "XG System On";
                return m;
            }
            // XG Master Volume: 00 00 7F xx
            if (arem >= 4 && a[0] == 0x00 && a[1] == 0x00 &&
                a[2] == 0x7F) {
                m.kind = Kind::XG_Master_Volume;
                m.value = a[3];
                m.description = std::format("XG Master Volume = {}", a[3]);
                return m;
            }
            // XG Master Tune: 00 00 06 ...
            if (arem >= 2 && a[0] == 0x00 && a[1] == 0x00 && arem >= 3 && a[2] == 0x06) {
                m.kind = Kind::XG_Master_Tune;
                m.description = "XG Master Tune";
                return m;
            }

            // Reverb type/param: 02 01 <param> <value>
            if (arem >= 4 && a[0] == 0x02 && a[1] == 0x01) {
                m.param = a[2];
                m.value = a[3];
                if (a[2] == 0x00) {
                    m.kind = Kind::XG_Reverb_Type;
                    m.description = std::format("Reverb Type = {} ({})",
                        reverb_type_name(a[3]), a[3]);
                } else {
                    m.kind = Kind::XG_Reverb_Param;
                    m.description = std::format("Reverb param {:02X} = {}",
                        a[2], a[3]);
                }
                return m;
            }
            // Chorus type/param: 02 02 <param> <value>
            if (arem >= 4 && a[0] == 0x02 && a[1] == 0x02) {
                m.param = a[2];
                m.value = a[3];
                if (a[2] == 0x00) {
                    m.kind = Kind::XG_Chorus_Type;
                    m.description = std::format("Chorus Type = {} ({})",
                        chorus_type_name(a[3]), a[3]);
                } else {
                    m.kind = Kind::XG_Chorus_Param;
                    m.description = std::format("Chorus param {:02X} = {}",
                        a[2], a[3]);
                }
                return m;
            }
            // Variation type/param: 02 03 <param> <value>
            if (arem >= 4 && a[0] == 0x02 && a[1] == 0x03) {
                m.param = a[2];
                m.value = a[3];
                if (a[2] == 0x00) {
                    m.kind = Kind::XG_Variation_Type;
                    m.description = std::format("Variation Type = {} ({})",
                        chorus_type_name(a[3]), a[3]);
                } else {
                    m.kind = Kind::XG_Variation_Param;
                    m.description = std::format("Variation param {:02X} = {}",
                        a[2], a[3]);
                }
                return m;
            }
            // Insertion type: 02 04 00 xx
            if (arem >= 4 && a[0] == 0x02 && a[1] == 0x04) {
                m.kind = Kind::XG_Insertion_Type;
                m.param = a[2];
                m.value = a[3];
                m.description = std::format("Insertion param {:02X} = {}",
                    a[2], a[3]);
                return m;
            }
            // Multi EQ: 03 <band> <param> <value...>
            // band 0=Off, 1..3 = EQ bands 1..3
            if (arem >= 4 && a[0] == 0x03) {
                m.kind = Kind::XG_Multi_EQ;
                m.part  = a[1];  // band
                m.param = a[2];
                m.value = a[3];
                const char* band_name = "?";
                switch (a[1]) {
                    case 0: band_name = "Off"; break;
                    case 1: band_name = "Band1"; break;
                    case 2: band_name = "Band2"; break;
                    case 3: band_name = "Band3"; break;
                }
                m.description = std::format("Multi EQ {} param {:02X} = {}",
                    band_name, a[2], a[3]);
                return m;
            }
            // Multi Part Param: 08 <part> <param> <value>
            if (arem >= 4 && a[0] == 0x08) {
                m.kind = Kind::XG_Multi_Part_Param;
                m.part  = a[1];
                m.param = a[2];
                m.value = a[3];
                m.description = std::format("Part {} {}: {} = {}",
                    a[1], part_param_name(a[2]), a[2], a[3]);
                return m;
            }
            // Drum Setup (per key): 0A <part> <key> <param> <value>
            if (arem >= 5 && a[0] == 0x0A) {
                m.kind = Kind::XG_Drum_Setup;
                m.part  = a[1];
                m.key   = a[2];
                m.param = a[3];
                m.value = a[4];
                m.description = std::format("Drum part {} key {} param {} = {}",
                    a[1], a[2], a[3], a[4]);
                return m;
            }
            // Drum Param: 0D <part> <param> <value>
            if (arem >= 4 && a[0] == 0x0D) {
                m.kind = Kind::XG_Drum_Param;
                m.part  = a[1];
                m.param = a[2];
                m.value = a[3];
                m.description = std::format("Drum part {} param {} = {}",
                    a[1], a[2], a[3]);
                return m;
            }

            // Other XG — show address
            m.kind = Kind::Unknown_Yamaha;
            m.description = std::format("XG other: {}", hex_bytes(payload));
            return m;
        }

        // Other Yamaha
        m.kind = Kind::Unknown_Yamaha;
        m.description = std::format("Yamaha: {}", hex_bytes(payload));
        return m;
    }

    m.kind = Kind::Unknown_Other;
    m.description = std::format("Unknown: {}", hex_bytes(payload));
    return m;
}

std::vector<Message> decode_stream(std::span<const u8> stream) noexcept {
    std::vector<Message> out;
    std::size_t i = 0;
    while (i < stream.size()) {
        if (stream[i] != 0xF0) { ++i; continue; }
        // Find F7
        std::size_t j = i + 1;
        while (j < stream.size() && stream[j] != 0xF7) ++j;
        if (j >= stream.size()) break;
        // Payload is between F0 and F7
        out.push_back(decode(stream.subspan(i + 1, j - i - 1)));
        i = j + 1;
    }
    return out;
}

} // namespace sysex

// =========================================================
//  OTS (One Touch Setting) parser
//
//  Correct structure (verified on real .prs/.sty files):
//
//    OTSc chunk (appears after the CASM block):
//      [SFF2] OTSi sub-chunk: "OTSi" + be32(8) + 8 bytes of header
//            - bytes [4..5] = panel count (typically 4, one per OTS button)
//      MTrk #1  →  OTS button 1  (4 channels: R1, R2, R3, Left)
//      MTrk #2  →  OTS button 2  (4 channels: R1, R2, R3, Left)
//      MTrk #3  →  OTS button 3  (4 channels: R1, R2, R3, Left)
//      MTrk #4  →  OTS button 4  (4 channels: R1, R2, R3, Left)
//
//      [SFF1] MTrk chunk(s) directly (no OTSi header).
//
//  Each MTrk contains:
//    - XG SysEx setup (reverb/chorus type, master volume, per-part params)
//    - CC/PC events on channels 0..3, where:
//        ch 0 = Right 1 (R1)  — main melody voice
//        ch 1 = Right 2 (R2)  — layer voice
//        ch 2 = Right 3 (R3)  — extra layer voice
//        ch 3 = Left          — left-hand voice
// =========================================================
void PrsStyle::attach_ots(std::size_t search_offset) {
    // Scan the entire remaining file for "OTSc" tag.
    std::size_t pos = search_offset;
    bool found = false;
    while (pos + 8 <= raw_.size()) {
        if (std::memcmp(raw_.data() + pos, "OTSc", 4) == 0) {
            found = true; break;
        }
        ++pos;
    }
    if (!found) return;  // No OTS block — legal.

    const u8* p = raw_.data() + pos + 4;
    const u32 ots_len = (static_cast<u32>(p[0]) << 24) |
                        (static_cast<u32>(p[1]) << 16) |
                        (static_cast<u32>(p[2]) <<  8) |
                         static_cast<u32>(p[3]);
    const std::size_t payload_start = pos + 8;
    if (payload_start + ots_len > raw_.size()) {
        std::cout << "[OTS] OTSc block extends past end of file\n";
        return;
    }

    parse_ots_block(std::span<const u8>(raw_.data() + payload_start, ots_len));
}

void PrsStyle::parse_ots_block(std::span<const u8> payload) {
    ots_ = OtsBlock{};
    ots_.present = true;

    std::size_t off = 0;

    // --- Optional OTSi header (SFF2 only) ---
    if (payload.size() >= 8 &&
        std::memcmp(payload.data(), "OTSi", 4) == 0) {
        const u32 otsi_len = (static_cast<u32>(payload[4]) << 24) |
                             (static_cast<u32>(payload[5]) << 16) |
                             (static_cast<u32>(payload[6]) <<  8) |
                              static_cast<u32>(payload[7]);
        ots_.is_sff2 = true;
        if (otsi_len >= 8 && 8 + otsi_len <= payload.size()) {
            std::memcpy(ots_.otsi_header.data(), payload.data() + 8, 8);
            ots_.panel_count = static_cast<u16>(
                (static_cast<u16>(ots_.otsi_header[4]) << 8) |
                 static_cast<u16>(ots_.otsi_header[5]));
        }
        off = 8 + otsi_len;
    }

    // --- Find ALL MTrks inside OTSc. Each one is an OTS button. ---
    u8 button_number = 0;
    while (off + 8 <= payload.size()) {
        if (std::memcmp(payload.data() + off, "MTrk", 4) != 0) break;
        const u8* q = payload.data() + off + 4;
        const u32 mtrk_len = (static_cast<u32>(q[0]) << 24) |
                             (static_cast<u32>(q[1]) << 16) |
                             (static_cast<u32>(q[2]) <<  8) |
                              static_cast<u32>(q[3]);
        const std::size_t mtrk_start = off + 8;
        if (mtrk_start + mtrk_len > payload.size()) break;

        OtsButton btn{};
        btn.number = static_cast<u8>(++button_number);
        parse_ots_mtrk(payload.subspan(mtrk_start, mtrk_len), btn);
        ots_.total_sysex += btn.sysex_count;
        ots_.buttons.push_back(std::move(btn));

        off = mtrk_start + mtrk_len;
    }

    std::cout << std::format("[OTS] Parsed {} button(s) ({} SysEx total), SFF{}{}\n",
        ots_.buttons.size(), ots_.total_sysex,
        ots_.is_sff2 ? 2 : 1,
        ots_.is_sff2 ? std::format(", OTSi panel_count={}", ots_.panel_count) : "");
}

void PrsStyle::parse_ots_mtrk(std::span<const u8> mtrk_data, OtsButton& out) {
    // Parse the MTrk. Bucket CC/PC events by channel 0..3 into out.parts[].
    // Decode each SysEx into out.decoded_sysex.
    ByteReader r(mtrk_data);
    u8 running = 0;

    while (r.position() < mtrk_data.size()) {
        [[maybe_unused]] const u32 delta = r.read_vlq();
        if (r.eof()) break;
        const u8 sb = r.read_u8();

        if (sb == 0xFF) {
            // Meta event
            const u8 mt = r.read_u8();
            const u32 ml = r.read_vlq();
            (void)r.read_bytes(ml);
            if (mt == 0x2F) break;  // End of track
        } else if (sb == 0xF0 || sb == 0xF7) {
            // SysEx — decode and store
            const u32 sl = r.read_vlq();
            const auto payload = r.read_bytes(sl);
            // Skip the trailing F7 if present (already consumed above as part of sl)
            out.decoded_sysex.push_back(sysex::decode(payload));
            ++out.sysex_count;
        } else {
            u8 cmd;
            u8 status;
            if (sb & 0x80) {
                running = sb;
                cmd     = sb >> 4;
                status  = sb;
            } else {
                r.seek(r.position() - 1);
                cmd    = running >> 4;
                status = running;
            }
            const u8 ch = status & 0x0F;
            u8 b0 = 0, b1 = 0;
            if (cmd == 0x8 || cmd == 0x9 || cmd == 0xA ||
                cmd == 0xB || cmd == 0xE) {
                b0 = r.read_u8();
                b1 = r.read_u8();
            } else if (cmd == 0xC || cmd == 0xD) {
                b0 = r.read_u8();
            } else {
                continue;
            }

            // Only channels 0..3 are OTS parts (R1/R2/R3/Left)
            if (ch > 3) continue;

            OtsPanel& panel = out.parts[ch];
            panel.channel = ch;

            if (cmd == 0xB) {
                // Save raw CC (replace if controller already seen)
                bool found = false;
                for (auto& [k, v] : panel.raw_ccs) {
                    if (k == b0) { v = b1; found = true; break; }
                }
                if (!found) panel.raw_ccs.emplace_back(b0, b1);

                switch (b0) {
                    case  0: panel.bank_msb       = b1; break;
                    case  5: panel.portamento_time= b1; break;
                    case  7: panel.volume         = b1; break;
                    case 10: panel.pan            = b1; break;
                    case 11: panel.expression     = b1; break;
                    case 32: panel.bank_lsb       = b1; break;
                    case 65: panel.portamento     = b1; break;
                    case 71: panel.filter_res     = b1; break;
                    case 72: panel.release        = b1; break;
                    case 73: panel.attack         = b1; break;
                    case 74: panel.brightness     = b1; break;
                    case 91: panel.reverb         = b1; break;
                    case 93: panel.chorus         = b1; break;
                    case 94: panel.variation      = b1; break;
                    default: break;
                }
            } else if (cmd == 0xC) {
                panel.pc    = static_cast<u8>(b0 + 1);  // 1-based
                panel.valid = true;
            }
        }
    }
}

namespace {
[[nodiscard]] const char* part_label(u8 channel) noexcept {
    switch (channel) {
        case 0: return "R1";
        case 1: return "R2";
        case 2: return "R3";
        case 3: return "Left";
        default: return "?";
    }
}
} // namespace

void PrsStyle::ots_dump() const {
    if (!ots_.present) {
        std::cout << "[OTS] Not present\n";
        return;
    }

    std::cout << std::format("[OTS] {} button(s)  SFF{}  {} SysEx total",
        ots_.buttons.size(), ots_.is_sff2 ? 2 : 1, ots_.total_sysex);
    if (ots_.is_sff2) {
        std::string hex;
        for (std::size_t i = 0; i < ots_.otsi_header.size(); ++i) {
            if (i) hex += ' ';
            hex += std::format("{:02X}", ots_.otsi_header[i]);
        }
        std::cout << std::format("  OTSi header: {}", hex);
    }
    std::cout << '\n';

    for (const auto& btn : ots_.buttons) {
        std::cout << std::format("\n  === OTS Button {} ({} SysEx) ===\n",
            btn.number, btn.sysex_count);
        std::cout << "    Part  Bank/PC            Vol Pan Exp Rev Cho Var Flt Rel Att Brt Prt\n";
        for (const auto& p : btn.parts) {
            if (!p.valid && p.raw_ccs.empty()) continue;
            std::cout << std::format(
                "    {:<4}  {:3}/{:3}/{:3}  {:3} {:3} {:3} {:3} {:3} {:3} {:3} {:3} {:3} {:3} {:3}\n",
                part_label(p.channel),
                static_cast<int>(p.bank_msb), static_cast<int>(p.bank_lsb),
                static_cast<int>(p.pc),
                static_cast<int>(p.volume),    static_cast<int>(p.pan),
                static_cast<int>(p.expression),static_cast<int>(p.reverb),
                static_cast<int>(p.chorus),   static_cast<int>(p.variation),
                static_cast<int>(p.filter_res),static_cast<int>(p.release),
                static_cast<int>(p.attack),   static_cast<int>(p.brightness),
                static_cast<int>(p.portamento));
        }

        // Show interesting decoded SysEx (skip the bulk per-part params unless
        // they're not the default value).
        std::cout << std::format("    SysEx ({} decoded):\n",
            btn.decoded_sysex.size());
        for (const auto& sx : btn.decoded_sysex) {
            // Always show: GM/XG System On, Reverb/Chorus type, Master Vol
            // Show other XG messages only if they look interesting
            bool show = false;
            switch (sx.kind) {
                case sysex::Kind::GM_System_On:
                case sysex::Kind::XG_System_On:
                case sysex::Kind::XG_Reverb_Type:
                case sysex::Kind::XG_Chorus_Type:
                case sysex::Kind::XG_Variation_Type:
                case sysex::Kind::XG_Master_Volume:
                case sysex::Kind::Yamaha_Song_Setup:
                case sysex::Kind::Yamaha_Style_Setup:
                case sysex::Kind::Yamaha_OTS_Setup:
                    show = true;
                    break;
                case sysex::Kind::XG_Multi_Part_Param:
                    // Show non-default part params
                    if (sx.param == 0x06 /*volume*/ ||
                        sx.param == 0x07 /*pan*/   ||
                        sx.param == 0x05 /*part_mode*/) show = true;
                    break;
                default: break;
            }
            if (show) {
                std::cout << std::format("      {}\n", sx.description);
            }
        }
    }
}

void PrsStyle::sysex_dump() const {
    // --- Header (pre-SInt) SysEx ---
    std::cout << std::format("[SYSEX] {} messages in header (pre-SInt) stream\n",
        decoded_hdr_sysex_.size());
    for (const auto& m : decoded_hdr_sysex_) {
        std::cout << std::format("  {}\n", m.description);
    }

    // --- Section (post-SInt) SysEx — the interesting XG setup ---
    std::cout << std::format("\n[SYSEX] {} messages in section (post-SInt) stream\n",
        decoded_section_sysex_.size());

    // Show all "interesting" messages: system on, reverb/chorus type,
    // master volume, and multi-part setup (bank/PC/vol/pan).
    for (const auto& m : decoded_section_sysex_) {
        bool show = false;
        switch (m.kind) {
            case sysex::Kind::GM_System_On:
            case sysex::Kind::XG_System_On:
            case sysex::Kind::XG_Reverb_Type:
            case sysex::Kind::XG_Chorus_Type:
            case sysex::Kind::XG_Variation_Type:
            case sysex::Kind::XG_Master_Volume:
            case sysex::Kind::Yamaha_Song_Setup:
            case sysex::Kind::Yamaha_Style_Setup:
            case sysex::Kind::Yamaha_OTS_Setup:
                show = true;
                break;
            case sysex::Kind::XG_Multi_Part_Param:
                // Show part setup params: bank/PC/vol/pan/part_mode
                if (m.param <= 0x07) show = true;
                break;
            default: break;
        }
        if (show) {
            std::cout << std::format("  {}\n", m.description);
        }
    }

    // Tally by kind for the section stream
    std::array<std::size_t, 32> counts{};
    for (const auto& m : decoded_section_sysex_) {
        const auto k = static_cast<unsigned>(m.kind);
        if (k < counts.size()) ++counts[k];
    }
    std::cout << "\n  Tally: ";
    bool first = true;
    for (std::size_t k = 0; k < counts.size(); ++k) {
        if (counts[k] == 0) continue;
        if (!first) std::cout << ", ";
        first = false;
        std::cout << std::format("{}={}",
            sysex::kind_name(static_cast<sysex::Kind>(k)), counts[k]);
    }
    std::cout << '\n';
}

} // namespace prs