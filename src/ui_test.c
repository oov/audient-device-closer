/**
 * Tests for the window of the application.
 *
 * These run against a real window: the defect they exist for was one control that never
 * received the font of the window, which only a real window shows.  They are the reason a
 * developer can start the debug build without an elevation (see app.debug.manifest): a
 * harness that had to ask for the administrator on every run would not be run often, and
 * a check nobody runs is not a check.
 */
#include <windows.h>

#include "test_suppress.h"

/**
 * @brief Put the hooks in place that keep the windows of this test out of the reader's way
 *
 * ovtest.h calls this before anything of the test runs, which is early enough for the hook
 * to catch the first window.
 */
static void adc_test_init(void) { test_suppress_install_quiet(); }

#define TEST_MY_INIT adc_test_init()
#define TEST_MY_FINI test_suppress_release()

#include "ui.c"

#include <ovtest.h>

#include <string.h>

#include <commctrl.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>
#include <ovthreads.h>

#include "device_probe.h"
#include "layout.h"
#include "options.h"
#include "removal_manager.h"
#include "repair.h"
#include "settings.h"
#include "ui.h"
#include <uxtheme.h>

enum {
  ADC_UI_TEST_TIMEOUT_MS = 20000,
};

struct adc_ui_result {
  bool opened;
  struct ov_error err;
  size_t fontless;
  HWND fontless_first;
  wchar_t fontless_class[32];
};

struct adc_ui_thread {
  struct options const *options;
  struct settings const *settings;
  struct adc_ui_result *out;
  HANDLE ready; //!< set by the thread once the window of the run is up
  HWND window;  //!< the window of the run, NULL until it is up and after it is gone
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
 * @brief A class name that belongs to this run of the test alone
 *
 * @param out receives the class name
 * @param out_size room of out, in characters
 * @param err receives the failure of the formatting
 * @return false when the name does not fit
 */
static NODISCARD bool test_window_class_name(wchar_t *const out, size_t const out_size, struct ov_error *const err) {
  int const written = ov_snprintf_wchar(out,
                                        out_size,
                                        L"adc-ui-test-%1$lu-%2$lu",
                                        L"adc-ui-test-%1$lu-%2$lu",
                                        (unsigned long)GetCurrentProcessId(),
                                        (unsigned long)GetTickCount());
  if (written < 0 || (size_t)written >= out_size) {
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "the class name could not be made (ov_snprintf_wchar returned %1$d)",
                  "the class name could not be made (ov_snprintf_wchar returned %1$d)",
                  written);
    return false;
  }
  return true;
}

/**
 * @brief A window of the test, of a class of its own
 */
struct test_window {
  wchar_t class_name[64];
  ATOM atom;
  HWND hwnd;
};

/**
 * @brief Take the window of the test down
 *
 * @param w the window to close, safe to call on one that is not open
 */
static void test_window_close(struct test_window *const w) {
  if (w->hwnd != NULL) {
    DestroyWindow(w->hwnd);
    w->hwnd = NULL;
  }
  if (w->atom != 0) {
    UnregisterClassW(w->class_name, GetModuleHandleW(NULL));
    w->atom = 0;
  }
}

/**
 * @brief Make the window of the test under the class name
 *
 * @param w receives the window, opened on success
 * @param class_name the class to register and make the window under
 * @param err receives the failure of the class or the window
 * @return false when the class or the window could not be made
 */
static NODISCARD bool test_window_open(struct test_window *const w, wchar_t const *const class_name, struct ov_error *const err) {
  bool success = false;
  WNDCLASSEXW wc;

  memset(w, 0, sizeof(*w));
  lstrcpynW(w->class_name, class_name, (int)(sizeof(w->class_name) / sizeof(w->class_name[0])));
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.lpszClassName = w->class_name;
  w->atom = RegisterClassExW(&wc);
  if (w->atom == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  w->hwnd = CreateWindowExW(0, w->class_name, L"adc ui test", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, NULL, NULL, wc.hInstance, NULL);
  if (w->hwnd == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  ShowWindow(w->hwnd, SW_SHOW);
  UpdateWindow(w->hwnd);
  success = true;

cleanup:
  if (!success) {
    test_window_close(w);
  }
  return success;
}

/**
 * @brief The thread the window runs on
 *
 * @param param the job to run
 * @return 0 when the window is closed
 */
static int run_ui(void *const param) {
  struct adc_ui_thread *const job = (struct adc_ui_thread *)(void *)param;
  struct ui_state *st = NULL;

  // the hook of the test is for the thread that installed it, and this window is made on
  // this thread, so the hook has to be put in place here as well
  test_suppress_install_quiet();
  job->window = NULL;
  job->out->opened = false;
  st = ui_main_window_create(job->options, job->settings, &job->out->err);
  if (st != NULL) {
    MSG msg;

    job->window = st->window;
    SetEvent(job->ready);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
      if (!IsDialogMessageW(st->window, &msg)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
    TEST_SUCCEEDED(ui_main_window_destroy(&st, &job->out->err), &job->out->err);
    job->window = NULL;
    job->out->opened = true;
  } else {
    SetEvent(job->ready); // the test asks for the window and finds that there is none
  }
  test_suppress_release();
  cndvar_broadcast(&job->done, 1);
  return 0;
}

/**
 * @brief Wait until the window of the run is up
 *
 * The window is made on the thread of the run, so that thread tells the test when it is
 * there instead of the test looking over the desktop for it.  A window that does not come
 * up in time is a failure of the run, and the test reports what the run said about it.
 *
 * @param job the run to wait for
 * @return false when the window did not come up in time
 */
static bool wait_until_ready(struct adc_ui_thread *const job) {
  DWORD const waited = WaitForSingleObject(job->ready, ADC_UI_TEST_TIMEOUT_MS);

  if (job->ready != NULL) {
    CloseHandle(job->ready);
    job->ready = NULL;
  }
  return waited == WAIT_OBJECT_0;
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
  job->window = NULL;
  job->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (job->ready == NULL) {
    return false;
  }
  cndvar_lock(&job->done);
  cndvar_signal(&job->done, 0);
  cndvar_unlock(&job->done);
  if (thrd_create(thread, run_ui, job) != thrd_success) {
    CloseHandle(job->ready);
    job->ready = NULL;
    return false;
  }
  return true;
}

/**
 * @brief Every control of the window carries the font of the window
 *
 * Runs the window, waits until it is up, then checks every control for a font and closes
 * it.
 */
static void test_every_control_carries_the_font_of_the_window(void) {
  wchar_t service_name[] = L"adc-ui-test-no-such-service";
  struct settings settings;
  struct options options;
  struct adc_ui_result result;
  struct adc_ui_result second;
  struct adc_ui_thread job;
  thrd_t thread = {0};
  bool started = false;
  HWND top = NULL;

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
  if (!TEST_CHECK(wait_until_ready(&job))) {
    goto cleanup;
  }
  top = job.window;
  if (!TEST_CHECK(top != NULL)) {
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
    int const least_w = layout_scale(ADC_MAIN_WINDOW_W, dpi);
    int const want_h = layout_scale(ADC_MAIN_WINDOW_H, dpi);
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
    job.out = &second;
    if (!TEST_CHECK(start_ui(&thread, &job))) {
      goto cleanup;
    }
    started = true;
    top = NULL;
    if (TEST_CHECK(wait_until_ready(&job))) {
      top = job.window;
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
  wchar_t service_name[] = L"adc-ui-test-no-such-service";
  struct settings settings;
  struct options options;
  struct adc_ui_result result;
  struct adc_ui_thread job;
  thrd_t thread = {0};
  bool started = false;
  HWND top = NULL;

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
  if (!TEST_CHECK(wait_until_ready(&job))) {
    goto cleanup;
  }
  top = job.window;
  if (!TEST_CHECK(top != NULL)) {
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
    HBRUSH const brush = (HBRUSH)brush_value;
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
  wchar_t service_name[] = L"adc-ui-test-no-such-service";
  struct settings settings;
  struct options options;
  struct adc_ui_result result;
  struct adc_ui_thread job;
  thrd_t thread = {0};
  bool started = false;
  HWND top = NULL;

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
  if (!TEST_CHECK(wait_until_ready(&job))) {
    goto cleanup;
  }
  top = job.window;
  if (!TEST_CHECK(top != NULL)) {
    goto cleanup;
  }
  SendMessageW(top, WM_NULL, 0, 0);

  TEST_CASE("the window is held while the run is going");
  {
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

  started = false;
  TEST_CHECK(join_ui(&thread, &job.done));
  TEST_SUCCEEDED(result.opened, &result.err);

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
  ui_result_dialog_text(REPAIR_OUTCOME_CLOSED, app_error_none, repair_failure_none, NULL, headline, sizeof(headline), body, sizeof(body));
  TEST_CHECK(strcmp(headline, "Audient Device Closer") != 0);
  TEST_CHECK(strstr(headline, "could not") == NULL);
  TEST_CHECK(body[0] != '\0');

  TEST_CASE("a device that is not there");
  ui_result_dialog_text(REPAIR_OUTCOME_ABSENT, app_error_none, repair_failure_none, NULL, headline, sizeof(headline), body, sizeof(body));
  TEST_CHECK(strcmp(headline, "Audient Device Closer") != 0);
  TEST_CHECK(strstr(body, "No Audient device was found") != NULL);
  TEST_CHECK(strstr(body, "try again") == NULL);

  TEST_CASE("a device with a problem the removal does not cure");
  ui_result_dialog_text(
      REPAIR_OUTCOME_OTHER_PROBLEM, app_error_none, repair_failure_none, NULL, headline, sizeof(headline), body, sizeof(body));
  TEST_CHECK(strcmp(headline, "Audient Device Closer") != 0);

  TEST_CASE("a removal the program was not allowed to ask for");
  ui_result_dialog_text(
      REPAIR_OUTCOME_FAILED, app_error_removal_access_denied, repair_failure_none, NULL, headline, sizeof(headline), body, sizeof(body));
  TEST_CHECK(strcmp(headline, "Failed.") == 0);
  TEST_MSG("want the verdict in the headline of a failed run");
  TEST_CHECK(strstr(body, "administrator") != NULL);
  TEST_MSG("want the reason the program worked out, in the body");
  TEST_CHECK(strstr(body, "could not be released") == NULL);
  TEST_MSG("want no sentence of the log in the dialog");

  TEST_CASE("the headline and the body are different sentences");
  TEST_CHECK(strcmp(headline, body) != 0);

  TEST_CASE("a process that could not be closed is named");
  ui_result_dialog_text(
      REPAIR_OUTCOME_FAILED, app_error_none, repair_failure_process_close, L"iD.exe", headline, sizeof(headline), body, sizeof(body));
  TEST_CHECK(strstr(body, "iD.exe") != NULL);
  TEST_MSG("want the process that could not be closed named");
  TEST_CHECK(strstr(body, "could not be closed") != NULL);

  TEST_CASE("the service that could not come back is named");
  ui_result_dialog_text(
      REPAIR_OUTCOME_FAILED, app_error_none, repair_failure_audio_service_start, NULL, headline, sizeof(headline), body, sizeof(body));
  TEST_CHECK(strstr(body, "Windows Audio") != NULL);
  TEST_MSG("want the service that would not start named");

  TEST_CASE("the devices that could not be read at all are answered");
  ui_result_dialog_text(
      REPAIR_OUTCOME_FAILED, app_error_none, repair_failure_device_state, NULL, headline, sizeof(headline), body, sizeof(body));
  TEST_CHECK(strcmp(headline, "Failed.") == 0);
  TEST_MSG("want the verdict in the headline of a run that could not look");
  TEST_CHECK(strstr(body, "could not be determined") != NULL);
  TEST_MSG("want the reader told that the state was never read");

  TEST_CASE("a veto names no process here");
  ui_result_dialog_text(REPAIR_OUTCOME_VETOED, app_error_none, repair_failure_none, NULL, headline, sizeof(headline), body, sizeof(body));
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
  static enum app_error_code const removal_errors[] = {app_error_none,
                                                       app_error_removal_access_denied,
                                                       app_error_removal_device_gone,
                                                       app_error_removal_call_failed,
                                                       app_error_removal_veto,
                                                       app_error_removal_refused};
  size_t i = 0;
  size_t k = 0;

  for (i = 0; i < (sizeof(outcomes) / sizeof(outcomes[0])); i++) {
    for (k = 0; k < (sizeof(removal_errors) / sizeof(removal_errors[0])); k++) {
      char headline[256];
      char body[512];

      TEST_CASE_("outcome %d with removal error %d", (int)outcomes[i], (int)removal_errors[k]);
      ui_result_dialog_text(outcomes[i], removal_errors[k], repair_failure_none, NULL, headline, sizeof(headline), body, sizeof(body));
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
  TEST_CHECK(ui_auto_run_action(true, DEVICE_STATE_NO_INTERFACE) == UI_AUTO_RUN_LEAVE);

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
  TEST_CHECK(strcmp(ui_auto_run_sentence(DEVICE_STATE_NO_INTERFACE), "The device state could not be determined.") == 0);
  TEST_CHECK(ui_auto_run_sentence(DEVICE_STATE_FAULTED) == NULL);
}

/**
 * @brief A list of targets with the states named, the way probe_all hands them out
 */
static struct device_target *make_press_targets(enum device_state const *const states, size_t const count) {
  wchar_t const *const devnodes[] = {
      L"TUSBAUDIO_ENUM\\VID_2708&PID_0001&KS\\6&deadbee&0&2",
      L"TUSBAUDIO_ENUM\\VID_2708&PID_0002&KS\\6&deadbee&0&2",
      L"TUSBAUDIO_ENUM\\VID_2708&PID_0003&KS\\6&deadbee&0&2",
  };
  struct device_target *targets = NULL;

  for (size_t i = 0; i < count; i++) {
    struct device_target t = {0};
    size_t const chars = wcslen(devnodes[i]) + 1;

    if (!OV_ARRAY_GROW(&t.devnode, chars)) {
      return NULL;
    }
    OV_ARRAY_SET_LENGTH(t.devnode, chars - 1);
    wcscpy(t.devnode, devnodes[i]);
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
    enum device_state const states[] = {DEVICE_STATE_NO_INTERFACE};
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
 * @brief A start reaches the window of the copy that is already running
 *
 * The call is what a person who pressed the icon twice gets instead of a second window: the
 * window that is there, in front.  Without such a window the call says that nobody runs.
 * The window is the one of this test, under a class of its own, so the answer does not
 * depend on whether the program happens to be running beside the harness.
 */
static void test_activate_reaches_the_window_of_the_running_copy(void) {
  struct test_window window;
  wchar_t class_name[64];
  struct ov_error err = {0};

  if (!TEST_SUCCEEDED(test_window_class_name(class_name, sizeof(class_name) / sizeof(class_name[0]), &err), &err)) {
    goto cleanup;
  }

  TEST_CASE("no copy is running yet");
  TEST_FAILED_WITH(activate_window_of_class(class_name, &err), &err, ov_error_type_generic, ov_error_generic_not_found);

  TEST_CASE("the window of the running copy is reached");
  if (!TEST_SUCCEEDED(test_window_open(&window, class_name, &err), &err)) {
    goto cleanup;
  }
  if (!TEST_SUCCEEDED(activate_window_of_class(class_name, &err), &err)) {
    goto cleanup;
  }

cleanup:
  test_window_close(&window);
}

/** @brief The dpis the checks of the geometry walk */
static UINT const geometry_dpi_list[] = {96, 120, 144, 192};
#define GEOMETRY_DPI_COUNT ((int)(sizeof(geometry_dpi_list) / sizeof(geometry_dpi_list[0])))

/** @brief The pixels the scaling of one part and the scaling of a whole can differ by */
static int const geometry_slack = 1;

/**
 * @brief The captions the geometry checks hand to the window
 *
 * These are the widest captions the application has, measured at 96dpi by the tests that
 * check the boxes, so the geometry is checked with room to spare.
 */
static int const settings_caption_logical = 90;
static int const install_caption_logical = 60;

/**
 * @brief The size of a difference, unsigned
 *
 * @param v the difference of two values
 * @return the size it stands for
 */
static int geometry_abs(int const v) { return (v < 0) ? -v : v; }

/**
 * @brief The client area of the default window
 *
 * @param dpi the dpi to draw for
 * @param extra_width how much wider than the default the window is
 * @param extra_height how much taller than the default the window is
 * @return the client rectangle of the window
 */
static RECT client_of(UINT const dpi, int const extra_width, int const extra_height) {
  RECT r;
  r.left = 0;
  r.top = 0;
  r.right = layout_scale(ADC_MAIN_WINDOW_W, dpi) + extra_width;
  r.bottom = layout_scale(ADC_MAIN_WINDOW_H, dpi) + extra_height;
  return r;
}

/**
 * @brief The geometry of the main window these checks walk
 *
 * @param dpi the dpi to draw for
 * @param extra_width how much wider than the default the window is
 * @param extra_height how much taller than the default the window is
 * @param caption_width the room the group captions take
 * @return the geometry to check
 */
static struct main_layout compute(UINT const dpi, int const extra_width, int const extra_height, int const caption_width) {
  struct main_layout g;
  RECT const c = client_of(dpi, extra_width, extra_height);
  int const settings_w = layout_scale(ADC_MAIN_SETTINGS_MIN_W, dpi);
  main_compute(&c, dpi, settings_w, caption_width, layout_scale(install_caption_logical, dpi), layout_scale(ADC_LAYOUT_CAPTION_H, dpi), &g);
  return g;
}

/**
 * @brief The width of the smallest client area
 *
 * It follows the width of the settings group.
 *
 * @param dpi the dpi to draw for
 * @return the narrowest client area that shows both groups
 */
static int min_client_width(UINT const dpi) {
  return layout_scale(ADC_MAIN_MIN_CLIENT_EXTRA_W, dpi) + layout_scale(ADC_MAIN_SETTINGS_MIN_W, dpi);
}

/**
 * @brief Every frame of the window, for the tests that walk them all
 *
 * @param g the geometry to read
 * @param out receives the rectangles
 * @param n receives how many there are
 */
static void frames_of(struct main_layout const *const g, RECT const **const out, size_t *const n) {
  out[(*n)++] = &g->settings_frame;
  out[(*n)++] = &g->install_frame;
}

/**
 * @brief Every caption of the window, for the tests that walk them all
 *
 * @param g the geometry to read
 * @param out receives the rectangles
 * @param n receives how many there are
 */
static void captions_of(struct main_layout const *const g, RECT const **const out, size_t *const n) {
  out[(*n)++] = &g->settings_caption;
  out[(*n)++] = &g->install_caption;
}

/**
 * @brief The top line of a frame runs through the middle of its caption
 *
 * A group box of the system puts the text at the top of its area and draws the top line at
 * half the height of that text, so the line balances the caption.  The frames here follow
 * the same convention: the caption is asked for its height, and the line goes through its
 * middle, at every dpi and for an even and an odd height alike.  Both groups are checked:
 * they share the code that places a caption on a frame.
 */
static void test_caption_line_balance(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    UINT const dpi = geometry_dpi_list[i];
    int const reserved = layout_scale(ADC_LAYOUT_CAPTION_H, dpi);
    int const heights[2] = {reserved, reserved - 1};
    int k = 0;
    for (k = 0; k < 2; k++) {
      RECT const c = client_of(dpi, 0, 0);
      struct main_layout g;
      int const caption_h = heights[k];
      int const group_h = layout_scale(ADC_MAIN_GROUP_H, dpi);

      main_compute(&c,
                   dpi,
                   layout_scale(ADC_MAIN_SETTINGS_MIN_W, dpi),
                   layout_scale(settings_caption_logical, dpi),
                   layout_scale(install_caption_logical, dpi),
                   caption_h,
                   &g);
      TEST_CHECK_(g.settings_caption.bottom - g.settings_caption.top == caption_h,
                  "dpi %u height %d: the caption is %d tall",
                  (unsigned)dpi,
                  caption_h,
                  (int)(g.settings_caption.bottom - g.settings_caption.top));
      TEST_CHECK_(g.settings_frame.top - g.settings_caption.top == caption_h / 2,
                  "dpi %u height %d: the line sits %d under the caption top",
                  (unsigned)dpi,
                  caption_h,
                  (int)(g.settings_frame.top - g.settings_caption.top));
      TEST_CHECK_((g.settings_caption.top + g.settings_caption.bottom) / 2 == g.settings_frame.top,
                  "dpi %u height %d: the line misses the middle of the caption",
                  (unsigned)dpi,
                  caption_h);
      TEST_CHECK_(g.settings_frame.bottom - g.settings_frame.top == group_h,
                  "dpi %u height %d: the frame is %d tall",
                  (unsigned)dpi,
                  caption_h,
                  (int)(g.settings_frame.bottom - g.settings_frame.top));
      TEST_CHECK_(g.install_frame.top - g.install_caption.top == caption_h / 2,
                  "dpi %u height %d: the second group is not balanced the same way",
                  (unsigned)dpi,
                  caption_h);
    }
  }
}

/**
 * @brief The frame line must run through the middle of the caption
 *
 * A fixed offset only holds at one dpi, which is exactly the defect this replaces.  Both
 * groups are checked: they share the code that places a caption on a frame.
 */
static void test_caption_rides_the_frame(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    UINT const dpi = geometry_dpi_list[i];
    struct main_layout const g = compute(dpi, 0, 0, layout_scale(settings_caption_logical, dpi));
    RECT const *frames[2];
    RECT const *captions[2];
    size_t nf = 0;
    size_t nc = 0;
    size_t k = 0;
    frames_of(&g, frames, &nf);
    captions_of(&g, captions, &nc);
    TEST_CHECK(nf == nc);
    for (k = 0; k < nf; k++) {
      int const caption_mid = (int)((captions[k]->top + captions[k]->bottom) / 2);
      int const frame_top = (int)frames[k]->top;
      int const tolerance = (dpi >= 144) ? 2 : 1;
      TEST_CHECK_(geometry_abs(caption_mid - frame_top) <= tolerance,
                  "dpi %u group %u: caption middle %d, frame top %d",
                  (unsigned)dpi,
                  (unsigned)k,
                  caption_mid,
                  frame_top);
    }
  }
}

/**
 * @brief The two groups of the band line up
 *
 * The same top, the same bottom, and the settings group on the left of the install group.
 */
static void test_groups_share_the_band(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    UINT const dpi = geometry_dpi_list[i];
    struct main_layout const g = compute(dpi, 0, 0, layout_scale(settings_caption_logical, dpi));
    TEST_CHECK_(g.settings_frame.top == g.install_frame.top,
                "dpi %u: the frames start on different lines, %d and %d",
                (unsigned)dpi,
                (int)g.settings_frame.top,
                (int)g.install_frame.top);
    TEST_CHECK(g.settings_frame.bottom == g.install_frame.bottom);
    TEST_CHECK(g.settings_frame.right <= g.install_frame.left);
    TEST_CHECK(g.settings_frame.right < g.install_frame.left);
  }
}

/**
 * @brief The task buttons of the install group are one above the other
 *
 * Not side by side, and the group is tall enough to hold both with the same inset as the
 * check boxes have.
 */
static void test_task_buttons_stack_in_group(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    UINT const dpi = geometry_dpi_list[i];
    struct main_layout const g = compute(dpi, 0, 0, layout_scale(settings_caption_logical, dpi));
    int const inset = layout_scale(ADC_MAIN_GROUP_INSET, dpi);
    TEST_CHECK_(g.task_remove.top > g.task_install.bottom, "dpi %u: the task buttons overlap vertically", (unsigned)dpi);
    TEST_CHECK_(g.task_install.left == g.task_remove.left, "dpi %u: the task buttons are not aligned", (unsigned)dpi);
    TEST_CHECK(g.task_install.left - g.install_frame.left == inset);
    TEST_CHECK(g.install_frame.right - g.task_install.right == inset);
    TEST_CHECK(g.task_remove.bottom <= g.install_frame.bottom - inset + 1);
    TEST_CHECK_(
        g.install_frame.bottom - g.task_remove.bottom >= inset - 1, "dpi %u: the last task button has no room below it", (unsigned)dpi);
  }
}

/**
 * @brief The task buttons stay inside the frame of their group
 *
 * The minimum width was taken from the client size while ptMinTrackSize is the size of the
 * whole window, so the last button stuck out of the frame.
 */
static void test_buttons_inside_the_frame(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    UINT const dpi = geometry_dpi_list[i];
    RECT c;
    struct main_layout g;
    SetRect(&c, 0, 0, min_client_width(dpi), layout_scale(ADC_MAIN_WINDOW_H, dpi));
    main_compute(&c,
                 dpi,
                 layout_scale(ADC_MAIN_SETTINGS_MIN_W, dpi),
                 layout_scale(settings_caption_logical, dpi),
                 layout_scale(install_caption_logical, dpi),
                 layout_scale(ADC_LAYOUT_CAPTION_H, dpi),
                 &g);
    TEST_CHECK_(g.task_remove.right <= g.install_frame.right,
                "dpi %u: button right %d exceeds frame right %d",
                (unsigned)dpi,
                (int)g.task_remove.right,
                (int)g.install_frame.right);
    TEST_CHECK(g.task_install.left >= g.install_frame.left);
    TEST_CHECK(g.task_remove.right <= c.right);
    TEST_CHECK(g.install_frame.right <= c.right);
  }
}

/**
 * @brief Everything scales with the dpi, nothing is measured twice
 *
 * Everything scaled twice (the width minus a logical margin, then scaled again) shows up
 * here: the button of a 144dpi window must be 1.5 times as wide as the one of a 96dpi
 * window.
 */
static void test_growth_is_proportional(void) {
  struct main_layout const small = compute(96, 0, 0, 120);
  struct main_layout const large = compute(144, 0, 0, 180);
  TEST_CHECK_(
      geometry_abs((small.task_remove.right - small.task_remove.left) * 144 / 96 - (large.task_remove.right - large.task_remove.left)) <= 2,
      "task button does not scale");
  TEST_CHECK_(geometry_abs((small.settings_frame.right - small.settings_frame.left) * 144 / 96 -
                           (large.settings_frame.right - large.settings_frame.left)) <= 2,
              "the settings group does not scale");
}

/**
 * @brief The minimum width covers what the group needs
 *
 * The minimum width is a separate number from the group geometry.  If somebody shrinks it
 * below what the group needs, the buttons overflow the frame again, and that happens
 * outside main_compute(), so it has to be checked against the constants directly.
 */
static void test_minimum_covers_the_group(void) {
  static int const width_parts[] = {
      ADC_LAYOUT_MARGIN_X, ADC_LAYOUT_MARGIN_X, ADC_MAIN_SETTINGS_MIN_W, ADC_MAIN_GROUP_GAP, ADC_MAIN_GROUP_INSET, ADC_MAIN_GROUP_INSET};
  static int const box_parts[] = {ADC_MAIN_GROUP_INSET, ADC_MAIN_CHECK_H, ADC_LAYOUT_GAP, ADC_MAIN_CHECK_H, ADC_MAIN_GROUP_INSET};
  static int const task_parts[] = {ADC_MAIN_GROUP_INSET, ADC_MAIN_TASK_H, ADC_MAIN_TASK_GAP, ADC_MAIN_TASK_H, ADC_MAIN_GROUP_INSET};
  size_t const boxes_in_the_catalog = 2;
  int const room_of_the_catalog = ADC_MAIN_GROUP_INSET + (int)boxes_in_the_catalog * ADC_MAIN_CHECK_H +
                                  (int)(boxes_in_the_catalog - 1) * ADC_LAYOUT_GAP + ADC_MAIN_GROUP_INSET;
  int need = 0;
  int boxes = 0;
  int tasks = 0;
  size_t i = 0;
  for (i = 0; i < (sizeof(width_parts) / sizeof(width_parts[0])); i++) {
    need += width_parts[i];
  }
  for (i = 0; i < (sizeof(box_parts) / sizeof(box_parts[0])); i++) {
    boxes += box_parts[i];
  }
  for (i = 0; i < (sizeof(task_parts) / sizeof(task_parts[0])); i++) {
    tasks += task_parts[i];
  }
  TEST_CHECK_((int)min_client_width(96) >= need, "minimum client width %d, the band needs %d", (int)min_client_width(96), need);
  TEST_CHECK_(boxes == room_of_the_catalog,
              "the layout gives the check boxes %d, the %u captions of the catalogue need %d",
              boxes,
              (unsigned)boxes_in_the_catalog,
              room_of_the_catalog);
  {
    int const taller = (boxes > tasks) ? boxes : tasks;
    TEST_CHECK_((int)ADC_MAIN_GROUP_H == taller, "the group height %d, the taller group needs %d", (int)ADC_MAIN_GROUP_H, taller);
  }
}

/**
 * @brief Nothing may leave the client area, not even at the smallest allowed window
 */
static void test_inside_the_client(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    RECT c;
    struct main_layout g;
    SetRect(&c, 0, 0, min_client_width(geometry_dpi_list[i]), layout_scale(ADC_MAIN_WINDOW_H, geometry_dpi_list[i]));
    main_compute(&c,
                 geometry_dpi_list[i],
                 layout_scale(ADC_MAIN_SETTINGS_MIN_W, geometry_dpi_list[i]),
                 layout_scale(settings_caption_logical, geometry_dpi_list[i]),
                 layout_scale(install_caption_logical, geometry_dpi_list[i]),
                 layout_scale(ADC_LAYOUT_CAPTION_H, geometry_dpi_list[i]),
                 &g);
    TEST_CHECK(g.install_frame.bottom <= c.bottom);
    TEST_CHECK(g.settings_frame.bottom <= c.bottom);
    TEST_CHECK(g.task_remove.bottom <= g.install_frame.bottom);
  }
}

/**
 * @brief The check boxes take their places in the stack without pushing anything out of the
 *        client area
 */
static void test_checks_stack(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    RECT c;
    struct main_layout g;
    SetRect(&c, 0, 0, min_client_width(geometry_dpi_list[i]), layout_scale(ADC_MAIN_WINDOW_H, geometry_dpi_list[i]));
    main_compute(&c,
                 geometry_dpi_list[i],
                 layout_scale(ADC_MAIN_SETTINGS_MIN_W, geometry_dpi_list[i]),
                 layout_scale(settings_caption_logical, geometry_dpi_list[i]),
                 layout_scale(install_caption_logical, geometry_dpi_list[i]),
                 layout_scale(ADC_LAYOUT_CAPTION_H, geometry_dpi_list[i]),
                 &g);
    TEST_CHECK_(g.check_audio_service.bottom > g.check_audio_service.top,
                "dpi %u: the audio service check has no height",
                (unsigned)geometry_dpi_list[i]);
    TEST_CHECK_(g.check_mixer.bottom <= g.check_audio_service.top,
                "dpi %u: the mixer check overlaps the audio service check",
                (unsigned)geometry_dpi_list[i]);
    TEST_CHECK(g.check_audio_service.left == g.check_mixer.left);
    TEST_CHECK(g.check_mixer.left >= g.settings_frame.left);
    TEST_CHECK(g.check_mixer.right <= g.settings_frame.right);
    TEST_CHECK(g.check_audio_service.bottom <= g.settings_frame.bottom);
    TEST_CHECK(g.check_audio_service.bottom <= c.bottom);
    {
      int const above = g.check_mixer.top - g.settings_frame.top;
      int const below = g.settings_frame.bottom - g.check_audio_service.bottom;
      TEST_CHECK_(geometry_abs(above - below) <= 1,
                  "dpi %u: the boxes sit %d from the top and %d from the bottom",
                  (unsigned)geometry_dpi_list[i],
                  above,
                  below);
    }
  }
}

/**
 * @brief The group gets exactly the width its captions asked for
 *
 * The point of the settings group is its captions, and the room it gives them is measured
 * at run time from those captions: a translation can be longer than the English text, and
 * a language can bring a font of its own.  What the geometry has to guarantee is that the
 * group gets exactly the width it asked for, so nothing is cut.  Two very different widths
 * stand in for two languages here.
 */
static void test_group_follows_its_captions(void) {
  static int const dpi = 96;
  int const widths[] = {layout_scale(260, (UINT)dpi), layout_scale(420, (UINT)dpi)};
  size_t i = 0;

  for (i = 0; i < (sizeof(widths) / sizeof(widths[0])); i++) {
    struct main_layout g;
    RECT const c = client_of((UINT)dpi, 400, 0);
    int const settings_w = widths[i];
    int const inset = layout_scale(ADC_MAIN_GROUP_INSET, (UINT)dpi);

    main_compute(&c,
                 (UINT)dpi,
                 settings_w,
                 layout_scale(settings_caption_logical, (UINT)dpi),
                 layout_scale(install_caption_logical, (UINT)dpi),
                 layout_scale(ADC_LAYOUT_CAPTION_H, (UINT)dpi),
                 &g);
    TEST_CHECK_(g.check_mixer.right - g.check_mixer.left == settings_w - 2 * inset,
                "%d: the check boxes got %d of the %d asked for",
                settings_w,
                (int)(g.check_mixer.right - g.check_mixer.left),
                settings_w - 2 * inset);
    TEST_CHECK(g.check_mixer.left - g.settings_frame.left == inset);
    TEST_CHECK(g.settings_frame.right - g.settings_frame.left == settings_w);
  }
}

/**
 * @brief The narrowest window follows the width of the settings group
 *
 * A longer translation moves the window limit with it instead of leaving the group cut
 * off.
 */
static void test_minimum_follows_the_group(void) {
  static int const dpi = 96;
  int const widths[] = {layout_scale(260, (UINT)dpi), layout_scale(420, (UINT)dpi)};
  size_t i = 0;

  for (i = 0; i < (sizeof(widths) / sizeof(widths[0])); i++) {
    struct main_layout g;
    RECT const c = client_of((UINT)dpi, 0, 0);
    int const expected = widths[i] + layout_scale(ADC_MAIN_MIN_CLIENT_EXTRA_W, (UINT)dpi);

    main_compute(&c, (UINT)dpi, widths[i], 0, 0, 0, &g);
    TEST_CHECK_(
        (int)g.min_client_width == expected, "%d: want %d (the group and the rest), got %d", widths[i], expected, (int)g.min_client_width);
    {
      RECT narrow;
      struct main_layout at_limit;
      SetRect(&narrow, 0, 0, g.min_client_width, layout_scale(ADC_MAIN_WINDOW_H, (UINT)dpi));
      main_compute(&narrow, (UINT)dpi, widths[i], 0, 0, 0, &at_limit);
      TEST_CHECK(at_limit.settings_frame.left >= 0);
      TEST_CHECK(at_limit.install_frame.right <= narrow.right);
      TEST_CHECK(at_limit.task_install.right <= at_limit.install_frame.right);
    }
  }
}

/**
 * @brief The window is no bigger than the parts it holds
 */
static void test_window_is_no_bigger_than_its_parts(void) {
  int i = 0;

  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    UINT const dpi = geometry_dpi_list[i];
    RECT client;
    struct main_layout g;
    int const section = layout_scale(ADC_MAIN_SECTION_GAP, dpi);
    int room = 0;

    SetRect(&client, 0, 0, layout_scale(ADC_MAIN_WINDOW_W, dpi), layout_scale(ADC_MAIN_WINDOW_H, dpi));
    main_compute(&client,
                 dpi,
                 layout_scale(ADC_MAIN_SETTINGS_MIN_W, dpi),
                 layout_scale(settings_caption_logical, dpi),
                 layout_scale(install_caption_logical, dpi),
                 layout_scale(ADC_LAYOUT_CAPTION_H, dpi),
                 &g);
    /* the parts are scaled one by one, the height of the window as a whole */
    room = g.settings_caption.top - g.button.bottom;
    TEST_CHECK_(geometry_abs(room - section) <= geometry_slack,
                "dpi %u: want %d of room between the button and the band, got %d",
                (unsigned)dpi,
                (int)section,
                (int)room);
    TEST_CHECK_(room >= 0, "dpi %u: the button sits on the band", (unsigned)dpi);
    TEST_CHECK_(g.min_client_width <= client.right - client.left, "dpi %u: the two groups do not fit side by side", (unsigned)dpi);
    TEST_CHECK_(
        g.install_frame.right <= client.right - layout_scale(ADC_LAYOUT_MARGIN_X, dpi), "dpi %u: the band sticks out", (unsigned)dpi);
    TEST_CHECK_(g.settings_caption.top >= 0, "dpi %u: the caption rides out of the window", (unsigned)dpi);
    TEST_CHECK_(g.task_remove.bottom <= client.bottom - layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, dpi),
                "dpi %u: the band is not above the margin",
                (unsigned)dpi);
  }
}

TEST_LIST = {
    {"geometry_caption_line_balance", test_caption_line_balance},
    {"geometry_caption_rides_the_frame", test_caption_rides_the_frame},
    {"geometry_groups_share_the_band", test_groups_share_the_band},
    {"geometry_task_buttons_stack_in_group", test_task_buttons_stack_in_group},
    {"geometry_buttons_inside_the_frame", test_buttons_inside_the_frame},
    {"geometry_growth_is_proportional", test_growth_is_proportional},
    {"geometry_minimum_covers_the_group", test_minimum_covers_the_group},
    {"geometry_inside_the_client", test_inside_the_client},
    {"geometry_checks_stack", test_checks_stack},
    {"geometry_group_follows_its_captions", test_group_follows_its_captions},
    {"geometry_minimum_follows_the_group", test_minimum_follows_the_group},
    {"geometry_window_is_no_bigger_than_its_parts", test_window_is_no_bigger_than_its_parts},
    {"every_control_carries_the_font", test_every_control_carries_the_font_of_the_window},
    {"window_without_the_theme_is_traditional", test_window_without_the_theme_is_traditional},
    {"window_tells_that_it_waits", test_window_tells_that_it_waits},
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
