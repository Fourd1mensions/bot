-- Migration 012: Additional stopwords based on real usage analysis

INSERT INTO stopwords (word, language) VALUES
    ('тут', 'ru'),
    ('такой', 'ru'),
    ('такое', 'ru'),
    ('такая', 'ru'),
    ('такие', 'ru')
ON CONFLICT DO NOTHING;
