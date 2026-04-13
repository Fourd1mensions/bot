-- Add avatar hash to discord_users for CDN URL construction
-- URL format: https://cdn.discordapp.com/avatars/{user_id}/{avatar_hash}.png
ALTER TABLE discord_users ADD COLUMN IF NOT EXISTS avatar_hash TEXT;
