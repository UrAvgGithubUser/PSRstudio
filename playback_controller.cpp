// playback_controller.cpp — 16-channel MIDI state model, arranger API,
// and playback orchestration.
#include "playback_controller.hpp"
#include "midi.hpp"


// ======================================================================
//   Channel model
// ======================================================================

namespace chmodel {

// =========================================================
//  Local helpers
// =========================================================
namespace {

// Playback-thread sources: obey locks, send through the ring (short_msg).
constexpr bool is_queued(Source s) noexcept {
    return s == Source::Style || s == Source::MidiFile;
}
constexpr bool obeys_locks(Source s) noexcept { return is_queued(s); }

constexpr u8 clamp7(int v) noexcept {
    return static_cast<u8>(v < 0 ? 0 : (v > 127 ? 127 : v));
}

constexpr u16 bit(Param p) noexcept {
    return static_cast<u16>(1u << static_cast<unsigned>(p));
}

constexpr bool valid_ch(int ch) noexcept { return ch >= 0 && ch < kChannels; }

struct CcParam { int cc; Param param; };
constexpr std::array<CcParam, 10> kCcParams = {{
    {  7, Param::Volume     },
    { 10, Param::Pan        },
    { 11, Param::Expression },
    { 91, Param::Reverb     },
    { 93, Param::Chorus     },
    { 94, Param::Variation  },
    { 71, Param::FilterRes  },
    { 72, Param::Release    },
    { 73, Param::Attack     },
    { 74, Param::Brightness },
}};

bool param_for_cc(int cc, Param& out) noexcept {
    for (const auto& e : kCcParams) {
        if (e.cc == cc) { out = e.param; return true; }
    }
    return false;
}

// Storage slot of a controller parameter. (Voice is not a single slot.)
u8& field_of(Channel& c, Param p) noexcept {
    switch (p) {
        case Param::Volume:     return c.volume;
        case Param::Pan:        return c.pan;
        case Param::Expression: return c.expression;
        case Param::Reverb:     return c.reverb;
        case Param::Chorus:     return c.chorus;
        case Param::Variation:  return c.variation;
        case Param::FilterRes:  return c.filter_res;
        case Param::Release:    return c.release;
        case Param::Attack:     return c.attack;
        case Param::Brightness: return c.brightness;
        default:                return c.volume;   // Voice/Count: never used
    }
}

} // namespace

// =========================================================
//  Construction — startup defaults
// =========================================================
ChannelModel::ChannelModel() {
    // Channel{} already carries the startup defaults:
    //   MSB 0 / LSB 0 / PC 0, volume 64, pan 64 (centre), reverb 0,
    //   chorus 0, expression 127, variation 0, tone controls 64.
    ch_.fill(Channel{});
}

// =========================================================
//  Internal helpers (mtx_ held)
// =========================================================
void ChannelModel::bump_locked() noexcept {
    version_.fetch_add(1, std::memory_order_release);
}

void ChannelModel::send_locked(u8 status, u8 d0, u8 d1, bool queued) {
    if (!sink_) return;
    if (queued) sink_->short_msg(status, d0, d1);
    else        sink_->short_direct(status, d0, d1);
}

int ChannelModel::effective_volume_locked(int ch) const noexcept {
    const Channel& c = ch_[static_cast<std::size_t>(ch)];
    if (c.muted) return 0;

    bool any_solo = false;
    for (const auto& x : ch_) {
        if (x.soloed) { any_solo = true; break; }
    }
    if (any_solo && !c.soloed) return 0;

    // master_volume_ is 0-100; round to nearest so 100 == unchanged.
    const int scaled = (static_cast<int>(c.volume) * master_volume_ + 50) / 100;
    return scaled < 0 ? 0 : (scaled > 127 ? 127 : scaled);
}

void ChannelModel::send_voice_locked(int ch, bool queued) {
    if (!sink_) return;
    const Channel& c = ch_[static_cast<std::size_t>(ch)];
    const u8 cc = static_cast<u8>(0xB0 | ch);
    send_locked(cc, 0,  c.msb, queued);
    send_locked(cc, 32, c.lsb, queued);
    send_locked(static_cast<u8>(0xC0 | ch), c.program, 0, queued);
}

void ChannelModel::send_param_locked(int ch, Param p, bool queued) {
    if (!sink_) return;
    const Channel& c = ch_[static_cast<std::size_t>(ch)];
    const u8 cc = static_cast<u8>(0xB0 | ch);

    switch (p) {
        case Param::Volume:
            send_locked(cc, 7, static_cast<u8>(effective_volume_locked(ch)), queued);
            break;
        case Param::Pan:        send_locked(cc, 10, c.pan,        queued); break;
        case Param::Expression: send_locked(cc, 11, c.expression, queued); break;
        case Param::Reverb:     send_locked(cc, 91, c.reverb,     queued); break;
        case Param::Chorus:
            // Montage-style destinations: chorus travels on CC#94.
            send_locked(cc, chorus_redirect_ ? 94 : 93, c.chorus, queued);
            break;
        case Param::Variation:
            // With the redirect on, CC#94 belongs to chorus — variation
            // is tracked in the model but kept off the wire.
            if (!chorus_redirect_) send_locked(cc, 94, c.variation, queued);
            break;
        case Param::FilterRes:  send_locked(cc, 71, c.filter_res, queued); break;
        case Param::Release:    send_locked(cc, 72, c.release,    queued); break;
        case Param::Attack:     send_locked(cc, 73, c.attack,     queued); break;
        case Param::Brightness: send_locked(cc, 74, c.brightness, queued); break;
        case Param::Voice:      send_voice_locked(ch, queued);              break;
        case Param::Count:      break;
    }
}

// Full state of one channel, in the order a synth wants it:
// bank select -> program change -> controllers.
void ChannelModel::sync_channel_locked(int ch, bool queued) {
    if (!sink_) return;
    send_voice_locked(ch, queued);
    for (int i = 0; i < static_cast<int>(Param::Voice); ++i) {
        send_param_locked(ch, static_cast<Param>(i), queued);
    }
}

void ChannelModel::sync_all_locked(bool queued) {
    if (!sink_) return;
    for (int ch = 0; ch < kChannels; ++ch) sync_channel_locked(ch, queued);
}

// Voice + controllers back to the startup defaults. Mute/solo are
// PSRstudio-side monitor state and are left to the caller.
void ChannelModel::reset_params_locked(int ch) noexcept {
    Channel& c = ch_[static_cast<std::size_t>(ch)];
    const bool muted  = c.muted;
    const bool soloed = c.soloed;
    c = Channel{};
    c.muted  = muted;
    c.soloed = soloed;
}

// One controller parameter. Returns false if dropped by a lock.
bool ChannelModel::apply_param_locked(int ch, Param p, int value, Source src) {
    Channel& c = ch_[static_cast<std::size_t>(ch)];

    if (obeys_locks(src) && (c.locks & bit(p))) return false;

    u8& slot = field_of(c, p);
    const u8 v = clamp7(value);
    const bool changed = (slot != v);
    slot = v;

    if (src == Source::User) c.locks = static_cast<u16>(c.locks | bit(p));

    send_param_locked(ch, p, is_queued(src));

    if (changed || src == Source::User) bump_locked();
    return true;
}

// One piece of the voice: part 0 = bank MSB, 1 = bank LSB, 2 = program.
bool ChannelModel::apply_voice_part_locked(int ch, int part, int value, Source src) {
    Channel& c = ch_[static_cast<std::size_t>(ch)];

    if (obeys_locks(src) && (c.locks & bit(Param::Voice))) return false;

    const u8 v = clamp7(value);
    u8& slot = (part == 0) ? c.msb : (part == 1 ? c.lsb : c.program);
    const bool changed = (slot != v);
    slot = v;

    if (src == Source::User) c.locks = static_cast<u16>(c.locks | bit(Param::Voice));

    const bool q = is_queued(src);
    if (part == 0)      send_locked(static_cast<u8>(0xB0 | ch), 0,  v, q);
    else if (part == 1) send_locked(static_cast<u8>(0xB0 | ch), 32, v, q);
    else                send_locked(static_cast<u8>(0xC0 | ch), v,  0, q);

    if (changed || src == Source::User) bump_locked();
    return true;
}

// =========================================================
//  Device attachment
// =========================================================
void ChannelModel::attach_sink_and_sync(prs::MidiSink* sink) {
    std::lock_guard<std::mutex> lk(mtx_);
    sink_ = sink;
    if (sink_) sync_all_locked(/*queued=*/false);
}

void ChannelModel::detach_sink() {
    std::lock_guard<std::mutex> lk(mtx_);
    sink_ = nullptr;
}

bool ChannelModel::has_sink() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return sink_ != nullptr;
}

void ChannelModel::sync_all() {
    std::lock_guard<std::mutex> lk(mtx_);
    sync_all_locked(/*queued=*/false);
}

void ChannelModel::set_chorus_redirect(bool on) {
    std::lock_guard<std::mutex> lk(mtx_);
    chorus_redirect_ = on;
}

// =========================================================
//  Parameter changes
// =========================================================
bool ChannelModel::is_tracked_cc(int cc) noexcept {
    if (cc == 0 || cc == 32) return true;
    Param p;
    return param_for_cc(cc, p);
}

bool ChannelModel::set_cc(int ch, int cc, int value, Source src) {
    if (!valid_ch(ch)) return false;
    std::lock_guard<std::mutex> lk(mtx_);

    if (cc == 0)  return apply_voice_part_locked(ch, 0, value, src);
    if (cc == 32) return apply_voice_part_locked(ch, 1, value, src);

    Param p;
    if (!param_for_cc(cc, p)) return false;
    return apply_param_locked(ch, p, value, src);
}

bool ChannelModel::set_program(int ch, int program, Source src) {
    if (!valid_ch(ch)) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    return apply_voice_part_locked(ch, 2, program, src);
}

bool ChannelModel::set_voice(int ch, int msb, int lsb, int program, Source src) {
    if (!valid_ch(ch)) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    Channel& c = ch_[static_cast<std::size_t>(ch)];

    if (obeys_locks(src) && (c.locks & bit(Param::Voice))) return false;

    c.msb     = clamp7(msb);
    c.lsb     = clamp7(lsb);
    c.program = clamp7(program);

    if (src == Source::User) c.locks = static_cast<u16>(c.locks | bit(Param::Voice));

    send_voice_locked(ch, is_queued(src));
    bump_locked();
    return true;
}

void ChannelModel::note_reset_all_controllers(int ch, Source src) {
    if (!valid_ch(ch)) return;
    std::lock_guard<std::mutex> lk(mtx_);

    // MIDI spec: Reset All Controllers returns expression to 127 (and
    // clears pitch bend, mod wheel, sustain...). Volume / pan / bank /
    // program are NOT reset by the spec, but synths differ on the
    // effect sends, so every controller is re-sent from the model.
    ch_[static_cast<std::size_t>(ch)].expression = 127;

    const bool q = is_queued(src);
    for (int i = 0; i < static_cast<int>(Param::Voice); ++i) {
        send_param_locked(ch, static_cast<Param>(i), q);
    }
    bump_locked();
}

// =========================================================
//  Mixer-side state
// =========================================================
void ChannelModel::set_mute(int ch, bool on) {
    if (!valid_ch(ch)) return;
    std::lock_guard<std::mutex> lk(mtx_);
    ch_[static_cast<std::size_t>(ch)].muted = on;
    send_param_locked(ch, Param::Volume, /*queued=*/false);
    bump_locked();
}

void ChannelModel::set_solo(int ch, bool on) {
    if (!valid_ch(ch)) return;
    std::lock_guard<std::mutex> lk(mtx_);
    ch_[static_cast<std::size_t>(ch)].soloed = on;
    // Soloing one channel changes every channel's effective volume.
    for (int i = 0; i < kChannels; ++i) send_param_locked(i, Param::Volume, false);
    bump_locked();
}

void ChannelModel::set_master_volume(int percent) {
    std::lock_guard<std::mutex> lk(mtx_);
    master_volume_ = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    for (int i = 0; i < kChannels; ++i) send_param_locked(i, Param::Volume, false);
    bump_locked();
}

int ChannelModel::master_volume() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return master_volume_;
}

// =========================================================
//  Locks
// =========================================================
void ChannelModel::clear_locks() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& c : ch_) c.locks = 0;
    bump_locked();
}

void ChannelModel::unlock(int ch, Param p) {
    if (!valid_ch(ch) || p == Param::Count) return;
    std::lock_guard<std::mutex> lk(mtx_);
    Channel& c = ch_[static_cast<std::size_t>(ch)];
    c.locks = static_cast<u16>(c.locks & ~bit(p));
    bump_locked();
}

// =========================================================
//  Style load / re-apply
// =========================================================
void ChannelModel::apply_setup(std::span<const prs::ChannelSetup> setups,
                               u16 reset_mask) {
    std::lock_guard<std::mutex> lk(mtx_);

    // 1. A new setup invalidates every "user touched this" lock.
    for (auto& c : ch_) c.locks = 0;

    // 2. Idle channels back to the startup defaults. Mute/solo cleared
    //    too (an idle channel left soloed would silence everything else).
    for (int ch = 0; ch < kChannels; ++ch) {
        if ((reset_mask >> ch) & 1u) {
            reset_params_locked(ch);
            Channel& c = ch_[static_cast<std::size_t>(ch)];
            c.muted  = false;
            c.soloed = false;
        }
    }

    // 3. Store what the style defines.
    for (const auto& s : setups) {
        if (s.ch >= kChannels) continue;
        Channel& c = ch_[s.ch];
        c.msb        = clamp7(s.msb);
        c.lsb        = clamp7(s.lsb);
        c.program    = clamp7(s.program);
        c.volume     = clamp7(s.volume);
        c.pan        = clamp7(s.pan);
        c.expression = clamp7(s.expression);
        c.reverb     = clamp7(s.reverb);
        c.chorus     = clamp7(s.chorus);
        c.variation  = clamp7(s.variation);
        c.filter_res = clamp7(s.filter_res);
        c.release    = clamp7(s.release);
        c.attack     = clamp7(s.attack);
        c.brightness = clamp7(s.brightness);
    }

    bump_locked();

    // 4. Full dump: the device now matches the model on all 16 channels,
    //    including channels the style did not touch (a header SysEx such
    //    as XG System On may just have reset the synth).
    sync_all_locked(/*queued=*/false);
}

// =========================================================
//  SysEx
// =========================================================
bool ChannelModel::handle_sysex(std::span<const u8> payload, Source src,
                                const SysexSender& send,
                                const PartMapper& map_part) {
    // Style / SMF SysEx events carry the closing F7 inside their data;
    // strip it for classification only (the raw payload is forwarded
    // untouched).
    std::span<const u8> p = payload;
    if (!p.empty() && p.back() == 0xF7) p = p.first(p.size() - 1);

    const bool queued = is_queued(src);
    std::lock_guard<std::mutex> lk(mtx_);

    // ---- GM / GM2 System On:  7E <dev> 09 01|03 -------------------
    const bool gm_on = p.size() >= 4 && p[0] == 0x7E && p[2] == 0x09 &&
                       (p[3] == 0x01 || p[3] == 0x03);
    // ---- XG System On:  43 1n 4C 00 00 7E 00 ----------------------
    const bool xg_on = p.size() >= 7 && p[0] == 0x43 && (p[1] & 0xF0) == 0x10 &&
                       p[2] == 0x4C && p[3] == 0x00 && p[4] == 0x00 &&
                       p[5] == 0x7E && p[6] == 0x00;

    if (gm_on || xg_on) {
        if (send) send(payload);
        // The synth has reset itself to ITS defaults: bring the model to
        // ITS defaults and push them, so both sides agree again.
        for (int ch = 0; ch < kChannels; ++ch) reset_params_locked(ch);
        bump_locked();
        sync_all_locked(queued);
        return true;
    }

    // ---- XG Multi Part parameter:  43 1n 4C 08 <part> <param> <val> ----
    // Converted to the equivalent controller message so that mute / solo /
    // master volume / user locks all apply, and the model stays exact.
    if (p.size() >= 7 && p[0] == 0x43 && (p[1] & 0xF0) == 0x10 &&
        p[2] == 0x4C && p[3] == 0x08) {
        const int xg_part = p[4];
        const int param   = p[5];
        const int value   = p[6];
        // XG part -> output channel (style routing, if the caller gave one).
        const int part = map_part ? map_part(xg_part) : xg_part;
        if (value <= 127 && (xg_part >= kChannels || part < 0 || part >= kChannels)) {
            // Addresses a part that maps to no output channel: forwarding it
            // raw would change the device without the model knowing.
            return true;
        }
        if (value <= 127) {
            switch (param) {
                case 0x00: return apply_voice_part_locked(part, 0, value, src);
                case 0x01: return apply_voice_part_locked(part, 1, value, src);
                case 0x02: return apply_voice_part_locked(part, 2, value, src);
                case 0x06: return apply_param_locked(part, Param::Volume,    value, src);
                case 0x07:
                    // XG pan 0 = "random": no controller equivalent.
                    if (value != 0) return apply_param_locked(part, Param::Pan, value, src);
                    break;
                case 0x08: return apply_param_locked(part, Param::Reverb,    value, src);
                case 0x09: return apply_param_locked(part, Param::Chorus,    value, src);
                case 0x0A: return apply_param_locked(part, Param::Variation, value, src);
                default: break;
            }
        }
    }

    // ---- everything else: pass through untouched ------------------
    if (send) send(payload);
    return true;
}

// =========================================================
//  Reading
// =========================================================
Snapshot ChannelModel::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    Snapshot s;
    s.ch            = ch_;
    s.master_volume = master_volume_;
    s.version       = version_.load(std::memory_order_acquire);
    return s;
}

Channel ChannelModel::channel(int ch) const {
    if (!valid_ch(ch)) return Channel{};
    std::lock_guard<std::mutex> lk(mtx_);
    return ch_[static_cast<std::size_t>(ch)];
}

int ChannelModel::effective_volume(int ch) const {
    if (!valid_ch(ch)) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    return effective_volume_locked(ch);
}

} // namespace chmodel


// ======================================================================
//   Arranger API
// ======================================================================

// High-level arranger API implementation.
//
// Wraps the transpose engine with:
//   1. SFF2 three-range pitch splitting
//   2. Slash-chord-aware MIDI chord detection
//   3. Phrase batching for chord-voicing channels
//
// See playback_controller.hpp for usage.


#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace arranger {

// =========================================================
//  Bass register anchor
// =========================================================
bool is_bass_channel(const prs::CasmChannel& cc) noexcept {
    // part_name is up to 8 chars, space-padded in the source file.
    // Case-insensitive substring match: "Bass", "bass", "BassLine"...
    char buf[9] = {};
    const std::string& n = cc.part_name;
    const std::size_t len = std::min<std::size_t>(n.size(), 8);
    for (std::size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(n[i])));
    }
    return std::strstr(buf, "bass") != nullptr;
}

int apply_bass_anchor(int pitch, const prs::CasmChannel& cc,
                      const prs::NoteRange& nr) noexcept {
    if (pitch < 0 || pitch > 127) return pitch;
    if (nr.ntr != prs::Ntr::RootTrans) return pitch;  // RootFixed/Guitar manage their own register
    if (!is_bass_channel(cc)) return pitch;

    // Anchor window: E1..Eb2 (MIDI 28..39). Only apply when the style's
    // own CASM note limits span the whole window — a style with a
    // deliberate low limit keeps its own register.
    constexpr int kAnchorLow  = 28;   // E1
    constexpr int kAnchorHigh = 39;   // Eb2
    if (nr.note_low > kAnchorLow || nr.note_high < kAnchorHigh) return pitch;

    // Fold the pitch class into the window: E -> 28, F -> 29, ... Eb -> 39.
    const int pc = ((pitch % 12) + 12) % 12;
    return kAnchorLow + ((pc - 4) + 12) % 12;
}

// =========================================================
//  Single-note transposition with three-range support
// =========================================================
int transpose_single_note(
    int src_pitch,
    const prs::CasmChannel& cc,
    const transpose::ChordCtx& src,
    const transpose::ChordCtx& dst) noexcept {

    if (src_pitch < 0 || src_pitch > 127) return src_pitch;

    // Skip if source channel has this pitch class muted for the dest chord.
    // (Caller should have already checked, but be defensive.)
    int src_pc = src_pitch % 12;
    if (cc.is_pitch_class_muted(src_pc)) return -1;

    // Pick the right NoteRange for this pitch (low/main/high).
    const prs::NoteRange& nr = cc.range_for_pitch(src_pitch);

    int pitch = transpose::note(src_pitch, nr, src, dst);

    // Bass register anchor: keep RootTrans bass channels in one
    // consistent low octave instead of letting the transposed line
    // wander or drop near-inaudible across chord changes.
    pitch = apply_bass_anchor(pitch, cc, nr);

    return pitch;
}

// =========================================================
//  Phrase-level transposition (chord-voicing channels)
//
//  Splits source events into three groups by pitch range:
//    - Low range events  → routed to transpose::note() with ctb2_low
//    - Main range events → routed to transpose::fit_phrase() with ctb2_main
//    - High range events → routed to transpose::note() with ctb2_high
//
//  This matches JJazzLab's per-range processing for SFF2 files:
//    "Split the source phrase in potentially 3 notes range (SFF2 support)"
//    (YamJJazzRhythmGenerator.java lines 561-635)
//
//  For single-ctb2 channels (SFF1 or unsplit SFF2), all events go
//  through fit_phrase() together — this is the most common case.
// =========================================================
void transpose_phrase(
    std::span<const PhraseInput> src_events,
    std::vector<int>& out_pitches,
    const prs::CasmChannel& cc,
    const transpose::ChordCtx& src,
    const transpose::ChordCtx& dst) {

    if (src_events.empty()) {
        out_pitches.clear();
        return;
    }

    out_pitches.resize(src_events.size());

    // SFF1 / single-ctb2 fast path: all events through fit_phrase()
    if (cc.is_single_ctb2()) {
        // Reuse the output vector's storage as the int span.
        // fit_phrase() requires same-size input and output spans.
        // PhraseInput and transpose::PhraseEvent must have the same layout
        // (both are {int pitch; int parent_index;}).
        static_assert(sizeof(PhraseInput) == sizeof(transpose::PhraseEvent),
                      "PhraseInput must match transpose::PhraseEvent layout");
        std::span<const transpose::PhraseEvent> events_span(
            reinterpret_cast<const transpose::PhraseEvent*>(src_events.data()),
            src_events.size());
        std::span<int> out_span(out_pitches.data(), out_pitches.size());
        int written = transpose::fit_phrase(events_span, out_span, cc.mid, src, dst);
        (void)written;  // fit_phrase always returns src_events.size() on success
        return;
    }

    // SFF2 three-range path: split events by pitch range.
    //
    // We need to call fit_phrase() with the main-range events together
    // (so the permutation search sees all chord tones), and route low/high
    // events through per-note note() with their respective ctb2 blocks.
    //
    // Output indices must be preserved (caller knows which input maps to
    // which output by position).

    // Pass 1: classify each event into low/main/high, remember original index.
    std::array<int, 32> low_indices{};     // indices into src_events
    std::array<int, 32> main_indices{};
    std::array<int, 32> high_indices{};
    int low_count = 0, main_count = 0, high_count = 0;

    for (size_t i = 0; i < src_events.size(); ++i) {
        int p = src_events[i].src_pitch;
        if (p < 0 || p > 127) {
            out_pitches[i] = p;  // bypass out-of-range
            continue;
        }
        if (p < cc.mid_low_pitch) {
            if (low_count < 32) low_indices[low_count++] = static_cast<int>(i);
        } else if (p > cc.mid_high_pitch) {
            if (high_count < 32) high_indices[high_count++] = static_cast<int>(i);
        } else {
            if (main_count < 32) main_indices[main_count++] = static_cast<int>(i);
        }
    }

    // Pass 2: route low events through note() with ctb2_low.
    for (int i = 0; i < low_count; ++i) {
        int idx = low_indices[static_cast<size_t>(i)];
        int p = src_events[static_cast<size_t>(idx)].src_pitch;
        out_pitches[static_cast<size_t>(idx)] =
            transpose::note(p, cc.low, src, dst);
    }

    // Pass 3: route high events through note() with ctb2_high.
    for (int i = 0; i < high_count; ++i) {
        int idx = high_indices[static_cast<size_t>(i)];
        int p = src_events[static_cast<size_t>(idx)].src_pitch;
        out_pitches[static_cast<size_t>(idx)] =
            transpose::note(p, cc.high, src, dst);
    }

    // Pass 4: route main events through fit_phrase() with ctb2_main.
    //
    // Build compact arrays for fit_phrase (it requires contiguous spans).
    // Use stack-allocated buffers since kMaxChordPhraseNotes=6 is small;
    // if main_count exceeds the cap, fit_phrase falls back internally.
    if (main_count > 0) {
        // For larger phrases we still need to call fit_phrase; use the
        // caller's vector as scratch via a temporary std::vector.
        std::vector<transpose::PhraseEvent> main_events;
        main_events.reserve(static_cast<size_t>(main_count));
        std::vector<int> main_out;
        main_out.reserve(static_cast<size_t>(main_count));

        for (int i = 0; i < main_count; ++i) {
            int idx = main_indices[static_cast<size_t>(i)];
            main_events.push_back({
                src_events[static_cast<size_t>(idx)].src_pitch,
                src_events[static_cast<size_t>(idx)].parent_index
            });
            main_out.push_back(0);
        }

        std::span<const transpose::PhraseEvent> events_span(
            main_events.data(), main_events.size());
        std::span<int> out_span(main_out.data(), main_out.size());
        int written = transpose::fit_phrase(events_span, out_span, cc.mid, src, dst);
        (void)written;  // count matches input size on success

        // Scatter main_out back to the original indices.
        for (int i = 0; i < main_count; ++i) {
            int idx = main_indices[static_cast<size_t>(i)];
            out_pitches[static_cast<size_t>(idx)] = main_out[static_cast<size_t>(i)];
        }
    }

    // MegaVoice articulation notes are handled by the caller (they are
    // never transposed at all — see transpose::megavoice).
}

// =========================================================
//  Retrigger for held notes
// =========================================================
transpose::RtrResult retrigger_held_note(
    int held_pitch,
    const prs::CasmChannel& cc,
    const transpose::ChordCtx& src,
    const transpose::ChordCtx& dst,
    bool src_was_transposed) noexcept {

    if (held_pitch < 0 || held_pitch > 127) {
        return {transpose::RtrAction::None, held_pitch};
    }

    const prs::NoteRange& nr = cc.range_for_pitch(held_pitch);
    return transpose::retrigger(held_pitch, nr, src, dst, src_was_transposed);
}

// =========================================================
//  Slash-chord-aware MIDI chord detector
//
//  Implements a pitch-class-set chord detector with slash-chord
//  bass detection. Mirrors JJazzLab's approach:
//    1. Bass = lowest pitch class
//    2. Build pitch-class set from all notes
//    3. Match against known chord-type masks
//    4. If bass != root, mark as slash chord
//
//  Chord-type masks mirror the kChordTones table in transpose.cpp
//  (CASM indices 0-34), but only the 19 types in chord::ChordType
//  are recognized (mapped via chord_type_to_casm).
// =========================================================
namespace {

// Chord-type masks indexed by chord::ChordType (cast to int).
// Each mask is a 12-bit set: bit i set means pitch class i is a chord tone
// (when root=0). Bit 0 = root, bit 4 = major 3rd, bit 7 = perfect 5th, etc.
//
// IMPORTANT: in C++ binary literals, the LEFTMOST digit is the MSB.
// So for bits {0, 4, 7} we write 0b000010010001 (bit 11 down to bit 0).
// Verified: 0b000010010001 = 0x091 = 1+16+128 = 145.
constexpr std::array<std::uint16_t, 37> kChordMasks = {
    0x000,  // None (unused)
    0x091,  // Maj      = {0,4,7}        = 1+16+128       = 0x091
    0x089,  // Min      = {0,3,7}        = 1+8+128        = 0x089
    0x491,  // Dom7     = {0,4,7,10}     = 1+16+128+1024  = 0x491
    0x891,  // Maj7     = {0,4,7,11}     = 1+16+128+2048  = 0x891
    0x489,  // Min7     = {0,3,7,10}     = 1+8+128+1024   = 0x489
    0x049,  // Dim      = {0,3,6}        = 1+8+64         = 0x049
    0x249,  // Dim7     = {0,3,6,9}      = 1+8+64+512     = 0x249
    0x111,  // Aug      = {0,4,8}        = 1+16+256       = 0x111
    0x0A1,  // Sus4     = {0,5,7}        = 1+32+128       = 0x0A1
    0x085,  // Sus2     = {0,2,7}        = 1+4+128        = 0x085
    0x291,  // Maj6     = {0,4,7,9}      = 1+16+128+512   = 0x291
    0x289,  // Min6     = {0,3,7,9}      = 1+8+128+512    = 0x289
    0x495,  // Dom9     = {0,2,4,7,10}   = 1+4+16+128+1024 = 0x495
    0x889,  // MinMaj7  = {0,3,7,11}     = 1+8+128+2048   = 0x889
    0x095,  // Add9     = {0,2,4,7}      = 1+4+16+128     = 0x095
    0x4A1,  // Sus4_7   = {0,5,7,10}     = 1+32+128+1024  = 0x4A1
    0x48D,  // Min9     = {0,2,3,7,10}   = 1+4+8+128+1024 = 0x48D
    0x895,  // Maj9     = {0,2,4,7,11}   = 1+4+16+128+2048 = 0x895
    0x449,  // Min7b5   = {0,3,6,10}     = 1+8+64+1024    = 0x449
    0x451,  // Dom7b5   = {0,4,6,10}     = 1+16+64+1024   = 0x451
    0x4D5,  // Dom7s11  = {0,2,4,6,7,10} = 1+4+16+64+128+1024 = 0x4D5
    0x493,  // Dom7b9   = {0,1,4,7,10}   = 1+2+16+128+1024 = 0x493
    0x499,  // Dom7s9   = {0,3,4,7,10}   = 1+8+16+128+1024 = 0x499
    0x591,  // Dom7b13  = {0,4,7,8,10}   = 1+16+128+256+1024 = 0x591
    0x511,  // Dom7Aug  = {0,4,8,10}     = 1+16+256+1024  = 0x511
    0x8D5,  // Maj7s11  = {0,2,4,6,7,11} = 1+4+16+64+128+2048 = 0x8D5
    0x911,  // Maj7Aug  = {0,4,8,11}     = 1+16+256+2048  = 0x911
    0x295,  // Maj69    = {0,2,4,7,9}    = 1+4+16+128+512 = 0x295
    0x08D,  // MinAdd9  = {0,2,3,7}      = 1+4+8+128      = 0x08D
    0x4AD,  // Min11    = {0,2,3,5,7,10} = 1+4+8+32+128+1024 = 0x4AD
    0x691,  // Dom13    = {0,4,7,9,10}   = 1+16+128+512+1024 = 0x691
    0x081,  // Power    = {0,7}          = 1+128          = 0x081
    0x849,  // MinMaj7b5 = {0,3,6,11}    = 1+8+64+2048    = 0x849
    0x851,  // Maj7b5   = {0,4,6,11}     = 1+16+64+2048   = 0x851
    0x051,  // Majb5    = {0,4,6}        = 1+16+64        = 0x051
    0x4B1,  // Dom11    = {0,4,5,7,10}   = 1+16+32+128+1024 = 0x4B1
};

// Rotates a 12-bit mask left by `root` positions (mod 12).
// Used to test "is this pitch-class set a Maj chord rooted at X?"
[[nodiscard]] constexpr std::uint16_t rotate_mask(std::uint16_t mask, int root) noexcept {
    // NOTE: use %= 12, not &= 11. 11 decimal is 0b1011, which is NOT
    // a power-of-2-minus-one, so & 11 is not equivalent to % 12.
    root %= 12;
    if (root == 0) return mask & 0xFFFu;
    return static_cast<std::uint16_t>(
        ((mask << root) | (mask >> (12 - root))) & 0xFFFu
    );
}

} // anonymous namespace

LiveChord detect_chord(std::span<const int> held_pitches) noexcept {
    LiveChord result;
    result.root = -1;
    result.type = chord::ChordType::None;
    result.bass = -1;

    if (held_pitches.size() < 3) {
        return result;  // Need at least 3 notes for chord detection
    }

    // Step 1: find the bass note (lowest pitch's pitch class).
    int lowest_pitch = held_pitches[0];
    for (int p : held_pitches) {
        if (p < lowest_pitch) lowest_pitch = p;
    }
    int bass_pc = lowest_pitch % 12;

    // Step 2: build the pitch-class set from all held notes.
    std::uint16_t pc_set = 0;
    int note_count = 0;
    for (int p : held_pitches) {
        if (p < 0 || p > 127) continue;
        int pc = p % 12;
        std::uint16_t bit = static_cast<std::uint16_t>(1u << pc);
        if ((pc_set & bit) == 0) {
            pc_set |= bit;
            ++note_count;
        }
    }

    if (note_count < 3) {
        return result;  // Need at least 3 distinct pitch classes
    }

    // Step 3: for each candidate root, try each chord type.
    // A chord matches if pc_set ⊇ rotated_mask (all chord tones present).
    // Among matches, prefer:
    //   - More chord tones matched (tighter fit)
    //   - Bass == root (non-slash) over bass != root (slash)
    int best_root = -1;
    chord::ChordType best_type = chord::ChordType::None;
    int best_tone_count = 0;
    bool best_is_non_slash = false;

    for (int root = 0; root < 12; ++root) {
        for (int t = 1; t < static_cast<int>(kChordMasks.size()); ++t) {
            std::uint16_t mask = kChordMasks[static_cast<size_t>(t)];
            std::uint16_t rotated = rotate_mask(mask, root);

            // All chord tones must be present in the pc_set.
            if ((pc_set & rotated) != rotated) continue;

            // Count how many bits in the mask (chord tone count).
            int tone_count = 0;
            std::uint16_t m = mask;
            while (m) { tone_count += m & 1; m >>= 1; }

            bool is_non_slash = (root == bass_pc);

            // Prefer: more chord tones, then non-slash over slash.
            if (best_root == -1
                || tone_count > best_tone_count
                || (tone_count == best_tone_count && is_non_slash && !best_is_non_slash)) {
                best_root = root;
                best_type = static_cast<chord::ChordType>(t);
                best_tone_count = tone_count;
                best_is_non_slash = is_non_slash;
            }
        }
    }

    if (best_root == -1) {
        return result;  // No chord matched
    }

    result.root = best_root;
    result.type = best_type;
    result.bass = best_is_non_slash ? -1 : bass_pc;
    return result;
}

LiveChord detect_chord_from_events(std::span<const MidiNoteEvent> events) noexcept {
    // Filter note-ons (velocity > 0) into a temporary pitch array.
    // Use a stack-allocated buffer to avoid heap allocation.
    std::array<int, 16> held_pitches{};
    int count = 0;

    for (const auto& ev : events) {
        if (ev.velocity > 0 && count < 16) {
            held_pitches[static_cast<size_t>(count++)] = ev.pitch;
        }
    }

    return detect_chord(std::span<const int>(held_pitches.data(),
                                             static_cast<size_t>(count)));
}

} // namespace arranger


// ======================================================================
//   Playback controller
// ======================================================================

#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace playback {

// =========================================================
//  Constructor / destructor
// =========================================================
PlaybackController::PlaybackController() = default;

PlaybackController::~PlaybackController() {
    request_stop();
    if (play_thread_.joinable()) {
        play_thread_.request_stop();
        play_thread_.join();
    }
}

// =========================================================
//  Loading
// =========================================================
bool PlaybackController::load_style(const std::string& path) {
    unload();
    style_ = std::make_unique<prs::PrsStyle>();
    if (!style_->parse(path)) {
        style_.reset();
        return false;
    }
    style_->cache_all_sections();
    mode_ = Mode::Style;

    // Set initial selected Main
    int main_idx = static_cast<int>(style_->find_main_A());
    player::g_selected_main.store(main_idx, std::memory_order_relaxed);

    return true;
}

bool PlaybackController::load_midi_file(const std::string& path) {
    unload();
    auto result = midi_player_.load(path);
    if (result != midi_player::Result::Ok) {
        std::cerr << std::format("[controller] MIDI load failed: {}\n",
            static_cast<int>(result));
        return false;
    }
    mode_ = Mode::MidiFile;
    return true;
}

void PlaybackController::unload() {
    request_stop();
    if (play_thread_.joinable()) {
        play_thread_.request_stop();
        play_thread_.join();
    }
    style_.reset();
    midi_player_.unload();
    mode_ = Mode::None;
    clear_queue();
}

// =========================================================
//  Queue state
// =========================================================
void PlaybackController::clear_queue() {
    queue_kind_ = QueueKind::None;
    queue_index_ = -1;
}

void PlaybackController::toggle_queue_fill() {
    if (queue_kind_ == QueueKind::Fill) clear_queue();
    else { queue_kind_ = QueueKind::Fill; queue_index_ = -1; }
}

void PlaybackController::set_queue_intro(int section_idx) {
    if (queue_kind_ == QueueKind::Intro && queue_index_ == section_idx)
        clear_queue();
    else { queue_kind_ = QueueKind::Intro; queue_index_ = section_idx; }
}

void PlaybackController::set_queue_ending(int section_idx) {
    queue_kind_ = QueueKind::Ending;
    queue_index_ = section_idx;
}

void PlaybackController::toggle_queue_break() {
    if (queue_kind_ == QueueKind::Break) clear_queue();
    else { queue_kind_ = QueueKind::Break; queue_index_ = -1; }
}

std::string PlaybackController::queue_status_text() const {
    const int main_idx = selected_main();
    const std::string main_name = (main_idx >= 0 && style_)
        ? style_->sections()[static_cast<std::size_t>(main_idx)].name
        : "(none)";

    switch (queue_kind_) {
        case QueueKind::None:
            return std::format("{} selected", main_name);
        case QueueKind::Fill:
            return std::format("Fill -> {}  on start", main_name);
        case QueueKind::Intro:
            if (queue_index_ >= 0 && style_) {
                return std::format("{} -> {}  on start",
                    style_->sections()[static_cast<std::size_t>(queue_index_)].name,
                    main_name);
            }
            return std::format("Intro -> {}  on start", main_name);
        case QueueKind::Ending:
            if (queue_index_ >= 0 && style_) {
                return std::format("{} -> stop  on start",
                    style_->sections()[static_cast<std::size_t>(queue_index_)].name);
            }
            return "Ending -> stop  on start";
        case QueueKind::Break:
            return std::format("Break -> {}  on start", main_name);
        default:
            return std::format("{} selected", main_name);
    }
}

// =========================================================
//  Playlist building
// =========================================================
std::vector<int> PlaybackController::build_playlist() const {
    std::vector<int> pl;
    if (!style_) return pl;

    int main_idx = selected_main();
    if (main_idx < 0) main_idx = static_cast<int>(style_->find_main_A());
    if (main_idx < 0) return pl;

    switch (queue_kind_) {
        case QueueKind::Ending:
            if (queue_index_ >= 0) pl.push_back(queue_index_);
            break;
        case QueueKind::Intro:
            if (queue_index_ >= 0) pl.push_back(queue_index_);
            pl.push_back(main_idx);
            break;
        case QueueKind::Break: {
            if (auto brk = style_->find_section("Fill In BA"); brk) {
                pl.push_back(static_cast<int>(*brk));
            }
            pl.push_back(main_idx);
            break;
        }
        case QueueKind::Fill: {
            if (auto fill = style_->find_corresponding_fill(
                    style_->sections()[static_cast<std::size_t>(main_idx)].name); fill) {
                pl.push_back(static_cast<int>(*fill));
            }
            pl.push_back(main_idx);
            break;
        }
        case QueueKind::None:
        default:
            pl.push_back(main_idx);
            break;
    }

    return pl;
}

// =========================================================
//  Transport
// =========================================================
bool PlaybackController::start_playback() {
    if (playing_.load() || !midi_out_) return false;
    if (mode_ == Mode::None) return false;

    // Stop any existing thread
    if (play_thread_.joinable()) {
        play_thread_.request_stop();
        play_thread_.join();
    }

    player::g_stop.store(false, std::memory_order_relaxed);
    player::g_requested_sec.store(-1, std::memory_order_relaxed);
    playing_.store(true, std::memory_order_relaxed);

    if (mode_ == Mode::Style) {
        auto playlist = build_playlist();
        if (playlist.empty()) {
            playing_.store(false, std::memory_order_relaxed);
            return false;
        }
        clear_queue();

        play_thread_ = std::jthread([this, playlist = std::move(playlist)](std::stop_token) {
            playback_thread_style(std::move(playlist));
        });
    } else {
        // MIDI file mode
        play_thread_ = std::jthread([this](std::stop_token) {
            playback_thread_midi();
        });
    }

    return true;
}

void PlaybackController::request_stop() {
    player::g_stop.store(true, std::memory_order_relaxed);
    player::g_requested_sec.store(-2, std::memory_order_relaxed);
}

// =========================================================
//  Playback threads
// =========================================================
void PlaybackController::playback_thread_style(std::vector<int> playlist) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif

    if (style_ && midi_out_ && remap_ && model_) {
        player_.play(*style_, *midi_out_, *remap_, playlist, *model_);
    }

    // Drain the MIDI output ring before signaling completion
    // (give the output thread time to send the last few messages)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    playing_.store(false, std::memory_order_relaxed);

    // Notify: Win32 via PostMessage, Qt via callback (or both)
#ifdef _WIN32
    if (notify_hwnd_) {
        PostMessageW(static_cast<HWND>(notify_hwnd_), WM_APP, 0, 0);
    }
#endif
    if (on_playback_finished_) {
        on_playback_finished_();
    }
}

void PlaybackController::playback_thread_midi() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif

    if (midi_out_) {
        midi_player_.play(*midi_out_, player::g_stop, 0, notify_hwnd_, model_);
    }

    playing_.store(false, std::memory_order_relaxed);

    if (on_playback_finished_) {
        on_playback_finished_();
    }
}

} // namespace playback