#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ADC_MOCK

/**
 * @brief What a stand-in clock answers
 *
 * A clock is one number, so the script is that number: a test sets the moment a run is at
 * instead of waiting for the machine to reach it.
 */
struct clock_mock_script {
  uint64_t now_ms; //!< the moment the stand-in is at
};

/**
 * @brief Make this program read a clock a caller sets from here on
 *
 * The script is copied, so it may live on the stack of the caller.  clock_mock_reset puts
 * the clock of the machine back.
 *
 * @param script the moment to answer with, may not be NULL
 * @return false when the script could not be taken
 */
bool clock_mock_install(struct clock_mock_script const *script);

/**
 * @brief Go back to the clock of the machine
 */
void clock_mock_reset(void);

#endif // ADC_MOCK
