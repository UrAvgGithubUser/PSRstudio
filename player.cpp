// player.cpp — Style playback sequencer + Standard MIDI File player.
#include "player.hpp"
#include "midi.hpp"
#include "playback_controller.hpp"


// ======================================================================
//   SMF player
// ======================================================================

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


// ======================================================================
//   Style sequencer
// ======================================================================

// Style playback sequencer.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <thread>

namespace player {

using prs::u8;

// =========================================================
//  Destructor
// =========================================================
Player::~Player() = default;

// =========================================================
//  Heap scheduler
// =========================================================
void Player::heap_push(EventNode node) {
    heap_.push_back(node);
    std::push_heap(heap_.begin(), heap_.end(), MinByTick{});
}

Player::EventNode Player::heap_pop() {
    std::pop_heap(heap_.begin(), heap_.end(), MinByTick{});
    EventNode result = heap_.back();
    heap_.pop_back();
    return result;
}

void Player::load_section_into_heap(const prs::PrsStyle& style, int sec_idx,
                                    std::uint64_t base_tick,
                                    std::uint64_t min_tick_offset) {
    const auto& sec = style.sections()[static_cast<std::size_t>(sec_idx)];
    const auto& events = style.events();
    for (std::size_t i = sec.ev_start; i < sec.ev_end; ++i) {
        std::uint64_t rel_tick = events[i].tick - sec.start_tick;
        if (rel_tick < min_tick_offset) continue;  // skip past events
        EventNode node;
        node.absolute_tick = base_tick + rel_tick;
        node.event_index   = static_cast<std::uint32_t>(i);
        node.section_id    = static_cast<std::uint32_t>(sec_idx);
        heap_push(node);
    }
}

// =========================================================
//  kill_all_held_notes — send note-offs for everything held
//
//  Uses held_out_ch_ (the remembered out_ch from when the note
//  was sent) so note-offs go to the right channel even if
//  casm_cache_ has been refreshed since (e.g. section changed).
// =========================================================
void Player::kill_all_held_notes(const prs::PrsStyle& /*style*/,
                                 const prs::StyleSection& /*cur_sec*/,
                                 prs::MidiSink& midi,
                                 const prs::RemapSource& remap) {
    for (int ch = 0; ch < 16; ++ch) {
        // Fall back to recomputing out_ch from the current casm_cache_
        // if no remembered out_ch (shouldn't happen, but defensive).
        const prs::CasmChannel* cc = casm_cache_[static_cast<std::size_t>(ch)];
        u8 fallback_out_ch = out_channel(remap, static_cast<u8>(ch), cc);
        for (int n = 0; n < 128; ++n) {
            if (!held_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)]) continue;
            int out_pitch = held_out_pitch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)];
            int out_ch    = held_out_ch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)];
            if (out_pitch < 0) out_pitch = n;
            if (out_ch    < 0) out_ch    = fallback_out_ch;
            midi.short_msg(static_cast<u8>(0x80 | out_ch), static_cast<u8>(out_pitch), 64);
            held_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] = 0;
            held_out_pitch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] = -1;
            held_out_ch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] = -1;
        }
    }
}

// =========================================================
//  advance_playlist — progress to next section
// =========================================================
bool Player::advance_playlist(const prs::PrsStyle& style,
                              std::vector<int>& pl,
                              int& current_pl_index,
                              int& current_sec_idx,
                              std::uint64_t& current_section_base_tick,
                              bool& stopping) {
    const auto& sections = style.sections();
    const auto& sec = sections[static_cast<std::size_t>(current_sec_idx)];
    bool auto_stop = false;
    bool loop_only = prs::PrsStyle::section_is_loop_only(sec.name, auto_stop);
    bool is_ending = auto_stop;

    // If it's a Main (not loop_only), reload the same section to loop it
    if (!loop_only && !is_ending) {
        current_section_base_tick = current_tick_;
        return true;
    }

    ++current_pl_index;
    if (current_pl_index >= static_cast<int>(pl.size())) {
        if (is_ending) {
            // Ending finished and nothing queued after it → stop playback.
            // The user can queue another section by pressing a button during
            // the Ending — that section gets added to pl[1] via the queue logic.
            stopping = true;
            return false;
        } else {
            int next_main = g_selected_main.load(std::memory_order_relaxed);
            if (next_main < 0 || next_main >= static_cast<int>(sections.size())) {
                next_main = static_cast<int>(style.find_main_A());
            }
            if (next_main >= 0) {
                pl.clear();
                pl.push_back(next_main);
                current_pl_index = 0;
            } else {
                stopping = true;
                return false;
            }
        }
    }

    current_sec_idx = pl[static_cast<std::size_t>(current_pl_index)];
    current_section_base_tick = current_tick_;
    return true;
}

// =========================================================
//  resolve_quality_variant — Ketron/Korg MAJ/MIN/7TH section variants
// =========================================================
int Player::resolve_quality_variant(const prs::PrsStyle& style,
                                    int sec_idx) const {
    if (style.format() == prs::StyleFormat::Yamaha) return sec_idx;
    const auto& sections = style.sections();
    if (sec_idx < 0 || sec_idx >= static_cast<int>(sections.size())) return sec_idx;

    // Chord family from the active chord's CASM type:
    //   minor = min/m(add9)/m7(9)/mMaj7/mMaj7(9)
    //   7th   = everything 7th-bearing (incl. m6/m7/m7b5/dim/dim7 per the
    //           observed mapping) — everything else counts as major.
    int family = 0;   // MAJ
    if (active_chord_.root >= 0) {
        const int t = active_chord_.type;
        switch (t) {
            case 0x08: case 0x0C: case 0x0D: case 0x0F: case 0x10:
                family = 1; break;   // MIN
            case 0x03: case 0x05: case 0x06: case 0x09: case 0x0A: case 0x0B:
            case 0x0E: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
            case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B:
            case 0x1D:
                family = 2; break;   // 7TH
            default:
                family = 0; break;   // MAJ
        }
    }

    const char* suffix = (family == 1) ? " MIN" : (family == 2) ? " 7TH" : nullptr;
    if (suffix) {
        if (auto v = style.find_section(sections[static_cast<std::size_t>(sec_idx)].name + suffix)) {
            return static_cast<int>(*v);
        }
    }
    return sec_idx;
}

// =========================================================
//  handle_section_request — mid-playback section changes
// =========================================================
void Player::handle_section_request(const prs::PrsStyle& style,
                                    int requested_sec_idx,
                                    std::vector<int>& pl,
                                    int current_sec_idx,
                                    std::uint64_t current_section_base_tick,
                                    std::uint64_t true_current_tick,
                                    prs::MidiSink& midi,
                                    const prs::RemapSource& remap) {
    // Ketron/Korg: resolve the MAJ/MIN/7TH variant of the requested
    // section against the current chord family.
    requested_sec_idx = resolve_quality_variant(style, requested_sec_idx);

    const auto& sections = style.sections();
    const auto& cur_sec = sections[static_cast<std::size_t>(current_sec_idx)];

    bool cur_auto_stop = false;
    bool cur_loop_only = prs::PrsStyle::section_is_loop_only(cur_sec.name, cur_auto_stop);
    bool cur_is_ending = cur_auto_stop;
    bool cur_is_main = !cur_loop_only;

    const auto& req_sec = sections[static_cast<std::size_t>(requested_sec_idx)];
    bool ns_is_fill = prs::PrsStyle::section_is_fillin(req_sec.name);
    bool ns_auto_stop = false;
    bool ns_loop_only = prs::PrsStyle::section_is_loop_only(req_sec.name, ns_auto_stop);
    bool ns_is_main = !ns_loop_only;
    bool is_break = ns_is_fill && req_sec.name == "Fill In BA";

    // If currently in an intro/fill/break/ending (loop-only), queue for later
    // The queued section will play when the current section finishes.
    if (cur_loop_only) {
        if (ns_is_fill) return;  // don't queue fills
        if (pl.size() >= 2) {
            pl[1] = requested_sec_idx;
        } else {
            pl.push_back(requested_sec_idx);
        }
        return;
    }

    int target_ns = requested_sec_idx;
    bool auto_fill = g_auto_fill.load(std::memory_order_relaxed) != 0;

    // Break returns to whatever Main was playing (g_selected_main)
    if (is_break) {
        target_ns = g_selected_main.load(std::memory_order_relaxed);
        if (target_ns < 0) target_ns = current_sec_idx;
    }

    // Insert a fill when:
    // 1. Auto Fill ON and switching to a different Main → insert fill for new Main
    // 2. Tapping the same Main (manual fill trigger) → insert fill regardless of Auto Fill
    if (!ns_is_fill && cur_is_main && ns_is_main) {
        bool is_same_main = (requested_sec_idx == current_sec_idx);
        if (auto_fill || is_same_main) {
            if (auto fill_idx = style.find_corresponding_fill(req_sec.name); fill_idx) {
                requested_sec_idx = static_cast<int>(*fill_idx);
                ns_is_fill = true;
            }
        }
    }

    const std::uint32_t tpqn = style_tpqn_;
    const std::uint32_t ticks_per_measure = tpqn * style.ts_num();

    // Position within current measure
    std::uint64_t elapsed_ticks = true_current_tick - current_section_base_tick;
    std::uint64_t pos_in_measure = elapsed_ticks % ticks_per_measure;

    // Determine timing:
    // - Break: ALWAYS immediate (mid-measure), regardless of Auto Fill
    // - Manual fill (same Main tap): ALWAYS immediate (mid-measure)
    // - Auto Fill ON, different Main: immediate unless on last beat → defer
    // - Auto Fill OFF, different Main: ALWAYS defer to next bar
    // - Intro/Ending: ALWAYS defer to next bar
    std::uint64_t last_beat_start = static_cast<std::uint64_t>(style.ts_num() - 1) * tpqn;
    bool on_last_beat = (pos_in_measure >= last_beat_start);
    bool is_same_main_tap = (ns_is_fill && !is_break &&
        target_ns == current_sec_idx);

    bool defer_to_next_measure;

    if (is_break || is_same_main_tap) {
        // Break and manual fill: always immediate (mid-measure)
        defer_to_next_measure = false;
    } else if (ns_is_fill) {
        // Auto Fill ON, different Main: immediate unless on last beat
        defer_to_next_measure = on_last_beat;
    } else if (ns_is_main && cur_is_main && !auto_fill) {
        // Auto Fill OFF, Main → different Main: always defer
        defer_to_next_measure = true;
    } else if (ns_loop_only) {
        // Intro/Ending: always defer
        defer_to_next_measure = true;
    } else {
        // Default: defer
        defer_to_next_measure = true;
    }

    if (defer_to_next_measure) {
        // Deferred to next measure barline
        std::uint64_t current_measure = elapsed_ticks / ticks_per_measure;
        std::uint64_t inject_tick = current_section_base_tick +
            (current_measure + 1) * ticks_per_measure;

        // Build new heap: keep events from the CURRENT measure that haven't
        // fired yet (they belong to the current pattern and should finish
        // playing before the barline). Drop everything at or after the barline
        // — those are from the next loop iteration of the old section and
        // would cause anomalous notes.
        std::vector<EventNode> old_heap = std::move(heap_);
        heap_.clear();

        for (const auto& node : old_heap) {
            if (node.absolute_tick < inject_tick) {
                heap_push(node);
            }
        }

        // Insert kill-notes event at the barline
        EventNode kill_node;
        kill_node.absolute_tick = inject_tick;
        kill_node.event_index   = kEventKillNotes;
        kill_node.section_id    = static_cast<std::uint32_t>(current_sec_idx);
        heap_push(kill_node);

        if (ns_is_fill || is_break) {
            // Load the Fill/Break from its beginning
            load_section_into_heap(style, requested_sec_idx, inject_tick, 0);
            // Queue the target Main after the Fill/Break
            if (pl.size() < 2) {
                pl.resize(2);
            }
            pl[1] = target_ns;
        } else {
            // Load the requested section from its beginning
            load_section_into_heap(style, requested_sec_idx, inject_tick, 0);
        }

        // Advance the global tick to the injection point
        current_tick_ = true_current_tick;
        tick_origin_ = now();
    } else {
        // Immediate — mid-measure switch
        std::uint64_t inject_tick = true_current_tick - pos_in_measure;
        std::uint64_t min_tick_offset = pos_in_measure;

        // Kill all held notes NOW
        kill_all_held_notes(style, cur_sec, midi, remap);

        // Clear the entire heap
        heap_.clear();

        if (ns_is_fill || is_break) {
            // Load the Fill/Break at the current beat position
            load_section_into_heap(style, requested_sec_idx, inject_tick, min_tick_offset);
            // Queue the target Main after the Fill/Break
            if (pl.size() < 2) {
                pl.resize(2);
            }
            pl[1] = target_ns;
        } else {
            // Load the requested section at the current beat position
            load_section_into_heap(style, requested_sec_idx, inject_tick, min_tick_offset);
        }

        // Advance the global tick to the injection point
        current_tick_ = true_current_tick;
        tick_origin_ = now();
    }

    // Track the selected Main for future playlist advances
    if (ns_is_main) {
        g_selected_main.store(target_ns, std::memory_order_relaxed);
    }
}

// =========================================================
//  refresh_casm_cache — cache CASM channel pointers for current section
//  Called once per section change, avoids per-note linear searches.
// =========================================================
void Player::refresh_casm_cache(const prs::PrsStyle& style,
                                const prs::StyleSection& sec) {
    for (int ch = 0; ch < 16; ++ch) {
        casm_cache_[static_cast<std::size_t>(ch)] =
            style.casm_get_channel(sec.name, static_cast<u8>(ch));
    }
}

// =========================================================
//  drum_note_at — find a drum note sounding at (out_ch, pitch)
//
//  Returns the source channel (0-15) of a drum note currently held at
//  the given output channel and pitch, or -1 if none. Used by
//  process_chord_change to protect drum notes from being killed by
//  non-drum retrigger note-offs on shared output channels (MONTAGE).
// =========================================================
int Player::drum_note_at(int out_ch, int pitch) const noexcept {
    for (int ch = 0; ch < 16; ++ch) {
        const prs::CasmChannel* cc = casm_cache_[static_cast<std::size_t>(ch)];
        if (!cc || !cc->is_drums) continue;   // only drum channels
        if (pitch < 0 || pitch > 127) continue;
        if (!held_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(pitch)]) continue;
        int drum_out_ch = held_out_ch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(pitch)];
        if (drum_out_ch < 0) continue;
        if (drum_out_ch == out_ch) return ch;
    }
    return -1;
}

// =========================================================
//  process_chord_change — retranspose all held notes
// =========================================================
void Player::process_chord_change(const prs::PrsStyle& style,
                                  const prs::StyleSection& /*sec*/,
                                  prs::MidiSink& midi,
                                  const prs::RemapSource& remap) {
    // One atomic load: root, type and bass pitch class always belong
    // together.
    int r = -1;
    int bass_pc = -1;
    chord::ChordType ct = chord::ChordType::None;
    if (!chord::g_active_chord.load(r, ct, bass_pc)) return;   // no chord yet
    const int t = static_cast<int>(ct);

    // Compare using the RAW ChordType values, NOT the CASM-converted
    // ones. The bass pitch class participates too: a bass movement
    // (C -> C/E) must re-voice bass_on channels even though root and
    // type are unchanged.
    if (r == active_chord_.root && t == active_chord_raw_type_ &&
        bass_pc == active_bass_) {
        return;
    }

    // Published N.C. (e.g. a 4-note chromatic cluster): stop chord-
    // following. Held notes keep sounding at their current pitch and new
    // notes play untransposed via the bypass path until a real chord
    // arrives.
    if (ct == chord::ChordType::NoChord) {
        active_chord_ = {-1, -1, -1};
        active_chord_raw_type_ = t;
        active_bass_ = -1;
        return;
    }

    transpose::ChordCtx new_chord;
    new_chord.root = r;
    new_chord.type = transpose::chord_type_to_casm(ct);
    new_chord.bass = bass_pc;   // pitch class 0-11, or -1

    // The chord the currently-held notes were transposed for.
    const transpose::ChordCtx prev_chord = active_chord_;
    const bool prev_valid = prev_chord.root >= 0;

    pending_rtr_.clear();

    // Sustain threshold (Cadenza-style, cf. its revoiceActiveNotes): a
    // held note whose matching note-off lies LESS than one beat ahead is
    // about to end on its own — re-articulating it mid-decay is an
    // audible blip that buys nothing. Notes with a beat or more of life
    // left DO get re-voiced, so long pads/strings struck shortly before
    // the change still follow the new chord instead of dragging the old
    // chord's pitch through their whole (long) duration.
    //
    // The matching note-off is a future event in the scheduler heap:
    // find its tick and compare with the current position. Remaining
    // life is measured in BEATS (style_tpqn_ ticks per quarter) so the
    // threshold is tempo-independent.
    constexpr std::uint64_t kNoOffTick = ~std::uint64_t{0};
    const auto note_off_tick = [this, &style](int ch, int n) -> std::uint64_t {
        const auto& events = style.events();
        std::uint64_t best = kNoOffTick;
        for (const EventNode& node : heap_) {
            if (node.event_index == kEventKillNotes) continue;
            if (node.event_index >= events.size()) continue;
            const prs::Event& ev = events[node.event_index];
            const u8 cmd = ev.status >> 4;
            const bool is_off = (cmd == 0x8) || (cmd == 0x9 && ev.d1 == 0);
            if (!is_off) continue;
            if ((ev.status & 0x0F) != ch) continue;
            if (ev.d0 != n) continue;
            if (node.absolute_tick < best) best = node.absolute_tick;
        }
        return best;
    };

    // ---- PASS 1: decide per note, send every note-off ----
    for (int ch = 0; ch < 16; ++ch) {
        const prs::CasmChannel* cc = casm_cache_[static_cast<std::size_t>(ch)];
        if (!cc || cc->is_drums) continue;   // drums: leave sounding

        // Chord-mute table: if the new chord type is muted for this
        // channel, its held notes must stop (not only new note-ons).
        const bool chord_ok =
            prs::PrsStyle::casm_is_chord_allowed(*cc, new_chord.type);

        // Source chord for retrigger = what the note was last shaped for.
        const transpose::ChordCtx src_chord =
            prev_valid ? prev_chord : arranger::src_from_casm(*cc);

        for (int n = 0; n < 128; ++n) {
            if (!held_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)]) continue;

            int old_out = held_out_pitch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)];
            if (old_out < 0) old_out = n;

            // Use the channel the note was actually sent on.
            int out_ch = held_out_ch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)];
            if (out_ch < 0) out_ch = out_channel(remap, static_cast<u8>(ch), cc);

            // Per-note range (low / main / high) -> that range's own RTR.
            const prs::NoteRange& nr = cc->range_for_pitch(n);

            // MegaVoice articulation notes (C6 and above) are never
            // transposed: held ones keep sounding at their pitch across
            // the chord change. With "Patch MegaVoices" on they are
            // silenced instead.
            const bool mega_channel = transpose::megavoice::is_mega_bank(
                live_msb_[ch], live_lsb_[ch]);
            const bool mega_high = mega_channel &&
                n >= transpose::megavoice::kRegion1Low;
            const bool patch = g_patch_megavoices.load();

            transpose::RtrResult rtr{transpose::RtrAction::None, -1};
            if (!chord_ok || (mega_high && patch)) {
                rtr = {transpose::RtrAction::Stop, -1};
            } else if (!mega_high) {
                rtr = transpose::retrigger(old_out, nr, src_chord, new_chord,
                                           prev_valid);
            }

            // Bass register anchor: keep retriggered bass notes in the
            // anchored low register too (matches the note-on path).
            if (rtr.action == transpose::RtrAction::Retrigger && rtr.new_pitch >= 0) {
                rtr.new_pitch = arranger::apply_bass_anchor(rtr.new_pitch, *cc, nr);
            }

            if (rtr.action == transpose::RtrAction::Stop) {
                // Before sending the note-off, check if a drum note is
                // sounding at the same (out_ch, old_out). In MONTAGE mode
                // drum and melodic channels share output channels, so a
                // note-off here would kill the drum note too. If a drum
                // note is found, re-trigger it immediately after the off.
                const int drum_src = drum_note_at(out_ch, old_out);
                midi.short_msg(static_cast<u8>(0x80 | out_ch),
                               static_cast<u8>(old_out), 64);
                held_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] = 0;
                held_out_pitch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] = -1;
                held_out_ch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] = -1;
                if (drum_src >= 0) {
                    // Restore the drum note that was collateral damage.
                    const u8 dvel = held_vel_[static_cast<std::size_t>(drum_src)]
                                              [static_cast<std::size_t>(old_out)];
                    midi.short_msg(static_cast<u8>(0x90 | out_ch),
                                   static_cast<u8>(old_out),
                                   dvel ? dvel : u8{64});
                }
            } else if (rtr.action == transpose::RtrAction::Retrigger) {
                // Sustain threshold: skip notes that end naturally within
                // less than one beat. Notes with no scheduled note-off in
                // the heap sustain indefinitely — they always get
                // re-voiced.
                const std::uint64_t off_tick = note_off_tick(ch, n);
                const bool sustaining = (off_tick == kNoOffTick) ||
                    (off_tick > current_tick_ &&
                     (off_tick - current_tick_) >= style_tpqn_);
                if (!sustaining) continue;
                // Same pitch: keep sounding. An off+on at one pitch is an
                // audible blip.
                if (rtr.new_pitch >= 0 && rtr.new_pitch != old_out) {
                    // Same drum-protection as above: if a drum note is at
                    // (out_ch, old_out), re-trigger it after the off.
                    const int drum_src = drum_note_at(out_ch, old_out);
                    midi.short_msg(static_cast<u8>(0x80 | out_ch),
                                   static_cast<u8>(old_out), 64);
                    pending_rtr_.push_back({ch, n, out_ch, rtr.new_pitch,
                        held_vel_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)]});
                    if (drum_src >= 0) {
                        const u8 dvel = held_vel_[static_cast<std::size_t>(drum_src)]
                                                  [static_cast<std::size_t>(old_out)];
                        midi.short_msg(static_cast<u8>(0x90 | out_ch),
                                       static_cast<u8>(old_out),
                                       dvel ? dvel : u8{64});
                    }
                }
            }
        }
    }

    // ---- PASS 2: all note-ons, after every note-off ----
    // Guarantees one note's off can never kill another note's freshly
    // retriggered on at the same pitch.
    for (const auto& p : pending_rtr_) {
        const u8 vel = p.vel ? p.vel : u8{64};
        midi.short_msg(static_cast<u8>(0x90 | p.out_ch),
                       static_cast<u8>(p.new_pitch), vel);
        held_out_pitch_[static_cast<std::size_t>(p.ch)][static_cast<std::size_t>(p.n)] = p.new_pitch;
        held_out_ch_[static_cast<std::size_t>(p.ch)][static_cast<std::size_t>(p.n)] = p.out_ch;
    }

    active_chord_ = new_chord;
    active_chord_raw_type_ = t;
    active_bass_ = bass_pc;
}

// =========================================================
//  process_midi_event — transpose and send one event
// =========================================================
void Player::process_midi_event(const prs::PrsStyle& style,
                                const prs::StyleSection& sec,
                                const prs::Event& ev,
                                prs::MidiSink& midi,
                                const prs::RemapSource& remap) {
    if (ev.kind == prs::EventKind::Tempo) return;  // EV_TEMPO / EV_SKIP
    if (ev.kind == prs::EventKind::SysEx) {
        if (ev.sx_offset + ev.sx_len <= style.sysex_pool().size()) {
            const std::span<const u8> payload(
                style.sysex_pool().data() + ev.sx_offset, ev.sx_len);
            // Through the model: GM/XG System On resets + re-syncs it, XG
            // part parameters become model-tracked controller changes, and
            // everything else is forwarded to the device unchanged.
            model_->handle_sysex(payload, chmodel::Source::Style,
                [&midi](std::span<const u8> raw) { midi.sysex(raw); },
                // A style's XG part N is the style's channel N: route it
                // like that channel's notes (CASM dst_ch / Montage fold).
                [this, &remap](int part) {
                    const auto src = static_cast<u8>(part & 0x0F);
                    return static_cast<int>(out_channel(remap, src, casm_cache_[src]));
                });
        }
        return;
    }

    const u8 cmd    = ev.status >> 4;
    const u8 src_ch = ev.status & 0x0F;

    // The remap profile handles all voice/bank/PC translation.
    // For MONTAGE profiles, the remap table contains explicit
    // Genos2→Montage voice mappings (2066 entries) that map each
    // Yamaha XG voice address to the correct Montage Performance.
    // No more special-case "force to GM" overrides — the remap does it.
    //
    // Every voice / mixer controller change below goes THROUGH the
    // channel model (never straight to the device), so the model, the
    // mixer UI and the synth always agree. The CC#93 -> CC#94 redirect
    // for Montage and the "user touched this" locks live in the model.

    if (cmd == 0xB) {
        // CC
        if (ev.d0 == 0)  live_msb_[src_ch] = ev.d1;
        if (ev.d0 == 32) live_lsb_[src_ch] = ev.d1;
        // Apply CASM / MONTAGE channel routing
        const prs::CasmChannel* cc = casm_cache_[src_ch];
        const u8 out_ch  = out_channel(remap, src_ch, cc);
        const u8 cc_num  = ev.d0;
        const u8 cc_val  = ev.d1;

        // Bank select is NOT forwarded on its own: it is latched in
        // live_msb_/live_lsb_ (above) and sent together with the next
        // Program Change — after voice remapping — as one atomic voice
        // change through the model (see the PC branch). A synth only acts
        // on bank select at the next Program Change anyway.
        if (cc_num == 0 || cc_num == 32) return;

        // Tracked controllers (volume, pan, expression, reverb, chorus,
        // variation, tone controls): stored in the model, mirrored in the
        // mixer, user locks obeyed.
        if (chmodel::ChannelModel::is_tracked_cc(cc_num)) {
            model_->set_cc(out_ch, cc_num, cc_val, chmodel::Source::Style);
            return;
        }

        // Everything else (sustain, modulation, RPN/NRPN, ...) is
        // forwarded untouched.
        midi.short_msg(static_cast<u8>(0xB0 | out_ch), cc_num, cc_val);
        if (cc_num == 121) {
            // Reset All Controllers: keep the model and the synth in step.
            model_->note_reset_all_controllers(out_ch, chmodel::Source::Style);
        }
    } else if (cmd == 0xC) {
        // PC — apply live remapping
        const prs::CasmChannel* cc = casm_cache_[src_ch];
        const u8 out_ch = out_channel(remap, src_ch, cc);

        // The remap table handles everything — including drum kit
        // mappings. If the active remap profile has explicit drum kit
        // entries (e.g., Genos 127/0/1 → Montage 63/21/75), they'll
        // be found by remap.apply(). If not, the fallback rules
        // (bank=127 → 63/21/72 PowerStandardKit) handle it.
        //
        // The voice (bank MSB, bank LSB, program) is applied as ONE
        // change through the model. If the user has picked a voice for
        // this channel in the mixer (voice lock), the model drops it.
        u8 pc = static_cast<u8>(ev.d0 + 1);
        u8 dm = live_msb_[src_ch], dl = live_lsb_[src_ch], dp = pc;
        if (remap.apply(live_msb_[src_ch], live_lsb_[src_ch], pc, dm, dl, dp)) {
            if (model_->set_voice(out_ch, dm, dl, prs::pc_to_wire(dp),
                                  chmodel::Source::Style)) {
                live_msb_[src_ch] = dm;
                live_lsb_[src_ch] = dl;
            }
        } else {
            model_->set_voice(out_ch, live_msb_[src_ch], live_lsb_[src_ch],
                              ev.d0, chmodel::Source::Style);
        }
    } else if (cmd == 0x9 || cmd == 0x8) {
        // Note on/off
        u8 src_pitch = ev.d0 & 0x7F;
        u8 vel = ev.d1;
        bool is_on = (cmd == 0x9 && vel > 0);
        const prs::CasmChannel* cc = casm_cache_[src_ch];
        const u8 out_ch = out_channel(remap, src_ch, cc);

        // --- Chord mute: skip note-on if channel is muted for current chord ---
        // (note-off always passes through so notes don't hang)
        if (cc && is_on && !prs::PrsStyle::casm_is_chord_allowed(*cc, active_chord_.type)) {
            return;  // channel muted for this chord type — skip note-on
        }

        // --- Note mute: skip note-on if pitch class is muted ---
        if (cc && is_on && !prs::PrsStyle::casm_is_note_allowed(*cc, src_pitch)) {
            return;  // note muted for this root — skip note-on
        }

        // MegaVoice channel (bank MSB=8, LSB 0-16)? Latched from the
        // style's own bank-select events.
        const bool mega_channel = transpose::megavoice::is_mega_bank(
            live_msb_[src_ch], live_lsb_[src_ch]);
        // MegaVoice articulation notes (C6 and above) are position-
        // sensitive sample mappings — they are NEVER transposed.
        const bool mega_high = mega_channel &&
            src_pitch >= transpose::megavoice::kRegion1Low;

        // "Patch MegaVoices" toggle: silence those notes entirely.
        if (is_on && mega_high && g_patch_megavoices.load()) {
            return;  // patched out — no note-on, not tracked
        }

        if (!cc || cc->is_drums || active_chord_.root < 0 || (is_on && mega_high)) {
            // No transposition needed
            if (is_on) {
                held_[src_ch][src_pitch] = 1;
                held_out_pitch_[src_ch][src_pitch] = src_pitch;
                held_out_ch_[src_ch][src_pitch] = out_ch;
                held_vel_[src_ch][src_pitch] = vel;
                midi.short_msg(static_cast<u8>((cmd << 4) | out_ch), src_pitch, vel);
            } else {
                // Ignore note-offs for notes that aren't sounding (never
                // started, or already stopped by a chord change / kill).
                if (!held_[src_ch][src_pitch]) return;
                // Send the off for the pitch/channel actually sounding —
                // a chord-change retrigger may have moved it.
                int off_pitch = held_out_pitch_[src_ch][src_pitch];
                int off_ch    = held_out_ch_[src_ch][src_pitch];
                held_[src_ch][src_pitch] = 0;
                held_out_pitch_[src_ch][src_pitch] = -1;
                held_out_ch_[src_ch][src_pitch] = -1;
                if (off_pitch < 0) off_pitch = src_pitch;
                if (off_ch    < 0) off_ch    = out_ch;
                midi.short_msg(static_cast<u8>(0x80 | off_ch),
                               static_cast<u8>(off_pitch), vel);
            }
            return;
        }

        transpose::ChordCtx src_chord = arranger::src_from_casm(*cc);

        if (is_on) {
            int out_pitch;
            // Guitar arpeggio = Ntr::Guitar + Ntt::Chord (0x02)
            const prs::NoteRange& nr = cc->range_for_pitch(src_pitch);

            // Named bass (slash chords): a bass_on channel in root-shift
            // mode (NTT=Bypass) follows the player's named bass note —
            // with C/E held, the bass pattern lands on E instead of C.
            // Chord/melody-mode channels keep root mapping here (the
            // slash reaches them only via held-note retriggering).
            transpose::ChordCtx dst = active_chord_;
            if (nr.bass_on && nr.ntt == prs::Ntt::Bypass &&
                active_chord_.bass >= 0 &&
                (active_chord_.bass % 12) != (active_chord_.root % 12)) {
                dst.root = active_chord_.bass % 12;
            }

            if (nr.ntr == prs::Ntr::Guitar && nr.ntt == prs::Ntt::Chord) {
                out_pitch = transpose::note_arpeggio(
                    src_pitch,
                    prev_src_pitch_[src_ch],
                    prev_out_pitch_[src_ch],
                    nr, active_chord_);
            } else {
                out_pitch = arranger::transpose_single_note(
                    src_pitch, *cc, src_chord, dst);
            }
            // MegaVoice articulation notes (C6+) never reach this point —
            // they bypass transposition above.
            if (out_pitch < 0) return;
            held_[src_ch][src_pitch] = 1;
            held_out_pitch_[src_ch][src_pitch] = out_pitch;
            held_out_ch_[src_ch][src_pitch] = out_ch;
            held_vel_[src_ch][src_pitch] = vel;
            prev_src_pitch_[src_ch] = src_pitch;
            prev_out_pitch_[src_ch] = out_pitch;
            midi.short_msg(static_cast<u8>(0x90 | out_ch), static_cast<u8>(out_pitch), vel);
        } else {
            // Ignore note-offs for notes that aren't sounding (already
            // stopped by a chord-change Stop / kill). Without this the
            // fallback below would send an off at the SOURCE pitch and
            // could cut an unrelated note.
            if (!held_[src_ch][src_pitch]) return;
            held_[src_ch][src_pitch] = 0;
            int out_pitch = held_out_pitch_[src_ch][src_pitch];
            int note_out_ch = held_out_ch_[src_ch][src_pitch];
            held_out_pitch_[src_ch][src_pitch] = -1;
            held_out_ch_[src_ch][src_pitch] = -1;
            // Use the remembered out_ch (which may differ from the
            // current out_ch if the section changed while the note
            // was held). Fall back to the current out_ch if no
            // remembered value (shouldn't happen, but defensive).
            if (note_out_ch < 0) note_out_ch = out_ch;
            if (out_pitch < 0) out_pitch = src_pitch;
            midi.short_msg(static_cast<u8>(0x80 | note_out_ch), static_cast<u8>(out_pitch), vel);
        }
    } else {
        // Other channel messages (pitch bend, channel pressure, etc.)
        const prs::CasmChannel* cc2 = casm_cache_[src_ch];
        const u8 out_ch2 = out_channel(remap, src_ch, cc2);
        midi.short_msg(static_cast<u8>((ev.status & 0xF0) | out_ch2), ev.d0, ev.d1);
    }
}

// =========================================================
//  play — the main playback loop
// =========================================================
void Player::play(const prs::PrsStyle& style,
                  prs::MidiSink& midi,
                  const prs::VoiceRemap& remap,
                  const std::vector<int>& playlist,
                  chmodel::ChannelModel& model) {
    if (playlist.empty()) return;

    // All CC / PC / SysEx events of this play() call go through the model.
    model_ = &model;

    std::vector<int> pl = playlist;
    int current_pl_index = 0;

    // Initialize live bank tracking
    for (int c = 0; c < 16; ++c) {
        if (style.voices()[c].valid) {
            live_msb_[c] = style.voices()[c].bank_msb;
            live_lsb_[c] = style.voices()[c].bank_lsb;
        }
    }

    // Initialize held-note arrays
    held_ = {};
    for (auto& arr : held_out_pitch_) arr.fill(-1);
    for (auto& arr : held_out_ch_)    arr.fill(-1);
    prev_src_pitch_.fill(-1);
    prev_out_pitch_.fill(-1);
    held_vel_ = {};
    pending_rtr_.clear();
    pending_rtr_.reserve(128);
    active_chord_ = {-1, -1, -1};
    active_chord_raw_type_ = -1;
    active_bass_ = -1;
    bass_legato_sent_ = false;

    int current_sec_idx = resolve_quality_variant(style, pl[0]);
    std::uint64_t current_section_base_tick = 0;

    // Initialize globals
    active_tempo_us_ = style.tempo_us();
    style_tpqn_      = style.ticks_per_qn();
    current_tick_    = 0;
    tick_origin_     = now();
    heap_.clear();
    g_requested_sec.store(-1, std::memory_order_relaxed);
    g_stop.store(false, std::memory_order_relaxed);

    bool stopping = false;

    const auto& sections = style.sections();

    // ===== Voice setup =====
    // Nothing to send here: the style's voices / volumes / pans were
    // stored in the channel model (and synced to the device) when the
    // style was loaded; the synth reset + full model dump happens when
    // the device is connected.

    // ===== Bass legato setup =====
    // Find the bass channel (accompaniment channel 10 = src ch 10 in SFF2)
    // and configure it for mono legato. Only sent once at playback start.
    // We look for a CASM channel with part_name containing "Bass" or
    // source channel 10 (the SFF2 default bass channel).
    if (!bass_legato_sent_) {
        // The legato CCs must go to the OUTPUT channel the bass actually
        // plays on (CASM dst_ch, or the Montage fold into channels 1-8),
        // not to the style's source channel number.
        u8 bass_out_ch = remap.route_channel(10, 10);  // default: SFF2 bass = src ch 10
        // Try to find the bass channel from CASM
        for (const auto& cseg : style.casm().csegs) {
            for (const auto& cc : cseg.channels) {
                // Check if this channel is the bass (part_name contains "Bass" or "bass")
                std::string name(cc.part_name);
                std::ranges::transform(name, name.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name.find("bass") != std::string::npos) {
                    bass_out_ch = remap.route_channel(cc.src_ch, cc.dst_ch);
                    break;
                }
            }
        }
        style.send_bass_legato_setup(midi, bass_out_ch);
        bass_legato_sent_ = true;
    }

    // Load first section
    load_section_into_heap(style, current_sec_idx, 0, 0);
    refresh_casm_cache(style, sections[static_cast<std::size_t>(current_sec_idx)]);
    g_current_sec_idx.store(current_sec_idx, std::memory_order_relaxed);
    g_pos_section_idx.store(current_sec_idx, std::memory_order_relaxed);
    g_playback_state.store(PlaybackState::Playing, std::memory_order_relaxed);

    std::cout << std::format("\n[PLAY] {}  ({} events)  looping\n",
        sections[static_cast<std::size_t>(current_sec_idx)].name,
        sections[static_cast<std::size_t>(current_sec_idx)].ev_end -
        sections[static_cast<std::size_t>(current_sec_idx)].ev_start);

    // ===== Scheduler loop =====
    while (!stopping) {
        // 1. Check stop flag
        if (g_stop.load(std::memory_order_relaxed)) break;

        // 2. Tempo polling — set active_tempo_us_ from UI or style
        {
            long ui_bpm = g_ui_tempo_bpm.load(std::memory_order_relaxed);
            if (ui_bpm >= kTempoBpmMin && ui_bpm <= kTempoBpmMax) {
                active_tempo_us_ = 60000000u / static_cast<prs::u32>(ui_bpm);
            } else {
                active_tempo_us_ = style.tempo_us();
            }
        }

        // 2b. Update position display (measure / beat / total bars)
        {
            const auto& cur_sec = sections[static_cast<std::size_t>(current_sec_idx)];
            const std::uint32_t tpqn = style_tpqn_;
            const std::uint32_t tpm  = tpqn * style.ts_num();
            std::uint64_t elapsed_ticks = current_tick_ - current_section_base_tick;
            std::uint64_t total_ticks = cur_sec.end_tick - cur_sec.start_tick;
            if (elapsed_ticks > total_ticks) elapsed_ticks = total_ticks;
            int measure = static_cast<int>(elapsed_ticks / tpm) + 1;
            int beat    = static_cast<int>((elapsed_ticks % tpm) / tpqn) + 1;
            int total   = (tpm > 0) ? static_cast<int>(total_ticks / tpm) : 0;
            g_pos_measure.store(measure, std::memory_order_relaxed);
            g_pos_beat.store(beat, std::memory_order_relaxed);
            g_pos_total_bars.store(total, std::memory_order_relaxed);
        }

        const auto& sec = sections[static_cast<std::size_t>(current_sec_idx)];

        // 3. Chord polling
        process_chord_change(style, sec, midi, remap);

        // 4. Check for empty heap → advance playlist
        if (heap_.empty()) {
            if (!advance_playlist(style, pl, current_pl_index,
                                  current_sec_idx, current_section_base_tick, stopping)) {
                break;
            }
            if (!stopping) {
                // Send note-offs for held notes before loading new section
                kill_all_held_notes(style, sections[static_cast<std::size_t>(current_sec_idx)], midi, remap);
                load_section_into_heap(style, current_sec_idx,
                                       current_section_base_tick, 0);
                refresh_casm_cache(style, sections[static_cast<std::size_t>(current_sec_idx)]);
                prev_src_pitch_.fill(-1);
                prev_out_pitch_.fill(-1);
                g_current_sec_idx.store(current_sec_idx, std::memory_order_relaxed);
                g_pos_section_idx.store(current_sec_idx, std::memory_order_relaxed);
            }
            continue;
        }

        // 5. Peek the root
        EventNode root = heap_.front();
        std::uint64_t target_tick = root.absolute_tick;

        // 6. Sleep until target_tick
        if (target_tick > current_tick_) {
            // Compute sleep duration based on current tempo.
            // We poll every 1ms and recompute the remaining sleep if tempo changes.
            // This ensures tempo changes take effect immediately.
            auto sleep_start = now();

            while (!stopping) {
                if (g_stop.load(std::memory_order_relaxed)) { stopping = true; break; }

                // Recompute tempo from UI
                long ui_bpm = g_ui_tempo_bpm.load(std::memory_order_relaxed);
                if (ui_bpm >= kTempoBpmMin && ui_bpm <= kTempoBpmMax) {
                    active_tempo_us_ = 60000000u / static_cast<prs::u32>(ui_bpm);
                } else {
                    active_tempo_us_ = style.tempo_us();
                }

                // How much time has elapsed since we started sleeping?
                auto elapsed_since_sleep = std::chrono::duration_cast<std::chrono::microseconds>(
                    now() - sleep_start).count();

                // Convert elapsed time to ticks at current tempo
                std::uint64_t elapsed_ticks = (static_cast<std::uint64_t>(elapsed_since_sleep) * style_tpqn_) / active_tempo_us_;

                // Have we reached the target tick?
                if (current_tick_ + elapsed_ticks >= target_tick) break;

                // Poll for section requests
                int ns = g_requested_sec.exchange(-1, std::memory_order_relaxed);
                if (ns == -2) { stopping = true; break; }
                if (ns >= 0 && ns < static_cast<int>(sections.size())) {
                    std::uint64_t true_cur_tick = current_tick_ + elapsed_ticks;
                    handle_section_request(style, ns, pl, current_sec_idx,
                                           current_section_base_tick,
                                           true_cur_tick, midi, remap);
                    break;
                }

                // Chord polling
                process_chord_change(style, sec, midi, remap);

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (stopping) break;

            // Advance to the target tick and reset the time origin
            current_tick_ = target_tick;
            tick_origin_ = now();
        }

        // 7. Pop and process
        EventNode popped = heap_pop();

        // Check for special kill event
        if (popped.event_index == kEventKillNotes) {
            // Send individual note-offs for held notes only (NOT all_notes_off).
            // Use short_msg to ensure immediate delivery.
            // Use held_out_ch_ (the remembered out_ch from when the note
            // was sent) so note-offs go to the right channel even if
            // casm_cache_ has been refreshed since.
            for (int ch = 0; ch < 16; ++ch) {
                const prs::CasmChannel* cc = casm_cache_[static_cast<std::size_t>(ch)];
                u8 fallback_out_ch = out_channel(remap, static_cast<u8>(ch), cc);
                for (int n = 0; n < 128; ++n) {
                    if (!held_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)]) continue;
                    int out_pitch = held_out_pitch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)];
                    int out_ch    = held_out_ch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)];
                    if (out_pitch < 0) out_pitch = n;
                    if (out_ch    < 0) out_ch    = fallback_out_ch;
                    midi.short_msg(static_cast<u8>(0x80 | out_ch), static_cast<u8>(out_pitch), 64);
                    held_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] = 0;
                    held_out_pitch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] = -1;
                    held_out_ch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] = -1;
                }
            }
            continue;
        }

        const auto& events = style.events();
        const prs::Event& ev = events[popped.event_index];

        // Track section changes
        if (popped.section_id != static_cast<std::uint32_t>(current_sec_idx)) {
            current_sec_idx = static_cast<int>(popped.section_id);
            const auto& new_sec = sections[static_cast<std::size_t>(current_sec_idx)];
            refresh_casm_cache(style, new_sec);
            g_current_sec_idx.store(current_sec_idx, std::memory_order_relaxed);
            g_pos_section_idx.store(current_sec_idx, std::memory_order_relaxed);
            current_section_base_tick = popped.absolute_tick -
                (ev.tick - new_sec.start_tick);
        }

        const auto& cur_sec = sections[static_cast<std::size_t>(current_sec_idx)];
        process_midi_event(style, cur_sec, ev, midi, remap);
    }

    // ===== Cleanup =====
    // Send individual note-offs for held notes (NOT All Notes Off).
    // Use short_direct (bypasses ring buffer) to ensure note-offs are
    // sent immediately before the player thread exits.
    // Use held_out_ch_ (the remembered out_ch from when the note was
    // sent) so note-offs go to the right channel even if casm_cache_
    // has been refreshed since (e.g. an Ending finished, then a new
    // section was queued, then the user pressed Stop).
    for (int ch = 0; ch < 16; ++ch) {
        const prs::CasmChannel* cc = casm_cache_[static_cast<std::size_t>(ch)];
        u8 fallback_out_ch = out_channel(remap, static_cast<u8>(ch), cc);
        for (int n = 0; n < 128; ++n) {
            if (!held_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)]) continue;
            int out_pitch = held_out_pitch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)];
            int out_ch    = held_out_ch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)];
            if (out_pitch < 0) out_pitch = n;
            if (out_ch    < 0) out_ch    = fallback_out_ch;
            midi.short_direct(static_cast<u8>(0x80 | out_ch), static_cast<u8>(out_pitch), 64);
            held_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] = 0;
            held_out_pitch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] = -1;
            held_out_ch_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] = -1;
        }
    }
    // Safety net: send "All Notes Off" (CC#123) + "All Sound Off"
    // (CC#120) on all 16 channels. The held-note tracking above should
    // have caught everything, but if anything slipped through (e.g.
    // a note-on that we didn't track because it was muted by chord
    // mute / note mute, or a note that was held when the section
    // changed and we lost the out_ch mapping), this ensures the
    // synth goes silent.
    for (int ch = 0; ch < 16; ++ch) {
        midi.short_direct(static_cast<u8>(0xB0 | ch), 63, 0);  // All Notes Off
        midi.short_direct(static_cast<u8>(0xB0 | ch), 123, 0);  // All Sound Off
    }
    g_playback_state.store(PlaybackState::Stopped, std::memory_order_relaxed);
    g_current_sec_idx.store(-1, std::memory_order_relaxed);
    g_pos_section_idx.store(-1, std::memory_order_relaxed);
    g_pos_measure.store(0, std::memory_order_relaxed);
    g_pos_beat.store(0, std::memory_order_relaxed);
    g_pos_total_bars.store(0, std::memory_order_relaxed);
    heap_.clear();
}

} // namespace player