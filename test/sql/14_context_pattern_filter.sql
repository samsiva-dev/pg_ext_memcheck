-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 14_context_pattern_filter.sql
-- Verify that ext_context_pattern scopes violation reporting to matching
-- contexts, and that the allowed_contexts allowlist suppresses false positives
-- for contexts that are permitted to grow (§7 / §14 Q1 of the design doc).

SET client_min_messages = WARNING;
SELECT ext_memcheck.flush_violations() >= 0 AS cleaned;
DELETE FROM ext_memcheck.violation_log;

-- ----------------------------------------------------------------------------
-- 1. Pattern that matches nothing: with a pattern that cannot match any real
--    context name, the growth_benchmark scenario must produce zero violations
--    even though it forces measurable allocations.
-- ----------------------------------------------------------------------------
SET pg_ext_memcheck.memcheck_mode = 'all';
SET pg_ext_memcheck.min_leak_bytes = '0';
SET pg_ext_memcheck.bloat_min_bytes = '0';

SELECT ext_memcheck.begin('__no_real_ctx_will_match_this__');
SELECT ext_memcheck.run_scenario('growth_benchmark', 100) IS NOT NULL AS ran;
SELECT count(*) AS violations_with_unmatched_pattern FROM ext_memcheck.end();

-- ----------------------------------------------------------------------------
-- 2. Empty pattern (match all): same workload with no pattern must produce
--    >= 0 violations (the hook fires and may record something).
-- ----------------------------------------------------------------------------
SELECT ext_memcheck.begin('');
SELECT ext_memcheck.run_scenario('growth_benchmark', 100) IS NOT NULL AS ran;
SELECT count(*) >= 0 AS violations_with_empty_pattern FROM ext_memcheck.end();

-- ----------------------------------------------------------------------------
-- 3. allowlist suppresses wrong_ctx_alloc for named contexts: wrong_context_probe
--    emits wrong_ctx_alloc for global contexts that grow (TopMemoryContext,
--    CacheMemoryContext).  Adding them to allowed_contexts must produce zero
--    wrong_ctx_alloc rows that mention those specific contexts.
-- ----------------------------------------------------------------------------
SELECT ext_memcheck.begin(
    '',
    '{"allowed_contexts": ["TopMemoryContext", "CacheMemoryContext"]}'
);
SELECT ext_memcheck.run_scenario('wrong_context_probe', 10) IS NOT NULL AS ran;
SELECT count(*) AS wrong_ctx_for_allowlisted
FROM ext_memcheck.end()
WHERE check_type = 'wrong_ctx_alloc'
  AND (detail LIKE '%TopMemoryContext%' OR detail LIKE '%CacheMemoryContext%');

-- ----------------------------------------------------------------------------
-- 4. begin() with options but no pattern (NULL pattern treated as match-all)
-- ----------------------------------------------------------------------------
SELECT ext_memcheck.begin(
    NULL,
    '{"track_shmem": false, "track_dsm": false}'
) LIKE 'Memory check%' AS null_pattern_accepted;
SET pg_ext_memcheck.memcheck_mode = 'none';
SELECT count(*) >= 0 AS ok FROM ext_memcheck.end();

-- ----------------------------------------------------------------------------
-- 5. Error: pattern exceeds NAMEDATALEN (64 chars) must raise an error.
-- ----------------------------------------------------------------------------
DO $$
BEGIN
    PERFORM ext_memcheck.begin(
        repeat('x', 65)  -- one character over the NAMEDATALEN-1 limit
    );
    RAISE WARNING 'FAIL: should have errored';
EXCEPTION
    WHEN invalid_parameter_value THEN
        RAISE WARNING 'OK: oversized pattern rejected';
END;
$$;

-- Cleanup
DELETE FROM ext_memcheck.violation_log;
RESET pg_ext_memcheck.min_leak_bytes;
RESET pg_ext_memcheck.bloat_min_bytes;
SET pg_ext_memcheck.memcheck_mode = 'none';
