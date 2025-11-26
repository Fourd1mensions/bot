-- Patchouli Bot Database Schema
-- Initial migration

-- Enable extensions
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- Users table: Discord ID to osu! user ID mapping
CREATE TABLE IF NOT EXISTS users (
    discord_id BIGINT PRIMARY KEY,
    osu_user_id BIGINT NOT NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_users_osu_id ON users(osu_user_id);

-- Chat map table: Channel context for beatmap links
CREATE TABLE IF NOT EXISTS chat_map (
    channel_id BIGINT PRIMARY KEY,
    message_id BIGINT NOT NULL,
    beatmap_id TEXT NOT NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Beatmap files table: Track downloaded and compressed beatmap assets
CREATE TABLE IF NOT EXISTS beatmap_files (
    beatmapset_id BIGINT PRIMARY KEY,
    audio_path TEXT,
    background_path TEXT,
    audio_compressed BOOLEAN DEFAULT false,
    bg_compressed BOOLEAN DEFAULT false,
    audio_last_accessed TIMESTAMP WITH TIME ZONE,
    bg_last_accessed TIMESTAMP WITH TIME ZONE,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_beatmap_audio_access ON beatmap_files(audio_last_accessed) WHERE audio_path IS NOT NULL;
CREATE INDEX idx_beatmap_bg_access ON beatmap_files(bg_last_accessed) WHERE background_path IS NOT NULL;
CREATE INDEX idx_beatmap_compressed ON beatmap_files(audio_compressed, bg_compressed);

-- User cache table: Cache osu! usernames (1 hour TTL)
CREATE TABLE IF NOT EXISTS user_cache (
    user_id BIGINT PRIMARY KEY,
    username TEXT NOT NULL,
    expires_at TIMESTAMP WITH TIME ZONE NOT NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_user_cache_expires ON user_cache(expires_at);

-- Function to auto-update updated_at timestamp
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = CURRENT_TIMESTAMP;
    RETURN NEW;
END;
$$ language 'plpgsql';

-- Triggers for auto-updating updated_at
CREATE TRIGGER update_users_updated_at BEFORE UPDATE ON users
    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_chat_map_updated_at BEFORE UPDATE ON chat_map
    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();

CREATE TRIGGER update_beatmap_files_updated_at BEFORE UPDATE ON beatmap_files
    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();

-- Function to clean expired cache entries
CREATE OR REPLACE FUNCTION cleanup_expired_cache()
RETURNS void AS $$
BEGIN
    DELETE FROM user_cache WHERE expires_at < CURRENT_TIMESTAMP;
END;
$$ LANGUAGE plpgsql;

-- Grant permissions (adjust username as needed)
-- GRANT ALL PRIVILEGES ON ALL TABLES IN SCHEMA public TO patchouli;
-- GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA public TO patchouli;
