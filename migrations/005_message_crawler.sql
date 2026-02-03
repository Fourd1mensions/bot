-- Migration 005: Message history crawler and word statistics
-- Tables for storing Discord messages and word frequency analysis

-- Table to store Discord messages
CREATE TABLE IF NOT EXISTS discord_messages (
    message_id BIGINT PRIMARY KEY,
    channel_id BIGINT NOT NULL,
    author_id BIGINT NOT NULL,
    content TEXT NOT NULL,
    created_at TIMESTAMP WITH TIME ZONE NOT NULL,
    stored_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    is_bot BOOLEAN DEFAULT FALSE
);

CREATE INDEX IF NOT EXISTS idx_discord_messages_channel ON discord_messages(channel_id);
CREATE INDEX IF NOT EXISTS idx_discord_messages_author ON discord_messages(author_id);
CREATE INDEX IF NOT EXISTS idx_discord_messages_created ON discord_messages(created_at);
CREATE INDEX IF NOT EXISTS idx_discord_messages_channel_created ON discord_messages(channel_id, created_at DESC);

-- Table to track crawl progress per channel
CREATE TABLE IF NOT EXISTS crawl_progress (
    channel_id BIGINT PRIMARY KEY,
    guild_id BIGINT NOT NULL,
    oldest_message_id BIGINT,
    newest_message_id BIGINT,
    total_messages_crawled BIGINT DEFAULT 0,
    last_crawl_at TIMESTAMP WITH TIME ZONE,
    initial_crawl_complete BOOLEAN DEFAULT FALSE,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_crawl_progress_guild ON crawl_progress(guild_id);

-- Table to store word statistics (materialized for performance)
CREATE TABLE IF NOT EXISTS word_stats (
    id SERIAL PRIMARY KEY,
    word TEXT NOT NULL,
    count BIGINT NOT NULL DEFAULT 0,
    language VARCHAR(10) NOT NULL DEFAULT 'unknown',
    last_updated TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(word, language)
);

CREATE INDEX IF NOT EXISTS idx_word_stats_count ON word_stats(count DESC);
CREATE INDEX IF NOT EXISTS idx_word_stats_language ON word_stats(language);
CREATE INDEX IF NOT EXISTS idx_word_stats_language_count ON word_stats(language, count DESC);

-- Table for stopwords (configurable filter)
CREATE TABLE IF NOT EXISTS stopwords (
    word TEXT PRIMARY KEY,
    language VARCHAR(10) NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_stopwords_language ON stopwords(language);

-- Insert common English stopwords (min 3 chars as per filter)
INSERT INTO stopwords (word, language) VALUES
    ('the', 'en'), ('and', 'en'), ('for', 'en'), ('are', 'en'), ('but', 'en'),
    ('not', 'en'), ('you', 'en'), ('all', 'en'), ('can', 'en'), ('had', 'en'),
    ('her', 'en'), ('was', 'en'), ('one', 'en'), ('our', 'en'), ('out', 'en'),
    ('has', 'en'), ('have', 'en'), ('been', 'en'), ('from', 'en'), ('with', 'en'),
    ('they', 'en'), ('this', 'en'), ('that', 'en'), ('what', 'en'), ('when', 'en'),
    ('where', 'en'), ('which', 'en'), ('who', 'en'), ('will', 'en'), ('more', 'en'),
    ('there', 'en'), ('their', 'en'), ('about', 'en'), ('would', 'en'), ('these', 'en'),
    ('into', 'en'), ('some', 'en'), ('than', 'en'), ('its', 'en'), ('also', 'en'),
    ('just', 'en'), ('like', 'en'), ('your', 'en'), ('them', 'en'), ('then', 'en'),
    ('could', 'en'), ('make', 'en'), ('been', 'en'), ('other', 'en'), ('only', 'en'),
    ('come', 'en'), ('over', 'en'), ('such', 'en'), ('take', 'en'), ('because', 'en'),
    ('does', 'en'), ('even', 'en'), ('good', 'en'), ('most', 'en'), ('those', 'en'),
    ('need', 'en'), ('back', 'en'), ('much', 'en'), ('here', 'en'), ('very', 'en'),
    ('after', 'en'), ('right', 'en'), ('think', 'en'), ('well', 'en'), ('being', 'en'),
    ('really', 'en'), ('going', 'en'), ('yeah', 'en'), ('okay', 'en'), ('want', 'en'),
    ('know', 'en'), ('dont', 'en'), ('didnt', 'en'), ('cant', 'en'), ('wont', 'en'),
    ('isnt', 'en'), ('thats', 'en'), ('youre', 'en'), ('were', 'en'), ('theyre', 'en'),
    ('http', 'en'), ('https', 'en'), ('www', 'en'), ('com', 'en')
ON CONFLICT DO NOTHING;

-- Insert common Russian stopwords
INSERT INTO stopwords (word, language) VALUES
    ('что', 'ru'), ('как', 'ru'), ('все', 'ru'), ('она', 'ru'), ('так', 'ru'),
    ('его', 'ru'), ('это', 'ru'), ('только', 'ru'), ('ещё', 'ru'), ('еще', 'ru'),
    ('было', 'ru'), ('вот', 'ru'), ('меня', 'ru'), ('нет', 'ru'), ('ему', 'ru'),
    ('теперь', 'ru'), ('когда', 'ru'), ('уже', 'ru'), ('для', 'ru'), ('этот', 'ru'),
    ('эта', 'ru'), ('эти', 'ru'), ('там', 'ru'), ('чем', 'ru'), ('тут', 'ru'),
    ('они', 'ru'), ('тебя', 'ru'), ('где', 'ru'), ('есть', 'ru'), ('надо', 'ru'),
    ('сейчас', 'ru'), ('очень', 'ru'), ('если', 'ru'), ('тоже', 'ru'), ('себя', 'ru'),
    ('может', 'ru'), ('потом', 'ru'), ('просто', 'ru'), ('чтобы', 'ru'), ('тебе', 'ru'),
    ('можно', 'ru'), ('даже', 'ru'), ('нам', 'ru'), ('него', 'ru'), ('ничего', 'ru'),
    ('мне', 'ru'), ('вас', 'ru'), ('здесь', 'ru'), ('мой', 'ru'), ('будет', 'ru'),
    ('типа', 'ru'), ('короче', 'ru'), ('ладно', 'ru'), ('нормально', 'ru'),
    ('который', 'ru'), ('которая', 'ru'), ('которые', 'ru'), ('которого', 'ru'),
    ('был', 'ru'), ('была', 'ru'), ('были', 'ru'), ('при', 'ru'), ('под', 'ru'),
    ('над', 'ru'), ('без', 'ru'), ('про', 'ru'), ('через', 'ru'), ('после', 'ru'),
    ('между', 'ru'), ('перед', 'ru'), ('ведь', 'ru'), ('или', 'ru'), ('чтоб', 'ru'),
    ('пока', 'ru'), ('хотя', 'ru'), ('либо', 'ru'), ('нибудь', 'ru'), ('кто', 'ru'),
    ('ага', 'ru'), ('ого', 'ru'), ('блин', 'ru'), ('ну', 'ru')
ON CONFLICT DO NOTHING;

-- View for crawl status summary
CREATE OR REPLACE VIEW crawl_status_view AS
SELECT
    cp.guild_id,
    COUNT(*) as total_channels,
    SUM(CASE WHEN cp.initial_crawl_complete THEN 1 ELSE 0 END) as completed_channels,
    SUM(cp.total_messages_crawled) as total_messages,
    MIN(cp.last_crawl_at) as oldest_crawl,
    MAX(cp.last_crawl_at) as newest_crawl
FROM crawl_progress cp
GROUP BY cp.guild_id;

-- Trigger for updated_at on crawl_progress
DROP TRIGGER IF EXISTS update_crawl_progress_updated_at ON crawl_progress;
CREATE TRIGGER update_crawl_progress_updated_at
    BEFORE UPDATE ON crawl_progress
    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();

COMMENT ON TABLE discord_messages IS 'Stores crawled Discord messages for analysis';
COMMENT ON TABLE crawl_progress IS 'Tracks message crawl progress per channel';
COMMENT ON TABLE word_stats IS 'Aggregated word frequency statistics';
COMMENT ON TABLE stopwords IS 'Common words to exclude from statistics';
