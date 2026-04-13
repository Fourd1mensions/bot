-- Download log for persistent statistics
CREATE TABLE IF NOT EXISTS download_log (
    id SERIAL PRIMARY KEY,
    beatmapset_id BIGINT NOT NULL,
    downloaded_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Index for time-based queries
CREATE INDEX IF NOT EXISTS idx_download_log_time ON download_log(downloaded_at);

-- Cleanup function - removes entries older than 24 hours
-- Called periodically to keep table small
CREATE OR REPLACE FUNCTION cleanup_old_downloads() RETURNS void AS $$
BEGIN
    DELETE FROM download_log WHERE downloaded_at < NOW() - INTERVAL '24 hours';
END;
$$ LANGUAGE plpgsql;
