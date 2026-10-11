#pragma once

#include <ovbase.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "removal.h"

struct settings;
struct culprit;

/**
 * @brief What came of the run
 *
 * The device is not in the state this tool is for, and the two ways that happens need
 * different sentences: a device that is not there at all, and a device that is there with a
 * problem the removal does not cure.
 */
enum repair_outcome {
  REPAIR_OUTCOME_UNKNOWN,
  REPAIR_OUTCOME_ABSENT,        // no device of that service is present
  REPAIR_OUTCOME_OTHER_PROBLEM, // the device is there, but not with the problem this tool removes
  REPAIR_OUTCOME_CLOSED,        // the removal succeeded
  REPAIR_OUTCOME_VETOED,        // still blocked; culprits may name who blocks it
  REPAIR_OUTCOME_FAILED,        // any other failure
  REPAIR_OUTCOME_ABORTED,       // the reader stopped it and it quit between its steps
};

/**
 * @brief What the caller should tell the user
 *
 * The outcome of the run and, when the system refused the removal, why.  The sentences
 * themselves are chosen by the UI, next to the dialog that shows them; the numbers stay in
 * the log.
 */
struct repair_result {
  enum repair_outcome outcome;
  enum removal_reason removal_reason; // REMOVAL_REASON_NONE unless the removal was refused
  struct culprit *culprits;           // ovarray or NULL.  released by repair_result_release
  char message[512];                  // UTF-8, meant for the UI log
};

/**
 * @brief The words of the setting that lets this tool close the mixer
 *
 * The window puts them on its checkbox and the log quotes them when a veto is left
 * standing, so they are said once here.  They are translated: whoever reads the log of a
 * window in their own language meets the words they can press.  The names come from the
 * settings: the display name of the mixer and its image name.
 *
 * @param s the settings that name the mixer, may be NULL for the defaults
 * @param out receives the caption, UTF-8, always terminated
 * @param out_size room of out
 */
void repair_option_mixer_label(struct settings const *const s, char *const out, size_t const out_size);

/**
 * @brief The words of the setting that lets this tool close the audio engine
 *
 * @return the caption, UTF-8
 */
char const *repair_option_audiodg_label(void);

/**
 * @brief The process the veto record names as a blocker this tool may close
 *
 * The answer is the exact instance the record points at: its pid and the time the record was
 * written.  Closing is then carried out through that identity, never through a name search,
 * so a process with the same image name somewhere else is out of reach by construction.
 */
struct repair_blocker {
  bool found; //!< false when the record names only blockers this tool may not close
  uint32_t pid;
  long long event_time_ms;
};

/**
 * @brief Maximum number of blockers this tool will close at once
 */
#define REPAIR_MAX_BLOCKERS 16

/**
 * @brief The blockers the run may close, in the order they are closed
 *
 * The same image may run several times on this machine, one per logged-in session, so the
 * records may name more than one blocker per image; every one of them is closed.
 */
struct repair_blockers {
  struct repair_blocker mixer[REPAIR_MAX_BLOCKERS];
  struct repair_blocker audiodg[REPAIR_MAX_BLOCKERS];
  size_t mixer_count;
  size_t audiodg_count;
};

/**
 * @brief The audio engine, a blocker as often as the mixer is
 *
 * Its image name is the same everywhere, so it is not part of the settings.
 */
#define REPAIR_AUDIODG_NAME "audiodg.exe"

/**
 * @brief What the run is waiting for at the moment
 *
 * The caller can show it while it happens.
 */
enum repair_stage {
  REPAIR_STAGE_WAIT,       // the devices are still coming back on the bus
  REPAIR_STAGE_PROBE,      // the state of the device is being determined
  REPAIR_STAGE_REMOVE,     // the removal is with the system, waiting for its answer
  REPAIR_STAGE_MIXER,      // the mixer is being closed and waited for
  REPAIR_STAGE_AUDIODG,    // audiodg is being closed and waited for
  REPAIR_STAGE_VETO_RECORD // the veto record is being waited for and read
};

/**
 * @brief The longest way through the run, in steps
 *
 * Probe, remove, the veto record, the two closable blockers and then the second removal with
 * its veto record.  A shorter way stops early; step counts up to this total.  The wait a
 * scheduled run starts with is counted by the caller on top of these.
 */
enum { REPAIR_STEPS_TOTAL = 7 };

/**
 * @brief Reports the progress of a run
 *
 * @param stage what the run is waiting for
 * @param step the step of the run, counted from one
 * @param total how many steps the longest way takes
 * @param userdata what the caller handed to repair_run
 */
typedef void (*repair_progress_fn)(enum repair_stage const stage, size_t const step, size_t const total, void *const userdata);

/**
 * @brief Run the repair
 *
 * The documented sequence: remove, look up the culprits when vetoed, optionally close the
 * blockers (the mixer and audiodg) and remove again.  It never touches anything outside the
 * settings.
 *
 * @param s the settings this run works for
 * @param allow_mixer_close the user allowed closing the mixer
 * @param allow_audiodg_close the user allowed closing the audio engine
 * @param progress may be NULL; it is called from the thread that runs the repair
 * @param progress_userdata handed back to progress
 * @param cancel may be NULL; a flag the caller owns.  The run reads it between its steps
 *        and ends with REPAIR_OUTCOME_ABORTED instead of walking into the next one, it
 *        never interrupts a step that is already going.
 * @param out receives what happened; release with repair_result_release
 * @param err receives the failure that stopped the run
 * @return false only on a hard failure.  The outcome carries the news: a step whose failure
 *         is folded into the outcome releases its error into the log before the next step
 *         starts, so err is only worth reading when the call returns false.
 */
bool repair_run(struct settings const *const s,
                bool const allow_mixer_close,
                bool const allow_audiodg_close,
                repair_progress_fn const progress,
                void *const progress_userdata,
                bool *const cancel,
                struct repair_result *const out,
                struct ov_error *const err);

/**
 * @brief Release what a run collected
 *
 * @param r NULL is allowed
 */
void repair_result_release(struct repair_result *const r);
