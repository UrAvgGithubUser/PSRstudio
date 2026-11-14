// channel_model.cpp — the authoritative state of all 16 MIDI channels.
// See channel_model.hpp for the design and threading rules.

#include "channel_model.hpp"

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
