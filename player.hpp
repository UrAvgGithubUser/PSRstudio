// player.hpp — Style playback sequencer + Standard MIDI File player.
//
// Provides two playback engines in distinct namespaces:
//
//   midi_player::MidiPlayer — Self-contained SMF (.mid) parser and player.
//     Parses Standard MIDI Files into an event list, pre-computes fire
//     times, and plays them through any prs::MidiSink. Supports looping
//     and routes CC/PC/SysEx through the ChannelModel when one is given.
//
//   player::Player — Style playback sequencer (the heart of the system).
//     Tick-based scheduler that plays style sections, follows chord
//     changes, handles section transitions (fills, intros, endings),
//     and manages live tempo. Uses a min-heap scheduler and routes all
//     events through the ChannelModel. Transposition goes through the
//     arranger/transpose modules; chord polling reads chord::g_active_chord.
//
// chmodel::ChannelModel is forward-declared here and defined in
// playback_controller.hpp.
#pragma once

// ----- Project headers -----
#include "style.hpp"
#include "chord_engine.hpp"
#include "transpose.hpp"
#include "remap.hpp"


// ======================================================================
//   SMF player
// ======================================================================

// Self-contained Standard MIDI File player.
//
// Parses .mid files into an event list, pre-computes fire times, and
// plays them through any prs::MidiSink. Position state (measure, beat,
// loop) is exposed via atomic members for lock-free UI polling.


#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace chmodel { class ChannelModel; }

namespace midi_player {

// =========================================================
//  Result type
// =========================================================
enum class Result : std::uint8_t {
    Ok = 0,
    ErrFile,        // cannot open / read file
    ErrFormat,      // not a valid SMF
    ErrMemory,      // allocation failed
    ErrOverflow,    // too many events / sysex bytes
};

// =========================================================
//  Metadata filled in by load()
// =========================================================
struct FileInfo {
    std::string  filename;          // basename only, for display
    int          format = 0;        // SMF format 0, 1, or 2
    int          num_tracks = 0;
    int          ticks_per_qn = 480;
    prs::u32     tempo_us = 500000; // initial tempo (µs per quarter note)
    prs::u8      ts_num = 4;
    prs::u8      ts_den = 4;        // actual denominator (e.g. 4 not 2)
    int          event_count = 0;
    double       duration_sec = 0.0;
};

// =========================================================
//  MidiPlayer — self-contained SMF parser + playback engine
// =========================================================
class MidiPlayer {
public:
    MidiPlayer() = default;
    ~MidiPlayer();

    MidiPlayer(const MidiPlayer&)            = delete;
    MidiPlayer& operator=(const MidiPlayer&) = delete;
    MidiPlayer(MidiPlayer&&)                 = delete;
    MidiPlayer& operator=(MidiPlayer&&)      = delete;

    // ----- Loading -----
    // Parse a .mid file into internal storage. Call from UI thread.
    [[nodiscard]] Result load(const std::string& path);

    // Free resources. Safe to call even if load failed.
    void unload();

    [[nodiscard]] bool is_loaded() const noexcept { return loaded_; }
    [[nodiscard]] const FileInfo& info() const noexcept { return info_; }

    // ----- Playback -----
    // Play the loaded file. Blocking — call from a dedicated thread.
    // midi_out: the open MIDI output device.
    // stop_flag: checked periodically; when true, playback stops.
    // loop_count: 0 = loop forever, N = play N times then return.
    // notify_hwnd: if non-null, PostMessage(notify_hwnd, WM_APP+1, 0, 0)
    //              is sent when playback ends.
    // model:       the 16-channel state. When non-null, every CC / PC /
    //              XG-part-SysEx event of the file is routed THROUGH it (so
    //              the mixer UI, the model and the synth stay identical);
    //              notes and untracked messages go straight to midi_out.
    //              Channels are used exactly as written in the file.
    void play(prs::MidiSink& midi_out,
              const std::atomic<bool>& stop_flag,
              int loop_count = 0,
              void* notify_hwnd = nullptr,
              chmodel::ChannelModel* model = nullptr);

    // ----- Position state (read by UI via WM_TIMER) -----
    [[nodiscard]] bool is_playing()      const noexcept { return playing_.load(); }
    [[nodiscard]] int  current_measure() const noexcept { return measure_.load(); }
    [[nodiscard]] int  current_beat()    const noexcept { return beat_.load(); }
    [[nodiscard]] int  total_bars()      const noexcept { return total_bars_.load(); }
    [[nodiscard]] int  current_loop()    const noexcept { return loop_.load(); }

private:
    // ----- Internal parsing -----
    [[nodiscard]] Result parse_track(prs::ByteReader& r, std::uint32_t abs_tick_offset);

    // ----- Storage -----
    std::vector<prs::Event> events_;
    std::vector<prs::u8>    sx_pool_;  // SysEx payloads
    FileInfo                info_;
    bool                    loaded_ = false;

    // Pre-computed fire times (microseconds from start)
    std::vector<std::int64_t> fire_us_;

    // ----- Position state (atomic for UI polling) -----
    std::atomic<bool> playing_    {false};
    std::atomic<int>  measure_    {0};
    std::atomic<int>  beat_       {0};
    std::atomic<int>  total_bars_ {0};
    std::atomic<int>  loop_       {0};
};

} // namespace midi_player


// ======================================================================
//   Style sequencer
// ======================================================================

// Style playback sequencer.
//
// This is the heart of the system — the tick-based scheduler that plays
// style sections, follows chord changes, handles section transitions
// (fills, intros, endings), and manages live tempo.
//
// All cross-thread state is inline std::atomic in this header so any
// module can read/write these globals directly.


// Forward declaration — full type is brought in via player.cpp
namespace chmodel { class ChannelModel; }

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace player {

// "Patch MegaVoices" toggle (UI checkbox). When set, note-ons at C6 and
// above on MegaVoice channels (bank MSB=8, LSB 0-16) are silenced — for
// sound sources without MegaVoice support. See transpose::megavoice.
inline std::atomic<bool> g_patch_megavoices{false};

// =========================================================
//  Constants
// =========================================================
inline constexpr int kPlaylistMax    = 64;
inline constexpr int kTempoBpmMin     = 10;
inline constexpr int kTempoBpmMax     = 280;

// =========================================================
//  Playback state
// =========================================================
enum class PlaybackState : std::uint8_t {
    Stopped = 0,
    Playing = 1,
};

// =========================================================
//  Global state (all cross-thread, all atomic)
//
//  Written by UI thread:
//    g_stop, g_requested_sec, g_ui_tempo_bpm, g_auto_fill, g_selected_main
//
//  Written by player thread:
//    g_playback_state, g_current_sec_idx, g_pos_section_idx,
//    g_pos_measure, g_pos_beat, g_pos_total_bars
// =========================================================

// Global stop flag. UI sets to true, player clears to false.
inline std::atomic<bool> g_stop{false};

// Current playback state. Written by player, read by UI.
inline std::atomic<PlaybackState> g_playback_state{PlaybackState::Stopped};

// The highlighted Main section. -1 = not yet selected.
inline std::atomic<int> g_selected_main{-1};

// Section request from UI to player.
//   -1  = no request pending
//   -2  = stop playback
//  >= 0 = index of section to switch to
inline std::atomic<int> g_requested_sec{-1};

// Index of the section currently playing. -1 if stopped.
inline std::atomic<int> g_current_sec_idx{-1};

// Auto-fill toggle: 1 = automatically insert fills between Main sections.
inline std::atomic<int> g_auto_fill{1};

// Tempo override from UI (BPM). 0 = use style's original tempo.
inline std::atomic<long> g_ui_tempo_bpm{0};

// Player → UI position channel
inline std::atomic<int> g_pos_section_idx{-1};
inline std::atomic<int> g_pos_measure{0};
inline std::atomic<int> g_pos_beat{0};
inline std::atomic<int> g_pos_total_bars{0};

// =========================================================
//  Player class
// =========================================================
class Player {
public:
    Player() = default;
    ~Player();

    Player(const Player&)            = delete;
    Player& operator=(const Player&) = delete;
    Player(Player&&)                 = delete;
    Player& operator=(Player&&)      = delete;

    // Play the style, cycling through the given playlist. Blocks until
    // playback stops (either playlist exhausted or g_stop set).
    //
    // style   — parsed style file
    // midi    — open MIDI output device (with thread started)
    // remap   — voice remap table (for live PC remapping during playback)
    // playlist — array of section indices
    // model   — the 16-channel state. Every CC / PC / XG-part-SysEx event of
    //           the style is routed THROUGH it (never straight to `midi`), so
    //           the mixer, the model and the device stay identical. Notes and
    //           untracked messages (sustain, pitch bend, ...) still go to
    //           `midi` directly.
    void play(const prs::PrsStyle& style,
              prs::MidiSink& midi,
              const prs::VoiceRemap& remap,
              const std::vector<int>& playlist,
              chmodel::ChannelModel& model);

    // Request a section change (called from UI thread).
    // sec_idx: section index to switch to, or -2 to stop.
    static void request_section(int sec_idx) {
        g_requested_sec.store(sec_idx, std::memory_order_relaxed);
    }

    // Request stop (called from UI thread).
    static void request_stop() {
        g_stop.store(true, std::memory_order_relaxed);
    }

private:
    // The channel model for the current play() call (set at its start,
    // never null while play() runs).
    chmodel::ChannelModel* model_ = nullptr;

    // Output channel for a style SOURCE channel: CASM dst_ch (or the
    // channel itself if CASM has no entry), then the remap profile's
    // routing.
    [[nodiscard]] static prs::u8 out_channel(const prs::RemapSource& remap,
                                             prs::u8 src_ch,
                                             const prs::CasmChannel* cc) noexcept {
        return remap.route_channel(src_ch, cc ? cc->dst_ch : src_ch);
    }

    // ----- Heap scheduler -----
    struct EventNode {
        std::uint64_t absolute_tick;  // tick since playback start
        std::uint32_t event_index;    // index into style.events()
        std::uint32_t section_id;     // which section this event came from
    };

    static constexpr std::uint32_t kEventKillNotes = 0xFFFFFFFFu;

    // Min-heap ordered by absolute_tick. Uses std::push_heap/pop_heap
    // with a comparator that makes it a min-heap (not the default max-heap).
    //
    // TIE-BREAK: at equal ticks, kill-notes events pop FIRST. Deferred
    // section switches push a kill-node at the barline and load the new
    // section's events at the SAME tick; without the tie-break the pop
    // order between them is heap-layout-dependent, so the new section's
    // downbeat note-ons could fire BEFORE the kill — which then chopped
    // those fresh notes off an instant later (audible stutter on Main →
    // Ending etc.). Kill-first guarantees: old notes cut, THEN the new
    // section's downbeat sounds.
    struct MinByTick {
        bool operator()(const EventNode& a, const EventNode& b) const noexcept {
            if (a.absolute_tick != b.absolute_tick) {
                return a.absolute_tick > b.absolute_tick;
            }
            const bool a_kill = (a.event_index == kEventKillNotes);
            const bool b_kill = (b.event_index == kEventKillNotes);
            if (a_kill != b_kill) return !a_kill;   // kill = "greatest" -> pops first
            return false;                            // otherwise: stable, unspecified
        }
    };

    std::vector<EventNode> heap_;

    void heap_push(EventNode node);
    [[nodiscard]] EventNode heap_pop();
    void load_section_into_heap(const prs::PrsStyle& style, int sec_idx,
                                std::uint64_t base_tick,
                                std::uint64_t min_tick_offset);

    // ----- Playback state -----
    std::uint64_t current_tick_      = 0;
    std::chrono::steady_clock::time_point tick_origin_;
    std::uint32_t active_tempo_us_   = 500000;
    std::uint32_t style_tpqn_        = 480;

    // Held note tracking: held[ch][pitch] = true, held_out_pitch[ch][pitch] = output pitch
    std::array<std::array<prs::u8, 128>, 16> held_{};
    std::array<std::array<int, 128>, 16> held_out_pitch_{};
    // held_out_ch_[ch][pitch] = the actual MIDI channel each held note was
    // sent to (post-CASM, post-MONTAGE remap). Indexed by [src_ch][src_pitch]
    // to match held_ and held_out_pitch_.
    //
    // We MUST remember this because casm_cache_ is refreshed on section
    // changes — if a note was held in section A (sent to out_ch=X) and
    // the section changes to B (which would map src_ch → out_ch=Y), the
    // cleanup loop must still send the note-off to out_ch=X, not Y.
    // Without this, notes can hang when sections change while notes are
    // held — especially visible in MONTAGE mode where ch 9-16 collapse
    // to ch 1-8 and collisions are common.
    std::array<std::array<int, 128>, 16> held_out_ch_{};
    // Velocity each held note was played with (reused when it is retriggered
    // on a chord change so sustained notes don't jump in loudness).
    std::array<std::array<prs::u8, 128>, 16> held_vel_{};

    // Scratch list for the two-pass chord-change retrigger (all note-offs
    // first, then all note-ons). Member so it allocates once, not per change.
    struct PendingRtr { int ch; int n; int out_ch; int new_pitch; prs::u8 vel; };
    std::vector<PendingRtr> pending_rtr_;

    std::array<int, 16> prev_src_pitch_{};
    std::array<int, 16> prev_out_pitch_{};

    // CASM channel cache — avoids calling casm_get_channel() (linear search
    // + string comparison) for every held note on every chord change.
    // Indexed by source channel (0-15). nullptr = no CASM entry for this ch.
    // Refreshed whenever the current section changes.
    std::array<const prs::CasmChannel*, 16> casm_cache_{};
    int casm_cache_section_ = -1;  // which section the cache is for

    // Refresh the CASM cache for the given section.
    void refresh_casm_cache(const prs::PrsStyle& style,
                            const prs::StyleSection& sec);

    // Ketron/Korg styles carry MAJ/MIN/7TH chord-quality variants of each
    // section ("<name>", "<name> MIN", "<name> 7TH"). Resolves a requested
    // section index to the variant matching the current chord family
    // (Yamaha styles pass through unchanged).
    [[nodiscard]] int resolve_quality_variant(const prs::PrsStyle& style,
                                              int sec_idx) const;

    // Live bank tracking (for PC remapping during playback)
    std::array<prs::u8, 16> live_msb_{};
    std::array<prs::u8, 16> live_lsb_{};

    // Current chord context (CASM-converted type for transposition)
    transpose::ChordCtx active_chord_{-1, -1, -1};
    // Raw ChordType from the chord engine (for change detection).
    // Must be stored separately because active_chord_.type is CASM-converted.
    int active_chord_raw_type_ = -1;
    // Played bass pitch class from the chord engine (for slash chords).
    // Part of chord-change detection: C -> C/E must re-voice bass_on
    // channels even though root and type are unchanged.
    int active_bass_ = -1;

    // Whether the bass legato setup has been sent (only sent once at playback start)
    bool bass_legato_sent_ = false;

    // ----- Internal helpers -----
    void kill_all_held_notes(const prs::PrsStyle& style,
                             const prs::StyleSection& cur_sec,
                             prs::MidiSink& midi,
                             const prs::RemapSource& remap);

    bool advance_playlist(const prs::PrsStyle& style,
                          std::vector<int>& pl,
                          int& current_pl_index,
                          int& current_sec_idx,
                          std::uint64_t& current_section_base_tick,
                          bool& stopping);

    void handle_section_request(const prs::PrsStyle& style,
                                int requested_sec_idx,
                                std::vector<int>& pl,
                                int current_sec_idx,
                                std::uint64_t current_section_base_tick,
                                std::uint64_t true_current_tick,
                                prs::MidiSink& midi,
                                const prs::RemapSource& remap);

    void process_chord_change(const prs::PrsStyle& style,
                              const prs::StyleSection& sec,
                              prs::MidiSink& midi,
                              const prs::RemapSource& remap);

    void process_midi_event(const prs::PrsStyle& style,
                            const prs::StyleSection& sec,
                            const prs::Event& ev,
                            prs::MidiSink& midi,
                            const prs::RemapSource& remap);

    // Check if a drum note is currently sounding at the given
    // (out_ch, pitch). Returns the source channel index (0-15) of the
    // drum note, or -1 if none.
    //
    // Used by process_chord_change to protect drum notes from being
    // killed by non-drum retrigger note-offs on shared output channels
    // (MONTAGE mode folds drums and melodic channels onto the same
    // output channel — a note-off for a chord tone at pitch P on out_ch
    // C would also kill a drum note at the same (C, P)).
    [[nodiscard]] int drum_note_at(int out_ch, int pitch) const noexcept;

    // ----- Time helpers -----
    [[nodiscard]] static std::chrono::steady_clock::time_point now() noexcept {
        return std::chrono::steady_clock::now();
    }

    [[nodiscard]] std::uint64_t elapsed_us() const noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now() - tick_origin_).count());
    }

    // Convert tick delta to microseconds at current tempo.
    [[nodiscard]] std::uint64_t tick_to_us(std::uint64_t ticks) const noexcept {
        return (ticks * active_tempo_us_) / style_tpqn_;
    }
};

} // namespace player