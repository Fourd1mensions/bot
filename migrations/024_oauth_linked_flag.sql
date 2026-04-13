-- Add is_oauth_linked flag to users table
-- Tracks whether user linked via osu! OAuth (true) or manual set (false)

ALTER TABLE users ADD COLUMN IF NOT EXISTS is_oauth_linked BOOLEAN DEFAULT false;

-- Create index for quick lookups
CREATE INDEX IF NOT EXISTS idx_users_oauth_linked ON users(is_oauth_linked);
