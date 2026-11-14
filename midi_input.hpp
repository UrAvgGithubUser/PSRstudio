// midi_input.hpp — WinMM MIDI input wiring for the chord detection engine.
//
// C++20 rewrite of midi_in.c. Key changes:
//
// * All globals (g_engine, g_ring, s_midi_in, etc.) are now instance
//   members of WinMidiInput. No file-scope globals.
// * RAII: destructor closes the port.
// * The WinMM callback is a free function that recovers `this` from
//   dwCallbackInstance and dispatches to the ChordEngine.
// * Uses chord::ChordEngine (already converted) instead of the C struct.
// * Port enumeration returns a vector (like WinMidiOutput::enumerate_ports).
//
// Thread safety:
//   open/close/reset — call from the UI thread only.
//   tick/drain       — UI thread only.
//   The WinMM callback runs on a private MM thread; it only calls
//   chord_engine.note_on/off(), which are lock-free.

#pragma once

#include "chord_engine.hpp"  // for chord::ChordEngine, chord::ChordRing, chord::InputMode

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace midi {

// =========================================================
//  InputPortInfo — describes one MIDI input device
// =========================================================
struct InputPortInfo {
    int         index;
    std::string name;
};

// =========================================================
//  WinMidiInput — WinMM MIDI input + chord engine
// =========================================================
class WinMidiInput {
public:
    WinMidiInput();
    ~WinMidiInput();

    WinMidiInput(const WinMidiInput&)            = delete;
    WinMidiInput& operator=(const WinMidiInput&) = delete;
    WinMidiInput(WinMidiInput&&)                 = delete;
    WinMidiInput& operator=(WinMidiInput&&)      = delete;

    // ----- Port management -----
    [[nodiscard]] static std::vector<InputPortInfo> enumerate_ports();

    // Open MIDI input port. Returns true on success.
    // mode: Fingered or SingleFinger (chord detection mode)
    // channel_mask: bitmask of channels to listen on (0xFFFF = all)
    [[nodiscard]] bool open(int port_index,
                            chord::InputMode mode = chord::InputMode::Fingered,
                            std::uint16_t channel_mask = 0xFFFF);

    // Close the currently open port.
    void close();

    // Close port, reset engine state, clear ring.
    void reset();

    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] const std::string& port_name() const noexcept { return port_name_; }

    // ----- Chord engine access -----
    // Call every ~5-10ms from the UI thread to expire the collection window.
    void tick() { engine_.tick(); }

    // Drain pending chords from the ring. Calls `cb` for each detected chord.
    void drain(const std::function<void(const chord::DetectedChord&)>& cb);

    // Direct access to the engine (for advanced use).
    [[nodiscard]] chord::ChordEngine& engine() noexcept { return engine_; }
    [[nodiscard]] chord::ChordRing& ring() noexcept { return ring_; }

private:
#ifdef _WIN32
    // WinMM callback thunk (free function, defined in .cpp)
    static void CALLBACK callback_thunk(HMIDIIN hmi, UINT msg,
                                        DWORD_PTR inst,
                                        DWORD_PTR p1, DWORD_PTR p2);

    void on_callback(UINT msg, DWORD_PTR p1);

    HMIDIIN     handle_     = nullptr;
#else
    // Non-Windows stub (for compile-testing)
    void*       handle_     = nullptr;
#endif

    chord::ChordEngine  engine_;
    chord::ChordRing    ring_;
    std::string         port_name_;
};

} // namespace midi
