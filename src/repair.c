#include "repair.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "audio_service.h"
#include "blocker.h"
#include "device.h"
#include "logger.h"
#include "process.h"
#include "removal.h"
#include "settings.h"
#include "veto_event.h"

/**
 * @brief The work a run does, and where it stands
 *
 * Everything a repair needs apart from the device it works on: the settings, the two
 * permissions, the report to the caller and the progress window.  The step is kept here
 * because every stage counts it up, so no call has to carry the pair of it and the total
 * around.
 */
struct repair_ctx {
  struct settings const *settings; //!< the settings this run works for
  bool allow_mixer_close;          //!< the user allowed closing the mixer
  bool allow_audiodg_close;        //!< the user allowed pausing the audio engine
  bool can_pause_audio_service;    //!< pausing the audio service is allowed on this build and by these settings
  repair_progress_fn progress;     //!< may be NULL; called from the thread that runs the repair
  void *progress_userdata;         //!< handed back to progress
  bool *cancel;                    //!< may be NULL; the run reads it between its steps
  struct repair_result *out;       //!< receives what happened
  size_t step;                     //!< the step the run is at, counted from zero
  size_t total;                    //!< how many steps the longest way takes
};

/**
 * @brief May this build pause the audio service at all
 *
 * Pausing it takes the sound away from the whole machine, so a build that ships only does it
 * when the settings say so in as many words.  A debug build is free to do it without that,
 * so the behaviour can be tried out at all.
 *
 * @param setting the value of allow_audio_service_pause from the settings file
 * @return true when the service may be paused
 */
static bool adc_can_pause_audio_service(bool const setting) {
#ifdef NDEBUG
  return setting;
#else
  (void)setting;
  return true;
#endif
}

bool repair_can_pause_audio_service(struct settings const *const s) {
  return adc_can_pause_audio_service((s != NULL) && s->allow_audio_service_pause);
}

/**
 * @brief The system calls a run goes through
 *
 * They read the machine and cannot be answered by a test, so every one of them is a
 * function the test replaces.  No production path ever replaces one.
 */
struct repair_deps {
  enum device_state (*probe_devnode)(char const *devnode, struct ov_error *err);
  bool (*removal_close)(char const *devnode, struct ov_error *err);
  struct veto_event *(*read_veto)(char const *log_name, char const *provider_name, unsigned short event_id, struct ov_error *err);
  ov_tribool (*names_ancestor)(char const *device_instance, char const *devnode, struct ov_error *err);
  struct blocker *(*blocker_list_build)(struct veto_event const *events,
                                        char const *const *mixer_names,
                                        size_t mixer_name_count,
                                        struct ov_error *err);
  bool (*blocker_close)(struct blocker const *b, bool allow_privilege, struct ov_error *err);
  bool (*audio_service_pause)(struct ov_error *err);
  bool (*audio_service_resume)(struct ov_error *err);
};

/**
 * @brief The system calls a run goes through, the real ones
 */
static struct repair_deps const kDefaultDeps = {
    .probe_devnode = device_probe_devnode,
    .removal_close = removal_close,
    .read_veto = veto_event_read,
    .names_ancestor = device_veto_event_names_ancestor,
    .blocker_list_build = blocker_list_build,
    .blocker_close = blocker_close,
    .audio_service_pause = audio_service_pause,
    .audio_service_resume = audio_service_resume,
};

/**
 * @brief Put a sentence of its own into the result
 *
 * A run that repairs more than one device says what came of every one of them,
 * so a sentence joins the ones before it on a line of its own.
 *
 * @param out the result to fill in
 * @param text the sentence to carry, UTF-8
 */
static void say(struct repair_result *const out, char const *const text) {
  size_t used = 0;
  size_t room = 0;

  if ((out == NULL) || (text == NULL)) {
    return;
  }
  used = strnlen(out->message, sizeof(out->message) - 1);
  room = sizeof(out->message) - 1 - used;
  if (used > 0) {
    if (room > 1) {
      out->message[used] = '\n';
      used += 1;
      room -= 1;
    } else {
      return;
    }
  }
  strncpy(out->message + used, text, room);
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
  (void)s;
  if ((out == NULL) || (out_size == 0)) {
    return;
  }
  strncpy(out, gettext("Close the Audient mixer automatically"), out_size - 1);
  out[out_size - 1] = '\0';
}

/**
 * @brief The words the stage report shows while a mixer is being closed
 *
 * @param s the settings that name the mixer, may be NULL for the defaults
 * @param out receives the caption, UTF-8, always terminated
 * @param out_size room of out
 */
void repair_stage_mixer_caption(struct settings const *const s, char *const out, size_t const out_size) {
  (void)s;
  if ((out == NULL) || (out_size == 0)) {
    return;
  }
  strncpy(out, gettext("Closing the Audient mixer..."), out_size - 1);
  out[out_size - 1] = '\0';
}

/**
 * @brief The words of the setting that lets this tool close the audio engine
 *
 * @return the caption, UTF-8
 */
char const *repair_option_audiodg_label(void) { return gettext("Allow pausing the Windows Audio service"); }

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
static void repair_blocked_sentence(char const *const option, size_t const blockers, char *const out, size_t const out_size) {
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
 * @brief Tell the caller what the run is waiting for
 *
 * @param ctx the run that reports
 * @param stage what the run is waiting for
 */
static void report_stage(struct repair_ctx const *const ctx, enum repair_stage const stage) {
  if (ctx->progress != NULL) {
    ctx->progress(stage, ctx->step, ctx->total, ctx->progress_userdata);
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
 * @param ctx the run that reads
 * @param deps the system calls the run goes through
 * @param since_ms records older than this belong to an earlier attempt
 * @param err receives the failure of the query
 * @return NULL when the query itself failed, an empty list when no veto record showed up
 */
static struct veto_event *
read_veto_events(struct repair_ctx *const ctx, struct repair_deps const *const deps, uint64_t const since_ms, struct ov_error *const err) {
  struct veto_event *list = NULL;
  size_t attempt = 0;

  for (;;) {
    if (attempt > 0) {
      Sleep(1000);
    }
    report_stage(ctx, REPAIR_STAGE_VETO_RECORD);
    if (cancel_requested(ctx->cancel)) {
      veto_event_destroy_list(&list);
      return NULL;
    }
    list =
        deps->read_veto(ctx->settings->veto_log_name, ctx->settings->veto_provider_name, (unsigned short)ctx->settings->veto_event_id, err);
    if (list == NULL) {
      return NULL;
    }
    veto_event_prune(&list, since_ms);
    if ((veto_event_count(list) > 0) || (attempt >= 5)) {
      return list; // the last try hands back the empty list, the query itself worked
    }
    veto_event_destroy_list(&list);
    attempt++;
  }
}

/**
 * @brief Close the mixers of the list and collect their ids
 *
 * The audio engine is not closed here: it is started again within milliseconds of being
 * terminated, so the service that starts it is paused instead (see pause_audio_engine).
 *
 * @param ctx the run that closes
 * @param deps the system calls the run goes through
 * @param blockers the list to go through
 * @param stage the stage to report
 * @param out_pids receives the ids that were closed, may be NULL
 * @param out_capacity room of out_pids
 * @param out_count receives how many ids were written
 * @param err receives the hard failure
 * @return false only on a hard failure
 */
static bool close_mixer_blockers(struct repair_ctx *const ctx,
                                 struct repair_deps const *const deps,
                                 struct blocker const *const blockers,
                                 enum repair_stage const stage,
                                 uint32_t *const out_pids,
                                 size_t const out_capacity,
                                 size_t *const out_count,
                                 struct ov_error *const err) {
  size_t const n = blocker_count(blockers);
  size_t i = 0;
  bool success = false;

  for (i = 0; i < n; i++) {
    if (blockers[i].kind != BLOCKER_KIND_MIXER) {
      continue;
    }
    if (i == 0) {
      report_stage(ctx, stage);
    }
    logger_notef("close", "mixer pid=%lu", (unsigned long)blockers[i].pid);
    if (!deps->blocker_close(&blockers[i], false, err)) {
      OV_ERROR_ADD_TRACE(err);
      ctx->out->outcome = REPAIR_OUTCOME_FAILED;
      sayf(ctx->out, "%1$hs could not be closed.", "The mixer");
      goto cleanup;
    }
    if ((out_pids != NULL) && (*out_count < out_capacity)) {
      out_pids[*out_count] = blockers[i].pid;
      (*out_count)++;
    }
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief Pause the audio engine, so it lets the device go
 *
 * The engine is started by the Windows Audio service and comes back within milliseconds of
 * being terminated, so terminating it does not free the device.  Pausing the service does,
 * and the caller starts it again when the device is done with.
 *
 * @param ctx the run that pauses
 * @param deps the system calls the run goes through
 * @param paused receives true when the service was paused and must be started again
 * @param err receives the hard failure
 * @return false only on a hard failure
 */
static bool
pause_audio_engine(struct repair_ctx *const ctx, struct repair_deps const *const deps, bool *const paused, struct ov_error *const err) {
  if (!ctx->can_pause_audio_service) {
    logger_notef("close", "the audio engine holds the device, but pausing the service is not allowed");
    return true;
  }
  report_stage(ctx, REPAIR_STAGE_AUDIODG);
  if (!deps->audio_service_pause(err)) {
    OV_ERROR_ADD_TRACE(err);
    ctx->out->outcome = REPAIR_OUTCOME_FAILED;
    say(ctx->out, "The Windows Audio service could not be paused.");
    return false;
  }
  *paused = true;
  return true;
}

/**
 * @brief Keep the records that name the device of the devnode
 *
 * A veto record belongs to the device whose removal was stopped, so a record
 * stays when the instance it names is one of the ancestors of the devnode.  A
 * record that names no instance cannot be told apart from the records of the
 * other devices, so it goes: acting on it would close a process that blocks
 * another device.
 *
 * @param deps the system calls the run goes through
 * @param list the list to shrink in place
 * @param devnode the KS devnode of the device being repaired
 * @param err receives the failure of the ancestor walk
 * @return false when the walk failed
 */
static bool veto_event_keep_device(struct repair_deps const *const deps,
                                   struct veto_event **const list,
                                   char const *const devnode,
                                   struct ov_error *const err) {
  size_t kept = 0;
  size_t i = 0;
  size_t const n = veto_event_count(*list);
  bool success = false;

  for (i = 0; i < n; i++) {
    struct veto_event *const record = &(*list)[i];

    if (record->device_instance == NULL) {
      OV_ARRAY_DESTROY(&record->device_instance);
      continue;
    }
    ov_tribool const named = deps->names_ancestor(record->device_instance, devnode, err);
    if (named == ov_indeterminate) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (named == ov_false) {
      OV_ARRAY_DESTROY(&record->device_instance);
      continue;
    }
    (*list)[kept++] = *record;
  }
  OV_ARRAY_SET_LENGTH(*list, kept);
  success = true;

cleanup:
  return success;
}

/**
 * @brief Which of two outcomes stands over the other
 *
 * The run stops for a device it could not repair for a reason it does not know,
 * so that outcome stands over the one that says the device is still in use.
 *
 * @param a one outcome of a device
 * @param b the other outcome of a device
 * @return the outcome that stands over the other
 */
static enum repair_outcome worse_outcome(enum repair_outcome const a, enum repair_outcome const b) {
  enum repair_outcome const order[] = {REPAIR_OUTCOME_CLOSED, REPAIR_OUTCOME_VETOED, REPAIR_OUTCOME_OTHER_PROBLEM, REPAIR_OUTCOME_FAILED};
  size_t ia = 0;
  size_t ib = 0;

  for (ia = 0; ia < sizeof(order) / sizeof(order[0]); ia++) {
    if (order[ia] == a) {
      break;
    }
  }
  for (ib = 0; ib < sizeof(order) / sizeof(order[0]); ib++) {
    if (order[ib] == b) {
      break;
    }
  }
  return (ib > ia) ? b : a;
}

/**
 * @brief How many looks the confirmation takes while it waits for a device to come back
 *
 * One second between two looks is fixed, so the number of looks follows from the wait the
 * settings name and nothing else.  The smallest wait still takes one look: a device that
 * is back at once has to be found by it.
 *
 * @param max_wait_ms how long the confirmation is willing to wait, milliseconds
 * @return how many looks it takes
 */
static size_t confirm_poll_count(DWORD const max_wait_ms) {
  size_t const count = (size_t)(max_wait_ms / REPAIR_CONFIRM_POLL_INTERVAL_MS);
  return (count < 1) ? 1 : count;
}

/**
 * @brief Ask the released device whether it is an audio device again
 *
 * The release puts the device back on the bus, and the bus takes a moment to bring the
 * device up again.  The confirmation waits one second between two looks and keeps looking
 * until the wait the settings name runs out, so a device that comes back is found however
 * slow the bus is, and a look that finds the device healthy ends the polling at once.
 *
 * @param ctx the run that confirms
 * @param deps the system calls the run goes through
 * @param target the device that was released
 * @param err receives the failure of the last look
 * @return the outcome of the confirmation
 */
static enum repair_outcome confirm_recovered(struct repair_ctx *const ctx,
                                             struct repair_deps const *const deps,
                                             struct device_target const *const target,
                                             struct ov_error *const err) {
  bool failed = false;

  ctx->step++;
  report_stage(ctx, REPAIR_STAGE_CONFIRM);
  for (size_t poll = 0; poll < confirm_poll_count((DWORD)ctx->settings->confirm_max_wait_ms); poll++) {
    enum device_state state = DEVICE_STATE_QUERY_FAILED;

    if (failed) {
      // the look before this one wrote its reason, and this one needs the struct back:
      // a look that could not be taken is one the run does without, so its reason is
      // spent here rather than carried to the caller
      OV_ERROR_DESTROY(err);
      failed = false;
    }
    Sleep(REPAIR_CONFIRM_POLL_INTERVAL_MS);
    if (cancel_requested(ctx->cancel)) {
      say_aborted(ctx->out);
      return REPAIR_OUTCOME_ABORTED;
    }
    state = deps->probe_devnode(target->devnode, err);
    failed = (state == DEVICE_STATE_QUERY_FAILED);
    if (state == DEVICE_STATE_HEALTHY) {
      return REPAIR_OUTCOME_CLOSED;
    }
  }
  // the reason of the last look is the one worth keeping: it is the one the device stayed
  // away for at the end of the wait
  OV_ERROR_REPORT(err, NULL);
  ctx->out->outcome = REPAIR_OUTCOME_FAILED;
  say(ctx->out, "The device did not come back as an audio device, and the reason is not known.");
  return REPAIR_OUTCOME_FAILED;
}

/**
 * @brief Repair one device
 *
 * The documented sequence for the device: remove, look up the events when
 * vetoed, optionally close the blockers (the mixer and audiodg) and remove
 * again, then confirm that the device came back as an audio device.  The
 * records the run leaves behind belong to the device, so the caller can name
 * the processes that still block it.
 *
 * @param ctx the run this device is repaired for
 * @param deps the system calls the run goes through
 * @param target the device to repair
 * @param err receives the failure that stopped the repair of the device
 * @return the outcome of the device
 */
static enum repair_outcome repair_target(struct repair_ctx *const ctx,
                                         struct repair_deps const *const deps,
                                         struct device_target const *const target,
                                         struct ov_error *const err) {
  struct settings const *const s = ctx->settings;
  struct repair_result *const out = ctx->out;
  enum repair_outcome outcome = REPAIR_OUTCOME_FAILED;
  uint64_t since = 0;
  struct blocker *blockers = NULL;
  struct veto_event *events = NULL;
  struct veto_event *current = NULL;
  uint32_t closed[64] = {0};
  size_t closed_count = 0;
  struct ov_error removal_err = {0};
  struct ov_error read_err = {0};
  bool audio_service_paused = false;
  bool success = false;

  if (target->state == DEVICE_STATE_PROBLEM) {
    outcome = REPAIR_OUTCOME_OTHER_PROBLEM;
    say(out, "The device has a problem that this tool cannot solve.");
    success = true;
    goto cleanup;
  }

  since = process_now_ms();
  logger_notef("remove", "first attempt, device=%s since=%llu", target->devnode, (unsigned long long)since);
  if (cancel_requested(ctx->cancel)) {
    say_aborted(out);
    outcome = REPAIR_OUTCOME_ABORTED;
    success = true;
    goto cleanup;
  }
  ctx->step++;
  report_stage(ctx, REPAIR_STAGE_REMOVE);
  if (deps->removal_close(target->devnode, &removal_err)) {
    outcome = REPAIR_OUTCOME_CLOSED;
    say(out, "The device was released.");
    logger_notef("remove", "The device was released on the first attempt.");
    outcome = confirm_recovered(ctx, deps, target, err);
    success = true;
    goto cleanup;
  }
  if (!removal_error_is_veto(&removal_err)) {
    outcome = REPAIR_OUTCOME_FAILED;
    out->removal_error = removal_error_of(&removal_err);
    say(out, "The device could not be released.");
    success = true;
    goto cleanup;
  }
  // the veto is spent here: the error is reported for the log, and the second attempt needs
  // the struct empty again to raise its own refusal into
  OV_ERROR_REPORT(&removal_err, NULL);
  if (cancel_requested(ctx->cancel)) {
    say_aborted(out);
    outcome = REPAIR_OUTCOME_ABORTED;
    success = true;
    goto cleanup;
  }
  events = read_veto_events(ctx, deps, since, &read_err);
  if (events == NULL) {
    if (cancel_requested(ctx->cancel)) {
      say_aborted(out);
      outcome = REPAIR_OUTCOME_ABORTED;
      success = true;
      goto cleanup;
    }
    outcome = REPAIR_OUTCOME_VETOED;
    say(out, "The device could not be released, and the reason could not be found.");
    success = true;
    goto cleanup;
  }
  logger_notef("veto", "the removal of device=%s was vetoed by %u record(s)", target->devnode, (unsigned)veto_event_count(events));
  if (!veto_event_keep_device(deps, &events, target->devnode, err)) {
    OV_ERROR_ADD_TRACE(err);
    outcome = REPAIR_OUTCOME_FAILED;
    goto cleanup;
  }
  // what a record says a process is called is what it was called then; the machine is
  // asked what the process the id names is now, and a process that took the id over after
  // the record was written is dropped by that same answer
  blockers = deps->blocker_list_build(events, settings_mixer_names(s), settings_mixer_name_count(s), err);
  if (blockers == NULL) {
    OV_ERROR_ADD_TRACE(err);
    outcome = REPAIR_OUTCOME_FAILED;
    goto cleanup;
  }
  if (blocker_count(blockers) == 0) {
    // every process the records name is already gone or is not the one they named
    logger_notef("probe", "none of the records names a running process");
  }

  if (cancel_requested(ctx->cancel)) {
    say_aborted(out);
    outcome = REPAIR_OUTCOME_ABORTED;
    success = true;
    goto cleanup;
  }
  if (blocker_count(blockers) > 0) {
    size_t const mixers = blocker_mixer_count(blockers);
    bool const has_audiodg = blocker_audiodg_count(blockers) > 0;
    bool const may_close_mixer = ctx->allow_mixer_close && (mixers > 0);
    bool const may_pause_audio = ctx->allow_audiodg_close && has_audiodg;

    if (may_close_mixer && !close_mixer_blockers(ctx, deps, blockers, REPAIR_STAGE_MIXER, closed, 64, &closed_count, err)) {
      outcome = REPAIR_OUTCOME_FAILED;
      goto cleanup;
    }
    if (cancel_requested(ctx->cancel)) {
      say_aborted(out);
      outcome = REPAIR_OUTCOME_ABORTED;
      success = true;
      goto cleanup;
    }
    if (may_pause_audio && !pause_audio_engine(ctx, deps, &audio_service_paused, err)) {
      outcome = REPAIR_OUTCOME_FAILED;
      goto cleanup;
    }
    if (!may_close_mixer && !may_pause_audio) {
      char option_buf[256] = {0};
      char const *option = NULL;
      char sentence[256];

      outcome = REPAIR_OUTCOME_VETOED;
      // the blockers the user did not allow this tool to close are still worth naming: the
      // kind that is there says which option would have helped
      if (mixers > 0) {
        repair_option_mixer_label(s, option_buf, sizeof(option_buf));
        option = option_buf;
      } else if (has_audiodg) {
        option = repair_option_audiodg_label();
      }
      repair_blocked_sentence(option, blocker_count(blockers), sentence, sizeof(sentence));
      say(out, sentence);
      if (veto_event_count(events) > 0) {
        out->events = events;
        events = NULL;
      }
      success = true;
      goto cleanup;
    }
  }

  if (cancel_requested(ctx->cancel)) {
    say_aborted(out);
    outcome = REPAIR_OUTCOME_ABORTED;
    success = true;
    goto cleanup;
  }
  since = process_now_ms();
  logger_notef("remove", "second attempt, device=%s since=%llu", target->devnode, (unsigned long long)since);
  ctx->step++;
  report_stage(ctx, REPAIR_STAGE_REMOVE);
  if (deps->removal_close(target->devnode, &removal_err)) {
    outcome = REPAIR_OUTCOME_CLOSED;
    say(out, "The device was released.");
    logger_notef("remove", "The device was released on the second attempt.");
    outcome = confirm_recovered(ctx, deps, target, err);
    success = true;
    goto cleanup;
  }
  if (cancel_requested(ctx->cancel)) {
    say_aborted(out);
    outcome = REPAIR_OUTCOME_ABORTED;
    success = true;
    goto cleanup;
  }
  current = read_veto_events(ctx, deps, since, &read_err);
  if (current != NULL) {
    veto_event_destroy_list(&events);
    events = current;
    current = NULL;
    if (!veto_event_keep_device(deps, &events, target->devnode, err)) {
      OV_ERROR_ADD_TRACE(err);
      outcome = REPAIR_OUTCOME_FAILED;
      goto cleanup;
    }
  }
  // the processes this run closed are gone from the machine, so the records that named them
  // are not reasons the device is held any more and must not be shown as ones
  veto_event_drop_pids(&events, closed, closed_count);
  if (veto_event_count(events) > 0) {
    out->events = events;
    events = NULL;
  }
  outcome = REPAIR_OUTCOME_VETOED;
  say(out, "The device is still in use.");
  logger_notef("remove", "The blockers were closed but the device is still blocked.");
  success = true;

cleanup:
  if (audio_service_paused) {
    struct ov_error resume_err = {0};
    // whatever came of the device, the sound of the machine comes back here: a service left
    // stopped is a machine with no audio at all, which is worse than the device this run
    // was about
    if (!deps->audio_service_resume(&resume_err)) {
      OV_ERROR_REPORT(&resume_err, NULL);
      ctx->out->outcome = REPAIR_OUTCOME_FAILED;
      say(ctx->out, "The Windows Audio service could not be started again.");
      outcome = REPAIR_OUTCOME_FAILED;
    }
  }
  if (success) {
    OV_ERROR_REPORT(err, NULL);
  }
  OV_ERROR_REPORT(&removal_err, NULL);
  OV_ERROR_REPORT(&read_err, NULL);
  veto_event_destroy_list(&events);
  veto_event_destroy_list(&current);
  blocker_list_destroy(&blockers);
  return outcome;
}

/**
 * @brief Run the repair
 *
 * The run repairs every device the caller names, or, when it names none, every
 * device that is faulted.  A device that does not come back as an audio device
 * stops the run, for the reason it stayed away is not one this tool knows.
 *
 * @param s the settings this run works for
 * @param targets the devices to repair, NULL says the run looks for the faulted
 *        devices itself; an empty list is the same as no device at all
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
static bool repair_run_with(struct settings const *const s,
                            struct device_target *const targets,
                            bool const allow_mixer_close,
                            bool const allow_audiodg_close,
                            repair_progress_fn const progress,
                            void *const progress_userdata,
                            bool *const cancel,
                            struct repair_result *const out,
                            struct repair_deps const *const deps,
                            struct ov_error *const err) {
  struct device_target *list = NULL;
  bool owned = false;
  bool probed = false;
  enum repair_outcome outcome = REPAIR_OUTCOME_CLOSED;
  struct repair_ctx ctx;
  size_t i = 0;
  size_t n = 0;
  bool success = false;

  if ((s == NULL) || (out == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  memset(out, 0, sizeof(*out));
  memset(&ctx, 0, sizeof(ctx));
  ctx.settings = s;
  ctx.allow_mixer_close = allow_mixer_close;
  ctx.allow_audiodg_close = allow_audiodg_close;
  // the service is only paused when this build allows it, and, in a build that ships, only
  // when the settings say so: pausing it takes the sound away from the whole machine
  ctx.can_pause_audio_service = repair_can_pause_audio_service(s);
  ctx.progress = progress;
  ctx.progress_userdata = progress_userdata;
  ctx.cancel = cancel;
  ctx.out = out;

  if (cancel_requested(cancel)) {
    say_aborted(out);
    success = true;
    goto cleanup;
  }
  if (targets != NULL) {
    list = targets;
  } else {
    ctx.step++;
    report_stage(&ctx, REPAIR_STAGE_PROBE);
    list = device_probe_all(s->ks_service, s->devices, OV_ARRAY_LENGTH(s->devices), err);
    if (list == NULL) {
      if (!ov_error_is(err, ov_error_type_generic, ov_error_generic_not_found)) {
        OV_ERROR_ADD_TRACE(err);
        out->outcome = REPAIR_OUTCOME_FAILED;
        say(out, "The device state could not be determined.");
        goto cleanup;
      }
      // no device of the service is present, which the run answers as ABSENT below
      OV_ERROR_DESTROY(err);
    }
    owned = true;
    probed = true;
    device_targets_keep_faulted(&list);
  }
  n = OV_ARRAY_LENGTH(list);
  if (n == 0) {
    if (cancel_requested(cancel)) {
      say_aborted(out);
      success = true;
      goto cleanup;
    }
    out->outcome = REPAIR_OUTCOME_ABSENT;
    say(out, "No Audient device was found.");
    success = true;
    goto cleanup;
  }
  ctx.total = (probed ? 1 : 0) + (n * REPAIR_STEPS_PER_TARGET);
  for (i = 0; i < n; i++) {
    enum repair_outcome const one = repair_target(&ctx, deps, &list[i], err);
    if (one == REPAIR_OUTCOME_ABORTED) {
      outcome = one;
      break;
    }
    if (worse_outcome(one, outcome) == one) {
      outcome = one;
    }
    if (one == REPAIR_OUTCOME_FAILED) {
      break; // the reason is not known, so the run stops here
    }
  }
  out->outcome = outcome;
  success = true;

cleanup:
  if (success) {
    OV_ERROR_REPORT(err, NULL);
  }
  if (owned) {
    device_targets_release(&list);
  }
  return success;
}

bool repair_run(struct settings const *const s,
                struct device_target *const targets,
                bool const allow_mixer_close,
                bool const allow_audiodg_close,
                repair_progress_fn const progress,
                void *const progress_userdata,
                bool *const cancel,
                struct repair_result *const out,
                struct ov_error *const err) {
  return repair_run_with(s, targets, allow_mixer_close, allow_audiodg_close, progress, progress_userdata, cancel, out, &kDefaultDeps, err);
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
  veto_event_destroy_list(&r->events);
  r->outcome = REPAIR_OUTCOME_UNKNOWN;
}
