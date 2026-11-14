// style_foreign.cpp — Ketron (KPAT) and Korg PA style parsers, plus the
// SFF-BASIC fallback CASM synthesizer.
//
// Clean-room implementation of the on-disk behavior of these formats as
// exercised by other arranger software; no third-party code is copied.
// Both parsers convert their native formats into PSRstudio's shared model
// (sections + a flat Event list + a synthetic default CASM), so the
// existing player, transposer and mixer work unchanged.
//
// Supported:
//   - Korg PA ("G8"/"EV" magic, MSE pointer tables): full support.
//   - Ketron ("KPAT", header byte[4] == 0 — the classic layout): notes,
//     lengths and the 12-slot chord-variant masks flattened into
//     MAJ/MIN/7TH section variants.
//   - SFF-BASIC (Yamaha styles without CASM): synthetic per-channel roles.
//
// Deviations / known limits (deliberate, documented):
//   - Ketron layouts with header byte[4] != 0 (SD-series styles) are not
//     supported yet — parse fails cleanly with a log line.
//   - Ketron control records (command class 1/2) are skipped; voice setup
//     falls back to GM program 0 per channel until a real file is tested.
//   - Korg records of unknown type (16/14) are skipped.

#include "style.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using prs::u8;
using prs::u16;
using prs::u32;

// ---------------------------------------------------------
//  MSB-first bit list over a byte buffer (bit i = byte[i/8] >> (7-i%8)).
// ---------------------------------------------------------
[[nodiscard]] std::vector<bool> make_bitlist(const std::vector<u8>& bytes) {
    std::vector<bool> bits;
    bits.reserve(bytes.size() * 8);
    for (const u8 b : bytes) {
        for (int k = 7; k >= 0; --k) {
            bits.push_back(((b >> k) & 1) != 0);
        }
    }
    return bits;
}

// Read `count` bits starting at `pos`, MSB-first, then shift the value
// LEFT by `shift` (mirrors the reference bit-reader, whose 4th parameter
// is a shift amount: bit k gets weight 2^(count-1-k+shift)).
[[nodiscard]] int read_bits(const std::vector<bool>& bits, std::size_t pos,
                            int count, int shift) {
    if (pos + static_cast<std::size_t>(count) > bits.size()) return 0;
    int v = 0;
    for (int k = 0; k < count; ++k) {
        v = (v << 1) | (bits[pos + static_cast<std::size_t>(k)] ? 1 : 0);
    }
    return v << shift;
}

[[nodiscard]] bool bits_all_set(const std::vector<bool>& bits,
                                std::size_t pos, int count) {
    if (pos + static_cast<std::size_t>(count) > bits.size()) return false;
    for (int k = 0; k < count; ++k) {
        if (!bits[pos + static_cast<std::size_t>(k)]) return false;
    }
    return true;
}

[[nodiscard]] std::string read_ascii(const std::vector<u8>& bytes,
                                     std::size_t off, std::size_t len) {
    std::string s;
    if (off >= bytes.size()) return s;
    if (off + len > bytes.size()) len = bytes.size() - off;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        const char c = static_cast<char>(bytes[off + i]);
        if (c == '\0') break;
        s.push_back(c);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

// ---------------------------------------------------------
//  Shared: part -> source channel (0-based) mapping using the standard
//  SFF accompaniment layout, so the whole PSRstudio pipeline (CASM
//  roles, mixer part names, drum detection) applies to foreign styles.
//    part 0 = drums   -> ch 9  (MIDI 10)
//    part 1 = perc    -> ch 8  (MIDI 9)
//    part 2 = bass    -> ch 10 (MIDI 11)
//    part 3 = chord 1 -> ch 11 (MIDI 12)
//    part 4 = chord 2 -> ch 12 (MIDI 13)
//    part 5 = pad     -> ch 13 (MIDI 14)
//    part 6 = phrase 1-> ch 14 (MIDI 15)
//    part 7 = phrase 2-> ch 15 (MIDI 16)
// ---------------------------------------------------------
[[nodiscard]] u8 part_to_channel(u8 part) noexcept {
    constexpr u8 kMap[8] = {9, 8, 10, 11, 12, 13, 14, 15};
    return (part < 8) ? kMap[part] : 9;
}

} // namespace

namespace prs {

// =========================================================
//  synthesize_default_casm — default per-channel roles for formats
//  without a real CASM (SFF-BASIC Yamaha styles; Korg styles, which are
//  emitted on the SFF channels above).
// =========================================================
void PrsStyle::synthesize_default_casm(const std::vector<std::string>& sec_names) {
    CasmCseg cseg;
    cseg.sec_names = sec_names;
    cseg.channels.assign(16, CasmChannel{});

    struct Role { u8 ch; Ntr ntr; Ntt ntt; bool bass_on; u8 note_low;
                  const char* part; bool drums; };
    static const Role kRoles[] = {
        // ch    NTR            NTT         bass   low  part       drums
        { 8,  Ntr::RootFixed,  Ntt::Bypass, false, 0,  "Perc",    true  },
        { 9,  Ntr::RootFixed,  Ntt::Bypass, false, 0,  "Drums",   true  },
        { 10, Ntr::RootTrans,  Ntt::Bypass, true,  28, "Bass",    false },
        { 11, Ntr::RootTrans,  Ntt::Chord,  false, 0,  "Chord1",  false },
        { 12, Ntr::RootTrans,  Ntt::Chord,  false, 0,  "Chord2",  false },
        { 13, Ntr::RootTrans,  Ntt::Chord,  false, 0,  "Pad",     false },
        { 14, Ntr::RootTrans,  Ntt::Melody, false, 0,  "Phrase1", false },
        { 15, Ntr::RootTrans,  Ntt::Melody, false, 0,  "Phrase2", false },
    };

    for (const auto& role : kRoles) {
        auto& cc = cseg.channels[role.ch];
        cc.src_ch = role.ch;
        cc.dst_ch = role.ch;
        cc.part_name = role.part;
        cc.mid = NoteRange{};
        cc.mid.ntr = role.ntr;
        cc.mid.ntt = role.ntt;
        cc.mid.note_low = role.note_low;
        cc.mid.rtr = Rtr::Retrigger;   // Yamaha Ctab default (spec Table 37)
        cc.mid.bass_on = role.bass_on;
        cc.low = cc.mid;
        cc.high = cc.mid;
        cc.is_drums = role.drums;
    }

    casm_.present = true;
    casm_.csegs.push_back(std::move(cseg));
}

// =========================================================
//  Korg PA parser ("G8" / "EV" magic)
//
//  Layout (reverse-engineered):
//    - Header: name 16 chars @2, tempo field BE16(20) = 500000 / BPM,
//      ts num = byte[24], den = 2^byte[25], TPQ fixed 120.
//    - Pointer table @58: 384 LE int32 block offsets (block = table + val).
//      Block i: slot = i % 48, bank = i / 48. A second table (base =
//      LE32(len-4) + 58) exists iff the file trailer starts with
//      "MSE Ver 1". Each slot maps to (section, variant) — see the
//      dispatch tables below (section = the 18 fixed Korg sections).
//    - Blocks decode as 6-byte records: running tick += data[p];
//      type = data[p+1]; notes carry an explicit duration byte data[p+5].
//    - Sections 4..7 (mains) have short sub-patterns tiled to the
//      section length.
//    - Each (section, variant 0/1/2) becomes a PSRstudio section named
//      "<name>", "<name> MIN", "<name> 7TH" — the chord-quality variants.
// =========================================================
bool PrsStyle::parse_korg() {
    const std::size_t n = raw_.size();
    if (n < 58 + 384 * 4) return false;

    format_ = StyleFormat::Korg;
    style_name_ = read_ascii(raw_, 2, 16);

    const u16 tempo_field = static_cast<u16>((raw_[20] << 8) | raw_[21]);
    if (tempo_field == 0) return false;
    tempo_us_ = static_cast<u32>(tempo_field) * 120u;   // = 60000000 / (500000/field)
    ts_num_ = raw_[24] ? raw_[24] : 4;
    ts_den_ = raw_[25] ? static_cast<u8>(1u << raw_[25]) : 4;
    ticks_per_qn_ = 120;
    const u32 ticks_per_measure =
        static_cast<u32>(static_cast<u64>(ticks_per_qn_) * ts_num_ * 4 / ts_den_);

    static const char* kNames[18] = {
        "Intro 1", "Intro 2", "Intro 3", "Intro 4",
        "Main A", "Main B", "Main C", "Main D",
        "Fill In 1D", "Fill In 1U", "Fill In 2D", "Fill In 2U",
        "Fill In 3D", "Fill In 3U",
        "Ending 1", "Ending 2", "Ending 3", "Ending 4",
    };

    struct KRec {
        u32 tick; u8 cmd; u8 bank; u8 b2; u8 b3; u8 b4; u8 variant; u32 seq;
    };
    struct KSec {
        std::vector<KRec> evs;
        u32 measures = 0;
    };
    KSec secs[18];
    u32 sub_len[96] = {};      // (main section - 4) * 24 + bank * 3 + variant
    u32 seq = 0;

    auto add_event = [&](int section, int variant, u32 tick,
                         u8 cmd, u8 bank, u8 b2, u8 b3, u8 b4) {
        secs[section].evs.push_back(
            {tick, cmd, static_cast<u8>(bank), b2, b3, b4,
             static_cast<u8>(variant), seq++});
    };

    // Decode one 6-byte-record block into (section, bank, variant).
    auto decode_block = [&](std::size_t addr, int section, int bank, int variant) {
        u32 tick = 0;
        std::size_t p = addr;
        while (p + 6 <= n) {
            tick += raw_[p];
            const u8 type = raw_[p + 1];
            if (type == 138) { p += 6; continue; }                 // jump
            if (type == 136) {                                     // end of pattern
                const u32 measures = tick / ticks_per_measure;
                if (measures > secs[section].measures) {
                    secs[section].measures = measures;
                }
                if (section >= 4 && section <= 7) {
                    sub_len[(section - 4) * 24 + bank * 3 + variant] = measures;
                }
                break;
            }
            if (type == 143) {                                     // marker + end
                add_event(section, variant, tick, 143,
                          static_cast<u8>(bank), 0, 0, 0);
                const u32 measures = tick / ticks_per_measure;
                if (measures > secs[section].measures) {
                    secs[section].measures = measures;
                }
                if (section >= 4 && section <= 7) {
                    sub_len[(section - 4) * 24 + bank * 3 + variant] = measures;
                }
                break;
            }
            if (type == 229) {                                     // program change
                add_event(section, variant, tick, 12,
                          static_cast<u8>(bank),
                          raw_[p + 5], raw_[p + 3], raw_[p + 4]);
                p += 6; continue;
            }
            if (type == 230) {                                     // control change
                add_event(section, variant, tick, 11,
                          static_cast<u8>(bank),
                          raw_[p + 4], raw_[p + 5], 0);
                p += 6; continue;
            }
            if (type == 234 || type == 235) {                      // unknown meta
                p += 6; continue;
            }
            if (type >= 128) break;                                // unknown -> stop
            if (raw_[p + 3] < 128) {                               // note + duration
                add_event(section, variant, tick, 9,
                          static_cast<u8>(bank),
                          raw_[p + 1], raw_[p + 3], 0);
                add_event(section, variant, tick + raw_[p + 5], 8,
                          static_cast<u8>(bank),
                          raw_[p + 1], 0, 0);
            }
            p += 6;
        }
    };

    // Slot -> (section, variant) dispatch: table 0 (base @58) and table 1.
    auto process_table = [&](std::size_t tbl_base, int tbl_id) {
        for (int i = 0; i < 384; ++i) {
            if (tbl_base + static_cast<std::size_t>(i) * 4 + 4 > n) break;
            u32 off = 0;
            std::memcpy(&off, raw_.data() + tbl_base + static_cast<std::size_t>(i) * 4, 4);
            const std::size_t addr = tbl_base + off;
            if (off == 0 || addr >= n) continue;

            const int slot = i % 48;
            const int bank = i / 48;
            int section = -1, variant = -1;
            if (tbl_id == 0) {
                switch (slot) {
                    case 0: case 1: case 2:  section = 2;  variant = slot - 0;  break;
                    case 3: case 4: case 5:  section = 4;  variant = slot - 3;  break;
                    case 6: case 7: case 8:  section = 6;  variant = slot - 6;  break;
                    case 12: case 13: case 14: section = 8;  variant = slot - 12; break;
                    case 15: case 16: case 17: section = 9;  variant = slot - 15; break;
                    case 21: case 22: case 23: section = 16; variant = slot - 21; break;
                    case 24: case 25: case 26: section = 3;  variant = slot - 24; break;
                    case 27: case 28: case 29: section = 5;  variant = slot - 27; break;
                    case 30: case 31: case 32: section = 7;  variant = slot - 30; break;
                    case 36: case 37: case 38: section = 12; variant = slot - 36; break;
                    case 39: case 40: case 41: section = 13; variant = slot - 39; break;
                    case 45: case 46: case 47: section = 17; variant = slot - 45; break;
                    default: break;
                }
            } else {
                switch (slot) {
                    case 0: case 1: case 2:  section = 0;  variant = slot - 0;  break;
                    case 12: case 13: case 14: section = 10; variant = slot - 12; break;
                    case 15: case 16: case 17: section = 11; variant = slot - 15; break;
                    case 21: case 22: case 23: section = 14; variant = slot - 21; break;
                    case 24: case 25: case 26: section = 1;  variant = slot - 24; break;
                    case 45: case 46: case 47: section = 15; variant = slot - 45; break;
                    default: break;
                }
            }
            if (section < 0) continue;
            decode_block(addr, section, bank, variant);
        }
    };

    process_table(58, 0);
    // Second table present iff the file trailer starts with "MSE Ver 1".
    if (n >= 19 && std::memcmp(raw_.data() + n - 15, "MSE Ver 1", 9) == 0) {
        u32 second = 0;
        std::memcpy(&second, raw_.data() + n - 4, 4);
        process_table(static_cast<std::size_t>(second) + 58, 1);
    }

    // Tiling: mains with short sub-patterns repeat them to the section
    // length. Only (bank, variant) records whose bank/variant match the
    // sub-pattern slot are tiled; already-tiled copies (tick >= sub_ticks)
    // are naturally excluded by the tick filter.
    for (int slot = 0; slot < 96; ++slot) {
        const int section = slot / 24 + 4;
        const int sub = (slot % 24) / 3;
        const int variant = slot % 3;
        const u32 sub_measures = sub_len[slot];
        if (sub_measures == 0 || sub_measures == secs[section].measures) continue;
        const u32 sub_ticks = sub_measures * ticks_per_measure;
        const u32 sec_ticks = secs[section].measures * ticks_per_measure;
        u32 acc_measures = 0;
        while (true) {
            acc_measures += sub_measures;
            if (acc_measures >= secs[section].measures) break;
            const u32 off_ticks = acc_measures * ticks_per_measure;
            const std::size_t snapshot = secs[section].evs.size();
            for (std::size_t i = 0; i < snapshot; ++i) {
                const KRec& r = secs[section].evs[i];
                if (r.tick < sub_ticks && r.tick + off_ticks < sec_ticks &&
                    r.bank == sub && r.variant == variant) {
                    secs[section].evs.push_back(
                        {r.tick + off_ticks, r.cmd, r.bank, r.b2, r.b3, r.b4,
                         r.variant, seq++});
                }
            }
        }
    }

    // Convert to PSRstudio sections/events: one section per
    // (base section, variant) that has events.
    std::vector<std::string> sec_names;
    for (int s = 0; s < 18; ++s) {
        if (secs[s].evs.empty()) continue;
        for (int v = 0; v < 3; ++v) {
            // gather this variant's events
            std::vector<KRec> evs;
            for (const auto& r : secs[s].evs) {
                if (r.variant == v) evs.push_back(r);
            }
            if (evs.empty()) continue;

            std::ranges::sort(evs, {}, [](const KRec& r) {
                return std::pair{r.tick, r.seq};
            });

            std::string name = kNames[s];
            if (v == 1) name += " MIN";
            else if (v == 2) name += " 7TH";

            StyleSection sec;
            sec.name = name;
            sec.quality_variant = static_cast<u8>(v);
            sec.start_tick = 0;
            sec.end_tick = secs[s].measures * ticks_per_measure;
            sec.ev_start = events_.size();

            for (const auto& r : evs) {
                const u8 ch = part_to_channel(
                    [](u8 bank) {
                        // SetInt0: bank -> part
                        constexpr u8 kPart[8] = {0, 2, 3, 4, 5, 6, 7, 1};
                        return (bank < 8) ? kPart[bank] : 0;
                    }(r.bank));
                if (r.cmd == 9) {
                    Event ev;
                    ev.tick = r.tick;
                    ev.kind = EventKind::Short;
                    ev.status = static_cast<u8>(0x90 | ch);
                    ev.d0 = r.b2;           // note
                    ev.d1 = r.b3;           // velocity
                    events_.push_back(ev);
                } else if (r.cmd == 8) {
                    Event ev;
                    ev.tick = r.tick;
                    ev.kind = EventKind::Short;
                    ev.status = static_cast<u8>(0x80 | ch);
                    ev.d0 = r.b2;           // note
                    events_.push_back(ev);
                } else if (r.cmd == 12) {   // PC + bank
                    Event ev;
                    ev.tick = r.tick;
                    ev.kind = EventKind::Short;
                    ev.status = static_cast<u8>(0xB0 | ch);
                    ev.d0 = 0; ev.d1 = r.b2;                 // CC0  = MSB
                    events_.push_back(ev);
                    ev.d0 = 32; ev.d1 = r.b3;                // CC32 = LSB
                    events_.push_back(ev);
                    ev.status = static_cast<u8>(0xC0 | ch);
                    ev.d0 = r.b4;                            // PC
                    events_.push_back(ev);
                } else if (r.cmd == 11) {   // CC
                    Event ev;
                    ev.tick = r.tick;
                    ev.kind = EventKind::Short;
                    ev.status = static_cast<u8>(0xB0 | ch);
                    ev.d0 = r.b2;           // controller
                    ev.d1 = r.b3;           // value
                    events_.push_back(ev);
                }
                // cmd 143: section marker — nothing to play
            }
            sec.ev_end = events_.size();
            if (sec.ev_end == sec.ev_start) continue;
            sections_.push_back(std::move(sec));
            sec_names.push_back(name);
        }
    }

    // Per-section end tick must cover the events (some styles have no
    // explicit length marker for a variant).
    for (auto& sec : sections_) {
        u32 max_tick = 0;
        for (std::size_t i = sec.ev_start; i < sec.ev_end; ++i) {
            if (events_[i].tick > max_tick) max_tick = events_[i].tick;
        }
        const u32 ev_end_measure =
            (max_tick / ticks_per_measure + 1) * ticks_per_measure;
        if (sec.end_tick == 0 || sec.end_tick < ev_end_measure) {
            sec.end_tick = ev_end_measure;
        }
    }

    synthesize_default_casm(sec_names);

    std::cout << std::format("[KORG] '{}'  {} events  {} sections  {} BPM\n",
        style_name_, events_.size(), sections_.size(),
        60000000.0 / static_cast<double>(tempo_us_));
    return true;
}

// =========================================================
//  Ketron KPAT parser — classic layout (header byte[4] == 0)
//
//  Layout (reverse-engineered):
//    - Header: name 8 chars @7, tempo BPM byte[27], ts byte[28]
//      (num = %16, den = /16, 0 -> 4), TPQ fixed 192.
//    - Section lengths in measures: byte[29..37]; byte[38] = RIFF flag.
//    - From bit 824 (MSB-first bit list): 64-bit records:
//        16 ones                      -> section end (tick resets to 0)
//        bit0 == 0                    -> NOTE: part @+1..3 (3b),
//                                        note @+4..10 (7b),
//                                        vel @+11..17 (7b),
//                                        duration @+18..31 (14b signed)
//        110 + track == current       -> tick delta @+3 (14b signed, -2),
//                                        new track @+17 (3b), mask @+20 (12b)
//        110 + track != current       -> resync paths (see code)
//        tail @+32: cls = 1 bit @+32;
//          cls 0: note (7b @+36, 7b @+43) + duration (14b @+50)
//          cls 1: control (2b @+36 -> CC map, 4b @+38, 6b @+42)
//      The 12-bit mask selects which of the 12 sub-slots play the event:
//      slot p -> letter = p / 3 (A..D), variant = p % 3 (MAJ/MIN/7TH).
//      For the classic layout every Fill record is reclassified as an
//      Ending record with the mask rotated left by 3 (observed behavior).
//    - Length records (class 100) carry a mask + length in ticks.
// =========================================================
namespace {

struct KetRecord {
    int type = 0;            // section type 1..4 (after reclassification)
    std::string mask;        // 12 chars '0'/'1'
    int tick = 0;            // absolute ticks within the section
    int cls = 0;             // 0 note, 5 release, 1/2 control, 100 length
    int part = 0;            // 3-bit part index (notes)
    int note = 0;            // b4
    int value = 0;           // byte_2 (velocity / control value)
    int dur = 0;             // num2 (duration for notes)
};

// Left-rotate the 12-char mask by 3 chars.
[[nodiscard]] std::string rotate_mask3(const std::string& m) {
    if (m.size() != 12) return m;
    return m.substr(3) + m.substr(0, 3);
}

} // namespace

bool PrsStyle::parse_ketron() {
    format_ = StyleFormat::Ketron;
    const u8 layout = raw_.size() > 4 ? raw_[4] : 0;
    if (layout != 0) {
        std::cerr << std::format(
            "[KETRON] layout {} not supported (classic layout only)\n",
            static_cast<int>(layout));
        return false;
    }
    if (raw_.size() < 104) return false;

    style_name_ = read_ascii(raw_, 7, 8);
    {
        const int bpm = raw_[27];
        tempo_us_ = (bpm > 0) ? static_cast<u32>(60000000 / bpm) : 500000;
    }
    ts_num_ = static_cast<u8>(raw_[28] % 16);
    ts_den_ = static_cast<u8>(raw_[28] / 16);
    if (ts_num_ == 0) ts_num_ = 4;
    if (ts_den_ == 0) ts_den_ = 4;
    ticks_per_qn_ = 192;
    const u32 ticks_per_measure =
        static_cast<u32>(static_cast<u64>(ticks_per_qn_) * ts_num_ * 4 / ts_den_);

    // Classic-layout length bytes — note the reference hard-zeroes
    // byte_33/36/37 and shifts the ending lengths one byte left:
    //   byte_29 = Main A, byte_30 = Fill In 1, byte_31/32 = Intro 1/2,
    //   byte_33 = 0 (no Intro 3), byte_34 = Ending 1, byte_35 = Ending 2,
    //   byte_36 = 0 (no Ending 3), byte_37 = 0 (no To End), byte_38 = 0.
    const u8 len_main_a = raw_[29];
    const u8 len_fill1  = raw_[30];
    const u8 len_intro1 = raw_[31];
    const u8 len_intro2 = raw_[32];
    const u8 len_intro3 = 0;
    const u8 len_end1   = raw_[33];
    const u8 len_end2   = raw_[34];
    const u8 len_end3   = 0;
    const u8 len_toend  = 0;
    const u8 riff       = 0;

    const auto bits = make_bitlist(raw_);

    // ----- SetByte22 state machine -----
    // State: byte_17 (last track/type marker), byte_16 (current type),
    // bool_4 (section-end seen), int_15 (tick delta of the current record).
    int cursor = 824;                       // bit index
    int byte_17 = 0;
    int byte_16 = 1;
    bool bool_4 = false;
    int int_15 = 0;
    std::string string_1(12, '0');

    u32 abs_tick = 0;                       // accumulated section-relative tick
    bool stop = false;

    struct Key { int type; int letter; int variant; bool operator==(const Key&) const = default; };
    std::vector<KetRecord> records;                 // in emission order
    std::vector<std::pair<Key, u32>> lengths;       // per (type, letter, variant)

    auto emit_record = [&](KetRecord rec) {
        // AddSectionEventData: classic layouts reclassify Fill records as
        // Ending records with the mask rotated (bool_8 == false here).
        if (rec.type == 2 && rec.cls != 100) {
            rec.type = 4;
            rec.mask = rotate_mask3(rec.mask);
        }
        if (rec.cls == 100) {
            // Length record: route by mask to (letter, variant).
            // Mask position p (0-based) maps to slot = 11 - p within the
            // type region; letter = slot / 3, variant = slot % 3. Verified
            // against the reference's own section-name dispatch: MAJ of
            // Main A = position 11, 7TH of Main D = position 0.
            if (rec.type >= 1 && rec.type <= 4) {
                for (int p = 0; p < 12; ++p) {
                    if (rec.mask[p] != '1') continue;
                    const int slot = 11 - p;
                    const int letter = slot / 3, variant = slot % 3;
                    const Key key{rec.type, letter, variant};
                    for (auto& [k, len] : lengths) {
                        if (k == key) {
                            len = std::max(len, static_cast<u32>(rec.tick));
                            goto counted;
                        }
                    }
                    lengths.emplace_back(key, static_cast<u32>(rec.tick));
                counted:;
                }
            }
            return;
        }
        if (rec.cls == 5 || rec.cls == 1 || rec.cls == 2) {
            records.push_back(std::move(rec));   // release / control records ride along
            return;
        }
        if (rec.type < 1 || rec.type > 4) return;  // unknown type (clicks etc.)
        records.push_back(std::move(rec));
    };

    long ctrl_skipped = 0;
    int guard = 0;
    const int guard_limit = 5 * 1024 * 1024;

    while (cursor + 64 <= static_cast<int>(bits.size())) {
        if (++guard > guard_limit) { stop = true; break; }

        // ---- Section end: 16 consecutive ones ----
        if (bits_all_set(bits, static_cast<std::size_t>(cursor), 16)) {
            bool_4 = true;
            int_15 = 0;
            abs_tick = 0;                       // tick resets at section end
            cursor -= 48;
            cursor += 64;                       // net +16
            continue;
        }

        // ---- Resync / dispatch on the first 3 bits ----
        const bool b0 = bits[static_cast<std::size_t>(cursor)];
        const bool b1 = bits[static_cast<std::size_t>(cursor) + 1];
        const bool b2 = bits[static_cast<std::size_t>(cursor) + 2];
        const bool b3 = bits[static_cast<std::size_t>(cursor) + 3];

        if (!b0) {
            // NOTE record fully contained in this window (tail @+32 reads
            // the same window after rewinding 32 bits).
            cursor -= 32;
            goto tail;
        }

        if (bool_4) {
            if (!(b0 && b1 && !b2)) {
                std::cerr << "[KETRON] unexpected record after section end\n";
                goto recover;
            }
            goto tick_header;
        }
        if (b0 && b1 && !b2 &&
            read_bits(bits, static_cast<std::size_t>(cursor) + 17, 3, 0) == byte_17) {
            goto tick_header;
        }
        if (read_bits(bits, static_cast<std::size_t>(cursor) + 1, 3, 0) == byte_17) {
            cursor -= 16;
            goto header;
        }
        cursor -= 32;
        goto tail;

    tick_header:
        int_15 = read_bits(bits, static_cast<std::size_t>(cursor) + 3, 14, 1) - 2;
        goto header;

    header:
        // Read the record header: track/type @+17, 12-bit mask @+20.
        byte_17 = read_bits(bits, static_cast<std::size_t>(cursor) + 17, 3, 0);
        {
            std::string m(12, '0');
            for (int g = 0; g < 4; ++g) {
                for (int k = 0; k < 3; ++k) {
                    m[static_cast<std::size_t>(g * 3 + k)] =
                        bits[static_cast<std::size_t>(cursor) + 20 + g * 3 + k] ? '1' : '0';
                }
            }
            int t = byte_17;
            const std::string first3 = m.substr(0, 3);
            if (t == 2) {
                t = 1;
            } else if (t == 1 && first3 == "100") {
                m[0] = m[1] = m[2] = '0';
                t = 4;
            } else if (t == 1 && first3 == "010") {
                m[0] = m[1] = m[2] = '0';
                t = 3;
            } else if (t == 1 && first3 == "001") {
                m[0] = m[1] = m[2] = '0';
                t = 2;
            } else {
                std::cerr << "[KETRON] unknown section type marker\n";
            }
            byte_16 = t;
            string_1 = m;
        }
        goto tail;

    recover:
        // Error-recovery paths, transcribed from the reference behavior.
        if (b0 && !b1 &&
            read_bits(bits, static_cast<std::size_t>(cursor) + 1, 3, 0) != byte_16) {
            cursor -= 16;
            goto header;
        }
        if (!(b0 && b1 && !b2 && b3)) {
            int_15 = read_bits(bits, static_cast<std::size_t>(cursor) + 3, 14, 1) - 2;
            goto header;
        }
        std::cerr << "[KETRON] resync\n";
        cursor -= 32;
        cursor += 64;
        continue;

    tail: {
        // Record tail @+32 (cursor may have been rewound for bit0==0 notes).
        const int cls = read_bits(bits, static_cast<std::size_t>(cursor) + 32, 1, 0);
        int value3 = 0;
        int b4 = 0;
        int value7 = 0;
        int dur = 0;
        if (cls == 1) {
            value3 = read_bits(bits, static_cast<std::size_t>(cursor) + 36, 2, 0);
            b4 = read_bits(bits, static_cast<std::size_t>(cursor) + 38, 4, 0);
            switch (b4) {
                case 0: b4 = 5;  break;
                case 1: b4 = 0;  break;
                case 2: b4 = 9;  break;
                case 7: b4 = 15;
                        value7 = read_bits(bits, static_cast<std::size_t>(cursor) + 42, 6, 1);
                        break;
                case 8: case 9: case 10: case 11:
                        // control-class switch: re-dispatch as class 2
                        b4 = read_bits(bits, static_cast<std::size_t>(cursor) + 40, 1, 0);
                        value7 = read_bits(bits, static_cast<std::size_t>(cursor) + 41, 7, 0);
                        break;
                case 14: case 15:
                        b4 = 13;
                        value7 = read_bits(bits, static_cast<std::size_t>(cursor) + 41, 7, 0);
                        break;
                case 3: case 4: break;
                default:
                    std::cerr << "[KETRON] unknown control record\n";
                    break;
            }
            cursor -= 16;
        } else {
            value3 = read_bits(bits, static_cast<std::size_t>(cursor) + 33, 3, 0);
            b4 = read_bits(bits, static_cast<std::size_t>(cursor) + 36, 7, 0);
            value7 = read_bits(bits, static_cast<std::size_t>(cursor) + 43, 7, 0);
            dur = read_bits(bits, static_cast<std::size_t>(cursor) + 50, 14, 2);
        }

        // Build the record and route it.
        KetRecord rec;
        rec.type = byte_16;
        rec.mask = string_1;
        rec.tick = static_cast<int>(abs_tick) + int_15;
        rec.cls = cls;
        rec.part = value3;
        rec.note = b4;
        rec.value = value7;
        rec.dur = dur;

        if (cls == 0) {
            abs_tick = static_cast<u32>(rec.tick);
            // note-on + paired release at tick + duration
            KetRecord rel = rec;
            rel.cls = 5;
            rel.tick = rec.tick + std::max(0, dur);
            emit_record(rec);
            emit_record(std::move(rel));
            bool_4 = false;
        } else if (cls == 5) {
            // standalone release (from a previous window pair) — routed only;
            // does not advance the tick chain (its tick is a note duration)
            emit_record(std::move(rec));
            bool_4 = false;
        } else if (cls == 1 || cls == 2) {
            // control records: skipped from output, but they still carry a
            // tick delta — advance the chain so later notes don't drift.
            ++ctrl_skipped;
            abs_tick = static_cast<u32>(rec.tick);
            bool_4 = false;
        } else if (cls == 100) {
            abs_tick = static_cast<u32>(rec.tick);
            emit_record(std::move(rec));
            bool_4 = false;
        } else {
            std::cerr << "[KETRON] unknown record class\n";
            bool_4 = false;
        }
        cursor += 64;
        continue;
    }

    (void)stop;
    (void)ctrl_skipped;
    }

    // ----- Build sections from the routed records -----
    static const char* kMainNames[4]  = { "Main A", "Main B", "Main C", "Main D" };
    static const char* kFillNames[4]  = { "Fill In 1", "Fill In 2", "Fill In 3", "Fill In Break" };
    static const char* kIntroNames[4] = { "Intro 1", "Intro 2", "Intro 3", "Intro 4" };
    static const char* kEndNames[4]   = { "Ending 1", "Ending 2", "Ending 3", "Ending To End" };

    // Collect records per (type, letter, variant).
    struct VSec { std::vector<KetRecord> recs; };
    VSec vs[4][4][3] = {};   // [type-1][letter][variant]
    for (const auto& r : records) {
        if (r.cls == 1 || r.cls == 2) continue;
        if (r.type < 1 || r.type > 4) continue;
        for (int p = 0; p < 12; ++p) {
            if (r.mask[p] != '1') continue;
            // Mask position p -> slot 11 - p -> letter = slot/3,
            // variant = slot%3 (MAJ/MIN/7TH). Verified against the
            // reference's section-name dispatch (A-MAJ = pos 11,
            // D-7TH = pos 0).
            const int slot = 11 - p;
            const int letter = slot / 3;
            const int variant = slot % 3;
            vs[r.type - 1][letter][variant].recs.push_back(r);
        }
    }

    // Section lengths from the length records (cls == 100), by mask.
    u32 sec_len[4][4][3] = {};
    for (const auto& [key, len] : lengths) {
        sec_len[key.type - 1][key.letter][key.variant] =
            std::max(sec_len[key.type - 1][key.letter][key.variant], len);
    }

    std::vector<std::string> sec_names;
    const char* type_names[4][4] = {
        { "Main A", "Main B", "Main C", "Main D" },
        { "Fill In 1", "Fill In 2", "Fill In 3", "Fill In Break" },
        { "Intro 1", "Intro 2", "Intro 3", "Intro 4" },
        { "Ending 1", "Ending 2", "Ending 3", "Ending To End" },
    };

    for (int t = 0; t < 4; ++t) {
        for (int letter = 0; letter < 4; ++letter) {
            for (int variant = 0; variant < 3; ++variant) {
                auto& vsr = vs[t][letter][variant].recs;
                if (vsr.empty()) continue;

                std::string name = type_names[t][letter];
                if (riff != 0 && t == 0 && letter == 3) name = "Main RIFF";
                if (variant == 1) name += " MIN";
                else if (variant == 2) name += " 7TH";

                std::ranges::sort(vsr, {}, [](const KetRecord& r) {
                    return std::pair{r.tick, r.note};
                });

                StyleSection sec;
                sec.name = name;
                sec.quality_variant = static_cast<u8>(variant);
                sec.start_tick = 0;
                sec.end_tick = sec_len[t][letter][variant];
                sec.ev_start = events_.size();

                for (const auto& r : vsr) {
                    const u8 ch = [](int part) {
                        // SetString02 (classic layout): part -> source channel
                        constexpr u8 kCh[8] = {4, 5, 6, 7, 9, 10, 11, 8};
                        return (part >= 0 && part < 8) ? kCh[part] : 4;
                    }(r.part);
                    if (r.cls == 0) {
                        Event ev;
                        ev.tick = static_cast<u32>(r.tick);
                        ev.kind = EventKind::Short;
                        ev.status = static_cast<u8>(0x90 | ch);
                        ev.d0 = static_cast<u8>(r.note);
                        ev.d1 = static_cast<u8>(std::clamp(r.value, 1, 127));
                        events_.push_back(ev);
                        Event off;
                        off.tick = static_cast<u32>(r.tick + std::max(0, r.dur));
                        off.kind = EventKind::Short;
                        off.status = static_cast<u8>(0x80 | ch);
                        off.d0 = static_cast<u8>(r.note);
                        events_.push_back(off);
                    } else if (r.cls == 5) {
                        Event off;
                        off.tick = static_cast<u32>(r.tick);
                        off.kind = EventKind::Short;
                        off.status = static_cast<u8>(0x80 | ch);
                        off.d0 = static_cast<u8>(r.note);
                        events_.push_back(off);
                    }
                    // control records (cls 1/2) are skipped — see header
                }
                sec.ev_end = events_.size();
                if (sec.ev_end == sec.ev_start) continue;

                // Cover the events if no length record matched.
                u32 max_tick = 0;
                for (std::size_t i = sec.ev_start; i < sec.ev_end; ++i) {
                    if (events_[i].tick > max_tick) max_tick = events_[i].tick;
                }
                const u32 ev_end_measure =
                    (max_tick / ticks_per_measure + 1) * ticks_per_measure;
                if (sec.end_tick == 0 || sec.end_tick < ev_end_measure) {
                    sec.end_tick = ev_end_measure;
                }

                sections_.push_back(std::move(sec));
                sec_names.push_back(name);
            }
        }
    }

    // Synthetic CASM: Ketron classic accompaniment channels.
    {
        CasmCseg cseg;
        cseg.sec_names = sec_names;
        cseg.channels.assign(16, CasmChannel{});
        struct Role { u8 ch; Ntr ntr; Ntt ntt; bool bass_on; u8 note_low;
                      const char* part; bool drums; };
        static const Role kRoles[] = {
            { 9,  Ntr::RootFixed,  Ntt::Bypass, false, 0,  "Drums", true  },
            { 10, Ntr::RootTrans,  Ntt::Bypass, true,  28, "Bass",  false },
            { 4,  Ntr::RootTrans,  Ntt::Chord,  false, 0,  "Chord1", false },
            { 5,  Ntr::RootTrans,  Ntt::Chord,  false, 0,  "Chord2", false },
            { 6,  Ntr::RootTrans,  Ntt::Chord,  false, 0,  "Pad",   false },
            { 7,  Ntr::RootTrans,  Ntt::Melody, false, 0,  "Phrase1", false },
            { 8,  Ntr::RootTrans,  Ntt::Melody, false, 0,  "Phrase2", false },
            { 11, Ntr::RootTrans,  Ntt::Melody, false, 0,  "Phrase3", false },
        };
        for (const auto& role : kRoles) {
            auto& cc = cseg.channels[role.ch];
            cc.src_ch = role.ch;
            cc.dst_ch = role.ch;
            cc.part_name = role.part;
            cc.mid = NoteRange{};
            cc.mid.ntr = role.ntr;
            cc.mid.ntt = role.ntt;
            cc.mid.note_low = role.note_low;
            cc.mid.rtr = Rtr::Retrigger;
            cc.mid.bass_on = role.bass_on;
            cc.low = cc.mid;
            cc.high = cc.mid;
            cc.is_drums = role.drums;
        }
        casm_.present = true;
        casm_.csegs.push_back(std::move(cseg));
    }

    std::cout << std::format("[KETRON] '{}'  {} events  {} sections  {} BPM\n",
        style_name_, events_.size(), sections_.size(),
        60000000.0 / static_cast<double>(tempo_us_));
    return true;
}

} // namespace prs
