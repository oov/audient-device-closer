#include "repair.h"

#include "repair.c"

/**
 * @brief The fake the confirmation is answered through, see probe_devnode_fn
 */
static void *g_confirm_ctx = NULL;

#include <ovtest.h>

#include <string.h>

#include <ovarray.h>
#include <ovprintf_ex.h>

#include "process.h"
#include "settings.h"

/**
 * @brief What the retry of a vetoed removal is answered with
 */
static struct {
  size_t removal_calls;  //!< how often the removal was asked for
  bool removal_recovers; //!< the removal succeeds from this call on
  size_t veto_records;   //!< how many records the veto read hands back
  size_t mixer_closed;   //!< how many blockers were closed
  char const *image;     //!< the image the veto records name, NULL for the mixer of the run
} g_retry;

/**
 * @brief Refuse every removal but the one the test lets through
 */
static bool fake_vetoed_removal(char const *const devnode, struct ov_error *const err) {
  g_retry.removal_calls++;
  if (g_retry.removal_recovers && (g_retry.removal_calls >= 2)) {
    return true;
  }
  (void)devnode;
  OV_ERROR_SETF(err,
                ov_error_type_removal,
                removal_error_veto,
                "the removal was vetoed, blocked device: %1$hs",
                "the removal was vetoed, blocked device: %1$hs",
                "USB\\VID_2708&PID_0008\\6&deadbee&0&2");
  return false;
}

/**
 * @brief Hand back the records of a veto, made the way culprit_read makes them
 */
static struct culprit *
fake_read_veto(char const *const log_name, char const *const provider_name, unsigned short const event_id, struct ov_error *const err) {
  struct culprit *list = NULL;
  struct culprit c = {0};

  (void)log_name;
  (void)provider_name;
  (void)event_id;
  (void)err;
  for (size_t i = 0; i < g_retry.veto_records; i++) {
    static char const instance[] = "USB\\VID_2708&PID_0008\\6&deadbee&0&2";
    char const *const image = (g_retry.image != NULL) ? g_retry.image : "\\Device\\HarddiskVolume2\\Program Files\\Audient\\iD\\iD.exe";
    size_t const image_bytes = strlen(image) + 1;

    if (!OV_REALLOC(&c.device_instance, sizeof(instance), sizeof(c.device_instance[0]))) {
      goto cleanup;
    }
    memcpy(c.device_instance, instance, sizeof(instance));
    if (!OV_REALLOC(&c.path, image_bytes, sizeof(c.path[0]))) {
      OV_FREE(&c.device_instance);
      goto cleanup;
    }
    memcpy(c.path, image, image_bytes);
    c.pid = 100 + (uint32_t)i;
    c.event_time_ms = process_now_ms() + 1000; // newer than the attempt, so the prune keeps it
    if (!OV_ARRAY_PUSH(&list, c)) {
      OV_FREE(&c.device_instance);
      OV_FREE(&c.path);
      goto cleanup;
    }
    memset(&c, 0, sizeof(c));
  }
cleanup:
  return list;
}

/**
 * @brief Every record belongs to the device the caller names
 */
static ov_tribool fake_names_ancestor(char const *const device_instance, char const *const devnode, struct ov_error *const err) {
  (void)device_instance;
  (void)devnode;
  (void)err;
  return ov_true;
}

/**
 * @brief The record names the instance that is running
 */
static bool fake_mixer_take(uint32_t const pid,
                            char const *const exe_name,
                            long long const event_time_ms,
                            bool *const out_found,
                            struct mixer_target *const out,
                            struct ov_error *const err) {
  (void)event_time_ms;
  (void)err;
  *out_found = true;
  out->pid = pid;
  if (!ov_sprintf_char(&out->path, NULL, NULL, "C:\\Program Files\\Audient\\iD\\%s", exe_name)) {
    return false;
  }
  return true;
}

/**
 * @brief The blocker goes when the run asks it to
 */
static bool fake_mixer_close(struct mixer_target *const target, struct ov_error *const err) {
  (void)target;
  (void)err;
  g_retry.mixer_closed++;
  return true;
}

static void fake_mixer_target_release(struct mixer_target *const target) {
  if ((target == NULL) || (target->path == NULL)) {
    return;
  }
  OV_ARRAY_DESTROY(&target->path);
  target->pid = 0;
}

/**
 * @brief The device that was released is an audio device again
 */
static enum device_state fake_probe_healthy(char const *const devnode, struct ov_error *const err) {
  (void)devnode;
  (void)err;
  return DEVICE_STATE_HEALTHY;
}

/**
 * @brief The image name at the end of a path is the name of the process
 */
static void test_path_matches_name(void) {
  TEST_CHECK(process_path_matches_name("\\Device\\HarddiskVolume1\\Program Files\\Audient\\iD\\iD.exe", "iD.exe") == true);
  TEST_CHECK(process_path_matches_name("C:\\WINDOWS\\system32\\AUDIODG.EXE", "audiodg.exe") == true);
  TEST_CHECK(process_path_matches_name("C:/x/y/foo.exe", "foo.exe") == true);
  TEST_CHECK(process_path_matches_name("C:\\dir\\foobar.exe", "foo.exe") == false);
  TEST_CHECK(process_path_matches_name("C:\\dir\\bar", "foo.exe") == false);
  TEST_CHECK(process_path_matches_name("short", "muchlongername.exe") == false);
  TEST_CHECK(process_path_matches_name(NULL, "iD.exe") == false);
  TEST_CHECK(process_path_matches_name("C:\\x\\iD.exe", NULL) == false);
  TEST_CHECK(process_path_matches_name("C:\\x\\iD.exe", "") == false);
  TEST_CHECK(process_path_matches_name("iD.exe", "iD.exe") == true);
}

/**
 * @brief The blocker picks the exact instance out of a veto record
 *
 * The pid and the time of the record, not a name.  A record that names a process somewhere
 * else must still be picked (the record is the truth), while the later check in mixer_take
 * is what keeps the close away from any other process.
 */
static void test_pick_blockers(void) {
  /* the mixer names come from the settings; the audio engine is named inside repair_pick_blockers */
  struct ov_error err = {0};
  struct culprit *list = NULL;
  struct repair_blockers b;
  char mixer_path[] = "\\Device\\HarddiskVolume1\\Program Files\\Audient\\iD\\iD.exe";
  char audiodg_path[] = "\\Device\\HarddiskVolume1\\Windows\\System32\\audiodg.exe";
  char audiodg_elsewhere[] = "\\Device\\HarddiskVolume1\\Users\\x\\Downloads\\audiodg.exe";
  char other_path[] = "\\Device\\HarddiskVolume1\\Apps\\holder.exe";
  struct culprit c;
  static char const *const names[] = {"iD.exe", "EVO.exe"};

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
  b = repair_pick_blockers(list, names, 2, true, true, &err);
  TEST_CHECK(b.mixer_count == 1);
  TEST_CHECK(b.mixer[0].found == true);
  TEST_CHECK(b.mixer[0].pid == 100);
  TEST_CHECK(b.mixer[0].event_time_ms == 111);
  TEST_CHECK(b.mixer[0].name != NULL && strcmp(b.mixer[0].name, "iD.exe") == 0);
  TEST_CHECK(b.audiodg_count == 1);
  TEST_CHECK(b.audiodg[0].found == true);
  TEST_CHECK(b.audiodg[0].pid == 200);
  TEST_CHECK(b.audiodg[0].event_time_ms == 222);
  TEST_CHECK(b.audiodg[0].name != NULL && strcmp(b.audiodg[0].name, "audiodg.exe") == 0);

  TEST_CASE("mixer allowed only");
  b = repair_pick_blockers(list, names, 2, true, false, &err);
  TEST_CHECK(b.mixer_count == 1);
  TEST_CHECK(b.mixer[0].pid == 100);
  TEST_CHECK(b.mixer[0].name != NULL && strcmp(b.mixer[0].name, "iD.exe") == 0);
  TEST_CHECK(b.audiodg_count == 0);

  {
    struct culprit *mixed = NULL;
    c.pid = 150;
    c.path = "\\Device\\HarddiskVolume1\\Program Files\\Audient\\EVO\\EVO.exe";
    c.event_time_ms = 555;
    if (TEST_CHECK(OV_ARRAY_PUSH(&mixed, c))) {
      struct repair_blockers const picked = repair_pick_blockers(mixed, names, 2, true, false, &err);
      TEST_CHECK(picked.mixer_count == 1);
      TEST_CHECK(picked.mixer[0].pid == 150);
      TEST_CHECK(picked.mixer[0].name != NULL && strcmp(picked.mixer[0].name, "EVO.exe") == 0);
      OV_ARRAY_DESTROY(&mixed);
    }
  }

  TEST_CASE("audiodg allowed only");
  b = repair_pick_blockers(list, names, 2, false, true, &err);
  TEST_CHECK(b.mixer_count == 0);
  TEST_CHECK(b.audiodg_count == 1);
  TEST_CHECK(b.audiodg[0].pid == 200);

  TEST_CASE("nothing allowed");
  b = repair_pick_blockers(list, names, 2, false, false, &err);
  TEST_CHECK(b.mixer_count == 0);
  TEST_CHECK(b.audiodg_count == 0);

  TEST_CASE("no blockers");
  b = repair_pick_blockers(NULL, names, 2, true, true, &err);
  TEST_CHECK(b.mixer_count == 0);
  TEST_CHECK(b.audiodg_count == 0);

  TEST_CASE("a record outside the system32 folder is still the record");
  {
    struct culprit *only = NULL;
    c.pid = 400;
    c.path = audiodg_elsewhere;
    c.event_time_ms = 444;
    if (TEST_CHECK(OV_ARRAY_PUSH(&only, c))) {
      struct repair_blockers const picked = repair_pick_blockers(only, names, 2, false, true, &err);
      TEST_CHECK(picked.audiodg_count == 1);
      TEST_CHECK(picked.audiodg[0].pid == 400);
      TEST_CHECK(picked.mixer_count == 0);
      OV_ARRAY_DESTROY(&only);
    }
  }

  TEST_CASE("every instance of the same image is picked");
  {
    struct culprit *dup = NULL;
    c.pid = 500;
    c.path = audiodg_path;
    c.event_time_ms = 999;
    if (TEST_CHECK(OV_ARRAY_PUSH(&dup, c))) {
      c.pid = 501;
      c.event_time_ms = 1000;
      if (TEST_CHECK(OV_ARRAY_PUSH(&dup, c))) {
        struct repair_blockers const picked = repair_pick_blockers(dup, names, 2, false, true, &err);
        TEST_CHECK(picked.audiodg_count == 2);
        TEST_CHECK(picked.audiodg[0].pid == 500);
        TEST_CHECK(picked.audiodg[1].pid == 501);
        TEST_CHECK(picked.audiodg[1].event_time_ms == 1000);
      }
      OV_ARRAY_DESTROY(&dup);
    }
  }

  TEST_CASE("a repeated pid is picked once");
  {
    struct culprit *dup = NULL;
    c.pid = 600;
    c.path = audiodg_path;
    c.event_time_ms = 100;
    if (TEST_CHECK(OV_ARRAY_PUSH(&dup, c))) {
      c.event_time_ms = 200;
      if (TEST_CHECK(OV_ARRAY_PUSH(&dup, c))) {
        struct repair_blockers const picked = repair_pick_blockers(dup, names, 2, false, true, &err);
        TEST_CHECK(picked.audiodg_count == 1);
        TEST_CHECK(picked.audiodg[0].pid == 600);
        TEST_CHECK(picked.audiodg[0].event_time_ms == 100);
      }
      OV_ARRAY_DESTROY(&dup);
    }
  }

  TEST_CASE_(NULL);

  TEST_CASE("more blockers than this tool carries are refused");
  {
    struct culprit *many = NULL;
    struct repair_blockers picked;
    size_t i = 0;
    for (i = 0; i <= REPAIR_MAX_BLOCKERS; i++) {
      c.pid = (uint32_t)(700 + i);
      c.path = audiodg_path;
      c.event_time_ms = (long long)(1000 + i);
      if (!TEST_CHECK(OV_ARRAY_PUSH(&many, c))) {
        goto cleanup;
      }
    }
    picked = repair_pick_blockers(many, names, 2, false, true, &err);
    TEST_CHECK(picked.audiodg_count == SIZE_MAX);
    TEST_FAILED_WITH(false, &err, ov_error_type_generic, ov_error_generic_fail);
    OV_ARRAY_DESTROY(&many);
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
          repair_run(NULL, NULL, true, true, NULL, NULL, NULL, &r, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument)) {
    goto cleanup;
  }
  if (!TEST_FAILED_WITH(
          repair_run(&s, NULL, true, true, NULL, NULL, NULL, NULL, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument)) {
    goto cleanup;
  }
  TEST_FAILED_WITH(
      repair_run(&s, NULL, true, true, NULL, NULL, NULL, &r, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument);

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
  uint32_t out[2 * REPAIR_MAX_BLOCKERS];

  memset(&b, 0, sizeof(b));

  TEST_CASE("nothing was closed");
  memset(out, 0, sizeof(out));
  TEST_CHECK(repair_closed_pids(&b, out) == 0);

  TEST_CASE("only the mixer was closed");
  b.mixer_count = 1;
  b.mixer[0].found = true;
  b.mixer[0].pid = 100;
  TEST_CHECK(repair_closed_pids(&b, out) == 1);
  TEST_CHECK(out[0] == 100);

  TEST_CASE("both were closed");
  b.audiodg_count = 1;
  b.audiodg[0].found = true;
  b.audiodg[0].pid = 200;
  TEST_CHECK(repair_closed_pids(&b, out) == 2);
  TEST_CHECK(out[0] == 100);
  TEST_CHECK(out[1] == 200);

  TEST_CASE("every closed instance is listed");
  b.mixer_count = 2;
  b.mixer[1].found = true;
  b.mixer[1].pid = 101;
  b.audiodg_count = 2;
  b.audiodg[1].found = true;
  b.audiodg[1].pid = 201;
  TEST_CHECK(repair_closed_pids(&b, out) == 4);
  TEST_CHECK(out[0] == 100);
  TEST_CHECK(out[1] == 101);
  TEST_CHECK(out[2] == 200);
  TEST_CHECK(out[3] == 201);

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
  if (TEST_CHECK(repair_run(&s, NULL, true, true, NULL, NULL, NULL, &r, &err))) {
    TEST_CHECK(r.outcome == REPAIR_OUTCOME_ABSENT);
    if (!TEST_CHECK(strcmp(r.message, "No Audient device was found.") == 0)) {
      TEST_MSG("want a system message, got [%hs]", r.message);
    }
  }
  TEST_CASE_(NULL);

  repair_result_release(&r);
}

struct cancel_ctx {
  struct settings const *s;
  bool *cancel;
  struct repair_result *r;
};

/**
 * @brief The looks the confirmation is answered with, and how many it took
 */
struct confirm_ctx {
  struct settings s;
  enum device_state states[8]; //!< the answer of each look, in the order they come
  size_t count;                //!< how many answers the list holds
  size_t calls;                //!< how many looks were taken
  size_t fail_until;           //!< the looks up to and including this one fail, 0 for none
};

/**
 * @brief Answer the confirmation with the states of the list, in order
 */
static enum device_state fake_probe_devnode(char const *const devnode, struct ov_error *const err) {
  struct confirm_ctx *const ctx = (struct confirm_ctx *)g_confirm_ctx;

  (void)devnode;
  ctx->calls++;
  if ((ctx->fail_until != 0) && (ctx->calls <= ctx->fail_until)) {
    // a look that could not be taken answers with its own value and leaves its reason,
    // just like the real query does
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    return DEVICE_STATE_QUERY_FAILED;
  }
  if (ctx->calls > ctx->count) {
    return DEVICE_STATE_NO_INTERFACE; // the list is over, the fake has nothing more to answer
  }
  return ctx->states[ctx->calls - 1];
}

/**
 * @brief Run the confirmation through the fake look
 */
static enum repair_outcome
confirm_through_fake(struct device_target *const target, struct repair_result *const r, struct confirm_ctx *const ctx) {
  struct repair_deps deps = {0};
  struct repair_ctx run = {0};
  struct ov_error err = {0};
  enum repair_outcome outcome;

  g_confirm_ctx = ctx;
  deps.probe_devnode = fake_probe_devnode;
  run.settings = &ctx->s;
  run.out = r;
  outcome = confirm_recovered(&run, &deps, target, &err);
  g_confirm_ctx = NULL;
  return outcome;
}

/**
 * @brief The confirmation keeps looking while the settings let it wait
 *
 * The look is replaced for the test, so the polling structure is what is checked here: a
 * device that comes back somewhere in the middle of the window ends the polling at that
 * look, and one that never comes back is given up on when the window is over.
 */
static void test_confirm_polls_until_the_device_comes_back(void) {
  char devnode[] = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct device_target target;
  struct repair_result r;
  struct confirm_ctx ctx;

  memset(&target, 0, sizeof(target));
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  memset(&r, 0, sizeof(r));
  memset(&ctx, 0, sizeof(ctx));
  ctx.s.confirm_max_wait_ms = 4000; // four looks, one second apart

  TEST_CASE("the device is an audio device again at the third look");
  ctx.states[0] = DEVICE_STATE_FAULTED;
  ctx.states[1] = DEVICE_STATE_FAULTED;
  ctx.states[2] = DEVICE_STATE_HEALTHY;
  ctx.count = 3;
  TEST_CHECK(confirm_through_fake(&target, &r, &ctx) == REPAIR_OUTCOME_CLOSED);
  TEST_CHECK_(ctx.calls == 3, "want the polling to stop at the healthy look, got %zu", ctx.calls);

  TEST_CASE("the device never comes back within the window");
  ctx.count = 0;
  ctx.calls = 0;
  TEST_CHECK(confirm_through_fake(&target, &r, &ctx) == REPAIR_OUTCOME_FAILED);
  TEST_CHECK_(ctx.calls == 4, "want the four looks the wait asks for, got %zu", ctx.calls);
}

/**
 * @brief A look that failed does not stop the next one on an assertion
 *
 * A look that could not be taken leaves its reason in the error it was given, and the
 * polling takes several looks.  Each look has to get the struct back to itself, so the
 * reason of the one before is spent before the next one writes its own.
 */
static void test_confirm_survives_a_failed_look(void) {
  char devnode[] = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct device_target target;
  struct repair_result r;
  struct confirm_ctx ctx;

  memset(&target, 0, sizeof(target));
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  memset(&r, 0, sizeof(r));
  memset(&ctx, 0, sizeof(ctx));
  ctx.s.confirm_max_wait_ms = 4000; // four looks, one second apart

  TEST_CASE("the first looks fail and the device is back at the fourth");
  ctx.fail_until = 3;
  ctx.states[3] = DEVICE_STATE_HEALTHY;
  ctx.count = 4;
  TEST_CHECK(confirm_through_fake(&target, &r, &ctx) == REPAIR_OUTCOME_CLOSED);
  TEST_CHECK_(ctx.calls == 4, "want the polling to stop at the healthy look, got %zu", ctx.calls);

  TEST_CASE("every look fails, the window runs out");
  ctx.calls = 0;
  ctx.fail_until = 8;
  ctx.count = 0;
  TEST_CHECK(confirm_through_fake(&target, &r, &ctx) == REPAIR_OUTCOME_FAILED);
  TEST_CHECK_(ctx.calls == 4, "want the four looks the wait asks for, got %zu", ctx.calls);
}

/**
 * @brief How many looks the confirmation takes for a wait the settings name
 *
 * One second between two looks is fixed, so a wait of five seconds is five looks.  The
 * smallest wait still looks once: a device that is back at once must be found by it.
 */
static void test_confirm_poll_count_from_the_wait(void) {
  TEST_CHECK_(confirm_poll_count(5000) == 5, "want five looks for five seconds, got %zu", confirm_poll_count(5000));
  TEST_CHECK_(confirm_poll_count(15000) == 15, "want the looks the wait asks for, got %zu", confirm_poll_count(15000));
  TEST_CHECK_(confirm_poll_count(1000) == 1, "want one look for one second, got %zu", confirm_poll_count(1000));
  TEST_CHECK_(confirm_poll_count(0) == 1, "want one look when the wait is over, got %zu", confirm_poll_count(0));
  TEST_CHECK_(confirm_poll_count(999) == 1, "want one look for a wait shorter than the interval, got %zu", confirm_poll_count(999));
}

/**
 * @brief A look that only ever comes back healthy needs no second look
 */
static void test_confirm_stops_at_the_first_healthy_look(void) {
  char devnode[] = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct device_target target;
  struct repair_result r;
  struct confirm_ctx ctx;

  memset(&target, 0, sizeof(target));
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  memset(&r, 0, sizeof(r));
  memset(&ctx, 0, sizeof(ctx));
  ctx.s.confirm_max_wait_ms = 5000;
  ctx.states[0] = DEVICE_STATE_HEALTHY;
  ctx.count = 1;

  TEST_CHECK(confirm_through_fake(&target, &r, &ctx) == REPAIR_OUTCOME_CLOSED);
  TEST_CHECK_(ctx.calls == 1, "want a single look, got %zu", ctx.calls);
}

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

  TEST_SUCCEEDED(repair_run(&s, NULL, true, true, NULL, NULL, &cancel, &r, &err), &err);
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
    TEST_SUCCEEDED(repair_run(&s, NULL, true, true, raise_cancel_progress, &ctx, &cancel, &r2, &err2), &err2);
    TEST_CHECK(r2.outcome == REPAIR_OUTCOME_ABORTED);
    repair_result_release(&r2);
  }

  TEST_CASE("no flag at all runs as before");
  {
    struct repair_result r3;
    struct ov_error err3 = {0};

    memset(&r3, 0, sizeof(r3));
    TEST_SUCCEEDED(repair_run(&s, NULL, true, true, NULL, NULL, NULL, &r3, &err3), &err3);
    TEST_CHECK(r3.outcome == REPAIR_OUTCOME_ABSENT);
    repair_result_release(&r3);
  }

  repair_result_release(&r);
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
  if (!TEST_CHECK(strcmp(msg, "The device is still in use because \"Close the Audient mixer automatically\" is turned off.") == 0)) {
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
  TEST_CHECK(strcmp(label, "Close the Audient mixer automatically") == 0);
  TEST_CHECK(strcmp(repair_option_audiodg_label(), "Close audiodg (audiodg.exe) automatically") == 0);
}

/**
 * @brief A second veto after the blockers were closed is answered, not a crash
 *
 * The removal is asked for twice when the first one is vetoed: the blockers the record
 * named are closed and the removal is asked again.  Both refusals are a veto, and the
 * error of the first one is spent before the second one raises its own; a run that kept it
 * would have no room for the second and would stop on the spot.
 */
static void test_second_veto_is_answered(void) {
  char service_name[] = "adc-repair-test-no-such-service";
  char devnode[] = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_ctx run = {0};
  struct repair_deps deps = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(&g_retry, 0, sizeof(g_retry));
  s.ks_service = service_name;
  s.confirm_max_wait_ms = 1000;
  {
    static char const mixer_name[] = "iD.exe";
    char *name = NULL;

    if (!OV_REALLOC(&name, sizeof(mixer_name), sizeof(name[0]))) {
      goto cleanup;
    }
    memcpy(name, mixer_name, sizeof(mixer_name));
    if (!TEST_CHECK(OV_ARRAY_PUSH(&s.mixer_process_names, name))) {
      OV_FREE(&name);
      goto cleanup;
    }
  }
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.allow_mixer_close = true;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;
  deps.removal_close = fake_vetoed_removal;
  deps.read_veto = fake_read_veto;
  deps.names_ancestor = fake_names_ancestor;
  deps.mixer_take = fake_mixer_take;
  deps.mixer_close = fake_mixer_close;
  deps.mixer_target_release = fake_mixer_target_release;
  deps.probe_devnode = fake_probe_healthy;
  g_retry.veto_records = 1;

  TEST_CASE("both removals are refused, the run answers instead of crashing");
  outcome = repair_target(&run, &deps, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(g_retry.removal_calls == 2, "want two removal attempts, got %zu", g_retry.removal_calls);
  TEST_CHECK_(g_retry.mixer_closed == 1, "want the blocker closed once, got %zu", g_retry.mixer_closed);

  TEST_CASE("the removal succeeds on the second try");
  memset(&g_retry, 0, sizeof(g_retry));
  memset(&r, 0, sizeof(r));
  g_retry.veto_records = 1;
  g_retry.removal_recovers = true;
  outcome = repair_target(&run, &deps, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_CLOSED, "want the closed outcome, got %d", (int)outcome);
  TEST_CHECK_(g_retry.removal_calls == 2, "want two removal attempts, got %zu", g_retry.removal_calls);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  repair_result_release(&r);
  if (s.mixer_process_names != NULL) {
    for (size_t i = 0; i < OV_ARRAY_LENGTH(s.mixer_process_names); i++) {
      OV_FREE(&s.mixer_process_names[i]);
    }
    OV_ARRAY_DESTROY(&s.mixer_process_names);
  }
}

/**
 * @brief More blockers than the run may close is a failure, not "still in use"
 *
 * The records may name more processes than the run carries room for.  That is a failure of
 * its own and not the same answer as a device that no record named: a caller that reads
 * the two as one would say the device is still in use and stop without saying why.
 */
static void test_too_many_blockers_is_a_failure(void) {
  char service_name[] = "adc-repair-test-no-such-service";
  char devnode[] = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_ctx run = {0};
  struct repair_deps deps = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(&g_retry, 0, sizeof(g_retry));
  s.ks_service = service_name;
  s.confirm_max_wait_ms = 1000;
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.allow_audiodg_close = true;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;
  deps.removal_close = fake_vetoed_removal;
  deps.read_veto = fake_read_veto;
  deps.names_ancestor = fake_names_ancestor;
  deps.mixer_take = fake_mixer_take;
  deps.mixer_close = fake_mixer_close;
  deps.mixer_target_release = fake_mixer_target_release;
  deps.probe_devnode = fake_probe_healthy;
  g_retry.veto_records = REPAIR_MAX_BLOCKERS + 1;
  g_retry.image = "\\Device\\HarddiskVolume2\\Windows\\System32\\audiodg.exe";

  TEST_CASE("more blockers than the run carries room for");
  outcome = repair_target(&run, &deps, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_FAILED, "want the failed outcome, got %d", (int)outcome);
  TEST_CHECK_(ov_error_is(&err, ov_error_type_generic, ov_error_generic_fail), "want a reason for the failure");

  OV_ERROR_REPORT(&err, NULL);
  repair_result_release(&r);
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
    {"confirm_polls_until_the_device_comes_back", test_confirm_polls_until_the_device_comes_back},
    {"confirm_stops_at_the_first_healthy_look", test_confirm_stops_at_the_first_healthy_look},
    {"confirm_poll_count_from_the_wait", test_confirm_poll_count_from_the_wait},
    {"confirm_survives_a_failed_look", test_confirm_survives_a_failed_look},
    {"second_veto_is_answered", test_second_veto_is_answered},
    {"too_many_blockers_is_a_failure", test_too_many_blockers_is_a_failure},
    {NULL, NULL},
};
