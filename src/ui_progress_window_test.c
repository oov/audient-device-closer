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

#include "ui_progress_window.c"

#include <ovtest.h>

#include <string.h>

#include <commctrl.h>
#include <uxtheme.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "layout.h"
#include "repair.h"
#include "settings.h"
#include "theme.h"

enum {
  ADC_TEST_PROGRESS_SUGGEST_LEFT = 240, /* where the window goes when a monitor suggests it */
  ADC_TEST_PROGRESS_SUGGEST_TOP = 200,
  ADC_TEST_PROGRESS_SUGGEST_RIGHT = 900, /* a size the layout of the window does not ask for */
  ADC_TEST_PROGRESS_SUGGEST_BOTTOM = 700,
};

/**
 * @brief The height argument of CreateFontW, the same rule the layout uses
 *
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the height to give to CreateFontW
 */
static int font_height(UINT const dpi) { return layout_font_height(dpi); }
/**
 * @brief Which palette the system asks this program to use
 *
 * The tests ask the system instead of the theme they check: the answer is the input both
 * work from, and a test that read it off the theme could only ever agree with the theme,
 * never tell that the theme took the wrong branch.
 *
 * @return true when the system is in the dark mode and no high contrast is on
 */
static bool theme_dark(void) {
  typedef BOOL(WINAPI * dark_mode_fn)(void);
  HMODULE const uxtheme = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  HIGHCONTRASTW hc;
  bool dark = false;

  if (uxtheme != NULL) {
    dark_mode_fn const should = (dark_mode_fn)(void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(132));

    dark = (should != NULL) && (should() != FALSE);
    FreeLibrary(uxtheme);
  }
  memset(&hc, 0, sizeof(hc));
  hc.cbSize = sizeof(hc);
  if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, FALSE) && ((hc.dwFlags & HCF_HIGHCONTRASTON) != 0)) {
    dark = false;
  }
  return dark;
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
  return lf.lfHeight == font_height((UINT)dpi);
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
 * @brief The label and the bar say what a run reported
 *
 * The window is the one thing a run talks to, so a test makes it, reports the steps a run
 * would report and reads back what the reader would see.  Nothing here waits: every step
 * is reported by the test itself.
 */
static void test_progress_window_shows_the_steps(void) {
  struct ov_error perr = {0};
  HANDLE cancel = NULL;
  ATOM atom = 0;
  HWND owner = NULL;
  HWND progress = NULL;
  WNDCLASSEXW wc;

  cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!TEST_CHECK(cancel != NULL)) {
    goto cleanup;
  }
  TEST_SUCCEEDED(theme_init(true, &perr), &perr);
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush(&perr);
  TEST_CHECK(wc.hbrBackground != NULL);
  wc.lpszClassName = L"adc_progress_report_owner";
  atom = RegisterClassExW(&wc);
  TEST_CHECK(atom != 0);
  owner = CreateWindowExW(0, wc.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(owner != NULL)) {
    goto cleanup;
  }
  if (!TEST_SUCCEEDED(
          ui_progress_window_create(
              owner, (int)GetDpiForWindow(owner), ui_progress_window_steps(true, 1), cancel, NULL, NULL, NULL, NULL, &progress, &perr),
          &perr)) {
    goto cleanup;
  }
  TEST_CASE("the bar counts the steps the window was made with");
  {
    HWND const bar = FindWindowExW(progress, NULL, PROGRESS_CLASSW, NULL);
    PBRANGE range = {0, 0};

    if (bar != NULL) {
      SendMessageW(bar, PBM_GETRANGE, TRUE, (LPARAM)&range);
    }
    TEST_CHECK_(range.iHigh == (int)ui_progress_window_steps(true, 1), "want a bar over the steps of the run, got %d", range.iHigh);
  }
  TEST_CASE("the label counts the step and the total, and the bar moves with it");
  {
    HWND const bar = FindWindowExW(progress, NULL, PROGRESS_CLASSW, NULL);

    TEST_SUCCEEDED(ui_progress_window_report(progress, 1, "Waiting...", 8, &perr), &perr);
    TEST_CHECK(find_child_with_text(progress, L"Waiting... (1/8)") != NULL);
    TEST_MSG("want the label to show the step and the total it was told");
    TEST_CHECK_((bar != NULL) && (SendMessageW(bar, PBM_GETPOS, 0, 0) == 1), "want the bar at the first step");
    TEST_SUCCEEDED(ui_progress_window_report(progress, 5, "Checking what is blocking the close...", 8, &perr), &perr);
    TEST_CHECK(find_child_with_text(progress, L"Checking what is blocking the close... (5/8)") != NULL);
    TEST_CHECK_((bar != NULL) && (SendMessageW(bar, PBM_GETPOS, 0, 0) == 5), "want the bar at the fifth step");
  }
  TEST_CASE("a report with no window is answered, not a crash");
  TEST_FAILED_WITH(
      ui_progress_window_report(NULL, 1, "Waiting...", 8, &perr), &perr, ov_error_type_generic, ov_error_generic_invalid_argument);
  TEST_CASE_(NULL);

cleanup:
  if (progress != NULL) {
    TEST_SUCCEEDED(ui_progress_window_destroy(&progress, &perr), &perr);
  }
  if (cancel != NULL) {
    CloseHandle(cancel);
  }
  if (owner != NULL) {
    DestroyWindow(owner);
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
  }
  TEST_SUCCEEDED(theme_release(&perr), &perr);
}

/**
 * @brief Every stage of a run is named, and the count of the steps follows the run
 *
 * The window is not part of this: a run reports a stage and a step, and the two decisions
 * of what to show -- the line of the stage and how many steps the bar counts -- are what a
 * test can check without a screen.
 */
static void test_stage_texts_and_steps(void) {
  struct settings s;
  char buf[256];

  memset(&s, 0, sizeof(s));

  TEST_CASE("every stage has a line of its own");
  {
    enum repair_stage const stages[] = {REPAIR_STAGE_WAIT,
                                        REPAIR_STAGE_PROBE,
                                        REPAIR_STAGE_REMOVE,
                                        REPAIR_STAGE_MIXER,
                                        REPAIR_STAGE_PAUSE,
                                        REPAIR_STAGE_VETO_RECORD,
                                        REPAIR_STAGE_CONFIRM,
                                        REPAIR_STAGE_START};

    for (size_t i = 0; i < (sizeof(stages) / sizeof(stages[0])); i++) {
      buf[0] = '\0';
      ui_progress_window_line(stages[i], &s, buf, sizeof(buf));
      TEST_CHECK_(buf[0] != '\0', "want a line for stage %d", (int)stages[i]);
    }
  }
  TEST_CASE("two stages of the run read differently");
  {
    char mixer[256];
    char probe[256];

    ui_progress_window_line(REPAIR_STAGE_MIXER, &s, mixer, sizeof(mixer));
    ui_progress_window_line(REPAIR_STAGE_PROBE, &s, probe, sizeof(probe));
    TEST_CHECK(strcmp(mixer, probe) != 0);
    TEST_MSG("want the stage of the mixer to read differently from the stage of the probe");
  }
  TEST_CASE("a call with no room to write writes nothing");
  buf[0] = 'x';
  ui_progress_window_line(REPAIR_STAGE_WAIT, &s, NULL, 0);
  ui_progress_window_line(REPAIR_STAGE_WAIT, &s, buf, 0);
  TEST_CHECK(buf[0] == 'x');
  TEST_CASE("the count of the steps follows the run");
  TEST_CHECK_(ui_progress_window_steps(false, 1) == REPAIR_STEPS_PER_TARGET, "want one device to take the steps of one target");
  TEST_CHECK_(ui_progress_window_steps(true, 1) == REPAIR_STEPS_PER_TARGET + 1, "want the wait to count on top of it");
  TEST_CHECK_(ui_progress_window_steps(false, 2) == 2 * REPAIR_STEPS_PER_TARGET, "want two devices to take twice the steps");
  TEST_CASE_(NULL);
}

/**
 * @brief One ask of the window, as the caller of ui_progress_window_create sees it
 *
 * The window tells its caller that the reader asked it to stop, and what it does with that
 * is the caller's: this counts the calls so a test can see that one was made.
 */
struct ask_count {
  size_t asks; //!< how often the window said the reader asked to stop
};

static void count_ask(void *const userdata) {
  struct ask_count *const count = (struct ask_count *)(void *)userdata;

  count->asks++;
}

/**
 * @brief The abort press and the close of the progress window are asks, not ends
 *
 * The window hands the ask over the flag its caller owns, tells its caller about it, greys
 * its own ways out and stays where it is: only the run that made it takes it down.  Every
 * press here is a message sent to a window of the test thread itself, so its answer is true
 * when the send has returned and no moment of the clock has to be trusted.
 */
static void test_progress_window_greys_the_asks_and_stays(void) {
  struct ov_error perr = {0};
  struct ask_count asked = {0};
  HANDLE cancel = NULL;
  ATOM atom = 0;
  HWND owner = NULL;
  HWND progress = NULL;
  WNDCLASSEXW wc;

  cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!TEST_CHECK(cancel != NULL)) {
    goto cleanup;
  }
  TEST_SUCCEEDED(theme_init(true, &perr), &perr);
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush(&perr);
  TEST_CHECK(wc.hbrBackground != NULL);
  wc.lpszClassName = L"adc_progress_ask_owner";
  atom = RegisterClassExW(&wc);
  TEST_CHECK(atom != 0);
  owner = CreateWindowExW(0, wc.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(owner != NULL)) {
    goto cleanup;
  }
  if (!TEST_SUCCEEDED(ui_progress_window_create(owner,
                                                (int)GetDpiForWindow(owner),
                                                ui_progress_window_steps(false, 1),
                                                cancel,
                                                count_ask,
                                                &asked,
                                                NULL,
                                                NULL,
                                                &progress,
                                                &perr),
                      &perr)) {
    goto cleanup;
  }

  TEST_CASE("the press of abort signals the ask of the caller and greys the button");
  {
    HWND cancel_btn = FindWindowExW(progress, NULL, L"Button", NULL);

    if (TEST_CHECK(cancel_btn != NULL)) {
      TEST_CHECK(IsWindowEnabled(cancel_btn) != FALSE);
      TEST_MSG("want the button offered when the caller named what the ask does");
      SendMessageW(progress, WM_COMMAND, MAKEWPARAM(IDC_PROGRESS_CANCEL, BN_CLICKED), (LPARAM)cancel_btn);
      TEST_CHECK(WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0);
      TEST_MSG("want the press to hand the ask to the event the caller owns");
      TEST_CHECK_(asked.asks == 1, "want the caller told about the ask once, got %zu", asked.asks);
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
  TEST_CHECK_(asked.asks == 1, "want the close not asked again once the ask is on its way, got %zu", asked.asks);

  TEST_CASE("only the hands of the run take the window down");
  TEST_SUCCEEDED(ui_progress_window_destroy(&progress, &perr), &perr);
  TEST_CHECK(progress == NULL);
  TEST_CHECK(IsWindow(progress) == 0);
  TEST_CASE_(NULL);

cleanup:
  if (progress != NULL) {
    TEST_SUCCEEDED(ui_progress_window_destroy(&progress, &perr), &perr);
  }
  if (cancel != NULL) {
    CloseHandle(cancel);
  }
  if (owner != NULL) {
    DestroyWindow(owner);
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
  }
  TEST_SUCCEEDED(theme_release(&perr), &perr);
}

/**
 * @brief A window with nobody to tell does not offer the ask it cannot pass on
 *
 * The button that asks the run to stop is greyed when the caller named no call for it: an
 * ask that reaches nobody would grey the button and leave the run going.
 */
static void test_progress_window_offers_no_ask_it_cannot_pass_on(void) {
  struct ov_error perr = {0};
  HANDLE cancel = NULL;
  ATOM atom = 0;
  HWND owner = NULL;
  HWND progress = NULL;
  WNDCLASSEXW wc;

  cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!TEST_CHECK(cancel != NULL)) {
    goto cleanup;
  }
  TEST_SUCCEEDED(theme_init(true, &perr), &perr);
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush(&perr);
  TEST_CHECK(wc.hbrBackground != NULL);
  wc.lpszClassName = L"adc_progress_no_ask_owner";
  atom = RegisterClassExW(&wc);
  TEST_CHECK(atom != 0);
  owner = CreateWindowExW(0, wc.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(owner != NULL)) {
    goto cleanup;
  }
  if (!TEST_SUCCEEDED(
          ui_progress_window_create(
              owner, (int)GetDpiForWindow(owner), ui_progress_window_steps(false, 1), cancel, NULL, NULL, NULL, NULL, &progress, &perr),
          &perr)) {
    goto cleanup;
  }

  {
    HWND cancel_btn = FindWindowExW(progress, NULL, L"Button", NULL);

    if (TEST_CHECK(cancel_btn != NULL)) {
      TEST_CHECK(IsWindowEnabled(cancel_btn) == FALSE);
      TEST_MSG("want no button for an ask that reaches no caller");
    }
  }

  TEST_CASE("the close of the window is no ask either");
  SendMessageW(progress, WM_CLOSE, 0, 0);
  TEST_CHECK(IsWindow(progress) != 0);
  TEST_MSG("want the progress window to stay until the run that made it is done");
  TEST_CHECK(WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0);
  TEST_MSG("want no flag raised for an ask that reaches no caller");

cleanup:
  if (progress != NULL) {
    TEST_SUCCEEDED(ui_progress_window_destroy(&progress, &perr), &perr);
  }
  if (cancel != NULL) {
    CloseHandle(cancel);
  }
  if (owner != NULL) {
    DestroyWindow(owner);
  }
  if (atom != 0) {
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
  }
  TEST_SUCCEEDED(theme_release(&perr), &perr);
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

  TEST_SUCCEEDED(theme_init(true, &berr), &berr);
  parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 100, 60, NULL, NULL, GetModuleHandleW(NULL), NULL);
  if (!TEST_CHECK(parent != NULL)) {
    goto cleanup;
  }
  bar = CreateWindowExW(
      0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 100, 20, parent, NULL, GetModuleHandleW(NULL), NULL);
  if (!TEST_CHECK(bar != NULL)) {
    goto cleanup;
  }
  TEST_SUCCEEDED(theme_attach(bar, &berr), &berr);
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
  TEST_SUCCEEDED(theme_release(&berr), &berr);
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
  HANDLE cancel = NULL;
  ATOM atom = 0;
  HFONT borrowed = NULL;
  HWND owner = NULL;
  HWND progress = NULL;
  WNDCLASSEXW wc;

  cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!TEST_CHECK(cancel != NULL)) {
    goto cleanup;
  }

  TEST_SUCCEEDED(theme_init(true, &perr), &perr);
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush(&perr);
  TEST_CHECK(wc.hbrBackground != NULL);
  wc.lpszClassName = L"adc_progress_test_owner";
  atom = RegisterClassExW(&wc);
  TEST_CHECK(atom != 0);
  owner = CreateWindowExW(0, wc.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, NULL, NULL, wc.hInstance, NULL);
  if (!TEST_CHECK(owner != NULL)) {
    goto cleanup;
  }
  TEST_SUCCEEDED(theme_attach(owner, &perr), &perr);
  borrowed = layout_make_font((UINT)GetDpiForWindow(owner));
  SendMessageW(owner, WM_SETFONT, (WPARAM)borrowed, TRUE);
  if (!TEST_SUCCEEDED(
          ui_progress_window_create(
              owner, (int)GetDpiForWindow(owner), ui_progress_window_steps(false, 1), cancel, NULL, NULL, NULL, NULL, &progress, &perr),
          &perr)) {
    goto cleanup;
  }
  ShowWindow(progress, SW_SHOW);
  /* the owner moves to a monitor of its own dpi and lets go of its font */
  DeleteObject(borrowed);
  borrowed = NULL;

  {
    int const least = layout_scale(ADC_PROGRESS_CANCEL_W, 96) + 2 * layout_scale(ADC_LAYOUT_MARGIN_X, 96);
    RECT outer;

    GetWindowRect(progress, &outer);
    TEST_CHECK_(least <= (outer.right - outer.left), "want the window at least %d wide, it is %d", least, (int)(outer.right - outer.left));
  }

  TEST_CASE("the window is the size its layout asks for");
  {
    int const dpi = (int)GetDpiForWindow(progress);
    int const want_w = layout_scale(ADC_PROGRESS_W, (UINT)dpi);
    struct progress_layout g;
    RECT client;

    progress_compute(want_w, (UINT)dpi, &g);
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
    int const want_w = layout_scale(ADC_PROGRESS_W, (UINT)dpi);
    struct progress_layout g;
    RECT client;
    RECT outer;
    RECT suggested;

    progress_compute(want_w, (UINT)dpi, &g);
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
    int const least = layout_scale(ADC_PROGRESS_W, (UINT)dpi);
    HWND wide = NULL;
    RECT client;

    if (!TEST_SUCCEEDED(ui_progress_window_create(
                            owner, dpi, ui_progress_window_steps(false, 1), cancel, NULL, NULL, long_line, long_line, &wide, &perr),
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
        OV_SNPRINTF(shown, sizeof(shown) / sizeof(shown[0]), L"%1$s", L"%1$s", long_line);
        SetWindowTextW(status, shown);
        TEST_CHECK(text_fits(status));
        TEST_MSG("want the line of the run shown whole in the window");
        TEST_CHECK_((GetWindowLongPtrW(status, GWL_STYLE) & SS_ENDELLIPSIS) == 0, "want room for the line, not an ellipsis that hides it");
      }
      TEST_SUCCEEDED(ui_progress_window_destroy(&wide, &perr), &perr);
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
    TEST_SUCCEEDED(ui_progress_window_destroy(&progress, &perr), &perr);
    progress = NULL;
  }
  if (cancel != NULL) {
    CloseHandle(cancel);
    cancel = NULL;
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
  TEST_SUCCEEDED(theme_release(&perr), &perr);
}

/** @brief The dpis the checks of the geometry walk */
static UINT const geometry_dpi_list[] = {96, 120, 144, 192};
#define GEOMETRY_DPI_COUNT ((int)(sizeof(geometry_dpi_list) / sizeof(geometry_dpi_list[0])))

/** @brief The pixels the scaling of one part and the scaling of a whole can differ by */
static int const scaling_slack = 1;

/**
 * @brief The size of a difference, unsigned
 *
 * @param v the difference of two values
 * @return the size it stands for
 */
static int geometry_abs(int const v) { return (v < 0) ? -v : v; }

/**
 * @brief The progress dialog puts the bar under the label, inside its margins
 */
static void test_progress_dialog_stacks(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    UINT const dpi = geometry_dpi_list[i];
    struct progress_layout g;
    int const width = layout_scale(ADC_PROGRESS_W, dpi);
    int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
    progress_compute(width, dpi, &g);
    TEST_CHECK(g.label.top == layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi));
    TEST_CHECK_(g.label.left == margin, "dpi %u: label left %d, margin %d", (unsigned)dpi, (int)g.label.left, margin);
    TEST_CHECK(g.label.right == width - margin);
    TEST_CHECK(g.bar.top == g.label.bottom + layout_scale(ADC_LAYOUT_GAP, dpi));
    TEST_CHECK(g.bar.bottom == g.bar.top + layout_scale(ADC_PROGRESS_BAR_H, dpi));
    TEST_CHECK(g.bar.left == g.label.left);
    TEST_CHECK(g.bar.right == g.label.right);
    TEST_CHECK(g.cancel.top == g.bar.bottom + layout_scale(ADC_LAYOUT_GAP, dpi));
    TEST_CHECK(g.cancel.bottom == g.cancel.top + layout_scale(ADC_PROGRESS_CANCEL_H, dpi));
    TEST_CHECK_(g.cancel.left > 0, "dpi %u: the button starts at %d", (unsigned)dpi, (int)g.cancel.left);
    TEST_CHECK(g.cancel.right < width);
    TEST_CHECK((g.cancel.left + g.cancel.right) / 2 == width / 2);
    /* the height the window is made with is the stack it holds: no room below the button,
     * and nothing of it cut off */
    TEST_CHECK_(g.needed_height - g.cancel.bottom == layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, dpi),
                "dpi %u: want the height %d, got %d",
                (unsigned)dpi,
                (int)(g.cancel.bottom + layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, dpi)),
                (int)g.needed_height);
    TEST_CHECK(g.label.right <= width);
    TEST_CHECK(g.bar.bottom <= g.needed_height);
    TEST_CHECK(g.cancel.bottom <= g.needed_height);
  }
}

/**
 * @brief The height the progress dialog needs does not depend on its width
 */
static void test_progress_dialog_height_fits_its_parts(void) {
  int i = 0;

  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    UINT const dpi = geometry_dpi_list[i];
    struct progress_layout wide;
    struct progress_layout narrow;

    progress_compute(layout_scale(ADC_PROGRESS_W, dpi), dpi, &wide);
    progress_compute(layout_scale(ADC_PROGRESS_CANCEL_W, dpi), dpi, &narrow);
    TEST_CHECK_(narrow.needed_height == wide.needed_height,
                "dpi %u: want %d, got %d",
                (unsigned)dpi,
                (int)wide.needed_height,
                (int)narrow.needed_height);
  }
}

/**
 * @brief The width of the dialog holds the button it centers
 */
static void test_progress_dialog_holds_its_button(void) {
  TEST_CHECK_(ADC_PROGRESS_W >= ADC_PROGRESS_CANCEL_W + 2 * ADC_LAYOUT_MARGIN_X,
              "the width %d, the button and its margins need %d",
              (int)ADC_PROGRESS_W,
              (int)(ADC_PROGRESS_CANCEL_W + 2 * ADC_LAYOUT_MARGIN_X));
}

/**
 * @brief The geometry of the progress dialog scales with the dpi
 */
static void test_progress_dialog_scales(void) {
  struct progress_layout small;
  struct progress_layout large;
  progress_compute(layout_scale(ADC_PROGRESS_W, 96), 96, &small);
  progress_compute(layout_scale(ADC_PROGRESS_W, 144), 144, &large);
  TEST_CHECK(geometry_abs((small.bar.bottom - small.bar.top) * 144 / 96 - (large.bar.bottom - large.bar.top)) <= 2);
  TEST_CHECK(geometry_abs(small.needed_height * 144 / 96 - large.needed_height) <= scaling_slack);
}

TEST_LIST = {
    {"progress_dialog_stacks", test_progress_dialog_stacks},
    {"progress_dialog_height_fits_its_parts", test_progress_dialog_height_fits_its_parts},
    {"progress_dialog_holds_its_button", test_progress_dialog_holds_its_button},
    {"progress_dialog_scales", test_progress_dialog_scales},
    {"progress_window_shows_the_steps", test_progress_window_shows_the_steps},
    {"stage_texts_and_steps", test_stage_texts_and_steps},
    {"progress_window_greys_the_asks", test_progress_window_greys_the_asks_and_stays},
    {"progress_window_offers_no_ask_it_cannot_pass_on", test_progress_window_offers_no_ask_it_cannot_pass_on},
    {"progress_bar_follows_the_palette", test_progress_bar_follows_the_palette},
    {"progress_window_keeps_its_size_and_font", test_progress_window_keeps_its_size_and_font},
    {NULL, NULL},
};
