-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 08_ring_buffer_overflow.sql
-- Fill the ring buffer past its 2048-entry capacity and confirm the extension
-- does not crash (oldest entries are silently overwritten).

SET client_min_messages = WARNING;
SET pg_ext_memcheck.memcheck_mode = 'all';
SET pg_ext_memcheck.min_leak_bytes = '0';   -- log every allocation change
SELECT ext_memcheck.flush_violations() >= 0 AS cleaned;
DELETE FROM ext_memcheck.violation_log;

-- Run enough iterations to fill the 2048-slot ring buffer many times over
SELECT ext_memcheck.run_scenario('growth_benchmark', 300) IS NOT NULL AS ok;

-- Extension must still respond normally after overflow
SET pg_ext_memcheck.memcheck_mode = 'none';
SELECT ext_memcheck.begin('');
SELECT count(*) >= 0 AS survived FROM ext_memcheck.end();

-- flush_violations must still work after overflow
SELECT ext_memcheck.flush_violations() >= 0 AS flush_ok;

-- Cleanup
DELETE FROM ext_memcheck.violation_log;
RESET pg_ext_memcheck.min_leak_bytes;
SET pg_ext_memcheck.memcheck_mode = 'none';
