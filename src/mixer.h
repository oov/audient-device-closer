#pragma once

#include <ovbase.h>

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief The process instance a close acts on
 *
 * Closing a process is a policy on top of process.c: the repair decides which process, and
 * this module carries out that decision and keeps what is needed to start it again.
 *
 * The target is an exact process instance, not a name: the caller gets it from the veto
 * record, so nothing else can be hit, even when another process with the same image name is
 * running somewhere else.
 */
struct mixer_target {
  uint32_t pid;
  char *path;         // UTF-8, win32 path of the running image, as taken before the close
  char *command_line; // UTF-8, as it was actually launched (may be NULL)
};

/**
 * @brief Take the identity of the process the record named
 *
 * It is checked that this is the instance the caller means: it must be alive, its image name
 * must match exe_name, and it must have started at or before event_time_ms (see
 * process_start_matches_event_time).  A mismatch is not an error: out_found stays false and
 * the reason goes to the log.
 *
 * @param pid the process the record named
 * @param exe_name the image name the caller expects
 * @param event_time_ms the moment of the veto record
 * @param out_found false when no instance matched, true when out carries one
 * @param out the identity to act on, release with mixer_target_release
 * @param err receives a hard failure, not a mismatch
 * @return false only on a hard failure
 */
bool mixer_take(uint32_t const pid,
                char const *const exe_name,
                long long const event_time_ms,
                bool *const out_found,
                struct mixer_target *const out,
                struct ov_error *const err);

/**
 * @brief Terminate the taken instance and wait until it is gone
 *
 * The saved path and command line stay usable afterwards, so the caller can restart it from
 * them.
 *
 * @param target the instance mixer_take found
 * @param err receives the failure of the termination
 * @return false when the process could not be closed
 */
bool mixer_close(struct mixer_target *const target, struct ov_error *const err);

/**
 * @brief Release what mixer_take kept
 *
 * @param target NULL is allowed
 */
void mixer_target_release(struct mixer_target *const target);
