#include "device_manager.h"

/** @brief The machine the program reads until another one is named */
static struct device_manager const *g_manager = NULL;

struct device_manager const *device_manager(void) {
  if (g_manager == NULL) {
    g_manager = device_manager_system();
  }
  return g_manager;
}

#ifdef ADC_MOCK
void device_manager_select(struct device_manager const *const manager) {
  g_manager = (manager != NULL) ? manager : device_manager_system();
}
#endif // ADC_MOCK
