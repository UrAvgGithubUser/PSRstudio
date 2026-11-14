// style.hpp — PRS style file parsing, fire-time cache, voice setup, CASM.
//
// Parses Yamaha PRS / SFF2 style files into an in-memory PrsStyle object.
// Provides:
//   - ByteReader: bounds-checked sequential byte reader (BE + VLQ)
//   - PrsStyle: owns events, SysEx pool, sections, CASM, fire-time cache
//   - MidiSink: abstract interface for MIDI output (implemented by
//     WinMidiOutput and FluidSynthOutput)
//   - RemapSource: abstract interface for channel/voice remapping
//   - ChannelSetup / CasmChannel / NoteRange / ChVoice: style data structs
//
// Fire times are stored as std::chrono::nanoseconds for portable timing.
// Multiple PrsStyle instances can coexist (all state is per-instance).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prs {

// =========================================================
//  Type aliases
// =========================================================
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

// =========================================================
//  Enums  (scoped, replaces the C macros in types.h/casm.h)
//
//  Values match casm.h exactly — transpose.c and player.c
//  depend on these numeric values for correct transposition.
// =========================================================
enum class EventKind : u8 { Tempo, SysEx, Short };

// NTR — Note Transposition Rule
enum class Ntr : u8 {
    RootTrans  = 0,   // shift root with chord root change
    RootFixed  = 1,   // no root shift (drums, fixed pitch)
    Guitar     = 2,   // guitar voicing mode (Ctb2 only)
};

// NTT — Note Transposition Table
// Bit 7 of the raw byte is bass_on — extracted into NoteRange.bass_on.
// Note: Guitar NTT values (0x00-0x02) overlap with Bypass/Melody/Chord.
// They're only valid when NTR == Guitar. We use the same enum values
// and disambiguate by checking NTR in the transposition code.
enum class Ntt : u8 {
    Bypass           = 0x00,
    Melody           = 0x01,
    Chord            = 0x02,
    MelodicMinor     = 0x03,
    MelodicMinor5    = 0x04,
    HarmonicMinor    = 0x05,
    HarmonicMinor5   = 0x06,
    NaturalMinor     = 0x07,
    NaturalMinor5    = 0x08,
    Dorian           = 0x09,
    Dorian5          = 0x0A,
    // Guitar NTT aliases (same numeric values as Bypass/Melody/Chord):
    //   Ntr::Guitar + Ntt::Bypass  = GuitarAll
    //   Ntr::Guitar + Ntt::Melody  = GuitarStroke
    //   Ntr::Guitar + Ntt::Chord   = GuitarArpeggio
};

// RTR — Retrigger Rule
enum class Rtr : u8 {
    Stop            = 0,
    PitchShift      = 1,
    PitchShiftRoot  = 2,
    Retrigger       = 3,
    RetriggerRoot   = 4,
    NoteGenerator   = 5,
};

// =========================================================
//  Data structures
// =========================================================
struct NoteRange {
    Ntr  ntr       = Ntr::RootTrans;
    Ntt  ntt       = Ntt::Bypass;
    u8   high_key  = 0;
    u8   note_low  = 0;
    u8   note_high = 127;
    Rtr  rtr       = Rtr::Stop;
    bool bass_on   = false;
};

// =========================================================
//  SysEx decoder
//
//  Yamaha XG style files use a small vocabulary of SysEx
//  messages. We decode the well-known ones into a Kind enum
//  and leave the rest as Unknown with the raw bytes intact.
// =========================================================
namespace sysex {

enum class Kind : u8 {
    GM_System_On,         // 7E 7F 09 01
    GM_Master_Volume,     // 7F 7F 04 01 ll hh
    XG_System_On,         // 43 10 4C 00 00 7E 00
    XG_Master_Volume,     // 43 10 4C 00 00 7F xx
    XG_Master_Tune,       // 43 10 4C 00 00 06 ...
    XG_Reverb_Type,       // 43 10 4C 02 01 00 xx
    XG_Reverb_Param,      // 43 10 4C 02 01 <p> xx
    XG_Chorus_Type,       // 43 10 4C 02 02 00 xx
    XG_Chorus_Param,      // 43 10 4C 02 02 <p> xx
    XG_Variation_Type,    // 43 10 4C 02 03 00 xx
    XG_Variation_Param,   // 43 10 4C 02 03 <p> xx
    XG_Insertion_Type,    // 43 10 4C 02 04 00 xx
    XG_Multi_EQ,          // 43 10 4C 03 <band> <param> <value...>
    XG_Multi_Part_Param,  // 43 10 4C 08 <part> <param> <value>
    XG_Drum_Setup,        // 43 10 4C 0A <part> <key> <param> <value>
    XG_Drum_Param,        // 43 10 4C 0D <part> <param> <value>
    Yamaha_Song_Setup,    // 43 76 1A ...
    Yamaha_Style_Setup,   // 43 73 39 ...
    Yamaha_OTS_Setup,     // 43 73 01 51 ...
    Unknown_Yamaha,       // 43 ...
    Unknown_Other,        // anything else
};

[[nodiscard]] std::string_view kind_name(Kind k) noexcept;

// Reverb/Chorus type name tables (XG spec).
[[nodiscard]] std::string_view reverb_type_name(u8 type) noexcept;
[[nodiscard]] std::string_view chorus_type_name(u8 type)  noexcept;
[[nodiscard]] std::string_view part_param_name(u8 param)  noexcept;

struct Message {
    Kind             kind = Kind::Unknown_Other;
    std::vector<u8>  raw;             // full payload (after F0, before F7)
    std::string      description;     // human-readable summary
    // Decoded address fields for XG Multi_Part_Param / Drum_Setup
    u8 part  = 0;
    u8 param = 0;
    u8 value = 0;
    u8 key   = 0;     // for Drum_Setup only
};

// Decode a single SysEx payload (the bytes between F0 and F7).
[[nodiscard]] Message decode(std::span<const u8> payload) noexcept;

// Walk a raw byte stream (hdr_sx_ style: F0 ... F7 F0 ... F7 ...)
// and decode each SysEx message. Skips any stray bytes between messages.
[[nodiscard]] std::vector<Message>
    decode_stream(std::span<const u8> stream) noexcept;

} // namespace sysex

// =========================================================
//  OTS (One Touch Setting) data structures
//
//  Correct structure (verified on real .prs/.sty files):
//
//    OTSc chunk
//      [SFF2] OTSi sub-chunk (8-byte header)
//      MTrk #1  →  OTS button 1  (4 channels: R1, R2, R3, Left)
//      MTrk #2  →  OTS button 2  (4 channels: R1, R2, R3, Left)
//      MTrk #3  →  OTS button 3  (4 channels: R1, R2, R3, Left)
//      MTrk #4  →  OTS button 4  (4 channels: R1, R2, R3, Left)
//
//  Each MTrk contains:
//    - XG SysEx setup (reverb/chorus type, master volume, per-part params)
//    - CC/PC events on channels 0..3, where:
//        ch 0 = Right 1 (R1)  — main melody voice
//        ch 1 = Right 2 (R2)  — layer voice
//        ch 2 = Right 3 (R3)  — extra layer voice
//        ch 3 = Left          — left-hand voice
// =========================================================

// One voice setup for one keyboard part within one OTS button.
struct OtsPanel {
    u8   channel     = 0;     // 0=R1, 1=R2, 2=R3, 3=Left
    u8   bank_msb    = 0;     // CC0
    u8   bank_lsb    = 0;     // CC32
    u8   pc          = 0;     // Program Change (1-based)
    u8   volume      = 100;   // CC7
    u8   pan         = 64;    // CC10
    u8   expression  = 127;   // CC11
    u8   reverb      = 0;     // CC91
    u8   chorus      = 0;     // CC93
    u8   variation   = 0;     // CC94
    u8   filter_res  = 64;    // CC71
    u8   release     = 64;    // CC72
    u8   attack      = 64;    // CC73
    u8   brightness  = 64;    // CC74
    u8   portamento  = 0;     // CC65
    u8   portamento_time = 0; // CC5
    bool valid       = false; // set true when a PC event is seen
    // Raw CC list (controller -> last value seen)
    std::vector<std::pair<u8, u8>> raw_ccs;
};

// One OTS button — 4 keyboard parts (R1/R2/R3/Left).
struct OtsButton {
    u8                          number      = 0;   // 1..4
    std::array<OtsPanel, 4>     parts{};           // indexed by channel 0..3
    std::size_t                 sysex_count = 0;
    std::vector<sysex::Message> decoded_sysex;
};

// Complete OTS block from an OTSc chunk.
struct OtsBlock {
    bool                       present      = false;
    bool                       is_sff2      = false;   // true if OTSi header present
    u16                        panel_count  = 0;       // from OTSi header
    std::array<u8, 8>          otsi_header  = {};      // raw OTSi bytes
    std::vector<OtsButton>     buttons;                // typically 4
    std::size_t                total_sysex  = 0;       // sum across all buttons
};

struct CasmChannel {
    u8          src_ch          = 0;
    u8          dst_ch          = 0;
    std::string part_name;          // up to 8 chars, space-padded in source
    u64         chord_mask       = 0;  // 40-bit chord mute mask (raw, see chord_mute_allowed)
    u8          src_root         = 0;
    u8          src_chord_type   = 0;

    // SFF2 three-range support (set middle_low=0 and middle_high=127 for SFF1).
    // The old fields mid_low_pitch / mid_high_pitch / low / mid / high are
    // kept for backward compatibility with the existing CASM parser and
    // player code; the new arranger module accesses them via the new
    // accessor methods below.
    u8          mid_low_pitch    = 0;       // alias for ctb2_middle_low_pitch
    u8          mid_high_pitch   = 127;     // alias for ctb2_middle_high_pitch
    NoteRange   low{}, mid{}, high{};       // ctb2_low / ctb2_main / ctb2_high

    bool        is_drums         = false;

    // Note mute: 2 bytes (bytes 11-12 of Ctab data).
    // Bit=1 → note allowed, Bit=0 → note muted.
    // Byte 0 (data[11]): bits 3-0 = B, Bb, A, G#
    // Byte 1 (data[12]): bits 7-0 = G, F#, F, E, Eb, D, C#, C
    // 0xFFFF = all notes allowed (default)
    u16         note_mute_mask   = 0xFFFF;

    // Chord mute: true = this channel plays for ALL chord types.
    // false = chord_mask must be checked per chord type.
    // When all 36 bits are set, this is true (optimization).
    bool        chord_mute_all   = true;

    // §5.2.9 Special features (Ctab only): byte 26 = feature id
    // (01H = "extra break drum voice"), byte 29 = instrument
    // (31H/32H = Crash Cymbal 1/2), byte 30 = volume 0-127.
    // Parsed for completeness; such channels carry NO notes, so they are
    // inert during playback (the break-voice injection is not implemented).
    u8          special_feature  = 0;
    u8          special_instr    = 0;
    u8          special_volume   = 0;

    // ----- SFF2 three-range accessor methods (used by arranger module) -----

    // Returns true if this channel uses a single ctb2 range (SFF1 or
    // unsplit SFF2). In this case, all pitches use the mid (main) range.
    [[nodiscard]] bool is_single_ctb2() const noexcept {
        return mid_low_pitch == 0 && mid_high_pitch == 127;
    }

    // Pick the right NoteRange for a given source pitch.
    [[nodiscard]] const NoteRange& range_for_pitch(int pitch) const noexcept {
        if (is_single_ctb2()) return mid;
        if (pitch < static_cast<int>(mid_low_pitch))  return low;
        if (pitch > static_cast<int>(mid_high_pitch)) return high;
        return mid;
    }

    // Check if a pitch class is muted for this channel.
    // The note_mute_mask is a 16-bit value where each bit maps to a pitch
    // class: bit 15 = B, bit 14 = Bb, ... bit 4 = C, bits 3-0 = unused.
    [[nodiscard]] bool is_pitch_class_muted(int pc) const noexcept {
        if (note_mute_mask == 0xFFFF) return false;  // fast path: all allowed
        if (pc < 0 || pc > 11) return false;
        const int bit = 15 - pc;
        if (bit < 0) return false;
        return !((note_mute_mask >> bit) & 1);
    }

    // Check if a chord type (CASM index 0-34) is muted for this channel.
    [[nodiscard]] bool is_chord_type_muted(int casm_type) const noexcept {
        if (chord_mute_all) return false;  // fast path: all allowed
        if (casm_type < 0 || casm_type > 34) return false;
        // chord_mask is a 40-bit mask; bit casm_type = allowed.
        return !((chord_mask >> casm_type) & 1);
    }
};

struct CasmCseg {
    std::vector<std::string>  sec_names;
    std::vector<CasmChannel>  channels;
};

struct CasmBlock {
    bool                       present = false;
    std::vector<CasmCseg>      csegs;
};

struct Event {
    u32        tick       = 0;
    EventKind  kind       = EventKind::Short;
    u8         status     = 0;       // for EV_SHORT: full status byte
    u8         d0         = 0;       // EV_SHORT: data1 / EV_TEMPO: msb
    u8         d1         = 0;       // EV_SHORT: data2 / EV_TEMPO: mid
    u32        sx_offset  = 0;       // EV_SYSEX: offset into sx_pool
    u16        sx_len     = 0;       // EV_SYSEX: payload length
};

struct StyleSection {
    std::string  name;
    u32          start_tick = 0;
    u32          end_tick   = 0xFFFF'FFFFu;
    std::size_t  ev_start   = 0;
    std::size_t  ev_end     = 0;
    std::chrono::nanoseconds dur{};
    // Ketron/Korg chord-quality variant: 0 = MAJ (plain name),
    // 1 = MIN ("<name> MIN"), 2 = 7TH ("<name> 7TH"). Yamaha styles
    // always use 0.
    u8           quality_variant = 0;
};

// =========================================================
//  StyleFormat — which native format the style was parsed from.
//  Ketron (KPAT) and Korg PA styles are converted into the same
//  internal sections/events model at parse time.
// =========================================================
enum class StyleFormat : u8 {
    Yamaha,
    Ketron,
    Korg,
};

struct ChVoice {
    bool valid      = false;
    u8   bank_msb   = 0;
    u8   bank_lsb   = 0;
    u8   pc         = 0;
    u8   volume     = 100;
    u8   pan        = 64;
    u8   expression = 127;
    u8   filter_res = 64;
    u8   release    = 64;
    u8   attack     = 64;
    u8   brightness = 64;
    u8   reverb     = 0;
    u8   chorus     = 0;
    u8   variation  = 0;
};

// =========================================================
//  ChannelSetup — the complete startup state of ONE OUTPUT channel
//  as derived from a style (after voice remap and channel routing).
//
//  Produced by PrsStyle::build_channel_setup() and consumed by
//  chmodel::ChannelModel::apply_setup(). Unlike ChVoice, every value
//  here is exactly what goes on the wire:
//    - ch      : OUTPUT channel 0-15 (CASM / Montage routing applied)
//    - program : 0-127 wire value (NOT the 1-based internal form)
// =========================================================
struct ChannelSetup {
    u8 ch         = 0;
    u8 msb        = 0;
    u8 lsb        = 0;
    u8 program    = 0;
    u8 volume     = 100;
    u8 pan        = 64;
    u8 expression = 127;
    u8 reverb     = 0;
    u8 chorus     = 0;
    u8 variation  = 0;
    u8 filter_res = 64;
    u8 release    = 64;
    u8 attack     = 64;
    u8 brightness = 64;
};

// =========================================================
//  External service interfaces
//  (replaces midi.h and remap.h hard dependencies)
// =========================================================

// Abstract MIDI output sink. Implementations send bytes to a real device.
class MidiSink {
public:
    virtual ~MidiSink() = default;
    virtual void sysex_nowait(std::span<const u8> data) = 0;
    virtual void short_direct(u8 status, u8 d0, u8 d1)   = 0;
    virtual void sysex_flush()                           = 0;
    virtual void sleep_ms(unsigned ms)                   = 0;

    // Queued short message (goes through ring buffer, thread-safe).
    // Default: calls short_direct (bypass ring). Override in WinMidiOutput
    // for proper ring-buffer queuing.
    virtual void short_msg(u8 status, u8 d0, u8 d1) {
        short_direct(status, d0, d1);
    }

    // Queued sysex (goes through ring buffer, thread-safe).
    // Default: calls sysex_nowait. Override in WinMidiOutput for proper
    // ring-buffer queuing.
    virtual void sysex(std::span<const u8> payload) {
        sysex_nowait(payload);
    }

    // All notes off (panic). Default: sends CC 123 on all 16 channels.
    virtual void all_notes_off();
};

// Abstract voice/bank remap source.
struct RemapEntry {
    u8          dst_msb;
    u8          dst_lsb;
    u8          dst_pc;
    std::string desc;
};

class RemapSource {
public:
    virtual ~RemapSource() = default;
    [[nodiscard]] virtual std::optional<RemapEntry>
        find(u8 msb, u8 lsb, u8 pc) const = 0;
    [[nodiscard]] virtual bool
        apply(u8 msb, u8 lsb, u8 pc, u8& dm, u8& dl, u8& dp) const = 0;

    // ----- MONTAGE / destination-synth profile queries -----
    // Default implementations return "MONTAGE disabled" so non-MONTAGE
    // RemapSources (e.g., IdentityRemap in tests) keep working
    // unchanged. VoiceRemap overrides these with its profile config.

    // Master MONTAGE switch. When false, all queries below return
    // their defaults (no channel remap, no CC redirect).
    [[nodiscard]] virtual bool montage_enabled() const noexcept { return false; }

    // Output-channel routing for STYLE playback.
    //
    //   src_ch      — the style's SOURCE channel (0-15, as in the .sty).
    //   casm_dst_ch — the destination channel the CASM table assigns to
    //                 that source channel (equals src_ch when the style
    //                 has no CASM entry for it).
    //
    // Default: follow CASM (return casm_dst_ch). Always returns 0-15.
    [[nodiscard]] virtual u8 route_channel(u8 src_ch, u8 casm_dst_ch) const noexcept {
        (void)src_ch;
        return static_cast<u8>(casm_dst_ch & 0x0F);
    }

    // Should CC#93 (Chorus) be redirected to CC#94 (Variation) for
    // this destination synth?
    [[nodiscard]] virtual bool redirect_chorus_to_variation() const noexcept {
        return false;
    }
};

// =========================================================
//  ByteReader — a tiny sequential big-endian reader.
//  Used internally; exposed in the header for unit tests.
// =========================================================
class ByteReader {
public:
    explicit constexpr ByteReader(std::span<const u8> data) noexcept
        : data_(data) {}

    [[nodiscard]] constexpr std::size_t position()  const noexcept { return pos_; }
    [[nodiscard]] constexpr std::size_t size()      const noexcept { return data_.size(); }
    [[nodiscard]] constexpr std::size_t remaining() const noexcept { return data_.size() - pos_; }
    [[nodiscard]] constexpr bool        eof()       const noexcept { return pos_ >= data_.size(); }

    constexpr void seek(std::size_t p) noexcept { pos_ = p; }

    [[nodiscard]] constexpr bool can_read(std::size_t n) const noexcept {
        return pos_ + n <= data_.size();
    }

    [[nodiscard]] constexpr u8 peek() const noexcept { return data_[pos_]; }

    [[nodiscard]] constexpr u8 read_u8() noexcept {
        return data_[pos_++];
    }

    [[nodiscard]] constexpr u16 read_be16() noexcept {
        const u16 v = static_cast<u16>(
            (static_cast<u16>(data_[pos_])     << 8) |
             static_cast<u16>(data_[pos_ + 1]));
        pos_ += 2;
        return v;
    }

    [[nodiscard]] constexpr u32 read_be32() noexcept {
        const u32 v = (static_cast<u32>(data_[pos_])     << 24) |
                      (static_cast<u32>(data_[pos_ + 1]) << 16) |
                      (static_cast<u32>(data_[pos_ + 2]) <<  8) |
                       static_cast<u32>(data_[pos_ + 3]);
        pos_ += 4;
        return v;
    }

    // Read a MIDI variable-length quantity.
    [[nodiscard]] constexpr u32 read_vlq() noexcept {
        u32 v = 0;
        while (pos_ < data_.size()) {
            const u8 b = data_[pos_++];
            v = (v << 7) | static_cast<u32>(b & 0x7F);
            if (!(b & 0x80)) [[likely]] break;
        }
        return v;
    }

    [[nodiscard]] constexpr std::span<const u8> read_bytes(std::size_t n) noexcept {
        const auto s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
    }

    [[nodiscard]] constexpr std::span<const u8> subspan(std::size_t off,
                                                        std::size_t n) const noexcept {
        return data_.subspan(off, n);
    }

private:
    std::span<const u8> data_;
    std::size_t         pos_ = 0;
};

// =========================================================
//  fnv1a 32-bit hash (constexpr-friendly)
// =========================================================
[[nodiscard]] constexpr u32 fnv1a(std::span<const u8> data) noexcept {
    u32 h = 2166136261u;
    for (const auto b : data) {
        h ^= b;
        h *= 16777619u;
    }
    return h;
}

// =========================================================
//  Program Change convention
//
//  On the wire a Program Change value is ALWAYS 0-127.
//  Internally, ChVoice::pc and the remap tables (RemapSource::apply)
//  store it 1-BASED (1-128), because that is how voice lists and the
//  .remap files are written. The ONLY place the two meet is here:
//  convert 1-based -> wire value with pc_to_wire() right before
//  sending, and never send (pc - 1) by hand. pc_to_wire() can never
//  underflow (0 -> 0) or exceed 127.
// =========================================================
[[nodiscard]] constexpr u8 pc_to_wire(u8 pc_one_based) noexcept {
    return pc_one_based == 0
        ? u8{0}
        : static_cast<u8>(std::min<int>(static_cast<int>(pc_one_based) - 1, 127));
}

// =========================================================
//  Case-insensitive string helpers
// =========================================================
[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;
// Case-fold, strip spaces. Used by find_section to be tolerant of
// "Main A" vs "MainA" vs "maina" variants.
[[nodiscard]] std::string normalize_name(std::string_view s);

// =========================================================
//  CASM chord-type name table (35 entries, indexed by byte 0x00..0x22)
// =========================================================
extern const std::array<std::string_view, 35> kChordTypeNames;
extern const std::array<std::string_view, 12> kNoteNames;

// =========================================================
//  Chord mute bit-position lookup table
//
//  Maps CASM chord type index (0..34) to bit position in the
//  40-bit chord_mask stored in CasmChannel.
//
//  The 5 raw bytes (13-17) are packed into chord_mask as:
//    bits 39-36 = byte 13 low nibble (sus4, 1+2+5, autostart)
//    bits 35-28 = byte 14 (1+5, 1+8, 7aug, Maj7aug, 7#9, 7b13, 7b9, 7(13))
//    bits 27-20 = byte 15 (7#11, 7(9), 7b5, 7sus4, 7th, dim7, dim, minMaj7(9))
//    bits 19-12 = byte 16 (minMaj7, min7(11), min7(9), min(9), m7b5, min7, min6, min)
//    bits 11-4  = byte 17 (aug, Maj6(9), Maj7(9), Maj(9), Maj7#11, Maj7, Maj6, Maj)
//    bits 3-0   = unused (always 0)
//
//  Given a CASM chord type index, this table gives the bit position
//  in chord_mask. Bit=1 → play, Bit=0 → mute.
// =========================================================
extern const std::array<int, 35> kChordMuteBitPos;

// =========================================================
//  PrsStyle — the main parser/holder class.
// =========================================================
class PrsStyle {
public:
    // ----- Parsing -----
    // Returns false on I/O error or invalid file. Diagnostics go to stderr.
    // Dispatches on the file magic: MThd (Yamaha SFF), KPAT (Ketron,
    // classic layout), G8/EV (Korg PA). Files protected with the
    // vArranger-style obfuscation trailer are decoded first.
    [[nodiscard]] bool parse(const std::filesystem::path& path);

    // Which native format this style was parsed from.
    [[nodiscard]] StyleFormat format() const noexcept { return format_; }

    // ----- Section helpers (replaces free functions in style.h) -----
    [[nodiscard]] std::optional<std::size_t>
        find_section(std::string_view name) const;

    // Returns the index of "Main A" or the first section containing "main".
    // Falls back to 0.
    [[nodiscard]] std::size_t find_main_A() const;

    // Given "Main A" returns the index of "Fill In AA", or std::nullopt.
    [[nodiscard]] std::optional<std::size_t>
        find_corresponding_fill(std::string_view main_name) const;

    // ----- Section predicates (replaces section_is_*) -----
    // Returns true if name is Intro / Fill / Ending. Sets auto_stop=true
    // for Ending.
    [[nodiscard]] static bool section_is_loop_only(std::string_view name,
                                                   bool& auto_stop);
    [[nodiscard]] static bool section_is_fillin(std::string_view name);
    [[nodiscard]] static bool section_is_ending(std::string_view name);

    // ----- CASM lookup API (replaces casm_get_channel / casm_get_range) -----
    [[nodiscard]] const CasmChannel*
        casm_get_channel(std::string_view section_name, u8 src_ch) const;

    // Never returns nullptr — falls back to mid range.
    [[nodiscard]] static const NoteRange*
        casm_get_range(const CasmChannel& cc, u8 pitch);

    [[nodiscard]] bool casm_is_drums(std::string_view section_name,
                                     u8 src_ch) const;

    // Check if a note should be muted based on the note mute mask.
    // pitch: the SOURCE MIDI pitch (before transposition).
    // Returns true if the note is allowed (not muted).
    [[nodiscard]] static bool casm_is_note_allowed(const CasmChannel& cc, u8 pitch) noexcept;

    // Check if a channel should play for the current chord type.
    // casm_chord_type: CASM index (0..34) of the detected chord.
    // Returns true if the channel is allowed to play.
    [[nodiscard]] static bool casm_is_chord_allowed(const CasmChannel& cc,
                                                     int casm_chord_type) noexcept;

    void casm_dump() const;

    // ----- OTS (One Touch Setting) -----
    void ots_dump() const;

    // ----- SysEx -----
    // Dump decoded SysEx messages from the header (pre-SInt) stream.
    void sysex_dump() const;

    // ----- Fire-time cache -----
    // Loads from disk cache if available, otherwise computes and saves.
    void cache_all_sections();

    // ----- Synth / style setup -----
    //
    // NOTE: PSRstudio keeps the authoritative state of all 16 output
    // channels in chmodel::ChannelModel. A style does not push its
    // voices straight to the device; instead:
    //   1. build_channel_setup() computes the style's per-channel state
    //   2. ChannelModel::apply_setup() stores it AND syncs the device
    // so the model and the external synth can never disagree.

    // One-time synth reset. Called ONCE per connection (opening a MIDI
    // port or starting FluidSynth), BEFORE the channel model is synced
    // to the device. Sends:
    //   1. XG System On SysEx
    //   2. Per-channel CC reset block (50 CCs x 16 channels)
    //   3. Universal master-volume SysEx
    // Static: it does not depend on any style.
    static void send_synth_init(MidiSink& midi);

    // Replay the style's header (pre-SInt) SysEx stream — XG effect
    // types, drum setup, etc. Only SysEx; no channel CC/PC (those come
    // from the channel model). Flushes in small batches so the setup
    // buffer pool of the output device is never overrun.
    void send_header_sysex(MidiSink& midi) const;

    // Compute the style's per-channel startup state, after voice remap
    // and output-channel routing (CASM, or Montage folding to 1-8).
    // Only channels the style actually defines are returned. If two
    // source channels route to the same output channel (possible only
    // with Montage folding) the higher source channel is listed last
    // and therefore wins when applied in order.
    [[nodiscard]] std::vector<ChannelSetup>
        build_channel_setup(const RemapSource& remap) const;

    // Configure the bass channel for mono legato (CC 126=0, CC 65=127, CC 5=1).
    // Called right before playback starts. ch is the bass channel (typically 10).
    void send_bass_legato_setup(MidiSink& midi, u8 ch) const;

    // ----- Accessors -----
    [[nodiscard]] constexpr u32 tempo_us()       const noexcept { return tempo_us_; }
    [[nodiscard]] constexpr u8  ts_num()         const noexcept { return ts_num_; }
    [[nodiscard]] constexpr u8  ts_den()         const noexcept { return ts_den_; }
    [[nodiscard]] constexpr u16 ticks_per_qn()   const noexcept { return ticks_per_qn_; }
    [[nodiscard]] std::string_view style_name()  const noexcept { return style_name_; }
    [[nodiscard]] std::string_view file_path()   const noexcept { return file_path_; }
    [[nodiscard]] constexpr u32 raw_hash()       const noexcept { return raw_hash_; }

    [[nodiscard]] const std::vector<StyleSection>& sections() const noexcept { return sections_; }
    [[nodiscard]] const std::vector<Event>&        events()   const noexcept { return events_; }
    [[nodiscard]] const std::array<ChVoice, 16>&   voices()   const noexcept { return ch_voices_; }
    [[nodiscard]] const CasmBlock&                 casm()     const noexcept { return casm_; }
    [[nodiscard]] const OtsBlock&                  ots()      const noexcept { return ots_; }
    [[nodiscard]] const std::vector<u8>&           sysex_pool() const noexcept { return sx_pool_; }
    [[nodiscard]] const std::vector<u8>&           header_sysex() const noexcept { return hdr_sx_; }
    [[nodiscard]] const std::vector<sysex::Message>& decoded_header_sysex() const noexcept { return decoded_hdr_sysex_; }
    [[nodiscard]] const std::vector<sysex::Message>& decoded_section_sysex() const noexcept { return decoded_section_sysex_; }

private:
    // ----- Internal parsing helpers -----
    void parse_track1(ByteReader& r, std::size_t track_end);
    void parse_extra_track(ByteReader& r, std::size_t track_end);
    void rebuild_sections();
    // Foreign-format parsers (style_foreign.cpp). Both populate
    // sections_/events_/tempo/ts and synthesize a default CASM.
    bool parse_ketron();   // classic KPAT layout (header byte[4] == 0)
    bool parse_korg();     // Korg PA "MSE" pointer-table layout
    // Build a synthetic CasmBlock with SFF-style default roles for the
    // given section names (used by SFF-BASIC fallback + foreign formats).
    void synthesize_default_casm(const std::vector<std::string>& sec_names);
    void attach_casm(std::size_t last_trk_end);
    void parse_casm_block(std::size_t casm_offset);
    void parse_cseg(std::span<const u8> data, CasmCseg& out);
    // Take a dynamic-extent span (the caller subspans into a 6-byte view).
    static void parse_note_range_ctb2(std::span<const u8> p, NoteRange& r);
    static void parse_note_range_ctab (std::span<const u8> p, NoteRange& r);
    static bool parse_casm_channel(std::span<const u8> data,
                                   bool is_ctb2, CasmChannel& cc);

    // ----- OTS parsing helpers -----
    // Find and parse the OTSc chunk. Scans the entire file from search_offset.
    void attach_ots(std::size_t search_offset);
    // Parse the OTSc payload (after the 8-byte chunk header). Finds all
    // MTrks inside — each becomes one OtsButton.
    void parse_ots_block(std::span<const u8> payload);
    // Parse one OTS MTrk into one OtsButton (4 parts: R1/R2/R3/Left).
    static void parse_ots_mtrk(std::span<const u8> mtrk_data, OtsButton& out);

    // ----- Fire-time cache helpers -----
    [[nodiscard]] static std::chrono::nanoseconds
        compute_section_fire_times(const StyleSection& sec,
                                   u32 base_tempo_us,
                                   u32 ticks_per_qn,
                                   u32 dur_ticks,
                                   std::span<const Event> events,
                                   std::span<i64> out_qpc,
                                   std::chrono::nanoseconds::rep qpc_freq);
    [[nodiscard]] std::filesystem::path cache_path() const;
    [[nodiscard]] bool cache_load();
    void cache_save() const;

    // ----- Storage -----
    std::vector<u8>        raw_;           // entire file
    u32                    raw_hash_   = 0;
    std::string            file_path_;
    std::string            style_name_;
    StyleFormat            format_     = StyleFormat::Yamaha;

    u32                    tempo_us_      = 500000;
    u8                     ts_num_        = 4;
    u8                     ts_den_        = 4;
    u16                    ticks_per_qn_  = 0;

    std::array<ChVoice, 16> ch_voices_{};
    std::vector<u8>         hdr_sx_;        // pre-SInt sysex stream
    std::vector<sysex::Message> decoded_hdr_sysex_;     // decoded hdr_sx_
    std::vector<sysex::Message> decoded_section_sysex_; // decoded post-SInt sysex (from sx_pool_)
    std::vector<Event>      events_;
    std::vector<u8>         sx_pool_;       // sysex payloads (post-SInt)
    std::vector<StyleSection> sections_;
    CasmBlock               casm_;
    OtsBlock                ots_;

    // Fire-time cache, in nanoseconds (fire-time cache).
    std::vector<i64>        fire_ns_;

    // Sample rate for fire-time calculations. We use
    // steady_clock::period::den / steady_clock::period::num which is
    // typically 1'000'000'000 (nanoseconds).
    static constexpr i64 kClockFreq =
        static_cast<i64>(std::chrono::nanoseconds::period::den) /
        static_cast<i64>(std::chrono::nanoseconds::period::num);
};

} // namespace prs