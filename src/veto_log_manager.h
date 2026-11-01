#pragma once

#include <ovbase.h>

#include <wchar.h>

#include "async.h"

struct veto_event;

/**
 * @brief One lookup of the records of a refused removal, running on a thread of its own
 */
struct veto_log_wait;

/**
 * @brief The record the system kept of a removal it refused
 *
 * One question, because looking up the record of one attempt is one act: whoever answers it
 * either hands back what the log holds or says why it could not be read.  The read waits for
 * a record that is written a moment later, which takes seconds on a machine under load, so it
 * is made on a thread of its own rather than on the one that has the window.
 */
struct veto_log_manager {
  /**
   * @brief Look up the records of the attempts made since a moment
   *
   * The wait is answered by the log, not by asking again and again, and records older than
   * since_ms belong to an earlier attempt.  The end is called from the thread of the lookup,
   * and the answer is read with wait_take once it was called.
   *
   * @param log_name event log that holds the records, may not be NULL
   * @param provider_name provider that writes them, may not be NULL
   * @param event_id the veto event of that provider
   * @param since_ms records older than this are dropped
   * @param max_wait_ms how long to wait at most, milliseconds
   * @param on_done called from the thread of the lookup when it is over, may be NULL
   * @param userdata handed to on_done
   * @param err receives the failure to start the lookup and, on the thread of the lookup, the
   *        failure of the lookup; it has to outlive the lookup: what the lookup raises is
   *        written into it while wait_take waits for it to be over.  NULL is allowed and
   *        drops both
   * @return the lookup, read with wait_take; NULL when it could not be started
   */
  struct veto_log_wait *(*wait)(wchar_t const *log_name,
                                wchar_t const *provider_name,
                                unsigned short event_id,
                                uint64_t since_ms,
                                unsigned long max_wait_ms,
                                async_notify_fn notify,
                                void *userdata,
                                struct ov_error *err);

  /**
   * @brief Hand back what the lookup read and release it
   *
   * Called on the caller's own thread, once the end of the lookup was called.  A lookup the
   * caller asked to stop, or one that ran out of its bound, hands back an empty list: both
   * are answers, and the caller tells them apart by what it asked for.
   *
   * @param w the lookup, set to NULL afterwards
   * @return the records, empty but never NULL when the lookup itself did not fail; NULL when
   *         it did, with the failure in the err the lookup was started with
   */
  struct veto_event *(*wait_take)(struct veto_log_wait **w);

  /**
   * @brief Ask the lookup to stop, from the caller's thread
   *
   * The wait of the lookup is answered at once, and wait_take hands back what was read up to
   * that moment.  It has to be the thread that started the lookup: the caller's own state is
   * what the ask reaches, and breaking this is a fault of the caller.
   *
   * @param w the lookup
   */
  void (*wait_cancel)(struct veto_log_wait *w);
};

/**
 * @brief The log this program reads until another one is named
 *
 * @return the manager in use, never NULL
 */
struct veto_log_manager const *veto_log_manager(void);

#ifdef ADC_MOCK
/**
 * @brief Work against another log from here on
 *
 * @param manager the log to read, NULL to go back to the log of the system
 */
void veto_log_manager_select(struct veto_log_manager const *manager);
#endif // ADC_MOCK

/**
 * @brief The log of the system itself
 *
 * @return the manager that reads the event log, never NULL
 */
struct veto_log_manager const *veto_log_manager_system(void);
