#include "removal_manager.h"

/** @brief The machine the program asks until another one is named */
static struct removal_manager const *g_manager = NULL;

struct removal_manager const *removal_manager(void) {
  if (g_manager == NULL) {
    g_manager = removal_manager_system();
  }
  return g_manager;
}

#ifdef ADC_MOCK
void removal_manager_select(struct removal_manager const *const manager) {
  g_manager = (manager != NULL) ? manager : removal_manager_system();
}
#endif // ADC_MOCK

bool removal_error_is_veto(struct ov_error const *const err) { return ov_error_is(err, ov_error_type_removal, removal_error_veto); }

enum removal_error_code removal_error_of(struct ov_error const *const err) {
  int code = 0;

  if (!ov_error_get_code(err, ov_error_type_removal, &code)) {
    return removal_error_none;
  }
  return (enum removal_error_code)code;
}
