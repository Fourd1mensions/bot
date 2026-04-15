-- Audit log for admin template changes
-- Tracks who changed what template, when, and what the change was

CREATE TABLE IF NOT EXISTS template_audit_log (
    id SERIAL PRIMARY KEY,
    discord_id BIGINT NOT NULL,           -- Admin who made the change
    discord_username TEXT,                 -- Username at time of change (for display)
    action TEXT NOT NULL,                  -- 'update', 'reset'
    command_id TEXT NOT NULL,              -- "rs", "compare", "map", etc.
    preset TEXT,                           -- "compact", "classic", "extended", or NULL for single-preset commands
    old_fields JSONB,                      -- Previous template fields (NULL for reset)
    new_fields JSONB,                      -- New template fields (NULL for reset to default)
    created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Index for listing by admin
CREATE INDEX IF NOT EXISTS idx_template_audit_log_admin
ON template_audit_log(discord_id);

-- Index for listing by command
CREATE INDEX IF NOT EXISTS idx_template_audit_log_command
ON template_audit_log(command_id);

-- Index for time-based queries
CREATE INDEX IF NOT EXISTS idx_template_audit_log_time
ON template_audit_log(created_at DESC);
