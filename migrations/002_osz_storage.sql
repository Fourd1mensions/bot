-- Patchouli Bot Database Schema
-- Migration 002: Switch to .osz file storage with temporary extracts

-- Drop old indexes
DROP INDEX IF EXISTS idx_beatmap_audio_access;
DROP INDEX IF EXISTS idx_beatmap_bg_access;
DROP INDEX IF EXISTS idx_beatmap_compressed;

-- Backup existing data (optional, for safety)
-- CREATE TABLE beatmap_files_backup AS SELECT * FROM beatmap_files;

-- Alter beatmap_files table to new schema
ALTER TABLE beatmap_files DROP COLUMN IF EXISTS audio_path;
ALTER TABLE beatmap_files DROP COLUMN IF EXISTS background_path;
ALTER TABLE beatmap_files DROP COLUMN IF EXISTS audio_compressed;
ALTER TABLE beatmap_files DROP COLUMN IF EXISTS bg_compressed;
ALTER TABLE beatmap_files DROP COLUMN IF EXISTS audio_last_accessed;
ALTER TABLE beatmap_files DROP COLUMN IF EXISTS bg_last_accessed;

-- Add new columns for .osz storage
ALTER TABLE beatmap_files ADD COLUMN IF NOT EXISTS osz_path TEXT;
ALTER TABLE beatmap_files ADD COLUMN IF NOT EXISTS mirror_hostname TEXT;
ALTER TABLE beatmap_files ADD COLUMN IF NOT EXISTS last_accessed TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP;

-- Create index for last_accessed
CREATE INDEX idx_beatmap_last_accessed ON beatmap_files(last_accessed) WHERE osz_path IS NOT NULL;

-- Create beatmap_extracts table for temporary extracted versions
CREATE TABLE IF NOT EXISTS beatmap_extracts (
    extract_id VARCHAR(32) PRIMARY KEY,
    beatmapset_id BIGINT NOT NULL,
    extract_path TEXT NOT NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    expires_at TIMESTAMP WITH TIME ZONE NOT NULL
);

-- Create indexes for beatmap_extracts
CREATE INDEX idx_extracts_beatmapset ON beatmap_extracts(beatmapset_id);
CREATE INDEX idx_extracts_expires ON beatmap_extracts(expires_at);

-- Function to cleanup expired extracts
CREATE OR REPLACE FUNCTION cleanup_expired_extracts()
RETURNS TABLE(extract_id VARCHAR(32), extract_path TEXT) AS $$
BEGIN
    RETURN QUERY
    DELETE FROM beatmap_extracts
    WHERE expires_at < CURRENT_TIMESTAMP
    RETURNING beatmap_extracts.extract_id, beatmap_extracts.extract_path;
END;
$$ LANGUAGE plpgsql;
