#include "loggate.h"

#include <string.h>

/**
 * @brief Report whether this message is worth writing again
 */
bool loggate_should_emit(struct loggate *const gate, char const *const text) {
  size_t len = 0;

  if ((gate == NULL) || (text == NULL)) {
    return true; // nothing to compare against, report rather than hide
  }
  if ((gate->last != NULL) && (0 == strcmp(gate->last, text))) {
    return false; // the same failure as the previous poll
  }
  len = strlen(text) + 1;
  if (!OV_REALLOC(&gate->last, len, sizeof(gate->last[0]))) {
    return true; // out of memory: repeating a message is safer than swallowing one
  }
  memcpy(gate->last, text, len);
  return true;
}

/**
 * @brief Forget the remembered message
 */
void loggate_release(struct loggate *const gate) {
  if ((gate != NULL) && (gate->last != NULL)) {
    OV_FREE(&gate->last);
  }
}
