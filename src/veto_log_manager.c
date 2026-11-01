#include "veto_log_manager.h"

/** @brief The log the program reads until another one is named */
static struct veto_log_manager const *g_manager = NULL;

struct veto_log_manager const *veto_log_manager(void) {
  if (g_manager == NULL) {
    g_manager = veto_log_manager_system();
  }
  return g_manager;
}

#ifdef ADC_MOCK
void veto_log_manager_select(struct veto_log_manager const *const manager) {
  g_manager = (manager != NULL) ? manager : veto_log_manager_system();
}
#endif // ADC_MOCK
