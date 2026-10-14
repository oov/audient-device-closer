#pragma once

#include <ovbase.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void process_string_free(char **const value);

/**
 * @brief Does an image path end with the given name
 *
 * The image name at the end of a path, without the directories.  This is the same rule the
 * repair uses to read the name out of a veto record, so it lives next to it.  Case is
 * ignored; the name has to start right after a separator, so "foo.exe" never matches
 * "foobar.exe".
 *
 * @param path the path to look at
 * @param name the image name to look for
 * @return true when the last component of path is name
 */
bool process_path_matches_name(char const *const path, char const *const name);

/**
 * @brief A handle on one process instance
 *
 * Windows reuses process ids, so a pid on its own does not identify a process: only a
 * handle stays bound to the instance it was opened on.  Everything below runs through the
 * handle, and while one handle is held the pid cannot move on to another process.
 */
struct process_ref {
  void *handle; // HANDLE, kept opaque so this header stays free of windows.h
};

/**
 * @brief Open one exact process instance
 *
 * Opens the process with just enough rights: the rights the caller needs, and no more.
 * The privilege is asked for only when the caller allows it and a plain open was refused
 * for lack of access, because the audio engine is owned by another account and lets nobody
 * else in.  It is switched off again right after the open.  A caller that reads a process
 * never allows it: opening what a record names must not need the right to take the machine
 * over.
 *
 * @param out receives the instance, release with process_ref_close
 * @param pid the process to open
 * @param need_terminate ask for the right to terminate the process
 * @param allow_privilege allow SeDebugPrivilege when a plain open was refused
 * @param err receives the refusal of the system
 * @return false when the process could not be opened
 */
bool process_ref_open(
    struct process_ref *const out, uint32_t const pid, bool const need_terminate, bool const allow_privilege, struct ov_error *const err);

/**
 * @brief Is the instance still running
 *
 * @param ref the instance to look at
 * @param err receives the failure of the question
 * @return ov_true while the process is alive, ov_false when it has exited, and
 *         ov_indeterminate when whether it is alive could not be read, with err set
 */
ov_tribool process_ref_alive(struct process_ref const *const ref, struct ov_error *const err);

/**
 * @brief Terminate this instance
 *
 * It does not touch any other process, even one that has taken over the pid in the
 * meantime.
 *
 * @param ref the instance to close
 * @param err receives the refusal of the system
 * @return false when the process could not be terminated
 */
bool process_ref_terminate(struct process_ref const *const ref, struct ov_error *const err);

/**
 * @brief Wait until this instance is gone
 *
 * The wait counts as answered when the process is already gone.
 *
 * @param ref the instance to wait for
 * @param timeout_ms how long to wait
 * @param err receives the failure of the wait
 * @return ov_true when the process is gone, ov_false when it is still there after the
 *         timeout, ov_indeterminate when the wait could not be answered, with err set
 */
ov_tribool process_ref_wait(struct process_ref const *const ref, uint32_t const timeout_ms, struct ov_error *const err);

/**
 * @brief Give the handle back
 *
 * @param ref the instance to close, NULL is allowed
 */
void process_ref_close(struct process_ref *const ref);

/**
 * @brief Did the process start before the event it is measured against
 *
 * A pid is reused only after its previous owner is gone, so a process that started at or
 * before the event was the one the event names, and one that started after it cannot be.
 *
 * @param start_ms_utc when the process started, epoch milliseconds UTC
 * @param event_time_ms when the event was written, epoch milliseconds UTC
 * @return false when one of the two is unknown or the process is younger than the event
 */
bool process_start_matches_event_time(uint64_t const start_ms_utc, uint64_t const event_time_ms);

/**
 * @brief Did this instance start before the event it is measured against
 *
 * The creation time is read from the handle itself, so the answer is about the instance the
 * handle is bound to and no other.
 *
 * @param ref the instance to look at
 * @param event_time_ms when the event was written, epoch milliseconds UTC
 * @param err receives the failure of the read
 * @return ov_true when the instance started at or before the event, ov_false when it
 *         started after it, and ov_indeterminate when the time could not be read, with err
 *         set
 */
ov_tribool process_ref_started_before_event(struct process_ref const *const ref, uint64_t const event_time_ms, struct ov_error *const err);

/**
 * @brief The moment now, in epoch milliseconds UTC
 *
 * @return the moment in epoch milliseconds UTC
 */
uint64_t process_now_ms(void);
