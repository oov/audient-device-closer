#pragma once

#include <ovbase.h>

#include <wchar.h>

struct veto_event;

/** @brief The list of processes a veto record names, built from the machine as it is now */
struct blocker;

/**
 * @brief Where a walk of the list stands
 *
 * Zero it before the first call and hand the same one back until the walk is over.  The
 * fields are the entry the walk just reached; the caller does not write them.
 */
struct blocker_iter {
  size_t idx;              //!< position in the list, the entry is at this one
  uint32_t pid;            //!< the process the record named
  uint64_t event_time_ms;  //!< the moment of the record that named it
  size_t mixer_name_index; //!< which of the mixer names it is, SIZE_MAX when it is the audio engine
};

/**
 * @brief Look every record up in the machine and keep the processes this tool may close
 *
 * The image of each named process is read from a snapshot of the running processes, which
 * reads the machine without opening anything and without a privilege, so a process that is
 * not one this tool closes is never opened at all.  A process whose image is neither one of
 * the mixer names nor the audio engine is left out of the list.
 *
 * The moment of the record and the id travel with the entry, so whoever closes it can still
 * tell the one the record names from a process that took the id over in between.
 *
 * @param events the records of the veto, may be NULL
 * @param mixer_names the image names the settings carry for the mixer
 * @param mixer_name_count how many names that is
 * @param err receives the failure of the snapshot
 * @return the processes to close, an empty list when no record names one of them;
 *         release with blocker_list_destroy
 */
NODISCARD struct blocker *blocker_list_build(struct veto_event const *const events,
                                             wchar_t const *const *const mixer_names,
                                             size_t const mixer_name_count,
                                             struct ov_error *const err);

/**
 * @brief Take the next process of the list
 *
 * The entries come out in the order the list keeps them: that is the order the records were
 * written in, with the ones that named no running process left out.  A mixer and the audio
 * engine both come out; mixer_name_index says which one an entry is.
 *
 * @param list the list to walk, NULL walks nothing
 * @param it where the walk stands, zeroed before the first call
 * @return true when it now holds the next entry, false when the walk is over
 */
bool blocker_next(struct blocker const *const list, struct blocker_iter *const it);

/**
 * @brief Release the list
 *
 * @param list set to NULL afterwards
 */
void blocker_list_destroy(struct blocker **const list);
