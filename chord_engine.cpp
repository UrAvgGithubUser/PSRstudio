// chord_engine.cpp — Real-time MIDI chord detection engine.
#include "chord_engine.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <span>
#include <string>

namespace chord {

// =========================================================
//  Constants
// =========================================================
const std::array<std::string_view, 12> kNoteNames = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// =========================================================
//  Single Finger layout helpers (Yamaha fingering convention)
//
//  The aux keys are the nearest BLACK resp. WHITE key to the LEFT of
//  the root in keyboard-layout terms — the semitone distance VARIES by
//  root (−1 for D/E/G/A/B, −2 for C/C#/D#/F#/G#/A#, −3 for F#), so a
//  fixed offset table cannot express the rule.
// =========================================================

// Nearest BLACK key strictly to the LEFT of pc (pitch class).
// Black pitch classes: 1, 3, 6, 8, 10.
[[nodiscard]] int nearest_black_left(int pc) noexcept {
    for (int step = 1; step < 12; ++step) {
        const int p = ((pc - step) % 12 + 12) % 12;
        if (p == 1 || p == 3 || p == 6 || p == 8 || p == 10) return p;
    }
    return pc;  // unreachable: 12 keys always contain a black key
}

// Nearest WHITE key strictly to the LEFT of pc (pitch class).
// White pitch classes: 0, 2, 4, 5, 7, 9, 11.
[[nodiscard]] int nearest_white_left(int pc) noexcept {
    for (int step = 1; step < 12; ++step) {
        const int p = ((pc - step) % 12 + 12) % 12;
        if (p == 0 || p == 2 || p == 4 || p == 5 ||
            p == 7 || p == 9 || p == 11) return p;
    }
    return pc;  // unreachable
}

// Global active chord — read by the playback thread without touching the ring.
ActiveChord g_active_chord;

// =========================================================
//  Chord template table
//
//  Matching is REQUIRED-TONE SUBSET matching (Yamaha/Cadenza style):
//    - every held pitch class must be a chord tone of the template
//      (omitted tones are allowed — a 5th-less voicing still matches),
//    - every "required" interval of the template must be held
//      (the tones that define the quality).
//
//  `required` is a 12-bit mask of intervals (bit i = interval i) that
//  MUST be present relative to the root, excluding the root itself.
// =========================================================
namespace {

struct ChordTemplate {
    std::array<u8, 6>    ivs{};
    int                  niv;
    ChordType            type;
    std::string_view     suffix;
    int                  weight;
    std::uint16_t        required;   // interval bits that must be held
};

constexpr std::uint16_t iv_bit(int interval) noexcept {
    return static_cast<std::uint16_t>(1u << interval);
}

constexpr std::array<ChordTemplate, 37> kChordTableFull = {{
    // 6 pitch classes (5 intervals)
    { {{2, 3,  5,  7, 10, 0}}, 5, ChordType::Min11,   "m11",     10,
      static_cast<std::uint16_t>(iv_bit(3) | iv_bit(5) | iv_bit(10)) },
    { {{2, 4,  6,  7, 10, 0}}, 5, ChordType::Dom7s11, "7#11",     9,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(6) | iv_bit(7) | iv_bit(10)) },
    { {{2, 4,  6,  7, 11, 0}}, 5, ChordType::Maj7s11, "maj7#11",  9,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(6) | iv_bit(11)) },
    // 5 pitch classes (4 intervals)
    { {{4, 5,  7, 10, 0, 0}}, 4, ChordType::Dom11,   "7(11)",   7,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(5) | iv_bit(7)) },
    { {{2, 4,  7, 11, 0, 0}}, 4, ChordType::Maj9,    "maj9",    9,
      static_cast<std::uint16_t>(iv_bit(2) | iv_bit(4) | iv_bit(11)) },
    { {{2, 3,  7, 10, 0, 0}}, 4, ChordType::Min9,    "m9",      9,
      static_cast<std::uint16_t>(iv_bit(2) | iv_bit(3) | iv_bit(10)) },
    { {{1, 4,  7, 10, 0, 0}}, 4, ChordType::Dom7b9,  "7b9",     8,
      static_cast<std::uint16_t>(iv_bit(1) | iv_bit(4) | iv_bit(10)) },
    { {{3, 4,  7, 10, 0, 0}}, 4, ChordType::Dom7s9,  "7#9",     8,
      static_cast<std::uint16_t>(iv_bit(3) | iv_bit(4) | iv_bit(10)) },
    { {{2, 4,  7, 10, 0, 0}}, 4, ChordType::Dom9,    "9",       8,
      static_cast<std::uint16_t>(iv_bit(2) | iv_bit(4) | iv_bit(10)) },
    { {{4, 7,  9, 10, 0, 0}}, 4, ChordType::Dom13,   "13",      7,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(9) | iv_bit(10)) },
    { {{4, 7,  8, 10, 0, 0}}, 4, ChordType::Dom7b13, "7b13",    7,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(8) | iv_bit(10)) },
    { {{2, 4,  7,  9, 0, 0}}, 4, ChordType::Maj69,   "6/9",     7,
      static_cast<std::uint16_t>(iv_bit(2) | iv_bit(4) | iv_bit(9)) },
    // 4 pitch classes (3 intervals)
    { {{2,  5, 10, 0, 0, 0}}, 3, ChordType::Dom11,   "7(11)",   7,
      static_cast<std::uint16_t>(iv_bit(2) | iv_bit(5) | iv_bit(10)) },
    { {{3,  7, 11, 0, 0, 0}}, 3, ChordType::MinMaj7, "m(maj7)", 7,
      static_cast<std::uint16_t>(iv_bit(3) | iv_bit(11)) },
    { {{4,  7, 11, 0, 0, 0}}, 3, ChordType::Maj7,    "maj7",    7,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(11)) },
    { {{4,  7, 10, 0, 0, 0}}, 3, ChordType::Dom7,    "7",       7,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(10)) },
    { {{3,  7, 10, 0, 0, 0}}, 3, ChordType::Min7,    "m7",      7,
      static_cast<std::uint16_t>(iv_bit(3) | iv_bit(10)) },
    { {{3,  6, 10, 0, 0, 0}}, 3, ChordType::Min7b5,  "m7b5",    8,
      static_cast<std::uint16_t>(iv_bit(3) | iv_bit(6) | iv_bit(10)) },
    { {{3,  6,  9, 0, 0, 0}}, 3, ChordType::Dim7,    "dim7",    7,
      static_cast<std::uint16_t>(iv_bit(3) | iv_bit(6) | iv_bit(9)) },
    { {{4,  6, 10, 0, 0, 0}}, 3, ChordType::Dom7b5,  "7b5",     7,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(6) | iv_bit(10)) },
    { {{4,  8, 11, 0, 0, 0}}, 3, ChordType::Maj7Aug, "maj7aug", 7,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(8) | iv_bit(11)) },
    { {{4,  8, 10, 0, 0, 0}}, 3, ChordType::Dom7Aug, "7aug",    7,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(8) | iv_bit(10)) },
    { {{5,  7, 10, 0, 0, 0}}, 3, ChordType::Sus4_7,  "7sus4",   6,
      static_cast<std::uint16_t>(iv_bit(5) | iv_bit(10)) },
    { {{2,  4,  7, 0, 0, 0}}, 3, ChordType::Add9,    "add9",    6,
      static_cast<std::uint16_t>(iv_bit(2) | iv_bit(4)) },
    { {{2,  3,  7, 0, 0, 0}}, 3, ChordType::MinAdd9, "m(add9)", 6,
      static_cast<std::uint16_t>(iv_bit(2) | iv_bit(3)) },
    { {{4,  7,  9, 0, 0, 0}}, 3, ChordType::Maj6,    "6",       6,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(9)) },
    { {{3,  7,  9, 0, 0, 0}}, 3, ChordType::Min6,    "m6",      6,
      static_cast<std::uint16_t>(iv_bit(3) | iv_bit(9)) },
    { {{3,  6, 11, 0, 0, 0}}, 3, ChordType::MinMaj7b5, "mM7(b5)", 7,
      static_cast<std::uint16_t>(iv_bit(3) | iv_bit(6) | iv_bit(11)) },
    { {{4,  6, 11, 0, 0, 0}}, 3, ChordType::Maj7b5,  "Maj7(b5)", 7,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(6) | iv_bit(11)) },
    // 3 pitch classes (2 intervals) — the MINIMUM in fingered mode.
    // There is deliberately no 1-interval (2-key) row: fingered-mode
    // detection requires >= 3 held keys (see match_root), matching both
    // the old exact-match engine and Cadenza/Cadenza's min-3 rule.
    { {{4, 7, 0, 0, 0, 0}},   2, ChordType::Maj,     "",        5,
      iv_bit(4) },
    { {{3, 7, 0, 0, 0, 0}},   2, ChordType::Min,     "m",       5,
      iv_bit(3) },
    { {{4, 10, 0, 0, 0, 0}},  2, ChordType::Dom7,    "7",       5,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(10)) },
    { {{3, 6, 0, 0, 0, 0}},   2, ChordType::Dim,     "dim",     4,
      static_cast<std::uint16_t>(iv_bit(3) | iv_bit(6)) },
    { {{4, 8, 0, 0, 0, 0}},   2, ChordType::Aug,     "aug",     4,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(8)) },
    { {{5, 7, 0, 0, 0, 0}},   2, ChordType::Sus4,    "sus4",    4,
      iv_bit(5) },
    { {{2, 7, 0, 0, 0, 0}},   2, ChordType::Sus2,    "sus2",    4,
      iv_bit(2) },
    { {{4, 6, 0, 0, 0, 0}},   2, ChordType::Majb5,   "b5",      4,
      static_cast<std::uint16_t>(iv_bit(4) | iv_bit(6)) },
}};

} // anonymous namespace

// =========================================================
//  Timing
// =========================================================
[[nodiscard]] std::uint64_t time_ms() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// =========================================================
//  ChordRing
// =========================================================
bool ChordRing::push(const DetectedChord& c) noexcept {
    const int h    = head_.load(std::memory_order_relaxed);
    const int next = (h + 1) & (kChordRingSize - 1);
    const int t    = tail_.load(std::memory_order_acquire);
    if (next == t) return false;  // full — drop

    slots_[static_cast<std::size_t>(h)] = c;

    // Release: the slot write must be visible before head advances.
    head_.store(next, std::memory_order_release);
    return true;
}

bool ChordRing::pop(DetectedChord& out) noexcept {
    const int t = tail_.load(std::memory_order_relaxed);
    const int h = head_.load(std::memory_order_acquire);
    if (t == h) return false;  // empty

    // Acquire: head read must complete before slot read.
    out = slots_[static_cast<std::size_t>(t)];

    tail_.store((t + 1) & (kChordRingSize - 1), std::memory_order_release);
    return true;
}

// =========================================================
//  Internal helpers
// =========================================================
namespace {

// Single name-building point used by both classifiers.
void format_chord_name(DetectedChord& r, std::string_view suffix) {
    if (r.root < 0 || r.root > 11) {
        r.name = "?";
        return;
    }
    r.name = std::string(kNoteNames[static_cast<std::size_t>(r.root)]);
    if (!suffix.empty()) r.name += suffix;
}

// Build sorted pitch-class array from held notes.
// Returns count of distinct pitch classes. Sets bass_raw to lowest held pitch.
int build_pitch_classes(const std::array<u8, 128>& held,
                        std::array<u8, kMaxChordNotes>& pcs,
                        int& bass_raw) {
    std::array<u8, 12> pc_seen{};
    int count = 0;
    bass_raw = -1;

    for (int n = 0; n < 128; ++n) {
        if (!held[n]) continue;
        if (bass_raw < 0) bass_raw = n;
        const int pc = n % 12;
        if (!pc_seen[pc]) {
            pc_seen[pc] = 1;
            pcs[static_cast<std::size_t>(count)] = static_cast<u8>(pc);
            if (++count == kMaxChordNotes) break;
        }
    }
    // Insertion sort
    for (int i = 1; i < count; ++i) {
        const u8 key = pcs[static_cast<std::size_t>(i)];
        int j = i - 1;
        while (j >= 0 && pcs[static_cast<std::size_t>(j)] > key) {
            pcs[static_cast<std::size_t>(j + 1)] = pcs[static_cast<std::size_t>(j)];
            --j;
        }
        pcs[static_cast<std::size_t>(j + 1)] = key;
    }
    return count;
}

// Try to match a chord with the given root candidate using required-
// tone subset matching: every held pitch class must be a chord tone of
// the template (omitted tones allowed), and every required tone must be
// held. Among matching templates, prefer the higher score
// (weight + 2 for an exact pitch-class-set match), then more intervals.
//
// Returns the best template index into kChordTableFull, or -1 if no
// template matches. *out_score receives the template's base score.
int match_root(const std::array<u8, kMaxChordNotes>& pcs, int npc, int root,
               int* out_score) {
    // 12-bit mask of held intervals relative to the root candidate.
    std::uint16_t played = 0;
    int niv = 0;
    for (int i = 0; i < npc; ++i) {
        const int interval = (pcs[i] - root + 12) % 12;
        if (interval == 0) continue;
        played = static_cast<std::uint16_t>(played | (1u << interval));
        ++niv;
    }

    // Fingered mode requires at least 2 INTERVALS (>= 3 held keys).
    // 1-2 key sets never classify here — they hold the previous chord,
    // exactly like the old exact-match engine (and Cadenza's min-3
    // rule). Without this gate, every passing two-finger moment would
    // publish a thin partial chord and retrigger the band.
    if (niv < 2) return -1;

    int best = -1;
    int best_score = -1;
    int best_niv = -1;
    for (std::size_t t = 0; t < kChordTableFull.size(); ++t) {
        const ChordTemplate& tmpl = kChordTableFull[static_cast<std::size_t>(t)];

        // The template's tone intervals as a 12-bit mask.
        std::uint16_t tones = 0;
        for (int k = 0; k < tmpl.niv; ++k) {
            tones = static_cast<std::uint16_t>(
                tones | (1u << tmpl.ivs[static_cast<std::size_t>(k)]));
        }

        // No held pitch class may fall outside the template's tones.
        if ((played & static_cast<std::uint16_t>(~tones & 0xFFFu)) != 0) continue;
        // Every required ("characteristic") tone must be held.
        if ((played & tmpl.required) != tmpl.required) continue;

        // EXACT MATCHES DOMINATE. A partial (omitted-tone) match must
        // never outrank an exact one, no matter the template weight or
        // the +3 bass bonus added later: D-F-A voiced F-A-D is Dm (exact
        // at root D), never F6 (partial at root F). The exact bonus
        // (+100) dwarfs every weight (<=10) and bonus (+3/+1); among
        // exact matches the weight only ever orders identical tone sets
        // (same chord type), so it is inert here and kept for the
        // cross-root tie path.
        const int score = tmpl.weight + ((played == tones) ? 100 : 0);
        if (score > best_score || (score == best_score && niv > best_niv)) {
            best_score = score;
            best_niv   = niv;
            best       = static_cast<int>(t);
        }
    }
    if (out_score) *out_score = best_score;
    return best;
}

} // anonymous namespace

// =========================================================
//  ChordEngine — publish_active_chord
//  Written after every successful classification so the playback
//  thread can read the current chord without touching the ring.
// =========================================================
void ChordEngine::publish_active_chord(const DetectedChord& d) const {
    // Root + type + bass pitch class are published together in one
    // atomic store. DetectedChord::bass_note is the lowest held MIDI
    // pitch; ChordCtx::bass wants a pitch class (0-11).
    //
    // Slash bass is published ONLY in Fingered On Bass mode, and only
    // when it differs from the chord root. Every other mode treats
    // inversions as voicings: bass = chord root (bass = -1). N.C. never
    // carries a bass.
    int bass_pc = -1;
    if (mode_ == InputMode::FingeredOnBass &&
        d.type != ChordType::NoChord &&
        d.bass_note > 0 && d.bass_note < 255) {
        const int b = d.bass_note % 12;
        if (b != d.root) bass_pc = b;
    }
    g_active_chord.store(d.root, d.type, bass_pc);
}

void ChordEngine::set_mode(InputMode mode) {
    std::lock_guard<std::mutex> lk(mtx_);
    mode_ = mode;
}

// =========================================================
//  ChordEngine — classify (fingered mode)
// =========================================================
void ChordEngine::classify() {
    std::array<u8, kMaxChordNotes> pcs{};
    int bass_raw = -1;
    const int npc = build_pitch_classes(held_, pcs, bass_raw);

    DetectedChord result;
    result.root         = -1;
    result.type         = ChordType::None;
    result.bass_note    = (bass_raw >= 0) ? static_cast<u8>(bass_raw) : u8{0};
    result.timestamp_ms = window_start_;

    result.note_count = 0;
    for (int n = 0; n < 128 && result.note_count < kMaxChordNotes; ++n) {
        if (held_[n]) {
            result.notes[static_cast<std::size_t>(result.note_count++)] = static_cast<u8>(n);
        }
    }

    if (npc == 0) {
        result.name = "(none)";
        current_ = result;
        return;
    }

    if (npc == 1) {
        // Fingered-class modes require >= 3 keys: fewer detects no chord
        // and the previous chord keeps sounding. (SingleFinger/MultiFinger
        // never reach here — 1-note sets always succeed in
        // classify_single_finger.)
        return;
    }

    // vArranger parity: a 4-note chromatic cluster is an explicit N.C. —
    // publish it so playback stops chord-following (new notes play the
    // style in its home key) and the display shows "N.C.".
    if (npc == 4 &&
        (pcs[1] - pcs[0] + 12) % 12 == 1 &&
        (pcs[2] - pcs[1] + 12) % 12 == 1 &&
        (pcs[3] - pcs[2] + 12) % 12 == 1) {
        result.root = (bass_raw >= 0) ? bass_raw % 12 : 0;
        result.type = ChordType::NoChord;
        result.name = "N.C.";
        previous_ = current_;
        current_  = result;
        if (ring_) ring_->push(result);
        publish_active_chord(result);
        published_       = result;
        published_valid_ = true;
        return;
    }

    // Release-stability (Cadenza-style downgrade prevention): while a
    // held chord is released finger-by-finger, the shrinking pitch-class
    // set matches weaker templates (power/sus/partial). If the current
    // set is a strict subset of the last PUBLISHED chord's set, keep the
    // published chord: current_ is unchanged (it still holds it) and
    // g_active_chord already carries it. Building a chord UP (adding
    // notes) is never a subset, so new chords are unaffected.
    if (published_valid_ && published_.type != ChordType::None) {
        std::uint16_t pub_mask = 0;
        for (int i = 0; i < published_.note_count && i < kMaxChordNotes; ++i) {
            pub_mask = static_cast<std::uint16_t>(
                pub_mask | (1u << (published_.notes[static_cast<std::size_t>(i)] % 12)));
        }
        std::uint16_t cur_mask = 0;
        for (int i = 0; i < npc; ++i) {
            cur_mask = static_cast<std::uint16_t>(cur_mask | (1u << pcs[static_cast<std::size_t>(i)]));
        }
        if ((cur_mask & static_cast<std::uint16_t>(~pub_mask & 0xFFFu)) == 0 &&
            std::popcount(cur_mask) < std::popcount(pub_mask)) {
            return;  // keep the published chord — no downgrade
        }
    }

    // Try every pitch class as root candidate
    int best_tmpl    = -1;
    int best_root    = -1;
    int best_weight  = -1;
    int best_matches = 0;

    for (int ri = 0; ri < npc; ++ri) {
        const int root = pcs[static_cast<std::size_t>(ri)];
        int tmpl_score = 0;
        const int ti   = match_root(pcs, npc, root, &tmpl_score);
        if (ti < 0) continue;

        int weight = tmpl_score;

        // Tie-break 1: bass note as root
        if (bass_raw >= 0 && root == bass_raw % 12)
            weight += 3;

        // Tie-break 2: same root as previous chord
        if (previous_.root == root && previous_.type != ChordType::None)
            weight += 1;

        // Tie-break 3: most intervals matched
        const int niv = kChordTableFull[static_cast<std::size_t>(ti)].niv + 1;
        if (weight > best_weight ||
            (weight == best_weight && niv > best_matches)) {
            best_weight  = weight;
            best_matches = niv;
            best_tmpl    = ti;
            best_root    = root;
        }
    }

    // Fingered On Bass: if the FULL set failed to match, re-match WITHOUT
    // the lowest key. A chord-only match (>= 3 distinct pitch classes)
    // whose root differs from the bass key rescues the chord — e.g.
    // C-E-G played over a foreign F# bass becomes "C/F#". A successful
    // FULL-set match is never overridden: chord-only re-matching can
    // otherwise misread voicings (C7 voiced C-E-G-Bb would become
    // Edim/C, since E-G-Bb alone is an Edim).
    if (best_tmpl < 0 && mode_ == InputMode::FingeredOnBass &&
        npc >= 2 && bass_raw >= 0) {
        std::array<u8, kMaxChordNotes> pcs2{};
        int npc2 = 0;
        for (int n = 0; n < 128 && npc2 < kMaxChordNotes; ++n) {
            if (!held_[n] || n == bass_raw) continue;
            const int pc = n % 12;
            bool dup = false;
            for (int k = 0; k < npc2; ++k) {
                if (pcs2[static_cast<std::size_t>(k)] == pc) { dup = true; break; }
            }
            if (!dup) pcs2[static_cast<std::size_t>(npc2++)] = static_cast<u8>(pc);
        }
        if (npc2 >= 3) {
            const int co_bass = pcs2[0];   // ascending scan -> lowest first
            int co_tmpl = -1, co_root = -1, co_best = -1, co_niv_best = 0;
            for (int root = 0; root < 12; ++root) {
                int score = 0;
                const int ti = match_root(pcs2, npc2, root, &score);
                if (ti < 0) continue;
                if (root == co_bass) score += 3;   // bass-priority tie-break
                const int niv2 = kChordTableFull[static_cast<std::size_t>(ti)].niv + 1;
                if (score > co_best || (score == co_best && niv2 > co_niv_best)) {
                    co_best = score;
                    co_niv_best = niv2;
                    co_tmpl = ti;
                    co_root = root;
                }
            }
            if (co_tmpl >= 0 && co_root != bass_raw % 12) {
                best_tmpl = co_tmpl;
                best_root = co_root;
            }
        }
    }

    if (best_tmpl >= 0) {
        const ChordTemplate& tmpl = kChordTableFull[static_cast<std::size_t>(best_tmpl)];
        result.root = best_root;
        result.type = tmpl.type;
        format_chord_name(result, tmpl.suffix);
    } else {
        // No clean match — report as unknown
        result.root = (bass_raw >= 0) ? bass_raw % 12 : pcs[0];
        result.type = ChordType::None;
        std::string tmp;
        for (int i = 0; i < npc; ++i) {
            if (i) tmp += "/";
            tmp += kNoteNames[pcs[static_cast<std::size_t>(i)]];
        }
        result.name = std::format("({}?)", tmp);
    }

    previous_ = current_;
    current_  = result;
    if (ring_) ring_->push(result);
    // An unmatched note cluster must NOT be published: it would reach the
    // player as {root=bass, type=None}, which chord_type_to_casm() maps
    // to plain Major and retriggers every held note to a bogus chord.
    // The previous (valid) chord stays active instead.
    if (best_tmpl >= 0) {
        publish_active_chord(result);
        published_       = result;
        published_valid_ = true;
    }
}

// (Fingered On Bass chord-only rescue lives inside classify(); see the
// block after the root-candidate loop.)

// =========================================================
//  ChordEngine — classify_single_finger
//
//  Yamaha "Single Finger" fingering (layout-based — see the
//  nearest_black_left / nearest_white_left helpers):
//    root only                          -> Major
//    root + nearest BLACK key left      -> minor
//    root + nearest WHITE key left      -> seventh
//    root + nearest BLACK and WHITE left -> minor 7th
//
//  The root is the HIGHEST held key (aux keys are always to its left).
//  Octave doublings of the root are tolerated (they don't change the
//  chord). STRICT: any other pattern returns false — the caller holds
//  the previous chord instead of falling back to full matching.
// =========================================================
bool ChordEngine::classify_single_finger() {
    std::array<u8, 8> held_notes{};
    int n_held = 0;
    for (int n = 0; n < 128 && n_held < 8; ++n) {
        if (held_[n]) held_notes[static_cast<std::size_t>(n_held++)] = static_cast<u8>(n);
    }

    if (n_held == 0) return false;

    const u8 main_note = held_notes[static_cast<std::size_t>(n_held - 1)];
    const int root_pc  = main_note % 12;
    const int black_left = nearest_black_left(root_pc);
    const int white_left = nearest_white_left(root_pc);

    bool has_black = false;
    bool has_white = false;
    for (int i = 0; i < n_held - 1; ++i) {
        const int aux_pc = held_notes[static_cast<std::size_t>(i)] % 12;
        if (aux_pc == root_pc)    continue;  // octave-doubled root: tolerated
        if (aux_pc == black_left) { has_black = true; continue; }
        if (aux_pc == white_left) { has_white = true; continue; }
        return false;                        // invalid pattern (strict)
    }

    DetectedChord result;
    result.bass_note    = held_notes[0];
    result.timestamp_ms = window_start_;
    result.note_count   = n_held;
    std::memcpy(result.notes.data(), held_notes.data(),
                static_cast<std::size_t>(n_held));
    result.root = root_pc;

    if (has_black && has_white) {
        result.type = ChordType::Min7;
        format_chord_name(result, "m7");
    } else if (has_black) {
        result.type = ChordType::Min;
        format_chord_name(result, "m");
    } else if (has_white) {
        result.type = ChordType::Dom7;
        format_chord_name(result, "7");
    } else {
        result.type = ChordType::Maj;
        format_chord_name(result, "");
    }

    previous_ = current_;
    current_  = result;
    if (ring_) ring_->push(result);
    publish_active_chord(result);
    published_       = result;
    published_valid_ = true;
    return true;
}

// =========================================================
//  ChordEngine — API
// =========================================================
void ChordEngine::init(InputMode mode, std::uint16_t channel_mask, ChordRing* ring) {
    std::lock_guard<std::mutex> lk(mtx_);
    init_unlocked(mode, channel_mask, ring);
}

void ChordEngine::init_unlocked(InputMode mode, std::uint16_t channel_mask, ChordRing* ring) {
    held_.fill(0);
    held_count_   = 0;
    bass_note_    = 255;
    window_start_ = 0;
    window_active_ = false;
    current_      = DetectedChord{};
    previous_     = DetectedChord{};
    published_    = DetectedChord{};
    current_.root  = -1;
    previous_.root = -1;
    published_valid_ = false;
    mode_         = mode;
    channel_mask_ = channel_mask;
    ring_         = ring;
}

void ChordEngine::note_on(u8 channel, u8 note, u8 vel) {
    (void)vel;

    std::lock_guard<std::mutex> lk(mtx_);

    // Channel filter
    if (!(channel_mask_ & (1u << channel))) return;

    // Split-point filter: notes at or above the split point are ignored.
    // FullKeyboard mode uses the whole keyboard — no split filter.
    if (mode_ != InputMode::FullKeyboard &&
        note >= g_split_point.load(std::memory_order_relaxed)) return;

    if (held_[note]) return;
    held_[note] = 1;
    ++held_count_;

    if (bass_note_ == 255 || note < bass_note_)
        bass_note_ = note;

    if (!window_active_) {
        // window_start is set here, on the FIRST note of a new chord, and
        // is NEVER reset on subsequent notes within the same window.
        // Resetting it on every note-on would prevent the 50ms window
        // from expiring during fast strumming.
        window_start_  = time_ms();
        window_active_ = true;
    }
    // No else — do not touch window_start_ once the window is active
}

void ChordEngine::note_off(u8 channel, u8 note) {
    std::lock_guard<std::mutex> lk(mtx_);

    // Channel filter
    if (!(channel_mask_ & (1u << channel))) return;

    // Split-point filter (must match note_on — otherwise an off for a
    // note ≥ split point would corrupt held[]). FullKeyboard bypasses
    // it exactly like note_on.
    if (mode_ != InputMode::FullKeyboard &&
        note >= g_split_point.load(std::memory_order_relaxed)) return;

    if (!held_[note]) return;
    held_[note] = 0;
    --held_count_;

    if (note == bass_note_) {
        bass_note_ = 255;
        for (int n = static_cast<int>(note) + 1; n < 128; ++n) {
            if (held_[n]) {
                bass_note_ = static_cast<u8>(n);
                break;
            }
        }
        // BUG3 fix: the original set bass_note = 255 again here after
        // all notes released. The rescan loop above already handles this
        // (bass_note stays 255 if no remaining notes), so the second
        // reset was redundant and misleading. Removed.
    }

    // If all notes released, flush immediately
    if (held_count_ == 0 && window_active_)
        flush_unlocked();
}

void ChordEngine::flush() {
    std::lock_guard<std::mutex> lk(mtx_);
    flush_unlocked();
}

void ChordEngine::flush_unlocked() {
    if (!window_active_) return;
    window_active_ = false;

    if (mode_ == InputMode::SingleFinger || mode_ == InputMode::MultiFinger) {
        if (classify_single_finger()) return;
        if (mode_ == InputMode::SingleFinger) {
            // Strict Single Finger: unrecognized patterns hold the
            // previous chord — no fallback to full matching. (Multi
            // Finger falls through to classify() instead.)
            return;
        }
    }
    classify();   // MultiFinger fallback + Fingered-class modes
}

void ChordEngine::tick() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!window_active_) return;
    if (time_ms() - window_start_ >= static_cast<std::uint64_t>(kWindowMs))
        flush_unlocked();
}

void ChordEngine::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    const InputMode     mode         = mode_;
    const std::uint16_t channel_mask = channel_mask_;
    ChordRing* const    ring         = ring_;

    init_unlocked(mode, channel_mask, ring);

    g_active_chord.store(-1, ChordType::None);
}

} // namespace chord