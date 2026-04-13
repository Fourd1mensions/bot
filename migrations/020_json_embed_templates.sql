-- JSON-based embed templates for full embed configuration
CREATE TABLE IF NOT EXISTS embed_json_templates (
    template_key TEXT PRIMARY KEY,        -- "rs:compact", "profile", "osc", etc.
    json_config  TEXT NOT NULL,           -- Full JSON configuration
    created_at   TIMESTAMPTZ DEFAULT NOW(),
    updated_at   TIMESTAMPTZ DEFAULT NOW()
);

CREATE OR REPLACE FUNCTION update_json_template_timestamp()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_json_template_updated
    BEFORE UPDATE ON embed_json_templates
    FOR EACH ROW
    EXECUTE FUNCTION update_json_template_timestamp();

-- Index for faster lookups
CREATE INDEX IF NOT EXISTS idx_json_templates_key ON embed_json_templates(template_key);
