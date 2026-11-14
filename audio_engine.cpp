// audio_engine.cpp — JUCE audio backend implementation.
// See audio_engine.hpp for the design.

#include "audio_engine.hpp"

#include <fluidsynth.h>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace audio_engine {

// =========================================================
//  Master DSP — biquad filters + brickwall limiter
//
//  Master DSP chain (pure C++,
//  real-time safe). Transposed Direct Form II biquads.
// =========================================================
namespace dsp {

struct Biquad {
    // Coefficients (normalized so a0 = 1)
    float b0 = 1.f, b1 = 0.f, b2 = 0.f;
    float a1 = 0.f, a2 = 0.f;
    // State (TDF-II)
    float z1 = 0.f, z2 = 0.f;

    void reset() noexcept { z1 = z2 = 0.f; }

    [[nodiscard]] float process(float in) noexcept {
        const float out = b0 * in + z1;
        z1 = b1 * in - a1 * out + z2;
        z2 = b2 * in - a2 * out;
        return out;
    }
};

// RBJ cookbook low-shelf coefficients.
Biquad makeLowShelf(float sampleRate, float freqHz, float gainDb) noexcept {
    const float A     = std::pow(10.f, gainDb / 40.f);
    const float w0    = 2.f * 3.14159265358979f * freqHz / sampleRate;
    const float cosw0 = std::cos(w0), sinw0 = std::sin(w0);
    const float alpha = sinw0 / 2.f * std::sqrt((A + 1.f / A) * (1.f / 0.707f - 1.f) + 2.f);
    const float temp  = 2.f * std::sqrt(A) * alpha;

    Biquad b;
    const float a0 =     (A + 1.f) + (A - 1.f) * cosw0 + temp;
    b.b0 =    A * ((A + 1.f) - (A - 1.f) * cosw0 + temp) / a0;
    b.b1 = 2.f * A * ((A - 1.f) - (A + 1.f) * cosw0) / a0;
    b.b2 =    A * ((A + 1.f) - (A - 1.f) * cosw0 - temp) / a0;
    b.a1 = -2.f * ((A - 1.f) + (A + 1.f) * cosw0) / a0;
    b.a2 =     ((A + 1.f) + (A - 1.f) * cosw0 - temp) / a0;
    return b;
}

// RBJ cookbook peaking EQ coefficients.
Biquad makePeak(float sampleRate, float freqHz, float gainDb, float Q) noexcept {
    const float A     = std::pow(10.f, gainDb / 40.f);
    const float w0    = 2.f * 3.14159265358979f * freqHz / sampleRate;
    const float cosw0 = std::cos(w0), sinw0 = std::sin(w0);
    const float alpha = sinw0 / (2.f * Q);

    Biquad b;
    const float a0 = 1.f + alpha / A;
    b.b0 = (1.f + alpha * A) / a0;
    b.b1 = -2.f * cosw0 / a0;
    b.b2 = (1.f - alpha * A) / a0;
    b.a1 = -2.f * cosw0 / a0;
    b.a2 = (1.f - alpha / A) / a0;
    return b;
}

// RBJ cookbook high-shelf coefficients.
Biquad makeHighShelf(float sampleRate, float freqHz, float gainDb) noexcept {
    const float A     = std::pow(10.f, gainDb / 40.f);
    const float w0    = 2.f * 3.14159265358979f * freqHz / sampleRate;
    const float cosw0 = std::cos(w0), sinw0 = std::sin(w0);
    const float alpha = sinw0 / 2.f * std::sqrt((A + 1.f / A) * (1.f / 0.707f - 1.f) + 2.f);
    const float temp  = 2.f * std::sqrt(A) * alpha;

    Biquad b;
    const float a0 =     (A + 1.f) - (A - 1.f) * cosw0 + temp;
    b.b0 =    A * ((A + 1.f) + (A - 1.f) * cosw0 + temp) / a0;
    b.b1 = -2.f * A * ((A - 1.f) + (A + 1.f) * cosw0) / a0;
    b.b2 =    A * ((A + 1.f) + (A - 1.f) * cosw0 - temp) / a0;
    b.a1 =  2.f * ((A - 1.f) - (A + 1.f) * cosw0) / a0;
    b.a2 =     ((A + 1.f) - (A - 1.f) * cosw0 - temp) / a0;
    return b;
}

// Simple brickwall limiter with instantaneous attack and exponential release.
struct Limiter {
    // Atomic so the UI thread can update it while the audio thread runs.
    std::atomic<float> ceiling{0.97f};   // linear (0.97 ≈ -0.27 dBFS)
    float releaseCoeff = 0.999f; // per-sample; ~80ms at 44.1kHz
    float gain = 1.f;

    void reset() noexcept { gain = 1.f; }

    [[nodiscard]] float process(float in) noexcept {
        const float ceilingNow = ceiling.load(std::memory_order_relaxed);
        const float absIn = std::fabs(in);
        if (absIn * gain > ceilingNow) {
            // Instant attack: clamp gain immediately.
            gain = ceilingNow / std::max(absIn, 1e-9f);
        } else {
            // Exponential release back toward 1.0.
            gain += (1.f - gain) * (1.f - releaseCoeff);
        }
        return in * gain;
    }
};

// 3-band EQ + limiter, stereo. All state per-channel.
struct MasterChain {
    Biquad lowShelfL, lowShelfR;
    Biquad midPeakL,  midPeakR;
    Biquad highShelfL, highShelfR;
    Limiter limiterL, limiterR;
    float sampleRate = 44100.f;
    std::atomic<bool> enabled{false};
    std::atomic<float> lowGainDb{0.f}, midGainDb{0.f}, highGainDb{0.f};
    // Set by the UI thread after a gain change; the audio thread rebuilds
    // the (non-atomic) biquad coefficients at the top of process() so they
    // are never swapped mid-block.
    std::atomic<bool> dirty{false};

    void prepare(double sr) {
        sampleRate = static_cast<float>(sr);
        rebuild();
    }

    void rebuild() {
        const float sr = sampleRate;
        lowShelfL  = makeLowShelf (sr, 120.f, lowGainDb.load());
        lowShelfR  = lowShelfL;
        midPeakL   = makePeak    (sr, 900.f, midGainDb.load(), 0.9f);
        midPeakR   = midPeakL;
        highShelfL = makeHighShelf(sr, 9000.f, highGainDb.load());
        highShelfR = highShelfL;
    }

    void reset() {
        lowShelfL.reset(); lowShelfR.reset();
        midPeakL.reset();  midPeakR.reset();
        highShelfL.reset(); highShelfR.reset();
        limiterL.reset(); limiterR.reset();
    }

    void process(float* left, float* right, int numSamples) noexcept {
        // Pick up coefficient changes published by the UI thread (see
        // set_master_eq_band) — rebuild here, on the audio thread.
        if (dirty.exchange(false, std::memory_order_acq_rel)) rebuild();
        if (!enabled.load(std::memory_order_relaxed)) return;
        for (int i = 0; i < numSamples; ++i) {
            float l = lowShelfL.process(left[i]);
            l = midPeakL.process(l);
            l = highShelfL.process(l);
            left[i]  = limiterL.process(l);

            float r = lowShelfR.process(right[i]);
            r = midPeakR.process(r);
            r = highShelfR.process(r);
            right[i] = limiterR.process(r);
        }
    }
};

} // namespace dsp

// =========================================================
//  MIDI queue — player thread → audio thread
//
//  Mutex-protected vector of packed MIDI messages. The player
//  thread pushes (short_msg/sysex); the audio thread drains before
//  rendering each block. MIDI events are sparse (~1k/sec max) so a
//  mutex is fine — no need for a lock-free queue.
// =========================================================
namespace {

struct QueuedMidi {
    // For short messages: status, d0, d1 packed. sysex_data empty.
    // For SysEx: status = 0xF0, sysex_data = payload (without F0/F7).
    prs::u8 status;
    prs::u8 d0;
    prs::u8 d1;
    std::vector<prs::u8> sysex_data;
};

} // namespace

// =========================================================
//  FluidSynth AudioSource — wraps FluidSynth as a JUCE AudioSource
// =========================================================
class FluidSynthAudioSource : public juce::AudioSource {
public:
    // The drain_midi callback is invoked at the top of each audio block,
    // before FluidSynth rendering. It pumps queued MIDI from the player
    // thread into FluidSynth so the synth's state is current for this
    // block's render.
    FluidSynthAudioSource(fluid_synth_t* synth, dsp::MasterChain& chain,
                          std::function<void()> drain_midi)
        : synth_(synth), chain_(chain), drain_midi_(std::move(drain_midi)) {}

    void prepareToPlay(int /*samplesPerBlockExpected*/, double sampleRate) override {
        chain_.prepare(sampleRate);
    }

    void releaseResources() override {
        chain_.reset();
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        // 1. Drain queued MIDI into FluidSynth (player thread → audio thread).
        if (drain_midi_) drain_midi_();

        const int numChannels = info.buffer->getNumChannels();
        const int numSamples  = info.numSamples;

        // 2. Render FluidSynth → planar float scratch buffers.
        // We always render stereo; downmix to mono if the device is mono.
        scratchL_.resize(static_cast<size_t>(numSamples));
        scratchR_.resize(static_cast<size_t>(numSamples));

        if (synth_ != nullptr) {
            fluid_synth_write_float(synth_, numSamples,
                                    scratchL_.data(), 0, 1,
                                    scratchR_.data(), 0, 1);
        } else {
            std::memset(scratchL_.data(), 0, numSamples * sizeof(float));
            std::memset(scratchR_.data(), 0, numSamples * sizeof(float));
        }

        // 3. Apply master DSP chain (EQ + limiter) on the stereo render.
        chain_.process(scratchL_.data(), scratchR_.data(), numSamples);

        // 4. Copy to the output buffer (handle mono/stereo/multi-channel).
        if (numChannels >= 2) {
            std::memcpy(info.buffer->getWritePointer(0, info.startSample),
                        scratchL_.data(), numSamples * sizeof(float));
            std::memcpy(info.buffer->getWritePointer(1, info.startSample),
                        scratchR_.data(), numSamples * sizeof(float));
            for (int ch = 2; ch < numChannels; ++ch) {
                const float* src = (ch % 2 == 0) ? scratchL_.data() : scratchR_.data();
                std::memcpy(info.buffer->getWritePointer(ch, info.startSample),
                            src, numSamples * sizeof(float));
            }
        } else if (numChannels == 1) {
            float* out = info.buffer->getWritePointer(0, info.startSample);
            for (int i = 0; i < numSamples; ++i) {
                out[i] = 0.5f * (scratchL_[i] + scratchR_[i]);
            }
        }
    }

private:
    fluid_synth_t* synth_;
    dsp::MasterChain& chain_;
    std::function<void()> drain_midi_;
    std::vector<float> scratchL_, scratchR_;
};

// =========================================================
//  AudioEngine::Impl — the Pimpl that owns everything
// =========================================================
struct AudioEngine::Impl {
    // ----- JUCE audio -----
    juce::AudioDeviceManager       deviceManager;
    std::unique_ptr<FluidSynthAudioSource> fluidSource;
    std::unique_ptr<juce::AudioSourcePlayer> sourcePlayer;

    // ----- FluidSynth -----
    fluid_settings_t* settings = nullptr;
    fluid_synth_t*    synth    = nullptr;
    int               default_sf_id = -1;

    // ----- Audio settings (atomic, set from UI thread) -----
    std::atomic<unsigned> sampleRate{48000};
    std::atomic<float>    gain{0.5f};
    std::atomic<int>      polyphony{256};

    // ----- MIDI queue (player thread → audio thread) -----
    std::mutex midiMutex;
    std::vector<QueuedMidi> midiQueue;

    // ----- Master DSP chain -----
    dsp::MasterChain masterChain;

    // ----- Constructor / destructor -----
    Impl() {
        // JUCE needs a MessageManager on the UI thread, but we explicitly
        // do NOT use it for audio. We only need the AudioDeviceManager,
        // which doesn't require the MessageManager.
    }

    ~Impl() {
        shutdown_fluidsynth();
    }

    void shutdown_fluidsynth() {
        if (synth)    { delete_fluid_synth(synth);    synth = nullptr; }
        if (settings) { delete_fluid_settings(settings); settings = nullptr; }
        default_sf_id = -1;
    }

    // Drain the MIDI queue and apply to FluidSynth.
    // Called from the audio thread before rendering each block.
    void drain_midi_to_fluidsynth() {
        std::vector<QueuedMidi> local;
        {
            std::lock_guard<std::mutex> lk(midiMutex);
            local.swap(midiQueue);
        }
        if (synth == nullptr) return;
        for (const auto& m : local) {
            if (m.status == 0xF0) {
                // SysEx — reconstruct F0 ... F7 and send.
                std::vector<char> full;
                full.reserve(m.sysex_data.size() + 2);
                full.push_back(static_cast<char>(0xF0));
                full.insert(full.end(),
                            reinterpret_cast<const char*>(m.sysex_data.data()),
                            reinterpret_cast<const char*>(m.sysex_data.data() + m.sysex_data.size()));
                full.push_back(static_cast<char>(0xF7));
                fluid_synth_sysex(synth, full.data(),
                                  static_cast<int>(full.size()),
                                  nullptr, nullptr, nullptr, 0);
            } else {
                send_short_to_fluidsynth(m.status, m.d0, m.d1);
            }
        }
    }

    void send_short_to_fluidsynth(prs::u8 status, prs::u8 d0, prs::u8 d1) {
        if (!synth) return;
        const int ch  = status & 0x0F;
        const int cmd = status >> 4;
        switch (cmd) {
            case 0x8: fluid_synth_noteoff(synth, ch, d0); break;
            case 0x9:
                if (d1 > 0) fluid_synth_noteon(synth, ch, d0, d1);
                else        fluid_synth_noteoff(synth, ch, d0);
                break;
            case 0xA: fluid_synth_key_pressure(synth, ch, d0, d1); break;
            case 0xB: fluid_synth_cc(synth, ch, d0, d1); break;
            case 0xC: fluid_synth_program_change(synth, ch, d0); break;
            case 0xD: fluid_synth_channel_pressure(synth, ch, d0); break;
            case 0xE: fluid_synth_pitch_bend(synth, ch, (d1 << 7) | d0); break;
            default: break;
        }
    }
};

// =========================================================
//  AudioEngine public API
// =========================================================
AudioEngine::AudioEngine()
    : pimpl_(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::init() {
    if (initialized_.load()) return true;
    auto& impl = *pimpl_;

    // ----- 1. Create FluidSynth settings -----
    impl.settings = new_fluid_settings();
    if (!impl.settings) return false;

    // Critical: XG bank-select so Yamaha XG banks resolve correctly
    // (without this, all XG voices collapse to GM piano).
    fluid_settings_setstr(impl.settings, "synth.midi-bank-select", "xg");

    fluid_settings_setnum(impl.settings, "synth.sample-rate",
                          static_cast<double>(impl.sampleRate.load()));
    fluid_settings_setnum(impl.settings, "synth.gain",
                          static_cast<double>(impl.gain.load()));
    fluid_settings_setint(impl.settings, "synth.polyphony",
                          impl.polyphony.load());

    // ----- 2. Create FluidSynth synth -----
    impl.synth = new_fluid_synth(impl.settings);
    if (!impl.synth) {
        delete_fluid_settings(impl.settings);
        impl.settings = nullptr;
        return false;
    }

    // Force channels 9 and 10 as drum channels (XG RHY1/RHY2).
    fluid_synth_set_channel_type(impl.synth, 9, CHANNEL_TYPE_DRUM);
    fluid_synth_set_channel_type(impl.synth, 8, CHANNEL_TYPE_DRUM);

    // ----- 3. Load default SoundFont if set -----
    if (!default_sf_path_.empty()) {
        const int sf_id = fluid_synth_sfload(impl.synth, default_sf_path_.c_str(), 1);
        if (sf_id == FLUID_FAILED) {
            // Non-fatal — user can load an SF later.
        } else {
            impl.default_sf_id = sf_id;
        }
    }

    // ----- 4. Set up JUCE audio device manager -----
    // Use no audio input, stereo output. Let JUCE pick the default device.
    const auto err = impl.deviceManager.initialise(
        0,        // numInputChannelsNeeded
        2,        // numOutputChannelsNeeded
        nullptr,  // savedState
        true      // selectDefaultDeviceOnFailure
    );
    if (err.isNotEmpty()) {
        // Audio device init failed — we can still run headless (no audio output),
        // useful for testing. Log and continue.
        impl.shutdown_fluidsynth();
        return false;
    }

    // Sync the actual sample rate back into our atomic.
    // (non-const pointer: getCurrentSampleRate() is a non-const virtual)
    auto* device = impl.deviceManager.getCurrentAudioDevice();
    if (device) {
        impl.sampleRate.store(static_cast<unsigned>(device->getCurrentSampleRate()),
                              std::memory_order_relaxed);
    }

    // ----- 5. Wire FluidSynth → AudioSource → AudioSourcePlayer → device -----
    impl.masterChain.prepare(static_cast<double>(impl.sampleRate.load()));
    impl.fluidSource = std::make_unique<FluidSynthAudioSource>(
        impl.synth, impl.masterChain,
        [this]() { pimpl_->drain_midi_to_fluidsynth(); });
    impl.sourcePlayer = std::make_unique<juce::AudioSourcePlayer>();
    impl.sourcePlayer->setSource(impl.fluidSource.get());

    impl.deviceManager.addAudioCallback(impl.sourcePlayer.get());

    initialized_.store(true, std::memory_order_release);
    return true;
}

void AudioEngine::shutdown() {
    if (!initialized_.load()) return;
    auto& impl = *pimpl_;

    if (impl.sourcePlayer) {
        impl.deviceManager.removeAudioCallback(impl.sourcePlayer.get());
        impl.sourcePlayer->setSource(nullptr);
        impl.sourcePlayer.reset();
    }
    impl.fluidSource.reset();
    impl.deviceManager.closeAudioDevice();
    impl.shutdown_fluidsynth();

    initialized_.store(false, std::memory_order_release);
}

// ----- SoundFont -----

int AudioEngine::load_soundfont(const std::string& path) {
    if (!pimpl_->synth) return -1;
    const int sf_id = fluid_synth_sfload(pimpl_->synth, path.c_str(), 1);
    if (sf_id == FLUID_FAILED) return -1;
    if (pimpl_->default_sf_id < 0) pimpl_->default_sf_id = sf_id;
    return sf_id;
}

// ----- Audio settings -----

void AudioEngine::set_sample_rate(unsigned hz) {
    pimpl_->sampleRate.store(hz);
    // Applied on next init() or already applied if running (FluidSynth
    // can't change sample rate at runtime — requires restart).
}

void AudioEngine::set_gain(float g) {
    pimpl_->gain.store(g);
    if (pimpl_->synth) {
        fluid_synth_set_gain(pimpl_->synth, static_cast<double>(g));
    }
}

void AudioEngine::set_polyphony(int voices) {
    pimpl_->polyphony.store(voices);
    if (pimpl_->synth) {
        fluid_synth_set_polyphony(pimpl_->synth, voices);
    }
}

void AudioEngine::set_gain_runtime(float g) {
    set_gain(g);  // set_gain already calls fluid_synth_set_gain immediately
}

void AudioEngine::set_reverb(bool on, float room_size, float damping,
                              float width, float level) {
    if (!pimpl_->synth) return;
    fluid_synth_set_reverb_on(pimpl_->synth, on ? 1 : 0);
    if (on) {
        fluid_synth_set_reverb(pimpl_->synth, room_size, damping, width, level);
    }
}

void AudioEngine::set_chorus(bool on, int nr, float level,
                              float speed, float depth, int type) {
    if (!pimpl_->synth) return;
    fluid_synth_set_chorus_on(pimpl_->synth, on ? 1 : 0);
    if (on) {
        fluid_synth_set_chorus(pimpl_->synth, nr, level, speed, depth, type);
    }
}

// ----- Master DSP -----

void AudioEngine::set_master_dsp_enabled(bool on) {
    pimpl_->masterChain.enabled.store(on, std::memory_order_relaxed);
}

void AudioEngine::set_master_eq_band(int band, float gain_db) {
    auto& chain = pimpl_->masterChain;
    gain_db = std::clamp(gain_db, -12.f, 12.f);
    switch (band) {
        case 0: chain.lowGainDb.store(gain_db);  break;
        case 1: chain.midGainDb.store(gain_db);  break;
        case 2: chain.highGainDb.store(gain_db); break;
        default: return;
    }
    // Publish the change: the audio thread rebuilds the coefficients at
    // the top of the next process() call (they are not atomic, so they
    // must only ever be touched on the audio thread).
    chain.dirty.store(true, std::memory_order_release);
}

void AudioEngine::set_limiter_ceiling(float ceiling_db) {
    const float linear = std::pow(10.f, ceiling_db / 20.f);
    pimpl_->masterChain.limiterL.ceiling.store(linear, std::memory_order_relaxed);
    pimpl_->masterChain.limiterR.ceiling.store(linear, std::memory_order_relaxed);
}

// ----- Diagnostics -----

float AudioEngine::cpu_load() const noexcept {
    if (!pimpl_->synth) return 0.f;
    return static_cast<float>(fluid_synth_get_cpu_load(pimpl_->synth));
}

int AudioEngine::active_voices() const noexcept {
    if (!pimpl_->synth) return 0;
    return fluid_synth_get_active_voice_count(pimpl_->synth);
}

// ----- MidiSink interface -----

void AudioEngine::short_msg(prs::u8 status, prs::u8 d0, prs::u8 d1) {
    // Queue for the audio thread.
    QueuedMidi m;
    m.status = status;
    m.d0 = d0;
    m.d1 = d1;
    {
        std::lock_guard<std::mutex> lk(pimpl_->midiMutex);
        pimpl_->midiQueue.push_back(std::move(m));
    }
}

void AudioEngine::short_direct(prs::u8 status, prs::u8 d0, prs::u8 d1) {
    // "Direct" = apply immediately (used for panic / note-offs on stop).
    // Still safe — FluidSynth's API is thread-safe.
    pimpl_->send_short_to_fluidsynth(status, d0, d1);
}

void AudioEngine::sysex(std::span<const prs::u8> payload) {
    QueuedMidi m;
    m.status = 0xF0;
    m.sysex_data.assign(payload.begin(), payload.end());
    {
        std::lock_guard<std::mutex> lk(pimpl_->midiMutex);
        pimpl_->midiQueue.push_back(std::move(m));
    }
}

void AudioEngine::sysex_nowait(std::span<const prs::u8> data) {
    if (!pimpl_->synth) return;
    // Reconstruct F0 ... F7 and send immediately.
    std::vector<char> full;
    full.reserve(data.size() + 2);
    full.push_back(static_cast<char>(0xF0));
    full.insert(full.end(),
                reinterpret_cast<const char*>(data.data()),
                reinterpret_cast<const char*>(data.data() + data.size()));
    full.push_back(static_cast<char>(0xF7));
    fluid_synth_sysex(pimpl_->synth, full.data(),
                      static_cast<int>(full.size()),
                      nullptr, nullptr, nullptr, 0);
}

void AudioEngine::sysex_flush() {
    // No-op — FluidSynth processes SysEx immediately.
}

void AudioEngine::sleep_ms(unsigned ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void AudioEngine::all_notes_off() {
    if (!pimpl_->synth) return;
    for (int ch = 0; ch < 16; ++ch) {
        fluid_synth_cc(pimpl_->synth, ch, 123, 0);
        fluid_synth_cc(pimpl_->synth, ch, 120, 0);
    }
}

// =========================================================
//  AudioPortInfo enumeration
// =========================================================
std::vector<AudioPortInfo> enumerate_audio_outputs() {
    juce::AudioDeviceManager dm;
    auto* deviceType = dm.getCurrentDeviceTypeObject();
    std::vector<AudioPortInfo> out;
    if (deviceType == nullptr) {
        // Scan all device types.
        for (auto* type : dm.getAvailableDeviceTypes()) {
            for (const auto& name : type->getDeviceNames()) {
                AudioPortInfo info;
                info.name = name.toStdString();
                info.type = type->getTypeName().toStdString();
                info.sample_rate = 0;
                info.channels = 0;
                out.push_back(std::move(info));
            }
        }
    }
    return out;
}

} // namespace audio_engine
