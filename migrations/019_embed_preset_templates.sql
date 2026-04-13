CREATE TABLE IF NOT EXISTS embed_preset_templates (
    preset_name TEXT NOT NULL,
    field_name  TEXT NOT NULL,
    template    TEXT NOT NULL DEFAULT '',
    updated_at  TIMESTAMPTZ DEFAULT NOW(),
    PRIMARY KEY (preset_name, field_name)
);

CREATE OR REPLACE FUNCTION update_preset_template_timestamp()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_preset_template_updated
    BEFORE UPDATE ON embed_preset_templates
    FOR EACH ROW
    EXECUTE FUNCTION update_preset_template_timestamp();
