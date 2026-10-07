#include <ovtest.h>

#include <string.h>

#include <ovarray.h>

#include "culprit.h"
#include "repair.h"
#include "settings.h"

/**
 * @brief The image name at the end of a path is the name of the process
 */
static void test_path_matches_name(void) {
  TEST_CHECK(repair_path_matches_name("\\Device\\HarddiskVolume1\\Program Files\\Audient\\iD\\iD.exe", "iD.exe") == true);
  TEST_CHECK(repair_path_matches_name("C:\\WINDOWS\\system32\\AUDIODG.EXE", "audiodg.exe") == true);
  TEST_CHECK(repair_path_matches_name("C:/x/y/foo.exe", "foo.exe") == true);
  TEST_CHECK(repair_path_matches_name("C:\\dir\\foobar.exe", "foo.exe") == false);
  TEST_CHECK(repair_path_matches_name("C:\\dir\\bar", "foo.exe") == false);
  TEST_CHECK(repair_path_matches_name("short", "muchlongername.exe") == false);
  TEST_CHECK(repair_path_matches_name(NULL, "iD.exe") == false);
  TEST_CHECK(repair_path_matches_name("C:\\x\\iD.exe", NULL) == false);
  TEST_CHECK(repair_path_matches_name("C:\\x\\iD.exe", "") == false);
  TEST_CHECK(repair_path_matches_name("iD.exe", "iD.exe") == true);
}

/**
 * @brief The blocker picks the exact instance out of a veto record
 *
 * The pid and the time of the record, not a name.  A record that names a process somewhere
 * else must still be picked (the record is the truth), while the later check in mixer_take
 * is what keeps the close away from any other process.
 */
static void test_pick_blockers(void) {
  struct culprit *list = NULL;
  struct repair_blockers b;
  char mixer_path[] = "\\Device\\HarddiskVolume1\\Program Files\\Audient\\iD\\iD.exe";
  char audiodg_path[] = "\\Device\\HarddiskVolume1\\Windows\\System32\\audiodg.exe";
  char audiodg_elsewhere[] = "\\Device\\HarddiskVolume1\\Users\\x\\Downloads\\audiodg.exe";
  char other_path[] = "\\Device\\HarddiskVolume1\\Apps\\holder.exe";
  struct culprit c;

  memset(&c, 0, sizeof(c));
  c.pid = 100;
  c.path = mixer_path;
  c.event_time_ms = 111;
  if (!TEST_CHECK(OV_ARRAY_PUSH(&list, c))) {
    goto cleanup;
  }
  c.pid = 200;
  c.path = audiodg_path;
  c.event_time_ms = 222;
  if (!TEST_CHECK(OV_ARRAY_PUSH(&list, c))) {
    goto cleanup;
  }
  c.pid = 300;
  c.path = other_path;
  c.event_time_ms = 333;
  if (!TEST_CHECK(OV_ARRAY_PUSH(&list, c))) {
    goto cleanup;
  }

  TEST_CASE("both allowed");
  b = repair_pick_blockers(list, "iD.exe", true, true);
  TEST_CHECK(b.mixer.found == true);
  TEST_CHECK(b.mixer.pid == 100);
  TEST_CHECK(b.mixer.event_time_ms == 111);
  TEST_CHECK(b.audiodg.found == true);
  TEST_CHECK(b.audiodg.pid == 200);
  TEST_CHECK(b.audiodg.event_time_ms == 222);

  TEST_CASE("mixer allowed only");
  b = repair_pick_blockers(list, "iD.exe", true, false);
  TEST_CHECK(b.mixer.found == true);
  TEST_CHECK(b.mixer.pid == 100);
  TEST_CHECK(b.audiodg.found == false);
  TEST_CHECK(b.audiodg.pid == 0);

  TEST_CASE("audiodg allowed only");
  b = repair_pick_blockers(list, "iD.exe", false, true);
  TEST_CHECK(b.mixer.found == false);
  TEST_CHECK(b.mixer.pid == 0);
  TEST_CHECK(b.audiodg.found == true);
  TEST_CHECK(b.audiodg.pid == 200);

  TEST_CASE("nothing allowed");
  b = repair_pick_blockers(list, "iD.exe", false, false);
  TEST_CHECK(b.mixer.found == false);
  TEST_CHECK(b.audiodg.found == false);

  TEST_CASE("no blockers");
  b = repair_pick_blockers(NULL, "iD.exe", true, true);
  TEST_CHECK(b.mixer.found == false);
  TEST_CHECK(b.audiodg.found == false);

  TEST_CASE("a record outside the system32 folder is still the record");
  {
    struct culprit *only = NULL;
    c.pid = 400;
    c.path = audiodg_elsewhere;
    c.event_time_ms = 444;
    if (TEST_CHECK(OV_ARRAY_PUSH(&only, c))) {
      struct repair_blockers const picked = repair_pick_blockers(only, "iD.exe", false, true);
      TEST_CHECK(picked.audiodg.found == true);
      TEST_CHECK(picked.audiodg.pid == 400);
      TEST_CHECK(picked.mixer.found == false);
      OV_ARRAY_DESTROY(&only);
    }
  }

  TEST_CASE("the newest record of the same image wins");
  {
    struct culprit *dup = NULL;
    c.pid = 500;
    c.path = audiodg_path;
    c.event_time_ms = 999;
    if (TEST_CHECK(OV_ARRAY_PUSH(&dup, c))) {
      c.pid = 501;
      c.event_time_ms = 1000;
      if (TEST_CHECK(OV_ARRAY_PUSH(&dup, c))) {
        struct repair_blockers const picked = repair_pick_blockers(dup, "iD.exe", false, true);
        TEST_CHECK(picked.audiodg.found == true);
        TEST_CHECK(picked.audiodg.pid == 501);
        TEST_CHECK(picked.audiodg.event_time_ms == 1000);
      }
      OV_ARRAY_DESTROY(&dup);
    }
  }

  TEST_CASE_(NULL);

cleanup:
  if (list != NULL) {
    OV_ARRAY_DESTROY(&list);
  }
}

/**
 * @brief A run without arguments is refused
 */
static void test_run_requires_arguments(void) {
  struct settings s;
  struct repair_result r;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  if (!TEST_FAILED_WITH(
          repair_run(NULL, true, true, NULL, NULL, NULL, &r, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument)) {
    goto cleanup;
  }
  if (!TEST_FAILED_WITH(
          repair_run(&s, true, true, NULL, NULL, NULL, NULL, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument)) {
    goto cleanup;
  }
  TEST_FAILED_WITH(repair_run(&s, true, true, NULL, NULL, NULL, &r, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);

cleanup:
  repair_result_release(&r);
}

/**
 * @brief Releasing what is not there must not crash
 */
static void test_release_null(void) { repair_result_release(NULL); }

/**
 * @brief The pids of the processes a run closed, which the caller keeps out of the list it
 *        shows
 *
 * The rule is short and it is the whole point of the feature: what was closed is not a
 * blocker any more, and a name is not an identity.
 */
static void test_closed_pids(void) {
  struct repair_blockers b;
  uint32_t out[2];

  memset(&b, 0, sizeof(b));

  TEST_CASE("nothing was closed");
  memset(out, 0, sizeof(out));
  TEST_CHECK(repair_closed_pids(&b, out) == 0);

  TEST_CASE("only the mixer was closed");
  b.mixer.found = true;
  b.mixer.pid = 100;
  TEST_CHECK(repair_closed_pids(&b, out) == 1);
  TEST_CHECK(out[0] == 100);

  TEST_CASE("both were closed");
  b.audiodg.found = true;
  b.audiodg.pid = 200;
  TEST_CHECK(repair_closed_pids(&b, out) == 2);
  TEST_CHECK(out[0] == 100);
  TEST_CHECK(out[1] == 200);

  TEST_CASE("the arguments may be absent");
  TEST_CHECK(repair_closed_pids(NULL, out) == 0);
  TEST_CHECK(repair_closed_pids(&b, NULL) == 0);

  TEST_CASE_(NULL);
}

/**
 * @brief The sentence of a run reads like a system message: what happened, in one sentence
 *
 * It ends up in the record that closes the run.  The defect this test exists for was prose
 * that repeated the state word of the record around it, saying "the removal failed for a
 * reason other than a veto" beside a log that had already said the run failed.
 */
static void test_result_sentence(void) {
  char service_name[] = "adc-repair-test-no-such-service";
  struct settings s;
  struct repair_result r;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  s.ks_service = service_name;

  TEST_CASE("a run with nothing to release");
  if (TEST_CHECK(repair_run(&s, true, true, NULL, NULL, NULL, &r, &err))) {
    TEST_CHECK(r.outcome == REPAIR_OUTCOME_ABSENT);
    if (!TEST_CHECK(strcmp(r.message, "No Audient device was found.") == 0)) {
      TEST_MSG("want a system message, got [%hs]", r.message);
    }
  }
  TEST_CASE_(NULL);

  repair_result_release(&r);
  OV_ERROR_REPORT(&err, NULL);
}

struct cancel_ctx {
  struct settings const *s;
  bool *cancel;
  struct repair_result *r;
};

/**
 * @brief The progress callback that raises the cancel flag at the first step
 *
 * @param stage unused, the first report is enough
 * @param step unused
 * @param total unused
 * @param userdata the context with the flag
 */
static void raise_cancel_progress(enum repair_stage const stage, size_t const step, size_t const total, void *const userdata) {
  struct cancel_ctx *const ctx = (struct cancel_ctx *)userdata;

  (void)stage;
  (void)step;
  (void)total;
  *ctx->cancel = true;
}

/**
 * @brief A cancel flag stops the run before its next step
 *
 * The reader of the progress window can ask the run to stop; the run is not killed in the
 * middle of a step, it reads the flag between the steps and ends with the outcome that
 * says it was aborted instead of walking on.  The flag is a pointer the caller owns, so
 * the window can set it from another thread.
 */
static void test_cancel_stops_the_run(void) {
  char service_name[] = "adc-repair-test-no-such-service";
  struct settings s;
  struct repair_result r;
  struct ov_error err = {0};
  bool cancel = true; // the flag is up before the run starts

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  s.ks_service = service_name;

  TEST_CHECK(repair_run(&s, true, true, NULL, NULL, &cancel, &r, &err));
  TEST_CHECK_(r.outcome == REPAIR_OUTCOME_ABORTED, "want the aborted outcome, got %d", (int)r.outcome);
  TEST_CHECK(strcmp(r.message, "The operation was aborted by the user.") == 0);

  TEST_CASE("a flag that goes up during the run");
  {
    struct cancel_ctx ctx;
    struct repair_result r2;
    struct ov_error err2 = {0};

    memset(&ctx, 0, sizeof(ctx));
    memset(&r2, 0, sizeof(r2));
    ctx.cancel = &cancel;
    cancel = false;
    TEST_CHECK(repair_run(&s, true, true, raise_cancel_progress, &ctx, &cancel, &r2, &err2));
    TEST_CHECK(r2.outcome == REPAIR_OUTCOME_ABORTED);
    repair_result_release(&r2);
    OV_ERROR_REPORT(&err2, NULL);
  }

  TEST_CASE("no flag at all runs as before");
  {
    struct repair_result r3;
    struct ov_error err3 = {0};

    memset(&r3, 0, sizeof(r3));
    TEST_CHECK(repair_run(&s, true, true, NULL, NULL, NULL, &r3, &err3));
    TEST_CHECK(r3.outcome == REPAIR_OUTCOME_ABSENT);
    repair_result_release(&r3);
    OV_ERROR_REPORT(&err3, NULL);
  }

  repair_result_release(&r);
  OV_ERROR_REPORT(&err, NULL);
}

/**
 * @brief A veto where nothing was closed says which way to look
 *
 * What the reader has to learn first is that the device is still in use, and then which
 * single thing they can change: the name of the setting that kept this tool from closing the
 * process, said the way the window says it, or the number of the processes in the way.  The
 * defect this test exists for was the bare "may not be closed", which named nothing.
 */
static void test_blocked_sentence(void) {
  char msg[256];
  char label[256];

  repair_option_mixer_label(NULL, label, sizeof(label));

  TEST_CASE("the setting that holds the run back is named as the window names it");
  repair_blocked_sentence(label, 1, msg, sizeof(msg));
  if (!TEST_CHECK(strcmp(msg, "The device is still in use because \"Close iD Mixer (iD.exe) automatically\" is turned off.") == 0)) {
    TEST_MSG("want the sentence to name the option, got [%hs]", msg);
  }

  TEST_CASE("the record names something else");
  repair_blocked_sentence(NULL, 1, msg, sizeof(msg));
  TEST_CHECK(strcmp(msg, "The device is still in use by another process.") == 0);

  TEST_CASE("more than one blocker is a number, not one name");
  repair_blocked_sentence(label, 3, msg, sizeof(msg));
  if (!TEST_CHECK(strcmp(msg, "The device is still in use by 3 processes.") == 0)) {
    TEST_MSG("want the number of the blockers, got [%hs]", msg);
  }

  TEST_CASE_(NULL);
}

/**
 * @brief The two settings this tool may close have one name each
 *
 * The window writes them on its checkboxes and the log quotes them in the sentence of a
 * veto, so the reader who meets the name in the log finds the same words to press.  Two
 * copies of the words would let one of the two drift.
 */
static void test_option_labels_are_named_once(void) {
  char label[256];

  repair_option_mixer_label(NULL, label, sizeof(label));
  TEST_CHECK(strcmp(label, "Close iD Mixer (iD.exe) automatically") == 0);
  TEST_CHECK(strcmp(repair_option_audiodg_label(), "Close audiodg (audiodg.exe) automatically") == 0);
}

TEST_LIST = {
    {"path_matches_name", test_path_matches_name},
    {"pick_blockers", test_pick_blockers},
    {"run_requires_arguments", test_run_requires_arguments},
    {"release_null", test_release_null},
    {"closed_pids", test_closed_pids},
    {"result_sentence", test_result_sentence},
    {"blocked_sentence", test_blocked_sentence},
    {"option_labels", test_option_labels_are_named_once},
    {"cancel_stops_the_run", test_cancel_stops_the_run},
    {NULL, NULL},
};
