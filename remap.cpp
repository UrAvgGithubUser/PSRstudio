// remap.cpp — Voice/bank remapping table + destination-synth profile.
#include "remap.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <ranges>
#include <sstream>

namespace prs {

    // =========================================================
    //  Constructor — populates defaults
    // =========================================================
    VoiceRemap::VoiceRemap() {
        load_default_fallbacks();
        load_defaults();
    }

    // =========================================================
    //  Adding / removing entries
    // =========================================================
    void VoiceRemap::add(u8 src_msb, u8 src_lsb, u8 src_pc,
        u8 dst_msb, u8 dst_lsb, u8 dst_pc,
        std::string_view desc) {
        RemapEntry e;
        e.dst_msb = dst_msb;
        e.dst_lsb = dst_lsb;
        e.dst_pc = dst_pc;
        e.desc = std::string(desc);
        entries_[make_key(src_msb, src_lsb, src_pc)] = std::move(e);
    }

    bool VoiceRemap::remove(u8 src_msb, u8 src_lsb, u8 src_pc) {
        return entries_.erase(make_key(src_msb, src_lsb, src_pc)) > 0;
    }

    void VoiceRemap::clear() {
        entries_.clear();
    }

    // =========================================================
    //  Tier-2 fallback rules
    // =========================================================
    void VoiceRemap::set_fallback(u8 src_msb, FallbackRule rule) {
        fallbacks_[src_msb] = rule;
    }

    void VoiceRemap::remove_fallback(u8 src_msb) {
        fallbacks_.erase(src_msb);
    }

    void VoiceRemap::clear_fallbacks() {
        fallbacks_.clear();
    }

    // =========================================================
    //  Default fallback rules (matching the fallback table's tier-2 logic)
    // =========================================================
    void VoiceRemap::load_default_fallbacks() {
        // bank 8, 11, 104 → (0, 0, KEEP) — XG/Genos/SFF2 voices → GM
        set_fallback(8, { 0, 0,   PcMode::Keep,   0 });
        set_fallback(11, { 0, 0,   PcMode::Keep,   0 });
        set_fallback(104, { 0, 0,   PcMode::Keep,   0 });
        // bank 126 → (127, 0, FIXED=1) — SFX perc → Standard Kit 1
        set_fallback(126, { 127, 0, PcMode::Fixed, 1 });
        // bank 127 → (127, 0, FIXED=1) — unknown drum kits → Standard Kit 1
        // Any drum kit PC not in the explicit table gets mapped to Standard
        // Kit 1 so the I455 always plays a valid drum kit.
        set_fallback(127, { 127, 0, PcMode::Fixed, 1 });
        // bank 0 → no rule (passes through unchanged — GM voices)
    }

    // =========================================================
    //  Default PSR-I455 remap table
    //  (same ~50 entries as the fallback table)
    // =========================================================
    void VoiceRemap::load_defaults() {
        // ---- Yamaha internal voices (bank 8) → XGlite ----
        add(8, 0, 1, 0, 0, 1, "Grand Piano");
        add(8, 0, 2, 0, 0, 2, "Bright Piano");
        add(8, 0, 3, 0, 0, 3, "Electric Grand Piano");
        add(8, 0, 17, 0, 0, 17, "Drawbar Organ");
        add(8, 0, 18, 0, 112, 17, "Perc Organ -> Jazz Organ 1");
        add(8, 0, 49, 0, 112, 49, "String Ensemble");
        add(8, 0, 50, 0, 112, 50, "Chamber Strings");
        add(8, 0, 52, 0, 0, 53, "Choir Aahs");
        add(8, 1, 3, 0, 0, 3, "Electric Grand Piano var");
        add(8, 1, 18, 0, 113, 17, "Perc Organ -> Jazz Organ 2");
        add(8, 1, 19, 0, 112, 19, "Rock Organ");
        add(8, 1, 49, 0, 112, 49, "String Ensemble");
        add(8, 1, 50, 0, 113, 50, "Slow Strings");
        add(8, 2, 4, 0, 0, 4, "Honky-tonk Piano");
        add(8, 2, 18, 0, 112, 17, "Org -> Jazz Organ 1");
        add(8, 3, 4, 0, 0, 4, "Honky-tonk Piano var");
        add(8, 4, 4, 0, 0, 4, "Honky-tonk Piano var2");
        add(8, 4, 50, 0, 113, 50, "Strings -> Slow Strings");
        add(8, 6, 4, 0, 0, 4, "Honky-tonk Piano var3");
        add(8, 81, 113, 0, 0, 79, "Deduk/Whistle -> Ocarina");

        // ---- SFF2 extension voices (bank 104) → XGlite ----
        add(104, 0, 13, 0, 0, 13, "Marimba");
        add(104, 0, 52, 0, 0, 53, "Choir Aahs");
        add(104, 0, 89, 0, 0, 89, "New Age Pad");
        add(104, 0, 96, 0, 0, 96, "Sweep Pad");
        add(104, 1, 18, 0, 113, 17, "Org -> Jazz Organ 2");
        add(104, 1, 19, 0, 112, 19, "Rock Organ");
        add(104, 1, 39, 0, 116, 39, "Synth Bass");
        add(104, 2, 51, 0, 0, 51, "Synth Strings");
        add(104, 2, 90, 0, 0, 90, "Warm Pad");
        add(104, 3, 22, 0, 113, 22, "Trad. Accordion");
        add(104, 3, 90, 0, 0, 90, "Warm Pad var");
        add(104, 4, 91, 0, 0, 91, "Poly Synth Pad");
        add(104, 8, 39, 0, 116, 39, "Synth Bass var");
        add(104, 9, 82, 0, 124, 82, "Sawtooth -> Analog Lead");
        add(104, 10, 89, 0, 0, 89, "New Age Pad var");
        add(104, 21, 39, 0, 116, 39, "BsSynth -> I455 Synth Bass");

        // ---- GM LSB variant ----
        add(0, 110, 18, 0, 0, 18, "Perc Organ (LSB110) -> XGlite");

        // ---- Genos2 voices (bank 11) → XGlite ----
        add(11, 0, 49, 0, 0, 49, "Genos2 Strings -> XGlite");
        add(11, 0, 50, 0, 0, 50, "Genos2 Strings2 -> XGlite");
        add(11, 0, 52, 0, 0, 53, "Genos2 Choir -> XGlite Choir Aahs");
        add(11, 0, 53, 0, 0, 53, "Genos2 Choir var -> XGlite Choir Aahs");
        add(11, 0, 89, 0, 0, 89, "Genos2 Pad -> XGlite New Age Pad");
        add(11, 0, 90, 0, 0, 90, "Genos2 Pad2 -> XGlite Warm Pad");
        add(11, 0, 91, 0, 0, 91, "Genos2 Pad3 -> XGlite Poly Synth");

        // ---- Drum kit remaps (bank 127 → bank 127, different PC) ----
        add(127, 0, 28, 127, 0, 1, "Kit -> Standard Kit 1");
        add(127, 0, 42, 127, 0, 41, "Kit -> Brush Kit");
        add(127, 0, 61, 127, 0, 9, "Kit -> Room Kit");
        add(127, 0, 90, 127, 0, 88, "Kit -> Power Kit");
        add(127, 0, 91, 127, 0, 25, "Kit -> Electronic Kit");
        add(127, 0, 92, 127, 0, 26, "Kit -> Analog Kit");

        // ---- SFX percussion kits (bank 126) → bank 127 (I455 drum kits) ----
        // The I455 does NOT support bank 126 (SFX). All SFX kits must be
        // remapped to bank 127 (standard XG drum kit bank) with a PC the
        // I455 actually has.
        add(126, 0, 44, 127, 0, 1, "Perc kit -> Standard Kit 1");
        add(126, 0, 68, 127, 0, 1, "Genos2 perc -> Standard Kit 1");
        add(126, 9, 51, 127, 0, 26, "Genos2 Cinema drum -> Analog Kit");
    }

    // =========================================================
    //  find — tier-1 explicit lookup only
    // =========================================================
    std::optional<RemapEntry>
        VoiceRemap::find(u8 msb, u8 lsb, u8 pc) const {
        const auto it = entries_.find(make_key(msb, lsb, pc));
        if (it == entries_.end()) return std::nullopt;
        return it->second;
    }

    // =========================================================
    //  apply — tier-1 then tier-2
    // =========================================================
    bool VoiceRemap::apply(u8 msb, u8 lsb, u8 pc,
        u8& dm, u8& dl, u8& dp) const {
        // Tier 1: explicit table lookup
        if (const auto it = entries_.find(make_key(msb, lsb, pc));
            it != entries_.end()) {
            dm = it->second.dst_msb;
            dl = it->second.dst_lsb;
            dp = it->second.dst_pc;
            return true;
        }

        // Tier 2: fallback rule by bank MSB
        if (const auto fb = fallbacks_.find(msb); fb != fallbacks_.end()) {
            const FallbackRule& r = fb->second;
            dm = r.dst_msb;
            dl = r.dst_lsb;
            switch (r.pc_mode) {
            case PcMode::Keep:  dp = pc;          break;
            case PcMode::Fixed: dp = r.fixed_pc;  break;
            }
            return true;
        }

        // No rule — pass through unchanged
        return false;
    }

    // =========================================================
    //  clear_all — reset to empty profile
    // =========================================================
    void VoiceRemap::clear_all() {
        entries_.clear();
        fallbacks_.clear();
        montage_ = MontageConfig{};
        name_.clear();
        description_.clear();
        source_path_.clear();
    }

    // =========================================================
    //  scan_folder — list .remap files in a folder (sorted)
    // =========================================================
    std::vector<std::filesystem::path>
        VoiceRemap::scan_folder(const std::filesystem::path& dir) {
        std::vector<std::filesystem::path> out;
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) return out;
        if (!std::filesystem::is_directory(dir, ec)) return out;

        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const auto& path = entry.path();
            if (path.extension() == ".remap" || path.extension() == ".REMAP") {
                out.push_back(path);
            }
        }
        std::ranges::sort(out);
        return out;
    }

    // =========================================================
    //  Legacy text parser (entries only) — kept for backward compat
    //
    //  Each line:
    //    # comment
    //    src_msb src_lsb src_pc -> dst_msb dst_lsb dst_pc # description
    // =========================================================
    namespace {

        // Parse a single unsigned integer from a string_view, advancing the view.
        [[nodiscard]] bool parse_u8(std::string_view& sv, u8& out) {
            while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
                sv.remove_prefix(1);
            if (sv.empty()) return false;

            std::size_t end = 0;
            while (end < sv.size() && sv[end] != ' ' && sv[end] != '\t' &&
                sv[end] != '#' && sv[end] != '-' && sv[end] != '\r')
                ++end;

            const auto token = sv.substr(0, end);
            sv.remove_prefix(end);

            int val = 0;
            const auto res = std::from_chars(token.data(), token.data() + token.size(), val);
            if (res.ec != std::errc{} || val < 0 || val > 255) return false;
            out = static_cast<u8>(val);
            return true;
        }

        [[nodiscard]] bool parse_arrow(std::string_view& sv) {
            while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
                sv.remove_prefix(1);
            if (sv.size() < 2 || sv[0] != '-' || sv[1] != '>') return false;
            sv.remove_prefix(2);
            return true;
        }

        [[nodiscard]] std::string parse_description(std::string_view sv) {
            while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
                sv.remove_prefix(1);
            if (sv.empty() || sv.front() != '#') return "";
            sv.remove_prefix(1);
            while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
                sv.remove_prefix(1);
            std::string result(sv);
            if (!result.empty() && result.back() == '\r') result.pop_back();
            return result;
        }

    } // anonymous namespace

    std::size_t VoiceRemap::load_from_text(std::string_view text) {
        std::size_t count = 0;
        std::size_t line_no = 0;

        while (!text.empty()) {
            ++line_no;
            const auto nl = text.find('\n');
            std::string_view line = (nl == std::string_view::npos)
                ? text : text.substr(0, nl);
            if (nl == std::string_view::npos)
                text.remove_prefix(text.size());
            else
                text.remove_prefix(nl + 1);

            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.remove_prefix(1);

            if (line.empty() || line.front() == '#') continue;
            // Skip @directives in legacy mode — they need the profile parser.
            if (line.front() == '@') continue;

            u8 sm, sl, sp, dm, dl, dp;
            std::string_view sv = line;

            if (!parse_u8(sv, sm) || !parse_u8(sv, sl) || !parse_u8(sv, sp)) {
                std::cerr << std::format("[remap] line {}: bad source (got {} {} {})\n",
                    line_no, sm, sl, sp);
                continue;
            }
            if (!parse_arrow(sv)) {
                std::cerr << std::format("[remap] line {}: missing '->' separator\n", line_no);
                continue;
            }
            if (!parse_u8(sv, dm) || !parse_u8(sv, dl) || !parse_u8(sv, dp)) {
                std::cerr << std::format("[remap] line {}: bad destination\n", line_no);
                continue;
            }
            const std::string desc = parse_description(sv);

            add(sm, sl, sp, dm, dl, dp, desc);
            ++count;
        }

        return count;
    }

    std::size_t VoiceRemap::load_from_file(const std::filesystem::path& path) {
        std::ifstream f(path);
        if (!f) {
            std::cerr << std::format("[remap] Cannot open '{}'\n", path.string());
            return 0;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        return load_from_text(ss.str());
    }

    // =========================================================
    //  Profile parser (full .remap format)
    //
    //  Directives (start with @):
    //    @name <free text>                       profile name (for UI)
    //    @description <free text>                longer description
    //    @montage enabled=<bool> channel_remap=<bool> redirect_chorus_to_variation=<bool>
    //    @fallback bank=<n> dst_msb=<n> dst_lsb=<n> pc_mode=keep|fixed [fixed_pc=<n>]
    //
    //  Entry lines (no @):
    //    <sm> <sl> <sp> -> <dm> <dl> <dp> # description
    //
    //  Bool values: true/false, yes/no, on/off, 1/0 (case-insensitive).
    //  Quoted strings: "..." with no embedded quotes (for kit names).
    // =========================================================
    namespace {

        // Trim leading/trailing whitespace from a string_view.
        std::string_view trim(std::string_view sv) {
            while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
                sv.remove_prefix(1);
            while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r'))
                sv.remove_suffix(1);
            return sv;
        }

        // Parse a bool from a string_view token (case-insensitive).
        bool parse_bool(std::string_view tok) {
            // Lowercase in place
            char buf[8] = {};
            std::size_t n = std::min<std::size_t>(tok.size(), 8);
            for (std::size_t i = 0; i < n; ++i) {
                char c = tok[i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                buf[i] = c;
            }
            std::string_view lo(buf, n);
            if (lo == "true" || lo == "yes" || lo == "on" || lo == "1") return true;
            if (lo == "false" || lo == "no" || lo == "off" || lo == "0") return false;
            return false;
        }

        // Parse a u8 from a string_view token.
        bool parse_u8_view(std::string_view tok, u8& out) {
            int val = 0;
            const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), val);
            if (res.ec != std::errc{} || val < 0 || val > 255) return false;
            out = static_cast<u8>(val);
            return true;
        }

        // Parse a key=value pair out of a string_view. Returns the value
        // (or empty string_view if not found) and removes the consumed
        // portion from the input.
        std::string_view consume_kv(std::string_view& sv, std::string_view key) {
            // Skip whitespace
            while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
                sv.remove_prefix(1);
            if (sv.size() < key.size() + 1) return {};
            if (sv.substr(0, key.size()) != key) return {};
            sv.remove_prefix(key.size());
            if (sv.empty() || sv.front() != '=') return {};
            sv.remove_prefix(1);
            // If value starts with a quote, take until end-quote
            if (!sv.empty() && sv.front() == '"') {
                sv.remove_prefix(1);
                const auto end_q = sv.find('"');
                if (end_q == std::string_view::npos) return {};
                std::string_view val = sv.substr(0, end_q);
                sv.remove_prefix(end_q + 1);
                return val;
            }
            // Otherwise take until whitespace
            std::size_t end = 0;
            while (end < sv.size() && sv[end] != ' ' && sv[end] != '\t' && sv[end] != '\r')
                ++end;
            std::string_view val = sv.substr(0, end);
            sv.remove_prefix(end);
            return val;
        }

    } // anonymous namespace

    bool VoiceRemap::load_profile_from_text(std::string_view text) {
        clear_all();
        // Always start with the built-in fallback rules; the file can
        // override them via @fallback lines.
        load_default_fallbacks();

        bool ok = true;
        std::size_t line_no = 0;

        while (!text.empty()) {
            ++line_no;
            const auto nl = text.find('\n');
            std::string_view line = (nl == std::string_view::npos)
                ? text : text.substr(0, nl);
            if (nl == std::string_view::npos) text.remove_prefix(text.size());
            else                              text.remove_prefix(nl + 1);

            line = trim(line);
            if (line.empty() || line.front() == '#') continue;

            // ----- Directive -----
            if (line.front() == '@') {
                line.remove_prefix(1);
                // Split "directive rest"
                std::string_view dir;
                std::size_t sp = 0;
                while (sp < line.size() && line[sp] != ' ' && line[sp] != '\t') ++sp;
                dir = line.substr(0, sp);
                std::string_view rest = (sp < line.size()) ? line.substr(sp) : "";

                if (dir == "name") {
                    name_ = std::string(trim(rest));
                }
                else if (dir == "description") {
                    description_ = std::string(trim(rest));
                }
                else if (dir == "montage") {
                    // @montage enabled=true channel_remap=true redirect_chorus_to_variation=false
                    std::string_view sv = rest;
                    std::string_view v;
                    if (!(v = consume_kv(sv, "enabled")).empty())
                        montage_.enabled = parse_bool(v);
                    if (!(v = consume_kv(sv, "channel_remap")).empty())
                        montage_.channel_remap = parse_bool(v);
                    if (!(v = consume_kv(sv, "redirect_chorus_to_variation")).empty())
                        montage_.redirect_chorus_to_variation = parse_bool(v);
                }
                else if (dir == "fallback") {
                    // @fallback bank=8 dst_msb=0 dst_lsb=0 pc_mode=keep
                    // @fallback bank=126 dst_msb=127 dst_lsb=0 pc_mode=fixed fixed_pc=1
                    std::string_view sv = rest;
                    std::string_view v;
                    u8 bank = 0, dst_msb = 0, dst_lsb = 0, fixed_pc = 0;
                    bool got_bank = false, got_msb = false, got_lsb = false;
                    bool got_pc_mode = false;
                    bool is_fixed = false;
                    if (!(v = consume_kv(sv, "bank")).empty())    got_bank = parse_u8_view(v, bank);
                    if (!(v = consume_kv(sv, "dst_msb")).empty()) got_msb = parse_u8_view(v, dst_msb);
                    if (!(v = consume_kv(sv, "dst_lsb")).empty()) got_lsb = parse_u8_view(v, dst_lsb);
                    if (!(v = consume_kv(sv, "pc_mode")).empty()) {
                        got_pc_mode = true;
                        char buf[8] = {};
                        std::size_t n = std::min<std::size_t>(v.size(), 8);
                        for (std::size_t i = 0; i < n; ++i) {
                            char c = v[i];
                            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                            buf[i] = c;
                        }
                        std::string_view lo(buf, n);
                        if (lo == "fixed") is_fixed = true;
                        else if (lo != "keep") got_pc_mode = false;
                    }
                    if (!(v = consume_kv(sv, "fixed_pc")).empty()) parse_u8_view(v, fixed_pc);
                    if (got_bank && got_msb && got_lsb && got_pc_mode) {
                        FallbackRule r;
                        r.dst_msb = dst_msb;
                        r.dst_lsb = dst_lsb;
                        r.pc_mode = is_fixed ? PcMode::Fixed : PcMode::Keep;
                        r.fixed_pc = is_fixed ? fixed_pc : 0;
                        set_fallback(bank, r);
                    }
                    else {
                        std::cerr << std::format("[remap] line {}: bad @fallback\n", line_no);
                        ok = false;
                    }
                }
                else {
                    std::cerr << std::format("[remap] line {}: unknown directive '@{}'\n",
                        line_no, std::string(dir));
                    // unknown directive — not fatal
                }
                continue;
            }

            // ----- Explicit entry -----
            u8 sm, sl, sp, dm, dl, dp;
            std::string_view sv = line;
            if (!parse_u8(sv, sm) || !parse_u8(sv, sl) || !parse_u8(sv, sp)) {
                std::cerr << std::format("[remap] line {}: bad source\n", line_no);
                ok = false;
                continue;
            }
            if (!parse_arrow(sv)) {
                std::cerr << std::format("[remap] line {}: missing '->'\n", line_no);
                ok = false;
                continue;
            }
            if (!parse_u8(sv, dm) || !parse_u8(sv, dl) || !parse_u8(sv, dp)) {
                std::cerr << std::format("[remap] line {}: bad destination\n", line_no);
                ok = false;
                continue;
            }
            const std::string desc = parse_description(sv);
            add(sm, sl, sp, dm, dl, dp, desc);
        }

        return ok;
    }

    bool VoiceRemap::load_profile_from_file(const std::filesystem::path& path) {
        std::ifstream f(path);
        if (!f) {
            std::cerr << std::format("[remap] Cannot open '{}'\n", path.string());
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        const bool ok = load_profile_from_text(ss.str());
        if (ok) source_path_ = path;
        return ok;
    }

    // =========================================================
    //  Diagnostics
    // =========================================================
    void VoiceRemap::dump() const {
        std::cout << std::format("[REMAP] {} explicit entries, {} fallback rules\n",
            entries_.size(), fallbacks_.size());
        std::cout << std::format("  name:        '{}'\n", name_);
        std::cout << std::format("  description: '{}'\n", description_);
        std::cout << std::format("  source:      '{}'\n", source_path_.string());
        std::cout << std::format("  montage:     enabled={} channel_remap={} redirect_cc93_94={}\n",
            montage_.enabled, montage_.channel_remap, montage_.redirect_chorus_to_variation);

        auto sorted = entries_sorted();
        std::cout << "  Explicit entries:\n";
        for (const auto& e : sorted) {
            std::cout << std::format("    {}/{}/PC{}  ->  {}/{}/PC{}  {}\n",
                e.src_msb, e.src_lsb, e.src_pc,
                e.dst_msb, e.dst_lsb, e.dst_pc,
                e.desc);
        }

        std::cout << "  Fallback rules:\n";
        for (const auto& [msb, rule] : fallbacks_) {
            const char* mode = (rule.pc_mode == PcMode::Keep) ? "KEEP" : "FIXED";
            std::cout << std::format("    bank {} -> {}/{}/{}",
                msb, rule.dst_msb, rule.dst_lsb, mode);
            if (rule.pc_mode == PcMode::Fixed)
                std::cout << std::format("={}", rule.fixed_pc);
            std::cout << '\n';
        }
    }

    std::vector<VoiceRemap::FullEntry> VoiceRemap::entries_sorted() const {
        std::vector<FullEntry> out;
        out.reserve(entries_.size());
        for (const auto& [key, entry] : entries_) {
            FullEntry fe;
            fe.src_msb = static_cast<u8>((key >> 16) & 0xFF);
            fe.src_lsb = static_cast<u8>((key >> 8) & 0xFF);
            fe.src_pc = static_cast<u8>(key & 0xFF);
            fe.dst_msb = entry.dst_msb;
            fe.dst_lsb = entry.dst_lsb;
            fe.dst_pc = entry.dst_pc;
            fe.desc = entry.desc;
            out.push_back(std::move(fe));
        }
        std::ranges::sort(out, [](const FullEntry& a, const FullEntry& b) {
            if (a.src_msb != b.src_msb) return a.src_msb < b.src_msb;
            if (a.src_lsb != b.src_lsb) return a.src_lsb < b.src_lsb;
            return a.src_pc < b.src_pc;
            });
        return out;
    }

} // namespace prs