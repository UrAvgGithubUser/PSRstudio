// midi.hpp — WinMM MIDI input + output.
//
// Provides:
//   - WinMidiInput:  MIDI input port + chord detection engine wiring
//   - WinMidiOutput: MIDI output port with SPSC ring buffer + output thread
//
// Both classes live in the `midi` namespace. WinMidiOutput implements
// prs::MidiSink so the parser and channel model can drive it directly.
//
// Thread safety:
//   open/close/reset — call from the UI thread only.
//   tick/drain       — UI thread only.
//   WinMM callbacks run on a private MM thread; they only call
//   lock-free methods (engine note_on/off, ring push).
//
// RAII: destructors close ports and stop threads.
#pragma once

// ----- Project headers -----
#include "chord_engine.hpp"
#include "style.hpp"


// ======================================================================
//   MIDI input
// ======================================================================

// WinMM MIDI input wiring for the chord detection engine.
//
// Thread safety:
//   open/close/reset — call from the UI thread only.
//   tick/drain       — UI thread only.
//   The WinMM callback runs on a private MM thread; it only calls
//   chord_engine.note_on/off(), which are lock-free.


#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

#include <atomic>
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

    // ----- Next-note capture (split-point picking) -----
    //
    // Arm capture: the NEXT note-on from ANY channel at ANY pitch is
    // captured instead of being fed to the chord engine. The check runs
    // BEFORE the channel mask and the engine's internal split-point
    // filter, so any key can be picked (including keys above the current
    // split, which chord detection would normally ignore). The captured
    // key also never enters the engine's held[] state, so it cannot
    // pollute chord detection; its later note-off is safely ignored.
    //
    // All three atomics — arm/cancel/poll from the UI thread while the
    // WinMM callback thread captures.
    void arm_split_capture() noexcept {
        captured_note_.store(-1, std::memory_order_relaxed);
        capture_armed_.store(true, std::memory_order_release);
    }
    void cancel_capture() noexcept {
        capture_armed_.store(false, std::memory_order_release);
    }
    [[nodiscard]] bool capture_armed() const noexcept {
        return capture_armed_.load(std::memory_order_acquire);
    }
    // The captured note (0-127), or -1 if none captured yet. Consuming
    // (re-arms nothing — the armed flag was cleared by the callback).
    [[nodiscard]] int take_captured_note() noexcept {
        return captured_note_.exchange(-1, std::memory_order_acq_rel);
    }

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

    // ----- Next-note capture state -----
    std::atomic<bool> capture_armed_{false};  // waiting for the next note-on
    std::atomic<int>  captured_note_{-1};     // note captured (0-127), or -1
};

} // namespace midi


// ======================================================================
//   MIDI output
// ======================================================================

// WinMidiOutput — WinMM MIDI output with ring buffer + output thread.
//
// Implements prs::MidiSink so the parser and channel model can drive it
// directly. Uses a lock-free SPSC ring buffer (producer = player thread,
// consumer = output thread) and a background std::jthread that wakes on
// push via condition_variable instead of polling.
//
// Public API:
//   WinMidiOutput out;
//   out.list_ports();           // print available ports
//   out.open(0);                // open port 0
//   out.start_thread();         // start the background output thread
//   out.short_msg(0x90, 60, 100);  // queue a note-on (thread-safe)
//   out.sysex(payload);            // queue sysex (thread-safe)
//   ...
//   out.stop_thread();          // stop background thread
//   out.all_notes_off();        // panic
//   out.close();                // close port
//   // or just let it go out of scope — destructor does all of the above.


#include <array>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// On Windows, we need the real WinMM types (HMIDIOUT, HANDLE, MIDIHDR).
// These are typedefs, not forward-declarable types, so we must include
// the headers. On non-Windows (compile-testing), use the stub.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#elif !defined(WINMM_STUB_H)
// Non-Windows stub for compile-testing only.
struct HMIDIOUT__;
typedef HMIDIOUT__* HMIDIOUT;
struct MIDIHDR;
typedef void* HANDLE;
#endif

namespace midi {

// =========================================================
//  PortInfo — describes one MIDI output device
// =========================================================
struct PortInfo {
    int         index;       // 0-based device index for open()
    std::string name;        // device name
    std::uint16_t technology; // MIDIOUTCAPS.wTechnology
    std::uint16_t voices;     // max polyphony
    std::uint16_t notes;      // max simultaneous notes
    std::uint16_t channel_mask; // supported channels
};

// =========================================================
//  WinMidiOutput — WinMM MIDI output with ring buffer + thread
// =========================================================
class WinMidiOutput final : public prs::MidiSink {
public:
    WinMidiOutput();
    ~WinMidiOutput() override;  // defined in .cpp (needs MIDIHDR complete type)

    WinMidiOutput(const WinMidiOutput&)            = delete;
    WinMidiOutput& operator=(const WinMidiOutput&) = delete;
    WinMidiOutput(WinMidiOutput&&)                 = delete;
    WinMidiOutput& operator=(WinMidiOutput&&)      = delete;

    // ----- Port management -----
    // Enumerate available MIDI output ports.
    [[nodiscard]] static std::vector<PortInfo> enumerate_ports();

    // Print available ports to stdout (convenience wrapper).
    static void list_ports();

    // Open MIDI output port `port`. Returns true on success.
    // Calling open() while a port is already open closes the old one first.
    [[nodiscard]] bool open(int port);

    // Close the currently open port. Stops the output thread if running.
    void close();

    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }

    // ----- Output thread -----
    // Start the background output thread that drains the ring buffer.
    // Must be called after open() and before any short_msg()/sysex() calls.
    void start_thread();

    // Stop the output thread. Drains the ring first.
    void stop_thread();

    [[nodiscard]] bool thread_running() const noexcept { return thread_running_.load(); }

    // ----- Player-facing API (thread-safe, queues into the ring) -----
    // Queue a 3-byte short message (note on/off, CC, PC, etc.).
    // Blocks if the ring is full (until the output thread drains some).
    void short_msg(prs::u8 status, prs::u8 d0, prs::u8 d1) override;

    // Queue a SysEx message. `payload` = bytes between F0 and F7 (exclusive).
    // Blocks if the sysex slot pool is full.
    void sysex(std::span<const prs::u8> payload) override;

    // ----- Direct send (bypass ring, block until done) -----
    // Send a short message immediately. Only safe before start_thread()
    // or from the same thread that owns the WinMidiOutput.
    void short_direct(prs::u8 status, prs::u8 d0, prs::u8 d1) override;

    // Send a SysEx immediately and wait for completion (max 500ms).
    void sysex_direct(std::span<const prs::u8> payload);

    // ----- Setup SysEx (batched, non-blocking) -----
    // Queue a setup SysEx for later flush. Falls back to sysex_direct()
    // if the payload exceeds the small setup buffer.
    void sysex_nowait(std::span<const prs::u8> payload) override;

    // Wait for all queued setup SysEx to complete (max 50ms total).
    void sysex_flush() override;

    // ----- Panic -----
    // Send All Notes Off (CC 123) + All Sound Off (CC 120) on all 16 channels.
    void all_notes_off() override;

    // ----- MidiSink interface (called by parser) -----
    void sleep_ms(unsigned ms) override;

    // ----- Diagnostics -----
    [[nodiscard]] std::string_view port_name() const noexcept { return port_name_; }

    // Ring buffer occupancy — useful for the player to detect drain.
    [[nodiscard]] std::uint32_t ring_pending() const noexcept {
        return static_cast<std::uint32_t>(ring_w_.load(std::memory_order_relaxed) -
                                          ring_r_.load(std::memory_order_acquire));
    }

    // Called by the internal WinMM callback thunk (free function in the .cpp).
    // Public so the thunk can dispatch here without friend gymnastics.
    void on_callback(std::uint32_t msg);

private:
    // ----- Ring buffer constants (same as C original) -----
    static constexpr std::uint32_t kRingPow2 = 12;
    static constexpr std::uint32_t kRingSize = 1u << kRingPow2;  // 4096
    static constexpr std::uint32_t kRingMask = kRingSize - 1;

    static constexpr std::uint32_t kSxRingSlots = 16;
    static constexpr std::uint32_t kSxRingBufSz = 512;

    static constexpr std::uint32_t kSetupSxSlots = 16;
    static constexpr std::uint32_t kSetupSxBufSz = 64;

    // ----- Ring slot types -----
    enum class RingKind : prs::u8 { Short, SysEx };

    struct RingSlot {
        std::uint32_t msg;       // packed: status | (d0<<8) | (d1<<16)
        RingKind      kind;
        prs::u8       sx_slot;   // index into sx_ring_ (for SysEx only)
        std::uint16_t sx_len;    // total sysex length including F0/F7
    };

    // ----- WinMM callback -----
    // The actual callback is a free function in the .cpp (WinMM requires
    // a free function pointer, not a member function pointer). It recovers
    // `this` from dwCallbackInstance and calls on_callback() (declared above).

private:

    // ----- Ring buffer helpers -----
    void ring_wait_space();
    void ring_push_short(prs::u8 st, prs::u8 d0, prs::u8 d1);
    void ring_push_sysex(std::span<const prs::u8> payload);

    // ----- Output thread main -----
    void output_thread_main(std::stop_token stoken);

    // ----- Storage -----
    HMIDIOUT        handle_     = nullptr;
    HANDLE          sx_event_   = nullptr;     // auto-reset event for sysex completion
    std::string     port_name_;

    // Ring buffer (SPSC: producer = player thread, consumer = output thread)
    std::array<RingSlot, kRingSize> ring_{};
    std::atomic<std::int32_t>       ring_w_{0};
    std::atomic<std::int32_t>       ring_r_{0};

    // SysEx slot pool (used by ring_push_sysex / output_thread)
    std::array<std::array<prs::u8, kSxRingBufSz>, kSxRingSlots> sx_ring_{};
    std::vector<MIDIHDR> sx_ring_hdr_;  // sized to kSxRingSlots in open()
    std::atomic<std::int32_t> sx_ring_w_{0};
    std::atomic<std::int32_t> sx_ring_r_{0};

    // Output thread
    std::jthread             output_thread_;
    std::atomic<bool>        thread_running_{false};
    std::mutex               cv_mutex_;
    std::condition_variable  cv_;

    // Setup SysEx pool (for sysex_nowait / sysex_flush)
    std::vector<MIDIHDR> setup_hdr_;  // sized to kSetupSxSlots in open()
    std::array<std::array<prs::u8, kSetupSxBufSz>, kSetupSxSlots> setup_buf_{};
    int setup_slot_ = 0;

    // Direct sysex buffer (for sysex_direct — single shot).
    // unique_ptr so the header doesn't need MIDIHDR's complete type.
    std::unique_ptr<MIDIHDR> direct_hdr_;
    std::array<prs::u8, 512> direct_buf_{};
};

} // namespace midi