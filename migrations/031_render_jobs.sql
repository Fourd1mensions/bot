CREATE TABLE IF NOT EXISTS render_jobs (
    id SERIAL PRIMARY KEY,
    replay_hash TEXT NOT NULL UNIQUE,
    beatmap_hash TEXT NOT NULL,
    user_id TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'queued',
    error TEXT,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    started_at TIMESTAMPTZ,
    completed_at TIMESTAMPTZ,
    retry_count INT DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_render_jobs_status ON render_jobs(status, created_at ASC);
CREATE INDEX IF NOT EXISTS idx_render_jobs_user ON render_jobs(user_id);
