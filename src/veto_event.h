#pragma once

#include <ovbase.h>

#include <stddef.h>
#include <stdint.h>

/**
 * @brief One veto event as the event log recorded it
 *
 * What the log states at the moment it stated it: which process was named, when it was
 * named, which device it blocked, and what that process was called.  The path is the
 * record of what happened, so it is what the reader is shown.
 */
struct veto_event {
  uint64_t event_time_ms; // epoch milliseconds, UTC, 0 when unknown
  uint32_t pid;
  char *path;            // UTF-8, what the process was called then.  Shown, never acted on
  char *device_instance; // UTF-8, the device whose removal was stopped
};

/**
 * @brief Read the veto events of the last 24 hours
 *
 * They come in the order the log returns them.
 *
 * @param log_name event log that holds the records
 * @param provider_name provider that writes them
 * @param event_id the veto event of that provider
 * @param err receives the failure of the query itself
 * @return NULL only when the query itself failed; an empty array means there is no veto event
 */
struct veto_event *
veto_event_read(char const *const log_name, char const *const provider_name, unsigned short const event_id, struct ov_error *const err);

/**
 * @brief Drop what the caller must not act on any more
 *
 * Events older than since_ms and duplicates of the same pid + device go,
 * the newest of the duplicates is kept.
 *
 * @param list the list to shrink in place
 * @param since_ms events older than this are dropped, 0 keeps every age
 */
void veto_event_prune(struct veto_event **const list, uint64_t const since_ms);

/**
 * @brief Number of records in the list
 *
 * @param list NULL is allowed
 * @return the number of records, 0 for NULL
 */
size_t veto_event_count(struct veto_event const *const list);

/**
 * @brief Drop every record that names one of the given processes
 *
 * A process this run closed is gone from the machine, so the record that named it is no
 * longer a reason the device is held and must not be shown as one.  The caller passes the
 * ids it closed, so a record that names anybody else stays.
 *
 * @param list  the list to shrink in place
 * @param pids  the process ids that were closed
 * @param count how many of them there are
 */
void veto_event_drop_pids(struct veto_event **list, uint32_t const *pids, size_t count);

/**
 * @brief Release the list and every record in it
 *
 * @param list set to NULL afterwards
 */
void veto_event_destroy_list(struct veto_event **const list);
