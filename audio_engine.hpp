// audio_engine.hpp — JUCE audio backend for PSRstudio.
//
// A single-module JUCE integration that:
//   1. Owns a juce::AudioDeviceManager for audio output to the sound card.
//   2. Hosts FluidSynth internally for SoundFont-based synthesis.
//   3. Implements prs::MidiSink so it's a drop-in replacement for
//      fluidsynth::FluidSynthOutput (the existing Qt code can switch
//      to AudioEngine with one line).
//   4. Includes a master DSP chain (3-band EQ + brickwall limiter)
//      ported from Cadenza's MasterEq + PolishedMaster.
//   5. Is structured so VST hosting can be added later without
//      changing the public API (just add juce::AudioPluginFormatManager
//      inside the Pimpl).
//
// Threading model:
//   Qt UI thread       — calls set_gain_runtime, set_master_eq_band, etc.
//   Player thread      — calls short_msg / sysex (MidiSink interface).
//                        These queue MIDI into a lock-free ring; the audio
//                        thread drains it before rendering each block.
//   JUCE audio thread  — renders FluidSynth → master DSP → audio device.
//
//   Cross-thread communication uses std::atomic for settings and a
//   mutex-protected std::vector for the MIDI queue (MIDI events are
//   sparse — ~1000/sec max — so a mutex is fine; no need for a
//   lock-free queue yet).
//
// Build requirements:
//   - JUCE 7+ as a CMake subdirectory (add_subdirectory(lib/JUCE))
//   - FluidSynth (already required by PSRstudio)
//   - Link: juce::juce_audio_basics, juce::juce_audio_devices,
//     juce::juce_core (NO GUI modules)

#pragma once

#include "style.hpp"  // prs::MidiSink, prs::u8

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace audio_engine {

// =========================================================
//  AudioEngine — JUCE audio backend (drop-in MidiSink)
// =========================================================
class AudioEngine final : public prs::MidiSink {
public:
    AudioEngine();
    ~AudioEngine() override;

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&)                 = delete;
    AudioEngine& operator=(AudioEngine&&)      = delete;

    // ----- Initialization -----
    // Initialize JUCE AudioDeviceManager and start the audio callback.
    // Call once at startup. Returns false on failure (no audio device,
    // FluidSynth init failed, etc.).
    [[nodiscard]] bool init();

    // Shut down: stop audio callback, free FluidSynth + JUCE resources.
    // Safe to call multiple times.
    void shutdown();

    [[nodiscard]] bool is_initialized() const noexcept {
        return initialized_.load(std::memory_order_acquire);
    }

    // ----- SoundFont management -----
    // Load a .sf2/.sf3 file. Returns the SF ID, or -1 on failure.
    [[nodiscard]] int load_soundfont(const std::string& path);

    // Set the default SoundFont to load on init(). If set before init(),
    // the SF is loaded during init(). If set after, loads immediately.
    void set_default_soundfont(const std::string& path) {
        default_sf_path_ = path;
    }
    [[nodiscard]] std::string_view soundfont_name() const noexcept {
        return default_sf_path_;
    }

    // ----- Audio settings -----
    // All of these can be set before init() (applied during init) or
    // after (applied immediately, thread-safe).
    void set_sample_rate(unsigned hz);
    void set_gain(float gain);
    void set_polyphony(int voices);

    // Real-time gain update — calls fluid_synth_set_gain immediately.
    // Safe to call from the UI thread while audio is running.
    void set_gain_runtime(float gain);

    // ----- Reverb / Chorus (FluidSynth built-in) -----
    void set_reverb(bool on, float room_size = 0.6f, float damping = 0.4f,
                    float width = 0.5f, float level = 0.55f);
    void set_chorus(bool on, int nr = 3, float level = 0.3f,
                    float speed = 0.3f, float depth = 0.4f, int type = 0);

    // ----- Master DSP chain -----
    // The chain is: FluidSynth → 3-band EQ → Brickwall Limiter → output.
    // Disabled by default (matches PSRstudio's current dry output).
    // Enable for the "polished master" sound.

    void set_master_dsp_enabled(bool on);

    // EQ band gains in dB. band: 0 = low shelf (120Hz), 1 = mid peak (900Hz),
    // 2 = high shelf (9kHz). Range: -12 to +12 dB. 0 = flat.
    void set_master_eq_band(int band, float gain_db);

    // Limiter ceiling in dBFS (default -0.27, i.e. 0.97 linear).
    // The limiter prevents any clipping from the EQ + FluidSynth sum.
    void set_limiter_ceiling(float ceiling_db);

    // ----- Diagnostics -----
    [[nodiscard]] float cpu_load() const noexcept;
    [[nodiscard]] int   active_voices() const noexcept;

    // ----- MidiSink interface (called from player thread) -----
    // These queue MIDI into the audio thread's processing ring.
    void short_msg(prs::u8 status, prs::u8 d0, prs::u8 d1) override;
    void short_direct(prs::u8 status, prs::u8 d0, prs::u8 d1) override;
    void sysex(std::span<const prs::u8> payload) override;
    void sysex_nowait(std::span<const prs::u8> data) override;
    void sysex_flush() override;
    void sleep_ms(unsigned ms) override;
    void all_notes_off() override;

private:
    // Pimpl hides all JUCE and FluidSynth headers from this header.
    // Other modules include audio_engine.hpp without needing JUCE installed.
    struct Impl;
    std::unique_ptr<Impl> pimpl_;

    std::atomic<bool> initialized_{false};
    std::string       default_sf_path_;
};

// =========================================================
//  AudioPortInfo — describes one audio output device
// =========================================================
struct AudioPortInfo {
    std::string name;
    std::string type;       // "WASAPI", "DirectSound", "ASIO", etc.
    int         sample_rate; // 0 if unknown
    int         channels;    // 0 if unknown
};

// Enumerate available audio output devices (for the Qt settings UI).
[[nodiscard]] std::vector<AudioPortInfo> enumerate_audio_outputs();

} // namespace audio_engine
