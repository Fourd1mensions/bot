-- Guild info cache for displaying in UI
CREATE TABLE IF NOT EXISTS guild_info (
    guild_id BIGINT PRIMARY KEY,
    name TEXT NOT NULL,
    icon_hash TEXT,
    member_count INT DEFAULT 0,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);
