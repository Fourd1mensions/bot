-- User track library: per-user saved tracks for quick access
CREATE TABLE IF NOT EXISTS user_track_library (
    id SERIAL PRIMARY KEY,
    discord_id BIGINT NOT NULL,
    url TEXT NOT NULL,
    title TEXT NOT NULL,
    duration INT DEFAULT 0,
    thumbnail TEXT DEFAULT '',
    added_at TIMESTAMPTZ DEFAULT NOW(),
    UNIQUE(discord_id, url)
);

CREATE INDEX IF NOT EXISTS idx_user_track_library_user ON user_track_library(discord_id);
