#pragma once

#include <shared_mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>
#include <dpp/snowflake.h>

// Forward declaration
namespace dpp { class embed; }

namespace services {

/// Generic template storage: field_name → template_text
using TemplateFields = std::unordered_map<std::string, std::string>;

/// Placeholder description for API/UI
struct PlaceholderInfo {
    std::string name;
    std::string description;
};

/// Describes a command's template configuration
struct CommandTemplateConfig {
    std::string command_id;       // "rs", "compare", "map", "leaderboard", "sim"
    std::string label;            // "Recent Score (!rs)"
    bool has_presets;             // true = compact/classic/extended, false = single "default"
    std::vector<std::string> field_names;  // ordered field list for UI
    std::vector<PlaceholderInfo> placeholders;
    // defaults: preset_name → {field→template}
    // For has_presets=false, use key "default"
    std::unordered_map<std::string, TemplateFields> defaults;
};

/// Validation issue from template parsing
struct ValidationIssue {
    enum class Level { Error, Warning };
    Level level;
    std::string field;    // which template field ("description", "title", etc.)
    std::string message;
    size_t position;      // byte offset in template text
};

/// User template validation result (comprehensive)
struct UserTemplateValidationResult {
    bool valid = true;
    std::vector<std::string> errors;    // Fatal issues (prevents save)
    std::vector<std::string> warnings;  // Non-fatal issues

    void add_error(const std::string& msg) {
        valid = false;
        errors.push_back(msg);
    }
    void add_warning(const std::string& msg) {
        warnings.push_back(msg);
    }
};

/// Legacy struct for !rs backward compatibility
struct EmbedTemplate {
    std::string content;
    std::string title;
    std::string description;
    std::string beatmap_info;
    std::string footer;
    std::string footer_icon;
    std::string color;  // Color template (hex, named color, or placeholder like {sr_color})
};

// ---------------------------------------------------------------------------
// Full Embed Template System (JSON-based)
// ---------------------------------------------------------------------------

/// Single embed field template
struct EmbedFieldTemplate {
    std::string name;           // Field name template (supports placeholders)
    std::string value;          // Field value template (supports placeholders)
    bool is_inline = false;     // Display inline with other fields
    std::string loop_array;     // If set, repeat this field for each item in array
    std::string condition;      // Optional visibility condition (e.g. "has_fc")
};

/// Complete embed template with all properties
struct FullEmbedTemplate {
    // Message content (outside embed)
    std::string content;

    // Embed color - hex string, named color, or placeholder
    std::string color;

    // Author section
    std::string author_name;
    std::string author_url;
    std::string author_icon;

    // Main content
    std::string title;
    std::string title_url;
    std::string description;

    // Media URLs
    std::string thumbnail;
    std::string image;

    // Dynamic fields
    std::vector<EmbedFieldTemplate> fields;

    // Footer section
    std::string footer_text;
    std::string footer_icon;

    // Timestamp
    bool show_timestamp = false;
};

/// Color palette for named colors
extern const std::unordered_map<std::string, uint32_t> COLOR_PALETTE;

class EmbedTemplateService {
public:
    EmbedTemplateService() = default;

    void load_from_db();

    /// Generic field access — key is "rs:compact", "leaderboard", etc.
    TemplateFields get_fields(const std::string& key) const;
    void set_fields(const std::string& key, const TemplateFields& fields);
    void reset_to_default(const std::string& key);

    /// Get all cached templates
    std::unordered_map<std::string, TemplateFields> get_all() const;

    /// Legacy wrappers for !rs (auto-prepends "rs:" prefix)
    EmbedTemplate get_template(const std::string& preset_name) const;
    void set_template(const std::string& preset_name, const EmbedTemplate& tmpl);
    void reset_to_default_legacy(const std::string& preset_name);

    /// Command registry
    static std::vector<CommandTemplateConfig> get_all_commands();
    static TemplateFields get_default_fields(const std::string& key);

    /// Validation
    static std::vector<ValidationIssue> validate_template(
        const std::string& field_name,
        const std::string& tmpl,
        const std::vector<PlaceholderInfo>& known_placeholders);

    static std::vector<ValidationIssue> validate_fields(
        const std::string& command_id,
        const std::string& preset,
        const TemplateFields& fields);

    // ---------------------------------------------------------------------------
    // User Custom Template Validation
    // ---------------------------------------------------------------------------

    /// Validate a user-submitted custom template JSON
    /// Returns errors for dangerous content, unknown placeholders, length violations
    static UserTemplateValidationResult validate_user_template(
        const std::string& command_id,
        const std::string& json_config);

    /// Check text for dangerous patterns (@everyone, @here, mentions, discord invites)
    static bool contains_dangerous_content(const std::string& text, std::vector<std::string>& found);

    /// Check if URL is from a whitelisted domain (ppy.sh, osu.ppy.sh, etc.)
    static bool is_url_whitelisted(const std::string& url);

    /// Extract all URLs from text
    static std::vector<std::string> extract_urls(const std::string& text);

    /// Get list of allowed placeholder names for a command
    static std::vector<std::string> get_allowed_placeholders(const std::string& command_id);

    // ---------------------------------------------------------------------------
    // User Custom Template Lookup
    // ---------------------------------------------------------------------------

    /// Get template fields for a user, checking custom templates first
    /// For Custom preset: looks up user_custom_templates table
    /// Falls back to classic preset if no custom template exists
    TemplateFields get_user_fields(
        dpp::snowflake discord_id,
        const std::string& command_id,
        const std::string& preset_name) const;

    /// Legacy wrapper for !rs that supports user custom templates
    EmbedTemplate get_user_template(
        dpp::snowflake discord_id,
        const std::string& preset_name) const;

    // ---------------------------------------------------------------------------
    // Full Embed Template Methods (JSON-based)
    // ---------------------------------------------------------------------------

    /// Get full embed template for a command/preset
    FullEmbedTemplate get_full_template(const std::string& key) const;

    /// Get full embed template with user custom template support
    FullEmbedTemplate get_user_full_template(
        dpp::snowflake discord_id,
        const std::string& command_id,
        const std::string& preset_name) const;

    /// Set full embed template (stores as JSON)
    void set_full_template(const std::string& key, const FullEmbedTemplate& tmpl);

    /// Check if a key has a JSON template (vs legacy fields)
    bool has_json_template(const std::string& key) const;

    /// Get raw JSON string for export
    std::optional<std::string> get_json_template(const std::string& key) const;

    /// Set template from raw JSON string (for import)
    void set_json_template(const std::string& key, const std::string& json_str);

private:
    void seed_all_defaults();
    void seed_json_defaults();

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, TemplateFields> cache_;           // Legacy field-based
    std::unordered_map<std::string, std::string> json_cache_;         // JSON templates
};

/// Type alias for array data (used in loops)
using TemplateArrays = std::unordered_map<std::string, std::vector<std::unordered_map<std::string, std::string>>>;

/// Render a template string with placeholder substitution
/// Supports:
///   - {key} - simple replacement
///   - {?key}...{/key} - conditional (show if key is non-empty)
///   - {!key}...{/key} - negated conditional
///   - {?key=val}...{:key}...{/key} - conditional with comparison and else
///   - {#array}...{.field}...{/array} - loop over array items
///   - {#index}, {#position} - loop index (0-based) and position (1-based)
std::string render_template(const std::string& tmpl,
                            const std::unordered_map<std::string, std::string>& values,
                            const TemplateArrays& arrays = {});

/// Parse JSON string into FullEmbedTemplate
FullEmbedTemplate parse_json_template(const std::string& json_str);

/// Serialize FullEmbedTemplate to JSON string
std::string serialize_template(const FullEmbedTemplate& tmpl);

/// Resolve a color string to uint32_t
/// Supports: hex (#ff66aa, 0xff66aa), named colors (osu_pink, star_rating), placeholders
uint32_t resolve_color(const std::string& color_str,
                       const std::unordered_map<std::string, std::string>& values);

/// Render a full embed from template, values, and optional arrays
dpp::embed render_full_embed(
    const FullEmbedTemplate& tmpl,
    const std::unordered_map<std::string, std::string>& values,
    const TemplateArrays& arrays = {});

/// Get default FullEmbedTemplate for a command
FullEmbedTemplate get_default_full_template(const std::string& command_id,
                                            const std::string& preset = "default");

} // namespace services
