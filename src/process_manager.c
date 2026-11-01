#include "process_manager.h"

/** @brief The machine the program asks until another one is named */
static struct process_manager const *g_manager = NULL;

struct process_manager const *process_manager(void) {
  if (g_manager == NULL) {
    g_manager = process_manager_system();
  }
  return g_manager;
}

#ifdef ADC_MOCK
void process_manager_select(struct process_manager const *const manager) {
  g_manager = (manager != NULL) ? manager : process_manager_system();
}
#endif // ADC_MOCK
