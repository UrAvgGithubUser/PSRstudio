// midi.cpp — WinMM MIDI input + output.
#include "midi.hpp"


// ======================================================================
//   MIDI input
// ======================================================================

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
        // Next-note capture (split-point picking): checked BEFORE the
        // chord engine, so any key — above or below the current split —
        // can be picked, and the captured key never enters the engine's
        // held[] state.
        if (capture_armed_.load(std::memory_order_acquire)) {
            capture_armed_.store(false, std::memory_order_release);
            captured_note_.store(d1, std::memory_order_release);
            return;
        }
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


// ======================================================================
//   MIDI output
// ======================================================================

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <thread>

// =========================================================
//  WinMM callback (free function)
//
//  WinMM requires a free function pointer, not a member function
//  pointer. We pass `this` as dwCallbackInstance to midiOutOpen,
//  recover it here, and dispatch to on_callback(). Defined before
//  open() so the reference in midiOutOpen resolves.
// =========================================================
namespace {

void CALLBACK midi_output_callback(HMIDIOUT /*hmo*/, std::uint32_t msg,
                                   std::uintptr_t inst,
                                   std::uintptr_t /*p1*/,
                                   std::uintptr_t /*p2*/) {
    auto* self = reinterpret_cast<midi::WinMidiOutput*>(inst);
    if (self) self->on_callback(msg);
}

} // anonymous namespace

namespace midi {

// =========================================================
//  Constructor / destructor
//  Both defined here (not inline) because they need MIDIHDR's
//  complete type to instantiate std::vector<MIDIHDR>'s destructor.
// =========================================================
WinMidiOutput::WinMidiOutput() = default;

WinMidiOutput::~WinMidiOutput() {
    stop_thread();
    close();
}

// =========================================================
//  Port enumeration
// =========================================================
std::vector<PortInfo> WinMidiOutput::enumerate_ports() {
    std::vector<PortInfo> out;
    const UINT n = midiOutGetNumDevs();
    out.reserve(n);
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPS c{};
        if (midiOutGetDevCaps(i, &c, sizeof(c)) != MMSYSERR_NOERROR) continue;
        PortInfo p;
        p.index        = static_cast<int>(i);
        // szPname is WCHAR[32] in Unicode builds, char[32] in ANSI.
        // Use MIDIOUTCAPSA to always get ANSI names for std::string.
        MIDIOUTCAPSA ca{};
        if (midiOutGetDevCapsA(i, &ca, sizeof(ca)) == MMSYSERR_NOERROR) {
            p.name = ca.szPname;
        } else {
            p.name = "<unknown>";
        }
        p.technology   = c.wTechnology;
        p.voices       = c.wVoices;
        p.notes        = c.wNotes;
        p.channel_mask = c.wChannelMask;
        out.push_back(std::move(p));
    }
    return out;
}

void WinMidiOutput::list_ports() {
    const auto ports = enumerate_ports();
    std::cout << std::format("MIDI output ports ({}):\n", ports.size());
    for (const auto& p : ports) {
        std::cout << std::format("  [{}] {}\n", p.index, p.name);
    }
}

// =========================================================
//  open / close
// =========================================================
bool WinMidiOutput::open(int port) {
    if (handle_) close();

    sx_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!sx_event_) {
        std::cerr << "[midi] CreateEvent failed\n";
        return false;
    }

    // Allocate the MIDIHDR pools (need MIDIHDR's complete type, which is
    // visible here because <windows.h> is included).
    sx_ring_hdr_.resize(kSxRingSlots);
    setup_hdr_.resize(kSetupSxSlots);
    direct_hdr_ = std::make_unique<MIDIHDR>();

    HMIDIOUT h = nullptr;
    const MMRESULT res = midiOutOpen(&h, static_cast<UINT>(port),
        reinterpret_cast<DWORD_PTR>(&midi_output_callback),
        reinterpret_cast<DWORD_PTR>(this),
        CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        std::cerr << std::format("[midi] Cannot open port {} (err {})\n", port, res);
        CloseHandle(sx_event_);
        sx_event_ = nullptr;
        return false;
    }
    handle_ = h;

    MIDIOUTCAPSA ca{};
    if (midiOutGetDevCapsA(static_cast<UINT>(port), &ca, sizeof(ca)) == MMSYSERR_NOERROR) {
        port_name_ = ca.szPname;
    } else {
        port_name_ = "<unknown>";
    }
    std::cout << std::format("[midi] Opened port {}: '{}'\n", port, port_name_);
    return true;
}

void WinMidiOutput::close() {
    if (!handle_) return;
    stop_thread();
    midiOutClose(handle_);
    handle_ = nullptr;
    if (sx_event_) { CloseHandle(sx_event_); sx_event_ = nullptr; }
    port_name_.clear();
}

// =========================================================
//  on_callback — called by the WinMM thunk on a private MM thread
// =========================================================
void WinMidiOutput::on_callback(std::uint32_t msg) {
    if (msg == MOM_DONE && sx_event_) {
        SetEvent(sx_event_);
    }
}

// =========================================================
//  Ring buffer helpers
// =========================================================
void WinMidiOutput::ring_wait_space() {
    while (static_cast<LONG>(ring_w_.load(std::memory_order_relaxed) -
                             ring_r_.load(std::memory_order_acquire)) >=
           static_cast<LONG>(kRingSize)) {
        Sleep(1);  // yield — Sleep(0) barely yields on loaded systems
    }
}

void WinMidiOutput::ring_push_short(prs::u8 st, prs::u8 d0, prs::u8 d1) {
    ring_wait_space();
    const LONG w = ring_w_.load(std::memory_order_relaxed) & kRingMask;
    ring_[w].kind = RingKind::Short;
    ring_[w].msg  = static_cast<std::uint32_t>(st) |
                    (static_cast<std::uint32_t>(d0) << 8) |
                    (static_cast<std::uint32_t>(d1) << 16);
    ring_w_.fetch_add(1, std::memory_order_release);
    cv_.notify_one();
}

void WinMidiOutput::ring_push_sysex(std::span<const prs::u8> payload) {
    // Wait for a free sysex slot
    while (static_cast<LONG>(sx_ring_w_.load(std::memory_order_relaxed) -
                             sx_ring_r_.load(std::memory_order_acquire)) >=
           static_cast<LONG>(kSxRingSlots)) {
        Sleep(1);
    }

    const auto len = static_cast<std::uint16_t>(payload.size());
    const auto total = static_cast<std::uint16_t>(len + 2);
    if (total > kSxRingBufSz) return;  // too big for the slot

    const LONG sw = sx_ring_w_.load(std::memory_order_relaxed) & (kSxRingSlots - 1);
    sx_ring_[sw][0] = 0xF0;
    std::memcpy(&sx_ring_[sw][1], payload.data(), len);
    sx_ring_[sw][len + 1] = 0xF7;

    ring_wait_space();
    const LONG w = ring_w_.load(std::memory_order_relaxed) & kRingMask;
    ring_[w].kind    = RingKind::SysEx;
    ring_[w].sx_slot = static_cast<prs::u8>(sw);
    ring_[w].sx_len  = total;

    // Commit the sysex slot FIRST, then the ring slot.
    sx_ring_w_.fetch_add(1, std::memory_order_release);
    ring_w_.fetch_add(1, std::memory_order_release);
    cv_.notify_one();
}

// =========================================================
//  Player-facing API
// =========================================================
void WinMidiOutput::short_msg(prs::u8 status, prs::u8 d0, prs::u8 d1) {
    ring_push_short(status, d0, d1);
}

void WinMidiOutput::sysex(std::span<const prs::u8> payload) {
    const auto len = payload.size();
    if (len > 65535) {
        ring_push_sysex(payload.subspan(0, 65535));
    } else {
        ring_push_sysex(payload);
    }
}

// =========================================================
//  Direct send (bypass ring)
// =========================================================
void WinMidiOutput::short_direct(prs::u8 status, prs::u8 d0, prs::u8 d1) {
    if (!handle_) return;
    midiOutShortMsg(handle_,
        static_cast<DWORD>(status) |
        (static_cast<DWORD>(d0) << 8) |
        (static_cast<DWORD>(d1) << 16));
}

void WinMidiOutput::sysex_direct(std::span<const prs::u8> payload) {
    if (!handle_ || !sx_event_) return;

    const auto len = payload.size();
    const std::size_t total = len + 2;

    // Use the stack-allocated direct_buf_ for small payloads, else heap.
    prs::u8* p;
    if (total <= direct_buf_.size()) {
        p = direct_buf_.data();
    } else {
        p = static_cast<prs::u8*>(std::malloc(total));
        if (!p) return;
    }

    p[0] = 0xF0;
    std::memcpy(p + 1, payload.data(), len);
    p[len + 1] = 0xF7;

    direct_hdr_->lpData          = reinterpret_cast<char*>(p);
    direct_hdr_->dwBufferLength  = static_cast<DWORD>(total);
    direct_hdr_->dwFlags         = 0;
    ResetEvent(sx_event_);
    midiOutPrepareHeader(handle_, direct_hdr_.get(), sizeof(MIDIHDR));
    midiOutLongMsg(handle_, direct_hdr_.get(), sizeof(MIDIHDR));
    WaitForSingleObject(sx_event_, 500);
    midiOutUnprepareHeader(handle_, direct_hdr_.get(), sizeof(MIDIHDR));

    if (p != direct_buf_.data()) std::free(p);
}

// =========================================================
//  Setup SysEx (batched, non-blocking)
// =========================================================
void WinMidiOutput::sysex_nowait(std::span<const prs::u8> payload) {
    if (!handle_) return;

    const auto len = payload.size();
    const std::size_t total = len + 2;
    if (total > kSetupSxBufSz) {
        // Too big for the small setup pool — fall back to direct send.
        sysex_direct(payload);
        return;
    }

    const int s = setup_slot_++ & static_cast<int>(kSetupSxSlots - 1);
    prs::u8* p = setup_buf_[s].data();
    p[0] = 0xF0;
    std::memcpy(p + 1, payload.data(), len);
    p[len + 1] = 0xF7;

    std::memset(&setup_hdr_[s], 0, sizeof(MIDIHDR));
    setup_hdr_[s].lpData         = reinterpret_cast<char*>(p);
    setup_hdr_[s].dwBufferLength = static_cast<DWORD>(total);
    midiOutPrepareHeader(handle_, &setup_hdr_[s], sizeof(MIDIHDR));
    midiOutLongMsg(handle_, &setup_hdr_[s], sizeof(MIDIHDR));
}

void WinMidiOutput::sysex_flush() {
    if (!handle_) return;

    const int used = (setup_slot_ < static_cast<int>(kSetupSxSlots))
                     ? setup_slot_ : static_cast<int>(kSetupSxSlots);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);

    for (;;) {
        bool pending = false;
        for (int i = 0; i < used; ++i) {
            if (!(setup_hdr_[i].dwFlags & MHDR_DONE)) { pending = true; break; }
        }
        if (!pending || std::chrono::steady_clock::now() > deadline) break;
        Sleep(1);
    }

    for (int i = 0; i < used; ++i) {
        if (setup_hdr_[i].dwFlags & MHDR_PREPARED) {
            midiOutUnprepareHeader(handle_, &setup_hdr_[i], sizeof(MIDIHDR));
        }
    }
    setup_slot_ = 0;
}

// =========================================================
//  Panic
// =========================================================
void WinMidiOutput::all_notes_off() {
    for (int ch = 0; ch < 16; ++ch) {
        short_direct(static_cast<prs::u8>(0xB0 | ch), 120, 0);  // All Sound Off
        short_direct(static_cast<prs::u8>(0xB0 | ch), 123, 0);  // All Notes Off
    }
}

// =========================================================
//  MidiSink: sleep_ms
// =========================================================
void WinMidiOutput::sleep_ms(unsigned ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// =========================================================
//  Output thread
// =========================================================
void WinMidiOutput::output_thread_main(std::stop_token stoken) {
    // HIGHEST, not TIME_CRITICAL — play thread is TIME_CRITICAL and must
    // not be starved by the output thread.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    while (!stoken.stop_requested()) {
        const LONG r = ring_r_.load(std::memory_order_acquire);
        const LONG w = ring_w_.load(std::memory_order_acquire);

        if (r == w) {
            // Ring empty — wait for a push or stop request.
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk, std::chrono::milliseconds(2),
                         [&] { return ring_w_.load(std::memory_order_relaxed) !=
                                       ring_r_.load(std::memory_order_relaxed) ||
                               stoken.stop_requested(); });
            continue;
        }

        // Acquire slot
        const auto& slot = ring_[static_cast<std::size_t>(r) & kRingMask];

        if (slot.kind == RingKind::Short) {
            midiOutShortMsg(handle_, slot.msg);
        } else if (slot.kind == RingKind::SysEx) {
            const int s    = slot.sx_slot & static_cast<int>(kSxRingSlots - 1);
            const int blen = slot.sx_len;
            MIDIHDR* hdr   = &sx_ring_hdr_[s];
            std::memset(hdr, 0, sizeof(MIDIHDR));
            hdr->lpData         = reinterpret_cast<char*>(sx_ring_[s].data());
            hdr->dwBufferLength = static_cast<DWORD>(blen);
            midiOutPrepareHeader(handle_, hdr, sizeof(MIDIHDR));
            midiOutLongMsg(handle_, hdr, sizeof(MIDIHDR));
            WaitForSingleObject(sx_event_, 300);
            midiOutUnprepareHeader(handle_, hdr, sizeof(MIDIHDR));
            sx_ring_r_.fetch_add(1, std::memory_order_release);
        }
        ring_r_.fetch_add(1, std::memory_order_release);
    }
}

void WinMidiOutput::start_thread() {
    if (thread_running_.load()) return;
    // Reset counters
    ring_w_.store(0, std::memory_order_relaxed);
    ring_r_.store(0, std::memory_order_relaxed);
    sx_ring_w_.store(0, std::memory_order_relaxed);
    sx_ring_r_.store(0, std::memory_order_relaxed);
    thread_running_.store(true, std::memory_order_relaxed);
    output_thread_ = std::jthread([this](std::stop_token st) {
        output_thread_main(std::move(st));
    });
    if (!output_thread_.joinable()) {
        std::cerr << "[midi] Failed to create output thread!\n";
        thread_running_.store(false, std::memory_order_relaxed);
    }
}

void WinMidiOutput::stop_thread() {
    if (!thread_running_.exchange(false)) return;
    if (output_thread_.joinable()) {
        output_thread_.request_stop();
        cv_.notify_all();
        output_thread_.join();
    }
}

} // namespace midi