#include "repair.h"

#include "repair.c"

#include <ovtest.h>

#include <string.h>

#include <ovarray.h>
#include <ovprintf_ex.h>

#include "device_manager.h"
#include "device_probe.h"
#include "mock/clock.h"
#include "mock/device.h"
#include "mock/process.h"
#include "mock/removal.h"
#include "mock/veto_log.h"
#include "settings.h"
#include "simulate.h"
#include "veto_log_manager.h"

/** @brief The device above the KS devnode the stand-in machines describe */
static wchar_t const kUsbInstance[] = L"USB\\VID_2708&PID_0008\\6&deadbee&0&2";

/**
 * @brief How long a run waits for a device to come back
 *
 * The stand-in answers at once, so the bound only has to be long enough for a look or two:
 * a wait that runs it out is what a device that never comes back costs, and a second of
 * that in every test is a second nobody is waiting for.
 */
enum { adc_test_confirm_max_wait_ms = 100 };

/**
 * @brief How a wait on the devnode of the script ends
 */
enum watch_script {
  WATCH_COMES_BACK, //!< the device reports itself back from the second look on
  WATCH_STAYS_AWAY, //!< it never does, so the bound ends the wait
  WATCH_LOOK_FAILS, //!< a look of the wait cannot be taken
};

/**
 * @brief Put stand-in machines in place that describe one KS devnode under kUsbInstance
 *
 * A record of a veto names the device above the devnode, so the walk that asks whether a
 * record belongs to the device the run repairs reads the parent of the devnode.  The removal
 * is refused as often as the caller says before it is let through.
 *
 * A run also asks for the machine to have its sound back at the end of every run, so a
 * stand-in process machine with no process in it is put in place as well: without it a test
 * that runs a repair would ask the machine of the system to start the audio service.
 *
 * @param service the service the machine is asked for
 * @param devnode the KS devnode the machine has
 * @param watch how a wait on the devnode ends
 * @param refusals how many removals are refused before one is let through
 * @return false when the stand-in could not be put in place
 */
static bool
install_machine(wchar_t const *const service, wchar_t const *const devnode, enum watch_script const watch, size_t const refusals) {
  static struct device_mock_node nodes[1];
  static struct device_mock_script devices;
  static struct removal_mock_script removals;
  static struct process_mock_entry no_entries[1];
  static struct process_mock_script no_processes;
  struct device_facts const away = {.problem_code = 0, .endpoint_present = false, .endpoint_registered = true};
  struct device_facts const back = {.problem_code = 0, .endpoint_present = true, .endpoint_registered = true};

  nodes[0] = (struct device_mock_node){
      .devnode = devnode,
      .parent = kUsbInstance,
      .facts = away,
      .restored = (watch == WATCH_COMES_BACK) ? back : away,
      .look_fails = (watch == WATCH_LOOK_FAILS),
  };
  devices = (struct device_mock_script){.service = service, .nodes = nodes, .node_count = 1};
  removals = (struct removal_mock_script){
      .refusals = refusals,
      .code = removal_error_veto,
      .vetoed_device = kUsbInstance,
  };
  no_processes = (struct process_mock_script){.entries = no_entries, .entry_count = 0};
  return device_mock_install(&devices) && removal_mock_install(&removals) && process_mock_install(&no_processes);
}

/**
 * @brief How many records the list holds
 *
 * The list is walked rather than asked, because that is the only way it is read.
 *
 * @param list the list to walk
 * @return the number of records
 */
static size_t event_list_length(struct veto_event const *const list) {
  struct veto_event_iter it = {0};
  size_t n = 0;

  while (veto_event_next(list, &it)) {
    n++;
  }
  return n;
}

/**
 * @brief Put a stand-in log in place that hands back the records of the caller
 *
 * The log is read once per removal attempt, so what the first read hands back is what the
 * first attempt is answered with.
 *
 * @param records what the log hands back
 * @param count how many records that is
 * @param second what a later read hands back, NULL to keep handing back the first ones
 * @param second_count how many records that is
 * @return false when the stand-in could not be put in place
 */
static bool install_log(struct veto_log_mock_record const *const records,
                        size_t const count,
                        struct veto_log_mock_record const *const second,
                        size_t const second_count) {
  static struct veto_log_mock_script log;

  log = (struct veto_log_mock_script){.records = records, .record_count = count, .second_read = second, .second_read_count = second_count};
  return veto_log_mock_install(&log);
}

/**
 * @brief Put a stand-in machine in place that has the processes of the caller
 *
 * @param entries the processes, walked in place so the stand-in can mark them gone
 * @param count how many there are
 * @param resume_fails whether the audio service refuses to start again
 * @return false when the stand-in could not be put in place
 */
static bool install_processes(struct process_mock_entry *const entries, size_t const count, bool const resume_fails) {
  static struct process_mock_script processes;

  processes = (struct process_mock_script){.entries = entries, .entry_count = count, .resume_fails = resume_fails};
  return process_mock_install(&processes);
}

/**
 * @brief Go back to the machines of the system
 *
 * The machines a run goes through are one for the whole program, so a test puts them back
 * when it is over and leaves nothing of its own behind for the next one.
 */
static void uninstall_mocks(void) {
  device_mock_reset();
  removal_mock_reset();
  veto_log_mock_reset();
  process_mock_reset();
  clock_mock_reset();
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
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  struct settings s;
  struct repair_result r;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  s.ks_service = service_name;

  TEST_CASE("a run with nothing to release");
  if (TEST_SUCCEEDED(repair_run(&s, NULL, true, true, NULL, NULL, NULL, &r, &err), &err)) {
    TEST_CHECK(r.outcome == REPAIR_OUTCOME_ABSENT);
    if (!TEST_CHECK(strcmp(r.message, "No Audient device was found.") == 0)) {
      TEST_MSG("want a system message, got [%hs]", r.message);
    }
  }
  TEST_CASE_(NULL);

  repair_result_release(&r);
}

struct cancel_ctx {
  HANDLE cancel;
};

/**
 * @brief The progress callback that signals the cancel event at the first step
 *
 * @param stage unused, the first report is enough
 * @param step unused
 * @param total unused
 * @param userdata the context with the event
 */
static void raise_cancel_progress(enum repair_stage const stage, size_t const step, size_t const total, void *const userdata) {
  struct cancel_ctx *const ctx = (struct cancel_ctx *)userdata;

  (void)stage;
  (void)step;
  (void)total;
  SetEvent(ctx->cancel);
}

/**
 * @brief A cancel event stops the run before its next step
 *
 * The reader of the progress window can ask the run to stop; the run is not killed in the
 * middle of a step, it reads the event between the steps and ends with the outcome that
 * says it was aborted instead of walking on.  The event is a handle the caller owns, so
 * the window can signal it from another thread, and the wait for the veto record is
 * answered by it as well.
 */
static void test_cancel_stops_the_run(void) {
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  struct settings s;
  struct repair_result r;
  struct ov_error err = {0};
  HANDLE cancel = NULL;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  s.ks_service = service_name;
  cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!TEST_CHECK(cancel != NULL)) {
    goto cleanup;
  }
  SetEvent(cancel); // the ask is up before the run starts

  TEST_SUCCEEDED(repair_run(&s, NULL, true, true, NULL, NULL, cancel, &r, &err), &err);
  TEST_CHECK_(r.outcome == REPAIR_OUTCOME_ABORTED, "want the aborted outcome, got %d", (int)r.outcome);
  TEST_CHECK(strcmp(r.message, "The operation was aborted by the user.") == 0);

  TEST_CASE("an ask that goes up during the run");
  {
    struct cancel_ctx ctx;
    struct repair_result r2;
    struct ov_error err2 = {0};
    HANDLE during = CreateEventW(NULL, TRUE, FALSE, NULL);

    memset(&ctx, 0, sizeof(ctx));
    memset(&r2, 0, sizeof(r2));
    if (!TEST_CHECK(during != NULL)) {
      goto cleanup;
    }
    ctx.cancel = during;
    TEST_SUCCEEDED(repair_run(&s, NULL, true, true, raise_cancel_progress, &ctx, during, &r2, &err2), &err2);
    TEST_CHECK(r2.outcome == REPAIR_OUTCOME_ABORTED);
    repair_result_release(&r2);
    CloseHandle(during);
  }

  TEST_CASE("no ask at all runs as before");
  {
    struct repair_result r3;
    struct ov_error err3 = {0};

    memset(&r3, 0, sizeof(r3));
    TEST_SUCCEEDED(repair_run(&s, NULL, true, true, NULL, NULL, NULL, &r3, &err3), &err3);
    TEST_CHECK(r3.outcome == REPAIR_OUTCOME_ABSENT);
    repair_result_release(&r3);
  }

  repair_result_release(&r);

cleanup:
  if (cancel != NULL) {
    CloseHandle(cancel);
  }
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
  blocked_sentence(label, 1, msg, sizeof(msg));
  if (!TEST_CHECK(strcmp(msg, "The device is still in use because \"Close the Audient mixer automatically\" is turned off.") == 0)) {
    TEST_MSG("want the sentence to name the option, got [%hs]", msg);
  }

  TEST_CASE("the record names something else");
  blocked_sentence(NULL, 1, msg, sizeof(msg));
  TEST_CHECK(strcmp(msg, "The device is still in use by another process.") == 0);

  TEST_CASE("more than one blocker is a number, not one name");
  blocked_sentence(label, 3, msg, sizeof(msg));
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
  TEST_CHECK(strcmp(repair_option_audio_service_label(), "Allow pausing the Windows Audio service") == 0);
}

/**
 * @brief Take one device through its repair on this thread
 *
 * The stand-in lookup is answered as soon as it is asked for, so the wait between the two
 * pieces of a step is not a wait at all here.
 */
static NODISCARD enum repair_outcome
repair_target(struct repair_run *const run, struct device_target const *const target, struct ov_error *const err) {
  run->target = target;
  run->count = 1;
  run->index = 0;
  run->outcome = REPAIR_OUTCOME_CLOSED;
  run->phase = REPAIR_PHASE_TARGET;
  while (repair_step(run, err) != REPAIR_STEP_DONE) {
  }
  return run->target_outcome;
}

/**
 * @brief The confirmation answers what the watch of the device saw
 *
 * The looks themselves belong to the device layer and are replaced here, so what is checked
 * is what the run makes of the answer: an audio device again is the run that is over, and one
 * that stayed away is the run that failed.
 */
static void test_confirm_answers_the_watch(void) {
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;

  TEST_CASE("the device is an audio device again");
  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 0))) {
    goto cleanup;
  }
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_CLOSED, "want the closed outcome, got %d", (int)outcome);

  TEST_CASE("the device stayed away");
  repair_result_release(&r);
  memset(&r, 0, sizeof(r));
  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_STAYS_AWAY, 0))) {
    goto cleanup;
  }
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_FAILED, "want the failed outcome, got %d", (int)outcome);

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
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
  static struct veto_log_mock_record const records[] = {
      {.pid = 100, .event_time_ms = 1000, .device_instance = kUsbInstance},
  };
  static struct process_mock_entry entries[1];
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"iD.exe", .start_ms = 0, .ends = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  {
    static wchar_t const mixer_name[] = L"iD.exe";
    wchar_t *name = NULL;

    if (!OV_ARRAY_GROW(&name, sizeof(mixer_name) / sizeof(mixer_name[0]))) {
      goto cleanup;
    }
    OV_ARRAY_SET_LENGTH(name, (sizeof(mixer_name) / sizeof(mixer_name[0])) - 1);
    wcscpy(name, mixer_name);
    if (!TEST_CHECK(OV_ARRAY_PUSH(&s.mixer_process_names, name))) {
      if (name != NULL) {
        OV_ARRAY_DESTROY(&name);
      }
      goto cleanup;
    }
  }
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.allow_mixer_close = true;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;

  TEST_CASE("both removals are refused, the run answers instead of crashing");
  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 2) && install_log(records, 1, NULL, 0) &&
                  install_processes(entries, 1, false))) {
    goto cleanup;
  }
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(removal_mock_remove_count() == 2, "want two removal attempts, got %zu", removal_mock_remove_count());
  TEST_CHECK_(process_mock_end_count() == 1, "want the blocker closed once, got %zu", process_mock_end_count());

  TEST_CASE("the removal succeeds on the second try");
  repair_result_release(&r);
  memset(&r, 0, sizeof(r));
  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 1) && install_log(records, 1, NULL, 0) &&
                  install_processes(entries, 1, false))) {
    goto cleanup;
  }
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_CLOSED, "want the closed outcome, got %d", (int)outcome);
  TEST_CHECK_(removal_mock_remove_count() == 2, "want two removal attempts, got %zu", removal_mock_remove_count());

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
  if (s.mixer_process_names != NULL) {
    for (size_t i = 0; i < OV_ARRAY_LENGTH(s.mixer_process_names); i++) {
      OV_ARRAY_DESTROY(&s.mixer_process_names[i]);
    }
    OV_ARRAY_DESTROY(&s.mixer_process_names);
  }
}

/**
 * @brief An ask to stop that comes while a mixer is being closed ends the run as aborted
 *
 * The close is the longest step of a run, so it is where an ask is most likely to come.  The
 * process did not go, so the close did not succeed, but nothing failed either: the run stops
 * with the outcome the caller asked for, and leaves no failure for it to act on.
 */
static void test_ask_while_closing_aborts(void) {
  static struct veto_log_mock_record const records[] = {
      {.pid = 100, .event_time_ms = 1000, .device_instance = kUsbInstance},
  };
  static struct process_mock_entry entries[1];
  static struct process_mock_script processes;
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"iD.exe", .start_ms = 0, .ends = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  {
    static wchar_t const mixer_name[] = L"iD.exe";
    wchar_t *name = NULL;

    if (!OV_ARRAY_GROW(&name, sizeof(mixer_name) / sizeof(mixer_name[0]))) {
      goto cleanup;
    }
    OV_ARRAY_SET_LENGTH(name, (sizeof(mixer_name) / sizeof(mixer_name[0])) - 1);
    wcscpy(name, mixer_name);
    if (!TEST_CHECK(OV_ARRAY_PUSH(&s.mixer_process_names, name))) {
      if (name != NULL) {
        OV_ARRAY_DESTROY(&name);
      }
      goto cleanup;
    }
  }
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.allow_mixer_close = true;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;

  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 1) && install_log(records, 1, NULL, 0))) {
    goto cleanup;
  }
  processes = (struct process_mock_script){.entries = entries, .entry_count = 1, .terminate_stops = true};
  if (!TEST_CHECK(process_mock_install(&processes))) {
    goto cleanup;
  }
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_ABORTED, "want the aborted outcome, got %d", (int)outcome);
  TEST_CHECK_(r.outcome == REPAIR_OUTCOME_ABORTED, "want the result to say aborted, got %d", (int)r.outcome);
  TEST_CHECK_(r.failure == repair_failure_none, "want no failure, got %d", (int)r.failure);
  TEST_CHECK(strcmp(r.message, "The operation was aborted by the user.") == 0);
  TEST_MSG("want the aborted sentence, got %s", r.message);
  TEST_CHECK_(process_mock_end_count() == 1, "want the close asked for once, got %zu", process_mock_end_count());

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
  if (s.mixer_process_names != NULL) {
    for (size_t i = 0; i < OV_ARRAY_LENGTH(s.mixer_process_names); i++) {
      OV_ARRAY_DESTROY(&s.mixer_process_names[i]);
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
  static struct veto_log_mock_record const records[] = {
      {.pid = 100, .event_time_ms = 1000, .device_instance = kUsbInstance},
  };
  static struct veto_log_mock_record const later[] = {
      {.pid = 700, .event_time_ms = 1000, .device_instance = kUsbInstance},
  };
  static struct process_mock_entry entries[2];
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"iD.exe", .start_ms = 0, .ends = true};
  entries[1] = (struct process_mock_entry){.pid = 700, .image = L"iD.exe", .start_ms = 0, .ends = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  {
    static wchar_t const mixer_name[] = L"iD.exe";
    wchar_t *name = NULL;

    if (!OV_ARRAY_GROW(&name, sizeof(mixer_name) / sizeof(mixer_name[0]))) {
      goto cleanup;
    }
    OV_ARRAY_SET_LENGTH(name, (sizeof(mixer_name) / sizeof(mixer_name[0])) - 1);
    wcscpy(name, mixer_name);
    if (!TEST_CHECK(OV_ARRAY_PUSH(&s.mixer_process_names, name))) {
      if (name != NULL) {
        OV_ARRAY_DESTROY(&name);
      }
      goto cleanup;
    }
  }
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;

  TEST_CASE("the mixer is allowed, so it is closed");
  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 2) && install_log(records, 1, NULL, 0) &&
                  install_processes(entries, 2, false))) {
    goto cleanup;
  }
  run.allow_mixer_close = true;
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(process_mock_end_count() == 1, "want the blocker closed once, got %zu", process_mock_end_count());
  TEST_CHECK_(removal_mock_remove_count() == 2, "want two removal attempts, got %zu", removal_mock_remove_count());
  TEST_CHECK_(veto_log_mock_read_count() == 2, "want the veto read once per attempt, got %zu", veto_log_mock_read_count());

  TEST_CASE("nobody allowed the mixer, so nothing is closed and the run says so");
  repair_result_release(&r);
  memset(&r, 0, sizeof(r));
  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 2) && install_log(records, 1, NULL, 0) &&
                  install_processes(entries, 2, false))) {
    goto cleanup;
  }
  run.allow_mixer_close = false;
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(process_mock_end_count() == 0, "want nothing closed, got %zu", process_mock_end_count());
  TEST_CHECK_(removal_mock_remove_count() == 1, "want a single removal attempt, got %zu", removal_mock_remove_count());
  TEST_CHECK_(event_list_length(r.events) == 1, "want the one record, got %zu", event_list_length(r.events));

  TEST_CASE("the second attempt names somebody else, and that one is shown");
  repair_result_release(&r);
  memset(&r, 0, sizeof(r));
  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 2) && install_log(records, 1, later, 1) &&
                  install_processes(entries, 2, false))) {
    goto cleanup;
  }
  run.allow_mixer_close = true;
  run.allow_audio_service_pause = true;
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(veto_log_mock_read_count() == 2, "want the veto read once per attempt, got %zu", veto_log_mock_read_count());
  if (TEST_CHECK(event_list_length(r.events) == 1)) {
    struct veto_event_iter it = {0};

    TEST_ASSERT(veto_event_next(r.events, &it));
    TEST_CHECK_(it.pid == 700, "want the record of the second attempt, got %lu", (unsigned long)it.pid);
  }

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
  if (s.mixer_process_names != NULL) {
    for (size_t i = 0; i < OV_ARRAY_LENGTH(s.mixer_process_names); i++) {
      OV_ARRAY_DESTROY(&s.mixer_process_names[i]);
    }
    OV_ARRAY_DESTROY(&s.mixer_process_names);
  }
}

/**
 * @brief Pausing and resuming the service for an audio engine blocker
 *
 * Terminating the audio engine does not work: Windows starts it again and it takes the
 * device.  The service that starts it is paused instead, and it is started again whatever
 * else happened, or the machine is left without sound.
 */
static void test_pausing_and_resuming_the_service_for_an_audio_engine_blocker(void) {
  static struct veto_log_mock_record const records[] = {
      {.pid = 100, .event_time_ms = 1000, .device_instance = kUsbInstance},
  };
  static struct process_mock_entry entries[1];
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"audiodg.exe", .start_ms = 0, .ends = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;
  run.allow_audio_service_pause = true;
  run.can_pause_audio_service = true;

  TEST_CASE("the audio engine is there, so the service is paused and started again");
  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 2) && install_log(records, 1, NULL, 0) &&
                  install_processes(entries, 1, false))) {
    goto cleanup;
  }
  run.allow_mixer_close = true;
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(process_mock_service_pause_count() == 1, "want the service paused once, got %zu", process_mock_service_pause_count());
  TEST_CHECK_(
      process_mock_service_resume_count() == 1, "want the service started again once, got %zu", process_mock_service_resume_count());
  TEST_CHECK_(process_mock_end_count() == 0, "want no mixer closed, got %zu", process_mock_end_count());
  TEST_CHECK_(removal_mock_remove_count() == 2, "want two removal attempts, got %zu", removal_mock_remove_count());

  TEST_CASE("nobody allowed it, so the service is left alone");
  repair_result_release(&r);
  memset(&r, 0, sizeof(r));
  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 2) && install_log(records, 1, NULL, 0) &&
                  install_processes(entries, 1, false))) {
    goto cleanup;
  }
  run.allow_audio_service_pause = false;
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)outcome);
  TEST_CHECK_(process_mock_service_pause_count() == 0, "want nothing paused, got %zu", process_mock_service_pause_count());
  TEST_CHECK_(process_mock_service_resume_count() == 1,
              "want the service only asked to run at the end, got %zu",
              process_mock_service_resume_count());
  TEST_CHECK_(removal_mock_remove_count() == 1, "want a single removal attempt, got %zu", removal_mock_remove_count());

  TEST_CASE("the service does not come back: the run says so as a failure");
  repair_result_release(&r);
  memset(&r, 0, sizeof(r));
  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 2) && install_log(records, 1, NULL, 0) &&
                  install_processes(entries, 1, true))) {
    goto cleanup;
  }
  run.allow_audio_service_pause = true;
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_FAILED, "want the failed outcome, got %d", (int)outcome);
  TEST_CHECK_(process_mock_service_resume_count() == 1, "want the start to be tried once, got %zu", process_mock_service_resume_count());
  TEST_FAILED_WITH(outcome != REPAIR_OUTCOME_FAILED, &err, ov_error_type_generic, ov_error_generic_fail);

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
}

/**
 * @brief A service that could not be stopped is still asked to come back
 *
 * The stop is a control that is sent before the service reports anything, so an act that
 * failed may have left the service on its way down: a machine with no sound is worse than
 * the device this run was about, so the start is asked for whatever came of the stop.
 */
static void test_a_pause_that_failed_still_starts_the_service(void) {
  static struct veto_log_mock_record const records[] = {
      {.pid = 100, .event_time_ms = 1000, .device_instance = kUsbInstance},
  };
  static struct process_mock_entry entries[1];
  static struct process_mock_script processes;
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"audiodg.exe", .start_ms = 0, .ends = true};
  processes = (struct process_mock_script){.entries = entries, .entry_count = 1, .pause_fails = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;
  run.allow_audio_service_pause = true;
  run.can_pause_audio_service = true;

  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 2) && install_log(records, 1, NULL, 0) &&
                  process_mock_install(&processes))) {
    goto cleanup;
  }
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_FAILED, "want the failed outcome, got %d", (int)outcome);
  TEST_CHECK_(process_mock_service_pause_count() == 1, "want the stop asked for once, got %zu", process_mock_service_pause_count());
  TEST_CHECK_(process_mock_service_resume_count() == 1, "want the start asked for once, got %zu", process_mock_service_resume_count());
  TEST_FAILED_WITH(outcome != REPAIR_OUTCOME_FAILED, &err, ov_error_type_generic, ov_error_generic_fail);

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
}

/**
 * @brief A run that never stopped the service still asks for it to be running
 *
 * The tool is used because the sound of the machine is wanted, so a service the run found
 * stopped is one it has to bring back even when it did not stop it itself: a user left
 * without audio has gained nothing from a device that was released.
 */
static void test_a_run_that_never_paused_still_asks_for_the_service(void) {
  static struct process_mock_entry entries[1];
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"iD.exe", .start_ms = 0, .ends = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;

  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 0) && install_processes(entries, 1, false))) {
    goto cleanup;
  }
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_CLOSED, "want the closed outcome, got %d", (int)outcome);
  TEST_CHECK_(process_mock_service_pause_count() == 0, "want nothing paused, got %zu", process_mock_service_pause_count());
  TEST_CHECK_(process_mock_service_resume_count() == 1, "want the service asked for once, got %zu", process_mock_service_resume_count());

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
}

/**
 * @brief A failure stops the run where it happened
 *
 * The run is cut into steps with a wait between them, so a failure has to end it there
 * rather than be carried along: a later step writes its own reason into the same place,
 * and the one that stopped the run would be lost.  What was already done is still cleaned
 * up, and the sound of the machine is still asked for.
 *
 * The devices of the run are stood in for, so the device the run does not reach is what
 * tells the run apart from one that went on.
 */
static void test_a_failure_stops_the_run(void) {
  static struct veto_log_mock_record const records[] = {
      {.pid = 100, .event_time_ms = 1000, .device_instance = kUsbInstance},
  };
  static struct process_mock_entry entries[1];
  static struct process_mock_script processes;
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct device_target targets[2];
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&targets, 0, sizeof(targets));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"iD.exe", .start_ms = 0, .ends = true};
  processes = (struct process_mock_script){.entries = entries, .entry_count = 1, .open_fails = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  {
    static wchar_t const mixer_name[] = L"iD.exe";
    wchar_t *name = NULL;

    if (!OV_ARRAY_GROW(&name, sizeof(mixer_name) / sizeof(mixer_name[0]))) {
      goto cleanup;
    }
    OV_ARRAY_SET_LENGTH(name, (sizeof(mixer_name) / sizeof(mixer_name[0])) - 1);
    wcscpy(name, mixer_name);
    if (!TEST_CHECK(OV_ARRAY_PUSH(&s.mixer_process_names, name))) {
      if (name != NULL) {
        OV_ARRAY_DESTROY(&name);
      }
      goto cleanup;
    }
  }
  targets[0].devnode = devnode;
  targets[0].state = DEVICE_STATE_FAULTED;
  targets[1] = targets[0];
  run.settings = &s;
  run.allow_mixer_close = true;
  run.out = &r;
  run.list = targets;
  run.count = 2;
  run.index = 0;
  run.target = &targets[0];
  run.outcome = REPAIR_OUTCOME_CLOSED;
  run.phase = REPAIR_PHASE_TARGET;
  run.total = 2 * REPAIR_STEPS_PER_TARGET;

  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 2) && install_log(records, 1, NULL, 0) &&
                  process_mock_install(&processes))) {
    goto cleanup;
  }
  while (repair_step(&run, &err) != REPAIR_STEP_DONE) {
  }
  TEST_CHECK_(r.outcome == REPAIR_OUTCOME_FAILED, "want the failed outcome, got %d", (int)r.outcome);
  TEST_CHECK_(removal_mock_remove_count() == 1, "want the second device left alone, got %zu removal(s)", removal_mock_remove_count());
  TEST_CHECK_(process_mock_service_resume_count() == 1, "want the service asked for once, got %zu", process_mock_service_resume_count());
  TEST_FAILED_WITH(r.outcome != REPAIR_OUTCOME_FAILED, &err, ov_error_type_generic, ov_error_generic_fail);

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
  if (s.mixer_process_names != NULL) {
    for (size_t i = 0; i < OV_ARRAY_LENGTH(s.mixer_process_names); i++) {
      OV_ARRAY_DESTROY(&s.mixer_process_names[i]);
    }
    OV_ARRAY_DESTROY(&s.mixer_process_names);
  }
}

/**
 * @brief A record list handed to the caller is let go when the next device hands over its own
 *
 * The caller shows one list, so a run of several devices hands the last one over; what the
 * device before it named still has to be released rather than dropped.
 */
static void test_the_records_of_an_earlier_device_are_let_go(void) {
  static struct veto_log_mock_record const records[] = {
      {.pid = 100, .event_time_ms = 1000, .device_instance = kUsbInstance},
  };
  static struct process_mock_entry entries[1];
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct device_target targets[2];
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&targets, 0, sizeof(targets));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"iD.exe", .start_ms = 0, .ends = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  {
    static wchar_t const mixer_name[] = L"iD.exe";
    wchar_t *name = NULL;

    if (!OV_ARRAY_GROW(&name, sizeof(mixer_name) / sizeof(mixer_name[0]))) {
      goto cleanup;
    }
    OV_ARRAY_SET_LENGTH(name, (sizeof(mixer_name) / sizeof(mixer_name[0])) - 1);
    wcscpy(name, mixer_name);
    if (!TEST_CHECK(OV_ARRAY_PUSH(&s.mixer_process_names, name))) {
      if (name != NULL) {
        OV_ARRAY_DESTROY(&name);
      }
      goto cleanup;
    }
  }
  targets[0].devnode = devnode;
  targets[0].state = DEVICE_STATE_FAULTED;
  targets[1] = targets[0];
  run.settings = &s;
  run.out = &r;
  run.list = targets;
  run.count = 2;
  run.index = 0;
  run.target = &targets[0];
  run.outcome = REPAIR_OUTCOME_CLOSED;
  run.phase = REPAIR_PHASE_TARGET;
  run.total = 2 * REPAIR_STEPS_PER_TARGET;

  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 2) && install_log(records, 1, NULL, 0) &&
                  install_processes(entries, 1, false))) {
    goto cleanup;
  }
  while (repair_step(&run, &err) != REPAIR_STEP_DONE) {
  }
  TEST_CHECK_(r.outcome == REPAIR_OUTCOME_VETOED, "want the vetoed outcome, got %d", (int)r.outcome);
  TEST_CHECK_(removal_mock_remove_count() == 2, "want both devices asked for, got %zu", removal_mock_remove_count());
  TEST_CHECK_(veto_log_mock_read_count() == 2, "want both refusals looked at, got %zu", veto_log_mock_read_count());
  TEST_CHECK_(event_list_length(r.events) == 1, "want the records of the last device, got %zu", event_list_length(r.events));

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
  if (s.mixer_process_names != NULL) {
    for (size_t i = 0; i < OV_ARRAY_LENGTH(s.mixer_process_names); i++) {
      OV_ARRAY_DESTROY(&s.mixer_process_names[i]);
    }
    OV_ARRAY_DESTROY(&s.mixer_process_names);
  }
}

/**
 * @brief A lookup that failed stops the run and is kept for the caller
 *
 * The records the lookup reads are what tell which process holds the device, so the repair
 * cannot go on without them: the reason is what the caller gets rather than a line in the
 * log that leaves the device looking as though it were in use.
 */
static void test_a_lookup_that_failed_stops_the_run(void) {
  static struct process_mock_entry entries[1];
  static struct veto_log_mock_script log;
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"iD.exe", .start_ms = 0, .ends = true};
  log = (struct veto_log_mock_script){.read_fails = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;

  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 1) && veto_log_mock_install(&log) &&
                  install_processes(entries, 1, false))) {
    goto cleanup;
  }
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_FAILED, "want the failed outcome, got %d", (int)outcome);
  TEST_CHECK_(veto_log_mock_read_count() == 1, "want the refusal looked at once, got %zu", veto_log_mock_read_count());
  TEST_CHECK_(removal_mock_remove_count() == 1, "want no second attempt, got %zu removal(s)", removal_mock_remove_count());
  TEST_FAILED_WITH(outcome != REPAIR_OUTCOME_FAILED, &err, ov_error_type_generic, ov_error_generic_fail);

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
}

/**
 * @brief A watch that could not look stops the run and is kept for the caller
 *
 * The device was released, and whether it is an audio device again is what the watch is
 * for: one that could not look leaves the run unable to tell, which is the caller's to know
 * about rather than a failure of the device.
 */
static void test_a_watch_that_failed_stops_the_run(void) {
  static struct process_mock_entry entries[1];
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"iD.exe", .start_ms = 0, .ends = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;

  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_LOOK_FAILS, 0) && install_processes(entries, 1, false))) {
    goto cleanup;
  }
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_FAILED, "want the failed outcome, got %d", (int)outcome);
  TEST_CHECK_(removal_mock_remove_count() == 1, "want the release asked for once, got %zu", removal_mock_remove_count());
  TEST_FAILED_WITH(outcome != REPAIR_OUTCOME_FAILED, &err, ov_error_type_generic, ov_error_generic_fail);

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
}

/**
 * @brief A removal that failed for another reason stops the run and is kept for the caller
 *
 * A refusal that is not a veto is not the answer the run works with, so the run stops: the
 * caller gets the code that says what to do about it as well as the reason.
 */
static void test_a_removal_that_failed_for_another_reason_stops_the_run(void) {
  static struct process_mock_entry entries[1];
  struct removal_mock_script removals;
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"iD.exe", .start_ms = 0, .ends = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;

  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 1))) {
    goto cleanup;
  }
  removals = (struct removal_mock_script){.refusals = 1, .code = removal_error_access_denied};
  if (!TEST_CHECK(removal_mock_install(&removals) && install_processes(entries, 1, false))) {
    goto cleanup;
  }
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_FAILED, "want the failed outcome, got %d", (int)outcome);
  TEST_CHECK_(r.removal_error == removal_error_access_denied, "want the code kept for the user, got %d", (int)r.removal_error);
  TEST_CHECK_(removal_mock_remove_count() == 1, "want no second attempt, got %zu removal(s)", removal_mock_remove_count());
  TEST_FAILED_WITH(outcome != REPAIR_OUTCOME_FAILED, &err, ov_error_type_removal, removal_error_access_denied);

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
}

/**
 * @brief A service that could not be started again fails the run
 *
 * The tool is used because the sound is wanted, and a machine whose audio service is stopped
 * has none: a device that came out well is no use to a user left without audio, so the run
 * is a failure and the reason is what the caller gets.
 */
static void test_a_service_that_could_not_start_fails_the_run(void) {
  static struct process_mock_entry entries[1];
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"iD.exe", .start_ms = 0, .ends = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;

  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 0) && install_processes(entries, 1, true))) {
    goto cleanup;
  }
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_FAILED, "want the failed outcome, got %d", (int)outcome);
  TEST_CHECK_(r.outcome == REPAIR_OUTCOME_FAILED, "want the run to report the failure, got %d", (int)r.outcome);
  TEST_CHECK_(process_mock_service_resume_count() == 1, "want the service asked for once, got %zu", process_mock_service_resume_count());
  TEST_FAILED_WITH(r.outcome != REPAIR_OUTCOME_FAILED, &err, ov_error_type_generic, ov_error_generic_fail);

cleanup:
  repair_result_release(&r);
  uninstall_mocks();
}

/**
 * @brief A failure the run stopped for first keeps the dialog the caller shows
 *
 * The sound of the machine is asked for whatever came of the device, so the run goes on to
 * the service even after it stopped for something else.  That later failure is often no more
 * than a consequence of the first, and letting it take the place of the first hides what the
 * user can do about the run: a device that could not be closed for want of the privilege is
 * then told to look at the audio service instead.
 */
static void test_a_failure_that_came_first_is_the_one_reported(void) {
  static struct process_mock_entry entries[1];
  struct removal_mock_script removals;
  wchar_t service_name[] = L"adc-repair-test-no-such-service";
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  struct settings s;
  struct repair_result r;
  struct repair_run run = {0};
  struct device_target target;
  struct ov_error err = {0};
  enum repair_outcome outcome;

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  memset(&target, 0, sizeof(target));
  memset(entries, 0, sizeof(entries));
  entries[0] = (struct process_mock_entry){.pid = 100, .image = L"iD.exe", .start_ms = 0, .ends = true};
  s.ks_service = service_name;
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  target.devnode = devnode;
  target.state = DEVICE_STATE_FAULTED;
  run.settings = &s;
  run.out = &r;
  run.total = REPAIR_STEPS_PER_TARGET;

  if (!TEST_CHECK(install_machine(service_name, devnode, WATCH_COMES_BACK, 1))) {
    goto cleanup;
  }
  removals = (struct removal_mock_script){.refusals = 1, .code = removal_error_access_denied};
  if (!TEST_CHECK(removal_mock_install(&removals) && install_processes(entries, 1, true))) {
    goto cleanup;
  }
  outcome = repair_target(&run, &target, &err);
  TEST_CHECK_(outcome == REPAIR_OUTCOME_FAILED, "want the failed outcome, got %d", (int)outcome);
  TEST_CHECK_(r.removal_error == removal_error_access_denied, "want the refusal kept for the user, got %d", (int)r.removal_error);
  TEST_CHECK_(r.failure == repair_failure_none, "want no later failure to take its place, got %d", (int)r.failure);
  TEST_CHECK_(ov_error_is(&err, ov_error_type_removal, removal_error_access_denied), "want the reason the run stopped");
  TEST_FAILED_WITH(outcome != REPAIR_OUTCOME_FAILED, &err, ov_error_type_generic, ov_error_generic_fail);
cleanup:
  repair_result_release(&r);
  uninstall_mocks();
}

/**
 * @brief A device a mixer holds comes back once the mixer is out of the way
 *
 * The machine itself is written here rather than taken from the simulation: what this checks
 * is what a run makes of a device that is stuck and that comes back once the process holding
 * it is closed, not what the simulation happens to put in place.
 */
static void test_a_held_device_comes_back(void) {
  struct settings s;
  struct repair_result r;
  struct device_target *targets = NULL;
  struct ov_error err = {0};

  memset(&s, 0, sizeof(s));
  memset(&r, 0, sizeof(r));
  s.ks_service = L"audientusbaudioks";
  s.confirm_max_wait_ms = adc_test_confirm_max_wait_ms;
  {
    static wchar_t const mixer_name[] = L"iD.exe";
    wchar_t *name = NULL;

    if (!OV_ARRAY_GROW(&name, sizeof(mixer_name) / sizeof(mixer_name[0]))) {
      goto cleanup;
    }
    OV_ARRAY_SET_LENGTH(name, (sizeof(mixer_name) / sizeof(mixer_name[0])) - 1);
    wcscpy(name, mixer_name);
    if (!TEST_CHECK(OV_ARRAY_PUSH(&s.mixer_process_names, name))) {
      if (name != NULL) {
        OV_ARRAY_DESTROY(&name);
      }
      goto cleanup;
    }
  }
  if (!TEST_CHECK(OV_ARRAY_GROW(&targets, 1))) {
    goto cleanup;
  }
  OV_ARRAY_SET_LENGTH(targets, 1);
  memset(&targets[0], 0, sizeof(targets[0]));
  targets[0].devnode = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";
  targets[0].state = DEVICE_STATE_FAULTED;

  if (!TEST_CHECK(install_machine(s.ks_service, targets[0].devnode, WATCH_COMES_BACK, 1))) {
    goto cleanup;
  }
  {
    static struct veto_log_mock_record records[1];

    records[0] = (struct veto_log_mock_record){
        .pid = 4000, .event_time_ms = 1500, .path = L"\\Device\\HarddiskVolume1\\Apps\\iD.exe", .device_instance = kUsbInstance};
    if (!TEST_CHECK(install_log(records, 1, NULL, 0))) {
      goto cleanup;
    }
  }
  {
    static struct process_mock_entry entries[1];

    entries[0] = (struct process_mock_entry){.pid = 4000, .image = L"iD.exe", .start_ms = 1000, .ends = true};
    if (!TEST_CHECK(install_processes(entries, 1, false))) {
      goto cleanup;
    }
  }
  TEST_SUCCEEDED(repair_run(&s, targets, true, true, NULL, NULL, NULL, &r, &err), &err);
  TEST_CHECK_(r.outcome == REPAIR_OUTCOME_CLOSED, "want the made-up device released and back, got %d", (int)r.outcome);

cleanup:
  repair_result_release(&r);
  if (targets != NULL) {
    OV_ARRAY_DESTROY(&targets); // the devnode is a string of this test, not one of the run's
  }
  uninstall_mocks();
  if (s.mixer_process_names != NULL) {
    for (size_t i = 0; i < OV_ARRAY_LENGTH(s.mixer_process_names); i++) {
      OV_ARRAY_DESTROY(&s.mixer_process_names[i]);
    }
    OV_ARRAY_DESTROY(&s.mixer_process_names);
  }
}

/**
 * @brief A record that names no instance is dropped without asking a release of nothing
 *
 * An event that carries no DeviceInstance leaves a record that holds no instance.  Letting
 * such a record go is all that is needed; asking for the release of what was never taken
 * is a call on an empty handle.
 */
static void test_keep_device_lets_a_record_without_instance_go(void) {
  struct veto_event *events = NULL;
  struct veto_event without; //!< the way a parsed event without a DeviceInstance is left
  struct ov_error err = {0};
  wchar_t devnode[] = L"TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&deadbee&0&2";

  memset(&without, 0, sizeof(without));
  without.pid = 4000;
  without.event_time_ms = 1000;
  without.device_instance = NULL; // the event named no device
  if (!TEST_CHECK(OV_ARRAY_PUSH(&events, without))) {
    goto cleanup;
  }
  if (!TEST_SUCCEEDED(veto_event_keep_records(&events, devnode, &err), &err)) {
    goto cleanup;
  }
  TEST_CHECK_(event_list_length(events) == 0, "want the record without an instance dropped, got %zu", event_list_length(events));

cleanup:
  veto_event_destroy_list(&events);
}

TEST_LIST = {
    {"run_requires_arguments", test_run_requires_arguments},
    {"release_null", test_release_null},
    {"result_sentence", test_result_sentence},
    {"blocked_sentence", test_blocked_sentence},
    {"option_labels", test_option_labels_are_named_once},
    {"cancel_stops_the_run", test_cancel_stops_the_run},
    {"confirm_answers_the_watch", test_confirm_answers_the_watch},
    {"second_veto_is_answered", test_second_veto_is_answered},
    {"ask_while_closing_aborts", test_ask_while_closing_aborts},
    {"blocker_not_allowed_is_left_alone", test_blocker_not_allowed_is_left_alone},
    {"pausing_and_resuming_the_service_for_an_audio_engine_blocker", test_pausing_and_resuming_the_service_for_an_audio_engine_blocker},
    {"a_pause_that_failed_still_starts_the_service", test_a_pause_that_failed_still_starts_the_service},
    {"a_run_that_never_paused_still_asks_for_the_service", test_a_run_that_never_paused_still_asks_for_the_service},
    {"a_failure_stops_the_run", test_a_failure_stops_the_run},
    {"the_records_of_an_earlier_device_are_let_go", test_the_records_of_an_earlier_device_are_let_go},
    {"a_lookup_that_failed_stops_the_run", test_a_lookup_that_failed_stops_the_run},
    {"a_watch_that_failed_stops_the_run", test_a_watch_that_failed_stops_the_run},
    {"a_removal_that_failed_for_another_reason_stops_the_run", test_a_removal_that_failed_for_another_reason_stops_the_run},
    {"a_service_that_could_not_start_fails_the_run", test_a_service_that_could_not_start_fails_the_run},
    {"a_failure_that_came_first_is_the_one_reported", test_a_failure_that_came_first_is_the_one_reported},
    {"a_held_device_comes_back", test_a_held_device_comes_back},
    {"keep_device_lets_a_record_without_instance_go", test_keep_device_lets_a_record_without_instance_go},
    {NULL, NULL},
};
