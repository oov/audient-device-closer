#include "mock/clock.h"

#ifdef ADC_MOCK

#  include "clock_manager.h"

/** @brief The moment a stand-in is at until it is told another one, epoch milliseconds UTC */
static uint64_t const kDefaultNowMs = 1000;

/** @brief The moment the stand-in answers with */
static uint64_t g_now_ms = kDefaultNowMs;

/**
 * @brief The moment the stand-in is at
 *
 * @return the moment in epoch milliseconds UTC
 */
static uint64_t now_ms(void) { return g_now_ms; }

/** @brief The clock of the stand-in, answering the moment the caller set */
static struct clock_manager const kMockClock = {
    .now_ms = now_ms,
};

static struct clock_manager const *manager(void) { return &kMockClock; }

bool clock_mock_install(struct clock_mock_script const *const script) {
  if (script == NULL) {
    return false;
  }
  clock_mock_reset();
  g_now_ms = script->now_ms;
  clock_manager_select(manager());
  return true;
}

void clock_mock_reset(void) {
  g_now_ms = kDefaultNowMs;
  clock_manager_select(NULL);
}

#endif // ADC_MOCK
