-- Namespace existing preset templates with "rs:" prefix
-- This allows multiple commands to share the same DB table
UPDATE embed_preset_templates SET preset_name = 'rs:' || preset_name
WHERE preset_name IN ('compact', 'classic', 'extended');
