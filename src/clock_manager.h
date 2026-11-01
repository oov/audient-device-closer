#pragma once

#include <stdint.h>

/**
 * @brief The moment the program is at
 *
 * Several kinds of thing are stamped with the same kind of moment: a removal record carries
 * the moment it was written, the log of the program stamps every line it writes, and a run
 * notes the moment before it asks for a removal so it can tell the record it reads afterwards
 * from one of an earlier attempt.  They all have to agree on what a moment is, so the one
 * place that reads the machine's clock answers for all of them.
 */
struct clock_manager {
  /**
   * @brief The moment now, in epoch milliseconds UTC
   *
   * @return the moment in epoch milliseconds UTC, 0 when it could not be read
   */
  uint64_t (*now_ms)(void);
};

/**
 * @brief The clock this program reads until another one is named
 *
 * @return the clock in use, never NULL
 */
struct clock_manager const *clock_manager(void);

#ifdef ADC_MOCK
/**
 * @brief Read another clock from here on
 *
 * @param manager the clock to read, NULL to go back to the clock of the machine
 */
void clock_manager_select(struct clock_manager const *manager);
#endif // ADC_MOCK

/**
 * @brief The moment now, in epoch milliseconds UTC
 *
 * @return the moment in epoch milliseconds UTC, 0 when it could not be read
 */
uint64_t clock_now_ms(void);

/**
 * @brief The clock of the machine itself
 *
 * @return the clock that asks the system, never NULL
 */
struct clock_manager const *clock_manager_system(void);
