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
  size_t removal_calls;   //!< how often the removal was asked for
  bool removal_recovers;  //!< the removal succeeds from this call on
  size_t veto_records;    //!< how many records the veto read hands back
  size_t veto_reads;      //!< how often the veto was read
  uint32_t second_pid;    //!< the pid the second read names, 0 to keep the first ones
  size_t mixer_closed;    //!< how many blockers were closed
  size_t service_paused;  //!< how often the audio service was paused
  size_t service_resumed; //!< how often the audio service was started again
  bool resume_fails;      //!< the service refuses to start again
  char const *image;      //!< the image the blockers report, NULL for the mixer of the run
  bool mixer_seen;        //!< whether a blocker the run may close was seen
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
 * @brief Hand back the records of a veto, made the way veto_event_read makes them
 */
static struct veto_event *
fake_read_veto(char const *const log_name, char const *const provider_name, unsigned short const event_id, struct ov_error *const err) {
  struct veto_event *list = NULL;
  struct veto_event c = {0};

  (void)log_name;
  (void)provider_name;
  (void)event_id;
  (void)err;
  g_retry.veto_reads++;
  if ((g_retry.second_pid != 0) && (g_retry.veto_reads >= 2)) {
    // the second read is the one of the second attempt: the record that named the process
    // this run closed is history, and what blocks the device now is somebody else
    static char const instance[] = "USB\\VID_2708&PID_0008\\6&deadbee&0&2";

    if (!OV_REALLOC(&c.device_instance, sizeof(instance), sizeof(c.device_instance[0]))) {
      goto cleanup;
    }
    memcpy(c.device_instance, instance, sizeof(instance));
    c.pid = g_retry.second_pid;
    c.event_time_ms = process_now_ms() + 1000;
    if (!OV_ARRAY_PUSH(&list, c)) {
      goto cleanup;
    }
    return list;
  }
  for (size_t i = 0; i < g_retry.veto_records; i++) {
    static char const instance[] = "USB\\VID_2708&PID_0008\\6&deadbee&0&2";

    if (!OV_REALLOC(&c.device_instance, sizeof(instance), sizeof(c.device_instance[0]))) {
      goto cleanup;
    }
    memcpy(c.device_instance, instance, sizeof(instance));
    c.pid = 100 + (uint32_t)i;
    c.event_time_ms = process_now_ms() + 1000; // newer than the attempt, so the prune keeps it
    if (!OV_ARRAY_PUSH(&list, c)) {
      OV_FREE(&c.device_instance);
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
 * @brief The veto records name processes that are running, one per record
 *
 * The records carry no image of their own: what the process is is read from the machine,
 * which the fake stands in for.  The kind of each entry follows g_retry.image.
 */
static struct blocker *fake_blocker_list_build(struct veto_event const *const events,
                                               char const *const *const mixer_names,
                                               size_t const mixer_name_count,
                                               struct ov_error *const err) {
  struct blocker *list = NULL;
  size_t const n = veto_event_count(events);
  char const *const name = g_retry.image ? g_retry.image : "iD.exe";
  size_t i = 0;

  (void)err;
  for (i = 0; i < n; i++) {
    struct blocker b;

    memset(&b, 0, sizeof(b));
    b.pid = events[i].pid;
    b.event_time_ms = events[i].event_time_ms;
    b.kind = ((mixer_name_count > 0) && (mixer_names != NULL) && process_path_matches_name(name, mixer_names[0])) ? BLOCKER_KIND_MIXER
                                                                                                                  : BLOCKER_KIND_AUDIODG;
    if (!OV_ARRAY_PUSH(&list, b)) {
      goto cleanup;
    }
  }
  if ((list == NULL) && !OV_ARRAY_GROW(&list, 1)) {
    goto cleanup;
  }
  return list;

cleanup:
  blocker_list_destroy(&list);
  return NULL;
}

/**
 * @brief The blocker goes when the run asks it to
 */
static bool fake_blocker_close(struct blocker const *const b, bool const allow_privilege, struct ov_error *const err) {
  (void)b;
  (void)allow_privilege;
  (void)err;
  g_retry.mixer_closed++;
  return true;
}

/**
 * @brief The audio service is paused when the run asks for it
 */
static bool fake_audio_service_pause(struct ov_error *const err) {
  (void)err;
  g_retry.service_paused++;
  return true;
}

/**
 * @brief The audio service is started again when the run asks for it
 */
static bool fake_audio_service_resume(struct ov_error *const err) {
  g_retry.service_resumed++;
  if (g_retry.resume_fails) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  return true;
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
 * @brief The kind of a blocker is what the lookup decided
 *
 * The kind is not read off a path at the time of the close: it is settled when the machine
 * is asked what the process is, so the run reads it from the entry and nothing else.
 */
static void test_blocker_kind(void) {
  struct blocker *blockers = NULL;
  struct blocker b;

  memset(&b, 0, sizeof(b));
  b.pid = 100;
  b.kind = BLOCKER_KIND_MIXER;
  if (!TEST_CHECK(OV_ARRAY_PUSH(&blockers, b))) {
    goto cleanup;
  }
  b.pid = 200;
  b.kind = BLOCKER_KIND_AUDIODG;
  TEST_CHECK(OV_ARRAY_PUSH(&blockers, b));
  b.pid = 300;
  TEST_CHECK(OV_ARRAY_PUSH(&blockers, b));

  TEST_CASE("the count follows the entries");
  TEST_CHECK(blocker_count(blockers) == 3);
  TEST_CHECK(blocker_mixer_count(blockers) == 1);

  TEST_CASE("an empty list has no mixer");
  TEST_CHECK(blocker_mixer_count(NULL) == 0);

  TEST_CASE_(NULL);

cleanup:
  blocker_list_destroy(&blockers);
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
  TEST_CHECK(strcmp(repair_option_audiodg_label(), "Allow pausing the Windows Audio service") == 0);
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
  deps.blocker_list_build = fake_blocker_list_build;
  deps.blocker_close = fake_blocker_close;
  deps.probe_devnode = fake_probe_healthy;
  g_retry.veto_records = 1;

  TEST_CASE("both removals are refused, the run answers instead of crashing");
  outcome = repair_target(&run, &deps, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(g_retry.removal_calls == 2, "want two removal attempts, got %zu", g_retry.removal_calls);
  TEST_CHECK_(g_retry.mixer_closed == 1, "want the blocker closed once, got %zu", g_retry.mixer_closed);

  TEST_CASE("the removal succeeds on the second try");
  memset(&g_retry, 0, sizeof(g_retry));
  repair_result_release(&r);
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
 * @brief A blocker the user did not allow is left alone and named
 *
 * The run may only close what the settings and the two checkboxes allow.  A device that is
 * held by a process the user kept it away from is still in use, and the sentence says which
 * setting would have helped instead of the run stopping without a reason.
 */
static void test_blocker_not_allowed_is_left_alone(void) {
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
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;
  deps.removal_close = fake_vetoed_removal;
  deps.read_veto = fake_read_veto;
  deps.names_ancestor = fake_names_ancestor;
  deps.blocker_list_build = fake_blocker_list_build;
  deps.blocker_close = fake_blocker_close;
  deps.probe_devnode = fake_probe_healthy;
  g_retry.veto_records = 1;

  TEST_CASE("the mixer is allowed, so it is closed");
  run.allow_mixer_close = true;
  outcome = repair_target(&run, &deps, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(g_retry.mixer_closed == 1, "want the blocker closed once, got %zu", g_retry.mixer_closed);
  TEST_CHECK_(g_retry.removal_calls == 2, "want two removal attempts, got %zu", g_retry.removal_calls);
  TEST_CHECK_(g_retry.veto_reads == 2, "want the veto read once per attempt, got %zu", g_retry.veto_reads);

  TEST_CASE("nobody allowed the mixer, so nothing is closed and the run says so");
  memset(&g_retry, 0, sizeof(g_retry));
  repair_result_release(&r);
  memset(&r, 0, sizeof(r));
  g_retry.veto_records = 1;
  run.allow_mixer_close = false;
  outcome = repair_target(&run, &deps, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(g_retry.mixer_closed == 0, "want nothing closed, got %zu", g_retry.mixer_closed);
  TEST_CHECK_(g_retry.removal_calls == 1, "want a single removal attempt, got %zu", g_retry.removal_calls);
  TEST_CHECK(veto_event_count(r.events) == 1);

  TEST_CASE("the second attempt names somebody else, and that one is shown");
  memset(&g_retry, 0, sizeof(g_retry));
  repair_result_release(&r);
  memset(&r, 0, sizeof(r));
  g_retry.veto_records = 1;
  g_retry.second_pid = 700; // what the records of the second attempt name
  run.allow_mixer_close = true;
  run.allow_audiodg_close = true;
  outcome = repair_target(&run, &deps, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(g_retry.veto_reads == 2, "want the veto read once per attempt, got %zu", g_retry.veto_reads);
  if (TEST_CHECK(veto_event_count(r.events) == 1)) {
    TEST_CHECK_(r.events[0].pid == 700, "want the record of the second attempt, got %lu", (unsigned long)r.events[0].pid);
  }

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
 * @brief The audio engine is answered by pausing the service that starts it, and it always comes back
 *
 * Terminating audiodg is useless, because Windows starts a new one within milliseconds.  The
 * service that starts it is paused instead, and it has to be started again whatever else
 * happened, or the machine is left without sound.
 */
static void test_audiodg_is_answered_by_pausing_the_service(void) {
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
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;
  run.allow_audiodg_close = true;
  run.can_pause_audio_service = true;
  deps.removal_close = fake_vetoed_removal;
  deps.read_veto = fake_read_veto;
  deps.names_ancestor = fake_names_ancestor;
  deps.blocker_list_build = fake_blocker_list_build;
  deps.blocker_close = fake_blocker_close;
  deps.audio_service_pause = fake_audio_service_pause;
  deps.audio_service_resume = fake_audio_service_resume;
  deps.probe_devnode = fake_probe_healthy;
  g_retry.veto_records = 1;
  g_retry.image = "audiodg.exe"; // the records name the audio engine

  TEST_CASE("the audio engine is there, so the service is paused and started again");
  run.allow_mixer_close = true;
  outcome = repair_target(&run, &deps, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(g_retry.service_paused == 1, "want the service paused once, got %zu", g_retry.service_paused);
  TEST_CHECK_(g_retry.service_resumed == 1, "want the service started again once, got %zu", g_retry.service_resumed);
  TEST_CHECK_(g_retry.mixer_closed == 0, "want no mixer closed, got %zu", g_retry.mixer_closed);
  TEST_CHECK_(g_retry.removal_calls == 2, "want two removal attempts, got %zu", g_retry.removal_calls);

  TEST_CASE("nobody allowed it, so the service is left alone");
  memset(&g_retry, 0, sizeof(g_retry));
  repair_result_release(&r);
  memset(&r, 0, sizeof(r));
  g_retry.veto_records = 1;
  g_retry.image = "audiodg.exe";
  run.allow_audiodg_close = false;
  outcome = repair_target(&run, &deps, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(g_retry.service_paused == 0, "want nothing paused, got %zu", g_retry.service_paused);
  TEST_CHECK_(g_retry.service_resumed == 0, "want nothing to start again, got %zu", g_retry.service_resumed);
  TEST_CHECK_(g_retry.removal_calls == 1, "want a single removal attempt, got %zu", g_retry.removal_calls);

  TEST_CASE("the service does not come back: the run says so as a failure");
  memset(&g_retry, 0, sizeof(g_retry));
  repair_result_release(&r);
  memset(&r, 0, sizeof(r));
  g_retry.veto_records = 1;
  g_retry.image = "audiodg.exe";
  g_retry.resume_fails = true;
  run.allow_audiodg_close = true;
  outcome = repair_target(&run, &deps, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_FAILED, "want the failed outcome, got %d", (int)outcome);
  TEST_CHECK_(g_retry.service_resumed == 1, "want the start to be tried once, got %zu", g_retry.service_resumed);

  OV_ERROR_REPORT(&err, NULL);
  repair_result_release(&r);
}

TEST_LIST = {
    {"path_matches_name", test_path_matches_name},
    {"blocker_kind", test_blocker_kind},
    {"run_requires_arguments", test_run_requires_arguments},
    {"release_null", test_release_null},
    {"result_sentence", test_result_sentence},
    {"blocked_sentence", test_blocked_sentence},
    {"option_labels", test_option_labels_are_named_once},
    {"cancel_stops_the_run", test_cancel_stops_the_run},
    {"confirm_polls_until_the_device_comes_back", test_confirm_polls_until_the_device_comes_back},
    {"confirm_stops_at_the_first_healthy_look", test_confirm_stops_at_the_first_healthy_look},
    {"confirm_poll_count_from_the_wait", test_confirm_poll_count_from_the_wait},
    {"confirm_survives_a_failed_look", test_confirm_survives_a_failed_look},
    {"second_veto_is_answered", test_second_veto_is_answered},
    {"blocker_not_allowed_is_left_alone", test_blocker_not_allowed_is_left_alone},
    {"audiodg_is_answered_by_pausing_the_service", test_audiodg_is_answered_by_pausing_the_service},
    {NULL, NULL},
};
