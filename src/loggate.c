#include "loggate.h"

#include <string.h>

/**
 * @brief Report whether this message is worth writing again
 */
bool loggate_should_emit(struct loggate *const gate, char const *const text) {
  size_t len = 0;
  bool emit = true; // nothing to compare against, report rather than hide

  if ((gate == NULL) || (text == NULL)) {
    goto cleanup;
  }
  if ((gate->last != NULL) && (0 == strcmp(gate->last, text))) {
    emit = false; // the same failure as the previous poll
    goto cleanup;
  }
  len = strlen(text) + 1;
  if (!OV_REALLOC(&gate->last, len, sizeof(gate->last[0]))) {
    emit = true; // out of memory: repeating a message is safer than swallowing one
    goto cleanup;
  }
  memcpy(gate->last, text, len);

cleanup:
  return emit;
}

/**
 * @brief Forget the remembered message
 */
void loggate_release(struct loggate *const gate) {
  if ((gate != NULL) && (gate->last != NULL)) {
    OV_FREE(&gate->last);
  }
}
