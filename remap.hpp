// remap.hpp — Voice/bank remapping table + destination-synth profile.
//
// Implements prs::RemapSource for the style parser's build_channel_setup().
// Uses std::unordered_map for O(1) lookups with no fixed capacity.
//
// Remaps can be added at runtime via add(), loaded from a text file via
// load_from_file(), or edited by a UI. Tier-2 fallback rules (per-bank-MSB
// defaults) are also configurable.
//
// MONTAGE ESP VST mode is integrated: the remap module owns the MONTAGE
// file is a complete destination-synth profile.
//
// File format (one entry per line):
//   # comment
//   src_bank_msb  src_bank_lsb  src_pc  ->  dst_bank_msb  dst_bank_lsb  dst_pc
//   @montage  on
//   @chorus_redirect  on
//   @fold  9->1  10->2  11->3  12->4  13->5  14->6  15->7  16->8
//   @tier2  src_msb  ->  dst_msb  dst_lsb  dst_pc
#pragma once

#include "style.hpp"  // for prs::RemapSource, prs::RemapEntry, prs::u8

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace prs {

// =========================================================
//  VoiceRemap — translates voice selections between keyboards.
//
//  Implements RemapSource so it can be passed directly to
//  PrsStyle::build_channel_setup().
//
//  Also owns the MONTAGE ESP VST profile: channel remap,
//  CC redirection. A .remap file is a complete destination-
//  synth profile, not just a voice table.
// =========================================================
class VoiceRemap final : public RemapSource {
public:
    // Constructs with only the fallback rules (no voice remapping)
    // AND MONTAGE mode disabled (for hardware MIDI / GM soundfonts).
    VoiceRemap();

    ~VoiceRemap() override = default;

    VoiceRemap(const VoiceRemap&)            = default;
    VoiceRemap& operator=(const VoiceRemap&) = default;
    VoiceRemap(VoiceRemap&&)                 = default;
    VoiceRemap& operator=(VoiceRemap&&)      = default;

    // ----- Adding / removing entries -----
    void add(u8 src_msb, u8 src_lsb, u8 src_pc,
             u8 dst_msb, u8 dst_lsb, u8 dst_pc,
             std::string_view desc = "");
    bool remove(u8 src_msb, u8 src_lsb, u8 src_pc);
    void clear();
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // ----- Tier-2 fallback rules -----
    enum class PcMode : u8 { Keep, Fixed };

    struct FallbackRule {
        u8     dst_msb;
        u8     dst_lsb;
        PcMode pc_mode;
        u8     fixed_pc;
    };

    void set_fallback(u8 src_msb, FallbackRule rule);
    void remove_fallback(u8 src_msb);
    void clear_fallbacks();
    [[nodiscard]] std::size_t fallback_count() const noexcept { return fallbacks_.size(); }

    // ----- Legacy file loading (entries only, no metadata) -----
    // Parses the simple "8 0 1 -> 0 0 1 # desc" format. Does NOT
    // touch existing entries, fallbacks, or MONTAGE config — only
    // appends new entries. Use load_profile_from_text() for the
    // full format that replaces state.
    std::size_t load_from_file(const std::filesystem::path& path);
    std::size_t load_from_text(std::string_view text);

    // ----- Profile loading (full .remap format) -----
    // Parses a complete .remap profile: @name, @description,
    // @montage config, @fallback rules, and explicit voice entries.
    // REPLACES the current state (entries, fallbacks, MONTAGE config,
    // metadata). Returns true on success.
    bool load_profile_from_file(const std::filesystem::path& path);
    bool load_profile_from_text(std::string_view text);

    // Clear everything: entries, fallbacks, MONTAGE config,
    // metadata. Returns the object to a pristine empty state.
    void clear_all();

    // Scan a folder for .remap files. Returns a sorted list of paths.
    // Non-existent folder → empty list.
    static std::vector<std::filesystem::path>
        scan_folder(const std::filesystem::path& dir);

    // ----- RemapSource interface -----
    [[nodiscard]] std::optional<RemapEntry>
        find(u8 msb, u8 lsb, u8 pc) const override;
    [[nodiscard]] bool
        apply(u8 msb, u8 lsb, u8 pc, u8& dm, u8& dl, u8& dp) const override;

    // =========================================================
    //  MONTAGE / destination-synth profile
    // =========================================================
    struct MontageConfig {
        bool enabled = false;                       // master switch
        // Legacy flag, still parsed from .remap files for compatibility
        // but IGNORED: with MONTAGE mode on and a single output device,
        // style channels play 1:1 on channels 1-16 (see route_channel()).
        bool channel_remap = false;
        bool redirect_chorus_to_variation = false;   // CC#93 → CC#94
    };

    [[nodiscard]] const MontageConfig& montage_config() const noexcept { return montage_; }
    void set_montage_config(MontageConfig c) noexcept { montage_ = c; }
    [[nodiscard]] bool montage_enabled() const noexcept override { return montage_.enabled; }
    void set_montage_enabled(bool on) noexcept { montage_.enabled = on; }

    // Output-channel routing for style playback:
    // follows the CASM destination channel (inherited from RemapSource).
    // NOTE: the previous MONTAGE `src & 7` fold was removed — the user
    // is rewriting the MONTAGE routing from scratch.

    // Should CC#93 be redirected to CC#94 for this destination synth?
    [[nodiscard]] bool redirect_chorus_to_variation() const noexcept override {
        return montage_.enabled && montage_.redirect_chorus_to_variation;
    }

    // ----- Profile metadata -----
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& description() const noexcept { return description_; }
    [[nodiscard]] const std::filesystem::path& source_path() const noexcept { return source_path_; }
    void set_name(std::string n) { name_ = std::move(n); }
    void set_description(std::string d) { description_ = std::move(d); }

    // ----- Diagnostics -----
    struct FullEntry {
        u8          src_msb, src_lsb, src_pc;
        u8          dst_msb, dst_lsb, dst_pc;
        std::string desc;
    };

    void dump() const;
    [[nodiscard]] std::vector<FullEntry> entries_sorted() const;

private:
    [[nodiscard]] static constexpr std::uint32_t
        make_key(u8 msb, u8 lsb, u8 pc) noexcept {
        return (static_cast<std::uint32_t>(msb) << 16) |
               (static_cast<std::uint32_t>(lsb) <<  8) |
                static_cast<std::uint32_t>(pc);
    }

    void load_default_fallbacks();

    std::unordered_map<std::uint32_t, RemapEntry> entries_;
    std::unordered_map<std::uint8_t, FallbackRule> fallbacks_;

    MontageConfig                montage_{};
    std::string                  name_;
    std::string                  description_;
    std::filesystem::path        source_path_;
};

} // namespace prs