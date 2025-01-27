-- This file and its contents are licensed under the Apache License 2.0.
-- Please see the included NOTICE for copyright information and
-- LICENSE-APACHE for a copy of the license.

--EVENT TRIGGER MUST exclude the ALTER EXTENSION tag.
CREATE EVENT TRIGGER timescaledb_ddl_command_end ON ddl_command_end
WHEN TAG IN ('ALTER TABLE','CREATE TRIGGER','CREATE TABLE','CREATE INDEX','ALTER INDEX', 'DROP TABLE', 'DROP INDEX', 'DROP SCHEMA')
EXECUTE FUNCTION _timescaledb_functions.process_ddl_event();

CREATE EVENT TRIGGER timescaledb_ddl_sql_drop ON sql_drop
EXECUTE FUNCTION _timescaledb_functions.process_ddl_event();
