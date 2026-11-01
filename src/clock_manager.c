#include "clock_manager.h"

/** @brief The clock the program reads until another one is named */
static struct clock_manager const *g_manager = NULL;

struct clock_manager const *clock_manager(void) {
  if (g_manager == NULL) {
    g_manager = clock_manager_system();
  }
  return g_manager;
}

#ifdef ADC_MOCK
void clock_manager_select(struct clock_manager const *const manager) { g_manager = (manager != NULL) ? manager : clock_manager_system(); }
#endif // ADC_MOCK

uint64_t clock_now_ms(void) { return clock_manager()->now_ms(); }
