#pragma once

#include <ovbase.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief One process of the list the snapshot returned
 */
struct process_entry {
  uint32_t pid;
  char *path; // UTF-8 win32 path, NULL when it could not be read
};

/**
 * @brief The processes that are running right now
 *
 * @param exe_name narrows the result to this image name (UTF-8, e.g. "iD.exe"), NULL lists
 *                 every process
 * @param err receives the failure of the walk
 * @return NULL only when the listing failed, an empty array is a normal answer
 * @note Release with process_list_destroy.
 */
struct process_entry *process_list(char const *const exe_name, struct ov_error *const err);

/**
 * @brief Number of entries in the list
 *
 * @param list NULL is allowed
 * @return the number of entries, 0 for NULL
 */
size_t process_count(struct process_entry const *const list);

/**
 * @brief Release the list and every entry in it
 *
 * @param list set to NULL afterwards
 */
void process_list_destroy(struct process_entry **const list);

/**
 * @brief The command line a process was started with
 *
 * Read from its PEB.
 *
 * @param pid the process to ask
 * @param err receives the refusal of the system
 * @return the command line in UTF-8, NULL when it could not be read
 * @note Release with process_string_free.
 */
char *process_command_line(uint32_t const pid, struct ov_error *const err);

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
 * handle stays bound to the instance it was opened on.  All of the checks below run
 * through the same handle, and the termination uses that handle.
 */
struct process_ref {
  void *handle;           // HANDLE, kept opaque so this header stays free of windows.h
  uint32_t pid;           // pid of the instance the handle belongs to
  long long start_ms_utc; // creation time, epoch milliseconds UTC, 0 when unknown
};

/**
 * @brief Open one exact process instance
 *
 * Opens the process with just enough rights: the cheap case asks for the rights the
 * caller needs, and a refused open is retried once with SeDebugPrivilege enabled, because
 * the audio engine is owned by another account and lets nobody else in.  The privilege is
 * switched off again right after the open, so it is on for as short as possible.
 *
 * @param out receives the instance, release with process_ref_close
 * @param pid the process to open
 * @param need_terminate ask for the right to terminate the process
 * @param err receives the refusal of the system
 * @return false when the process could not be opened
 */
bool process_ref_open(struct process_ref *const out, uint32_t const pid, bool const need_terminate, struct ov_error *const err);

/**
 * @brief Is the instance still running
 *
 * True when this handle still refers to a live process (GetExitCodeProcess ==
 * STILL_ACTIVE).
 *
 * @param ref the instance to look at
 * @return true while the process is alive
 */
bool process_ref_alive(struct process_ref const *const ref);

/**
 * @brief The win32 path of the image this instance runs
 *
 * @param ref the instance to look at
 * @param err receives the refusal of the system
 * @return the path in UTF-8, NULL when it could not be read
 * @note Release with process_string_free.
 */
char *process_ref_image_path(struct process_ref const *const ref, struct ov_error *const err);

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
 * out_gone tells whether it exited in time; a process that is already gone counts as gone.
 *
 * @param ref the instance to wait for
 * @param timeout_ms how long to wait
 * @param out_gone receives whether the process is gone
 * @param err receives the failure of the wait
 * @return false when the wait could not be answered
 */
bool process_ref_wait(struct process_ref const *const ref, uint32_t const timeout_ms, bool *const out_gone, struct ov_error *const err);

/**
 * @brief Give the handle back
 *
 * @param ref the instance to close, NULL is allowed
 */
void process_ref_close(struct process_ref *const ref);

/**
 * @brief Wait until a process is gone, without holding a handle on it
 *
 * out_gone tells whether it actually exited in time; a pid that cannot be opened counts as
 * gone.
 *
 * @param pid the process to wait for
 * @param timeout_ms how long to wait
 * @param out_gone receives whether the process is gone
 * @param err receives the failure of the wait
 * @return false when the wait could not be answered
 */
bool process_wait(uint32_t const pid, uint32_t const timeout_ms, bool *const out_gone, struct ov_error *const err);

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
bool process_start_matches_event_time(long long const start_ms_utc, long long const event_time_ms);
