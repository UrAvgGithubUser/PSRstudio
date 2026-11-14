// channel_model.hpp — the authoritative state of all 16 MIDI channels.
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

#pragma once

#include "style.hpp"   // prs::MidiSink, prs::ChannelSetup, prs::u8

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
    // 4. dumps ALL channels to the device (if attached)
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
