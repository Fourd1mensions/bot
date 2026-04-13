-- Discord channels cache table
CREATE TABLE IF NOT EXISTS discord_channels (
    channel_id BIGINT PRIMARY KEY,
    guild_id BIGINT NOT NULL,
    channel_name TEXT NOT NULL DEFAULT '',
    channel_type SMALLINT NOT NULL DEFAULT 0,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_discord_channels_guild ON discord_channels(guild_id);

-- Trigger to update updated_at on changes
CREATE OR REPLACE TRIGGER update_discord_channels_updated_at
    BEFORE UPDATE ON discord_channels
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();
