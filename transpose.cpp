// transpose.cpp — SFF2 note transposition engine.
#include "transpose.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <span>

namespace transpose {

// =========================================================
//  Scale tables (12 semitones from root, 1 = in scale)
// =========================================================
const std::array<u8, 12> kScaleMajor =         { 1,0,1,0,1,1,0,1,0,1,0,1 };
const std::array<u8, 12> kScaleMelodicMinor =  { 1,0,1,1,0,1,0,1,0,1,0,1 };
const std::array<u8, 12> kScaleHarmonicMinor = { 1,0,1,1,0,1,0,1,1,0,0,1 };
const std::array<u8, 12> kScaleNaturalMinor =  { 1,0,1,1,0,1,0,1,1,0,1,0 };
const std::array<u8, 12> kScaleDorian =        { 1,0,1,1,0,1,0,1,0,1,1,0 };

// =========================================================
//  Chord tone tables (boolean, root = interval 0)
//  Indexed by CASM src_chord_type (0..34)
// =========================================================
const std::array<std::array<u8, 12>, 35> kChordTones = {{
    {1,0,0,0,1,0,0,1,0,0,0,0}, // 00 Maj
    {1,0,0,0,1,0,0,1,0,1,0,0}, // 01 Maj6
    {1,0,0,0,1,0,0,1,0,0,0,1}, // 02 Maj7
    {1,0,0,0,1,0,1,1,0,0,0,1}, // 03 Maj7#11
    {1,0,1,0,1,0,0,1,0,0,0,0}, // 04 Maj(9)
    {1,0,1,0,1,0,0,1,0,0,0,1}, // 05 Maj7(9)
    {1,0,1,0,1,0,0,1,0,1,0,0}, // 06 Maj6(9)
    {1,0,0,0,1,0,0,0,1,0,0,0}, // 07 aug
    {1,0,0,1,0,0,0,1,0,0,0,0}, // 08 min
    {1,0,0,1,0,0,0,1,0,1,0,0}, // 09 min6
    {1,0,0,1,0,0,0,1,0,0,1,0}, // 0A min7
    {1,0,0,1,0,0,1,0,0,0,1,0}, // 0B min7b5
    {1,0,1,1,0,0,0,1,0,0,0,0}, // 0C min(9)
    {1,0,1,1,0,0,0,1,0,0,1,0}, // 0D min7(9)
    {1,0,0,1,0,1,0,1,0,0,1,0}, // 0E min7(11)
    {1,0,0,1,0,0,0,1,0,0,0,1}, // 0F minMaj7
    {1,0,1,1,0,0,0,1,0,0,0,1}, // 10 minMaj7(9)
    {1,0,0,1,0,0,1,0,0,0,0,0}, // 11 dim
    {1,0,0,1,0,0,1,0,0,1,0,0}, // 12 dim7
    {1,0,0,0,1,0,0,1,0,0,1,0}, // 13 7th
    {1,0,0,0,0,1,0,1,0,0,1,0}, // 14 7sus4
    {1,0,0,0,1,0,1,0,0,0,1,0}, // 15 7b5
    {1,0,1,0,1,0,0,1,0,0,1,0}, // 16 7(9)
    {1,0,0,0,1,0,1,1,0,0,1,0}, // 17 7#11
    {1,0,0,0,1,0,0,1,0,1,1,0}, // 18 7(13)
    {1,1,0,0,1,0,0,1,0,0,1,0}, // 19 7(b9)
    {1,0,0,0,1,0,0,1,1,0,1,0}, // 1A 7(b13)
    {1,0,0,1,1,0,0,1,0,0,1,0}, // 1B 7(#9)
    {1,0,0,0,1,0,0,0,1,0,0,1}, // 1C Maj7aug
    {1,0,0,0,1,0,0,0,1,0,1,0}, // 1D 7aug
    {1,0,0,0,0,0,0,0,0,0,0,0}, // 1E 1+8
    {1,0,0,0,0,0,0,1,0,0,0,0}, // 1F 1+5
    {1,0,0,0,0,1,0,1,0,0,0,0}, // 20 sus4
    {1,0,1,0,0,0,0,1,0,0,0,0}, // 21 1+2+5
    {1,0,0,0,0,0,0,0,0,0,0,0}, // 22 cancel
}};

// =========================================================
//  ChordType → CASM chord type index mapping
// =========================================================
int chord_type_to_casm(chord::ChordType ct) noexcept {
    // chord::ChordType numbering:
    //  0 None, 1 Maj, 2 Min, 3 Dom7, 4 Maj7, 5 Min7, 6 Dim, 7 Dim7,
    //  8 Aug, 9 Sus4, 10 Sus2, 11 Maj6, 12 Min6, 13 Dom9, 14 MinMaj7,
    // 15 Add9, 16 Sus4_7, 17 Min9, 18 Maj9, 19 Min7b5, 20 Dom7b5,
    // 21 Dom7s11, 22 Dom7b9, 23 Dom7s9, 24 Dom7b13, 25 Dom7Aug,
    // 26 Maj7s11, 27 Maj7Aug, 28 Maj69, 29 MinAdd9, 30 Min11,
    // 31 Dom13, 32 Power, 33 MinMaj7b5, 34 Maj7b5, 35 Majb5,
    // 36 Dom11, 37 NoChord
    static constexpr std::array<int, 38> map = {
         0,  // None      → 0x00 Maj (safe fallback)
         0,  // Maj       → 0x00 Maj
         8,  // Min       → 0x08 min
        19,  // Dom7      → 0x13 7th
         2,  // Maj7      → 0x02 Maj7
        10,  // Min7      → 0x0A min7
        17,  // Dim       → 0x11 dim
        18,  // Dim7      → 0x12 dim7
         7,  // Aug       → 0x07 aug
        32,  // Sus4      → 0x20 sus4
        33,  // Sus2      → 0x21 1+2+5
         1,  // Maj6      → 0x01 Maj6
         9,  // Min6      → 0x09 min6
        22,  // Dom9      → 0x16 7(9)
        15,  // MinMaj7   → 0x0F minMaj7
         4,  // Add9      → 0x04 Maj(9)
        20,  // Sus4_7    → 0x14 7sus4
        13,  // Min9      → 0x0D min7(9)
         5,  // Maj9      → 0x05 Maj7(9)
        11,  // Min7b5    → 0x0B min7b5
        21,  // Dom7b5    → 0x15 7b5
        23,  // Dom7s11   → 0x17 7#11
        25,  // Dom7b9    → 0x19 7(b9)
        27,  // Dom7s9    → 0x1B 7(#9)
        26,  // Dom7b13   → 0x1A 7(b13)
        29,  // Dom7Aug   → 0x1D 7aug
         3,  // Maj7s11   → 0x03 Maj7#11
        28,  // Maj7Aug   → 0x1C Maj7aug
         6,  // Maj69     → 0x06 Maj6(9)
        12,  // MinAdd9   → 0x0C min(9)
        14,  // Min11     → 0x0E min7(11)
        24,  // Dom13     → 0x18 7(13)
        31,  // Power     → 0x1F 1+5
        11,  // MinMaj7b5 → 0x0B min7b5 (closest voicing; maj7 snaps to b7)
         3,  // Maj7b5    → 0x03 Maj7#11 (superset: holds b5 + maj7)
        21,  // Majb5     → 0x15 7b5 (superset: holds b5 + maj3)
        22,  // Dom11     → 0x16 7(9) (nearest dominant voicing)
         0,  // NoChord   → never voiced (player clears the chord)
    };
    const auto idx = static_cast<unsigned>(ct);
    if (idx >= map.size()) return 0;
    return map[idx];
}

// =========================================================
//  Pitch utilities
// =========================================================
int clamp(int pitch, u8 note_low, u8 note_high) noexcept {
    // Always keep the result inside the legal MIDI range first,
    // regardless of whether the NoteRange itself is narrower than an
    // octave or degenerate (note_low > note_high) — those cases used
    // to return the pitch completely unclamped, which let wildly
    // out-of-range values (e.g. from guitar_transpose) through as-is.
    while (pitch < 0)   pitch += 12;
    while (pitch > 127) pitch -= 12;

    if (note_low > note_high) return pitch;
    if (static_cast<int>(note_high) - static_cast<int>(note_low) < 12) return pitch;

    while (pitch < static_cast<int>(note_low))  pitch += 12;
    while (pitch > static_cast<int>(note_high)) pitch -= 12;
    return pitch;
}

int nearest_pitch_class(int reference_pitch, int pc) noexcept {
    int base = (reference_pitch / 12) * 12 + pc;
    int best = base;
    int best_dist = std::abs(base - reference_pitch);
    for (int delta = -12; delta <= 12; delta += 12) {
        int candidate = base + delta;
        if (candidate < 0 || candidate > 127) continue;
        int dist = std::abs(candidate - reference_pitch);
        if (dist < best_dist) { best_dist = dist; best = candidate; }
    }
    return best;
}

int nearest_chord_tone(int reference_pitch, int root,
                       int chord_type_idx) noexcept {
    if (chord_type_idx < 0 || chord_type_idx > 34) chord_type_idx = 0;
    const auto& tones = kChordTones[static_cast<std::size_t>(chord_type_idx)];
    int best = -1, best_dist = 999;
    for (int iv = 0; iv < 12; ++iv) {
        if (!tones[static_cast<std::size_t>(iv)]) continue;
        int pc = (root + iv) % 12;
        int candidate = nearest_pitch_class(reference_pitch, pc);
        int dist = std::abs(candidate - reference_pitch);
        if (dist < best_dist) { best_dist = dist; best = candidate; }
    }
    return (best >= 0) ? best : reference_pitch;
}

int nearest_scale_note(int reference_pitch, int root,
                       const std::array<u8, 12>& scale) noexcept {
    int best = -1, best_dist = 999;
    for (int iv = 0; iv < 12; ++iv) {
        if (!scale[static_cast<std::size_t>(iv)]) continue;
        int pc = (root + iv) % 12;
        int candidate = nearest_pitch_class(reference_pitch, pc);
        int dist = std::abs(candidate - reference_pitch);
        if (dist < best_dist) { best_dist = dist; best = candidate; }
    }
    return (best >= 0) ? best : reference_pitch;
}

// =========================================================
//  Internal helpers
// =========================================================
namespace {

int root_shift(int pitch, int src_root, int dst_root) noexcept {
    int delta = ((dst_root - src_root) % 12 + 12) % 12;
    if (delta > 6) delta -= 12;
    return pitch + delta;
}

// High Key for NTT=Bypass (simple root shift).
// Uses the same anti-double-drop logic as melody_degree_map: only
// shift down if the result is above the original source pitch.
int apply_high_key_simple(int pitch, int dst_root, int src_pitch, u8 high_key) noexcept {
    if (dst_root > static_cast<int>(high_key)) {
        if (pitch >= src_pitch) pitch -= 12;
    }
    return pitch;
}

// =========================================================
//  Role tables for the §5.2.5.2 / §5.2.5.4 spec algorithms
// =========================================================

// Interval of the source pitch above its root (0-11).
[[nodiscard]] int rel_iv(int pitch, int root) noexcept {
    return ((pitch - root) % 12 + 12) % 12;
}

// "Third" interval per CASM chord type (index 0-34), -1 = the type has
// no third-like tone. Sus types map their characteristic tone (the 4th
// / 2nd) into the third slot so maj<->sus changes adapt sensibly.
constexpr std::array<int, 35> kTypeThird = {
    4, 4, 4, 4, 4, 4, 4, 4,          // 00-07  Maj .. aug
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, // 08-18  min .. dim7
    4,                               // 19     7th
    5,                               // 20     7sus4
    4, 4, 4, 4, 4, 4, 4, 4, 4,       // 21-29  7b5 .. 7aug
    -1, -1,                          // 30-31  1+8, 1+5
    5, 2,                            // 32-33  sus4, 1+2+5
    -1                               // 34     cancel
};

// Minor-family types (third = minor 3rd): 0x08 min .. 0x12 dim7.
[[nodiscard]] bool is_minor_family(int type) noexcept {
    return type >= 8 && type <= 18;
}

// =========================================================
//  §5.2.5.2 — NTT Melodic Minor / Harmonic Minor, per the spec:
//    "Lowers the third scale degree by a semitone when the played chord
//    changes from a major to a minor chord, or raises the minor third
//    scale degree a semitone when changing from a minor to a major
//    chord. Other notes are not changed."
//    (Harmonic Minor: the sixth scale degree adapts too.)
//
//  shifted_pitch is the ROOT-TRANSPOSED pitch. Only a note sitting on
//  the SOURCE chord's third (resp. sixth) degree is re-targeted to the
//  DESTINATION chord's same degree; every other note keeps its
//  root-shifted pitch. The _5 variants use the same algorithm — the
//  spec describes no separate behavior for them (the old scale-snap
//  interpretation over-adapted and is retired).
// =========================================================
[[nodiscard]] int minor_adapt(int shifted_pitch, int src_root, int src_type,
                              int dst_root, int dst_type, bool sixth_too) noexcept {
    // Root transposition preserves the interval relative to the root,
    // so the shifted pitch's rel equals the source pitch's rel.
    const int rel = rel_iv(shifted_pitch, dst_root);

    const int src_third = (src_type >= 0 && src_type <= 34)
                              ? kTypeThird[static_cast<std::size_t>(src_type)] : 4;
    if (src_third >= 0 && rel == src_third) {
        int dst_third = (dst_type >= 0 && dst_type <= 34)
                            ? kTypeThird[static_cast<std::size_t>(dst_type)] : src_third;
        if (dst_third < 0) dst_third = src_third;
        return nearest_pitch_class(shifted_pitch, (dst_root + dst_third) % 12);
    }
    if (sixth_too) {
        // Sixth scale degree: 9 in a major context, b6 (8) in a minor one.
        const int src_sixth = is_minor_family(src_type) ? 8 : 9;
        if (rel == src_sixth) {
            const int dst_sixth = is_minor_family(dst_type) ? 8 : 9;
            return nearest_pitch_class(shifted_pitch, (dst_root + dst_sixth) % 12);
        }
    }
    return shifted_pitch;   // "other notes are not changed"
}

// =========================================================
//  §5.2.5.4 (Pöhnl) — Root Fixed + Chord role mapping.
//
//  The source note's harmonic FUNCTION is preserved in the destination
//  chord: root->root, third->third, fifth->fifth, seventh->seventh.
//  Over a destination without the same role, the 7th folds to the root;
//  extensions (9th/11th/13th) carry over only when the destination has
//  them, else fall back to the nearest chord tone. This honors the
//  spec's constraints that a source root/fifth never maps to a
//  destination third/seventh, and never invents double roles the way a
//  plain nearest-tone snap can.
// =========================================================
[[nodiscard]] int pohnl_chord_map(int src_pitch, int src_root, int src_type,
                                  int dst_root, int dst_type) noexcept {
    const int rel = rel_iv(src_pitch, src_root);
    if (rel == 0) return nearest_pitch_class(src_pitch, dst_root);   // root -> root

    const auto& src_tones =
        kChordTones[static_cast<std::size_t>((src_type >= 0 && src_type <= 34) ? src_type : 0)];
    const auto& dst_tones =
        kChordTones[static_cast<std::size_t>((dst_type >= 0 && dst_type <= 34) ? dst_type : 0)];

    auto third_of = [](int type) -> int {
        return (type >= 0 && type <= 34) ? kTypeThird[static_cast<std::size_t>(type)] : 4;
    };
    auto fifth_of = [](const std::array<u8, 12>& t) -> int {
        if (t[7]) return 7;
        if (t[8]) return 8;   // aug
        if (t[6]) return 6;   // dim / m7b5 / 7b5
        return -1;
    };
    auto seventh_of = [](const std::array<u8, 12>& t) -> int {
        if (t[10]) return 10;
        if (t[11]) return 11;
        if (t[9])  return 9;  // 6th chords: the 6th takes the 7th slot
        return -1;
    };

    const int src_third = third_of(src_type);
    const int dst_third = third_of(dst_type);
    const int src_fifth = fifth_of(src_tones);
    const int dst_fifth = fifth_of(dst_tones);
    const int src_sev   = seventh_of(src_tones);
    const int dst_sev   = seventh_of(dst_tones);

    int target = -1;
    if (src_third >= 0 && rel == src_third && dst_third >= 0) {
        target = (dst_root + dst_third) % 12;
    } else if (src_fifth >= 0 && rel == src_fifth) {
        // The fifth maps only to the destination fifth — or to the root
        // when the destination has none (1+8).
        target = (dst_fifth >= 0) ? (dst_root + dst_fifth) % 12 : dst_root % 12;
    } else if (src_sev >= 0 && rel == src_sev) {
        // 7th/6th role: kept when the destination has one, folds to the
        // root over plain triads.
        target = (dst_sev >= 0) ? (dst_root + dst_sev) % 12 : dst_root % 12;
    } else if (rel == 2 && dst_tones[2]) {
        target = (dst_root + 2) % 12;   // 9th carries over
    } else if (rel == 5 && dst_tones[5]) {
        target = (dst_root + 5) % 12;   // 11th carries over
    } else if (rel == 9 && dst_tones[9]) {
        target = (dst_root + 9) % 12;   // 13th/6th carries over
    }
    if (target < 0) return nearest_chord_tone(src_pitch, dst_root, dst_type);
    return nearest_pitch_class(src_pitch, target);
}

// (guitar_chord_fit removed — replaced by guitar_transpose above)

// Precomputed ordered degree lists for degree-index mapping.
// degree_lists[type] = ordered array of intervals present in that chord type.
// degree_counts[type] = count of intervals.
struct DegreeList {
    std::array<int, 12> intervals{};
    int count = 0;
};

[[nodiscard]] std::array<DegreeList, 35> compute_degree_lists() noexcept {
    std::array<DegreeList, 35> lists{};
    for (int t = 0; t < 35; ++t) {
        int cnt = 0;
        for (int iv = 0; iv < 12; ++iv) {
            if (kChordTones[static_cast<std::size_t>(t)][static_cast<std::size_t>(iv)]) {
                lists[static_cast<std::size_t>(t)].intervals[static_cast<std::size_t>(cnt++)] = iv;
            }
        }
        lists[static_cast<std::size_t>(t)].count = cnt;
    }
    return lists;
}

const DegreeList& get_chord_degrees(int type) noexcept {
    static const auto lists = compute_degree_lists();
    if (type < 0 || type > 34) type = 0;
    return lists[static_cast<std::size_t>(type)];
}

// Forced scale degrees for modal NTT variants.
void get_scale_degrees(prs::Ntt ntt, int /*root*/,
                       std::array<int, 12>& out_degrees, int& out_count) noexcept {
    const std::array<u8, 12>* scale;
    switch (ntt) {
        case prs::Ntt::MelodicMinor:
        case prs::Ntt::MelodicMinor5:   scale = &kScaleMelodicMinor;  break;
        case prs::Ntt::HarmonicMinor:
        case prs::Ntt::HarmonicMinor5:  scale = &kScaleHarmonicMinor; break;
        case prs::Ntt::NaturalMinor:
        case prs::Ntt::NaturalMinor5:   scale = &kScaleNaturalMinor;  break;
        case prs::Ntt::Dorian:
        case prs::Ntt::Dorian5:         scale = &kScaleDorian;        break;
        default:                        scale = &kScaleMajor;          break;
    }
    int cnt = 0;
    for (int iv = 0; iv < 12; ++iv) {
        if ((*scale)[static_cast<std::size_t>(iv)]) {
            out_degrees[static_cast<std::size_t>(cnt++)] = iv;
        }
    }
    // For _5 variants, transpose degrees by +7 semitones
    if (ntt == prs::Ntt::MelodicMinor5 || ntt == prs::Ntt::HarmonicMinor5 ||
        ntt == prs::Ntt::NaturalMinor5 || ntt == prs::Ntt::Dorian5) {
        for (int i = 0; i < cnt; ++i) {
            out_degrees[static_cast<std::size_t>(i)] =
                (out_degrees[static_cast<std::size_t>(i)] + 7) % 12;
        }
        std::sort(out_degrees.begin(), out_degrees.begin() + cnt);
    }
    out_count = cnt;
}

// Degree-index mapping for NTT_MELODY and modal variants.
int melody_degree_map(int src_pitch,
                      int src_root, int src_type,
                      int dst_root, int dst_type,
                      const int* forced_scale_degrees, int forced_scale_count,
                      bool bass_on, u8 high_key) noexcept {
    // Source degree list
    const DegreeList& src_dl = get_chord_degrees(src_type);
    int src_deg_cnt = src_dl.count;

    // Find nearest source degree index
    int src_rel = ((src_pitch - src_root) % 12 + 12) % 12;
    int best_idx = 0, best_dist = 12;
    for (int i = 0; i < src_deg_cnt; ++i) {
        int iv = src_dl.intervals[static_cast<std::size_t>(i)];
        int dist = std::abs(iv - src_rel);
        if (dist > 6) dist = 12 - dist;
        if (dist < best_dist) { best_dist = dist; best_idx = i; }
    }

    // Destination degree list (chord or forced scale)
    int dst_deg_cnt;
    const int* dst_degs;
    if (forced_scale_degrees) {
        dst_degs = forced_scale_degrees;
        dst_deg_cnt = forced_scale_count;
    } else {
        const DegreeList& dst_dl = get_chord_degrees(dst_type);
        dst_degs = dst_dl.intervals.data();
        dst_deg_cnt = dst_dl.count;
    }

    int dst_iv;
    if (best_idx < dst_deg_cnt) {
        dst_iv = dst_degs[static_cast<std::size_t>(best_idx)];
    } else {
        dst_iv = dst_degs[static_cast<std::size_t>(dst_deg_cnt - 1)];
    }

    int dst_pc;
    if (bass_on && best_idx == 0) {
        dst_pc = dst_root % 12;
    } else {
        dst_pc = (dst_root + dst_iv) % 12;
    }

    // Anchor: src_pitch shifted by shortest root delta.
    // This gives us a starting pitch that's close to the source
    // pitch but adjusted for the root change.
    int anchor = root_shift(src_pitch, src_root, dst_root);
    int result = nearest_pitch_class(anchor, dst_pc);

    // High Key (§5.2.7): "Chords with a root higher than the specified
    // limit will be played in the octave immediately below."
    //
    // The spec says the WHOLE PHRASE shifts down when dst_root > high_key.
    // We apply this to ALL notes (not just the bass degree) because the
    // spec says "chords" (the entire channel), not just the bass note.
    //
    // CRITICAL: we must check the RESULT pitch, not just dst_root, because
    // root_shift's shortest-distance path can already push the anchor
    // down. If root_shift took -5 (C→G down) and then we subtract another
    // 12 for High Key, the note drops 17 semitones — a double-octave drop.
    //
    // The fix: only apply High Key if the result is still above the
    // original source pitch's octave. If root_shift already brought it
    // down, don't compound it.
    if (dst_root > static_cast<int>(high_key)) {
        // Only shift down if the result is ABOVE the source pitch
        // (i.e., root_shift went UP, and High Key needs to bring it
        // back down). If root_shift went DOWN, the note is already
        // low enough — don't shift further.
        if (result >= src_pitch) {
            result -= 12;
        }
    }

    return result;
}

} // anonymous namespace

// =========================================================
//  Internal: Guitar NTR — string-to-fret mapping
//
//  Per spec §5.2.5.1:
//    Source pitch class → guitar string:
//      B  (11) → 1st string (high E)
//      A  (9)  → 2nd string (B)
//      G  (7)  → 3rd string (G)
//      F  (5)  → 4th string (D)
//      E  (4)  → 5th string (A)
//      D  (2)  → 6th string (low E)
//      C# (1)  → a fifth above/below
//      C  (0)  → root note
//
//    Octave → fretboard position:
//      C2-B2 → 1st position (open + low frets)
//      C3-B3 → 2nd position
//      C4-B4 → 3rd position
//      C5-B5 → 4th position
//
//  Standard guitar tuning (MIDI pitches at open string):
//    String 6 (low E): E2 = MIDI 40
//    String 5 (A):     A2 = MIDI 45
//    String 4 (D):     D3 = MIDI 50
//    String 3 (G):     G3 = MIDI 55
//    String 2 (B):     B3 = MIDI 59
//    String 1 (high E): E4 = MIDI 64
//
//  For a given destination chord, each string plays the nearest
//  chord tone to its position on the fretboard.
// =========================================================
namespace {

// Standard guitar open-string MIDI pitches (string 1 = high E, string 6 = low E)
constexpr std::array<int, 6> kGuitarOpenStrings = {
    64,  // String 1: E4 (high E)
    59,  // String 2: B3
    55,  // String 3: G3
    50,  // String 4: D3
    45,  // String 5: A2
    40,  // String 6: E2 (low E)
};

// Map source pitch class to guitar string index (0=string 1 high E, 5=string 6 low E).
// Returns -1 for pitch classes not in the mapping (shouldn't happen with valid styles).
[[nodiscard]] int pc_to_guitar_string(int pc) noexcept {
    switch (pc) {
        case 11: return 0;  // B  → 1st string (high E)
        case 9:  return 1;  // A  → 2nd string (B)
        case 7:  return 2;  // G  → 3rd string (G)
        case 5:  return 3;  // F  → 4th string (D)
        case 4:  return 4;  // E  → 5th string (A)
        case 2:  return 6;  // D  → 6th string (low E) — note: index 6, corrected below
        case 1:  return -1; // C# → fifth above/below (handle specially)
        case 0:  return -1; // C  → root note (handle specially)
        default: return -1;
    }
}

// Map source pitch to a guitar-string-based output pitch.
// The source octave determines fretboard position.
[[nodiscard]] int guitar_transpose(int src_pitch, int dst_root, int dst_type,
                                   const ChordCtx& dst, bool bass_on) noexcept {
    const int pc = src_pitch % 12;
    const int octave = src_pitch / 12;

    // C and C# are special: C = root note, C# = fifth above/below.
    // §5.2.5.1: "C and C# will be mapped to the root of an on-bass chord,
    // if parameter BASS is set to on" — with BASS on, C targets the
    // played on-bass root (the named bass), not the chord root.
    if (pc == 0) {
        if (bass_on && dst.bass >= 0) {
            return clamp(nearest_pitch_class(src_pitch, dst.bass % 12), 0, 127);
        }
        return clamp(nearest_pitch_class(src_pitch, dst_root), 0, 127);
    }
    if (pc == 1) {
        int fifth = (dst_root + 7) % 12;
        return clamp(nearest_pitch_class(src_pitch, fifth), 0, 127);
    }

    int string_idx = pc_to_guitar_string(pc);
    if (string_idx == 6) string_idx = 5;

    if (string_idx < 0 || string_idx > 5) {
        return nearest_chord_tone(src_pitch, dst_root, dst_type);
    }

    int open_pitch = kGuitarOpenStrings[static_cast<std::size_t>(string_idx)];
    int position_shift = (octave - 2) * 12;
    int target_pitch = open_pitch + position_shift;

    // The fretboard-position formula assumes source patterns stay within
    // guitar octaves 2-5 (§5.2.5.1). A style can include occasional notes
    // outside that band; without folding, target_pitch can land far
    // outside 0-127, and nearest_chord_tone() would then hunt for chord
    // tones in that same wrong octave — producing a note transposed
    // several octaves away from the rest of the chord.
    while (target_pitch > 115) target_pitch -= 12;
    while (target_pitch < 12)  target_pitch += 12;

    return nearest_chord_tone(target_pitch, dst_root, dst_type);
}

} // anonymous namespace

// =========================================================
//  note — main entry point
// =========================================================
int note(int src_pitch, const prs::NoteRange& nr,
         const ChordCtx& src, const ChordCtx& dst) noexcept {
    if (src_pitch < 0 || src_pitch > 127) return src_pitch;

    int pitch    = src_pitch;
    int dst_root = (dst.root >= 0) ? dst.root : 0;
    int src_root = (src.root >= 0) ? src.root : 0;
    int dst_type = (dst.type >= 0) ? dst.type : 0;
    int src_type = (src.type >= 0 && src.type <= 34) ? src.type : 0;

    switch (nr.ntr) {
        case prs::Ntr::RootFixed:
            // §5.2.5.2: Bypass = no transposition at all.
            // Chord = §5.2.5.4 Pöhnl role mapping (function-preserving).
            // Melody = adapts to the destination chord TYPE without root
            //          movement (Table 36: monophonic channels).
            // Modal minors = degree adaptation only, no root shift.
            if (nr.ntt == prs::Ntt::Bypass) {
                return clamp(src_pitch, nr.note_low, nr.note_high);  // no transposition
            } else if (nr.ntt == prs::Ntt::Chord) {
                pitch = pohnl_chord_map(src_pitch, src_root, src_type,
                                        dst_root, dst_type);
                return clamp(pitch, nr.note_low, nr.note_high);
            } else if (nr.ntt == prs::Ntt::Melody) {
                // Same root on both sides: the degree mapping adapts the
                // note to the destination chord type while the anchor
                // (root shift) is the identity, so the line stays near
                // its written range. High Key is Root-Trans-only (§5.2.7).
                pitch = melody_degree_map(src_pitch, src_root, src_type,
                                          src_root, dst_type,
                                          nullptr, 0, nr.bass_on, 127);
                return clamp(pitch, nr.note_low, nr.note_high);
            } else if (nr.ntt == prs::Ntt::MelodicMinor ||
                       nr.ntt == prs::Ntt::MelodicMinor5) {
                pitch = minor_adapt(src_pitch, src_root, src_type,
                                    src_root, dst_type, false);
                return clamp(pitch, nr.note_low, nr.note_high);
            } else if (nr.ntt == prs::Ntt::HarmonicMinor ||
                       nr.ntt == prs::Ntt::HarmonicMinor5) {
                pitch = minor_adapt(src_pitch, src_root, src_type,
                                    src_root, dst_type, true);
                return clamp(pitch, nr.note_low, nr.note_high);
            } else {
                // Everything else with RootFixed: note stays at original
                // pitch (Root Fixed = no root shift). Still apply limits.
                return clamp(src_pitch, nr.note_low, nr.note_high);
            }

        case prs::Ntr::RootTrans: {
            switch (nr.ntt) {
                case prs::Ntt::Bypass:
                    pitch = root_shift(pitch, src_root, dst_root);
                    break;
                case prs::Ntt::Chord:
                    pitch = nearest_chord_tone(pitch, dst_root, dst_type);
                    break;
                case prs::Ntt::Melody:
                    pitch = melody_degree_map(pitch, src_root, src_type,
                                              dst_root, dst_type,
                                              nullptr, 0, nr.bass_on, nr.high_key);
                    break;
                case prs::Ntt::MelodicMinor:
                case prs::Ntt::MelodicMinor5:
                case prs::Ntt::HarmonicMinor:
                case prs::Ntt::HarmonicMinor5: {
                    // §5.2.5.2: root transposition, then ONLY the third
                    // (melodic minor) or third+sixth (harmonic minor)
                    // degree adapts on a maj<->min change — every other
                    // note keeps its root-shifted pitch.
                    pitch = root_shift(pitch, src_root, dst_root);
                    pitch = minor_adapt(pitch, src_root, src_type,
                                        dst_root, dst_type,
                                        nr.ntt == prs::Ntt::HarmonicMinor ||
                                        nr.ntt == prs::Ntt::HarmonicMinor5);
                    break;
                }
                case prs::Ntt::NaturalMinor:
                case prs::Ntt::NaturalMinor5:
                case prs::Ntt::Dorian:
                case prs::Ntt::Dorian5: {
                    // No algorithm is given in the spec for these
                    // ("Session Styles" only) — keep the scale-degree
                    // mapping. High Key is applied inside
                    // melody_degree_map for this path.
                    std::array<int, 12> scale_degs{};
                    int scale_cnt;
                    get_scale_degrees(nr.ntt, dst_root, scale_degs, scale_cnt);
                    pitch = melody_degree_map(pitch, src_root, src_type,
                                              dst_root, dst_type,
                                              scale_degs.data(), scale_cnt,
                                              nr.bass_on, nr.high_key);
                    break;
                }
                default:
                    pitch = root_shift(pitch, src_root, dst_root);
                    break;
            }
            // High Key for Bypass and the minor variants: shifts the
            // whole pattern (simple root shift). For Melody and the
            // Natural/Dorian path, high_key is already applied inside
            // melody_degree_map.
            if (nr.ntt == prs::Ntt::Bypass ||
                nr.ntt == prs::Ntt::MelodicMinor ||
                nr.ntt == prs::Ntt::MelodicMinor5 ||
                nr.ntt == prs::Ntt::HarmonicMinor ||
                nr.ntt == prs::Ntt::HarmonicMinor5) {
                pitch = apply_high_key_simple(pitch, dst_root, src_pitch, nr.high_key);
            }
            return clamp(pitch, nr.note_low, nr.note_high);
        }

        case prs::Ntr::Guitar:
            // §5.2.5.1: Guitar NTR maps source notes to guitar strings.
            // NTT determines the playing style:
            //   Bypass (0x00) = All-Purpose (mixed stroke/arpeggio)
            //   Melody (0x01) = Stroke (chordal)
            //   Chord  (0x02) = Arpeggio (fingerpicking)
            // All three use the same string mapping; the difference is
            // in how notes are sequenced (handled by the player, not here).
            pitch = guitar_transpose(src_pitch, dst_root, dst_type, dst, nr.bass_on);
            return clamp(pitch, nr.note_low, nr.note_high);

        default:
            return src_pitch;
    }
}

// =========================================================
//  note_arpeggio — for Guitar + Arpeggio NTT
//
//  Uses guitar string mapping to determine which string the note
//  plays on, then finds the nearest chord tone on that string.
//  Preserves melodic contour (ascending/descending direction).
// =========================================================
int note_arpeggio(int src_pitch, int prev_src_pitch, int prev_out_pitch,
                  const prs::NoteRange& nr, const ChordCtx& dst) noexcept {
    if (src_pitch < 0 || src_pitch > 127) return src_pitch;
    int dst_root = (dst.root >= 0) ? dst.root : 0;
    int chord_type = (dst.type >= 0) ? dst.type : 0;

    // Get the base pitch from guitar string mapping
    int base = guitar_transpose(src_pitch, dst_root, chord_type, dst, nr.bass_on);

    int candidate;
    if (prev_out_pitch < 0) {
        // First note of phrase — just use the guitar-mapped pitch
        candidate = base;
    } else {
        int src_dir = src_pitch - prev_src_pitch;
        candidate = base;
        // Preserve melodic contour: if ascending, candidate should be
        // >= previous output; if descending, <= previous output.
        if (!((src_dir > 0 && candidate >= prev_out_pitch) ||
              (src_dir < 0 && candidate <= prev_out_pitch) ||
              src_dir == 0)) {
            int corrected = candidate + (src_dir > 0 ? 12 : -12);
            if (corrected >= 0 && corrected <= 127)
                candidate = corrected;
        }
    }
    return clamp(candidate, nr.note_low, nr.note_high);
}

// =========================================================
//  retrigger — RTR handling for held notes
//
//  Per the migration guide (§4.2), RtrAction::Shift is never returned
//  — PitchShift and PitchShiftRoot both collapse to Retrigger (note-off
//  + note-on at new pitch). This simplifies the caller: it only needs
//  to handle Stop, Retrigger, and None.
//
//  Per the migration guide (§9), retrigger() now uses dst.bass for root
//  targeting when bass_on is true (slash-chord aware).
// =========================================================
RtrResult retrigger(int held_pitch, const prs::NoteRange& nr,
                    const ChordCtx& src, const ChordCtx& dst,
                    bool src_was_transposed) noexcept {
    RtrResult res;
    res.action    = RtrAction::None;
    res.new_pitch = held_pitch;

    int dst_root = (dst.root >= 0) ? dst.root : 0;
    int dst_type = (dst.type >= 0) ? dst.type : 0;
    // For bass_on channels, target the slash-chord bass note instead of root.
    int target_root = (nr.bass_on && dst.bass >= 0) ? dst.bass : dst_root;
    // Chord the held note currently "belongs" to.
    const int src_root_raw = (src.root >= 0) ? src.root : dst_root;
    const int src_root = (nr.bass_on && src.bass >= 0) ? src.bass : src_root_raw;
    const int src_type = (src.type >= 0 && src.type <= 34) ? src.type : 0;

    switch (nr.rtr) {
        case prs::Rtr::Stop:
            res.action    = RtrAction::Stop;
            res.new_pitch = -1;
            break;
        case prs::Rtr::PitchShift:
            // Collapse Shift → Retrigger
            res.action    = RtrAction::Retrigger;
            // Map from the PREVIOUS chord to the new one. High Key is
            // disabled inside melody_degree_map (127) and applied here as
            // a *delta*: held_pitch already carries the previous chord's
            // High Key drop, so we only add the difference. This stops the
            // -12 from compounding on every chord change.
            res.new_pitch = melody_degree_map(held_pitch, src_root, src_type,
                                              target_root, dst_type,
                                              nullptr, 0, nr.bass_on, 127);
            {
                const int hk = static_cast<int>(nr.high_key);
                const int pre  = (src_was_transposed && src_root_raw > hk) ? -12 : 0;
                const int post = (dst_root > hk) ? -12 : 0;
                res.new_pitch += post - pre;
            }
            res.new_pitch = clamp(res.new_pitch, nr.note_low, nr.note_high);
            break;
        case prs::Rtr::PitchShiftRoot:
            // Collapse Shift → Retrigger
            res.action    = RtrAction::Retrigger;
            res.new_pitch = nearest_pitch_class(held_pitch, target_root);
            res.new_pitch = clamp(res.new_pitch, nr.note_low, nr.note_high);
            break;
        case prs::Rtr::Retrigger:
            res.action    = RtrAction::Retrigger;
            res.new_pitch = nearest_chord_tone(held_pitch, target_root, dst_type);
            res.new_pitch = clamp(res.new_pitch, nr.note_low, nr.note_high);
            break;
        case prs::Rtr::RetriggerRoot:
            res.action    = RtrAction::Retrigger;
            res.new_pitch = nearest_pitch_class(held_pitch, target_root);
            res.new_pitch = clamp(res.new_pitch, nr.note_low, nr.note_high);
            break;
        case prs::Rtr::NoteGenerator:
            res.action    = RtrAction::Retrigger;
            res.new_pitch = nearest_chord_tone(held_pitch, target_root, dst_type);
            res.new_pitch = clamp(res.new_pitch, nr.note_low, nr.note_high);
            break;
        default:
            break;
    }
    return res;
}

// =========================================================
//  fit_phrase — phrase-level chord-voicing fit
//
//  Tries all N! permutations (for N ≤ kMaxChordPhraseNotes) and picks
//  the one with the lowest total voice-leading cost. Falls back to
//  greedy per-note note() for larger phrases.
// =========================================================
int fit_phrase(
    std::span<const PhraseEvent> src_events,
    std::span<int> out_pitches,
    const prs::NoteRange& nr,
    const ChordCtx& src,
    const ChordCtx& dst) noexcept {

    const int n = static_cast<int>(src_events.size());
    if (n == 0) return 0;
    if (static_cast<int>(out_pitches.size()) < n) return 0;

    // Greedy fallback for phrases larger than the permutation cap.
    if (n > kMaxChordPhraseNotes) {
        for (int i = 0; i < n; ++i) {
            out_pitches[static_cast<std::size_t>(i)] =
                note(src_events[static_cast<std::size_t>(i)].pitch, nr, src, dst);
        }
        return n;
    }

    // Compute per-note candidate output pitches.
    std::array<int, 6> candidates{};
    for (int i = 0; i < n; ++i) {
        candidates[static_cast<std::size_t>(i)] =
            note(src_events[static_cast<std::size_t>(i)].pitch, nr, src, dst);
    }

    // For N ≤ 2, just use the greedy result (no permutation search needed).
    if (n <= 2) {
        for (int i = 0; i < n; ++i) {
            out_pitches[static_cast<std::size_t>(i)] = candidates[static_cast<std::size_t>(i)];
        }
        return n;
    }

    // Permutation search: try all N! orderings and pick the one with
    // the lowest total voice-leading cost = sum of |src_pitch - out_pitch|.
    std::array<int, 6> perm{};
    for (int i = 0; i < n; ++i) perm[static_cast<std::size_t>(i)] = i;

    std::array<int, 6> best_perm{};
    int best_cost = INT_MAX;

    do {
        int cost = 0;
        for (int i = 0; i < n; ++i) {
            int src_p = src_events[static_cast<std::size_t>(i)].pitch;
            int out_p = candidates[static_cast<std::size_t>(perm[static_cast<std::size_t>(i)])];
            cost += std::abs(src_p - out_p);
        }
        if (cost < best_cost) {
            best_cost = cost;
            best_perm = perm;
        }
    } while (std::next_permutation(perm.begin(), perm.begin() + n));

    // Write the best permutation to output.
    for (int i = 0; i < n; ++i) {
        out_pitches[static_cast<std::size_t>(i)] =
            candidates[static_cast<std::size_t>(best_perm[static_cast<std::size_t>(i)])];
    }
    return n;
}

} // namespace transpose