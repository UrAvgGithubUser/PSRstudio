// transpose.hpp — SFF2 note transposition engine.
//
// Transforms a source MIDI pitch to match the destination chord symbol,
// applying NTR / NTT / high_key / note_low / note_high rules from CASM.
//
// Usage (inside the player, per MIDI note-on event):
//
//   const prs::CasmChannel* cc = style.casm_get_channel(sec_name, src_ch);
//   const prs::NoteRange*   nr = prs::PrsStyle::casm_get_range(*cc, pitch);
//   transpose::ChordCtx src_chord{cc->src_root, cc->src_chord_type};
//   transpose::ChordCtx dst_chord{live_root, live_type};
//   int out = transpose::note(pitch, *nr, src_chord, dst_chord);
//   if (out >= 0) midi.short_msg(0x90 | cc->dst_ch, out, vel);
//
// Held notes across chord changes use transpose::retrigger().
#pragma once

#include "style.hpp"  // for prs::NoteRange, prs::Ntr, prs::Ntt, prs::Rtr
#include "chord_engine.hpp"  // for chord::ChordType

#include <array>
#include <cstdint>

namespace transpose {

using prs::u8;

// =========================================================
//  ChordContext — describes one chord (source or destination)
//
//  root: 0-11 = C-B, -1 = unknown/none
//  type: CASM chord type index (0..34), or -1 for chromatic/unknown.
//        For dst chords from the live detector, use chord_type_to_casm()
//        to convert from chord::ChordType before constructing.
// =========================================================
struct ChordCtx {
    int root = -1;
    int type = -1;  // CASM index (0..34), NOT chord::ChordType
    int bass = -1;  // Slash-chord bass note (0-11), or -1 = use root
};

// =========================================================
//  ChordType → CASM chord type index mapping
//
//  chord::ChordType uses its own numbering (None=0, Maj=1, Min=2, ...).
//  CASM uses Yamaha's numbering (Maj=0, min=8, 7th=19, ...).
//  Use this to convert a live-detected chord for transposition.
// =========================================================
[[nodiscard]] int chord_type_to_casm(chord::ChordType ct) noexcept;

// =========================================================
//  Scale tables (12 semitones from root, 1 = in scale)
// =========================================================
extern const std::array<u8, 12> kScaleMajor;
extern const std::array<u8, 12> kScaleMelodicMinor;
extern const std::array<u8, 12> kScaleHarmonicMinor;
extern const std::array<u8, 12> kScaleNaturalMinor;
extern const std::array<u8, 12> kScaleDorian;

// Chord tone tables: 1 = tone belongs to chord type.
// Indexed by CASM src_chord_type (0..34). Each row: 12 semitones from root.
extern const std::array<std::array<u8, 12>, 35> kChordTones;

// =========================================================
//  Core transposition
//
//  src_pitch — original MIDI pitch (0-127)
//  nr        — NoteRange from CASM for this channel+range
//  src       — chord the style was recorded in (type = CASM index)
//  dst       — chord the user is currently playing (type = CASM index,
//              use chord_type_to_casm() to convert from chord::ChordType)
//
//  Returns the transposed MIDI pitch (0-127), or -1 if the note
//  should be suppressed.
// =========================================================
[[nodiscard]] int note(int src_pitch,
                       const prs::NoteRange& nr,
                       const ChordCtx& src,
                       const ChordCtx& dst) noexcept;

// =========================================================
//  Arpeggio-aware variant — call for NTT::GuitarArpeggio channels.
//  Requires the previous source and output pitches for contour preservation.
//  Pass -1 for both prev_* on the first note of a phrase.
// =========================================================
[[nodiscard]] int note_arpeggio(int src_pitch,
                                int prev_src_pitch,
                                int prev_out_pitch,
                                const prs::NoteRange& nr,
                                const ChordCtx& dst) noexcept;

// =========================================================
//  Retrigger handling
//
//  Called when a note that was already playing crosses a chord
//  change boundary.
// =========================================================
enum class RtrAction : u8 {
    None      = 0,  // no change needed
    Stop      = 1,  // send note-off, do not re-trigger
    Shift     = 2,  // update pitch, note keeps sounding
    Retrigger = 3,  // note-off then note-on at new pitch
};

struct RtrResult {
    RtrAction action = RtrAction::None;
    int       new_pitch = -1;  // valid when action != Stop
};

// src: the chord the held note was last transposed FOR (the previous live
//      chord, or the CASM source chord if the note was played before any
//      live chord existed).
// src_was_transposed: false if `held_pitch` is still an untransposed
//      source pitch (no High Key shift has been applied to it yet).
[[nodiscard]] RtrResult retrigger(int held_pitch,
                                  const prs::NoteRange& nr,
                                  const ChordCtx& src,
                                  const ChordCtx& dst,
                                  bool src_was_transposed = true) noexcept;

// =========================================================
//  Pitch utility helpers (exposed for tests)
// =========================================================

// Clamp pitch into [note_low .. note_high] by shifting octaves.
[[nodiscard]] int clamp(int pitch, u8 note_low, u8 note_high) noexcept;

// =========================================================
//  MegaVoice support
//
//  Yamaha MegaVoice voices (bank MSB=8, LSB 0-16) carry extra sampled
//  articulations (fret noises, slides, harmonics, body hits, ...) in
//  the high note regions at C6 and above. These notes are position-
//  sensitive sample mappings — they must NOT be transposed at all.
//  On a MegaVoice channel, notes below C6 transpose normally; notes
//  at C6 and above are played at their recorded pitch.
//
// =========================================================
namespace megavoice {

inline constexpr int kMegaMsb    = 8;    // bank MSB shared by all MegaVoice voices
inline constexpr int kMegaLsbMax = 16;   // LSB 0-16 = the MegaVoice bank range

// Articulation regions (scientific pitch): above C6, split at C8.
inline constexpr int kRegion1Low  = 84;   // C6 — no-transposition threshold
inline constexpr int kRegion1High = 107;  // B7
inline constexpr int kRegion2Low  = 108;  // C8
inline constexpr int kRegion2High = 131;  // B9 — above MIDI's 127 ceiling,
                                          // so the effective cap is 127

// True when the bank select (MSB, LSB) selects a MegaVoice voice.
[[nodiscard]] inline bool is_mega_bank(int msb, int lsb) noexcept {
    return msb == kMegaMsb && lsb >= 0 && lsb <= kMegaLsbMax;
}

// True when the pitch lies in one of the MegaVoice articulation regions
// (C6 and above).
[[nodiscard]] inline bool in_region(int pitch) noexcept {
    return pitch >= kRegion1Low && pitch <= kRegion2High;
}

} // namespace megavoice

// Find the nearest pitch class pc (0-11) to reference_pitch.
[[nodiscard]] int nearest_pitch_class(int reference_pitch, int pc) noexcept;

// Find nearest chord tone to reference_pitch given chord root + CASM type index.
[[nodiscard]] int nearest_chord_tone(int reference_pitch, int root,
                                     int chord_type_idx) noexcept;

// Find nearest scale degree to reference_pitch given root + scale table.
[[nodiscard]] int nearest_scale_note(int reference_pitch, int root,
                                     const std::array<u8, 12>& scale) noexcept;

// =========================================================
//  Phrase-level chord-voicing fit
//
//  Given a set of simultaneous source notes (one chord-symbol span),
//  finds the best destination voicing by trying all N! permutations
//  (for N ≤ kMaxChordPhraseNotes) and picking the one with the lowest
//  total voice-leading cost (sum of |src_pitch - out_pitch| distances).
//
//  For phrases larger than kMaxChordPhraseNotes, falls back to greedy
//  per-note transpose::note() calls.
//
//  src_events and out_pitches must be the same size. Each output
//  position corresponds to the input at the same index.
// =========================================================
inline constexpr int kMaxChordPhraseNotes = 6;

struct PhraseEvent {
    int pitch;          // source MIDI pitch (0-127)
    int parent_index;   // index of parent note (for RTR support), or -1
};

[[nodiscard]] int fit_phrase(
    std::span<const PhraseEvent> src_events,
    std::span<int> out_pitches,
    const prs::NoteRange& nr,
    const ChordCtx& src,
    const ChordCtx& dst) noexcept;

} // namespace transpose