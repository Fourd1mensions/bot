-- Migrate leaderboard and compare templates to new loop-based format
-- Old: score_header, score_body (leaderboard) / header, score_line (compare)
-- New: description with {#scores}...{/scores} loop syntax

-- Helper function to convert old placeholders to loop syntax
-- e.g., {rank} -> {.rank}, {username} -> {.username}
-- include_max_combo: true for leaderboard (per-score), false for compare (global)
CREATE OR REPLACE FUNCTION convert_to_loop_placeholders(template TEXT, include_max_combo BOOLEAN DEFAULT FALSE)
RETURNS TEXT AS $$
DECLARE
    result TEXT := template;
    -- List of per-score placeholders that need to be converted
    placeholders TEXT[] := ARRAY[
        'rank', 'username', 'user_id', 'pp', 'mods', 'rank_emoji',
        'acc', 'combo', 'score', '300', '100', '50', 'miss', 'date',
        'index', 'rank_raw', 'mods_suffix', 'pp_int', 'score_raw',
        'passed', 'failed_line', 'date_unix', 'date_relative',
        'weight_pct', 'weight_pp'
    ];
    ph TEXT;
BEGIN
    FOREACH ph IN ARRAY placeholders LOOP
        -- Replace {placeholder} with {.placeholder}
        result := regexp_replace(result, '\{' || ph || '\}', '{.' || ph || '}', 'g');
    END LOOP;
    -- max_combo is per-score for leaderboard but global for compare
    IF include_max_combo THEN
        result := regexp_replace(result, '\{max_combo\}', '{.max_combo}', 'g');
    END IF;
    RETURN result;
END;
$$ LANGUAGE plpgsql;

-- Migrate leaderboard templates
DO $$
DECLARE
    header_val TEXT;
    body_val TEXT;
    new_desc TEXT;
BEGIN
    -- Get current values
    SELECT template INTO header_val FROM embed_preset_templates
        WHERE preset_name = 'leaderboard' AND field_name = 'score_header';
    SELECT template INTO body_val FROM embed_preset_templates
        WHERE preset_name = 'leaderboard' AND field_name = 'score_body';

    IF header_val IS NOT NULL AND body_val IS NOT NULL THEN
        -- Build new description with loop syntax
        -- Pass true for include_max_combo since it's per-score in leaderboard
        new_desc := '{#scores}**' || convert_to_loop_placeholders(header_val, true) || '**' || E'\n' ||
                    convert_to_loop_placeholders(body_val, true) || E'\n\n' || '{/scores}';

        -- Insert or update description field
        INSERT INTO embed_preset_templates (preset_name, field_name, template)
        VALUES ('leaderboard', 'description', new_desc)
        ON CONFLICT (preset_name, field_name) DO UPDATE SET template = new_desc;

        -- Delete old fields
        DELETE FROM embed_preset_templates
        WHERE preset_name = 'leaderboard' AND field_name IN ('score_header', 'score_body');

        RAISE NOTICE 'Migrated leaderboard template';
    END IF;
END $$;

-- Migrate compare templates (all presets: compact, classic, extended)
DO $$
DECLARE
    preset TEXT;
    header_val TEXT;
    score_line_val TEXT;
    new_desc TEXT;
BEGIN
    FOR preset IN SELECT DISTINCT preset_name FROM embed_preset_templates
                  WHERE preset_name LIKE 'compare:%' LOOP
        -- Get current values
        SELECT template INTO header_val FROM embed_preset_templates
            WHERE preset_name = preset AND field_name = 'header';
        SELECT template INTO score_line_val FROM embed_preset_templates
            WHERE preset_name = preset AND field_name = 'score_line';

        IF header_val IS NOT NULL AND score_line_val IS NOT NULL THEN
            -- Build new description with loop syntax
            -- Header stays as-is (uses global placeholders), score_line gets converted
            new_desc := header_val || E'\n\n' ||
                        '{#scores}' || convert_to_loop_placeholders(score_line_val) || E'\n\n' || '{/scores}';

            -- Insert or update description field
            INSERT INTO embed_preset_templates (preset_name, field_name, template)
            VALUES (preset, 'description', new_desc)
            ON CONFLICT (preset_name, field_name) DO UPDATE SET template = new_desc;

            -- Delete old fields
            DELETE FROM embed_preset_templates
            WHERE preset_name = preset AND field_name IN ('header', 'score_line');

            RAISE NOTICE 'Migrated % template', preset;
        END IF;
    END LOOP;
END $$;

-- Cleanup helper function
DROP FUNCTION IF EXISTS convert_to_loop_placeholders(TEXT, BOOLEAN);
