// midi_player.cpp — Self-contained Standard MIDI File player.
// C++20 rewrite of midi_player.c. See midi_player.hpp for design notes.

#include "midi_player.hpp"
#include "midi_output.hpp"
#include "channel_model.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <format>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace midi_player {

using prs::u8;
using prs::u32;

// =========================================================
//  Destructor
// =========================================================
MidiPlayer::~MidiPlayer() {
    unload();
}

// =========================================================
//  load — parse SMF into internal storage
// =========================================================
Result MidiPlayer::load(const std::string& path) {
    unload();

    // ----- Read file -----
    // On Windows, std::ifstream doesn't open UTF-8 paths correctly.
    // Convert to wchar_t for MSVC.
    std::ifstream f;
#ifdef _WIN32
    // Convert UTF-8 to UTF-16 for Windows file I/O
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
    f.open(wpath, std::ios::binary | std::ios::ate);
#else
    f.open(path, std::ios::binary | std::ios::ate);
#endif
    if (!f) return Result::ErrFile;
    const std::streamoff fsz = f.tellg();
    if (fsz < 14) return Result::ErrFormat;
    std::vector<u8> buf(static_cast<std::size_t>(fsz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), fsz);
    if (!f) return Result::ErrFile;

    // ----- Validate MThd -----
    if (std::memcmp(buf.data(), "MThd", 4) != 0) return Result::ErrFormat;
    // Skip the "MThd" tag — start reading from byte 4
    prs::ByteReader r(buf);
    r.seek(4);
    const u32 hdr_len    = r.read_be32();  // should be 6
    const int smf_fmt    = r.read_be16();
    const int num_tracks = r.read_be16();
    const int tpq_raw    = r.read_be16();

    if (hdr_len < 6 || num_tracks < 1) return Result::ErrFormat;
    if (tpq_raw & 0x8000) return Result::ErrFormat;  // SMPTE not supported

    const int tpq = tpq_raw;

    // ----- Parse all tracks -----
    std::size_t pos = 8 + hdr_len;
    std::uint32_t format2_tick_offset = 0;

    for (int t = 0; t < num_tracks && pos + 8 <= buf.size(); ++t) {
        if (std::memcmp(buf.data() + pos, "MTrk", 4) != 0) break;
        const u8* q = buf.data() + pos + 4;
        const u32 trk_len2 = (static_cast<u32>(q[0]) << 24) |
                             (static_cast<u32>(q[1]) << 16) |
                             (static_cast<u32>(q[2]) <<  8) |
                              static_cast<u32>(q[3]);
        const std::size_t trk_start = pos + 8;
        const std::size_t trk_end   = (std::min)(trk_start + trk_len2, buf.size());

        const u32 offset = (smf_fmt == 2) ? format2_tick_offset : 0;

        prs::ByteReader trk_reader(std::span<const u8>(
            buf.data() + trk_start, trk_end - trk_start));
        Result res = parse_track(trk_reader, offset);
        if (res != Result::Ok) return res;

        if (smf_fmt == 2) {
            u32 max_tick = 0;
            for (const auto& ev : events_) {
                u32 local = ev.tick - offset;
                if (local > max_tick) max_tick = local;
            }
            format2_tick_offset += max_tick + static_cast<u32>(tpq);
        }

        pos = trk_end;
    }

    if (events_.empty()) return Result::ErrFormat;

    // ----- Sort by tick (tempo events first on tie) -----
    std::ranges::sort(events_, [](const prs::Event& a, const prs::Event& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.kind == prs::EventKind::Tempo && b.kind != prs::EventKind::Tempo) return true;
        if (b.kind == prs::EventKind::Tempo && a.kind != prs::EventKind::Tempo) return false;
        return false;
    });

    // ----- Build tempo map for duration + fire-time computation -----
    struct TempoSeg { u32 tick; u32 tempo_us; };
    std::vector<TempoSeg> tmap;
    tmap.push_back({0, 500000});
    u32 initial_tempo = 500000;

    for (std::size_t i = 0; i < events_.size(); ++i) {
        const auto& ev = events_[i];
        if (ev.kind == prs::EventKind::Tempo) {
            if (ev.status == 0xFE) {
                // Time signature event
                u8 nn = ev.d0;
                u8 dd_pow = ev.d1;
                u8 dd = 1;
                while (dd < dd_pow) dd <<= 1;
                if (nn > 0 && dd > 0) {
                    info_.ts_num = nn;
                    info_.ts_den = dd;
                }
            } else {
                u32 t = (static_cast<u32>(ev.d0) << 16) |
                        (static_cast<u32>(ev.d1) <<  8) |
                         static_cast<u32>(ev.status);
                if (i == 0 || (i < 16 && tmap.size() == 1)) initial_tempo = t;
                tmap.push_back({ev.tick, t});
            }
        }
    }

    // ----- Compute total duration in seconds -----
    const u32 last_tick = events_.back().tick;
    double dur_s = 0.0;
    for (std::size_t s = 0; s < tmap.size(); ++s) {
        u32 seg_start = tmap[s].tick;
        u32 seg_end = (s + 1 < tmap.size()) ? tmap[s + 1].tick : last_tick;
        if (seg_end <= seg_start) continue;
        double beats = static_cast<double>(seg_end - seg_start) / static_cast<double>(tpq);
        dur_s += beats * (static_cast<double>(tmap[s].tempo_us) / 1000000.0);
    }

    // ----- Pre-compute fire times (microseconds from start) -----
    fire_us_.resize(events_.size());
    std::int64_t cumul_us = 0;
    std::size_t tmap_seg = 0;
    u32 prev_tick = 0;

    for (std::size_t i = 0; i < events_.size(); ++i) {
        const u32 tick = events_[i].tick;

        while (tmap_seg + 1 < tmap.size() && tmap[tmap_seg + 1].tick <= tick)
            ++tmap_seg;

        if (tick > prev_tick) {
            std::int64_t span_ticks = static_cast<std::int64_t>(tick - prev_tick);
            std::int64_t span_us = span_ticks *
                static_cast<std::int64_t>(tmap[tmap_seg].tempo_us) / tpq;
            cumul_us += span_us;
        }

        fire_us_[i] = cumul_us;
        prev_tick = tick;
    }

    // ----- Total bars -----
    double ticks_per_bar = static_cast<double>(tpq) * static_cast<double>(info_.ts_num);
    int total_bars = (ticks_per_bar > 0)
        ? static_cast<int>(static_cast<double>(last_tick) / ticks_per_bar) + 1 : 0;

    // ----- Fill info -----
    // basename only
    std::string basename = path;
    std::size_t slash = basename.find_last_of("/\\");
    if (slash != std::string::npos) basename = basename.substr(slash + 1);

    info_.filename     = basename;
    info_.format       = smf_fmt;
    info_.num_tracks   = num_tracks;
    info_.ticks_per_qn = tpq;
    info_.tempo_us     = initial_tempo;
    // Note: info_.ts_num and info_.ts_den were already set during
    // time-signature parsing (lines ~131-134). Do NOT overwrite them
    // with defaults here — that would clobber 3/4, 6/8, etc.
    if (info_.ts_num == 0) info_.ts_num = 4;  // fallback if never set
    if (info_.ts_den == 0) info_.ts_den = 4;
    info_.event_count  = static_cast<int>(events_.size());
    info_.duration_sec = dur_s;

    total_bars_.store(total_bars, std::memory_order_relaxed);
    loaded_ = true;

    std::cout << std::format("[MID] '{}'  fmt={}  tracks={}  events={}  dur={:.1f}s  {} bars\n",
        basename, smf_fmt, num_tracks, events_.size(), dur_s, total_bars);
    std::cout << std::format("[MID] Initial tempo {:.1f} BPM  TPQ={}  TS={}/{}\n",
        60000000.0 / initial_tempo, tpq, 4, 4);

    return Result::Ok;
}

// =========================================================
//  parse_track — parse one MTrk chunk into events_/sx_pool_
// =========================================================
Result MidiPlayer::parse_track(prs::ByteReader& r, std::uint32_t abs_tick_offset) {
    std::uint32_t abs_tick = abs_tick_offset;
    u8 running = 0;

    while (r.position() < r.size()) {
        abs_tick += r.read_vlq();
        if (r.eof()) break;

        const u8 sb = r.read_u8();

        // --- Meta event ---
        if (sb == 0xFF) {
            const u8 mtype = r.read_u8();
            const u32 mlen = r.read_vlq();
            if (!r.can_read(mlen)) break;
            const auto md = r.read_bytes(mlen);

            if (mtype == 0x51 && mlen == 3) {
                prs::Event& ev = events_.emplace_back();
                ev.tick    = abs_tick;
                ev.kind    = prs::EventKind::Tempo;
                ev.d0      = md[0];
                ev.d1      = md[1];
                ev.status  = md[2];
            } else if (mtype == 0x58 && mlen == 4) {
                // Time signature: nn, dd, cc, bb
                // nn = numerator, dd = denominator (power of 2), cc = clocks per tick, bb = 32nd notes per quarter
                // Store in the next event's status field as nn | (dd << 4)
                // We'll use it to set info_.ts_num and ts_den
                // For now, store as a Tempo event with special encoding
                prs::Event& ev = events_.emplace_back();
                ev.tick    = abs_tick;
                ev.kind    = prs::EventKind::Tempo;  // reuse Tempo kind for time-sig
                ev.status  = 0xFE;  // marker for time signature
                ev.d0      = md[0];  // numerator
                ev.d1      = md[1];  // denominator (power of 2)
            } else if (mtype == 0x2F) {
                break;  // end of track
            }
            continue;
        }

        // --- SysEx ---
        if (sb == 0xF0 || sb == 0xF7) {
            const u32 slen = r.read_vlq();
            if (!r.can_read(slen)) break;
            const auto sd = r.read_bytes(slen);

            prs::Event& ev = events_.emplace_back();
            ev.tick      = abs_tick;
            ev.kind      = prs::EventKind::SysEx;
            ev.status    = sb;
            ev.sx_offset = static_cast<u32>(sx_pool_.size());
            ev.sx_len    = static_cast<prs::u16>((std::min<u32>)(slen, 65535));
            sx_pool_.insert(sx_pool_.end(), sd.begin(), sd.end());
            continue;
        }

        // --- Channel message ---
        u8 cmd;
        u8 status;
        if (sb & 0x80) {
            running = sb;
            cmd     = sb >> 4;
            status  = sb;
        } else {
            r.seek(r.position() - 1);
            cmd    = running >> 4;
            status = running;
        }

        u8 b0 = 0, b1 = 0;
        if (cmd == 0x8 || cmd == 0x9 || cmd == 0xA ||
            cmd == 0xB || cmd == 0xE) {
            b0 = r.read_u8();
            b1 = r.read_u8();
        } else if (cmd == 0xC || cmd == 0xD) {
            b0 = r.read_u8();
        } else {
            continue;  // real-time etc.
        }

        prs::Event& ev = events_.emplace_back();
        ev.tick    = abs_tick;
        ev.kind    = prs::EventKind::Short;
        ev.status  = status;
        ev.d0      = b0;
        ev.d1      = b1;
    }

    return Result::Ok;
}

// =========================================================
//  unload
// =========================================================
void MidiPlayer::unload() {
    loaded_ = false;
    playing_.store(false, std::memory_order_relaxed);
    measure_.store(0, std::memory_order_relaxed);
    beat_.store(0, std::memory_order_relaxed);
    total_bars_.store(0, std::memory_order_relaxed);
    loop_.store(0, std::memory_order_relaxed);
    events_.clear();
    sx_pool_.clear();
    fire_us_.clear();
}

// =========================================================
//  play — blocking playback loop
// =========================================================
void MidiPlayer::play(prs::MidiSink& midi_out,
                      const std::atomic<bool>& stop_flag,
                      int loop_count,
                      void* notify_hwnd,
                      chmodel::ChannelModel* model) {
    if (!loaded_ || events_.empty()) {
#ifdef _WIN32
        if (notify_hwnd) PostMessageW(static_cast<HWND>(notify_hwnd), WM_APP + 1, 0, 0);
#endif
        return;
    }

    playing_.store(true, std::memory_order_relaxed);
    loop_.store(0, std::memory_order_relaxed);
    bool stopping = false;

    const int tpq = info_.ticks_per_qn;
    const u32 ts_num = info_.ts_num;
    const u32 ticks_per_bar = static_cast<u32>(tpq) * ts_num;

    // Flat held-note list for cleanup
    struct HeldNote { u8 ch; u8 note; };
    std::array<HeldNote, 256> held_list{};
    int held_count = 0;

    // ----- Event delivery through the channel model -----
    // Short (channel) message: controller / program changes that the model
    // tracks go through it; everything else straight to the device.
    const auto send_short = [&](const prs::Event& e) {
        if (model) {
            const u8 cmd = e.status >> 4;
            const int ch = e.status & 0x0F;
            if (cmd == 0xB) {
                if (chmodel::ChannelModel::is_tracked_cc(e.d0)) {
                    model->set_cc(ch, e.d0, e.d1, chmodel::Source::MidiFile);
                    return;
                }
                midi_out.short_msg(e.status, e.d0, e.d1);
                if (e.d0 == 121) {
                    model->note_reset_all_controllers(ch, chmodel::Source::MidiFile);
                }
                return;
            }
            if (cmd == 0xC) {
                model->set_program(ch, e.d0, chmodel::Source::MidiFile);
                return;
            }
        }
        midi_out.short_msg(e.status, e.d0, e.d1);
    };
    // SysEx: through the model (GM/XG On re-syncs it, XG part parameters
    // become tracked changes); otherwise straight through.
    const auto send_sysex = [&](std::span<const u8> payload) {
        if (model) {
            model->handle_sysex(payload, chmodel::Source::MidiFile,
                [&midi_out](std::span<const u8> raw) { midi_out.sysex(raw); });
        } else {
            midi_out.sysex(payload);
        }
    };

    while (!stopping) {
        loop_.fetch_add(1, std::memory_order_relaxed);
        if (loop_count > 0 && loop_.load(std::memory_order_relaxed) > loop_count) break;

        // Stamp t0 as late as possible
        auto t0 = std::chrono::steady_clock::now();

        // Release any held notes from the PREVIOUS loop iteration.
        // Without this, notes that were still sounding when the loop
        // restarted would hang forever (held_count was reset but
        // note-offs were never sent).
        for (int h = 0; h < held_count; ++h) {
            midi_out.short_msg(static_cast<u8>(0x80 | held_list[static_cast<std::size_t>(h)].ch),
                              held_list[static_cast<std::size_t>(h)].note, 0);
        }
        held_count = 0;

        // Fast-forward through leading silence: send setup events immediately.
        // Only CC, PC, pitch bend, SysEx — NOT note on/off.
        int first_note_idx = -1;
        for (std::size_t i = 0; i < events_.size(); ++i) {
            const auto& ev = events_[i];
            if (ev.kind == prs::EventKind::Short) {
                u8 cmd = ev.status >> 4;
                if (cmd == 0x9 && ev.d1 > 0) { first_note_idx = static_cast<int>(i); break; }
            }
            // Send non-note setup events immediately
            if (ev.kind == prs::EventKind::SysEx) {
                if (ev.sx_offset + ev.sx_len <= sx_pool_.size()) {
                    send_sysex(std::span<const u8>(
                        sx_pool_.data() + ev.sx_offset, ev.sx_len));
                }
            } else if (ev.kind == prs::EventKind::Short) {
                u8 cmd = ev.status >> 4;
                if (cmd != 0x8 && cmd != 0x9) {
                    send_short(ev);
                }
            }
        }
        // Re-anchor t0 so first note fires immediately
        if (first_note_idx > 0) {
            t0 -= std::chrono::microseconds(fire_us_[static_cast<std::size_t>(first_note_idx)]);
        }

        for (std::size_t i = 0; i < events_.size() && !stopping; ++i) {
            const auto& ev = events_[i];
            auto fire_time = t0 + std::chrono::microseconds(fire_us_[i]);

            // Check stop signal
            if (stop_flag.load(std::memory_order_relaxed)) { stopping = true; break; }

            // Wait for fire time
            auto now = std::chrono::steady_clock::now();
            if (fire_time > now) {
                auto gap_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    fire_time - now).count();
                if (gap_us > 4000) {
                    // Sleep until ~4ms before fire, then spin
                    auto wake = fire_time - std::chrono::microseconds(4000);
                    while (!stopping && std::chrono::steady_clock::now() < wake) {
                        if (stop_flag.load(std::memory_order_relaxed)) { stopping = true; break; }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                // Spin for final <4ms
                while (!stopping && std::chrono::steady_clock::now() < fire_time) {
                    if (stop_flag.load(std::memory_order_relaxed)) { stopping = true; break; }
                    std::this_thread::yield();
                }
            }

            if (stopping) break;

            // Skip non-audio events
            if (ev.kind == prs::EventKind::Tempo) continue;

            // Update position
            if (ticks_per_bar > 0) {
                u32 tick = ev.tick;
                measure_.store(static_cast<int>(tick / ticks_per_bar) + 1,
                              std::memory_order_relaxed);
                beat_.store(static_cast<int>((tick % ticks_per_bar) / static_cast<u32>(tpq)) + 1,
                           std::memory_order_relaxed);
            }

            // Send event
            if (ev.kind == prs::EventKind::SysEx) {
                if (ev.sx_offset + ev.sx_len <= sx_pool_.size()) {
                    send_sysex(std::span<const u8>(
                        sx_pool_.data() + ev.sx_offset, ev.sx_len));
                }
            } else {
                // Track held notes
                u8 cmd = ev.status >> 4;
                u8 ch  = ev.status & 0x0F;
                u8 note = ev.d0 & 0x7F;
                if (cmd == 0x9 && ev.d1 > 0) {
                    if (held_count < 256) {
                        held_list[static_cast<std::size_t>(held_count)] = {ch, note};
                        ++held_count;
                    }
                } else if (cmd == 0x8 || (cmd == 0x9 && ev.d1 == 0)) {
                    for (int h = 0; h < held_count; ++h) {
                        if (held_list[static_cast<std::size_t>(h)].ch == ch &&
                            held_list[static_cast<std::size_t>(h)].note == note) {
                            held_list[static_cast<std::size_t>(h)] =
                                held_list[static_cast<std::size_t>(--held_count)];
                            break;
                        }
                    }
                }
                send_short(ev);
            }
        }

        // Release held notes
        for (int h = 0; h < held_count; ++h) {
            midi_out.short_msg(static_cast<u8>(0x80 | held_list[static_cast<std::size_t>(h)].ch),
                              held_list[static_cast<std::size_t>(h)].note, 0);
        }
        held_count = 0;

        if (stopping) break;

        // Seamless loop wait
        if (loop_count != 1 && !events_.empty()) {
            std::int64_t one_tick_us = static_cast<std::int64_t>(info_.tempo_us) /
                static_cast<std::int64_t>(info_.ticks_per_qn);
            auto loop_end = t0 + std::chrono::microseconds(
                fire_us_.back() + one_tick_us);
            while (!stopping && std::chrono::steady_clock::now() < loop_end) {
                if (stop_flag.load(std::memory_order_relaxed)) { stopping = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    // Thorough stop: sustain off, individual note-offs for held notes.
    // Use short_direct to ensure immediate delivery before thread exit.
    for (int ch = 0; ch < 16; ++ch) {
        midi_out.short_direct(static_cast<u8>(0xB0 | ch), 64, 0);   // sustain off
    }
    for (int h = 0; h < held_count; ++h) {
        midi_out.short_direct(static_cast<u8>(0x80 | held_list[static_cast<std::size_t>(h)].ch),
                          held_list[static_cast<std::size_t>(h)].note, 0);
    }
    // Safety net: All Notes Off + All Sound Off on all 16 channels.
    // Catches any notes that slipped through the held-note tracking
    // (same pattern as the style player's end-of-playback cleanup).
    for (int ch = 0; ch < 16; ++ch) {
        midi_out.short_direct(static_cast<u8>(0xB0 | ch), 123, 0);  // All Notes Off
        midi_out.short_direct(static_cast<u8>(0xB0 | ch), 120, 0);  // All Sound Off
    }

    playing_.store(false, std::memory_order_relaxed);
    measure_.store(0, std::memory_order_relaxed);
    beat_.store(0, std::memory_order_relaxed);

#ifdef _WIN32
    if (notify_hwnd) PostMessageW(static_cast<HWND>(notify_hwnd), WM_APP + 1, 0, 0);
#endif
}

} // namespace midi_player
