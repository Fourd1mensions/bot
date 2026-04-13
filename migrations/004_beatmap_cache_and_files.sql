-- Migration 004: Beatmap ID cache and improved file tracking
-- Created: 2025-01-26

-- Table to cache beatmap_id to beatmapset_id mappings
CREATE TABLE IF NOT EXISTS beatmap_id_cache (
    beatmap_id INTEGER PRIMARY KEY,
    beatmapset_id INTEGER NOT NULL,
    mode TEXT NOT NULL DEFAULT 'osu',
    cached_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_accessed TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_beatmap_id_cache_beatmapset ON beatmap_id_cache(beatmapset_id);
CREATE INDEX idx_beatmap_id_cache_accessed ON beatmap_id_cache(last_accessed);

-- Table to track individual .osu files
CREATE TABLE IF NOT EXISTS osu_files (
    id SERIAL PRIMARY KEY,
    beatmap_id INTEGER UNIQUE NOT NULL,
    beatmapset_id INTEGER NOT NULL,
    file_path TEXT NOT NULL,
    file_size BIGINT,
    source TEXT DEFAULT 'osu.ppy.sh',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_accessed TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    access_count INTEGER DEFAULT 0
);

CREATE INDEX idx_osu_files_beatmap ON osu_files(beatmap_id);
CREATE INDEX idx_osu_files_beatmapset ON osu_files(beatmapset_id);
CREATE INDEX idx_osu_files_accessed ON osu_files(last_accessed);

-- Update beatmap_files table to track .osz files better
ALTER TABLE beatmap_files
    ADD COLUMN IF NOT EXISTS file_size BIGINT,
    ADD COLUMN IF NOT EXISTS last_accessed TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    ADD COLUMN IF NOT EXISTS access_count INTEGER DEFAULT 0;

-- View for complete file inventory
CREATE OR REPLACE VIEW file_inventory AS
SELECT
    'osz' as file_type,
    bf.beatmapset_id,
    NULL::INTEGER as beatmap_id,
    bf.osz_path as file_path,
    bf.file_size,
    bf.mirror_hostname as source,
    bf.created_at,
    bf.last_accessed,
    bf.access_count
FROM beatmap_files bf
UNION ALL
SELECT
    'osu' as file_type,
    of.beatmapset_id,
    of.beatmap_id,
    of.file_path,
    of.file_size,
    of.source,
    of.created_at,
    of.last_accessed,
    of.access_count
FROM osu_files of
ORDER BY created_at DESC;

-- View for storage statistics
CREATE OR REPLACE VIEW storage_stats AS
SELECT
    file_type,
    COUNT(*) as file_count,
    COALESCE(SUM(file_size), 0) as total_size_bytes,
    ROUND(COALESCE(SUM(file_size), 0) / 1024.0 / 1024.0, 2) as total_size_mb,
    ROUND(COALESCE(SUM(file_size), 0) / 1024.0 / 1024.0 / 1024.0, 2) as total_size_gb,
    MIN(created_at) as oldest_file,
    MAX(created_at) as newest_file,
    SUM(access_count) as total_accesses
FROM file_inventory
GROUP BY file_type;

COMMENT ON TABLE beatmap_id_cache IS 'Cache for fast beatmap_id to beatmapset_id lookups';
COMMENT ON TABLE osu_files IS 'Tracking for individual .osu difficulty files';
COMMENT ON VIEW file_inventory IS 'Complete inventory of all beatmap files (.osz and .osu)';
COMMENT ON VIEW storage_stats IS 'Statistics about file storage usage';
