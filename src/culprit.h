#pragma once

#include <ovbase.h>

#include <stddef.h>
#include <stdint.h>

/**
 * @brief One veto event as the event log recorded it
 */
struct culprit {
  long long event_time_ms; // epoch milliseconds, UTC, 0 when unknown
  uint32_t pid;
  char *path;            // UTF-8, as reported by the veto event (\Device\HarddiskVolumeN\...)
  char *command_line;    // UTF-8
  char *device_instance; // UTF-8, the device whose removal was stopped
};

/**
 * @brief Parse one rendered event into a record
 *
 * The input is one rendered event (EvtRenderEventXml).
 *
 * @param xml_utf8 the event as EvtRenderEventXml produced it, UTF-8
 * @param out filled in when the event names a process; the strings become the caller's
 * @return false when the event carries no readable process id, true otherwise
 */
bool culprit_parse_xml(char const *const xml_utf8, struct culprit *const out);

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
struct culprit *
culprit_read(char const *const log_name, char const *const provider_name, unsigned short const event_id, struct ov_error *const err);

/**
 * @brief Drop what the caller must not act on any more
 *
 * Events older than since_ms and duplicates of the same pid + device go,
 * the newest of the duplicates is kept.
 *
 * @param list the list to shrink in place
 * @param since_ms events older than this are dropped, 0 keeps every age
 */
void culprit_prune(struct culprit **const list, long long const since_ms);

/**
 * @brief Number of records in the list
 *
 * @param list NULL is allowed
 * @return the number of records, 0 for NULL
 */
size_t culprit_count(struct culprit const *const list);

/**
 * @brief Drop every entry that names one of the given processes
 *
 * A process this tool closed on its own is not a reason the device was blocked, so it must
 * not appear in the list the user is shown.  The caller passes the processes it closed, not
 * the image names, so an entry is dropped only when it is the exact instance that was
 * closed: the same image running somewhere else is a different blocker.
 *
 * @param list   the list to shrink in place
 * @param pids   the process ids that were closed
 * @param count  how many of them there are
 */
void culprit_drop_pids(struct culprit **list, uint32_t const *pids, size_t count);

/**
 * @brief Drop the members of the list and leave it empty
 *
 * Used when the device was closed: nothing blocks it any more, so whatever a record named
 * before is history.  An empty list and an absent one mean the same thing to the caller.
 *
 * @param list the list to empty in place
 */
void culprit_clear(struct culprit **list);

/**
 * @brief Release the list and every record in it
 *
 * @param list set to NULL afterwards
 */
void culprit_destroy_list(struct culprit **const list);
