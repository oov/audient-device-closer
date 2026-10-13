/**
 * Tests for the window of the application.
 *
 * These run against a real window: the defect they exist for was one control that never
 * received the font of the window, which only a real window shows.  They are the reason a
 * developer can start the debug build without an elevation (see app.debug.manifest): a
 * harness that had to ask for the administrator on every run would not be run often, and
 * a check nobody runs is not a check.
 */
#include <ovtest.h>

#include "layout.c"
#include "ui_device_picker.c"

#include <string.h>

#include <windows.h>

#include <commctrl.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>
#include <ovthreads.h>

#include "culprit.h"
#include "device.h"
#include "layout.h"
#include "options.h"
#include "removal.h"
#include "repair.h"
#include "settings.h"
#include "theme.h"
#include "theme_internal.h"
#include "ui.h"
#include "ui_blockers_window.h"
#include "ui_progress_window.h"
#include <uxtheme.h>

#define IDC_PROGRESS_CANCEL 14 // the cancel button of the progress window, from ui_progress_window.c
#define IDC_BLOCKERS_OK 15     // the OK button of the blockers window, from ui_blockers_window.c

enum {
  ADC_UI_TEST_TIMEOUT_MS = 20000,
  ADC_UI_TEST_POLL_MS = 50,
  ADC_BLOCKERS_TEST_TIMER_ID = 1,
  ADC_BLOCKERS_TEST_TIMER_MS = 100,
  ADC_PICKER_TEST_TIMER_ID = 2,
  ADC_PICKER_TEST_TIMER_MS = 100,
  ADC_TEST_PROGRESS_SUGGEST_LEFT = 240, /* where the window goes when a monitor suggests it */
  ADC_TEST_PROGRESS_SUGGEST_TOP = 200,
  ADC_TEST_PROGRESS_SUGGEST_RIGHT = 900, /* a size the layout of the window does not ask for */
  ADC_TEST_PROGRESS_SUGGEST_BOTTOM = 700,
};

struct adc_ui_result {
  bool ok;
  bool opened;
  bool timed_out;
  struct ov_error err;
  size_t fontless;
  HWND fontless_first;
  wchar_t fontless_class[32];
};

struct adc_ui_thread {
  struct options const *options;
  struct settings const *settings;
  struct adc_ui_result *out;
  struct cndvar done;
};

/**
 * @brief Collect the controls of the window
 *
 * @param parent the window to walk
 * @param out receives the controls
 * @param n receives how many were written
 * @param max room of out
 */
static void find_windows(HWND const parent, HWND *const out, size_t *const n, size_t const max) {
  HWND child = GetWindow(parent, GW_CHILD);

  while ((child != NULL) && (*n < max)) {
    out[(*n)++] = child;
    find_windows(child, out, n, max);
    child = GetWindow(child, GW_HWNDNEXT);
  }
}

/**
 * @brief Does one of these controls miss the font of the window
 *
 * @param windows the controls to look at
 * @param n how many there are
 * @param out_first receives the control that misses the font
 * @param out_class receives the class of that control
 * @return true when one of them has no font
 */
static bool any_has_no_font(HWND const *const windows, size_t const n, HWND *const out_first, wchar_t *const out_class) {
  bool found = false;

  for (size_t i = 0; i < n; i++) {
    wchar_t cls[32];
    memset(cls, 0, sizeof(cls));
    GetClassNameW(windows[i], cls, (int)(sizeof(cls) / sizeof(cls[0])));
    if ((lstrcmpiW(cls, L"Button") != 0) && (lstrcmpiW(cls, L"Static") != 0) && (lstrcmpiW(cls, L"SysListView32") != 0) &&
        (lstrcmpiW(cls, L"SysHeader32") != 0)) {
      continue;
    }
    if (SendMessageW(windows[i], WM_GETFONT, 0, 0) == 0) {
      found = true;
      if (out_first != NULL) {
        *out_first = windows[i];
        if (out_class != NULL) {
          memcpy(out_class, cls, sizeof(cls));
        }
      }
    }
  }
  return found;
}

/**
 * @brief The control that says exactly this
 *
 * The window of the run is on another thread, so the harness reads the text of every child
 * until it finds the one it is looking for.
 *
 * @param parent the window to walk
 * @param want the text to look for
 * @return the control that carries it, NULL when there is none
 */
static HWND find_child_with_text(HWND const parent, wchar_t const *const want) {
  HWND child = GetWindow(parent, GW_CHILD);

  while (child != NULL) {
    wchar_t text[256];

    memset(text, 0, sizeof(text));
    GetWindowTextW(child, text, (int)(sizeof(text) / sizeof(text[0])));
    if (lstrcmpW(text, want) == 0) {
      return child;
    }
    child = GetWindow(child, GW_HWNDNEXT);
  }
  return NULL;
}

/**
 * @brief The window of this process that carries the class, the first one it finds
 *
 * The desktop is shared with the living program and with windows of runs that outlived
 * their deadline: the class name alone does not say whose window it is, the process id does.
 *
 * @param class_name the class the window has to carry
 * @return the first window of this process with that class, NULL when there is none
 */
static HWND find_own_window(wchar_t const *const class_name) {
  HWND hwnd = NULL;

  while ((hwnd = FindWindowExW(NULL, hwnd, class_name, NULL)) != NULL) {
    DWORD pid = 0;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
      return hwnd;
    }
  }
  return NULL;
}

/**
 * @brief The thread the window runs on
 *
 * @param param the job to run
 * @return 0 when the window is closed
 */
static int run_ui(void *const param) {
  struct adc_ui_thread *const job = (struct adc_ui_thread *)(void *)param;

  job->out->opened = ui_run(job->options, job->settings, &job->out->err);
  cndvar_broadcast(&job->done, 1);
  return 0;
}

/**
 * @brief Wait for the window to close, with a deadline
 *
 * The run of the window has a deadline: a test must not hang when the window never closes.
 * A run that outlives it is left to the end of the process and answered with a failure.
 *
 * @param thread the thread the window runs on
 * @param done the flag the thread sets when it is over
 * @return false when the window did not close in time
 */
static bool join_ui(thrd_t *const thread, struct cndvar *const done) {
  struct timespec deadline = {0, 0};

  timespec_get(&deadline, TIME_UTC);
  deadline.tv_sec += ADC_UI_TEST_TIMEOUT_MS / 1000;
  deadline.tv_nsec += (long)((ADC_UI_TEST_TIMEOUT_MS % 1000) * 1000000L);
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000L;
  }
  cndvar_lock(done);
  if (cndvar_timedwait_while(done, 0, &deadline) != thrd_success) {
    cndvar_unlock(done);
    TEST_CHECK(thrd_detach(*thread) == thrd_success);
    return false;
  }
  cndvar_unlock(done);
  return thrd_join(*thread, NULL) == thrd_success;
}

/**
 * @brief Start the window on a thread of its own
 *
 * The job is used by both runs of the test, so the done flag is cleared before the thread
 * can set it again.
 *
 * @param thread receives the thread the window runs on
 * @param job the job to run
 * @return false when the thread could not be started
 */
static bool start_ui(thrd_t *const thread, struct adc_ui_thread *const job) {
  cndvar_lock(&job->done);
  cndvar_signal(&job->done, 0);
  cndvar_unlock(&job->done);
  return thrd_create(thread, run_ui, job) == thrd_success;
}

/**
 * @brief Every control of the window carries the font of the window
 *
 * Runs the window, waits until it is up, then checks every control for a font and closes
 * it.
 */
static void test_every_control_carries_the_font_of_the_window(void) {
  char service_name[] = "adc-ui-test-no-such-service";
  struct settings settings;
  struct options options;
  struct adc_ui_result result;
  struct adc_ui_result second;
  struct adc_ui_thread job;
  thrd_t thread = {0};
  bool started = false;
  HWND top = NULL;
  DWORD const start = GetTickCount();

  memset(&settings, 0, sizeof(settings));
  memset(&result, 0, sizeof(result));
  memset(&second, 0, sizeof(second));
  options_default(&options);
  settings.ks_service = service_name;
  settings.use_theme = true;

  job.options = &options;
  job.settings = &settings;
  job.out = &result;
  cndvar_init(&job.done);

  if (!TEST_CHECK(start_ui(&thread, &job))) {
    goto cleanup;
  }
  started = true;
  while ((GetTickCount() - start) < ADC_UI_TEST_TIMEOUT_MS) {
    top = find_own_window(L"audient_device_closer_main");
    if (top != NULL) {
      break;
    }
    Sleep(ADC_UI_TEST_POLL_MS);
  }
  if (!TEST_CHECK(top != NULL)) {
    result.timed_out = true;
    goto cleanup;
  }
  /* the barrier is the whole settling the window needs: what WM_CREATE made is final when
     a message of its own is answered */
  SendMessageW(top, WM_NULL, 0, 0);

  TEST_CASE("the main window has no list of its own any more");
  {
    HWND windows[32];
    size_t n = 0;
    size_t i = 0;
    bool has_list = false;

    memset(windows, 0, sizeof(windows));
    find_windows(top, windows, &n, sizeof(windows) / sizeof(windows[0]));
    for (i = 0; i < n; i++) {
      wchar_t cls[32];
      memset(cls, 0, sizeof(cls));
      GetClassNameW(windows[i], cls, (int)(sizeof(cls) / sizeof(cls[0])));
      if (lstrcmpiW(cls, L"SysListView32") == 0) {
        has_list = true;
      }
    }
    TEST_CHECK(has_list == false);
    TEST_MSG("want no list view in the main window, the blockers window shows it");
  }
  TEST_CASE_(NULL);

  TEST_CASE("the window is not resizable");
  {
    LONG_PTR const style = GetWindowLongPtrW(top, GWL_STYLE);

    TEST_CHECK((style & WS_THICKFRAME) == 0);
    TEST_MSG("want a main window without the thick frame of a resizable window");
    {
      RECT rc;
      RECT before;

      GetWindowRect(top, &rc);
      before = rc;
      SendMessageW(top, WM_SIZE, 0, 0);
      GetWindowRect(top, &rc);
      TEST_CHECK((rc.right - rc.left) == (before.right - before.left));
    }
  }
  TEST_CASE_(NULL);

  TEST_CASE("the window is the size its layout asks for");
  {
    UINT const dpi = GetDpiForWindow(top);
    int const least_w = layout_scale(ADC_LAYOUT_WINDOW_W, dpi);
    int const want_h = layout_scale(ADC_LAYOUT_WINDOW_H, dpi);
    RECT client;
    RECT outer;
    RECT want;

    GetClientRect(top, &client);
    GetWindowRect(top, &outer);
    /* the application asks for the client area, the window manager adds the frame around it */
    SetRect(&want, 0, 0, client.right - client.left, client.bottom - client.top);
    if (TEST_CHECK(AdjustWindowRectExForDpi(
            &want, (DWORD)GetWindowLongPtrW(top, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(top, GWL_EXSTYLE), dpi))) {
      TEST_CHECK((client.bottom - client.top) == want_h);
      TEST_MSG("want a client %d tall at dpi %u, got %d", want_h, (unsigned)dpi, (int)(client.bottom - client.top));
      TEST_CHECK((client.right - client.left) >= least_w);
      TEST_MSG("want a client at least %d wide at dpi %u, got %d", least_w, (unsigned)dpi, (int)(client.right - client.left));
      TEST_CHECK((outer.right - outer.left) == (want.right - want.left));
      TEST_MSG("want the window %d wide, got %d", (int)(want.right - want.left), (int)(outer.right - outer.left));
      TEST_CHECK((outer.bottom - outer.top) == (want.bottom - want.top));
      TEST_MSG("want the window %d tall, got %d", (int)(want.bottom - want.top), (int)(outer.bottom - outer.top));
    }
  }
  TEST_CASE_(NULL);

  TEST_CASE("the settings group offers the boxes of the catalogue and no more");
  {
    HWND boxes[8];
    size_t n = 0;
    HWND child = GetWindow(top, GW_CHILD);

    memset(boxes, 0, sizeof(boxes));
    while ((child != NULL) && (n < (sizeof(boxes) / sizeof(boxes[0])))) {
      wchar_t cls[32];
      memset(cls, 0, sizeof(cls));
      GetClassNameW(child, cls, (int)(sizeof(cls) / sizeof(cls[0])));
      if ((lstrcmpiW(cls, L"Button") == 0) && ((GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX)) {
        boxes[n++] = child;
      }
      child = GetWindow(child, GW_HWNDNEXT);
    }
    TEST_CHECK_(n == 2, "the settings group shows %u boxes, the window has 2", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
      wchar_t title[256];
      memset(title, 0, sizeof(title));
      GetWindowTextW(boxes[i], title, (int)(sizeof(title) / sizeof(title[0])));
      TEST_CHECK_(lstrcmpW(title, L"Close this tool when it worked") != 0, "the box that was removed is still in the window: [%ls]", title);
    }
  }
  TEST_CASE_(NULL);

  TEST_CASE("every control of the window carries a font");
  {
    HWND windows[32];
    size_t n = 0;
    memset(windows, 0, sizeof(windows));
    find_windows(top, windows, &n, sizeof(windows) / sizeof(windows[0]));
    if (TEST_CHECK(n > 0)) {
      result.fontless = 0;
      if (any_has_no_font(windows, n, &result.fontless_first, result.fontless_class)) {
        result.fontless = 1;
      }
    }
  }
  TEST_CHECK(result.fontless == 0);
  if (result.fontless != 0) {
    TEST_MSG("the control [%ls] has no font of its own, so it is painted with the stock font", result.fontless_class);
  }
  TEST_CASE_(NULL);

  PostMessageW(top, WM_CLOSE, 0, 0);
  started = false; // join_ui hands the thread up whatever it answers
  if (!TEST_CHECK(join_ui(&thread, &job.done))) {
    goto cleanup;
  }
  TEST_SUCCEEDED(result.opened, &result.err);

  TEST_CASE("the window can be opened a second time");
  {
    DWORD const second_start = GetTickCount();

    job.out = &second;
    if (!TEST_CHECK(start_ui(&thread, &job))) {
      goto cleanup;
    }
    started = true;
    top = NULL;
    while ((GetTickCount() - second_start) < ADC_UI_TEST_TIMEOUT_MS) {
      top = find_own_window(L"audient_device_closer_main");
      if (top != NULL) {
        break;
      }
      Sleep(ADC_UI_TEST_POLL_MS);
    }
    if (TEST_CHECK(top != NULL)) {
      PostMessageW(top, WM_CLOSE, 0, 0);
      started = false;
      TEST_CHECK(join_ui(&thread, &job.done));
      TEST_SUCCEEDED(second.opened, &second.err);
    }
  }
  TEST_CASE_(NULL);

cleanup:
  if (top != NULL) {
    PostMessageW(top, WM_CLOSE, 0, 0);
  }
  if (started) {
    TEST_CHECK(join_ui(&thread, &job.done));
    started = false;
  }
  cndvar_exit(&job.done);
}

/**
 * @brief A window without the theme is drawn by the system alone
 *
 * The switch exists for a Windows the undocumented entry points of the theme
 * do not work on: such a window asks the system for everything and the
 * system answers with the traditional look -- the surface a dialog of the
 * system is drawn on, and the group boxes of comctl32 around their controls.
 * The groups are the boxes of the system here, not the captions and the
 * frames this program draws when it themes the window itself, and the boxes
 * sit under the controls they hold.
 */
static void test_window_without_the_theme_is_traditional(void) {
  char service_name[] = "adc-ui-test-no-such-service";
  struct settings settings;
  struct options options;
  struct adc_ui_result result;
  struct adc_ui_thread job;
  thrd_t thread = {0};
  bool started = false;
  HWND top = NULL;
  DWORD const start = GetTickCount();

  memset(&settings, 0, sizeof(settings));
  memset(&result, 0, sizeof(result));
  options_default(&options);
  settings.ks_service = service_name;
  settings.use_theme = false;

  job.options = &options;
  job.settings = &settings;
  job.out = &result;
  cndvar_init(&job.done);

  if (!TEST_CHECK(start_ui(&thread, &job))) {
    goto cleanup;
  }
  started = true;
  while ((GetTickCount() - start) < ADC_UI_TEST_TIMEOUT_MS) {
    top = find_own_window(L"audient_device_closer_main");
    if (top != NULL) {
      break;
    }
    Sleep(ADC_UI_TEST_POLL_MS);
  }
  if (!TEST_CHECK(top != NULL)) {
    result.timed_out = true;
    goto cleanup;
  }
  /* the barrier is the whole settling the window needs: what WM_CREATE made is final when
     a message of its own is answered */
  SendMessageW(top, WM_NULL, 0, 0);

  TEST_CASE("the groups are the boxes of the system");
  {
    HWND groups[2];
    size_t n = 0;
    HWND child = GetWindow(top, GW_CHILD);

    memset(groups, 0, sizeof(groups));
    while ((child != NULL) && (n < (sizeof(groups) / sizeof(groups[0])))) {
      wchar_t cls[32];
      memset(cls, 0, sizeof(cls));
      GetClassNameW(child, cls, (int)(sizeof(cls) / sizeof(cls[0])));
      if ((lstrcmpiW(cls, L"Button") == 0) && ((GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX)) {
        groups[n++] = child;
      }
      child = GetWindow(child, GW_HWNDNEXT);
    }
    TEST_CHECK_(n == 2, "the window has %u group boxes, the layout has 2", (unsigned)n);
  }
  TEST_CASE_(NULL);

  TEST_CASE("the boxes of the settings sit inside their group box");
  {
    HWND boxes[8];
    HWND groups[2];
    size_t n = 0;
    size_t g = 0;
    HWND child = GetWindow(top, GW_CHILD);

    memset(boxes, 0, sizeof(boxes));
    memset(groups, 0, sizeof(groups));
    while ((child != NULL) && ((n < (sizeof(boxes) / sizeof(boxes[0]))) || (g < (sizeof(groups) / sizeof(groups[0]))))) {
      wchar_t cls[32];
      LONG_PTR const type = GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK;

      memset(cls, 0, sizeof(cls));
      GetClassNameW(child, cls, (int)(sizeof(cls) / sizeof(cls[0])));
      if (lstrcmpiW(cls, L"Button") == 0) {
        if ((type == BS_AUTOCHECKBOX) && (n < (sizeof(boxes) / sizeof(boxes[0])))) {
          boxes[n++] = child;
        } else if ((type == BS_GROUPBOX) && (g < (sizeof(groups) / sizeof(groups[0])))) {
          groups[g++] = child;
        }
      }
      child = GetWindow(child, GW_HWNDNEXT);
    }
    if (TEST_CHECK_(n == 2, "the settings group shows %u boxes, the window has 2", (unsigned)n)) {
      for (size_t i = 0; i < n; i++) {
        RECT box;
        RECT holder;
        bool inside = false;

        GetWindowRect(boxes[i], &box);
        for (size_t j = 0; j < g; j++) {
          GetWindowRect(groups[j], &holder);
          if ((box.left >= holder.left) && (box.top >= holder.top) && (box.right <= holder.right) && (box.bottom <= holder.bottom)) {
            inside = true;
            break;
          }
        }
        TEST_CHECK(inside);
        TEST_MSG("want the box of the settings inside a group box of the window");
      }
    }
  }
  TEST_CASE_(NULL);

  TEST_CASE("the surface of the window is the one a dialog of the system is drawn on");
  {
    ULONG_PTR const brush_value = GetClassLongPtrW(top, GCLP_HBRBACKGROUND);
    HBRUSH const brush = (HBRUSH)(void *)brush_value;
    LOGBRUSH lb;

    if (TEST_CHECK(brush != NULL)) {
      TEST_CHECK((GetObjectW(brush, sizeof(lb), &lb) != 0) && (lb.lbColor == GetSysColor(COLOR_BTNFACE)));
      TEST_MSG("want the surface 0x%08lX of the system, got 0x%08lX", (unsigned long)GetSysColor(COLOR_BTNFACE), (unsigned long)lb.lbColor);
    }
  }
  TEST_CASE_(NULL);

  PostMessageW(top, WM_CLOSE, 0, 0);
  started = false; // join_ui hands the thread up whatever it answers
  if (!TEST_CHECK(join_ui(&thread, &job.done))) {
    goto cleanup;
  }
  TEST_SUCCEEDED(result.opened, &result.err);

cleanup:
  if (top != NULL) {
    PostMessageW(top, WM_CLOSE, 0, 0);
  }
  if (started) {
    TEST_CHECK(join_ui(&thread, &job.done));
    started = false;
  }
  cndvar_exit(&job.done);
}

/**
 * @brief The wait of a scheduled run is a step of the run, not a silent pause
 *
 * The run of the scheduler waits for the devices to come back on the bus, and that wait is
 * a step of the run like any other: the window counts it in ("(1/8)") instead of sitting
 * there in silence, and its controls stay locked while it waits: a window that answers the
 * mouse and shows nothing looks frozen.  The defect this test exists for is exactly that
 * silent wait.
 *
 * Nothing here trusts a moment of the clock.  The state the start-up leaves is read after a
 * message barrier, the line of the wait is watched until the run announces itself done, and
 * the asks of the abort and of the close are messages, which answer before they return.
 */
static void test_window_tells_that_it_waits(void) {
  char service_name[] = "adc-ui-test-no-such-service";
  struct settings settings;
  struct options options;
  struct adc_ui_result result;
  struct adc_ui_thread job;
  thrd_t thread = {0};
  bool started = false;
  HWND top = NULL;
  DWORD const start = GetTickCount();

  memset(&settings, 0, sizeof(settings));
  memset(&result, 0, sizeof(result));
  options_default(&options);
  options.auto_run = true;
  settings.ks_service = service_name;
  settings.use_theme = true;
  settings.auto_run_delay_ms = 500; // the wait of the run is the stage the looks read; its length is not asserted

  job.options = &options;
  job.settings = &settings;
  job.out = &result;
  cndvar_init(&job.done);

  if (!TEST_CHECK(start_ui(&thread, &job))) {
    goto cleanup;
  }
  started = true;
  while ((GetTickCount() - start) < ADC_UI_TEST_TIMEOUT_MS) {
    top = find_own_window(L"audient_device_closer_main");
    if (top != NULL) {
      break;
    }
    Sleep(ADC_UI_TEST_POLL_MS);
  }
  if (!TEST_CHECK(top != NULL)) {
    goto cleanup;
  }
  SendMessageW(top, WM_NULL, 0, 0);

  TEST_CASE("the start-up of the run has its own screen, made before the first answer");
  {
    HWND progress = find_own_window(L"audient_device_closer_progress");
    HWND bar = NULL;

    if (TEST_CHECK(progress != NULL)) {
      bar = FindWindowExW(progress, NULL, PROGRESS_CLASSW, NULL);
    }
    if (TEST_CHECK(bar != NULL)) {
      LRESULT const range = SendMessageW(bar, PBM_GETRANGE, FALSE, 0);
      TEST_CHECK_(range == 8, "want a bar over the 8 steps of the run, got %ld", (long)range);
    }
    TEST_CHECK(IsWindowEnabled(top) == FALSE);
    TEST_MSG("want a locked main window while the run is going");
    {
      HWND const button = find_child_with_text(top, L"Close the Audient device");
      if (TEST_CHECK(button != NULL)) {
        TEST_CHECK(IsWindowEnabled(button) == FALSE);
        TEST_MSG("want a disabled button, got an enabled one");
      }
    }
  }
  TEST_CASE_(NULL);

  TEST_CASE("the wait is told as the first step, and the asks do not end the run's window");
  {
    bool wait_step = false;
    bool pressed = false;
    bool alive_after_close = false;
    bool run_over = false;
    DWORD const sample_start = GetTickCount();

    while (!run_over && ((GetTickCount() - sample_start) < ADC_UI_TEST_TIMEOUT_MS)) {
      struct timespec next;
      HWND progress = NULL;

      timespec_get(&next, TIME_UTC);
      next.tv_nsec += (long)ADC_UI_TEST_POLL_MS * 1000000L;
      if (next.tv_nsec >= 1000000000L) {
        next.tv_sec += 1;
        next.tv_nsec -= 1000000000L;
      }
      cndvar_lock(&job.done);
      run_over = (cndvar_timedwait_while(&job.done, 0, &next) == thrd_success);
      cndvar_unlock(&job.done);
      if (run_over) {
        break; // the run took its window down before it announced itself: there is nothing left to see
      }
      progress = find_own_window(L"audient_device_closer_progress");
      if (progress == NULL) {
        continue;
      }
      if (!wait_step) {
        wait_step = (find_child_with_text(progress, L"Waiting... (1/8)") != NULL);
        continue;
      }
      if (!pressed) {
        HWND const cancel_btn = find_child_with_text(progress, L"Abort");
        if (TEST_CHECK(cancel_btn != NULL)) {
          SendMessageW(progress, WM_COMMAND, MAKEWPARAM(IDC_PROGRESS_CANCEL, BN_CLICKED), (LPARAM)cancel_btn);
          TEST_CHECK(IsWindowEnabled(cancel_btn) == FALSE);
          TEST_MSG("want a greyed cancel button after the press");
          SendMessageW(progress, WM_COMMAND, MAKEWPARAM(IDC_PROGRESS_CANCEL, BN_CLICKED), (LPARAM)cancel_btn);
          TEST_CHECK(IsWindowEnabled(cancel_btn) == FALSE);
        }
        PostMessageW(progress, WM_CLOSE, 0, 0);
        pressed = true;
        continue;
      }
      alive_after_close = alive_after_close || (IsWindow(progress) != 0);
    }
    TEST_CHECK(wait_step);
    TEST_MSG("want the wait shown as step (1/8) of the run, got a different status line");
    TEST_CHECK(pressed);
    TEST_MSG("want the wait told before the run was over");
    TEST_CHECK(alive_after_close);
    TEST_MSG("want the progress window to stay until the run is done");
  }
  TEST_CASE_(NULL);

  started = false;
  TEST_CHECK(join_ui(&thread, &job.done));
  TEST_SUCCEEDED(result.opened, &result.err);
  TEST_CHECK(find_own_window(L"audient_device_closer_progress") == NULL);
  TEST_MSG("want the progress window to be gone when the run is over");

cleanup:
  if (started) {
    TEST_CHECK(join_ui(&thread, &job.done));
    started = false;
  }
  cndvar_exit(&job.done);
}

/**
 * @brief The headline says what the run did instead of repeating the name of the program
 *
 * The dialog has two lines of its own: the title bar already carries the name of the
 * program.  The defect this test exists for is exactly that: the name of the program was
 * passed as the main instruction of the dialog as well as the title bar, which is two
 * identical lines.
 */
static void test_result_headline(void) {
  char headline[256];
  char body[512];

  TEST_CASE("a closed device");
  ui_result_dialog_text(REPAIR_OUTCOME_CLOSED, removal_error_none, headline, sizeof(headline), body, sizeof(body));
  TEST_CHECK(strcmp(headline, "Audient Device Closer") != 0);
  TEST_CHECK(strstr(headline, "could not") == NULL);
  TEST_CHECK(body[0] != '\0');

  TEST_CASE("a device that is not there");
  ui_result_dialog_text(REPAIR_OUTCOME_ABSENT, removal_error_none, headline, sizeof(headline), body, sizeof(body));
  TEST_CHECK(strcmp(headline, "Audient Device Closer") != 0);
  TEST_CHECK(strstr(body, "No Audient device was found") != NULL);
  TEST_CHECK(strstr(body, "try again") == NULL);

  TEST_CASE("a device with a problem the removal does not cure");
  ui_result_dialog_text(REPAIR_OUTCOME_OTHER_PROBLEM, removal_error_none, headline, sizeof(headline), body, sizeof(body));
  TEST_CHECK(strcmp(headline, "Audient Device Closer") != 0);

  TEST_CASE("a removal the program was not allowed to ask for");
  ui_result_dialog_text(REPAIR_OUTCOME_FAILED, removal_error_access_denied, headline, sizeof(headline), body, sizeof(body));
  TEST_CHECK(strcmp(headline, "Failed.") == 0);
  TEST_MSG("want the verdict in the headline of a failed run");
  TEST_CHECK(strstr(body, "administrator") != NULL);
  TEST_MSG("want the reason the program worked out, in the body");
  TEST_CHECK(strstr(body, "could not be released") == NULL);
  TEST_MSG("want no sentence of the log in the dialog");

  TEST_CASE("the headline and the body are different sentences");
  TEST_CHECK(strcmp(headline, body) != 0);

  TEST_CASE("a veto names no process here");
  ui_result_dialog_text(REPAIR_OUTCOME_VETOED, removal_error_none, headline, sizeof(headline), body, sizeof(body));
  TEST_CHECK(strstr(body, "A process is still using the device") != NULL);
  TEST_CHECK(strstr(body, ".exe") == NULL);
  TEST_MSG("the names of the blockers belong to the blockers window");

  TEST_CASE_(NULL);
}

/**
 * @brief A dialog that says the same thing twice is not a dialog
 *
 * This walks every outcome the run can end in and demands that the headline and the body
 * say different things: the headline states what happened, the body states what the reader
 * does with it.  The defect it exists for is the success case, which read "The device was
 * closed." on both of its lines.
 */
static void test_result_dialog_lines_differ(void) {
  static enum repair_outcome const outcomes[] = {REPAIR_OUTCOME_CLOSED,
                                                 REPAIR_OUTCOME_ABSENT,
                                                 REPAIR_OUTCOME_OTHER_PROBLEM,
                                                 REPAIR_OUTCOME_VETOED,
                                                 REPAIR_OUTCOME_FAILED,
                                                 REPAIR_OUTCOME_UNKNOWN};
  static enum removal_error_code const removal_errors[] = {removal_error_none,
                                                           removal_error_access_denied,
                                                           removal_error_device_gone,
                                                           removal_error_call_failed,
                                                           removal_error_veto,
                                                           removal_error_refused};
  size_t i = 0;
  size_t k = 0;

  for (i = 0; i < (sizeof(outcomes) / sizeof(outcomes[0])); i++) {
    for (k = 0; k < (sizeof(removal_errors) / sizeof(removal_errors[0])); k++) {
      char headline[256];
      char body[512];

      TEST_CASE_("outcome %d with removal error %d", (int)outcomes[i], (int)removal_errors[k]);
      ui_result_dialog_text(outcomes[i], removal_errors[k], headline, sizeof(headline), body, sizeof(body));
      TEST_CHECK(headline[0] != '\0');
      TEST_CHECK(body[0] != '\0');
      if (!TEST_CHECK_(strcmp(headline, body) != 0, "both lines read [%hs]", headline)) {
        TEST_MSG("the dialog says the same thing twice");
      }
    }
  }
  TEST_CASE_(NULL);
}

/**
 * @brief A run nobody is watching acts on the state this tool is for
 *
 * The task starts this program at every resume from sleep and nobody stands in front of
 * the window then: every other state is a run with nothing to do, and a window that waits
 * for a person who is not there is the defect this test exists for.  A run a person
 * started waits for that person instead.
 */
static void test_auto_run_acts_on_the_state_it_is_for(void) {
  TEST_CASE("a run the scheduler started");
  TEST_CHECK(ui_auto_run_action(true, DEVICE_STATE_FAULTED) == UI_AUTO_RUN_REPAIR);
  TEST_CHECK(ui_auto_run_action(true, DEVICE_STATE_HEALTHY) == UI_AUTO_RUN_LEAVE);
  TEST_CHECK(ui_auto_run_action(true, DEVICE_STATE_ABSENT) == UI_AUTO_RUN_LEAVE);
  TEST_CHECK(ui_auto_run_action(true, DEVICE_STATE_PROBLEM) == UI_AUTO_RUN_LEAVE);
  TEST_CHECK(ui_auto_run_action(true, DEVICE_STATE_UNKNOWN) == UI_AUTO_RUN_LEAVE);

  TEST_CASE_("a run a person started");
  TEST_CHECK(ui_auto_run_action(false, DEVICE_STATE_FAULTED) == UI_AUTO_RUN_WAIT);
  TEST_CHECK(ui_auto_run_action(false, DEVICE_STATE_HEALTHY) == UI_AUTO_RUN_WAIT);
  TEST_CASE_(NULL);
}

/**
 * @brief A run nobody is watching says the state it found in a sentence of its own
 *
 * The record that closes such a run is the only trace it leaves, and a bare word tells
 * nobody whether the device was fine, missing, or somebody else's problem.  The faulty
 * state is the one the run answers by working, so it has no sentence here.
 */
static void test_auto_run_says_the_state_it_found(void) {
  TEST_CHECK(strcmp(ui_auto_run_sentence(DEVICE_STATE_HEALTHY), "The device is working.") == 0);
  TEST_CHECK(strcmp(ui_auto_run_sentence(DEVICE_STATE_ABSENT), "No Audient device was found.") == 0);
  TEST_CHECK(strcmp(ui_auto_run_sentence(DEVICE_STATE_PROBLEM), "The device has a problem that this tool cannot solve.") == 0);
  TEST_CHECK(strcmp(ui_auto_run_sentence(DEVICE_STATE_UNKNOWN), "The device state could not be determined.") == 0);
  TEST_CHECK(ui_auto_run_sentence(DEVICE_STATE_FAULTED) == NULL);
}

/**
 * @brief A list of targets with the states named, the way probe_all hands them out
 */
static struct device_target *make_press_targets(enum device_state const *const states, size_t const count) {
  char const *const devnodes[] = {
      "TUSBAUDIO_ENUM\\VID_2708&PID_0001&KS\\6&deadbee&0&2",
      "TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS\\6&deadbee&0&2",
      "TUSBAUDIO_ENUM\\VID_2708&PID_0003&KS\\6&deadbee&0&2",
  };
  struct device_target *targets = NULL;

  for (size_t i = 0; i < count; i++) {
    struct device_target t = {0};
    if (!ov_sprintf_char(&t.devnode, NULL, NULL, "%s", devnodes[i])) {
      return NULL;
    }
    t.state = states[i];
    if (!OV_ARRAY_PUSH(&targets, t)) {
      OV_ARRAY_DESTROY(&t.devnode);
      return NULL;
    }
  }
  return targets;
}

/**
 * @brief The press looks at the devices it finds and answers them at once
 *
 * The state the devices are in when the button is pressed is what the press answers, and
 * nothing looks at it before that: the state of one run must not decide the answer of
 * the next one.
 */
static void test_press_answers_the_devices_it_finds(void) {
  {
    enum device_state const states[] = {DEVICE_STATE_FAULTED};
    struct device_target *targets = make_press_targets(states, 1);
    if (TEST_CHECK(targets != NULL)) {
      TEST_CHECK(ui_press_action(targets, OV_ARRAY_LENGTH(targets)) == UI_PRESS_REPAIR);
      device_targets_release(&targets);
    }
  }
  {
    enum device_state const states[] = {DEVICE_STATE_HEALTHY, DEVICE_STATE_FAULTED};
    struct device_target *targets = make_press_targets(states, 2);
    if (TEST_CHECK(targets != NULL)) {
      TEST_CHECK(ui_press_action(targets, OV_ARRAY_LENGTH(targets)) == UI_PRESS_REPAIR);
      device_targets_release(&targets);
    }
  }
  {
    enum device_state const states[] = {DEVICE_STATE_HEALTHY};
    struct device_target *targets = make_press_targets(states, 1);
    if (TEST_CHECK(targets != NULL)) {
      TEST_CHECK(ui_press_action(targets, OV_ARRAY_LENGTH(targets)) == UI_PRESS_ASK);
      device_targets_release(&targets);
    }
  }
  {
    enum device_state const states[] = {DEVICE_STATE_HEALTHY, DEVICE_STATE_HEALTHY};
    struct device_target *targets = make_press_targets(states, 2);
    if (TEST_CHECK(targets != NULL)) {
      TEST_CHECK(ui_press_action(targets, OV_ARRAY_LENGTH(targets)) == UI_PRESS_ASK);
      device_targets_release(&targets);
    }
  }
  {
    enum device_state const states[] = {DEVICE_STATE_PROBLEM};
    struct device_target *targets = make_press_targets(states, 1);
    if (TEST_CHECK(targets != NULL)) {
      TEST_CHECK(ui_press_action(targets, OV_ARRAY_LENGTH(targets)) == UI_PRESS_NOT_REPAIRABLE);
      device_targets_release(&targets);
    }
  }
  {
    enum device_state const states[] = {DEVICE_STATE_UNKNOWN};
    struct device_target *targets = make_press_targets(states, 1);
    if (TEST_CHECK(targets != NULL)) {
      TEST_CHECK(ui_press_action(targets, OV_ARRAY_LENGTH(targets)) == UI_PRESS_NOT_REPAIRABLE);
      device_targets_release(&targets);
    }
  }
}

/**
 * @brief A press with nothing connected has nothing to answer
 */
static void test_press_with_nothing_connected(void) { TEST_CHECK(ui_press_action(NULL, 0) == UI_PRESS_NOT_REPAIRABLE); }

/**
 * @brief The window belongs to the run that nobody is watching
 *
 * A run a person started keeps its result in front of them, whatever came of it, and a
 * failed run keeps it in front of everybody: the sentence of the failure is the reason
 * the reader is there.  The box that used to ask for this is gone from the window.
 */
static void test_auto_close_only_for_a_run_nobody_watches(void) {
  TEST_CHECK(ui_closes_itself_when_done(false, REPAIR_OUTCOME_CLOSED) == false);
  TEST_CHECK(ui_closes_itself_when_done(true, REPAIR_OUTCOME_CLOSED) == true);
  TEST_CHECK(ui_closes_itself_when_done(true, REPAIR_OUTCOME_FAILED) == false);
  TEST_CHECK(ui_closes_itself_when_done(true, REPAIR_OUTCOME_VETOED) == false);
  TEST_CHECK(ui_closes_itself_when_done(false, REPAIR_OUTCOME_ABSENT) == false);
}

/**
 * @brief The dialog of a finished run is for the window that stays
 *
 * The run nobody watches has what it wanted once the device was released: it says nothing
 * and takes the result with it, and the dialog nobody reads would hold the window open
 * after the run is done.  A failure keeps its sentence in front of the reader, whatever
 * the run was.
 */
static void test_result_dialog_only_for_a_window_that_stays(void) {
  TEST_CASE("a run nobody watches that released the device says nothing");
  TEST_CHECK(ui_shows_result_dialog(true, REPAIR_OUTCOME_CLOSED) == false);

  TEST_CASE("a run a person started says what happened");
  TEST_CHECK(ui_shows_result_dialog(false, REPAIR_OUTCOME_CLOSED) == true);
  TEST_CHECK(ui_shows_result_dialog(false, REPAIR_OUTCOME_VETOED) == true);

  TEST_CASE("a failure is worth its sentence in both cases");
  TEST_CHECK(ui_shows_result_dialog(true, REPAIR_OUTCOME_VETOED) == true);
  TEST_CHECK(ui_shows_result_dialog(true, REPAIR_OUTCOME_FAILED) == true);

  TEST_CASE_(NULL);
}

/**
 * @brief The line of the combo names the device and the unit of it
 *
 * The driver package lists one name for more than one device,
 * and the same device can be connected more than once, so the
 * line carries the devnode of the unit.  A unit no entry names
 * is told by the only string it has.
 */
static void test_device_picker_label(void) {
  struct device_entry const named = {.id = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS", .name = "iD14"};
  struct device_entry const nameless = {.id = "TUSBAUDIO_ENUM\\VID_2708&PID_000B&KS", .name = NULL};
  char line[256];
  char const unit[] = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2A&0&2";

  TEST_CHECK(ui_device_picker_label(&named, unit, line, sizeof(line)) == true);
  TEST_CHECK(strcmp(line, "iD14 (TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2A&0&2)") == 0);
  TEST_CHECK(ui_device_picker_label(&nameless, unit, line, sizeof(line)) == true);
  TEST_CHECK(strcmp(line, "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2A&0&2") == 0);
  TEST_CHECK(ui_device_picker_label(NULL, unit, line, sizeof(line)) == true);
  TEST_CHECK(strcmp(line, "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&D6CB2A&0&2") == 0);
  TEST_CHECK(ui_device_picker_label(&named, NULL, line, sizeof(line)) == false);
  TEST_CHECK(ui_device_picker_label(&named, unit, NULL, sizeof(line)) == false);
  TEST_CHECK(ui_device_picker_label(&named, unit, line, 0) == false);
  TEST_CHECK(ui_device_picker_label(&named, "", line, sizeof(line)) == false);
}

/**
 * @brief Does one control draw with a font of the given dpi
 *
 * @param control the control to read the font of
 * @param dpi the dpi the font has to be made for
 * @return false when the control has no font, or a font that is not there any more
 */
static bool font_of_dpi(HWND const control, int const dpi) {
  HFONT const font = (HFONT)(INT_PTR)SendMessageW(control, WM_GETFONT, 0, 0);
  LOGFONTW lf;

  memset(&lf, 0, sizeof(lf));
  if (font == NULL) {
    return false;
  }
  if (GetObjectW(font, sizeof(lf), &lf) == 0) {
    return false;
  }
  return lf.lfHeight == layout_font_height((UINT)dpi);
}

/**
 * @brief Does every line of the text of one control fit the room it is given
 *
 * The lines of the text are the parts its line
 * breaks separate, and each of them has to fit
 * the room of the control on its own.
 *
 * @param control the control to read
 * @return false when a line is cut off, or nothing could be measured
 */
static bool control_lines_fit(HWND const control) {
  wchar_t text[512];
  HFONT const font = (HFONT)(INT_PTR)SendMessageW(control, WM_GETFONT, 0, 0);
  HDC dc = GetDC(control);
  HFONT old = NULL;
  RECT rc;
  bool fits = true;

  if (dc == NULL) {
    return false;
  }
  memset(text, 0, sizeof(text));
  GetWindowTextW(control, text, (int)(sizeof(text) / sizeof(text[0])) - 1);
  old = (HFONT)SelectObject(dc, font);
  GetClientRect(control, &rc);
  for (wchar_t const *line = text; (line != NULL) && fits;) {
    wchar_t const *const end = wcschr(line, L'\n');
    int const len = (int)((end != NULL) ? (size_t)(end - line) : wcslen(line));
    SIZE size = {0, 0};

    if ((len > 0) && ((GetTextExtentPoint32W(dc, line, len, &size) == 0) || (size.cx > (rc.right - rc.left)))) {
      fits = false;
    }
    line = (end != NULL) ? (end + 1) : NULL;
  }
  if (old != NULL) {
    SelectObject(dc, old);
  }
  ReleaseDC(control, dc);
  return fits;
}

/**
 * @brief Does the line the combo shows fit the room the combo keeps for it
 *
 * The button that opens the list, the frame
 * around the line and the padding the line is
 * drawn away from take room of the combo, so
 * the line has to fit in the room that is left.
 *
 * @param combo the combo to read
 * @return false when the line is cut off, or nothing could be measured
 */
static bool combo_line_fits(HWND const combo) {
  wchar_t text[512];
  HFONT const font = (HFONT)(INT_PTR)SendMessageW(combo, WM_GETFONT, 0, 0);
  HDC dc = GetDC(combo);
  HFONT old = NULL;
  SIZE size = {0, 0};
  RECT rc;
  COMBOBOXINFO info = {0};
  int button = 0;
  bool measured = false;

  if (dc == NULL) {
    return false;
  }
  memset(text, 0, sizeof(text));
  SendMessageW(combo, WM_GETTEXT, (WPARAM)(sizeof(text) / sizeof(text[0])), (LPARAM)text);
  old = (HFONT)SelectObject(dc, font);
  measured = GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size) != FALSE;
  if (old != NULL) {
    SelectObject(dc, old);
  }
  ReleaseDC(combo, dc);
  if (!measured) {
    return false;
  }
  info.cbSize = sizeof(info);
  if (GetComboBoxInfo(combo, &info) && ((info.rcButton.right - info.rcButton.left) > 0)) {
    button = info.rcButton.right - info.rcButton.left;
  } else {
    button = GetSystemMetrics(SM_CXVSCROLL);
  }
  GetClientRect(combo, &rc);
  return (size.cx + button + (2 * GetSystemMetrics(SM_CXEDGE)) + layout_scale(ADC_LAYOUT_PICKER_COMBO_INSET, GetDpiForWindow(combo))) <=
         (rc.right - rc.left);
}

static bool field_name_drawn_whole(HWND const combo, HDC const field, HDC const screen);

/**
 * @brief Read the picker window while it is up, then answer it
 *
 * The units the combo holds are the two
 * the timer was made for, so each line has
 * to name its unit, and the window has to
 * be wide enough for the widest one to be
 * read whole.  The answer is the press of
 * the button that repairs.
 *
 * @param hwnd the owner the timer was made for
 * @param msg the message of the timer
 * @param id the identifier of the timer
 * @param tickcount the tick count when the timer fired
 */
static void CALLBACK picker_test_timer(HWND const hwnd, UINT const msg, UINT_PTR const id, DWORD const tickcount) {
  static wchar_t const first_unit[] = L"iD14 (TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS\\6&deadbee&0&2)";
  static wchar_t const second_unit[] = L"iD14 (TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&feedface&0&3)";
  HWND picker = NULL;
  HWND combo = NULL;
  HWND label = NULL;
  HWND ok_btn = NULL;
  int lines = 0;
  int i = 0;

  (void)msg;
  (void)id;
  (void)tickcount;
  KillTimer(hwnd, ADC_PICKER_TEST_TIMER_ID);
  picker = GetLastActivePopup(hwnd);
  if (!TEST_CHECK((picker != NULL) && (picker != hwnd))) {
    return;
  }
  TEST_CHECK(IsWindowEnabled(hwnd) == FALSE);
  TEST_MSG("want a locked owner while the picker is up");
  combo = FindWindowExW(picker, NULL, WC_COMBOBOXW, NULL);
  if (TEST_CHECK(combo != NULL)) {
    lines = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
    TEST_CHECK_(lines == 2, "want the two units of the device, got %d", lines);
    for (i = 0; i < lines; i++) {
      wchar_t line[512];
      wchar_t const *const want = (i == 0) ? first_unit : second_unit;

      memset(line, 0, sizeof(line));
      SendMessageW(combo, CB_GETLBTEXT, (WPARAM)i, (LPARAM)line);
      TEST_CHECK_(lstrcmpW(line, want) == 0, "unit %d: want [%ls], got [%ls]", i, want, line);
    }
    TEST_CHECK(combo_line_fits(combo));
    TEST_MSG("want the widest unit readable whole in the room the combo keeps");
    if (theme_dark()) {
      // the list of the drop down is a window of the system the control makes for itself:
      // the theme class of the palette gives it its dark look, and this component paints
      // the frame the theme of the system leaves in the light border of the classic
      // frame.  The check reads the frame back through WM_PRINT, which paints into a
      // context the test holds, so the reader never depends on the list being on the
      // screen, and the list never has to be dropped to be read
      COMBOBOXINFO info = {0};

      info.cbSize = sizeof(info);
      if (TEST_CHECK(GetComboBoxInfo(combo, &info) && (info.hwndList != NULL))) {
        HDC dc = GetDC(info.hwndList);
        HDC mem = CreateCompatibleDC(dc);
        RECT rc = {0, 0, 0, 0};
        HBITMAP bmp = NULL;
        HBITMAP old = NULL;
        COLORREF frame_pixel = 0;

        GetWindowRect(info.hwndList, &rc);
        bmp = CreateCompatibleBitmap(dc, rc.right - rc.left, rc.bottom - rc.top);
        if (TEST_CHECK((dc != NULL) && (mem != NULL) && (bmp != NULL))) {
          old = (HBITMAP)SelectObject(mem, bmp);
          SendMessageW(info.hwndList, WM_PRINT, (WPARAM)mem, PRF_NONCLIENT);
          frame_pixel = GetPixel(mem, 0, (rc.bottom - rc.top) / 2);
          SelectObject(mem, old);
          old = NULL;
        }
        if (bmp != NULL) {
          DeleteObject(bmp);
          bmp = NULL;
        }
        if (mem != NULL) {
          DeleteDC(mem);
          mem = NULL;
        }
        if (dc != NULL) {
          ReleaseDC(info.hwndList, dc);
          dc = NULL;
        }
        TEST_CHECK_(
            frame_pixel == RGB(0x39, 0x39, 0x39), "want the frame of the list in the colour of the dark palette, got %08lx", frame_pixel);
        // the rows of the list are painted here, and the reader reads them back
        // through WM_PRINTCLIENT, which paints into a context the test holds
        {
          HDC row_dc = GetDC(NULL);
          HDC row_mem = CreateCompatibleDC(row_dc);
          HBITMAP row_bmp = NULL;
          HBITMAP row_old = NULL;
          COLORREF row = 0;
          COLORREF chosen_row = 0;
          RECT cr = {0, 0, 0, 0};
          LONG const item_height = (LONG)SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0);

          GetClientRect(info.hwndList, &cr);
          row_bmp = CreateCompatibleBitmap(row_dc, cr.right - cr.left, cr.bottom - cr.top);
          if (TEST_CHECK((row_mem != NULL) && (row_bmp != NULL))) {
            row_old = (HBITMAP)SelectObject(row_mem, row_bmp);
            SendMessageW(info.hwndList, WM_PRINTCLIENT, (WPARAM)row_mem, PRF_CLIENT);
            // the first row is the one the reader chose, the second one is not,
            // and the name of a unit starts a little into the row, so the edge
            // of the row is the face of the row
            row = GetPixel(row_mem, 0, item_height + 3);
            chosen_row = GetPixel(row_mem, 0, 3);
            SelectObject(row_mem, row_old);
            row_old = NULL;
          }
          if (row_bmp != NULL) {
            DeleteObject(row_bmp);
            row_bmp = NULL;
          }
          if (row_mem != NULL) {
            DeleteDC(row_mem);
            row_mem = NULL;
          }
          if (row_dc != NULL) {
            ReleaseDC(NULL, row_dc);
            row_dc = NULL;
          }
          TEST_CHECK_(row == RGB(0x2B, 0x2B, 0x2B), "want the face of a row of the list in the colour of the dark palette, got %08lx", row);
          TEST_CHECK_(chosen_row == GetSysColor(COLOR_HIGHLIGHT),
                      "want the chosen row in the colour of the selection of Windows, got %08lx",
                      chosen_row);
        }
      }
    }
    if (theme_dark()) {
      // the rows the reader sees are the ones the screen shows once the list is dropped, so
      // the check drops the list, has it painted now, and reads the screen where the rows are
      COMBOBOXINFO dd_info;
      LONG const item_height = (LONG)SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0);
      HDC screen = GetDC(NULL);
      POINT plain = {0, item_height + 3};
      POINT chosen = {0, 3};
      COLORREF plain_px = 0;
      COLORREF chosen_px = 0;

      memset(&dd_info, 0, sizeof(dd_info));
      dd_info.cbSize = sizeof(dd_info);
      GetComboBoxInfo(combo, &dd_info);
      {
        // the reader opens the list the way a click on the arrow does
        LONG const arrow_x = (dd_info.rcButton.left + dd_info.rcButton.right) / 2;
        LONG const arrow_y = (dd_info.rcButton.top + dd_info.rcButton.bottom) / 2;
        SendMessageW(combo, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(arrow_x, arrow_y));
        SendMessageW(combo, WM_LBUTTONUP, 0, MAKELPARAM(arrow_x, arrow_y));
      }
      RedrawWindow(dd_info.hwndList, NULL, NULL, RDW_UPDATENOW);
      ClientToScreen(dd_info.hwndList, &plain);
      ClientToScreen(dd_info.hwndList, &chosen);
      if (screen != NULL) {
        plain_px = GetPixel(screen, plain.x, plain.y);
        chosen_px = GetPixel(screen, chosen.x, chosen.y);
        ReleaseDC(NULL, screen);
      }
      SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
      TEST_CHECK_(plain_px == RGB(0x2B, 0x2B, 0x2B),
                  "want the unchosen row of the open list in the colour of the dark palette, got %08lx",
                  plain_px);
      TEST_CHECK_(chosen_px == GetSysColor(COLOR_HIGHLIGHT),
                  "want the chosen row of the open list in the colour of the selection, got %08lx",
                  chosen_px);
    }
    if (theme_dark()) {
      // the list is erased before it is painted, and an animation of the drop shows the area
      // the erase covers first, so the erase has to fill it in the palette too
      HDC erase_dc = GetDC(NULL);
      HDC erase_mem = CreateCompatibleDC(erase_dc);
      HBITMAP erase_bmp = CreateCompatibleBitmap(erase_dc, 64, 64);
      HBITMAP erase_old = NULL;
      RECT erase_rc = {0, 0, 64, 64};
      COLORREF erased = 0;
      COMBOBOXINFO erase_info;

      memset(&erase_info, 0, sizeof(erase_info));
      erase_info.cbSize = sizeof(erase_info);
      GetComboBoxInfo(combo, &erase_info);
      if ((erase_mem != NULL) && (erase_bmp != NULL)) {
        erase_old = (HBITMAP)SelectObject(erase_mem, erase_bmp);
        FillRect(erase_mem, &erase_rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        SendMessageW(erase_info.hwndList, WM_ERASEBKGND, (WPARAM)erase_mem, 0);
        erased = GetPixel(erase_mem, 5, 5);
        SelectObject(erase_mem, erase_old);
      }
      if (erase_bmp != NULL) {
        DeleteObject(erase_bmp);
      }
      if (erase_mem != NULL) {
        DeleteDC(erase_mem);
      }
      ReleaseDC(NULL, erase_dc);
      TEST_CHECK_(erased == RGB(0x2B, 0x2B, 0x2B), "want the erase of the list in the colour of the dark palette, got %08lx", erased);
    }
    if (theme_dark()) {
      // the selection is moved with the arrow keys while the list is open, and the rows the
      // screen shows then have to follow the selection, so the check opens the list, moves
      // the selection down, has the list painted now and reads the screen where the rows are
      COMBOBOXINFO key_info;
      LONG const item_height = (LONG)SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0);
      HDC screen = NULL;
      POINT first = {0, 3};
      POINT second = {0, item_height + 3};
      COLORREF first_px = 0;
      COLORREF second_px = 0;
      LRESULT moved = 0;

      memset(&key_info, 0, sizeof(key_info));
      key_info.cbSize = sizeof(key_info);
      GetComboBoxInfo(combo, &key_info);
      {
        LONG const arrow_x = (key_info.rcButton.left + key_info.rcButton.right) / 2;
        LONG const arrow_y = (key_info.rcButton.top + key_info.rcButton.bottom) / 2;
        SendMessageW(combo, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(arrow_x, arrow_y));
        SendMessageW(combo, WM_LBUTTONUP, 0, MAKELPARAM(arrow_x, arrow_y));
      }
      SendMessageW(key_info.hwndList, WM_KEYDOWN, VK_DOWN, 0);
      SendMessageW(key_info.hwndList, WM_KEYUP, VK_DOWN, 0);
      moved = SendMessageW(key_info.hwndList, LB_GETCURSEL, 0, 0);
      RedrawWindow(key_info.hwndList, NULL, NULL, RDW_UPDATENOW);
      ClientToScreen(key_info.hwndList, &first);
      ClientToScreen(key_info.hwndList, &second);
      screen = GetDC(NULL);
      if (screen != NULL) {
        first_px = GetPixel(screen, first.x, first.y);
        second_px = GetPixel(screen, second.x, second.y);
        ReleaseDC(NULL, screen);
      }
      // the choice is put back to the first unit, the window answers with the chosen one
      SendMessageW(key_info.hwndList, WM_KEYDOWN, VK_UP, 0);
      SendMessageW(key_info.hwndList, WM_KEYUP, VK_UP, 0);
      SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
      TEST_CHECK_(moved == 1, "want the arrow key to choose the second unit of the open list, got %ld", (long)moved);
      TEST_CHECK_(first_px == RGB(0x2B, 0x2B, 0x2B),
                  "want the first row, now unchosen, in the colour of the dark palette after the arrow key, got %08lx",
                  first_px);
      TEST_CHECK_(second_px == GetSysColor(COLOR_HIGHLIGHT),
                  "want the second row, now chosen, in the colour of the selection after the arrow key, got %08lx",
                  second_px);
    }
    if (theme_dark()) {
      // the field of a drop down list is painted from the theme, which is light in every
      // state, so the component paints it: the check reads the field back through
      // WM_PRINTCLIENT, which paints without the window and never races a repaint
      HDC screen = GetDC(combo);
      HDC mem = CreateCompatibleDC(screen);
      HBITMAP bmp = NULL;
      HBITMAP old = NULL;
      RECT rc;
      COLORREF frame_pixel = 0;
      COLORREF face_pixel = 0;
      bool name_whole = false;

      GetClientRect(combo, &rc);
      bmp = CreateCompatibleBitmap(screen, rc.right - rc.left, rc.bottom - rc.top);
      if ((screen != NULL) && (mem != NULL) && (bmp != NULL)) {
        old = (HBITMAP)SelectObject(mem, bmp);
        SendMessageW(combo, WM_PRINTCLIENT, (WPARAM)mem, PRF_CLIENT);
        frame_pixel = GetPixel(mem, 0, (rc.bottom - rc.top) / 2);
        face_pixel = GetPixel(mem, 1, 1);
        name_whole = field_name_drawn_whole(combo, mem, screen);
        SelectObject(mem, old);
        old = NULL;
      }
      if (bmp != NULL) {
        DeleteObject(bmp);
        bmp = NULL;
      }
      if (mem != NULL) {
        DeleteDC(mem);
        mem = NULL;
      }
      if (screen != NULL) {
        ReleaseDC(combo, screen);
        screen = NULL;
      }
      TEST_CHECK_(
          frame_pixel == RGB(0x39, 0x39, 0x39), "want the frame of the field in the colour of the dark palette, got %08lx", frame_pixel);
      TEST_CHECK_(
          face_pixel == RGB(0x2D, 0x2D, 0x2D), "want the face of the field in the colour of the dark palette, got %08lx", face_pixel);
      TEST_CHECK_(name_whole, "want the chosen name drawn whole in the field, the sign of an accelerator must not be eaten");
    }
  }
  label = FindWindowExW(picker, NULL, L"STATIC", NULL);
  if (TEST_CHECK(label != NULL)) {
    TEST_CHECK(control_lines_fit(label));
    TEST_MSG("want every sentence of the question readable whole in the room it is given");
  }
  ok_btn = FindWindowExW(picker, NULL, L"Button", NULL);
  if (!TEST_CHECK(ok_btn != NULL)) {
    return;
  }
  TEST_CHECK(font_of_dpi(ok_btn, (int)GetDpiForWindow(picker)));
  TEST_MSG("want the button drawn with a font of the window");
  SendMessageW(picker, WM_COMMAND, MAKEWPARAM(IDC_PICKER_OK, BN_CLICKED), (LPARAM)ok_btn);
  TEST_CHECK(!IsWindow(picker));
  TEST_MSG("want the picker window gone after the press of its repair button");
}

/**
 * @brief Does the field of a drop down list draw the chosen name whole
 *
 * A name carries the sign of an accelerator.  A field that draws the name as a prefix eats
 * the sign and underlines the letter after it.  The check draws the same name, with the
 * sign kept, in the palette of the field, and compares the band the name fills with the
 * field.  The edges of the field are left out, the focus cue is drawn there.
 *
 * @param combo the combo box whose field was read
 * @param field the context the field was printed into
 * @param screen a context of the screen, the reference is made in a context like it
 * @return true when the name is drawn whole in the field
 */
static bool field_name_drawn_whole(HWND const combo, HDC const field, HDC const screen) {
  enum { field_text_inset = 2, edge_skip = 1 };
  wchar_t name[256];
  int const cur = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
  LRESULT const font_value = SendMessageW(combo, WM_GETFONT, 0, 0);
  HFONT const font = (HFONT)(void *)font_value;
  COMBOBOXINFO info;
  RECT box = {0, 0, 0, 0};
  RECT client = {0, 0, 0, 0};
  RECT measure = {0, 0, 0, 0};
  HDC ref = NULL;
  HBITMAP ref_bmp = NULL;
  HBITMAP ref_old = NULL;
  HFONT ref_font = NULL;
  HBRUSH face = NULL;
  LONG end = 0;
  LONG x = 0;
  LONG y = 0;
  bool made = false;
  bool differs = false;

  memset(name, 0, sizeof(name));
  if ((cur == CB_ERR) || (font == NULL) || (SendMessageW(combo, CB_GETLBTEXT, (WPARAM)cur, (LPARAM)name) == CB_ERR)) {
    return false;
  }
  memset(&info, 0, sizeof(info));
  info.cbSize = sizeof(info);
  if (!GetComboBoxInfo(combo, &info)) {
    return false;
  }
  box = info.rcItem;
  InflateRect(&box, -layout_scale(field_text_inset, (UINT)GetDpiForWindow(combo)), 0);
  GetClientRect(combo, &client);
  ref = CreateCompatibleDC(screen);
  ref_bmp = CreateCompatibleBitmap(screen, client.right - client.left, client.bottom - client.top);
  face = CreateSolidBrush(RGB(0x2D, 0x2D, 0x2D));
  if ((ref == NULL) || (ref_bmp == NULL) || (face == NULL)) {
    goto cleanup;
  }
  made = true;
  ref_old = (HBITMAP)SelectObject(ref, ref_bmp);
  ref_font = (HFONT)SelectObject(ref, font);
  FillRect(ref, &client, face);
  SetBkMode(ref, TRANSPARENT);
  SetTextColor(ref, theme_palette_text());
  DrawTextW(ref, name, -1, &measure, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
  DrawTextW(ref, name, -1, &box, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  end = box.left + measure.right;
  if (end > box.right - edge_skip) {
    end = box.right - edge_skip;
  }
  for (y = box.top + edge_skip; (y < box.bottom - edge_skip) && !differs; y++) {
    for (x = box.left + edge_skip; (x < end) && !differs; x++) {
      if (GetPixel(field, x, y) != GetPixel(ref, x, y)) {
        differs = true;
      }
    }
  }

cleanup:
  if (ref_font != NULL) {
    SelectObject(ref, ref_font);
  }
  if (ref_old != NULL) {
    SelectObject(ref, ref_old);
  }
  if (face != NULL) {
    DeleteObject(face);
  }
  if (ref_bmp != NULL) {
    DeleteObject(ref_bmp);
  }
  if (ref != NULL) {
    DeleteDC(ref);
  }
  return made && !differs;
}

/**
 * @brief The picker names every unit of the device and answers with the one
 *
 * Two units of one device are connected, so the
 * combo has to tell them apart by the unit
 * each line names, and the window has to be
 * wide enough for the widest one to be read
 * whole.  The reader answers with the button
 * that repairs, and the window hands the unit
 * it held selected back to the caller.
 */
static void test_picker_window_names_every_unit(void) {
  struct device_entry *devices = NULL;
  struct device_target *targets = NULL;
  struct device_target *picked = NULL;
  struct settings s;
  struct device_entry entry;
  struct device_target target;
  struct ov_error err = {0};
  ATOM atom = 0;
  HFONT font = NULL;
  HWND owner = NULL;
  WNDCLASSEXW wc;

  memset(&s, 0, sizeof(s));
  memset(&entry, 0, sizeof(entry));
  entry.id = "TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS";
  entry.name = "iD14";
  TEST_CHECK(OV_ARRAY_PUSH(&devices, entry));
  entry.id = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS";
  TEST_CHECK(OV_ARRAY_PUSH(&devices, entry));
  s.devices = devices;
  memset(&target, 0, sizeof(target));
  target.devnode = "TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS\\6&deadbee&0&2";
  target.state = DEVICE_STATE_PROBLEM;
  TEST_CHECK(OV_ARRAY_PUSH(&targets, target));
  target.devnode = "TUSBAUDIO_ENUM\\VID_2708&PID_0008&KS\\6&feedface&0&3";
  target.state = DEVICE_STATE_PROBLEM;
  TEST_CHECK(OV_ARRAY_PUSH(&targets, target));

  TEST_CHECK(theme_init(true, &err));
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush();
  wc.lpszClassName = L"adc_picker_test_owner";
  atom = RegisterClassExW(&wc);
  TEST_CHECK(atom != 0);
  font = CreateFontW(layout_font_height(USER_DEFAULT_SCREEN_DPI),
                     0,
                     0,
                     0,
                     FW_NORMAL,
                     FALSE,
                     FALSE,
                     FALSE,
                     DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE,
                     L"Segoe UI");
  if (!TEST_CHECK(font != NULL)) {
    goto cleanup;
  }
  owner = CreateWindowExW(0, wc.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(owner != NULL)) {
    goto cleanup;
  }
  theme_attach(owner);
  SendMessageW(owner, WM_SETFONT, (WPARAM)font, TRUE);
  ShowWindow(owner, SW_SHOW);

  TEST_CASE("the picker names every unit and answers with the one it held");
  {
    UINT_PTR timer = 0;

    timer = SetTimer(owner, ADC_PICKER_TEST_TIMER_ID, ADC_PICKER_TEST_TIMER_MS, picker_test_timer);
    if (!TEST_CHECK(timer != 0)) {
      goto cleanup;
    }
    if (!TEST_SUCCEEDED(ui_device_picker_show(owner, &s, targets, &picked, &err), &err)) {
      goto cleanup;
    }
    KillTimer(owner, ADC_PICKER_TEST_TIMER_ID);
    TEST_CHECK(IsWindowEnabled(owner) != 0);
    TEST_MSG("want the owner unlocked when the picker is done");
    TEST_CHECK(GetActiveWindow() == owner);
    TEST_MSG("want the owner active again after the popup is gone");
    if (TEST_CHECK(picked != NULL)) {
      TEST_CHECK_(strcmp(picked->devnode, "TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS\\6&deadbee&0&2") == 0,
                  "want the first unit, got [%s]",
                  picked->devnode);
    }
    device_targets_release(&picked);
  }
  TEST_CASE_(NULL);

cleanup:
  if (owner != NULL) {
    DestroyWindow(owner);
  }
  if (font != NULL) {
    DeleteObject(font);
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
  }
  OV_ARRAY_DESTROY(&targets);
  OV_ARRAY_DESTROY(&devices);
}

/**
 * @brief Does the whole text of one control fit in the room the control is given
 *
 * @param control the control to read
 * @return false when the text is cut off, or nothing could be measured
 */
static bool text_fits(HWND const control) {
  wchar_t text[256];
  HFONT const font = (HFONT)(INT_PTR)SendMessageW(control, WM_GETFONT, 0, 0);
  HDC dc = GetDC(control);
  HFONT old = NULL;
  SIZE size = {0, 0};
  RECT rc;
  bool measured = false;

  if (dc == NULL) {
    return false;
  }
  memset(text, 0, sizeof(text));
  GetWindowTextW(control, text, (int)(sizeof(text) / sizeof(text[0])) - 1);
  old = (HFONT)SelectObject(dc, font);
  measured = GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size) != FALSE;
  if (old != NULL) {
    SelectObject(dc, old);
  }
  ReleaseDC(control, dc);
  if (!measured) {
    return false;
  }
  GetClientRect(control, &rc);
  return size.cx <= (rc.right - rc.left);
}

/**
 * @brief Press the OK button of the blockers window, the way a reader does
 *
 * The timer plays the reader while the modal loop of ui_blockers_window_show runs: the
 * window is the popup the owner last activated, and its OK button is the button child it
 * carries.  It also reads what the list shows, so the content of the window is checked
 * while it is up.
 *
 * @param hwnd the owner the timer was set on
 * @param msg the timer message
 * @param id the identifier of the timer
 * @param tickcount the tick count when the timer fired
 */
static void CALLBACK blockers_test_timer(HWND const hwnd, UINT const msg, UINT_PTR const id, DWORD const tickcount) {
  HWND blockers = NULL;
  HWND list = NULL;
  HWND ok_btn = NULL;
  int rows = 0;
  int col = 0;

  (void)msg;
  (void)id;
  (void)tickcount;
  KillTimer(hwnd, ADC_BLOCKERS_TEST_TIMER_ID);
  blockers = GetLastActivePopup(hwnd);
  if (blockers == NULL || blockers == hwnd) {
    TEST_CHECK(false && "want the blockers window as the popup of the owner");
    return;
  }
  TEST_CHECK(IsWindowEnabled(hwnd) == FALSE);
  TEST_MSG("want a locked owner while the list is up");
  list = FindWindowExW(blockers, NULL, WC_LISTVIEWW, NULL);
  if (TEST_CHECK(list != NULL)) {
    wchar_t pid_text[32];
    LVITEMW item;

    TEST_CHECK((SendMessageW(list, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0) & LVS_EX_DOUBLEBUFFER) != 0);
    TEST_MSG("want the list view to paint through a buffer of its own");
    rows = (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
    TEST_CHECK_(rows == 2, "want the two records of the veto, got %d", rows);
    memset(&item, 0, sizeof(item));
    item.iSubItem = 0;
    item.pszText = pid_text;
    item.cchTextMax = (int)(sizeof(pid_text) / sizeof(pid_text[0]));
    SendMessageW(list, LVM_GETITEMTEXTW, (WPARAM)0, (LPARAM)&item);
    TEST_CHECK_(lstrcmpW(pid_text, L"100") == 0, "want the pid of the first record, got [%ls]", pid_text);
    for (col = 0; col < 3; col++) {
      wchar_t title[64];
      wchar_t want[64];
      LVCOLUMNW c;
      char const *const caption = (col == 0) ? "PID" : ((col == 1) ? "Process" : "Blocked device");

      memset(title, 0, sizeof(title));
      memset(want, 0, sizeof(want));
      memset(&c, 0, sizeof(c));
      c.mask = LVCF_TEXT;
      c.pszText = title;
      c.cchTextMax = (int)(sizeof(title) / sizeof(title[0]));
      if (!TEST_CHECK(SendMessageW(list, LVM_GETCOLUMNW, (WPARAM)col, (LPARAM)&c) != 0)) {
        continue;
      }
      OV_SNPRINTF(want, sizeof(want) / sizeof(WCHAR), L"%1$s", L"%1$s", caption);
      TEST_CHECK_(lstrcmpW(title, want) == 0, "column %d: want [%ls], got [%ls]", col, want, title);
    }
  }
  ok_btn = FindWindowExW(blockers, NULL, L"Button", NULL);
  if (!TEST_CHECK(ok_btn != NULL)) {
    return;
  }
  TEST_CASE("the blockers window draws with a font of its own dpi");
  {
    int const dpi = (int)GetDpiForWindow(blockers);
    HWND label = FindWindowExW(blockers, NULL, L"STATIC", NULL);
    HFONT const borrowed = (HFONT)(INT_PTR)SendMessageW(hwnd, WM_GETFONT, 0, 0);

    if (TEST_CHECK(label != NULL)) {
      TEST_CHECK(font_of_dpi(label, dpi));
      TEST_MSG("want the label of the list drawn with a font for dpi %d", dpi);
      TEST_CHECK(text_fits(label));
      TEST_MSG("want the sentence of the window readable whole in the room it is given");
      TEST_CHECK_((INT_PTR)SendMessageW(label, WM_GETFONT, 0, 0) != (INT_PTR)borrowed,
                  "want a font of the window itself, got the one the owner lends");
    }
    if (list != NULL) {
      TEST_CHECK(font_of_dpi(list, dpi));
      TEST_MSG("want the list drawn with a font for dpi %d", dpi);
    }
    TEST_CHECK(font_of_dpi(ok_btn, dpi));
    TEST_MSG("want the button drawn with a font for dpi %d", dpi);
  }
  TEST_CASE_(NULL);

  TEST_CASE("the smallest size the reader may resize to carries the frame");
  {
    MINMAXINFO limits;
    RECT smallest;
    UINT const dpi = GetDpiForWindow(blockers);

    memset(&limits, 0, sizeof(limits));
    SendMessageW(blockers, WM_GETMINMAXINFO, 0, (LPARAM)&limits);
    layout_window_rect(layout_scale(ADC_LAYOUT_BLOCKERS_MIN_W, dpi),
                       layout_scale(ADC_LAYOUT_BLOCKERS_MIN_H, dpi),
                       dpi,
                       (DWORD)GetWindowLongPtrW(blockers, GWL_STYLE),
                       (DWORD)GetWindowLongPtrW(blockers, GWL_EXSTYLE),
                       &smallest);
    TEST_CHECK_((int)limits.ptMinTrackSize.x == (smallest.right - smallest.left),
                "want the window %d wide at its smallest, it is held at %d",
                (int)(smallest.right - smallest.left),
                (int)limits.ptMinTrackSize.x);
    TEST_CHECK_((int)limits.ptMinTrackSize.y == (smallest.bottom - smallest.top),
                "want the window %d tall at its smallest, it is held at %d",
                (int)(smallest.bottom - smallest.top),
                (int)limits.ptMinTrackSize.y);
  }
  TEST_CASE_(NULL);

  SendMessageW(blockers, WM_COMMAND, MAKEWPARAM(IDC_BLOCKERS_OK, BN_CLICKED), (LPARAM)ok_btn);
}

static void test_blockers_window_shows_the_processes(void) {
  struct culprit *culprits = NULL;
  struct culprit record;
  struct ov_error berr = {0};
  ATOM atom = 0;
  HFONT font = NULL;
  HWND owner = NULL;
  HWND blockers = NULL;
  WNDCLASSEXW wc;

  memset(&record, 0, sizeof(record));
  record.pid = 100;
  record.path = "\\Device\\HarddiskVolume1\\Program Files\\Audient\\iD\\iD.exe";
  record.device_instance = "USB\\VID_2708&PID_0008";
  TEST_CHECK(OV_ARRAY_PUSH(&culprits, record));
  record.pid = 200;
  record.path = "C:\\WINDOWS\\system32\\audiodg.exe";
  record.device_instance = NULL;
  TEST_CHECK(OV_ARRAY_PUSH(&culprits, record));

  TEST_CHECK(theme_init(true, &berr));
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush();
  wc.lpszClassName = L"adc_blockers_test_owner";
  atom = RegisterClassExW(&wc);
  TEST_CHECK(atom != 0);
  font = CreateFontW(layout_font_height(USER_DEFAULT_SCREEN_DPI),
                     0,
                     0,
                     0,
                     FW_NORMAL,
                     FALSE,
                     FALSE,
                     FALSE,
                     DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE,
                     L"Segoe UI");
  if (!TEST_CHECK(font != NULL)) {
    goto cleanup;
  }
  owner = CreateWindowExW(0, wc.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(owner != NULL)) {
    goto cleanup;
  }
  theme_attach(owner);
  SendMessageW(owner, WM_SETFONT, (WPARAM)font, TRUE);
  ShowWindow(owner, SW_SHOW);

  TEST_CASE("the blockers window opens over the main window and takes it back");
  {
    UINT_PTR timer = 0;

    timer = SetTimer(owner, ADC_BLOCKERS_TEST_TIMER_ID, ADC_BLOCKERS_TEST_TIMER_MS, blockers_test_timer);
    if (!TEST_CHECK(timer != 0)) {
      goto cleanup;
    }
    if (!TEST_SUCCEEDED(ui_blockers_window_show(owner, culprits, &berr), &berr)) {
      goto cleanup;
    }
    KillTimer(owner, ADC_BLOCKERS_TEST_TIMER_ID);
    TEST_CHECK(!IsWindow(blockers));
    TEST_MSG("want the blockers window gone after the press of its OK button");
    TEST_CHECK(IsWindowEnabled(owner) != 0);
    TEST_MSG("want the owner unlocked when the list is done");
    TEST_CHECK(GetActiveWindow() == owner);
    TEST_MSG("want the owner active again after the popup is gone");
    blockers = NULL;
  }
  TEST_CASE_(NULL);

cleanup:
  if ((blockers != NULL) && IsWindow(blockers)) {
    DestroyWindow(blockers);
  }
  if (owner != NULL) {
    DestroyWindow(owner);
  }
  if (font != NULL) {
    DeleteObject(font);
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
  }
  OV_ARRAY_DESTROY(&culprits);
}

/**
 * @brief The abort press and the close of the progress window are asks, not ends
 *
 * The window hands the ask over the flag its caller owns, greys its own ways out and stays
 * where it is: only the run that made it takes it down.  Every press here is a message sent
 * to a window of the test thread itself, so its answer is true when the send has returned
 * and no moment of the clock has to be trusted.
 */
static void test_progress_window_greys_the_asks_and_stays(void) {
  struct ov_error perr = {0};
  bool cancel = false;
  ATOM atom = 0;
  HWND owner = NULL;
  HWND progress = NULL;
  WNDCLASSEXW wc;

  TEST_CHECK(theme_init(true, &perr));
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush();
  wc.lpszClassName = L"adc_progress_ask_owner";
  atom = RegisterClassExW(&wc);
  TEST_CHECK(atom != 0);
  owner = CreateWindowExW(0, wc.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(owner != NULL)) {
    goto cleanup;
  }
  if (!TEST_SUCCEEDED(ui_progress_window_create(
                          owner, (int)GetDpiForWindow(owner), ui_progress_window_steps(false, 1), &cancel, NULL, NULL, &progress, &perr),
                      &perr)) {
    goto cleanup;
  }

  TEST_CASE("the press of abort raises the flag of the caller and greys the button");
  {
    HWND cancel_btn = FindWindowExW(progress, NULL, L"Button", NULL);

    if (TEST_CHECK(cancel_btn != NULL)) {
      SendMessageW(progress, WM_COMMAND, MAKEWPARAM(IDC_PROGRESS_CANCEL, BN_CLICKED), (LPARAM)cancel_btn);
      TEST_CHECK(cancel == true);
      TEST_MSG("want the press to hand the ask to the flag the caller owns");
      TEST_CHECK(IsWindowEnabled(cancel_btn) == FALSE);
      TEST_MSG("want the greyed button to say the ask was heard");
      SendMessageW(progress, WM_COMMAND, MAKEWPARAM(IDC_PROGRESS_CANCEL, BN_CLICKED), (LPARAM)cancel_btn);
      TEST_CHECK(IsWindowEnabled(cancel_btn) == FALSE);
      TEST_MSG("want a second press to find nothing left to press");
    }
  }
  TEST_CASE_(NULL);

  TEST_CASE("the close of the window is an ask too, and it does not end the window");
  SendMessageW(progress, WM_CLOSE, 0, 0);
  TEST_CHECK(IsWindow(progress) != 0);
  TEST_MSG("want the progress window to stay until the run that made it is done");

  TEST_CASE("only the hands of the run take the window down");
  ui_progress_window_destroy(&progress);
  TEST_CHECK(progress == NULL);
  TEST_CHECK(IsWindow(progress) == 0);
  TEST_CASE_(NULL);

cleanup:
  if (progress != NULL) {
    ui_progress_window_destroy(&progress);
  }
  if (owner != NULL) {
    DestroyWindow(owner);
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
  }
  theme_release();
}

/**
 * @brief The progress bar follows the palette the way the list view does
 *
 * The dark class set of the theme has no progress parts, so a theme class cannot turn the
 * bar: with the dark palette the theme has to come off the bar and the palette has to
 * colour it, with the light palette the system theme draws it again.  The window of a run
 * lives on another thread, so this test makes a bar of its own on the thread it runs on.
 * The light side asks the theme manager for the progress parts directly: GetWindowTheme()
 * only answers once a paint has opened them, which a freshly created bar has not done yet.
 */
static void test_progress_bar_follows_the_palette(void) {
  struct ov_error berr = {0};
  HWND parent = NULL;
  HWND bar = NULL;

  TEST_CHECK(theme_init(true, &berr));
  parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 100, 60, NULL, NULL, GetModuleHandleW(NULL), NULL);
  if (!TEST_CHECK(parent != NULL)) {
    goto cleanup;
  }
  bar = CreateWindowExW(
      0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 100, 20, parent, NULL, GetModuleHandleW(NULL), NULL);
  if (!TEST_CHECK(bar != NULL)) {
    goto cleanup;
  }
  theme_attach(bar);
  {
    COLORREF const fill = (COLORREF)SendMessageW(bar, PBM_GETBARCOLOR, 0, 0);
    COLORREF const channel = (COLORREF)SendMessageW(bar, PBM_GETBKCOLOR, 0, 0);

    if (theme_dark()) {
      TEST_CHECK(GetWindowTheme(bar) == NULL);
      TEST_MSG("want the bar without a theme class in the dark palette");
      TEST_CHECK_(fill != (COLORREF)CLR_DEFAULT, "want a fill colour of the palette, got 0x%08lX", (unsigned long)fill);
      TEST_CHECK_(channel != (COLORREF)CLR_DEFAULT, "want a channel colour of the palette, got 0x%08lX", (unsigned long)channel);
    } else {
      HTHEME const system_theme = OpenThemeData(bar, L"Progress");
      TEST_CHECK(system_theme != NULL);
      TEST_MSG("want the system theme to draw the bar in the light palette");
      if (system_theme != NULL) {
        CloseThemeData(system_theme);
      }
      TEST_CHECK_(fill == (COLORREF)CLR_DEFAULT, "want the default fill back, got 0x%08lX", (unsigned long)fill);
      TEST_CHECK_(channel == (COLORREF)CLR_DEFAULT, "want the default channel back, got 0x%08lX", (unsigned long)channel);
    }
  }

cleanup:
  if (bar != NULL) {
    DestroyWindow(bar);
  }
  if (parent != NULL) {
    DestroyWindow(parent);
  }
  theme_release();
}

/**
 * @brief What the probe window of the palette broadcast test saw
 */
struct theme_probe {
  unsigned theme_changed;
  unsigned painted;
};

/**
 * @brief A window that talks to the theme the way the windows of the application do
 *
 * The probe counts the two things a palette change has to bring to every window: the
 * re-apply that theme_reapply() announces with WM_THEMECHANGED, and the redraw that comes
 * with WM_PAINT.
 */
static LRESULT CALLBACK theme_probe_wndproc(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam) {
  struct theme_probe *probe = (struct theme_probe *)(INT_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

  if (msg == WM_NCCREATE) {
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW const *)lparam)->lpCreateParams);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (probe != NULL) {
    if (msg == WM_THEMECHANGED) {
      probe->theme_changed++;
      return 0;
    }
    if (msg == WM_PAINT) {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      probe->painted++;
      return 0;
    }
  }
  {
    LRESULT result = 0;
    bool handled = false;

    if (!theme_message(hwnd, msg, wparam, lparam, &result, &handled, NULL)) {
      return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (handled) {
      return result;
    }
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/**
 * @brief A palette change reaches every window, not only the first one
 *
 * The shell broadcasts the change to every top-level window and the theme reads the new
 * palette once: the window that answers the broadcast first must not consume the change.
 * A modal dialog is a second top-level window, so with one open the old code updated one
 * window and left the other on the old palette.  The probe plays the owner of the modal,
 * which sits disabled behind the dialog: it has to re-apply and to redraw all the same.
 */
static void test_theme_change_reaches_every_window(void) {
  struct ov_error berr = {0};
  WNDCLASSEXW wc;
  struct theme_probe probe_owner = {0};
  struct theme_probe probe_dialog = {0};
  HWND owner = NULL;
  HWND dialog = NULL;
  ATOM atom = 0;

  TEST_CHECK(theme_init(true, &berr));
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = theme_probe_wndproc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  wc.lpszClassName = L"adc_theme_probe_test";
  atom = RegisterClassExW(&wc);
  if (!TEST_CHECK(atom != 0)) {
    goto cleanup;
  }
  owner = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE, 0, 0, 100, 60, NULL, NULL, wc.hInstance, &probe_owner);
  dialog = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE, 0, 0, 100, 60, NULL, NULL, wc.hInstance, &probe_dialog);
  if (!TEST_CHECK(owner != NULL && dialog != NULL)) {
    goto cleanup;
  }
  EnableWindow(owner, FALSE);

  SendMessageW(owner, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet");
  SendMessageW(dialog, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet");

  TEST_CHECK_(probe_owner.theme_changed >= 1, "want the disabled owner re-applied, got %u", (unsigned)probe_owner.theme_changed);
  TEST_CHECK_(probe_dialog.theme_changed >= 1, "want the dialog re-applied, got %u", (unsigned)probe_dialog.theme_changed);
  TEST_CHECK_(probe_owner.painted >= 1, "want the disabled owner painted, got %u", (unsigned)probe_owner.painted);
  TEST_CHECK_(probe_dialog.painted >= 1, "want the dialog painted, got %u", (unsigned)probe_dialog.painted);

cleanup:
  if (owner != NULL) {
    DestroyWindow(owner);
  }
  if (dialog != NULL) {
    DestroyWindow(dialog);
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
  }
  theme_release();
}

/**
 * @brief The switch that keeps the theme out of the process
 *
 * The undocumented entry points the theme works through are what a
 * future Windows may change and what an older one never had, so one
 * switch has to be able to detach the whole theme: no window is
 * themed, no painting is taken over and no colour message is answered
 * by the application.  A button, a bar and a list with its header are
 * made here, and each of them has to stay exactly as the system made
 * it.
 */
static void test_theme_switch_detaches_the_theme(void) {
  struct ov_error berr = {0};
  struct ov_error merr = {0};
  WNDCLASSEXW wc;
  ATOM atom = 0;
  HWND owner = NULL;
  HWND button = NULL;
  HWND bar = NULL;
  HWND list = NULL;
  HWND header = NULL;
  LONG_PTR button_proc = 0;
  LONG_PTR header_proc = 0;
  LRESULT result = 0;
  bool handled = true;
  HDC dc = NULL;

  TEST_CHECK(theme_init(false, &berr));
  TEST_CHECK(theme_dark() == false);
  TEST_CHECK(theme_refresh(&merr) == false);
  TEST_CHECK(theme_class_brush() != NULL);

  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush();
  wc.lpszClassName = L"adc_theme_off_test";
  atom = RegisterClassExW(&wc);
  if (!TEST_CHECK(atom != 0)) {
    goto cleanup;
  }
  owner = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 100, 60, NULL, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(owner != NULL)) {
    goto cleanup;
  }

  button = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 80, 24, owner, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(button != NULL)) {
    goto cleanup;
  }
  button_proc = GetWindowLongPtrW(button, GWLP_WNDPROC);
  theme_attach(button);
  TEST_CHECK(GetWindowLongPtrW(button, GWLP_WNDPROC) == button_proc);
  TEST_MSG("want the button painted by the system, not by a subclass of the theme");

  bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 80, 20, owner, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(bar != NULL)) {
    goto cleanup;
  }
  theme_attach(bar);
  TEST_CHECK(GetWindowTheme(bar) == NULL);
  TEST_MSG("want no theme class of the palette on the bar");
  {
    COLORREF const fill = (COLORREF)SendMessageW(bar, PBM_GETBARCOLOR, 0, 0);
    TEST_CHECK(fill == (COLORREF)CLR_DEFAULT);
    TEST_MSG("want the fill colour of the system, not one of the palette");
  }

  list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 0, 0, 80, 60, owner, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(list != NULL)) {
    goto cleanup;
  }
  {
    LVCOLUMNW column;

    memset(&column, 0, sizeof(column));
    column.mask = LVCF_TEXT;
    column.pszText = L"";
    SendMessageW(list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column);
  }
  {
    LRESULT const hdr = SendMessageW(list, LVM_GETHEADER, 0, 0);

    header = (HWND)(void *)hdr;
  }
  if (TEST_CHECK(header != NULL)) {
    header_proc = GetWindowLongPtrW(header, GWLP_WNDPROC);
  }
  theme_attach_list(list);
  TEST_CHECK(GetWindowTheme(list) == NULL);
  TEST_MSG("want no theme class of the palette on the list");
  if (header != NULL) {
    TEST_CHECK(GetWindowLongPtrW(header, GWLP_WNDPROC) == header_proc);
    TEST_MSG("want the header painted by the system, not by a subclass of the theme");
  }

  dc = GetDC(button);
  if (TEST_CHECK(dc != NULL)) {
    theme_message(button, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)button, &result, &handled, NULL);
    TEST_CHECK(handled == false);
    TEST_MSG("want the colour messages answered by the system, not by the application");
    ReleaseDC(button, dc);
    dc = NULL;
  }

cleanup:
  if (list != NULL) {
    DestroyWindow(list);
  }
  if (bar != NULL) {
    DestroyWindow(bar);
  }
  if (button != NULL) {
    DestroyWindow(button);
  }
  if (owner != NULL) {
    DestroyWindow(owner);
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    atom = 0;
  }
  theme_release();
}

/**
 * @brief The progress window keeps the size and the font of its own layout
 *
 * The window is made once for one run, on whatever monitor the run was started from: the
 * size it is made at, the size a monitor of another dpi gives it, and the font it draws with
 * all come from the layout, not from the window the run belongs to.
 */
static void test_progress_window_keeps_its_size_and_font(void) {
  struct ov_error perr = {0};
  bool cancel = false;
  ATOM atom = 0;
  HFONT borrowed = NULL;
  HWND owner = NULL;
  HWND progress = NULL;
  WNDCLASSEXW wc;

  TEST_CHECK(theme_init(true, &perr));
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush();
  wc.lpszClassName = L"adc_progress_test_owner";
  atom = RegisterClassExW(&wc);
  TEST_CHECK(atom != 0);
  owner = CreateWindowExW(0, wc.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(owner != NULL)) {
    goto cleanup;
  }
  theme_attach(owner);
  borrowed = layout_make_font((UINT)GetDpiForWindow(owner));
  SendMessageW(owner, WM_SETFONT, (WPARAM)borrowed, TRUE);
  if (!TEST_SUCCEEDED(ui_progress_window_create(
                          owner, (int)GetDpiForWindow(owner), ui_progress_window_steps(false, 1), &cancel, NULL, NULL, &progress, &perr),
                      &perr)) {
    goto cleanup;
  }
  ShowWindow(progress, SW_SHOW);
  /* the owner moves to a monitor of its own dpi and lets go of its font */
  DeleteObject(borrowed);
  borrowed = NULL;

  {
    int const least = layout_scale(ADC_LAYOUT_PROGRESS_CANCEL_W, 96) + 2 * layout_scale(ADC_LAYOUT_MARGIN_X, 96);
    RECT outer;

    GetWindowRect(progress, &outer);
    TEST_CHECK_(least <= (outer.right - outer.left), "want the window at least %d wide, it is %d", least, (int)(outer.right - outer.left));
  }

  TEST_CASE("the window is the size its layout asks for");
  {
    int const dpi = (int)GetDpiForWindow(progress);
    int const want_w = layout_scale(ADC_LAYOUT_PROGRESS_W, (UINT)dpi);
    struct progress_layout g;
    RECT client;

    layout_progress_compute(want_w, (UINT)dpi, &g);
    GetClientRect(progress, &client);
    TEST_CHECK_((client.right - client.left) == want_w, "want the client %d wide, got %d", want_w, (int)(client.right - client.left));
    TEST_CHECK_((client.bottom - client.top) == g.needed_height,
                "want the client %d tall, got %d",
                (int)g.needed_height,
                (int)(client.bottom - client.top));
  }
  TEST_CASE_(NULL);

  TEST_CASE("the rectangle a monitor suggests moves the window, it does not size it");
  {
    int const dpi = (int)GetDpiForWindow(progress);
    int const want_w = layout_scale(ADC_LAYOUT_PROGRESS_W, (UINT)dpi);
    struct progress_layout g;
    RECT client;
    RECT outer;
    RECT suggested;

    layout_progress_compute(want_w, (UINT)dpi, &g);
    /* the size of this suggestion is not the size the layout asks for */
    SetRect(&suggested,
            ADC_TEST_PROGRESS_SUGGEST_LEFT,
            ADC_TEST_PROGRESS_SUGGEST_TOP,
            ADC_TEST_PROGRESS_SUGGEST_RIGHT,
            ADC_TEST_PROGRESS_SUGGEST_BOTTOM);
    SendMessageW(progress, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&suggested);
    GetWindowRect(progress, &outer);
    GetClientRect(progress, &client);
    TEST_CHECK_(outer.left == suggested.left, "want the window moved to %d, it is at %d", (int)suggested.left, (int)outer.left);
    TEST_CHECK_(outer.top == suggested.top, "want the window moved to %d, it is at %d", (int)suggested.top, (int)outer.top);
    TEST_CHECK_((client.right - client.left) == want_w, "want the client %d wide, got %d", want_w, (int)(client.right - client.left));
    TEST_CHECK_((client.bottom - client.top) == g.needed_height,
                "want the client %d tall, got %d",
                (int)g.needed_height,
                (int)(client.bottom - client.top));
  }
  TEST_CASE_(NULL);

  TEST_CASE("the window makes room for the line a run may report, instead of hiding it");
  {
    static char const long_line[] = "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM (7/7)";
    int const dpi = (int)GetDpiForWindow(progress);
    int const least = layout_scale(ADC_LAYOUT_PROGRESS_W, (UINT)dpi);
    HWND wide = NULL;
    RECT client;

    if (!TEST_SUCCEEDED(
            ui_progress_window_create(owner, dpi, ui_progress_window_steps(false, 1), &cancel, long_line, long_line, &wide, &perr),
            &perr)) {
      goto cleanup;
    }
    {
      wchar_t shown[256];
      HWND status = FindWindowExW(wide, NULL, L"STATIC", NULL);

      GetClientRect(wide, &client);
      TEST_CHECK_((client.right - client.left) > least,
                  "want the window wider than its least %d, it is %d",
                  least,
                  (int)(client.right - client.left));
      if (TEST_CHECK(status != NULL)) {
        memset(shown, 0, sizeof(shown));
        OV_SNPRINTF(shown, sizeof(shown) / sizeof(WCHAR), L"%1$s", L"%1$s", long_line);
        SetWindowTextW(status, shown);
        TEST_CHECK(text_fits(status));
        TEST_MSG("want the line of the run shown whole in the window");
        TEST_CHECK_((GetWindowLongPtrW(status, GWL_STYLE) & SS_ENDELLIPSIS) == 0, "want room for the line, not an ellipsis that hides it");
      }
      ui_progress_window_destroy(&wide);
      wide = NULL;
    }
  }
  TEST_CASE_(NULL);

  TEST_CASE("the window draws with fonts of its own, alive after the owner let go of its font");
  {
    int const dpi = (int)GetDpiForWindow(progress);
    HWND label = FindWindowExW(progress, NULL, L"STATIC", NULL);
    HWND button = FindWindowExW(progress, NULL, L"Button", NULL);

    if (TEST_CHECK(label != NULL)) {
      TEST_CHECK(font_of_dpi(label, dpi));
      TEST_MSG("want the status line drawn with a font for dpi %d", dpi);
    }
    if (TEST_CHECK(button != NULL)) {
      TEST_CHECK(font_of_dpi(button, dpi));
      TEST_MSG("want the button drawn with a font for dpi %d", dpi);
    }
  }
  TEST_CASE_(NULL);

cleanup:
  if (progress != NULL) {
    ui_progress_window_destroy(&progress);
    progress = NULL;
  }
  if (borrowed != NULL) {
    DeleteObject(borrowed);
    borrowed = NULL;
  }
  if (owner != NULL) {
    DestroyWindow(owner);
    owner = NULL;
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    atom = 0;
  }
  theme_release();
}

/**
 * @brief A second start reaches the window of the copy that is already running
 *
 * The call is what a person who pressed the icon twice gets instead of a second window: the
 * window that is there, in front.  Without such a window the call says that nobody runs.
 */
static void test_activate_reaches_the_window_of_the_running_copy(void) {
  char service_name[] = "adc-ui-test-no-such-service";
  struct settings settings;
  struct options options;
  struct adc_ui_result result;
  struct adc_ui_thread job;
  thrd_t thread = {0};
  struct ov_error err = {0};
  bool started = false;
  HWND top = NULL;
  DWORD const start = GetTickCount();

  memset(&settings, 0, sizeof(settings));
  memset(&result, 0, sizeof(result));
  options_default(&options);
  settings.ks_service = service_name;
  settings.use_theme = true;

  TEST_CASE("no copy is running yet");
  if (find_own_window(UI_MAIN_WINDOW_CLASS) != NULL) {
    TEST_SKIP("a window of this program is on the screen already");
    goto cleanup;
  }
  TEST_FAILED_WITH(ui_activate_running_window(&err), &err, ov_error_type_generic, ov_error_generic_not_found);

  TEST_CASE("the window of the running copy is reached");
  job.options = &options;
  job.settings = &settings;
  job.out = &result;
  cndvar_init(&job.done);
  if (!TEST_CHECK(start_ui(&thread, &job))) {
    goto cleanup;
  }
  started = true;
  while ((GetTickCount() - start) < ADC_UI_TEST_TIMEOUT_MS) {
    top = find_own_window(UI_MAIN_WINDOW_CLASS);
    if (top != NULL) {
      break;
    }
    Sleep(ADC_UI_TEST_POLL_MS);
  }
  if (!TEST_CHECK(top != NULL)) {
    result.timed_out = true;
    goto cleanup;
  }
  SendMessageW(top, WM_NULL, 0, 0);
  if (!TEST_SUCCEEDED(ui_activate_running_window(&err), &err)) {
    goto cleanup;
  }

  PostMessageW(top, WM_CLOSE, 0, 0);
  started = false;
  TEST_CHECK(join_ui(&thread, &job.done));
  TEST_CASE_(NULL);

cleanup:
  if (started) {
    TEST_CHECK(join_ui(&thread, &job.done));
    started = false;
  }
  cndvar_exit(&job.done);
}

TEST_LIST = {
    {"every_control_carries_the_font", test_every_control_carries_the_font_of_the_window},
    {"window_without_the_theme_is_traditional", test_window_without_the_theme_is_traditional},
    {"window_tells_that_it_waits", test_window_tells_that_it_waits},
    {"blockers_window_shows_the_processes", test_blockers_window_shows_the_processes},
    {"progress_window_keeps_its_size_and_font", test_progress_window_keeps_its_size_and_font},
    {"progress_window_greys_the_asks", test_progress_window_greys_the_asks_and_stays},
    {"progress_bar_follows_the_palette", test_progress_bar_follows_the_palette},
    {"theme_change_reaches_every_window", test_theme_change_reaches_every_window},
    {"theme_switch_detaches_the_theme", test_theme_switch_detaches_the_theme},
    {"device_picker_label", test_device_picker_label},
    {"picker_window_names_every_unit", test_picker_window_names_every_unit},
    {"auto_close_follows_the_run", test_auto_close_only_for_a_run_nobody_watches},
    {"result_dialog_follows_the_window", test_result_dialog_only_for_a_window_that_stays},
    {"auto_run_acts_on_the_state", test_auto_run_acts_on_the_state_it_is_for},
    {"auto_run_says_the_state", test_auto_run_says_the_state_it_found},
    {"press_answers_the_devices_it_finds", test_press_answers_the_devices_it_finds},
    {"press_with_nothing_connected", test_press_with_nothing_connected},
    {"activate_reaches_the_running_copy", test_activate_reaches_the_window_of_the_running_copy},
    {"result_headline", test_result_headline},
    {"result_dialog_lines_differ", test_result_dialog_lines_differ},
    {NULL, NULL},
};
