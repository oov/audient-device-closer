#pragma once

#include <ovbase.h>

/**
 * @brief One long operation, running on a thread of its own
 *
 * A run that has a window must not sit on its own thread while the machine is asked
 * something that takes seconds, so the work is handed here and the thread of the operation
 * reports back when it is over.  Every call on this side is answered at once.
 */
struct async;

/**
 * @brief How the caller is told what came of the operation
 *
 * It is called at the two moments the caller waits for, and which of them it is says which
 * thread it runs on: one is the ask to stop, made on the thread that asked; the other is the
 * body being over, made on the thread of the operation.
 */
typedef void (*async_notify_fn)(void *userdata, bool stopped);

/**
 * @brief What a body is given, to reach the operation
 *
 * The body is not handed the operation itself, and how it waits is its own affair: it makes
 * whatever it waits on and waits for that and for the ask to stop at once.  A stop is then
 * taken the moment it is asked for, and nothing has to look again to find out, so no thread
 * is kept aside for it and no wait is cut short by asking over and over.
 */
struct async_context {
  /**
   * @brief Was the operation asked to stop
   *
   * For the moments between two waits, where there is nothing to wait on that the answer
   * would come in through.
   *
   * @param ctx this
   * @return true when it was asked to stop
   */
  bool (*stopped)(struct async_context *ctx);

  void *userdata; //!< what the caller handed to async_start
};

/**
 * @brief The body of an operation
 *
 * Runs on the thread of the operation, so it may not touch what belongs to the caller's
 * thread.  It hands in what tells it to stop, waits for that and for what it is doing at
 * once, and gives up when the caller asked it to.  What it found is left where the caller can
 * read it, which the caller does only once the end was called.
 *
 * @param ctx the operation, as this thread may reach it
 */
typedef void (*async_body_fn)(struct async_context *ctx);

/**
 * @brief The end of an operation
 *
 * Called from the thread of the operation once the body returned, through the call named at
 * async_start with stopped false.  This is the one moment that thread reaches the caller's
 * world, so it must be cheap and must not wait: it posts a message and returns, and the
 * caller reads the answer on its own thread afterwards.
 */

/**
 * @brief Start an operation on a thread of its own
 *
 * What wakes the caller when a stop is asked for is named here and nowhere else, so an ask is
 * answered for the whole life of the operation: there is no moment at which the operation
 * can be asked to stop and nothing is there to hear it.
 *
 * @param body what the thread runs, may not be NULL
 * @param userdata handed to the body and to notify
 * @param notify called with stopped true on the thread that asks the operation to stop, and
 *        with stopped false on the thread of the operation once the body returned; may be
 *        NULL when the caller watches for neither
 * @param err receives the failure to start the thread
 * @return the operation, release with async_release when it is over; NULL when it could not
 *         be started
 */
NODISCARD struct async *async_start(async_body_fn body, void *userdata, async_notify_fn notify, struct ov_error *err);

/**
 * @brief Ask the operation to stop
 *
 * What the body handed in is called here and now, on this thread, so the wait of the body is
 * over at once rather than when it happens to run out.
 *
 * Only the thread that started the operation may call this: what the body handed in
 * carries state it made, and that state is gone once the body ended, so an ask from another
 * thread could be handed state that is already gone.  A caller that breaks this is stopped
 * on it while the program is built with assertions: it is a fault of the caller, not a
 * failure the operation can answer.
 *
 * @param a the operation
 */
void async_cancel(struct async *a);

/**
 * @brief Wait for the operation to be over and release it
 *
 * The caller either has the operation tell it the body returned and comes here when that
 * arrived, or calls this where it has nothing else to do.  A body that was told to stop is
 * expected to return; one that cannot makes this wait as long as it takes.
 *
 * Only the thread that started the operation may call this, and only once no ask is on its
 * way any more: this is the call that ends the body, so what the body made for an ask is
 * gone once it returns.  A caller that breaks this is stopped on it while the program is
 * built with assertions: it is a fault of the caller, not a failure the operation can
 * answer.
 *
 * @param a the operation, set to NULL afterwards, NULL is allowed
 * @param err receives the failure of the join
 * @return false when the thread of the operation could not be joined
 */
NODISCARD bool async_release(struct async **a, struct ov_error *err);
