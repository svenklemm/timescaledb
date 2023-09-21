-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

----
-- Chunk interval 1 month - compression schedule interval should be 12 hours
----
CREATE TABLE sensor_data_1month(
  time timestamptz not null,
  sensor_id integer not null
);

SELECT FROM create_hypertable('sensor_data_1month', 'time', chunk_time_interval => INTERVAL '1 month');
ALTER TABLE sensor_data_1month SET (timescaledb.compress);

-- Add a compression policy and check the schedule interval
SELECT add_compression_policy('sensor_data_1month','7 days'::INTERVAL) as compression_job \gset
SELECT schedule_interval from timescaledb_information.jobs where job_id = :compression_job;

----
-- Chunk interval 1 week - compression schedule interval should be 12 hours
----
CREATE TABLE sensor_data_1week(
  time timestamptz not null,
  sensor_id integer not null
);

SELECT FROM create_hypertable('sensor_data_1week', 'time', chunk_time_interval => INTERVAL '1 week');
ALTER TABLE sensor_data_1week SET (timescaledb.compress);

-- Add a compression policy and check the schedule interval
SELECT add_compression_policy('sensor_data_1week','7 days'::INTERVAL) as compression_job \gset
SELECT schedule_interval from timescaledb_information.jobs where job_id = :compression_job;

----
-- Chunk interval 1 day - compression schedule interval should be 12 hours
----
CREATE TABLE sensor_data_1day(
  time timestamptz not null,
  sensor_id integer not null
);

SELECT FROM create_hypertable('sensor_data_1day', 'time', chunk_time_interval => INTERVAL '1 day');
ALTER TABLE sensor_data_1day SET (timescaledb.compress);

-- Add a compression policy and check the schedule interval
SELECT add_compression_policy('sensor_data_1day','7 days'::INTERVAL) as compression_job \gset
SELECT schedule_interval from timescaledb_information.jobs where job_id = :compression_job;


----
-- Chunk interval 1 hour - compression schedule interval should be 30 minutes
----
CREATE TABLE sensor_data_1hour(
  time timestamptz not null,
  sensor_id integer not null
);

SELECT FROM create_hypertable('sensor_data_1hour', 'time', chunk_time_interval => INTERVAL '1 hour');
ALTER TABLE sensor_data_1hour SET (timescaledb.compress);

-- Add a compression policy and check the schedule interval
SELECT add_compression_policy('sensor_data_1hour','7 days'::INTERVAL) as compression_job \gset
SELECT schedule_interval from timescaledb_information.jobs where job_id = :compression_job;

----
-- Chunk interval 1 hour - compression schedule is set to a custom value
----
CREATE TABLE sensor_data_1hour_custom(
  time timestamptz not null,
  sensor_id integer not null
);

SELECT FROM create_hypertable('sensor_data_1hour_custom', 'time', chunk_time_interval => INTERVAL '1 hour');
ALTER TABLE sensor_data_1hour_custom SET (timescaledb.compress);

-- Add a compression policy and check the schedule interval
SELECT add_compression_policy('sensor_data_1hour_custom','7 days'::INTERVAL, schedule_interval => '7 days') as compression_job \gset
SELECT schedule_interval from timescaledb_information.jobs where job_id = :compression_job;
