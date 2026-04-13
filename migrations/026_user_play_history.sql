-- Per-user play history: tracks each user has played across all servers
CREATE TABLE IF NOT EXISTS user_play_history (
    id SERIAL PRIMARY KEY,
    discord_id BIGINT NOT NULL,
    guild_id BIGINT NOT NULL,
    url TEXT NOT NULL,
    title TEXT NOT NULL,
    duration INT DEFAULT 0,
    thumbnail TEXT DEFAULT '',
    played_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_user_play_history_user_time
    ON user_play_history(discord_id, played_at DESC);

CREATE INDEX IF NOT EXISTS idx_user_play_history_user_guild
    ON user_play_history(discord_id, guild_id, played_at DESC);
