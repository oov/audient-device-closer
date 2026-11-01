#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef ADC_MOCK

/**
 * @brief One process of a stand-in machine
 */
struct process_mock_entry {
  uint32_t pid;         //!< the process id
  wchar_t const *image; //!< the image name the snapshot reports
  uint64_t start_ms;    //!< when the process started, epoch milliseconds UTC
  bool ends;            //!< whether the process goes when it is asked to
};

/**
 * @brief What a stand-in machine answers
 */
struct process_mock_script {
  struct process_mock_entry const *entries; //!< the processes, may be NULL when there are none
  size_t entry_count;                       //!< how many there are
  bool open_fails;                          //!< the snapshot cannot be opened
  bool pause_fails;                         //!< the audio service refuses to stop
  bool resume_fails;                        //!< the audio service refuses to start again
  bool terminate_stops;                     //!< the close is cut short by an ask to stop
};

/**
 * @brief Make this program see a stand-in machine from here on
 *
 * The script is copied, so it may live on the stack of the caller.  What the stand-in saw
 * happen is asked for with process_mock_ended, and process_mock_reset puts the machine of
 * the system back.
 *
 * @param script what the machine answers, may not be NULL
 * @return false when the script could not be taken
 */
bool process_mock_install(struct process_mock_script const *script);

/**
 * @brief How often a process was asked to end since the stand-in was installed
 *
 * @return the number of ends asked for
 */
size_t process_mock_end_count(void);

/**
 * @brief How often the audio service was paused since the stand-in was installed
 *
 * @return the number of pauses asked for
 */
size_t process_mock_service_pause_count(void);

/**
 * @brief How often the audio service was started again since the stand-in was installed
 *
 * @return the number of starts asked for
 */
size_t process_mock_service_resume_count(void);

/**
 * @brief Whether the stand-in let one process go
 *
 * @param pid the process to ask about
 * @return true when the run asked for it and the stand-in let it go
 */
bool process_mock_ended(uint32_t pid);

/**
 * @brief Go back to the machine of the system
 */
void process_mock_reset(void);

#endif // ADC_MOCK
