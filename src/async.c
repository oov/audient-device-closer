#include "async.h"

#include <ovbase.h>
#include <ovthreads.h>

#include <assert.h>
#include <string.h>

/**
 * @brief One operation on its thread, as this file puts it together
 *
 * The context sits first, so what the body holds is this very object seen through the calls
 * it was published as.  Nothing of the rest is reachable from outside this file.
 *
 * The lock is held only while the answer and the call are read or written, never across a
 * wait: this file waits for nothing at all, which is what lets the body wait for the stop and
 * for what it is doing in one go, with nothing kept aside for the first.
 */
struct async {
  struct async_context ctx; //!< what is handed out
  mtx_t lock;               //!< guards asked and notified
  thrd_t thread;            //!< the thread of the operation
  bool asked;               //!< whether the caller asked the operation to stop
  bool notified;            //!< whether the ask was already told
  async_notify_fn notify;   //!< the call the caller named, may be NULL
  async_body_fn body;       //!< what the thread runs
  thrd_t owner;             //!< the thread that started it, and the only one that may end it
};

/**
 * @brief The operation a published context belongs to
 *
 * The context sits first in the operation, so the one is the other.
 *
 * @param ctx the context, as it was handed out
 * @return the operation behind it
 */
static struct async *async_of(struct async_context *const ctx) { return (struct async *)ctx; }

static bool ctx_stopped(struct async_context *const ctx) {
  struct async *const a = async_of(ctx);
  bool stopped = false;

  mtx_lock(&a->lock);
  stopped = a->asked;
  mtx_unlock(&a->lock);
  return stopped;
}

/**
 * @brief What the thread of an operation runs
 *
 * @param param the operation
 * @return 0, always
 */
static int thread_main(void *const param) {
  struct async *const a = (struct async *)param;

  a->body(&a->ctx);
  if (a->notify != NULL) {
    a->notify(a->ctx.userdata, false);
  }
  return 0;
}

void async_cancel(struct async *const a) {
  bool tell = false;

  if (a == NULL) {
    return;
  }
  assert((thrd_equal(a->owner, thrd_current()) != 0) && "async_cancel is only for the thread that started the operation");
  mtx_lock(&a->lock);
  a->asked = true;
  if ((a->notify != NULL) && !a->notified) {
    a->notified = true;
    tell = true;
  }
  mtx_unlock(&a->lock);
  if (tell) {
    // called outside the lock, so what the caller made for this may do as it likes
    a->notify(a->ctx.userdata, true);
  }
}

NODISCARD struct async *
async_start(async_body_fn const body, void *const userdata, async_notify_fn const notify, struct ov_error *const err) {
  struct async *a = NULL;
  bool success = false;

  if (body == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!OV_REALLOC(&a, 1, sizeof(*a))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(a, 0, sizeof(*a));
  a->ctx = (struct async_context){.stopped = ctx_stopped, .userdata = userdata};
  a->body = body;
  a->notify = notify;
  a->owner = thrd_current();
  if (mtx_init(&a->lock, mtx_plain) != thrd_success) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  if (thrd_create(&a->thread, thread_main, a) != thrd_success) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success && (a != NULL)) {
    OV_FREE(&a);
  }
  return success ? a : NULL;
}

NODISCARD bool async_release(struct async **const a, struct ov_error *const err) {
  bool success = false;

  if ((a == NULL) || (*a == NULL)) {
    return true;
  }
  assert((thrd_equal((*a)->owner, thrd_current()) != 0) && "async_release is only for the thread that started the operation");
  if (thrd_join((*a)->thread, NULL) != thrd_success) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  success = true;

cleanup:
  OV_FREE(a);
  return success;
}
