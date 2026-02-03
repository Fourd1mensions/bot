-- Migration 007: Update stopwords list
-- Clear old stopwords and insert curated list

-- Clear existing stopwords
TRUNCATE stopwords;

-- Russian stopwords (союзы, частицы, предлоги, местоимения, вопросительные, глаголы-связки, наречия)
INSERT INTO stopwords (word, language) VALUES
    -- Союзы/частицы/предлоги
    ('это', 'ru'), ('что', 'ru'), ('как', 'ru'), ('там', 'ru'), ('так', 'ru'),
    ('если', 'ru'), ('когда', 'ru'), ('вот', 'ru'), ('или', 'ru'), ('еще', 'ru'),
    ('ещё', 'ru'), ('только', 'ru'), ('тоже', 'ru'), ('для', 'ru'), ('где', 'ru'),
    ('чтобы', 'ru'), ('даже', 'ru'), ('про', 'ru'), ('без', 'ru'), ('под', 'ru'),
    ('через', 'ru'), ('после', 'ru'), ('чем', 'ru'), ('потому', 'ru'), ('хотя', 'ru'),
    ('пока', 'ru'), ('тогда', 'ru'), ('значит', 'ru'),
    -- Местоимения
    ('все', 'ru'), ('всё', 'ru'), ('меня', 'ru'), ('мне', 'ru'), ('тебя', 'ru'),
    ('тебе', 'ru'), ('она', 'ru'), ('его', 'ru'), ('него', 'ru'), ('ему', 'ru'),
    ('они', 'ru'), ('себе', 'ru'), ('нас', 'ru'), ('вас', 'ru'), ('всех', 'ru'),
    ('всем', 'ru'), ('кого', 'ru'), ('мой', 'ru'), ('этот', 'ru'), ('этого', 'ru'),
    ('эту', 'ru'), ('который', 'ru'), ('ничего', 'ru'), ('нибудь', 'ru'),
    -- Вопросительные
    ('кто', 'ru'), ('почему', 'ru'), ('какой', 'ru'), ('какая', 'ru'), ('какие', 'ru'),
    ('зачем', 'ru'), ('сколько', 'ru'), ('куда', 'ru'),
    -- Глаголы-связки
    ('есть', 'ru'), ('был', 'ru'), ('было', 'ru'), ('была', 'ru'), ('будет', 'ru'),
    ('буду', 'ru'), ('быть', 'ru'), ('может', 'ru'), ('можно', 'ru'),
    -- Наречия
    ('уже', 'ru'), ('очень', 'ru'), ('больше', 'ru'), ('теперь', 'ru'), ('много', 'ru'),
    ('всегда', 'ru'), ('сразу', 'ru')
ON CONFLICT DO NOTHING;

-- English stopwords (basic set)
INSERT INTO stopwords (word, language) VALUES
    ('the', 'en'), ('and', 'en'), ('for', 'en'), ('are', 'en'), ('but', 'en'),
    ('not', 'en'), ('you', 'en'), ('all', 'en'), ('can', 'en'), ('had', 'en'),
    ('was', 'en'), ('one', 'en'), ('our', 'en'), ('out', 'en'), ('has', 'en'),
    ('have', 'en'), ('been', 'en'), ('from', 'en'), ('with', 'en'), ('they', 'en'),
    ('this', 'en'), ('that', 'en'), ('what', 'en'), ('when', 'en'), ('where', 'en'),
    ('which', 'en'), ('who', 'en'), ('will', 'en'), ('more', 'en'), ('there', 'en'),
    ('their', 'en'), ('about', 'en'), ('would', 'en'), ('these', 'en'), ('into', 'en'),
    ('some', 'en'), ('than', 'en'), ('its', 'en'), ('also', 'en'), ('just', 'en'),
    ('your', 'en'), ('them', 'en'), ('then', 'en'), ('could', 'en'), ('other', 'en'),
    ('only', 'en'), ('over', 'en'), ('such', 'en'), ('because', 'en'), ('does', 'en'),
    ('even', 'en'), ('most', 'en'), ('those', 'en'), ('here', 'en'), ('very', 'en'),
    ('after', 'en'), ('being', 'en'), ('were', 'en')
ON CONFLICT DO NOTHING;
