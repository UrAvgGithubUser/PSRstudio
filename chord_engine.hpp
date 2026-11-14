// chord_engine.hpp — Real-time MIDI chord detection engine.
//
// Detects chords from incoming MIDI note-on events in real time.
// Supports both fingered and single-finger (PSR-style) input modes.
//
// Key features:
//   - Lock-free ActiveChord (atomic packed int) for the playback thread
//   - SPSC ring buffer for chord events between MIDI and UI threads
//   - Configurable split point (notes at or above are ignored)
//   - 50 ms collection window before classification
//   - Split point is an inline std::atomic<int> so any module can
//     read/write it thread-safely:
//
//       #include "chord_engine.hpp"
//       if (note < chord::g_split_point.load()) { ... }
//       chord::g_split_point.store(48);  // C2
//
//   This lets the UI settings page change the split point at runtime
//   without the chord engine needing a setter method.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace chord {

// =========================================================
//  Type aliases
// =========================================================
using u8  = std::uint8_t;
using u32 = std::uint32_t;

// =========================================================
//  Split point (global, thread-safe)
//
//  Notes with MIDI pitch >= g_split_point are ignored by the chord
//  detector. Default is C2 (Yamaha convention: MIDI 48).
//
//  - Read:   int sp = g_split_point.load(std::memory_order_relaxed);
//  - Write:  g_split_point.store(50, std::memory_order_relaxed);
//
//  Relaxed ordering is safe: a stale read just means one missed note,
//  and the next note-on will see the new value.
// =========================================================
inline std::atomic<int> g_split_point{48};  // C2 (Yamaha)

// =========================================================
//  ChordType enum
// =========================================================
enum class ChordType : u8 {
    None    = 0,
    Maj,            // major            {4,7}
    Min,            // minor            {3,7}
    Dom7,           // dominant 7th     {4,7,10}
    Maj7,           // major 7th        {4,7,11}
    Min7,           // minor 7th        {3,7,10}
    Dim,            // diminished       {3,6}
    Dim7,           // diminished 7th   {3,6,9}
    Aug,            // augmented        {4,8}
    Sus4,           // suspended 4th    {5,7}
    Sus2,           // suspended 2nd    {2,7}
    Maj6,           // major 6th        {4,7,9}
    Min6,           // minor 6th        {3,7,9}
    Dom9,           // dominant 9th     {4,7,10,2}
    MinMaj7,        // minor-major 7th  {3,7,11}
    Add9,           // add9             {4,7,2}
    Sus4_7,         // 7sus4            {5,7,10}
    Min9,           // minor 9th        {3,7,10,2}
    Maj9,           // major 9th        {4,7,11,2}
    // --- Extended vocabulary (Yamaha/Cadenza set) ---
    Min7b5,         // half-diminished  {3,6,10}
    Dom7b5,         // 7 flat 5         {4,6,10}
    Dom7s11,        // 7 sharp 11       {2,4,6,7,10}
    Dom7b9,         // 7 flat 9         {1,4,7,10}
    Dom7s9,         // 7 sharp 9        {3,4,7,10}
    Dom7b13,        // 7 flat 13        {4,7,8,10}
    Dom7Aug,        // augmented 7th    {4,8,10}
    Maj7s11,        // maj7 sharp 11    {2,4,6,7,11}
    Maj7Aug,        // maj7 augmented   {4,8,11}
    Maj69,          // 6/9              {2,4,7,9}
    MinAdd9,        // minor add 9      {2,3,7}
    Min11,          // minor 11th       {2,3,5,7,10}
    Dom13,          // dominant 13th    {4,7,9,10}
    Power,          // root + 5th       {7}
    // --- vArranger-parity extensions ---
    MinMaj7b5,      // mM7(b5)          {3,6,11}
    Maj7b5,         // Maj7(b5)         {4,6,11}
    Majb5,          // b5               {4,6}
    Dom11,          // 7(11)            {4,5,7,10} or {2,5,10}
    NoChord,        // published "N.C." (e.g. 4-note chromatic cluster)
    Count
};

// =========================================================
//  Note names (C .. B)
// =========================================================
extern const std::array<std::string_view, 12> kNoteNames;

// =========================================================
//  Constants
// =========================================================
inline constexpr int kMaxChordNotes = 8;
inline constexpr int kChordRingSize = 64;  // must be power of 2
inline constexpr int kWindowMs      = 50;  // chord collection window

// =========================================================
//  DetectedChord — result of one classification pass
// =========================================================
struct DetectedChord {
    int                       root = -1;        // 0-11, -1 = none
    ChordType                 type = ChordType::None;
    std::string               name;             // e.g. "Cmaj7", "F#m"
    int                       note_count = 0;
    std::array<u8, kMaxChordNotes> notes{};     // raw MIDI pitches held
    u8                        bass_note = 0;    // lowest held pitch
    std::uint64_t             timestamp_ms = 0; // time of first note in window
};

// =========================================================
//  ActiveChord — lock-free current chord for the playback thread
//
//  Root and type are packed into ONE atomic int so a reader can never
//  see a new root combined with an old type (the torn read that two
//  separate atomics allowed).
//
//    packed < 0          -> no chord (root = -1)
//    otherwise           -> root in bits 0-7, ChordType in bits 8-15,
//                           bass pitch class in bits 16-23 (0xFF = none)
// =========================================================
class ActiveChord {
public:
    // bass: played bass pitch class (0-11) for slash chords, or -1 when
    // the bass equals the root / is unavailable.
    void store(int root, ChordType type, int bass = -1) noexcept {
        const int bass_field = (bass >= 0) ? (bass & 0x0F) : 0xFF;
        const int v = (root < 0)
            ? -1
            : ((root & 0xFF) | (static_cast<int>(type) << 8)
               | (bass_field << 16));
        packed_.store(v, std::memory_order_release);
    }

    // Single atomic load. Returns false (and root=-1, type=None) if no chord.
    [[nodiscard]] bool load(int& root, ChordType& type) const noexcept {
        int bass;
        return load(root, type, bass);
    }

    // Same, but also receives the bass pitch class (0-11, or -1).
    [[nodiscard]] bool load(int& root, ChordType& type, int& bass) const noexcept {
        const int v = packed_.load(std::memory_order_acquire);
        if (v < 0) { root = -1; type = ChordType::None; bass = -1; return false; }
        root = v & 0xFF;
        type = static_cast<ChordType>((v >> 8) & 0xFF);
        const int b = (v >> 16) & 0xFF;
        bass = (b == 0xFF) ? -1 : b;
        return true;
    }

    [[nodiscard]] int root() const noexcept {
        int r; ChordType t; (void)load(r, t); return r;   // false == root=-1, returned as-is
    }
    [[nodiscard]] ChordType type() const noexcept {
        int r; ChordType t; (void)load(r, t); return t;   // false == None, returned as-is
    }

private:
    std::atomic<int> packed_{-1};
};

extern ActiveChord g_active_chord;  // defined in chord_engine.cpp

// =========================================================
//  ChordRing — lock-free SPSC ring buffer
//  Producer = chord engine (MIDI callback thread)
//  Consumer = UI/player thread
// =========================================================
class ChordRing {
public:
    // Returns true on success, false if ring is full (chord dropped).
    bool push(const DetectedChord& c) noexcept;

    // Returns true and fills *out on success, false if empty.
    bool pop(DetectedChord& out) noexcept;

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    std::array<DetectedChord, kChordRingSize> slots_{};
    std::atomic<int> head_{0};  // written by producer
    std::atomic<int> tail_{0};  // written by consumer
};

// =========================================================
//  Input mode
//
//  Five chord recognition modes (user-facing fingering spec):
//    SingleFinger   — Yamaha layout fingering, STRICT: root only = Maj;
//                     root + nearest BLACK key left = min; root +
//                     nearest WHITE key left = 7th; root + both = m7.
//                     Unrecognized patterns hold the previous chord.
//    MultiFinger    — SingleFinger rules first, full Fingered matching
//                     as fallback.
//    Fingered       — >= 3 keys, full 32-type subset matching. < 3 keys
//                     detects nothing (previous chord keeps sounding).
//                     All inversions are voicings: bass = chord root.
//    FingeredOnBass — Same as Fingered, but the lowest held key is the
//                     published bass when it differs from the root.
//                     Only mode with slash detection; a full-set match
//                     failure is rescued by re-matching without the
//                     bass key (foreign bass, e.g. C-E-G over F#).
//    FullKeyboard   — Split point ignored: the whole keyboard feeds
//                     chord detection (the melody zone receives
//                     nothing). Matching identical to Fingered.
// =========================================================
enum class InputMode : u8 {
    SingleFinger,
    MultiFinger,
    Fingered,
    FingeredOnBass,
    FullKeyboard
};

// =========================================================
//  ChordEngine
// =========================================================
class ChordEngine {
public:
    ChordEngine() = default;

    // Initialise with config. channel_mask=0xFFFF listens on all channels.
    void init(InputMode mode, std::uint16_t channel_mask, ChordRing* ring);

    // Feed a note-on event (vel > 0 already verified by caller).
    // Notes at or above g_split_point are ignored (except FullKeyboard).
    void note_on(u8 channel, u8 note, u8 vel);

    // Feed a note-off event.
    // Notes at or above g_split_point are ignored (except FullKeyboard).
    void note_off(u8 channel, u8 note);

    // Call periodically (every 5-10ms) to expire the collection window.
    void tick();

    // Force an immediate classification flush (e.g. on all-notes-off).
    void flush();

    // Reset all state (e.g. when changing input port).
    void reset();

    // Switch the recognition mode at runtime (UI thread; mode changes
    // take effect from the next classification flush).
    void set_mode(InputMode mode);

    // ----- Accessors -----
    [[nodiscard]] const DetectedChord& current()  const noexcept { return current_; }
    [[nodiscard]] const DetectedChord& previous() const noexcept { return previous_; }
    [[nodiscard]] InputMode mode()                const noexcept { return mode_; }
    [[nodiscard]] std::uint16_t channel_mask()    const noexcept { return channel_mask_; }
    [[nodiscard]] u8 bass_note()                  const noexcept { return bass_note_; }
    [[nodiscard]] int held_count()                const noexcept { return held_count_; }
    [[nodiscard]] bool is_note_held(u8 note)      const noexcept { return held_[note] != 0; }

private:
    // ----- Internal classification -----
    void classify();
    [[nodiscard]] bool classify_single_finger();
    void publish_active_chord(const DetectedChord& d) const;

    // Lock-free-internal variants. The public entry points take mtx_
    // (note_on/note_off run on the MIDI thread, tick/flush/reset on the
    // UI thread) and then call these.
    void init_unlocked(InputMode mode, std::uint16_t channel_mask, ChordRing* ring);
    void flush_unlocked();

    // Guards held_/window state/current_/previous_ between the MIDI
    // callback thread and the UI thread.
    mutable std::mutex mtx_;

    // ----- Note state -----
    std::array<u8, 128> held_{};            // 1 = note held, 0 = released
    int                 held_count_ = 0;
    u8                  bass_note_  = 255;  // 255 = sentinel

    // ----- Collection window -----
    // window_start_ is set ONCE on the first note of a new chord and
    // never reset on subsequent notes. The original C code reset it on
    // every note-on which prevented the 50ms window from expiring
    // during fast strumming.
    std::uint64_t       window_start_  = 0;
    bool                window_active_ = false;

    // ----- Classification history -----
    DetectedChord       current_;
    DetectedChord       previous_;

    // Last chord actually PUBLISHED to g_active_chord (root >= 0). Used
    // by the release-stability rule in classify(): while a held chord is
    // released finger-by-finger, the shrinking pitch-class set must not
    // downgrade it to a weaker partial chord.
    DetectedChord       published_;
    bool                published_valid_ = false;

    // ----- Config -----
    InputMode           mode_         = InputMode::Fingered;
    std::uint16_t       channel_mask_ = 0xFFFF;

    // ----- Output -----
    ChordRing*          ring_ = nullptr;
};

// =========================================================
//  Timing — std::chrono-based
// =========================================================
// Returns current time in milliseconds (monotonic).
[[nodiscard]] std::uint64_t time_ms() noexcept;

} // namespace chord