-- Migration: Add pending button removals table
-- This table tracks messages with interactive buttons that need to be cleaned up after TTL expires
-- Allows button removal to persist across bot restarts

CREATE TABLE IF NOT EXISTS pending_button_removals (
    channel_id BIGINT NOT NULL,
    message_id BIGINT NOT NULL,
    expires_at TIMESTAMP WITH TIME ZONE NOT NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (channel_id, message_id)
);

-- Index for efficient query of expired removals
CREATE INDEX IF NOT EXISTS idx_pending_button_removals_expires_at
    ON pending_button_removals(expires_at);

-- Index for efficient cleanup queries
CREATE INDEX IF NOT EXISTS idx_pending_button_removals_created_at
    ON pending_button_removals(created_at);
