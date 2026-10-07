#include "repair.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "culprit.h"
#include "device.h"
#include "logger.h"
#include "mixer.h"
#include "removal.h"
#include "settings.h"

/**
 * @brief Does an image path end with the given name
 *
 * @param path the path as reported by the veto event
 * @param name the image name to look for
 * @return true when the last component of path is name
 */
bool repair_path_matches_name(char const *const path, char const *const name) {
  size_t plen = 0;
  size_t nlen = 0;
  size_t i = 0;

  if ((path == NULL) || (name == NULL) || (name[0] == '\0')) {
    return false;
  }
  plen = strlen(path);
  nlen = strlen(name);
  if (plen < nlen) {
    return false;
  }
  i = plen - nlen;
  if (i > 0) {
    char const prev = path[i - 1];
    if ((prev != (char)0x5C) && (prev != '/') && (prev != ':')) {
      return false;
    }
  }
  return (_strnicmp(&path[i], name, nlen) == 0);
}

/**
 * @brief The moment now, in epoch milliseconds UTC
 *
 * @return the moment in epoch milliseconds UTC
 */
static long long now_ms(void) {
  FILETIME ft;
  ULARGE_INTEGER u;

  GetSystemTimeAsFileTime(&ft);
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return (long long)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

/**
 * @brief Put a sentence of its own into the result
 *
 * @param out the result to fill in
 * @param text the sentence to carry, UTF-8
 */
static void say(struct repair_result *const out, char const *const text) {
  strncpy(out->message, text, sizeof(out->message) - 1);
  out->message[sizeof(out->message) - 1] = '\0';
}

/**
 * @brief The words of the setting that lets this tool close the mixer
 *
 * @param s the settings that name the mixer, may be NULL for the defaults
 * @param out receives the caption, UTF-8, always terminated
 * @param out_size room of out
 */
void repair_option_mixer_label(struct settings const *const s, char *const out, size_t const out_size) {
  char const *const display = (s != NULL && s->mixer_display_name != NULL) ? s->mixer_display_name : "iD Mixer";
  char const *const exe = (s != NULL && s->mixer_process_name != NULL) ? s->mixer_process_name : "iD.exe";
  if ((out == NULL) || (out_size == 0)) {
    return;
  }
  OV_SNPRINTF(out, out_size, "%1$s%2$s", gettext("Close %1$s (%2$s) automatically"), display, exe);
}

/**
 * @brief The words of the setting that lets this tool close the audio engine
 *
 * @return the caption, UTF-8
 */
char const *repair_option_audiodg_label(void) { return gettext("Close audiodg (audiodg.exe) automatically"); }

/**
 * @brief The sentence of a veto that closed nothing for lack of a blocker this run may close
 *
 * blockers is how many distinct processes the records name as the blockers of the device.
 * Every one of these sentences opens with the state the reader is left with, because that is
 * what they came to the log for.  Then it names the single thing they can change: option
 * carries the words of the setting that would have let this tool close the process, said the
 * way the window says them; NULL says the record named somebody this tool never closes.
 * More than one blocker makes the sentence carry the number instead: naming one of them
 * would read as if it were the only one.
 *
 * @param option the words of the setting that stands in the way, or NULL
 * @param blockers how many distinct processes the records name
 * @param out receives the sentence, UTF-8
 * @param out_size room of out
 */
void repair_blocked_sentence(char const *const option, size_t const blockers, char *const out, size_t const out_size) {
  if ((out == NULL) || (out_size == 0)) {
    return;
  }
  out[0] = '\0';
  if (blockers > 1) {
    OV_SNPRINTF(out, out_size, NULL, "The device is still in use by %1$u processes.", (unsigned)blockers);
  } else if ((option != NULL) && (option[0] != '\0')) {
    OV_SNPRINTF(out, out_size, NULL, "The device is still in use because \"%s\" is turned off.", option);
  } else {
    strncpy(out, "The device is still in use by another process.", out_size - 1);
    out[out_size - 1] = '\0';
  }
}

/**
 * @brief Put a formatted sentence into the result
 *
 * The message names the process that was in the way; reference describes the arguments
 * for the format the way ov_sprintf_char is called everywhere else here.
 *
 * @param out the result to fill in
 * @param reference the pattern of the sentence, and its reference
 * @param what the process the sentence names
 */
static void sayf(struct repair_result *const out, char const *const reference, char const *const what) {
  OV_SNPRINTF(out->message, sizeof(out->message), reference, reference, what);
}

/**
 * @brief The pids of the blockers of a run
 *
 * The caller uses them to keep the processes it closed out of the list it shows.  A
 * process this tool closed is not a reason the device was blocked: it is gone, the user
 * watched it go, and a list that still names it says the opposite of what happened.  The
 * identity that is dropped is the exact instance that was closed, so the same image running
 * elsewhere stays on the list.
 *
 * @param blockers the blockers of the run
 * @param out receives the pids, capacity 2 (the mixer and audiodg)
 * @return how many pids were written
 */
size_t repair_closed_pids(struct repair_blockers const *const blockers, uint32_t *const out) {
  size_t n = 0;

  if ((blockers == NULL) || (out == NULL)) {
    return 0;
  }
  if (blockers->mixer.found) {
    out[n++] = blockers->mixer.pid;
  }
  if (blockers->audiodg.found) {
    out[n++] = blockers->audiodg.pid;
  }
  return n;
}

/**
 * @brief Keep the processes a run closed out of the list of the culprits
 *
 * @param list the list to shrink in place
 * @param closed the blockers the run closed
 */
static void drop_closed(struct culprit **const list, struct repair_blockers const *const closed) {
  uint32_t pids[2];
  size_t const n = repair_closed_pids(closed, pids);

  culprit_drop_pids(list, pids, n);
}

/**
 * @brief Tell the caller what the run is waiting for
 *
 * @param progress the callback of the caller, may be NULL
 * @param userdata handed back to progress
 * @param stage what the run is waiting for
 * @param step the step of the run
 */
static void report_stage(repair_progress_fn const progress, void *const userdata, enum repair_stage const stage, size_t const step) {
  if (progress != NULL) {
    progress(stage, step, REPAIR_STEPS_TOTAL, userdata);
  }
}

/**
 * @brief Was the run asked to stop
 *
 * The flag is a plain bool the caller owns: the window sets it from its own thread, and
 * the run reads it between its steps.  A bool written by one thread and read by another is
 * not a synchronisation primitive, but one writer that only ever writes true is exactly
 * the case the plain flag is for.
 *
 * @param cancel the flag of the caller, may be NULL
 * @return true when the run is to quit at the next step boundary
 */
static bool cancel_requested(bool const *const cancel) { return (cancel != NULL) && *cancel; }

/**
 * @brief One line about what the run is doing, in the log
 *
 * The UI stays free of the technical detail, and the detail is worthless when it is
 * nowhere.  A record that cannot be written is reported where it stands.
 *
 * @param tag the module the record belongs to
 * @param text the record itself, UTF-8
 */
static void log_note(char const *const tag, char const *const text) {
  struct ov_error err = {0};

  if (!logger_write(tag, text, &err)) {
    OV_ERROR_REPORT(&err, NULL);
  }
}

/**
 * @brief Fold the abort into the result
 *
 * The sentence says who stopped the run: only a person at the progress window can, whether
 * they pressed its button or closed the window, and the run does not tell the two apart.
 * That the device was left alone follows from the stop and is not said twice.
 *
 * @param out receives the outcome and the sentence
 */
static void say_aborted(struct repair_result *const out) {
  out->outcome = REPAIR_OUTCOME_ABORTED;
  say(out, "The operation was aborted by the user.");
}

/**
 * @brief Read the veto records of the attempt that just failed
 *
 * The veto record is written a few seconds after the removal call returns, so the read
 * waits for the record of this attempt instead of racing it.  Events older than since_ms
 * belong to an earlier attempt and are dropped here.
 *
 * @param s the settings that name the log and the provider
 * @param since_ms records older than this belong to an earlier attempt
 * @param progress the callback of the caller, may be NULL
 * @param progress_userdata handed back to progress
 * @param step the step of the run
 * @param cancel the flag of the caller, may be NULL: a raised flag ends the wait
 * @param err receives the failure of the query
 * @return NULL when the query itself failed, an empty list when no veto record showed up
 */
static struct culprit *read_veto_events(struct settings const *const s,
                                        long long const since_ms,
                                        repair_progress_fn const progress,
                                        void *const progress_userdata,
                                        size_t const step,
                                        bool const *const cancel,
                                        struct ov_error *const err) {
  struct culprit *list = NULL;
  size_t attempt = 0;

  for (;;) {
    if (attempt > 0) {
      Sleep(1000);
    }
    report_stage(progress, progress_userdata, REPAIR_STAGE_VETO_RECORD, step);
    if (cancel_requested(cancel)) {
      culprit_destroy_list(&list);
      return NULL;
    }
    list = culprit_read(s->veto_log_name, s->veto_provider_name, (unsigned short)s->veto_event_id, err);
    if (list == NULL) {
      return NULL;
    }
    culprit_prune(&list, since_ms);
    if ((culprit_count(list) > 0) || (attempt >= 5)) {
      return list; // the last try hands back the empty list, the query itself worked
    }
    culprit_destroy_list(&list);
    attempt++;
  }
}

/**
 * @brief The blocker this tool may close, out of the record of one veto
 *
 * The newest record wins when the same image is named twice.
 *
 * @param culprits the records of the veto
 * @param name the image name to look for
 * @return the blocker to close, found is false when the records name only blockers this
 *         tool may not close
 */
static struct repair_blocker pick_one(struct culprit const *const culprits, char const *const name) {
  struct repair_blocker result;
  size_t const n = culprit_count(culprits);
  size_t i = 0;

  memset(&result, 0, sizeof(result));
  if ((culprits == NULL) || (name == NULL)) {
    return result;
  }
  for (i = 0; i < n; i++) {
    if (culprits[i].pid == 0) {
      continue;
    }
    if (!repair_path_matches_name(culprits[i].path, name)) {
      continue;
    }
    if (result.found && (culprits[i].event_time_ms <= result.event_time_ms)) {
      continue;
    }
    result.found = true;
    result.pid = culprits[i].pid;
    result.event_time_ms = culprits[i].event_time_ms;
  }
  return result;
}

/**
 * @brief Which blockers of a veto record this run may close
 *
 * mixer_name is the image name from the settings, REPAIR_AUDIODG_NAME is the audio
 * engine; the two allow_* flags are the user's decision to let this tool close them at
 * all.
 *
 * @param culprits the records of the veto, may be NULL
 * @param mixer_name the image name the settings name
 * @param allow_mixer_close the user allowed closing the mixer
 * @param allow_audiodg_close the user allowed closing the audio engine
 * @return the blockers to close, found is false when there is none
 */
struct repair_blockers repair_pick_blockers(struct culprit const *const culprits,
                                            char const *const mixer_name,
                                            bool const allow_mixer_close,
                                            bool const allow_audiodg_close) {
  struct repair_blockers b;

  memset(&b, 0, sizeof(b));
  if (allow_mixer_close) {
    b.mixer = pick_one(culprits, mixer_name);
  }
  if (allow_audiodg_close) {
    b.audiodg = pick_one(culprits, REPAIR_AUDIODG_NAME);
  }
  return b;
}

/**
 * @brief Close one blocking process and wait until it is gone
 *
 * The identity comes from the veto record, so only that instance is touched; a process
 * that is already gone is no blocker any more and costs nothing.  Only a hard failure
 * stops the run and folds its reason into the outcome.
 *
 * @param blocker the instance the record named
 * @param name the image name to check against
 * @param stage the stage to report
 * @param progress the callback of the caller, may be NULL
 * @param progress_userdata handed back to progress
 * @param step the step of the run
 * @param out receives what happened
 * @param target receives the identity that was closed, for the restart hint
 * @param err receives the hard failure
 * @return false only on a hard failure
 */
static bool close_blocking_process(struct repair_blocker const *const blocker,
                                   char const *const name,
                                   enum repair_stage const stage,
                                   repair_progress_fn const progress,
                                   void *const progress_userdata,
                                   size_t *const step,
                                   struct repair_result *const out,
                                   struct mixer_target *const target,
                                   struct ov_error *const err) {
  bool found = false;
  bool success = false;
  struct ov_error log_err = {0};

  report_stage(progress, progress_userdata, stage, ++(*step));
  if (!logger_writef("close", &log_err, "%s pid=%lu event_time=%lld", name, (unsigned long)blocker->pid, blocker->event_time_ms)) {
    OV_ERROR_REPORT(&log_err, NULL);
  }
  if (!mixer_take(blocker->pid, name, blocker->event_time_ms, &found, target, err)) {
    OV_ERROR_ADD_TRACE(err);
    out->outcome = REPAIR_OUTCOME_FAILED;
    sayf(out, "%1$hs could not be inspected.", name);
    goto cleanup;
  }
  if (!found) {
    if (!logger_writef(
            "close", &log_err, "%s pid=%lu was not closed: it is not the instance the record named", name, (unsigned long)blocker->pid)) {
      OV_ERROR_REPORT(&log_err, NULL);
    }
    sayf(out, "%1$hs is no longer running.", name);
    success = true;
    goto cleanup;
  }
  if (!mixer_close(target, err)) {
    OV_ERROR_ADD_TRACE(err);
    out->outcome = REPAIR_OUTCOME_FAILED;
    sayf(out, "%1$hs could not be closed.", name);
    goto cleanup;
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief Run the repair
 *
 * The documented sequence: remove, look up the culprits when vetoed, optionally close the
 * blockers (the mixer and audiodg) and remove again.  It never touches anything outside
 * the settings.
 *
 * @param s the settings this run works for
 * @param allow_mixer_close the user allowed closing the mixer
 * @param allow_audiodg_close the user allowed closing the audio engine
 * @param progress may be NULL; it is called from the thread that runs the repair
 * @param progress_userdata handed back to progress
 * @param cancel the flag of the caller, may be NULL: when it is up the run quits at the
 *        next boundary between the steps and ends with the aborted outcome
 * @param out receives what happened; release with repair_result_release
 * @param err receives the failure that stopped the run
 * @return false only on a hard failure.  The outcome carries the news: a step whose
 *         failure is folded into the outcome releases its error into the log before the
 *         next step starts, so err is only worth reading when the call returns false.
 */
bool repair_run(struct settings const *const s,
                bool const allow_mixer_close,
                bool const allow_audiodg_close,
                repair_progress_fn const progress,
                void *const progress_userdata,
                bool *const cancel,
                struct repair_result *const out,
                struct ov_error *const err) {
  bool success = false;
  char *devnode = NULL;
  enum device_state state = DEVICE_STATE_UNKNOWN;
  enum removal_result removal = REMOVAL_RESULT_FAILED;
  enum removal_reason removal_reason = REMOVAL_REASON_NONE;
  long long since = 0;
  struct repair_blockers picked;
  struct mixer_target taken_mixer;
  struct mixer_target taken_audiodg;
  struct culprit *current = NULL;
  struct ov_error removal_err = {0};
  struct ov_error read_err = {0};
  struct ov_error log_err = {0};
  size_t step = 0;

  if ((s == NULL) || (out == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  memset(out, 0, sizeof(*out));
  memset(&taken_mixer, 0, sizeof(taken_mixer));
  memset(&taken_audiodg, 0, sizeof(taken_audiodg));
  memset(&picked, 0, sizeof(picked));

  if (cancel_requested(cancel)) {
    say_aborted(out);
    success = true;
    goto cleanup;
  }
  report_stage(progress, progress_userdata, REPAIR_STAGE_PROBE, ++step);
  state = device_probe(s->ks_service, s->ks_instance_prefix, err);
  switch (state) {
  case DEVICE_STATE_HEALTHY:
  case DEVICE_STATE_FAULTED:
    break;
  case DEVICE_STATE_ABSENT:
    if (cancel_requested(cancel)) {
      say_aborted(out);
      success = true;
      goto cleanup;
    }
    out->outcome = REPAIR_OUTCOME_ABSENT;
    say(out, "No Audient device was found.");
    success = true;
    goto cleanup;
  case DEVICE_STATE_PROBLEM:
    out->outcome = REPAIR_OUTCOME_OTHER_PROBLEM;
    say(out, "The device has a problem that this tool cannot solve.");
    success = true;
    goto cleanup;
  case DEVICE_STATE_UNKNOWN:
    out->outcome = REPAIR_OUTCOME_FAILED;
    say(out, "The device state could not be determined.");
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  devnode = device_ks_devnode(s->ks_service, s->ks_instance_prefix, err);
  if (devnode == NULL) {
    out->outcome = REPAIR_OUTCOME_FAILED;
    say(out, "The device could not be found.");
    success = true;
    goto cleanup;
  }

  since = now_ms();
  if (!logger_writef("remove", &log_err, "first attempt, device=%s since=%lld", devnode, since)) {
    OV_ERROR_REPORT(&log_err, NULL);
  }
  if (cancel_requested(cancel)) {
    say_aborted(out);
    success = true;
    goto cleanup;
  }
  report_stage(progress, progress_userdata, REPAIR_STAGE_REMOVE, ++step);
  removal = removal_close(devnode, &removal_reason, &removal_err);
  if (removal == REMOVAL_RESULT_OK) {
    out->outcome = REPAIR_OUTCOME_CLOSED;
    say(out, "The device was released.");
    log_note("remove", "The device was released on the first attempt.");
    culprit_clear(&out->culprits); // nothing blocks the device any more
    success = true;
    goto cleanup;
  }
  if (removal != REMOVAL_RESULT_VETO) {
    out->outcome = REPAIR_OUTCOME_FAILED;
    out->removal_reason = removal_reason;
    say(out, "The device could not be released.");
    success = true;
    goto cleanup;
  }
  if (cancel_requested(cancel)) {
    say_aborted(out);
    success = true;
    goto cleanup;
  }
  out->culprits = read_veto_events(s, since, progress, progress_userdata, ++step, cancel, err);
  if (out->culprits == NULL) {
    out->outcome = REPAIR_OUTCOME_VETOED;
    say(out, "The device could not be released, and the reason could not be found.");
    success = true;
    goto cleanup;
  }
  if (!logger_writef("veto", &log_err, "the removal was vetoed by %u record(s)", (unsigned)culprit_count(out->culprits))) {
    OV_ERROR_REPORT(&log_err, NULL);
  }

  picked = repair_pick_blockers(out->culprits, s->mixer_process_name, allow_mixer_close, allow_audiodg_close);
  if (!picked.mixer.found && !picked.audiodg.found) {
    char option_buf[256] = {0};
    char const *option = NULL;
    char sentence[256];

    out->outcome = REPAIR_OUTCOME_VETOED;
    if (pick_one(out->culprits, s->mixer_process_name).found) {
      repair_option_mixer_label(s, option_buf, sizeof(option_buf));
      option = option_buf;
    } else if (pick_one(out->culprits, REPAIR_AUDIODG_NAME).found) {
      option = repair_option_audiodg_label();
    }
    repair_blocked_sentence(option, culprit_count(out->culprits), sentence, sizeof(sentence));
    say(out, sentence);
    success = true;
    goto cleanup;
  }

  if (cancel_requested(cancel)) {
    say_aborted(out);
    success = true;
    goto cleanup;
  }
  if (picked.mixer.found &&
      !close_blocking_process(
          &picked.mixer, s->mixer_process_name, REPAIR_STAGE_MIXER, progress, progress_userdata, &step, out, &taken_mixer, err)) {
    success = true;
    goto cleanup;
  }
  if (cancel_requested(cancel)) {
    say_aborted(out);
    success = true;
    goto cleanup;
  }
  if (picked.audiodg.found &&
      !close_blocking_process(
          &picked.audiodg, REPAIR_AUDIODG_NAME, REPAIR_STAGE_AUDIODG, progress, progress_userdata, &step, out, &taken_audiodg, err)) {
    success = true;
    goto cleanup;
  }

  if (cancel_requested(cancel)) {
    say_aborted(out);
    success = true;
    goto cleanup;
  }
  since = now_ms();
  if (!logger_writef("remove", &log_err, "second attempt, device=%s since=%lld", devnode, since)) {
    OV_ERROR_REPORT(&log_err, NULL);
  }
  report_stage(progress, progress_userdata, REPAIR_STAGE_REMOVE, ++step);
  removal = removal_close(devnode, &removal_reason, err);
  if (removal == REMOVAL_RESULT_OK) {
    out->outcome = REPAIR_OUTCOME_CLOSED;
    say(out, "The device was released.");
    log_note("remove", "The device was released on the second attempt.");
    culprit_clear(&out->culprits); // nothing blocks the device any more
    success = true;
    goto cleanup;
  }
  if (cancel_requested(cancel)) {
    say_aborted(out);
    success = true;
    goto cleanup;
  }
  current = read_veto_events(s, since, progress, progress_userdata, ++step, cancel, &read_err);
  if (current != NULL) {
    drop_closed(&current, &picked);
    if (culprit_count(current) > 0) {
      culprit_destroy_list(&out->culprits);
      out->culprits = current;
    } else {
      culprit_destroy_list(&current);
    }
    current = NULL;
  }
  if (cancel_requested(cancel)) {
    say_aborted(out);
    success = true;
    goto cleanup;
  }
  drop_closed(&out->culprits, &picked);
  out->outcome = REPAIR_OUTCOME_VETOED;
  say(out, "The device is still in use.");
  log_note("remove", "The blockers were closed but the device is still blocked.");
  success = true;

cleanup:
  if (success) {
    OV_ERROR_REPORT(err, NULL);
  }
  OV_ERROR_REPORT(&removal_err, NULL);
  OV_ERROR_REPORT(&read_err, NULL);
  device_string_free(&devnode);
  mixer_target_release(&taken_mixer);
  mixer_target_release(&taken_audiodg);
  return success;
}

/**
 * @brief Release what a run collected
 *
 * @param r NULL is allowed
 */
void repair_result_release(struct repair_result *const r) {
  if (r == NULL) {
    return;
  }
  culprit_destroy_list(&r->culprits);
  r->outcome = REPAIR_OUTCOME_UNKNOWN;
}
