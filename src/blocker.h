#pragma once

#include <ovbase.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct veto_event;

/**
 * @brief What kind of process holds a device
 */
enum blocker_kind {
  BLOCKER_KIND_MIXER,   //!< one of the mixer image names the settings carry
  BLOCKER_KIND_AUDIODG, //!< the audio engine, audiodg.exe
};

/**
 * @brief One process that holds a device, as the machine states it now
 *
 * A veto record names a process id and the moment the record was written, and nothing else
 * about it can be acted on: what the record says the process was called is what it was
 * called then.  The id is looked up in the machine as it is at this moment, and the process
 * enters this list only when it is one this tool may close at all.
 *
 * What the list is for is deciding whom to close.  The name of the running process is what
 * decides, and whether the process is really the one the record names is settled by the
 * creation time, read from the process itself at the moment it is opened.  The two are
 * separate: the name is read once for the whole list, the time only for the process that is
 * about to be closed.
 */
struct blocker {
  uint32_t pid;
  uint64_t event_time_ms; //!< the moment of the record that named it
  enum blocker_kind kind; //!< what the running process is
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
struct blocker *blocker_list_build(struct veto_event const *const events,
                                   char const *const *const mixer_names,
                                   size_t const mixer_name_count,
                                   struct ov_error *const err);

/**
 * @brief The number of processes in the list
 *
 * @param list NULL is allowed
 * @return the number, 0 for NULL
 */
size_t blocker_count(struct blocker const *const list);

/**
 * @brief How many of the entries are the mixer
 *
 * @param list the list to count in, may be NULL
 * @return how many entries are the mixer
 */
size_t blocker_mixer_count(struct blocker const *const list);

/**
 * @brief How many of the entries are the audio engine
 *
 * @param list the list to count in, may be NULL
 * @return how many entries are the audio engine
 */
size_t blocker_audiodg_count(struct blocker const *const list);

/**
 * @brief Terminate one process and wait until it is gone
 *
 * The process is opened at this moment, and the open is refused unless it still is the one
 * the record names: a process that started after the record was written cannot be it, the
 * id was given to somebody else in between.  The handle stays open from the check to the
 * termination, so the instance that was checked is the instance that goes.
 *
 * The right to terminate, and the privilege the audio engine needs to be opened at all, are
 * asked for only when the caller allows them.
 *
 * @param b the process to close
 * @param allow_privilege allow SeDebugPrivilege when a plain open was refused
 * @param err receives the failure
 * @return false when the process could not be closed
 */
bool blocker_close(struct blocker const *const b, bool const allow_privilege, struct ov_error *const err);

/**
 * @brief Release the list
 *
 * @param list set to NULL afterwards
 */
void blocker_list_destroy(struct blocker **const list);
