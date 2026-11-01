#pragma once

#include <ovbase.h>

#include <wchar.h>

#include "async.h"

/**
 * @brief One long act on the processes of the machine, running on a thread of its own
 */
struct process_act;

/** @brief The longest image name the machine can report, the MAX_PATH of its own entry */
enum { PROCESS_IMAGE_MAX = 260 };

/**
 * @brief One process the caller asked about, and what the look found it to be called
 *
 * The caller fills in pid and takes the image out.  An image that is empty rather than a
 * name says that the moment the look was taken held no process of that id, which is an
 * answer and not a failure.
 */
struct process_name {
  uint32_t pid;                     //!< the id to look for, filled in by the caller
  wchar_t image[PROCESS_IMAGE_MAX]; //!< the name the look found, empty when it found none
};

/**
 * @brief The processes of the machine
 *
 * This program reads what the processes of the machine are and does three things to them,
 * and all three of them wait for seconds: ending one, stopping the Windows Audio service and
 * starting it again.  A caller that has a window must not sit on its own thread for that, so
 * each act is started here and its answer is taken once the act has ended.
 */
struct process_manager {
  /**
   * @brief Take one look at the processes that are running now
   *
   * Nothing is opened and no privilege is asked for, so a process that is not one this tool
   * closes is never touched.
   *
   * Looking each id up on its own reads the machine again every time, and the answers can
   * then disagree with each other: one record is filed against a process that was there when
   * the walk started and gone when the walk reached it.  The look is taken once over all the
   * names it was handed, so every one of them sees the same moment.
   *
   * Only the ids the caller names are looked for, so what the look costs follows what the
   * caller needs and not how much the machine happens to be running.
   *
   * @param names the processes to look for, their image is written in place; may be NULL
   *              when count is 0
   * @param count how many of them there are
   * @param err receives the failure of the look
   * @return how many of the names the machine held a process of, SIZE_MAX when the machine
   *         could not be read, with err set.  An id it held none of is not a failure: it is
   *         left out of the number and its image is left empty
   */
  size_t (*take_snapshot)(struct process_name *names, size_t count, struct ov_error *err);

  /**
   * @brief Start ending the process a removal record names
   *
   * The process is opened now, and the end is refused unless it still is the one the record
   * names: a process that started after the record was written cannot be it, the id was
   * given to somebody else in between.  The instance that was checked is the one that goes.
   *
   * @param pid the process the record named
   * @param event_time_ms when the record was written, epoch milliseconds UTC
   * @param on_done called from the thread of the act when it is over, may be NULL
   * @param userdata handed to on_done
   * @param err receives the failure to start
   * @return the act, release it with act_release; NULL when it could not be started
   */
  struct process_act *(*terminate)(uint32_t pid, uint64_t event_time_ms, async_notify_fn notify, void *userdata, struct ov_error *err);

  /**
   * @brief Start pausing the Windows Audio service, so the audio engine lets the device go
   *
   * The audio engine is started by this service and is restarted within milliseconds
   * whenever it is terminated, so ending it on its own never frees the device for longer
   * than that.  Pausing the service that starts it leaves the device alone until the service
   * is started again.
   *
   * @param on_done called from the thread of the act when it is over, may be NULL
   * @param userdata handed to on_done
   * @param err receives the failure to start
   * @return the act, release it with act_release; NULL when it could not be started
   */
  struct process_act *(*pause_audio_service)(async_notify_fn notify, void *userdata, struct ov_error *err);

  /**
   * @brief Start starting the Windows Audio service again
   *
   * It has to be called after every pause, whatever came in between, or the machine is left
   * without sound.
   *
   * @param on_done called from the thread of the act when it is over, may be NULL
   * @param userdata handed to on_done
   * @param err receives the failure to start
   * @return the act, release it with act_release; NULL when it could not be started
   */
  struct process_act *(*resume_audio_service)(async_notify_fn notify, void *userdata, struct ov_error *err);

  /**
   * @brief Wait for the act to be over and release it
   *
   * Called on the caller's own thread once the end of the act was called.  What the act
   * raised is already in the err the act was started with, so this asks for none.
   *
   * @param act the act, set to NULL afterwards
   * @return true when the act went through
   *
   * An act that was asked to stop answers false, with ov_error_generic_abort in the err it
   * was started with: what it was asked to do then has no answer, which is not a refusal of
   * the machine and is not the caller's to explain.
   */
  bool (*act_release)(struct process_act **act);

  /**
   * @brief Ask the act to stop, from the caller's thread
   *
   * It has to be the thread that started the act: the caller's own state is what the ask
   * reaches, and breaking this is a fault of the caller.  The ask ends the wait the act is
   * in, so an act the machine has not answered yet is released with ov_error_generic_abort
   * rather than with an answer of the machine.
   *
   * @param act the act
   */
  void (*act_cancel)(struct process_act *act);
};

/**
 * @brief The machine this program is working against
 *
 * @return the manager in use, never NULL
 */
struct process_manager const *process_manager(void);

#ifdef ADC_MOCK
/**
 * @brief Work against another machine from here on
 *
 * @param manager the machine to work against, NULL to go back to the machine of the system
 */
void process_manager_select(struct process_manager const *manager);
#endif // ADC_MOCK

/**
 * @brief The machine of the system itself
 *
 * @return the manager that asks the system, never NULL
 */
struct process_manager const *process_manager_system(void);
