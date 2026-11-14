// trace_chords.cpp — console debug harness for chord-change tracing.
//
// Plays one style section in real time against a logging MidiSink and
// publishes chord changes via chord::g_active_chord, exactly like the
// GUI does. Usage:
//
//   trace_chords.exe <stylePath> <montage 0|1> <change 0|1> <outLog>
//
// Timeline: Main section loops; chord C major published at t=500 ms;
// at t=6500 ms either G major (change=1) or C major again (change=0,
// process_chord_change early-returns -> no retrigger); stop at 13 s.
// The MIDI stream is written to <outLog> as "ms status d0 d1" lines,
// preceded by the source->output channel map (with is_drums flags).
#include "style.hpp"
#include "player.hpp"
#include "playback_controller.hpp"   // brings chmodel::ChannelModel (its own copy)
#include "remap.hpp"
#include "chord_engine.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

std::chrono::steady_clock::time_point g_start;
std::mutex g_log_mtx;

long ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_start)
        .count();
}

class LoggingSink final : public prs::MidiSink {
public:
    explicit LoggingSink(std::ofstream& out) : out_(out) {}

    void short_direct(prs::u8 status, prs::u8 d0, prs::u8 d1) override {
        const std::lock_guard<std::mutex> lock(g_log_mtx);
        out_ << ms_now() << ' ' << int(status) << ' ' << int(d0) << ' '
             << int(d1) << '\n';
    }
    void sysex(std::span<const prs::u8>) override {}
    void sysex_nowait(std::span<const prs::u8>) override {}
    void sysex_flush() override {}
    void sleep_ms(unsigned ms) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    void all_notes_off() override {}

private:
    std::ofstream& out_;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: trace_chords <style> <montage 0|1> <change 0|1> <outLog>\n";
        return 2;
    }
    const bool montage = argv[2][0] == '1';
    const bool change  = argv[3][0] == '1';
    const bool map_only = argv[2][0] == 'm';

    prs::PrsStyle style;
    if (!style.parse(argv[1])) {
        std::cerr << "failed to parse style\n";
        return 1;
    }

    // Pick the first Main section.
    int main_idx = -1;
    for (std::size_t i = 0; i < style.sections().size(); ++i) {
        std::string n = style.sections()[i].name;
        for (auto& c : n) c = char(std::tolower((unsigned char)c));
        if (n.find("main") != std::string::npos) { main_idx = int(i); break; }
    }
    if (main_idx < 0) {
        std::cerr << "no Main section found; sections are:";
        for (const auto& s : style.sections())
            std::cerr << " '" << s.name << "'";
        std::cerr << '\n';
        return 1;
    }
    std::cout << "section: '" << style.sections()[std::size_t(main_idx)].name
              << "'  tempo_us=" << style.tempo_us()
              << " tpqn=" << style.ticks_per_qn() << '\n';

    // Channel map exactly as the player computes it.
    prs::VoiceRemap remap;
    if (montage) {
        if (!remap.load_profile_from_file(
                "C:/Users/DELL/Documents/PSRstudio/remaps/genos2-montage.remap")) {
            std::cerr << "failed to load montage remap\n";
            return 1;
        }
        remap.set_montage_enabled(true);
    }
    std::cout << (montage ? "MONTAGE" : "CASM   ")
              << (change ? "  CHANGE C->G" : "  no change") << "\n";
    for (int ch = 0; ch < 16; ++ch) {
        const prs::CasmChannel* cc =
            style.casm_get_channel(style.sections()[std::size_t(main_idx)].name,
                                   static_cast<prs::u8>(ch));
        const int out = remap.route_channel(static_cast<prs::u8>(ch),
                                            cc ? cc->dst_ch
                                               : static_cast<prs::u8>(ch));
        std::cout << "  src" << ch << " -> out" << out
                  << (cc && cc->is_drums ? "  DRUMS" : "") << '\n';
    }

    chmodel::ChannelModel model;

    // Map-only mode: print the source->output map and exit.
    if (map_only) {
        for (int ch = 0; ch < 16; ++ch) {
            const prs::CasmChannel* cc =
                style.casm_get_channel(style.sections()[std::size_t(main_idx)].name,
                                       static_cast<prs::u8>(ch));
            const int dst = cc ? cc->dst_ch : ch;
            std::cout << "  src" << ch
                      << " casm_dst" << dst
                      << (cc && cc->is_drums ? " DRUMS" : "") << '\n';
        }
        return 0;
    }

    std::ofstream out(argv[4]);
    LoggingSink sink(out);

    g_start = std::chrono::steady_clock::now();
    model.attach_sink_and_sync(&sink);

    player::Player pl;
    std::thread player_thread([&]() {
        pl.play(style, sink, remap, {main_idx}, model);
    });

    // Chord timeline. ChordType::Maj; roots: C=0, G=7.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    chord::g_active_chord.store(0, chord::ChordType::Maj);
    std::this_thread::sleep_for(std::chrono::milliseconds(6000));
    chord::g_active_chord.store(change ? 7 : 0, chord::ChordType::Maj);
    std::this_thread::sleep_for(std::chrono::milliseconds(6000));
    player::Player::request_stop();
    player_thread.join();

    out.flush();
    std::cout << "log written\n";
    return 0;
}
