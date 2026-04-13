-- User custom templates for per-user embed customization
-- Each user can have their own template for each command

CREATE TABLE IF NOT EXISTS user_custom_templates (
    discord_id BIGINT NOT NULL,
    command_id TEXT NOT NULL,  -- "rs", "compare", "map", "lb", "top", "profile", "osc"
    json_config TEXT NOT NULL,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW(),
    PRIMARY KEY (discord_id, command_id)
);

-- Index for fast user lookup
CREATE INDEX IF NOT EXISTS idx_user_custom_templates_user
ON user_custom_templates(discord_id);

-- Function for automatic updated_at
CREATE OR REPLACE FUNCTION update_user_custom_templates_timestamp()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- Trigger for automatic updated_at
CREATE TRIGGER trg_user_custom_templates_updated
    BEFORE UPDATE ON user_custom_templates
    FOR EACH ROW EXECUTE FUNCTION update_user_custom_templates_timestamp();
