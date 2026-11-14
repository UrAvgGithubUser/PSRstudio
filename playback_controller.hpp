// playback_controller.hpp — 16-channel MIDI state model, arranger API,
// and playback orchestration.
//
// Combines three subsystems in distinct namespaces:
//
//   chmodel::  — Authoritative RAM-resident state of all 16 MIDI channels
//                (voice + mixer controls). The external synth is only ever
//                a mirror of this model. All changes (UI, style events,
//                MIDI-file events, XG SysEx) go through ChannelModel,
//                which stores and sends atomically. Includes per-parameter
//                user locks, mute/solo/master-volume, and style-load sync.
//
//   arranger:: — High-level arranger API wrapping the transpose engine.
//                Adds SFF2 three-range pitch splitting, slash-chord-aware
//                MIDI chord detection, and phrase batching for chord-voicing
//                channels.
//
//   playback:: — Orchestrates style and MIDI file playback. Owns the parsed
//                PrsStyle, the MidiPlayer, the Player sequencer, and the
//                playback thread. Manages pre-start queue state (Intro /
//                Ending / Break / Fill) and playlist building.
//
// This header includes player.hpp (for player::Player and
// midi_player::MidiPlayer) which forward-declares chmodel::ChannelModel
// — no circular include.
#pragma once

// ----- Project headers -----
#include "style.hpp"
#include "transpose.hpp"
#include "chord_engine.hpp"
#include "player.hpp"
#include "remap.hpp"


// ======================================================================
//   Channel model
// ======================================================================

// The authoritative state of all 16 MIDI channels.
//
// PSRstudio behaves like an arranger workstation: from the moment the
// program starts it owns a complete, RAM-resident image of the 16 MIDI
// channels (voice + mixer controls), independent of whether a MIDI
// device is connected or a style is loaded. The external synth is only
// ever a MIRROR of this model:
//
//   * Startup           every channel = MSB 0 / LSB 0 / PC 0, volume 64,
//                       pan centre, reverb 0, chorus 0, expression 127,
//                       variation 0, tone controls 64.
//   * Any change        (mixer, voice selector, style events, MIDI-file
//                       events, XG part SysEx) goes through THIS class,
//                       which stores it and — if a device is attached —
//                       sends the matching MIDI message in the same
//                       critical section. Model and device cannot drift.
//   * Connect/reconnect attach_sink_and_sync() dumps all 16 channels.
//   * Style load        apply_setup() stores the style's channel state and
//                       dumps all 16 channels.
//   * UI                polls snapshot() when version() changes.
//
// ---------------------------------------------------------------------
//  Threading
// ---------------------------------------------------------------------
//  All mutating calls take one internal mutex, so "update model + send to
//  device" is atomic with respect to every other mutation. The mutex is
//  held while sending; senders never call back into the model.
//
//  Two kinds of caller:
//    UI thread        Source::User / Source::Setup. Messages are sent with
//                     MidiSink::short_direct() (bypasses the output ring).
//    Playback thread  Source::Style / Source::MidiFile. Messages are sent
//                     with MidiSink::short_msg() (ring buffer), so they
//                     stay ordered with the note events the same thread
//                     queues. WinMidiOutput's ring is single-producer;
//                     using short_msg() ONLY from the playback thread (and
//                     short_direct() from the UI thread) preserves that.
//
//  Lock semantics ("user has touched this"):
//    Source::User sets a per-parameter lock. While locked, Style and
//    MidiFile events for that (channel, parameter) are ignored (and not
//    sent). Locks are cleared on every style / MIDI-file load, by
//    apply_setup(), and by unlock().


#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>

namespace chmodel {

using prs::u8;
using prs::u16;

inline constexpr int kChannels = 16;

// Where a change came from. Determines lock handling and which send path
// (direct vs ring) is used. See the header comment.
enum class Source : u8 {
    User,       // UI thread. Sets the parameter lock. Sent direct.
    Setup,      // UI thread (style load, remap re-apply). Ignores locks,
                //   sets none. Sent direct.
    Style,      // Playback thread, style events. Obeys locks. Sent queued.
    MidiFile,   // Playback thread, .mid events. Obeys locks. Sent queued.
};

// Lockable parameters. Voice = bank MSB + bank LSB + program as one unit.
enum class Param : u8 {
    Volume, Pan, Expression, Reverb, Chorus, Variation,
    FilterRes, Release, Attack, Brightness,
    Voice,
    Count
};

// One channel's complete state.
struct Channel {
    // Voice (program is the 0-127 WIRE value).
    u8 msb = 0, lsb = 0, program = 0;

    // Controllers. `volume` is the BASE volume (what the user/style set);
    // the value sent to the device is effective_volume (mute / solo /
    // master volume applied) — see ChannelModel::effective_volume().
    u8 volume     = 64;   // CC7
    u8 pan        = 64;   // CC10
    u8 expression = 127;  // CC11
    u8 reverb     = 0;    // CC91
    u8 chorus     = 0;    // CC93 (or CC94 when chorus redirect is on)
    u8 variation  = 0;    // CC94
    u8 filter_res = 64;   // CC71
    u8 release    = 64;   // CC72
    u8 attack     = 64;   // CC73
    u8 brightness = 64;   // CC74

    // PSRstudio-side monitor state (never sent as such).
    bool muted  = false;
    bool soloed = false;

    // Bit per Param: set = user-locked.
    u16 locks = 0;
};

// Consistent copy of the whole model for the UI.
struct Snapshot {
    std::array<Channel, kChannels> ch{};
    int           master_volume = 100;   // 0-100
    std::uint32_t version       = 0;
};

class ChannelModel {
public:
    ChannelModel();

    ChannelModel(const ChannelModel&)            = delete;
    ChannelModel& operator=(const ChannelModel&) = delete;

    // ----- Device attachment -----------------------------------------

    // Attach `sink` and immediately dump all 16 channels to it. Use on
    // connect AND reconnect (after the synth reset has been sent).
    void attach_sink_and_sync(prs::MidiSink* sink);

    // Detach the device. The model keeps all its state.
    void detach_sink();

    [[nodiscard]] bool has_sink() const;

    // Re-send all 16 channels to the attached device (no-op if none).
    // UI thread only (uses direct sends).
    void sync_all();

    // CC#93 (chorus) is not understood by some destinations (Montage):
    // when on, the chorus value is sent on CC#94 instead and the
    // "variation" value is tracked but NOT sent (they would collide on
    // CC#94). Changing this does not re-send; call sync_all() or
    // apply_setup() afterwards.
    void set_chorus_redirect(bool on);

    // ----- Parameter changes -----------------------------------------

    // CC numbers the model tracks: 0, 32 (bank), 7, 10, 11, 71-74, 91,
    // 93, 94. Anything else must be sent to the device by the caller.
    [[nodiscard]] static bool is_tracked_cc(int cc) noexcept;

    // Set a tracked CC. Returns true if applied (false = dropped because
    // the parameter is user-locked, or `ch`/`cc` invalid).
    // CC0 / CC32 update the bank MSB / LSB individually.
    bool set_cc(int ch, int cc, int value, Source src);

    // Program change (0-127 wire value).
    bool set_program(int ch, int program, Source src);

    // Whole voice (MSB, LSB, program) sent as one group: CC0, CC32, PC.
    bool set_voice(int ch, int msb, int lsb, int program, Source src);

    // Call AFTER the caller has sent CC#121 (Reset All Controllers) to the
    // device. Mirrors the reset in the model (expression -> 127) and
    // re-sends this channel's controllers so the device matches the model
    // regardless of how that particular synth interprets CC#121.
    void note_reset_all_controllers(int ch, Source src);

    // ----- Mixer-side state (User, UI thread) -------------------------
    void set_mute(int ch, bool on);
    void set_solo(int ch, bool on);
    void set_master_volume(int percent);          // 0-100
    [[nodiscard]] int master_volume() const;

    // ----- Locks -------------------------------------------------------
    void clear_locks();                           // all channels, all params
    void unlock(int ch, Param p);

    // ----- Style load / re-apply (UI thread) ----------------------------
    // 1. clears every lock
    // 2. for each channel in `reset_mask` (bit n = channel n): resets the
    //    voice + controllers to the startup defaults and clears mute/solo
    // 3. stores each ChannelSetup
    // 4. dumps ALL 16 channels to the device (if attached)
    // Channels neither reset nor set by the style keep their state.
    void apply_setup(std::span<const prs::ChannelSetup> setups, u16 reset_mask);

    // ----- SysEx --------------------------------------------------------
    // Called by the playback thread for every SysEx in the style / MIDI
    // file stream. `send` delivers the raw message to the device.
    //
    //   * GM / GM2 / XG System On  -> `send`, then the model resets to its
    //     defaults and re-dumps all 16 channels (the synth just reset
    //     itself to ITS defaults).
    //   * XG Multi Part parameters (bank, program, volume, pan, reverb /
    //     chorus / variation send) are CONVERTED into the equivalent
    //     controller messages through the model, so mute / solo / master
    //     volume / locks all apply. The raw SysEx is not sent. If the
    //     target parameter is user-locked the message is dropped.
    //     `map_part` (optional) translates the XG part number in the
    //     message to the OUTPUT channel it should act on — a style's XG
    //     part N addresses the style's channel N, which is routed exactly
    //     like its notes (CASM / Montage folding). Without it, part N is
    //     channel N. A mapper result outside 0-15 drops the message.
    //   * anything else           -> `send` unchanged.
    //
    // Returns false only if the message was dropped because of a lock.
    using SysexSender = std::function<void(std::span<const u8>)>;
    using PartMapper  = std::function<int(int)>;
    bool handle_sysex(std::span<const u8> payload, Source src,
                      const SysexSender& send,
                      const PartMapper& map_part = PartMapper{});

    // ----- Reading --------------------------------------------------------
    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] Channel  channel(int ch) const;

    // Incremented on every change (including lock changes). Lock-free.
    [[nodiscard]] std::uint32_t version() const noexcept {
        return version_.load(std::memory_order_acquire);
    }

    // Volume actually sent to the device for `ch` (mute / solo / master).
    [[nodiscard]] int effective_volume(int ch) const;

private:
    // ---- everything below requires mtx_ to be held ("_locked") ----
    void bump_locked() noexcept;
    void send_locked(u8 status, u8 d0, u8 d1, bool queued);
    [[nodiscard]] int effective_volume_locked(int ch) const noexcept;
    void send_param_locked(int ch, Param p, bool queued);
    void send_voice_locked(int ch, bool queued);
    void sync_channel_locked(int ch, bool queued);
    void sync_all_locked(bool queued);
    void reset_params_locked(int ch) noexcept;
    bool apply_param_locked(int ch, Param p, int value, Source src);
    bool apply_voice_part_locked(int ch, int part, int value, Source src);

    mutable std::mutex                 mtx_;
    std::array<Channel, kChannels>     ch_{};
    int                                master_volume_  = 100;
    prs::MidiSink*                     sink_           = nullptr;
    bool                               chorus_redirect_ = false;
    std::atomic<std::uint32_t>         version_{0};
};

} // namespace chmodel


// ======================================================================
//   Arranger API
// ======================================================================

// High-level arranger API.
//
// Wraps the transpose::note() / transpose::fit_phrase() / transpose::
// retrigger() functions with:
//
//   1. SFF2 three-range pitch splitting (§3 of the migration guide):
//      picks the right ctb2 sub-block per source pitch.
//
//   2. Slash-chord-aware MIDI chord detection (§5 of the migration
//      guide): given the player's left-hand chord, returns a ChordCtx
//      with root, type, AND bass note (for slash chords).
//
//   3. Phrase batching for chord-voicing channels (§4.3 of the
//      migration guide): a helper that collects simultaneous notes
//      for one chord-symbol span and calls transpose::fit_phrase()
//      once, instead of per-note note().
//
// This module is the recommended integration point between the player
// and the transpose engine. Callers should use:
//
//   arranger::transpose_single_note()     — per-note (bass, melody, drums)
//   arranger::transpose_phrase()          — per-phrase (chord channels)
//   arranger::retrigger_held_note()       — for held notes across chord changes
//   arranger::detect_chord()              — slash-chord-aware detection
//
// All functions are noexcept and allocation-free where possible.
// Phrase batching uses a caller-owned std::vector — no internal
// allocations.


#include <cstdint>
#include <span>
#include <vector>

namespace arranger {

// =========================================================
//  LiveChord — what the player is currently holding
//
//  root: 0-11 (C=0, B=11), or -1 if no chord is held.
//  type: chord::ChordType, or chord::ChordType::None if no chord.
//  bass: 0-11 for slash chords (e.g. C/E → bass=4), or -1 if bass
//        equals root. Use arranger::detect_chord() to populate this
//        from raw MIDI input.
// =========================================================
struct LiveChord {
    int root = -1;
    chord::ChordType type = chord::ChordType::None;
    int bass = -1;          // -1 = use root; 0-11 = slash-chord bass
};

// Convert a LiveChord to a transpose::ChordCtx.
// Caller for sources should use src_from_casm() instead.
[[nodiscard]] inline transpose::ChordCtx dst_from_live(const LiveChord& lc) noexcept {
    return transpose::ChordCtx{
        lc.root,
        transpose::chord_type_to_casm(lc.type),
        lc.bass
    };
}

// Build a source ChordCtx from a CASM channel's recorded source chord.
[[nodiscard]] inline transpose::ChordCtx src_from_casm(const prs::CasmChannel& cc) noexcept {
    return transpose::ChordCtx{cc.src_root, cc.src_chord_type, /*bass=*/cc.src_root};
}

// =========================================================
//  Single-note transposition (with SFF2 three-range support)
//
//  Picks the right NoteRange from the CasmChannel for the source
//  pitch (low/main/high), then delegates to transpose::note().
//
//  Use this for: bass, melody, phrase1, phrase2 channels.
//  Use transpose_phrase() instead for: chord1, chord2, pad channels.
// =========================================================
[[nodiscard]] int transpose_single_note(
    int src_pitch,
    const prs::CasmChannel& cc,
    const transpose::ChordCtx& src,
    const transpose::ChordCtx& dst) noexcept;

// =========================================================
//  Phrase-level transposition (for chord-voicing channels)
//
//  Collects source pitches for one chord-symbol span, calls
//  transpose::fit_phrase() once with the main NoteRange, and returns
//  one output pitch per input event.
//
//  Caller-owned output vector (no internal allocations). Source
//  pitches outside the main range are routed through transpose_single_
//  note() per-note (with their respective ctb2_low/ctb2_high ranges).
//
//  Use this for: chord1, chord2, pad channels where voice-leading
//  matters. For bass/melody/phrase channels, transpose_single_note()
//  per-note is sufficient and faster.
// =========================================================
struct PhraseInput {
    int src_pitch;       // source MIDI pitch (0-127)
    int parent_index;    // index of parent note (for future RTR support), or -1
};

void transpose_phrase(
    std::span<const PhraseInput> src_events,
    std::vector<int>& out_pitches,
    const prs::CasmChannel& cc,
    const transpose::ChordCtx& src,
    const transpose::ChordCtx& dst);

// =========================================================
//  Retrigger for held notes across chord changes
//
//  Picks the right NoteRange for the held pitch, then delegates to
//  transpose::retrigger(). Returns the action and new pitch.
// =========================================================
[[nodiscard]] transpose::RtrResult retrigger_held_note(
    int held_pitch,
    const prs::CasmChannel& cc,
    const transpose::ChordCtx& src,
    const transpose::ChordCtx& dst,
    bool src_was_transposed = true) noexcept;

// =========================================================
//  Bass register anchor
//
//  A RootTrans bass channel's output pitch class changes with every
//  chord; left alone, the line can wander up into guitar territory or
//  drop near-inaudible. Real arrangers fold the bass into one
//  consistent low register (E1..Eb2, MIDI 28..39).
//
//  is_bass_channel(): true when the CASM part name identifies a bass
//  part ("Bass", "bassline", ...). Drums never reach here.
//
//  apply_bass_anchor(): folds pitch into E1..Eb2 — but only for
//  RootTrans channels whose CASM note limits actually span the anchor
//  window (a style with a deliberate low limit keeps its own register).
// =========================================================
[[nodiscard]] bool is_bass_channel(const prs::CasmChannel& cc) noexcept;
[[nodiscard]] int  apply_bass_anchor(int pitch, const prs::CasmChannel& cc,
                                     const prs::NoteRange& nr) noexcept;

// =========================================================
//  Slash-chord-aware MIDI chord detector
//
//  Given a set of held MIDI note pitches (typically from the
//  player's left hand), detect the chord root, type, and bass note.
//
//  Detection algorithm:
//    1. Bass note = lowest held pitch's pitch class.
//    2. Build pitch-class set from all held notes.
//    3. For each candidate root (0-11), build the chord-type mask
//       and match against known chord-type masks.
//    4. If bass != root, mark as slash chord (bass field set).
//
//  Returns LiveChord{root=-1, type=None} if no chord could be
//  detected (fewer than 3 notes, or unrecognized pitch-class set).
// =========================================================
[[nodiscard]] LiveChord detect_chord(std::span<const int> held_pitches) noexcept;

// =========================================================
//  Convenience: detect chord from a NoteOn event stream
//
//  Same as detect_chord() but accepts a span of (pitch, velocity)
//  pairs and filters out velocity-0 events (note-offs).
// =========================================================
struct MidiNoteEvent {
    int pitch;
    int velocity;   // 0 = note-off; >0 = note-on
};

[[nodiscard]] LiveChord detect_chord_from_events(
    std::span<const MidiNoteEvent> events) noexcept;

} // namespace arranger


// ======================================================================
//   Playback controller
// ======================================================================

// Orchestrates style and MIDI file playback.
//
// Sits between the UI and the player/midi_player modules. Owns:
//   - The parsed PrsStyle (for style playback)
//   - The MidiPlayer (for .mid file playback)
//   - The player::Player (style sequencer)
//   - The playback thread
//   - The queue state machine (pre-start Intro/Ending/Break/Fill queuing)
//   - Playlist building from queue state + selected Main
//
// The UI calls into this controller; the controller spawns a thread that
// runs either player::Player::play() or midi_player::MidiPlayer::play().
// When playback ends, the controller posts WM_APP to the UI's HWND.


#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace midi { class WinMidiOutput; }

namespace playback {

// =========================================================
//  Queue kind — pre-start playlist queuing
// =========================================================
enum class QueueKind : std::uint8_t {
    None   = 0,
    Intro  = 1,
    Ending = 2,
    Break  = 3,
    Fill   = 4,
};

// =========================================================
//  Playback mode
// =========================================================
enum class Mode : std::uint8_t {
    None    = 0,
    Style   = 1,
    MidiFile = 2,
};

// =========================================================
//  PlaybackController
// =========================================================
class PlaybackController {
public:
    PlaybackController();
    ~PlaybackController();

    PlaybackController(const PlaybackController&)            = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;
    PlaybackController(PlaybackController&&)                 = delete;
    PlaybackController& operator=(PlaybackController&&)      = delete;

    // ----- MIDI output connection -----
    // The controller needs a pointer to the MIDI output device.
    // Accepts any prs::MidiSink implementation (WinMidiOutput, FluidSynthOutput, etc.)
    void set_midi_output(prs::MidiSink* midi_out) { midi_out_ = midi_out; }

    // ----- Notification -----
    // HWND to post WM_APP to when playback ends (Win32 UI).
    void set_notify_hwnd(void* hwnd) { notify_hwnd_ = hwnd; }

    // Callback for when playback ends (Qt UI — use QMetaObject::invokeMethod
    // inside the callback to marshal back to the UI thread).
    void set_playback_callback(std::function<void()> cb) {
        on_playback_finished_ = std::move(cb);
    }

    // ----- Style / MIDI file loading -----
    // Load a .prs/.sty style file. Unloads any previously loaded content.
    [[nodiscard]] bool load_style(const std::string& path);

    // Load a .mid MIDI file. Unloads any previously loaded content.
    [[nodiscard]] bool load_midi_file(const std::string& path);

    // Unload whatever is loaded.
    void unload();

    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] bool is_loaded() const noexcept { return mode_ != Mode::None; }
    [[nodiscard]] bool is_playing() const noexcept { return playing_.load(); }

    // ----- Style accessors -----
    [[nodiscard]] prs::PrsStyle* style() const { return style_.get(); }
    [[nodiscard]] const midi_player::FileInfo* midi_info() const {
        return mode_ == Mode::MidiFile ? &midi_player_.info() : nullptr;
    }
    [[nodiscard]] midi_player::MidiPlayer& midi_player_obj() { return midi_player_; }

    // ----- Queue state (pre-start playlist building) -----
    [[nodiscard]] QueueKind queue_kind() const noexcept { return queue_kind_; }
    [[nodiscard]] int queue_index() const noexcept { return queue_index_; }
    void clear_queue();
    void toggle_queue_fill();
    void set_queue_intro(int section_idx);
    void set_queue_ending(int section_idx);
    void toggle_queue_break();

    // Build a status string describing the current queue state.
    [[nodiscard]] std::string queue_status_text() const;

    // ----- Section selection -----
    [[nodiscard]] int selected_main() const noexcept {
        return player::g_selected_main.load(std::memory_order_relaxed);
    }
    void set_selected_main(int idx) {
        player::g_selected_main.store(idx, std::memory_order_relaxed);
    }

    // ----- Auto-fill -----
    [[nodiscard]] bool auto_fill() const noexcept {
        return player::g_auto_fill.load(std::memory_order_relaxed) != 0;
    }
    void set_auto_fill(bool on) {
        player::g_auto_fill.store(on ? 1 : 0, std::memory_order_relaxed);
    }

    // ----- Transport -----
    // Start playback. Builds a playlist from queue state + selected Main
    // (for style mode) or just starts the MIDI player.
    // Returns false if nothing loaded or already playing.
    bool start_playback();

    // Request stop (non-blocking). The playback thread will stop and
    // post WM_APP when done.
    void request_stop();

    // Request a section change during playback.
    void request_section(int section_idx) {
        player::g_requested_sec.store(section_idx, std::memory_order_relaxed);
    }

    // ----- Remap -----
    void set_remap(const prs::VoiceRemap* remap) { remap_ = remap; }

    // ----- Channel model -----
    // The 16-channel state every style / MIDI-file CC, PC and SysEx event
    // is routed through. Must outlive the controller and be set before
    // start_playback().
    void set_channel_model(chmodel::ChannelModel* model) { model_ = model; }

private:
    // ----- Thread management -----
    void playback_thread_style(std::vector<int> playlist);
    void playback_thread_midi();

    // ----- Playlist building -----
    [[nodiscard]] std::vector<int> build_playlist() const;

    // ----- Storage -----
    std::unique_ptr<prs::PrsStyle> style_;
    midi_player::MidiPlayer midi_player_;
    player::Player player_;
    prs::MidiSink* midi_out_ = nullptr;
    const prs::VoiceRemap* remap_ = nullptr;
    chmodel::ChannelModel* model_ = nullptr;
    void* notify_hwnd_ = nullptr;
    std::function<void()> on_playback_finished_;

    // ----- State -----
    Mode mode_ = Mode::None;
    std::atomic<bool> playing_{false};
    std::jthread play_thread_;

    // ----- Queue state -----
    QueueKind queue_kind_ = QueueKind::None;
    int queue_index_ = -1;
};

} // namespace playback