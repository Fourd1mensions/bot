-- Track incremental statistics processing
CREATE TABLE IF NOT EXISTS stats_processing_state (
    key VARCHAR(50) PRIMARY KEY,
    last_message_id BIGINT NOT NULL DEFAULT 0,
    last_processed_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Initialize state
INSERT INTO stats_processing_state (key, last_message_id) VALUES
    ('word_stats', 0),
    ('phrase_stats', 0)
ON CONFLICT (key) DO NOTHING;

-- Add index for efficient incremental queries
CREATE INDEX IF NOT EXISTS idx_discord_messages_id ON discord_messages(message_id);
