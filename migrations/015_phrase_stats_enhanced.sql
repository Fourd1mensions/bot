-- Migration 015: Enhanced phrase statistics
-- Adds NPMI, LLR, temporal analysis, and lemmatization support

-- NPMI (Normalized PMI) - bounded to [-1, 1] for easier comparison
ALTER TABLE phrase_stats ADD COLUMN IF NOT EXISTS npmi_score DOUBLE PRECISION;
CREATE INDEX IF NOT EXISTS idx_phrase_stats_npmi ON phrase_stats(npmi_score DESC NULLS LAST);
CREATE INDEX IF NOT EXISTS idx_phrase_stats_lang_npmi ON phrase_stats(language, npmi_score DESC NULLS LAST);

-- LLR (Log-Likelihood Ratio) - more statistically robust than PMI
ALTER TABLE phrase_stats ADD COLUMN IF NOT EXISTS llr_score DOUBLE PRECISION;
CREATE INDEX IF NOT EXISTS idx_phrase_stats_llr ON phrase_stats(llr_score DESC NULLS LAST);

-- Temporal analysis
ALTER TABLE phrase_stats ADD COLUMN IF NOT EXISTS first_seen TIMESTAMP WITH TIME ZONE;
ALTER TABLE phrase_stats ADD COLUMN IF NOT EXISTS trend_score DOUBLE PRECISION DEFAULT 0;
CREATE INDEX IF NOT EXISTS idx_phrase_stats_first_seen ON phrase_stats(first_seen DESC NULLS LAST);
CREATE INDEX IF NOT EXISTS idx_phrase_stats_trend ON phrase_stats(trend_score DESC NULLS LAST);

-- Lemmatization support
ALTER TABLE phrase_stats ADD COLUMN IF NOT EXISTS lemmatized_phrase TEXT;
CREATE INDEX IF NOT EXISTS idx_phrase_stats_lemma ON phrase_stats(lemmatized_phrase);

-- Phrase history table for trend calculation (daily snapshots)
CREATE TABLE IF NOT EXISTS phrase_history (
    id SERIAL PRIMARY KEY,
    phrase TEXT NOT NULL,
    language VARCHAR(10) NOT NULL,
    count BIGINT NOT NULL,
    recorded_date DATE NOT NULL DEFAULT CURRENT_DATE,
    recorded_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(phrase, language, recorded_date)
);

CREATE INDEX IF NOT EXISTS idx_phrase_history_phrase ON phrase_history(phrase, language);
CREATE INDEX IF NOT EXISTS idx_phrase_history_date ON phrase_history(recorded_date);

-- Word blacklist table (words to exclude from phrases)
CREATE TABLE IF NOT EXISTS word_blacklist (
    id SERIAL PRIMARY KEY,
    word TEXT NOT NULL UNIQUE,
    language VARCHAR(10) NOT NULL DEFAULT 'all',
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);
