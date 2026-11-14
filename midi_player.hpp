// midi_player.hpp — Self-contained Standard MIDI File player.
//
// C++20 rewrite of midi_player.c. Key improvements:
//
// * Reuses prs::ByteReader for SMF parsing (no duplicated be32/be16/vlq).
// * Stores events in its own std::vector (no global g_events[] sharing).
// * Uses std::chrono for timing (no QPC dependency).
// * Uses midi::WinMidiOutput for all MIDI output (no midi_short/midi_sysex
//   globals, no ring_push_short).
// * Position state (measure/beat/loop) exposed via atomic members for
//   lock-free UI polling.
// * RAII: destructor stops playback and releases resources.

#pragma once

#include "style.hpp"  // for prs::Event, prs::EventKind, prs::u8, prs::u32, prs::MidiSink

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
