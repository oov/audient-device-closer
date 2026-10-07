#include "options.h"

#include <string.h>

#include <ovarray.h>
#include <ovprintf_ex.h>

/**
 * @brief The options a plain start of this program runs with
 *
 * @param out filled in with the defaults
 */
void options_default(struct options *const out) {
  out->auto_run = false;
  out->close_mixer = true;
  out->close_audiodg = false;
}

/**
 * @brief One token of the command line
 */
struct scanner {
  char const *text;
  size_t pos;
};

/**
 * @brief Read the next token of the command line
 *
 * Spaces separate the tokens; a token may be wrapped in double quotes to carry spaces.
 *
 * @param sc the scan to advance
 * @param token receives the token without its quotes
 * @param token_size room of token
 * @return false at the end of the command line
 */
static bool scanner_next(struct scanner *const sc, char *const token, size_t const token_size) {
  while (sc->text[sc->pos] == ' ' || sc->text[sc->pos] == '\t') {
    sc->pos++;
  }
  if (sc->text[sc->pos] == '\0') {
    return false;
  }
  bool quoted = false;
  if (sc->text[sc->pos] == '"') {
    quoted = true;
    sc->pos++;
  }
  size_t n = 0;
  for (; sc->text[sc->pos] != '\0'; sc->pos++) {
    if (quoted) {
      if (sc->text[sc->pos] == '"') {
        sc->pos++;
        break;
      }
    } else if (sc->text[sc->pos] == ' ' || sc->text[sc->pos] == '\t') {
      break;
    }
    if (n + 1 < token_size) {
      token[n++] = sc->text[sc->pos];
    }
  }
  token[n] = '\0';
  return true;
}

/**
 * @brief Read the switches of the command line
 *
 * Unknown tokens are ignored on purpose: a task registration of an older version must keep
 * working.  A switch that is spelled out twice keeps its last answer.
 *
 * @param command_line the command line of this process, NULL is allowed
 * @param out filled in with what the command line says
 * @param err receives a failure of the parsing
 * @return false only on a hard failure
 */
bool options_parse(NATIVE_CHAR const *const command_line, struct options *const out, struct ov_error *const err) {
  bool success = false;
  char *text = NULL;
  char token[256];
  struct scanner sc = {NULL, 0};

  if (!out) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  options_default(out);
  if ((command_line == NULL) || (command_line[0] == 0)) {
    success = true;
    goto cleanup;
  }
  if (!ov_sprintf_char(&text, err, NULL, "%ls", command_line)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  sc.text = text;
  sc.pos = 0;
  while (scanner_next(&sc, token, sizeof(token))) {
    if (strcmp(token, "-auto-run") == 0) {
      out->auto_run = true;
    } else if (strcmp(token, "-close-mixer") == 0) {
      out->close_mixer = true;
    } else if (strcmp(token, "-no-close-mixer") == 0) {
      out->close_mixer = false;
    } else if (strcmp(token, "-close-audiodg") == 0) {
      out->close_audiodg = true;
    } else if (strcmp(token, "-no-close-audiodg") == 0) {
      out->close_audiodg = false;
    }
  }
  success = true;

cleanup:
  if (text) {
    OV_ARRAY_DESTROY(&text);
  }
  return success;
}
