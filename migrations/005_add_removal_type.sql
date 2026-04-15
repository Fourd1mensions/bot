-- Migration: Add type field to pending_button_removals
-- This allows distinguishing between different types of interactive messages (leaderboard, recent_scores, etc.)

-- Add type column with default 'leaderboard' for backward compatibility
ALTER TABLE pending_button_removals
ADD COLUMN IF NOT EXISTS removal_type VARCHAR(50) NOT NULL DEFAULT 'leaderboard';

-- Create index for efficient type-based queries
CREATE INDEX IF NOT EXISTS idx_pending_button_removals_type
    ON pending_button_removals(removal_type);
