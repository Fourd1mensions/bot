-- Phrase statistics with PMI (Pointwise Mutual Information)

CREATE TABLE IF NOT EXISTS phrase_stats (
    id SERIAL PRIMARY KEY,
    phrase TEXT NOT NULL,
    words TEXT[] NOT NULL,
    word_count SMALLINT NOT NULL,
    count BIGINT NOT NULL DEFAULT 0,
    pmi_score DOUBLE PRECISION,
    language VARCHAR(10) NOT NULL DEFAULT 'unknown',
    last_updated TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(phrase, language)
);

CREATE INDEX IF NOT EXISTS idx_phrase_stats_count ON phrase_stats(count DESC);
CREATE INDEX IF NOT EXISTS idx_phrase_stats_pmi ON phrase_stats(pmi_score DESC NULLS LAST);
CREATE INDEX IF NOT EXISTS idx_phrase_stats_word_count ON phrase_stats(word_count);
CREATE INDEX IF NOT EXISTS idx_phrase_stats_lang_count ON phrase_stats(language, count DESC);
CREATE INDEX IF NOT EXISTS idx_phrase_stats_lang_pmi ON phrase_stats(language, pmi_score DESC NULLS LAST);
