// fluidsynth_output.hpp — FluidSynth software synthesizer output.
//
// Implements prs::MidiSink using the FluidSynth library, allowing
// style playback through SoundFonts (.sf2/.sf3) without a hardware
// MIDI device.
//
// Architecture:
//
//   Player thread -> short_msg()/sysex() -> FluidSynth (thread-safe API)
//                                            |
//                                            v
//                                    Audio driver thread
//                                    (FluidSynth manages this)
//                                            |
//                                            v
//                                    Sound card (WASAPI/DirectSound)
//
// FluidSynth's API is already thread-safe — fluid_synth_noteon/cc/pc
// can be called from any thread. The synth has its own audio callback
// that renders samples. So we DON'T need our own ring buffer or
// output thread (unlike WinMidiOutput).
//
// Usage:
//   fluidsynth::FluidSynthOutput synth;
//   synth.set_soundfont("C:/GeneralUser GS.sf2");
//   synth.open();
//   synth.start_audio();
//   synth.short_msg(0x90, 60, 100);  // note on
#pragma once

#include "style.hpp"  // for prs::MidiSink, prs::u8

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fluidsynth {

// =========================================================
//  SoundFontInfo — describes one loaded SoundFont
// =========================================================
struct SoundFontInfo {
    int         id;       // FluidSynth SF ID
    std::string name;     // file path or display name
    int         bank_count;
    int         preset_count;
};

// =========================================================
//  AudioDriver — available audio backends
// =========================================================
enum class AudioDriver {
    Default,     // let FluidSynth choose
    DSound,      // DirectSound (Windows)
    Wasapi,      // WASAPI (Windows, low-latency)
    WaveOut,     // waveOut (Windows, legacy)
    Alsa,        // ALSA (Linux)
    PulseAudio,  // PulseAudio (Linux)
    CoreAudio,   // CoreAudio (macOS)
};

// =========================================================
//  FluidSynthOutput — software synth implementing prs::MidiSink
// =========================================================
class FluidSynthOutput final : public prs::MidiSink {
public:
    FluidSynthOutput();
    ~FluidSynthOutput() override;

    FluidSynthOutput(const FluidSynthOutput&)            = delete;
    FluidSynthOutput& operator=(const FluidSynthOutput&) = delete;
    FluidSynthOutput(FluidSynthOutput&&)                 = delete;
    FluidSynthOutput& operator=(FluidSynthOutput&&)      = delete;

    // ----- SoundFont management -----
    // Load a .sf2/.sf3 file. Returns the SF ID, or -1 on failure.
    // Multiple SoundFonts can be loaded — later ones override earlier
    // presets for the same bank/program.
    int load_soundfont(const std::string& path);

    // Unload a SoundFont by ID.
    bool unload_soundfont(int sf_id);

    // Get all loaded SoundFonts.
    [[nodiscard]] std::vector<SoundFontInfo> loaded_soundfonts() const;

    // Set the default SoundFont to use (convenience: loads + selects).
    // Call before open().
    void set_soundfont(const std::string& path) { soundfont_path_ = path; }

    // ----- Audio driver -----
    // Set the audio driver. Call before open().
    void set_audio_driver(AudioDriver driver) { audio_driver_ = driver; }

    // ----- Open / close -----
    // Initialize FluidSynth, load SoundFont, start audio driver.
    [[nodiscard]] bool open();

    // Stop audio driver and free FluidSynth resources.
    void close();

    [[nodiscard]] bool is_open() const noexcept { return synth_ != nullptr; }

    // ----- Thread management -----
    // No-op — FluidSynth manages its own audio thread.
    // Provided for API compatibility with WinMidiOutput.
    void start_thread() {}
    void stop_thread() {}

    // ----- Audio settings -----
    void set_sample_rate(unsigned hz) { sample_rate_ = hz; }
    void set_gain(float gain) { gain_ = gain; }
    void set_polyphony(int voices) { polyphony_ = voices; }

    // Apply a new gain value to a running synth in real time.
    // Safe to call before open() (no-op) or after (calls
    // fluid_synth_set_gain immediately). Also updates gain_ so a
    // subsequent open() uses the new value.
    void set_gain_runtime(float gain);

    // ----- Reverb / Chorus -----
    void set_reverb(bool on, float room_size = 0.2f, float damping = 0.0f,
                    float width = 0.5f, float level = 0.9f);
    void set_chorus(bool on, int nr = 3, float level = 0.3f,
                    float speed = 0.3f, float depth = 0.4f,
                    int type = 0);

    // ----- MidiSink interface -----
    void short_msg(prs::u8 status, prs::u8 d0, prs::u8 d1) override;
    void short_direct(prs::u8 status, prs::u8 d0, prs::u8 d1) override;
    void sysex(std::span<const prs::u8> payload) override;
    void sysex_nowait(std::span<const prs::u8> data) override;
    void sysex_flush() override;
    void sleep_ms(unsigned ms) override;
    void all_notes_off() override;

    // ----- Diagnostics -----
    [[nodiscard]] std::string_view soundfont_name() const noexcept {
        return soundfont_path_;
    }

    // Get the current CPU load (0.0 - 1.0). Only valid while open.
    [[nodiscard]] float cpu_load() const noexcept;

    // Get the number of active voices.
    [[nodiscard]] int active_voices() const noexcept;

private:
    // Opaque FluidSynth handles (defined as void* to avoid pulling
    // fluidsynth.h into the header)
    void* settings_  = nullptr;  // fluid_settings_t*
    void* synth_     = nullptr;  // fluid_synth_t*
    void* adriver_   = nullptr;  // fluid_audio_driver_t*

    std::string soundfont_path_;
    AudioDriver audio_driver_ = AudioDriver::Default;
    unsigned    sample_rate_  = 44100;
    float       gain_         = 0.5f;
    int         polyphony_    = 256;

    // Send a packed MIDI message to FluidSynth
    void send_midi_event(prs::u8 status, prs::u8 d0, prs::u8 d1);
};

} // namespace fluidsynth