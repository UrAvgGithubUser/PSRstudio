// midi_input.cpp — WinMM MIDI input wiring for the chord detection engine.
// C++20 rewrite of midi_in.c. See midi_input.hpp for design notes.

#include "midi_input.hpp"

#include <cstring>
#include <iostream>

#ifdef _WIN32

namespace midi {

// =========================================================
//  Constructor / destructor
// =========================================================
WinMidiInput::WinMidiInput() = default;

WinMidiInput::~WinMidiInput() {
    close();
}

// =========================================================
//  Port enumeration
// =========================================================
std::vector<InputPortInfo> WinMidiInput::enumerate_ports() {
    std::vector<InputPortInfo> out;
    const UINT n = midiInGetNumDevs();
    out.reserve(n);
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSA caps{};
        if (midiInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            out.push_back({static_cast<int>(i), caps.szPname});
        } else {
            out.push_back({static_cast<int>(i), "<error>"});
        }
    }
    return out;
}

// =========================================================
//  WinMM callback thunk
//  Recovers `this` from dwCallbackInstance and dispatches to on_callback().
// =========================================================
void CALLBACK WinMidiInput::callback_thunk(HMIDIIN /*hmi*/, UINT msg,
                                           DWORD_PTR inst,
                                           DWORD_PTR p1, DWORD_PTR /*p2*/) {
    auto* self = reinterpret_cast<WinMidiInput*>(inst);
    if (self) self->on_callback(msg, p1);
}

void WinMidiInput::on_callback(UINT msg, DWORD_PTR p1) {
    // We only care about short MIDI messages
    if (msg != MIM_DATA) return;

    const std::uint8_t status  = static_cast<std::uint8_t>(p1 & 0xFF);
    const std::uint8_t d1      = static_cast<std::uint8_t>((p1 >> 8) & 0xFF);   // note
    const std::uint8_t d2      = static_cast<std::uint8_t>((p1 >> 16) & 0xFF);  // velocity
    const std::uint8_t cmd     = status >> 4;
    const std::uint8_t channel = status & 0x0F;

    if (cmd == 0x9 && d2 > 0) {
        // Note On
        engine_.note_on(channel, d1, d2);
    } else if (cmd == 0x8 || (cmd == 0x9 && d2 == 0)) {
        // Note Off (or Note On vel=0)
        engine_.note_off(channel, d1);
    }
    // All other messages (CC, pitch bend, SysEx, etc.) ignored
}

// =========================================================
//  open
// =========================================================
bool WinMidiInput::open(int port_index, chord::InputMode mode,
                        std::uint16_t channel_mask) {
    // Close any currently open port
    if (handle_) close();

    const UINT num_devs = midiInGetNumDevs();
    if (num_devs == 0) {
        std::cerr << "[midi_in] No MIDI input devices found.\n";
        return false;
    }
    if (port_index < 0 || static_cast<UINT>(port_index) >= num_devs) {
        std::cerr << std::format("[midi_in] Port {} out of range (0-{}).\n",
            port_index, num_devs - 1);
        return false;
    }

    // Initialize engine for this session
    engine_.init(mode, channel_mask, &ring_);

    // Open the WinMM device
    HMIDIIN h = nullptr;
    const MMRESULT res = midiInOpen(&h, static_cast<UINT>(port_index),
        reinterpret_cast<DWORD_PTR>(&WinMidiInput::callback_thunk),
        reinterpret_cast<DWORD_PTR>(this),
        CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        char err_buf[128];
        midiInGetErrorTextA(res, err_buf, sizeof(err_buf));
        std::cerr << std::format("[midi_in] Cannot open port {}: {} (err {})\n",
            port_index, err_buf, res);
        return false;
    }
    handle_ = h;

    // Cache port name
    MIDIINCAPSA caps{};
    if (midiInGetDevCapsA(static_cast<UINT>(port_index), &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
        port_name_ = caps.szPname;
    } else {
        port_name_ = "<unknown>";
    }

    midiInStart(handle_);
    return true;
}

// =========================================================
//  close
// =========================================================
void WinMidiInput::close() {
    if (!handle_) return;

    midiInStop(handle_);
    midiInReset(handle_);
    midiInClose(handle_);

    handle_ = nullptr;
    port_name_.clear();
}

// =========================================================
//  reset
// =========================================================
void WinMidiInput::reset() {
    close();
    engine_.reset();
    // Ring is cleared implicitly by engine_.reset() → init() cycle
}

// =========================================================
//  drain
// =========================================================
void WinMidiInput::drain(const std::function<void(const chord::DetectedChord&)>& cb) {
    chord::DetectedChord chord;
    while (ring_.pop(chord)) {
        // Skip silent / unclassified results
        if (chord.root < 0 && chord.type == chord::ChordType::None) continue;
        cb(chord);
    }
}

} // namespace midi

#endif // _WIN32
