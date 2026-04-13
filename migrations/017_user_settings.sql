CREATE TABLE IF NOT EXISTS user_settings (
    discord_id BIGINT PRIMARY KEY,
    embed_preset TEXT NOT NULL DEFAULT 'classic',
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE OR REPLACE FUNCTION update_user_settings_timestamp()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_user_settings_updated
    BEFORE UPDATE ON user_settings
    FOR EACH ROW
    EXECUTE FUNCTION update_user_settings_timestamp();
