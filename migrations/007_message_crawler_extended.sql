-- Migration: 006_message_crawler_extended
-- Add reply tracking, attachments, and edit timestamp to discord_messages

-- Reply tracking: which message this is a reply to
ALTER TABLE discord_messages
ADD COLUMN IF NOT EXISTS reply_to_message_id BIGINT;

-- Attachment tracking
ALTER TABLE discord_messages
ADD COLUMN IF NOT EXISTS has_attachments BOOLEAN DEFAULT FALSE;

ALTER TABLE discord_messages
ADD COLUMN IF NOT EXISTS attachment_urls TEXT[];

-- Edit timestamp (NULL if never edited)
ALTER TABLE discord_messages
ADD COLUMN IF NOT EXISTS edited_at TIMESTAMP WITH TIME ZONE;

-- Index for finding replies to a specific message
CREATE INDEX IF NOT EXISTS idx_discord_messages_reply_to
ON discord_messages(reply_to_message_id)
WHERE reply_to_message_id IS NOT NULL;

-- Index for finding messages with attachments
CREATE INDEX IF NOT EXISTS idx_discord_messages_has_attachments
ON discord_messages(has_attachments)
WHERE has_attachments = TRUE;
