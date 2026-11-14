// fluidsynth_output.cpp — FluidSynth software synthesizer output.
#include "fluidsynth_output.hpp"

#include <fluidsynth.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <iostream>
#include <thread>
#include <chrono>

namespace fluidsynth {

// =========================================================
//  Constructor / destructor
// =========================================================
FluidSynthOutput::FluidSynthOutput() = default;

FluidSynthOutput::~FluidSynthOutput() {
    close();
}

// =========================================================
//  SoundFont management
// =========================================================
int FluidSynthOutput::load_soundfont(const std::string& path) {
    if (!synth_) {
        std::cerr << "[fluidsynth] Cannot load SoundFont before open()\n";
        return -1;
    }

    const int sf_id = fluid_synth_sfload(
        static_cast<fluid_synth_t*>(synth_), path.c_str(), 1);

    if (sf_id == FLUID_FAILED) {
        std::cerr << std::format("[fluidsynth] Failed to load SoundFont: {}\n", path);
        return -1;
    }

    std::cout << std::format("[fluidsynth] Loaded SoundFont '{}' (ID={})\n", path, sf_id);
    return sf_id;
}

bool FluidSynthOutput::unload_soundfont(int sf_id) {
    if (!synth_) return false;
    return fluid_synth_sfunload(static_cast<fluid_synth_t*>(synth_), sf_id, 1) == FLUID_OK;
}

std::vector<SoundFontInfo> FluidSynthOutput::loaded_soundfonts() const {
    std::vector<SoundFontInfo> out;
    if (!synth_) return out;

    int count = fluid_synth_sfcount(static_cast<fluid_synth_t*>(synth_));
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        fluid_sfont_t* sf = fluid_synth_get_sfont(
            static_cast<fluid_synth_t*>(synth_), i);
        if (sf) {
            SoundFontInfo info;
            info.id = i;
            info.name = fluid_sfont_get_name(sf);
            info.bank_count = 0;
            info.preset_count = 0;
            out.push_back(std::move(info));
        }
    }
    return out;
}

// =========================================================
//  Open / close
// =========================================================
bool FluidSynthOutput::open() {
    if (synth_) return true;  // already open

    // ----- Create settings -----
    settings_ = new_fluid_settings();
    if (!settings_) {
        std::cerr << "[fluidsynth] Failed to create settings\n";
        return false;
    }

    // Audio driver
    const char* driver_name = nullptr;
    switch (audio_driver_) {
        case AudioDriver::DSound:     driver_name = "dsound";      break;
        case AudioDriver::Wasapi:     driver_name = "wasapi";      break;
        case AudioDriver::WaveOut:    driver_name = "waveout";     break;
        case AudioDriver::Alsa:       driver_name = "alsa";        break;
        case AudioDriver::PulseAudio: driver_name = "pulseaudio";  break;
        case AudioDriver::CoreAudio:  driver_name = "coreaudio";   break;
        default:                      driver_name = nullptr;        break;
    }
    if (driver_name) {
        fluid_settings_setstr(static_cast<fluid_settings_t*>(settings_),
            "audio.driver", driver_name);
    }

    // Sample rate
    fluid_settings_setnum(static_cast<fluid_settings_t*>(settings_),
        "synth.sample-rate", static_cast<double>(sample_rate_));

    // Gain
    fluid_settings_setnum(static_cast<fluid_settings_t*>(settings_),
        "synth.gain", static_cast<double>(gain_));

    // Polyphony
    fluid_settings_setint(static_cast<fluid_settings_t*>(settings_),
        "synth.polyphony", polyphony_);

    // ----- Create synthesizer -----
    synth_ = new_fluid_synth(static_cast<fluid_settings_t*>(settings_));
    if (!synth_) {
        std::cerr << "[fluidsynth] Failed to create synthesizer\n";
        delete_fluid_settings(static_cast<fluid_settings_t*>(settings_));
        settings_ = nullptr;
        return false;
    }

    // ----- Load SoundFont -----
    if (!soundfont_path_.empty()) {
        const int sf_id = fluid_synth_sfload(
            static_cast<fluid_synth_t*>(synth_), soundfont_path_.c_str(), 1);
        if (sf_id == FLUID_FAILED) {
            std::cerr << std::format("[fluidsynth] Warning: Failed to load SoundFont '{}'\n",
                soundfont_path_);
            // Continue anyway — user can load SF later
        } else {
            std::cout << std::format("[fluidsynth] Loaded SoundFont '{}' (ID={})\n",
                soundfont_path_, sf_id);
        }
    }

    // ----- Start audio driver -----
    adriver_ = new_fluid_audio_driver(
        static_cast<fluid_settings_t*>(settings_),
        static_cast<fluid_synth_t*>(synth_));
    if (!adriver_) {
        std::cerr << "[fluidsynth] Warning: Failed to start audio driver.\n";
        std::cerr << "[fluidsynth] Synth is running in headless mode (no audio output).\n";
        // Not fatal — useful for testing
    } else {
        std::cout << std::format("[fluidsynth] Audio driver started ({} Hz, gain={:.1f})\n",
            sample_rate_, gain_);
    }

    return true;
}

void FluidSynthOutput::close() {
    if (adriver_) {
        delete_fluid_audio_driver(static_cast<fluid_audio_driver_t*>(adriver_));
        adriver_ = nullptr;
    }
    if (synth_) {
        delete_fluid_synth(static_cast<fluid_synth_t*>(synth_));
        synth_ = nullptr;
    }
    if (settings_) {
        delete_fluid_settings(static_cast<fluid_settings_t*>(settings_));
        settings_ = nullptr;
    }
}

// =========================================================
//  Reverb / Chorus
// =========================================================
void FluidSynthOutput::set_gain_runtime(float gain) {
    gain_ = gain;
    if (synth_) {
        fluid_synth_set_gain(static_cast<fluid_synth_t*>(synth_),
                             static_cast<double>(gain));
    }
}

void FluidSynthOutput::set_reverb(bool on, float room_size, float damping,
                                   float width, float level) {
    if (!synth_) return;
    fluid_synth_set_reverb_on(static_cast<fluid_synth_t*>(synth_), on ? 1 : 0);
    if (on) {
        fluid_synth_set_reverb(static_cast<fluid_synth_t*>(synth_),
            room_size, damping, width, level);
    }
}

void FluidSynthOutput::set_chorus(bool on, int nr, float level,
                                  float speed, float depth, int type) {
    if (!synth_) return;
    fluid_synth_set_chorus_on(static_cast<fluid_synth_t*>(synth_), on ? 1 : 0);
    if (on) {
        fluid_synth_set_chorus(static_cast<fluid_synth_t*>(synth_),
            nr, level, speed, depth, type);
    }
}

// =========================================================
//  Internal: send MIDI event to FluidSynth
// =========================================================
void FluidSynthOutput::send_midi_event(prs::u8 status, prs::u8 d0, prs::u8 d1) {
    if (!synth_) return;

    const int ch = status & 0x0F;
    const int cmd = status >> 4;

    switch (cmd) {
        case 0x8:  // Note Off
            fluid_synth_noteoff(static_cast<fluid_synth_t*>(synth_), ch, d0);
            break;
        case 0x9:  // Note On
            if (d1 > 0)
                fluid_synth_noteon(static_cast<fluid_synth_t*>(synth_), ch, d0, d1);
            else
                fluid_synth_noteoff(static_cast<fluid_synth_t*>(synth_), ch, d0);
            break;
        case 0xA:  // Polyphonic Aftertouch
            fluid_synth_key_pressure(static_cast<fluid_synth_t*>(synth_), ch, d0, d1);
            break;
        case 0xB:  // Control Change
            fluid_synth_cc(static_cast<fluid_synth_t*>(synth_), ch, d0, d1);
            break;
        case 0xC:  // Program Change
            fluid_synth_program_change(static_cast<fluid_synth_t*>(synth_), ch, d0);
            break;
        case 0xD:  // Channel Pressure
            fluid_synth_channel_pressure(static_cast<fluid_synth_t*>(synth_), ch, d0);
            break;
        case 0xE:  // Pitch Bend
            fluid_synth_pitch_bend(static_cast<fluid_synth_t*>(synth_), ch,
                (d1 << 7) | d0);
            break;
        default:
            break;
    }
}

// =========================================================
//  MidiSink interface
// =========================================================
void FluidSynthOutput::short_msg(prs::u8 status, prs::u8 d0, prs::u8 d1) {
    send_midi_event(status, d0, d1);
}

void FluidSynthOutput::short_direct(prs::u8 status, prs::u8 d0, prs::u8 d1) {
    send_midi_event(status, d0, d1);
}

void FluidSynthOutput::sysex(std::span<const prs::u8> payload) {
    if (!synth_ || payload.empty()) return;

    // FluidSynth expects the full SysEx including F0 and F7
    std::vector<char> full_msg;
    full_msg.reserve(payload.size() + 2);
    full_msg.push_back(static_cast<char>(0xF0));
    full_msg.insert(full_msg.end(),
        reinterpret_cast<const char*>(payload.data()),
        reinterpret_cast<const char*>(payload.data() + payload.size()));
    full_msg.push_back(static_cast<char>(0xF7));

    fluid_synth_sysex(static_cast<fluid_synth_t*>(synth_),
        full_msg.data(), static_cast<int>(full_msg.size()),
        nullptr, nullptr, nullptr, 0);
}

void FluidSynthOutput::sysex_nowait(std::span<const prs::u8> data) {
    sysex(data);
}

void FluidSynthOutput::sysex_flush() {
    // No-op — FluidSynth processes SysEx immediately
}

void FluidSynthOutput::sleep_ms(unsigned ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void FluidSynthOutput::all_notes_off() {
    if (!synth_) return;
    for (int ch = 0; ch < 16; ++ch) {
        fluid_synth_cc(static_cast<fluid_synth_t*>(synth_), ch, 123, 0);
        fluid_synth_cc(static_cast<fluid_synth_t*>(synth_), ch, 120, 0);
    }
}

// =========================================================
//  Diagnostics
// =========================================================
float FluidSynthOutput::cpu_load() const noexcept {
    if (!synth_) return 0.0f;
    return static_cast<float>(fluid_synth_get_cpu_load(
        static_cast<fluid_synth_t*>(synth_)));
}

int FluidSynthOutput::active_voices() const noexcept {
    if (!synth_) return 0;
    return fluid_synth_get_active_voice_count(
        static_cast<fluid_synth_t*>(synth_));
}

} // namespace fluidsynth