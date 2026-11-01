/**
 * Tests for the window that names the processes that hold the device.
 *
 * The window is one of its own, so the test makes it against a window of its own and reads
 * the list, the fonts and the smallest size back from it while it is up.  The modal loop of
 * the window is driven from the timer of the owner, the way a person presses the button.
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

#include "ui_blockers_window.c"

#include <ovtest.h>

#include <string.h>

#include <commctrl.h>

#include <ovprintf.h>

#include "layout.h"
#include "theme.h"
#include "veto_event.h"

enum {
  ADC_BLOCKERS_TEST_TIMER_ID = 1,
  ADC_BLOCKERS_TEST_TIMER_MS = 100,
};

/** @brief Room for one event the way the log renders it */
enum { kEventXmlLen = 512 };

/**
 * @brief Write one record as the log renders it
 *
 * The values are written as they are: the reader resolves entities.
 *
 * @param xml room for the event
 * @param size room of xml, in wchar_t
 * @param record what the event states
 */
static void render_event(wchar_t *const xml, size_t const size, struct veto_event const *const record) {
  wchar_t path_data[kEventXmlLen] = L"";
  wchar_t device_data[kEventXmlLen] = L"";

  if (record->path != NULL) {
    swprintf(path_data, sizeof(path_data) / sizeof(path_data[0]), L"<Data Name='ProcessName'>%ls</Data>", record->path);
  }
  if (record->device_instance != NULL) {
    swprintf(device_data, sizeof(device_data) / sizeof(device_data[0]), L"<Data Name='DeviceInstance'>%ls</Data>", record->device_instance);
  }
  swprintf(xml,
           size,
           L"<Event><EventData><Data Name='ProcessId'>%lu</Data>%ls%ls</EventData></Event>",
           (unsigned long)record->pid,
           path_data,
           device_data);
}

/** @brief The records a list is built from */
struct event_script {
  struct veto_event const *records; //!< the records, in the order the log wrote them
  size_t count;                     //!< how many there are
};

/**
 * @brief Hand the records of the script to the list
 *
 * @param ctx where the events go
 * @param userdata the script
 * @param err receives the failure
 * @return false when the list could not take one of them
 */
static NODISCARD bool emit_script(struct veto_event_context *const ctx, void *const userdata, struct ov_error *const err) {
  struct event_script const *const script = (struct event_script const *)userdata;
  wchar_t xml[kEventXmlLen];

  for (size_t i = 0; i < script->count; i++) {
    render_event(xml, sizeof(xml) / sizeof(xml[0]), &script->records[i]);
    if (!ctx->emit(ctx, xml, err)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief The list of the records the caller states
 *
 * @param records what the log wrote, in order
 * @param count how many records that is
 * @param err receives the failure
 * @return the list, NULL with err set when it could not be built
 */
static NODISCARD struct veto_event *build_list(struct veto_event const *const records, size_t const count, struct ov_error *const err) {
  struct event_script script = {.records = records, .count = count};

  return veto_event_list_build(emit_script, &script, err);
}

/**
 * @brief The height argument of CreateFontW, the rule the layout of every window uses
 *
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the height to give to CreateFontW
 */
static int font_height(UINT const dpi) { return layout_font_height(dpi); }

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
      OV_SNPRINTF(want, sizeof(want) / sizeof(want[0]), L"%1$s", L"%1$s", caption);
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
    layout_window_rect(layout_scale(ADC_BLOCKERS_MIN_W, dpi),
                       layout_scale(ADC_BLOCKERS_MIN_H, dpi),
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

  SendMessageW(blockers, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)ok_btn);
}

/**
 * @brief Answer the blockers window with the key that says nothing
 *
 * The message is the one IsDialogMessageW sends for ESC.
 *
 * @param hwnd the owner the timer was set on
 * @param msg the timer message
 * @param id the identifier of the timer
 * @param tickcount the tick count when the timer fired
 */
static void CALLBACK blockers_test_escape_timer(HWND const hwnd, UINT const msg, UINT_PTR const id, DWORD const tickcount) {
  HWND blockers = NULL;

  (void)msg;
  (void)id;
  (void)tickcount;
  KillTimer(hwnd, ADC_BLOCKERS_TEST_TIMER_ID);
  blockers = GetLastActivePopup(hwnd);
  if (!TEST_CHECK((blockers != NULL) && (blockers != hwnd))) {
    return;
  }
  SendMessageW(blockers, WM_COMMAND, IDCANCEL, 0);
}

static void test_blockers_window_shows_the_processes(void) {
  struct veto_event const records[] = {
      {.pid = 100, .path = L"\\Device\\HarddiskVolume1\\Program Files\\Audient\\iD\\iD.exe", .device_instance = L"USB\\VID_2708&PID_0008"},
      {.pid = 200, .path = L"C:\\WINDOWS\\system32\\audiodg.exe"},
  };
  struct veto_event *events = NULL;
  struct ov_error berr = {0};
  ATOM atom = 0;
  HFONT font = NULL;
  HWND owner = NULL;
  HWND blockers_wnd = NULL;
  WNDCLASSEXW wc;

  events = build_list(records, sizeof(records) / sizeof(records[0]), &berr);
  if (!TEST_SUCCEEDED(events != NULL, &berr)) {
    goto cleanup;
  }

  TEST_SUCCEEDED(theme_init(true, &berr), &berr);
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hbrBackground = theme_class_brush(&berr);
  TEST_CHECK(wc.hbrBackground != NULL);
  wc.lpszClassName = L"adc_blockers_test_owner";
  atom = RegisterClassExW(&wc);
  TEST_CHECK(atom != 0);
  font = CreateFontW(font_height(USER_DEFAULT_SCREEN_DPI),
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
  TEST_SUCCEEDED(theme_attach(owner, &berr), &berr);
  SendMessageW(owner, WM_SETFONT, (WPARAM)font, TRUE);
  ShowWindow(owner, SW_SHOW);

  TEST_CASE("the blockers window opens over the main window and takes it back");
  {
    UINT_PTR timer = 0;

    timer = SetTimer(owner, ADC_BLOCKERS_TEST_TIMER_ID, ADC_BLOCKERS_TEST_TIMER_MS, blockers_test_timer);
    if (!TEST_CHECK(timer != 0)) {
      goto cleanup;
    }
    if (!TEST_SUCCEEDED(ui_blockers_window_show(owner, events, NULL, 0, &berr), &berr)) {
      goto cleanup;
    }
    KillTimer(owner, ADC_BLOCKERS_TEST_TIMER_ID);
    TEST_CHECK(!IsWindow(blockers_wnd));
    TEST_MSG("want the blockers window gone after the press of its OK button");
    TEST_CHECK(IsWindowEnabled(owner) != 0);
    TEST_MSG("want the owner unlocked when the list is done");
    TEST_CHECK(GetActiveWindow() == owner);
    TEST_MSG("want the owner active again after the popup is gone");
    blockers_wnd = NULL;
  }
  TEST_CASE_(NULL);

  TEST_CASE("the key that says nothing takes the blockers window down");
  {
    UINT_PTR timer = 0;

    timer = SetTimer(owner, ADC_BLOCKERS_TEST_TIMER_ID, ADC_BLOCKERS_TEST_TIMER_MS, blockers_test_escape_timer);
    if (!TEST_CHECK(timer != 0)) {
      goto cleanup;
    }
    if (!TEST_SUCCEEDED(ui_blockers_window_show(owner, events, NULL, 0, &berr), &berr)) {
      goto cleanup;
    }
    KillTimer(owner, ADC_BLOCKERS_TEST_TIMER_ID);
    TEST_CHECK(GetLastActivePopup(owner) == owner);
    TEST_MSG("want the blockers window gone after the key that says nothing");
    TEST_CHECK(IsWindowEnabled(owner) != 0);
    TEST_MSG("want the owner unlocked when the list is done");
  }
  TEST_CASE_(NULL);

cleanup:
  if ((blockers_wnd != NULL) && IsWindow(blockers_wnd)) {
    DestroyWindow(blockers_wnd);
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
  veto_event_destroy_list(&events);
}

/** @brief The dpis the checks of the geometry walk */
static UINT const geometry_dpi_list[] = {96, 120, 144, 192};
#define GEOMETRY_DPI_COUNT ((int)(sizeof(geometry_dpi_list) / sizeof(geometry_dpi_list[0])))

/**
 * @brief The size of a difference, unsigned
 *
 * @param v the difference of two values
 * @return the size it stands for
 */
static int geometry_abs(int const v) { return (v < 0) ? -v : v; }

/**
 * @brief The geometry of the blockers window these checks walk
 *
 * @param dpi the dpi to draw for
 * @param extra_width how much wider than the default the window is
 * @param extra_height how much taller than the default the window is
 * @return the geometry to check
 */
static struct blockers_layout geometry_compute(UINT const dpi, int const extra_width, int const extra_height) {
  struct blockers_layout g;
  blockers_compute(layout_scale(ADC_BLOCKERS_W, dpi) + extra_width, layout_scale(ADC_BLOCKERS_H, dpi) + extra_height, dpi, &g);
  return g;
}

/**
 * @brief The columns of the list fit into its width
 *
 * The sum of the column widths must never exceed the usable list width, that is what
 * produced a horizontal scroll bar at every dpi.
 */
static void test_columns_fit(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    int d = 0;
    for (d = -200; d <= 400; d += 100) {
      struct blockers_layout const g = geometry_compute(geometry_dpi_list[i], d, 0);
      int sum = 0;
      int k = 0;
      int const usable = (g.list.right - g.list.left) - layout_scale(ADC_BLOCKERS_SCROLL_W, geometry_dpi_list[i]);
      for (k = 0; k < ADC_BLOCKERS_COLUMN_COUNT; k++) {
        sum += g.column[k];
        TEST_CHECK_(g.column[k] >= 0, "dpi %u column %d is %d", (unsigned)geometry_dpi_list[i], k, g.column[k]);
      }
      TEST_CHECK_(sum == usable, "dpi %u width %d: columns add to %d, usable is %d", (unsigned)geometry_dpi_list[i], d, sum, usable);
    }
  }
}

/**
 * @brief The proportions of the columns must not drift with the dpi
 */
static void test_column_proportions(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    struct blockers_layout const g = geometry_compute(geometry_dpi_list[i], 0, 0);
    int const usable = (g.list.right - g.list.left) - layout_scale(ADC_BLOCKERS_SCROLL_W, geometry_dpi_list[i]);
    TEST_CHECK_(geometry_abs(g.column[0] - MulDiv(usable, blockers_column_share[0], ADC_BLOCKERS_COLUMN_TOTAL)) <= 1,
                "dpi %u first column",
                (unsigned)geometry_dpi_list[i]);
    TEST_CHECK_(geometry_abs(g.column[1] - MulDiv(usable, blockers_column_share[1], ADC_BLOCKERS_COLUMN_TOTAL)) <= 1,
                "dpi %u second column",
                (unsigned)geometry_dpi_list[i]);
    TEST_CHECK_(g.column[1] > g.column[2],
                "dpi %u: the process column %d is not wider than the device column %d",
                (unsigned)geometry_dpi_list[i],
                g.column[1],
                g.column[2]);
  }
}

/**
 * @brief The height of the header is a layout value
 *
 * comctl32 grows the header with the font only.
 */
static void test_header_scales(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    struct blockers_layout const g = geometry_compute(geometry_dpi_list[i], 0, 0);
    TEST_CHECK_(g.header_height == layout_scale(ADC_BLOCKERS_HEADER_H, geometry_dpi_list[i]),
                "dpi %u header %d",
                (unsigned)geometry_dpi_list[i],
                g.header_height);
  }
}

/**
 * @brief The sentence that names the outcome sits above the list
 *
 * The reader reads why the close did not go through before the records, so the label is
 * at the top of the window and the list starts a gap under it, both inside the margins.
 */
static void test_blockers_label_sits_above_the_list(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    struct blockers_layout const g = geometry_compute(geometry_dpi_list[i], 0, 0);
    int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, geometry_dpi_list[i]);
    int const label_h = layout_scale(ADC_BLOCKERS_LABEL_H, geometry_dpi_list[i]);

    TEST_CHECK_(g.label.top == layout_scale(ADC_LAYOUT_MARGIN_TOP, geometry_dpi_list[i]),
                "dpi %u want label top %ld, got %ld",
                (unsigned)geometry_dpi_list[i],
                (long)layout_scale(ADC_LAYOUT_MARGIN_TOP, geometry_dpi_list[i]),
                (long)g.label.top);
    TEST_CHECK_(g.label.left == margin, "dpi %u want label left %d, got %ld", (unsigned)geometry_dpi_list[i], margin, (long)g.label.left);
    TEST_CHECK_(g.label.bottom - g.label.top == label_h,
                "dpi %u want label height %d, got %ld",
                (unsigned)geometry_dpi_list[i],
                label_h,
                (long)(g.label.bottom - g.label.top));
    TEST_CHECK_(g.list.top == g.label.bottom + layout_scale(ADC_LAYOUT_GAP, geometry_dpi_list[i]),
                "dpi %u want list top %ld, got %ld",
                (unsigned)geometry_dpi_list[i],
                (long)(g.label.bottom + layout_scale(ADC_LAYOUT_GAP, geometry_dpi_list[i])),
                (long)g.list.top);
  }
}

/**
 * @brief The extra height of a window goes to the list
 *
 * The band stays anchored to the bottom edge and the list keeps room for its rows.
 */
static void test_extra_height_goes_to_the_list(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    struct blockers_layout const base = geometry_compute(geometry_dpi_list[i], 0, 0);
    struct blockers_layout const tall = geometry_compute(geometry_dpi_list[i], 0, 200);
    int const base_list = base.list.bottom - base.list.top;
    int const tall_list = tall.list.bottom - tall.list.top;
    TEST_CHECK_(tall_list - base_list >= 198, "dpi %u: list grew by %d of 200", (unsigned)geometry_dpi_list[i], tall_list - base_list);
    TEST_CHECK_(geometry_abs((tall.ok.bottom - tall.list.bottom) - (base.ok.bottom - base.list.bottom)) <= 2,
                "dpi %u: the button drifted away from the bottom",
                (unsigned)geometry_dpi_list[i]);
  }
}

/**
 * @brief The blockers window holds the list above the button, inside its margins
 *
 * The veto records are shown in a window of their own, which is resizable: the list fills
 * the window and the button sits in the corner at the bottom right.  The whole geometry
 * comes out of the same constants as the other windows.
 */
static void test_blockers_window_stacks(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    UINT const dpi = geometry_dpi_list[i];
    int const width = layout_scale(ADC_BLOCKERS_W, dpi);
    int const height = layout_scale(ADC_BLOCKERS_H, dpi);
    int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
    struct blockers_layout const g = geometry_compute(dpi, 0, 0);

    TEST_CHECK_(g.label.top == layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi),
                "dpi %u: label top %d, margin %d",
                (unsigned)dpi,
                (int)g.label.top,
                layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi));
    TEST_CHECK_(g.list.top == g.label.bottom + layout_scale(ADC_LAYOUT_GAP, dpi),
                "dpi %u: list top %d, label bottom %d",
                (unsigned)dpi,
                (int)g.list.top,
                (int)g.label.bottom);
    TEST_CHECK(g.label.left == margin);
    TEST_CHECK(g.list.left == margin);
    TEST_CHECK(g.list.right == width - margin);
    TEST_CHECK(g.ok.top == g.list.bottom + layout_scale(ADC_LAYOUT_GAP, dpi));
    TEST_CHECK(g.ok.bottom == g.ok.top + layout_scale(ADC_BLOCKERS_OK_H, dpi));
    TEST_CHECK(g.ok.right == width - margin);
    TEST_CHECK_(g.ok.left < g.ok.right, "dpi %u: the button has no width", (unsigned)dpi);
    TEST_CHECK(g.ok.bottom == height - layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, dpi));
    TEST_CHECK(g.list.bottom > g.list.top);
  }
}

/**
 * @brief The height of the blockers window is the sum of its parts
 *
 * The window height is the top margin, the list, the gap, the button and the bottom
 * margin; the list is the one that takes what is left.  The constant ADC_BLOCKERS_H has
 * to cover the fixed parts at the smallest useful list, which is what the parts sum
 * checks.
 */
static void test_blockers_height_fits_its_parts(void) {
  int const parts = layout_scale(ADC_LAYOUT_MARGIN_TOP, 96) + layout_scale(ADC_BLOCKERS_LABEL_H, 96) + layout_scale(ADC_LAYOUT_GAP, 96) +
                    layout_scale(ADC_BLOCKERS_LABEL_H, 96) + layout_scale(ADC_LAYOUT_GAP, 96) + layout_scale(ADC_BLOCKERS_OK_H, 96) +
                    layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, 96);
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    UINT const dpi = geometry_dpi_list[i];
    int const height = layout_scale(ADC_BLOCKERS_H, dpi);
    struct blockers_layout const g = geometry_compute(dpi, 0, 0);

    TEST_CHECK_(layout_scale(ADC_BLOCKERS_H, dpi) >= parts * (int)dpi / 96,
                "dpi %u: the window opens %d tall, its parts need %d",
                (unsigned)dpi,
                (int)layout_scale(ADC_BLOCKERS_H, dpi),
                parts * (int)dpi / 96);
    TEST_CHECK(g.ok.bottom - g.label.top ==
               height - (layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi) + layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, dpi)));
    TEST_CHECK(g.ok.top == g.list.bottom + layout_scale(ADC_LAYOUT_GAP, dpi));
    TEST_CHECK(g.ok.bottom == g.ok.top + layout_scale(ADC_BLOCKERS_OK_H, dpi));
    TEST_CHECK(g.label.top == layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi));
    TEST_CHECK(g.list.bottom - g.list.top >= ADC_BLOCKERS_LABEL_H);
  }
}

/**
 * @brief The blockers window follows the size it is given
 *
 * The window is resizable and the layout has to follow every size: a larger window gives
 * the extra room to the list, the button stays anchored at the corner.
 */
static void test_blockers_follow_the_size(void) {
  int i = 0;
  for (i = 0; i < GEOMETRY_DPI_COUNT; i++) {
    UINT const dpi = geometry_dpi_list[i];
    struct blockers_layout const base = geometry_compute(dpi, 0, 0);
    struct blockers_layout const big = geometry_compute(dpi, 200, 160);
    int const base_list = base.list.bottom - base.list.top;
    int const big_list = big.list.bottom - big.list.top;
    TEST_CHECK_(big_list - base_list >= 158, "dpi %u: list grew by %d of 160", (unsigned)dpi, big_list - base_list);
    TEST_CHECK_(big.ok.right == base.ok.right + 200, "dpi %u: the button does not follow the right edge", (unsigned)dpi);
    TEST_CHECK_(
        geometry_abs((big.ok.bottom - base.ok.bottom) - 160) <= 2, "dpi %u: the button does not follow the bottom edge", (unsigned)dpi);
  }
}

TEST_LIST = {
    {"columns_fit", test_columns_fit},
    {"column_proportions", test_column_proportions},
    {"header_scales", test_header_scales},
    {"blockers_label_sits_above_the_list", test_blockers_label_sits_above_the_list},
    {"extra_height_goes_to_the_list", test_extra_height_goes_to_the_list},
    {"blockers_window_stacks", test_blockers_window_stacks},
    {"blockers_height_fits_its_parts", test_blockers_height_fits_its_parts},
    {"blockers_follow_the_size", test_blockers_follow_the_size},
    {"blockers_window_shows_the_processes", test_blockers_window_shows_the_processes},
    {NULL, NULL},
};
