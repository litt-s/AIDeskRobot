-- Import preserves SQLite row order; advancing identity sequences needs UPDATE.
GRANT UPDATE ON ALL SEQUENCES IN SCHEMA aidesk TO service_role;
