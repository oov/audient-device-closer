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

#include "theme.c"

#include <ovtest.h>

#include <string.h>

#include <uxtheme.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

/**
 * @brief What the probe window of the palette broadcast test saw
 */
struct theme_probe {
  unsigned theme_changed;
  unsigned painted;
};

/**
 * @brief The window property the probe keeps its state under
 */
static wchar_t const probe_prop[] = L"adc_theme_probe_state";

/**
 * @brief A window that talks to the theme the way the windows of the application do
 *
 * The probe counts the two things a palette change has to bring to every window: the
 * re-apply that theme_reapply() announces with WM_THEMECHANGED, and the redraw that comes
 * with WM_PAINT.
 */
static LRESULT CALLBACK theme_probe_wndproc(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam) {
  struct ov_error err = {0};
  struct theme_probe *probe = (struct theme_probe *)GetPropW(hwnd, probe_prop);
  LRESULT answer = 0;
  bool success = true;
  bool taken = false; // the probe answered, so the default procedure has nothing to say

  if (msg == WM_NCCREATE) {
    if (SetPropW(hwnd, probe_prop, (HANDLE)((CREATESTRUCTW const *)lparam)->lpCreateParams) == 0) {
      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
      success = false;
    } else {
      probe = (struct theme_probe *)((CREATESTRUCTW const *)lparam)->lpCreateParams;
    }
  }
  if (success && (probe != NULL)) {
    switch (msg) {
    case WM_THEMECHANGED:
      probe->theme_changed++;
      taken = true;
      break;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC const dc = BeginPaint(hwnd, &ps);

      if (dc == NULL) {
        OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
        success = false;
      } else if (!EndPaint(hwnd, &ps)) {
        OV_ERROR_SET_GENERIC(&err, ov_error_generic_fail); // GDI does not say why
        success = false;
      } else {
        probe->painted++;
      }
      // a paint that failed is still a paint the probe spent, not one the system is asked
      // to do again
      taken = true;
      break;
    }
    case WM_NCDESTROY:
      if (!RemovePropW(hwnd, probe_prop)) {
        OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
        success = false;
      }
      break;
    default:
      break;
    }
  }
  if (success && !taken) {
    ov_tribool const handled = theme_message(hwnd, msg, wparam, lparam, &answer, &err);

    if (handled == ov_indeterminate) {
      success = false;
    } else if (handled == ov_true) {
      taken = true;
    }
  }
  if (!success) {
    OV_ERROR_REPORT(&err, NULL); // a window procedure has no caller to be told of a failure
  }
  return taken ? answer : DefWindowProcW(hwnd, msg, wparam, lparam);
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

  TEST_SUCCEEDED(theme_init(true, &berr), &berr);
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
  if ((owner != NULL) && IsWindow(owner)) {
    HGDIOBJ const stock = GetStockObject(BLACK_BRUSH);

    // the system deletes the background brush of a class when the class goes, and the
    // brush of the palette is not its to delete
    SetClassLongPtrW(owner, GCLP_HBRBACKGROUND, (LONG_PTR)stock);
  }
  if (owner != NULL) {
    DestroyWindow(owner);
  }
  if (dialog != NULL) {
    DestroyWindow(dialog);
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
  }
  TEST_SUCCEEDED(theme_release(&berr), &berr);
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
  HDC dc = NULL;

  TEST_SUCCEEDED(theme_init(false, &berr), &berr);
  TEST_CHECK(theme_state_dark() == false);
  TEST_CHECK(theme_refresh(&merr) == ov_false);

  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush(&berr);
  if (!TEST_SUCCEEDED(wc.hbrBackground != NULL, &berr)) {
    goto cleanup;
  }
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
  TEST_SUCCEEDED(theme_attach(button, &berr), &berr);
  TEST_CHECK(GetWindowLongPtrW(button, GWLP_WNDPROC) == button_proc);
  TEST_MSG("want the button painted by the system, not by a subclass of the theme");

  bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 80, 20, owner, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(bar != NULL)) {
    goto cleanup;
  }
  TEST_SUCCEEDED(theme_attach(bar, &berr), &berr);
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
  TEST_SUCCEEDED(theme_attach_list(list, &berr), &berr);
  TEST_CHECK(GetWindowTheme(list) == NULL);
  TEST_MSG("want no theme class of the palette on the list");
  if (header != NULL) {
    TEST_CHECK(GetWindowLongPtrW(header, GWLP_WNDPROC) == header_proc);
    TEST_MSG("want the header painted by the system, not by a subclass of the theme");
  }

  dc = GetDC(button);
  if (TEST_CHECK(dc != NULL)) {
    TEST_CHECK(theme_message(button, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)button, &result, &berr) == ov_false);
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
  TEST_SUCCEEDED(theme_release(&berr), &berr);
}

/**
 * @brief Stand in for the answer of the system
 *
 * @return true, the system asked for the dark palette
 */
static bool says_dark(void) { return true; }

/**
 * @brief Stand in for the answer of the system
 *
 * @return false, the system asked for the light palette
 */
static bool says_light(void) { return false; }

/**
 * @brief Both palettes are walked, whatever the machine is set to
 *
 * Which palette the process paints with is an input the theme takes from the system.  The
 * test gives that input itself, so the dark branch is walked on a light machine and the
 * other way round, and a palette change is a change of the input rather than a change of
 * the machine.
 *
 * The system cannot be asked for the answer: ShouldAppsUseDarkMode describes the app mode of
 * the process, which is light until the theme itself asks for the dark palette, so its answer
 * cannot tell which branch the theme took.
 */
static void test_both_palettes_are_reached(void) {
  struct ov_error err = {0};
  COLORREF const system_window = GetSysColor(COLOR_WINDOW);
  COLORREF const system_text = GetSysColor(COLOR_WINDOWTEXT);
  COLORREF const system_face = GetSysColor(COLOR_BTNFACE);

  TEST_CASE("the dark palette");
  g_prefers_dark = says_dark;
  TEST_SUCCEEDED(theme_init(true, &err), &err);
  TEST_CHECK_(theme_state_dark() == true, "want the dark palette, got the light one");
  TEST_CHECK(theme_palette_surface() == RGB(0x20, 0x20, 0x20));
  TEST_CHECK(theme_palette_text() == RGB(0xFF, 0xFF, 0xFF));
  TEST_CHECK(theme_palette_control() == RGB(0x2B, 0x2B, 0x2B));
  TEST_SUCCEEDED(theme_release(&err), &err);

  TEST_CASE("the light palette");
  g_prefers_dark = says_light;
  TEST_SUCCEEDED(theme_init(true, &err), &err);
  TEST_CHECK_(theme_state_dark() == false, "want the light palette, got the dark one");
  TEST_CHECK(theme_palette_surface() == system_window);
  TEST_CHECK(theme_palette_text() == system_text);
  TEST_CHECK(theme_palette_control() == system_face);
  TEST_SUCCEEDED(theme_release(&err), &err);

  TEST_CASE("a change of the palette is told and followed");
  g_prefers_dark = says_light;
  TEST_SUCCEEDED(theme_init(true, &err), &err);
  TEST_CHECK(theme_palette_surface() == system_window);
  g_prefers_dark = says_dark;
  TEST_CHECK(theme_refresh(&err) == ov_true);
  TEST_MSG("want the change of the palette told");
  TEST_CHECK(theme_state_dark() == true);
  TEST_CHECK(theme_palette_surface() == RGB(0x20, 0x20, 0x20));
  TEST_SUCCEEDED(theme_release(&err), &err);

  TEST_CASE("a palette that did not change is not told");
  g_prefers_dark = says_light;
  TEST_SUCCEEDED(theme_init(true, &err), &err);
  TEST_CHECK(theme_refresh(&err) == ov_false);
  TEST_MSG("want a palette that did not change to be silent");
  TEST_SUCCEEDED(theme_release(&err), &err);
}

/**
 * @brief A list is attached in the light palette without a header to detach
 *
 * The light palette takes the theme of the system off the list and its header.  The
 * header of a list that was never themed has no subclass to remove, which is not a
 * failure: the list is still the one the system made.
 */
static void test_light_palette_attaches_a_list(void) {
  struct ov_error err = {0};
  WNDCLASSEXW wc;
  ATOM atom = 0;
  HWND owner = NULL;
  HWND list = NULL;
  HWND header = NULL;
  LONG_PTR header_proc = 0;

  g_prefers_dark = says_light;
  TEST_SUCCEEDED(theme_init(true, &err), &err);
  TEST_CHECK(theme_state_dark() == false);

  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush(&err);
  if (!TEST_SUCCEEDED(wc.hbrBackground != NULL, &err)) {
    goto cleanup;
  }
  wc.lpszClassName = L"adc_theme_light_list_test";
  atom = RegisterClassExW(&wc);
  if (!TEST_CHECK(atom != 0)) {
    goto cleanup;
  }
  owner = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE, 0, 0, 100, 60, NULL, NULL, wc.hInstance, NULL);
  list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT, 0, 0, 80, 60, owner, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(owner != NULL && list != NULL)) {
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
  TEST_SUCCEEDED(theme_attach_list(list, &err), &err);
  if (header != NULL) {
    TEST_CHECK(GetWindowLongPtrW(header, GWLP_WNDPROC) == header_proc);
    TEST_MSG("want the header painted by the system, not by a subclass of the theme");
  }

cleanup:
  if (list != NULL) {
    DestroyWindow(list);
  }
  if (owner != NULL) {
    DestroyWindow(owner);
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
  }
  TEST_SUCCEEDED(theme_release(&err), &err);
}

TEST_LIST = {
    {"both_palettes_are_reached", test_both_palettes_are_reached},
    {"theme_change_reaches_every_window", test_theme_change_reaches_every_window},
    {"theme_switch_detaches_the_theme", test_theme_switch_detaches_the_theme},
    {"light_palette_attaches_a_list", test_light_palette_attaches_a_list},
    {NULL, NULL},
};
