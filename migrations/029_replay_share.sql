ALTER TABLE replay_history ADD COLUMN IF NOT EXISTS share_id TEXT UNIQUE DEFAULT NULL;
CREATE INDEX IF NOT EXISTS idx_replay_share ON replay_history(share_id) WHERE share_id IS NOT NULL;
