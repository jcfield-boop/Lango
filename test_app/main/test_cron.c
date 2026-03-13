/**
 * test_cron.c — Unity tests for cron scheduling edge cases.
 *
 * Tests the logic that guards against stale epoch fires on boot,
 * interval rollover, and one-shot job semantics — all without
 * needing LittleFS or network.
 */

#include "unity.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Inline simplified cron_job_t for white-box testing ─────── */

typedef enum { CRON_KIND_INTERVAL = 0, CRON_KIND_ONETIME } cron_kind_t;

typedef struct {
    char         id[16];
    bool         enabled;
    cron_kind_t  kind;
    uint32_t     interval_s;
    int64_t      at_epoch;      /* for ONETIME: target unix timestamp */
    int64_t      next_run;      /* 0 = unscheduled */
    bool         delete_after;
} test_cron_job_t;

/* Mirrors compute_initial_next_run() in cron_service.c */
static void compute_next_run(test_cron_job_t *job, int64_t now)
{
    if (job->kind == CRON_KIND_INTERVAL && job->interval_s > 0) {
        job->next_run = now + job->interval_s;
    } else if (job->kind == CRON_KIND_ONETIME) {
        job->next_run = (job->at_epoch > now) ? job->at_epoch : 0;
    } else {
        job->next_run = 0;
    }
}

/* Mirrors the fire-and-reschedule logic in cron_service.c */
static bool should_fire(const test_cron_job_t *job, int64_t now)
{
    return job->enabled && job->next_run > 0 && job->next_run <= now;
}

static void after_fire(test_cron_job_t *job, int64_t now)
{
    if (job->kind == CRON_KIND_INTERVAL) {
        job->next_run = now + job->interval_s;
    } else {
        job->next_run = job->delete_after ? -1 /* mark for deletion */ : 0;
    }
}

/* ── interval job: fires exactly at next_run ────────────────── */

TEST_CASE("interval job fires at correct time", "[cron]")
{
    int64_t now = 1700000000LL;
    test_cron_job_t job = {
        .enabled    = true,
        .kind       = CRON_KIND_INTERVAL,
        .interval_s = 3600,
    };
    compute_next_run(&job, now);

    TEST_ASSERT_FALSE(should_fire(&job, now));         /* not yet */
    TEST_ASSERT_FALSE(should_fire(&job, now + 3599));  /* 1 s early */
    TEST_ASSERT_TRUE(should_fire(&job, now + 3600));   /* exactly on time */
    TEST_ASSERT_TRUE(should_fire(&job, now + 7200));   /* overdue */
}

/* ── stale epoch bug: at_epoch in the past must NOT fire ────── */

TEST_CASE("one-shot with past epoch is not scheduled", "[cron]")
{
    int64_t now = 1700000000LL;
    test_cron_job_t job = {
        .enabled   = true,
        .kind      = CRON_KIND_ONETIME,
        .at_epoch  = now - 3600,  /* 1 hour in the past */
    };
    compute_next_run(&job, now);

    /* next_run must be 0 (unscheduled), not the stale past epoch */
    TEST_ASSERT_EQUAL(0, job.next_run);
    TEST_ASSERT_FALSE(should_fire(&job, now));
}

/* ── future one-shot schedules correctly ─────────────────────── */

TEST_CASE("one-shot with future epoch fires at right time", "[cron]")
{
    int64_t now = 1700000000LL;
    int64_t target = now + 7200;
    test_cron_job_t job = {
        .enabled  = true,
        .kind     = CRON_KIND_ONETIME,
        .at_epoch = target,
    };
    compute_next_run(&job, now);

    TEST_ASSERT_EQUAL(target, job.next_run);
    TEST_ASSERT_FALSE(should_fire(&job, now + 7199));
    TEST_ASSERT_TRUE(should_fire(&job, now + 7200));
}

/* ── interval reschedule: next_run advances by interval ─────── */

TEST_CASE("interval job reschedules after fire", "[cron]")
{
    int64_t now = 1700000000LL;
    test_cron_job_t job = {
        .enabled    = true,
        .kind       = CRON_KIND_INTERVAL,
        .interval_s = 86400,   /* daily */
    };
    compute_next_run(&job, now);
    int64_t first_run = job.next_run;

    TEST_ASSERT_TRUE(should_fire(&job, first_run));
    after_fire(&job, first_run);

    TEST_ASSERT_EQUAL(first_run + 86400, job.next_run);
    TEST_ASSERT_FALSE(should_fire(&job, first_run));       /* just fired */
    TEST_ASSERT_TRUE(should_fire(&job, first_run + 86400));
}

/* ── disabled job never fires ────────────────────────────────── */

TEST_CASE("disabled job does not fire", "[cron]")
{
    int64_t now = 1700000000LL;
    test_cron_job_t job = {
        .enabled    = false,
        .kind       = CRON_KIND_INTERVAL,
        .interval_s = 60,
        .next_run   = now - 1,  /* overdue */
    };
    TEST_ASSERT_FALSE(should_fire(&job, now));
}

/* ── delete_after: one-shot marks itself for deletion ────────── */

TEST_CASE("one-shot delete_after job marks self after fire", "[cron]")
{
    int64_t now = 1700000000LL;
    test_cron_job_t job = {
        .enabled      = true,
        .kind         = CRON_KIND_ONETIME,
        .at_epoch     = now + 100,
        .delete_after = true,
    };
    compute_next_run(&job, now);
    TEST_ASSERT_TRUE(should_fire(&job, now + 100));

    after_fire(&job, now + 100);
    /* next_run == -1 signals deletion to the cron service */
    TEST_ASSERT_EQUAL(-1, job.next_run);
}

/* ── zero interval: job stays unscheduled ────────────────────── */

TEST_CASE("interval job with zero interval stays unscheduled", "[cron]")
{
    int64_t now = 1700000000LL;
    test_cron_job_t job = {
        .enabled    = true,
        .kind       = CRON_KIND_INTERVAL,
        .interval_s = 0,
    };
    compute_next_run(&job, now);
    TEST_ASSERT_EQUAL(0, job.next_run);
    TEST_ASSERT_FALSE(should_fire(&job, now + 99999));
}
