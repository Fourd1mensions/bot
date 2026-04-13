-- Discord users cache for storing usernames
CREATE TABLE IF NOT EXISTS discord_users (
    user_id BIGINT PRIMARY KEY,
    username TEXT NOT NULL,
    global_name TEXT,  -- Display name (may be null)
    is_bot BOOLEAN DEFAULT FALSE,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Index for searching by username
CREATE INDEX IF NOT EXISTS idx_discord_users_username ON discord_users(username);
