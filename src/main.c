#include <windows.h>

#include <ovarray.h>
#include <ovbase.h>
#include <ovmo.h>
#include <ovprintf_ex.h>

#include "instance.h"
#include "logger.h"
#include "options.h"
#include "paths.h"
#include "settings.h"
#include "ui.h"

static char const ph[] = "%1$hs";

/**
 * @brief Tell the person at the machine that the settings file could not be read
 *
 * The name the program looks for is the name of the executable with its extension replaced, so
 * what the dialog shows is that name as it came out, not a description of the rule that made
 * it.
 *
 * The translation resources are loaded before this is called, so the dialog follows the
 * language of the user.  A failure to load a translation is reported to the log only and
 * stays untranslated on purpose.
 *
 * @param path_utf8 the file that could not be read, UTF-8, may be NULL
 */
static void show_startup_error(char const *const path_utf8) {
  wchar_t *text = NULL;
  wchar_t *title = NULL;
  char const *const fmt = gettext("Could not read the settings file.\n"
                                  "It has to sit next to the program as %1$hs.");
  char const *const unknown = gettext("(unknown)");
  char const *const name = paths_file_name(path_utf8);

  if (!ov_sprintf_char2wchar(&text, NULL, ph, fmt, (name != NULL) ? name : unknown)) {
    goto cleanup;
  }
  if (!ov_sprintf_wchar(&title, NULL, L"%1$s", L"%1$s", gettext("Audient Device Closer"))) {
    goto cleanup;
  }
  MessageBoxW(NULL, text, title, MB_OK | MB_ICONERROR);

cleanup:
  if (title != NULL) {
    OV_ARRAY_DESTROY(&title);
    title = NULL;
  }
  if (text != NULL) {
    OV_ARRAY_DESTROY(&text);
    text = NULL;
  }
}

/**
 * @brief Start the program: read the settings and open the window
 *
 * @param hInstance unused
 * @param hPrevInstance unused
 * @param pCmdLine unused, the command line is read through GetCommandLineW
 * @param nCmdShow unused
 * @return the code the process exits with
 */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
  struct ov_init_options init = ov_init_get_default_options();
  struct ov_error err = {0};
  struct ov_error instance_err = {0};
  struct options options;
  struct settings settings;
  struct ov_error mo_err = {0};
  struct ov_error log_err = {0};
  struct mo *mo = NULL;
  char *path = NULL;
  HANDLE instance = NULL;
  bool other_running = false;
  int result = 1;

  // trans: This dagger helps UTF-8 detection. You don't need to translate this.
  (void)gettext_noop("†");
  (void)hInstance;
  (void)hPrevInstance;
  (void)nCmdShow;

  memset(&settings, 0, sizeof(settings));

  init.output_func = logger_output;
  if (!ov_init(&init)) {
    return 2;
  }
  logger_init();
  if (!logger_open(&log_err)) {
    OV_ERROR_REPORT(&log_err, NULL);
  }

  // the copy that finds the mutex taken is a second start: it shows the window that is
  // already there and leaves, with the answer a start that did what it was asked to do
  instance = instance_take(INSTANCE_MUTEX_NAME, &other_running, &instance_err);
  if (instance == NULL) {
    // without the mutex no other copy can be ruled out, so this copy goes on with its
    // window and the log carries why the check is missing
    OV_ERROR_REPORT(&instance_err, NULL);
  } else if (other_running) {
    struct ov_error activate_err = {0};

    if (!ui_activate_running_window(&activate_err)) {
      OV_ERROR_REPORT(&activate_err, NULL);
    }
    result = 0;
    goto cleanup;
  }

  mo = mo_parse_from_resource(GetModuleHandleW(NULL), &mo_err);
  if (mo != NULL) {
    mo_set_default(mo);
  }

  options_default(&options);
  if (!options_parse(pCmdLine, &options, &err)) {
    goto cleanup;
  }
  path = settings_default_path(&err);
  if (path == NULL) {
    show_startup_error(NULL);
    goto cleanup;
  }
  if (!settings_load(path, &settings, &err)) {
    show_startup_error(path);
    goto cleanup;
  }

  if (ui_run(&options, &settings, &err)) {
    result = 0;
  }

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  instance_release(&instance);
  logger_close();
  OV_ERROR_REPORT(&mo_err, NULL);
  settings_string_free(&path);
  settings_release(&settings);
  if (mo != NULL) {
    mo_set_default(NULL);
    mo_free(&mo);
    mo = NULL;
  }
  ov_exit();
  return result;
}
