CREATE TABLE IF NOT EXISTS replay_history (
    id SERIAL PRIMARY KEY,
    discord_id BIGINT NOT NULL,
    replay_hash TEXT NOT NULL,
    beatmap_hash TEXT NOT NULL,
    player_name TEXT NOT NULL,
    map_artist TEXT DEFAULT '',
    map_title TEXT DEFAULT '',
    map_version TEXT DEFAULT '',
    max_combo INT DEFAULT 0,
    accuracy FLOAT DEFAULT 0,
    count_300 INT DEFAULT 0,
    count_100 INT DEFAULT 0,
    count_50 INT DEFAULT 0,
    count_miss INT DEFAULT 0,
    mods INT DEFAULT 0,
    unstable_rate FLOAT DEFAULT 0,
    uploaded_at TIMESTAMPTZ DEFAULT NOW(),
    UNIQUE(discord_id, replay_hash)
);

CREATE INDEX IF NOT EXISTS idx_replay_history_user ON replay_history(discord_id, uploaded_at DESC);
