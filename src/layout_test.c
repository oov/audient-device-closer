#include <windows.h>

#include <string.h>

#include <ovtest.h>

#include "layout.h"

/**
 * The measuring every window shares has to hold for every dpi.
 *
 * The geometry of a window is checked in the test of that window: it is worked out there,
 * from the sizes that window places.  What is left here is the scaling and the frame that
 * every window is made with.
 */

/** @brief The dpis the checks walk */
static UINT const dpi_list[] = {96, 120, 144, 192};
#define DPI_COUNT ((int)(sizeof(dpi_list) / sizeof(dpi_list[0])))

/** @brief The pixels the scaling of one part and the scaling of a whole can differ by */
static int const scaling_slack = 1;

/**
 * @brief The size of a rectangle, unsigned
 *
 * @param v the difference of two edges
 * @return the size it stands for
 */
static int abs_of(int const v) { return (v < 0) ? -v : v; }

/** @brief The style the main window is made with, the window whose size is worked out here */
static DWORD const main_window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

/**
 * @brief The point size of the font turns into a height that grows with the dpi
 *
 * The dpi of a window decides how tall a caption is drawn, so a font height that stayed the
 * same would make the text of a 192dpi monitor as small as the one of a 96dpi monitor.
 */
static void test_font_height_follows_the_dpi(void) {
  int i = 0;
  for (i = 1; i < DPI_COUNT; i++) {
    UINT const small = dpi_list[i - 1];
    UINT const large = dpi_list[i];
    int const small_h = -layout_font_height(small);
    int const large_h = -layout_font_height(large);

    TEST_CHECK_(
        large_h > small_h, "%u: the font height %d is not taller than %d at %u", (unsigned)large, large_h, small_h, (unsigned)small);
    TEST_CHECK_(abs_of(large_h * (int)small / (int)large - small_h) <= scaling_slack,
                "%u: the height %d does not follow %u from %d",
                (unsigned)large,
                large_h,
                (unsigned)small,
                small_h);
  }
}

/**
 * @brief A dpi of zero falls back to the base the whole program is measured in
 *
 * A window that could not be asked for its dpi still has to be placed, and the base of 96
 * is the value every logical unit stands for.
 */
static void test_zero_dpi_is_the_base(void) {
  TEST_CHECK(layout_scale(100, 0) == layout_scale(100, USER_DEFAULT_SCREEN_DPI));
  TEST_CHECK(layout_font_height(0) == layout_font_height(USER_DEFAULT_SCREEN_DPI));
}

/**
 * @brief The outer size of the window is its client plus the frame of its style
 */
static void test_window_rect_carries_the_frame(void) {
  int i = 0;
  int previous_w = 0;
  int previous_h = 0;

  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    int const client_w = layout_scale(400, dpi);
    int const client_h = layout_scale(300, dpi);
    RECT outer;
    RECT want;

    layout_window_rect(client_w, client_h, dpi, main_window_style, 0, &outer);
    SetRect(&want, 0, 0, client_w, client_h);
    TEST_CHECK(AdjustWindowRectExForDpi(&want, main_window_style, FALSE, 0, dpi));
    TEST_CHECK_(memcmp(&outer, &want, sizeof(outer)) == 0,
                "dpi %u: want %dx%d, got %dx%d",
                (unsigned)dpi,
                (int)(want.right - want.left),
                (int)(want.bottom - want.top),
                (int)(outer.right - outer.left),
                (int)(outer.bottom - outer.top));
    TEST_CHECK_(outer.right - outer.left >= client_w, "dpi %u: the frame eats into the client", (unsigned)dpi);
    TEST_CHECK_(outer.bottom - outer.top >= client_h, "dpi %u: the frame eats into the client", (unsigned)dpi);
    if (i > 0) {
      TEST_CHECK_(outer.right - outer.left >= previous_w, "dpi %u: the window shrinks as the dpi grows", (unsigned)dpi);
      TEST_CHECK_(outer.bottom - outer.top >= previous_h, "dpi %u: the window shrinks as the dpi grows", (unsigned)dpi);
    }
    previous_w = outer.right - outer.left;
    previous_h = outer.bottom - outer.top;
  }
}

/**
 * @brief A rectangle from its left, top and size
 *
 * The geometry of every window is built from these, so the one place that adds an origin
 * and a size has a check of its own.
 */
static void test_rect_from_its_size(void) {
  RECT const r = layout_rect(10, 20, 30, 40);
  TEST_CHECK(r.left == 10);
  TEST_CHECK(r.top == 20);
  TEST_CHECK(r.right == 40);
  TEST_CHECK(r.bottom == 60);
  {
    RECT const none = layout_rect(-5, -7, 0, 0);
    TEST_CHECK(none.left == none.right);
    TEST_CHECK(none.top == none.bottom);
  }
}

TEST_LIST = {
    {"font_height_follows_the_dpi", test_font_height_follows_the_dpi},
    {"zero_dpi_is_the_base", test_zero_dpi_is_the_base},
    {"window_rect_carries_the_frame", test_window_rect_carries_the_frame},
    {"rect_from_its_size", test_rect_from_its_size},
    {NULL, NULL},
};
