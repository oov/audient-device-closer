#pragma once

#include <ovbase.h>

#include <wchar.h>

/**
 * @brief One veto event as the event log recorded it
 *
 * What the log states at the moment it stated it: which process was named, when it was
 * named, which device it blocked, and what that process was called.  The path is the
 * record of what happened, so it is what the reader is shown.
 *
 * A caller that only reads the list walks it with veto_event_next rather than indexing it:
 * what it does with the records is a walk, and how the list keeps them is its own business.
 */
struct veto_event {
  uint64_t event_time_ms; // epoch milliseconds, UTC, 0 when unknown
  uint32_t pid;
  wchar_t *path;            // what the process was called then.  Shown, never acted on
  wchar_t *device_instance; // the device whose removal was stopped
};

/**
 * @brief Where a walk of the list stands
 *
 * Zero it before the first call and hand the same one back until the walk is over.  The
 * fields are the record the walk just reached; the caller does not write them.
 */
struct veto_event_iter {
  size_t idx;               //!< position in the list, the record is at this one
  uint32_t pid;             //!< the process the record named
  uint64_t event_time_ms;   //!< the moment of the record, epoch milliseconds UTC
  wchar_t *path;            //!< what the process was called then, borrowed from the list
  wchar_t *device_instance; //!< the device whose removal was stopped, borrowed from the list
};

/**
 * @brief Where a list that is being built takes its records
 *
 * The builder hands one of these to the emitter, and the emitter calls emit for every
 * event it has.  What a record is made of is the business of the builder: the emitter hands
 * over the event as the log writes it, and the list keeps what could be read out of it.  A
 * caller never makes one of these itself; it is what the emitter is given.
 */
struct veto_event_context {
  /**
   * @brief Take one event into the list
   *
   * An event the reader makes nothing of is not a failure: the log holds such records, and
   * a list that kept them would be a list of guesses.  It is left out, and true is answered.
   *
   * @param ctx the context itself
   * @param xml the event as the log writes it
   * @param err receives the failure
   * @return false when the list could not take the event
   */
  bool (*emit)(struct veto_event_context *const ctx, wchar_t const *const xml, struct ov_error *const err);
};

/**
 * @brief Build a list of the records an emitter reads
 *
 * The emitter is called once and hands every event it has to ctx->emit, as often as it has
 * one.  What it reads them from is its own business: the log of the system, a script of a
 * stand-in, or anything else that can be written as an event.
 *
 * @param emitter called once, handing its events to ctx->emit
 * @param userdata handed to the emitter
 * @param err receives the failure of the emitter or of the list
 * @return the records, empty but never NULL when the emitter answered; NULL with err set
 *         when it did not; release with veto_event_destroy_list
 */
NODISCARD struct veto_event *
veto_event_list_build(bool (*emitter)(struct veto_event_context *const ctx, void *const userdata, struct ov_error *const err),
                      void *const userdata,
                      struct ov_error *const err);

/**
 * @brief Take the next record of the list
 *
 * The records come out in the order the log wrote them.
 *
 * @param list the list to walk, NULL walks nothing
 * @param it where the walk stands, zeroed before the first call
 * @return true when it now holds the next record, false when the walk is over
 */
NODISCARD bool veto_event_next(struct veto_event const *const list, struct veto_event_iter *const it);

/**
 * @brief Release the list and every record in it
 *
 * @param list set to NULL afterwards
 */
void veto_event_destroy_list(struct veto_event **const list);
