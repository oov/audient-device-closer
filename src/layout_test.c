#include <windows.h>

#include <string.h>

#include <ovtest.h>

#include "layout.h"

/**
 * The geometry has to hold for every dpi and every window size.  Each check below
 * reproduces a defect that this application actually had, so that it cannot come back.
 */

/** @brief The dpis the checks walk */
static UINT const dpi_list[] = {96, 120, 144, 192};
#define DPI_COUNT ((int)(sizeof(dpi_list) / sizeof(dpi_list[0])))

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
  r.right = layout_scale(ADC_LAYOUT_WINDOW_W, dpi) + extra_width;
  r.bottom = layout_scale(ADC_LAYOUT_WINDOW_H, dpi) + extra_height;
  return r;
}

/**
 * @brief The size of a rectangle, unsigned
 *
 * @param v the difference of two edges
 * @return the size it stands for
 */
static int abs_of(int const v) { return (v < 0) ? -v : v; }

/** @brief The pixels the scaling of one part and the scaling of a whole can differ by */
static int const scaling_slack = 1;

/**
 * @brief The captions the checks hand to the layout
 *
 * The band of the two groups.  These are the widest captions the application has, measured
 * at 96dpi by the tests that check the boxes, so the geometry is checked with room to
 * spare.
 */
static int const settings_caption_logical = 90;
static int const install_caption_logical = 60;
/**
 * @brief The geometry of the blockers window these checks walk
 *
 * @param dpi the dpi to draw for
 * @param extra_width how much wider than the default the window is
 * @param extra_height how much taller than the default the window is
 * @return the geometry to check
 */
static struct blockers_layout blockers_compute(UINT const dpi, int const extra_width, int const extra_height) {
  struct blockers_layout g;
  layout_blockers_compute(
      layout_scale(ADC_LAYOUT_BLOCKERS_W, dpi) + extra_width, layout_scale(ADC_LAYOUT_BLOCKERS_H, dpi) + extra_height, dpi, &g);
  return g;
}

/**
 * @brief The geometry of the window these checks walk
 *
 * @param dpi the dpi to draw for
 * @param extra_width how much wider than the default the window is
 * @param extra_height how much taller than the default the window is
 * @param caption_width the room the group captions take
 * @return the geometry to check
 */
static struct layout compute(UINT const dpi, int const extra_width, int const extra_height, int const caption_width) {
  struct layout g;
  RECT const c = client_of(dpi, extra_width, extra_height);
  int const settings_w = layout_scale(ADC_LAYOUT_SETTINGS_MIN_W, dpi);
  layout_compute(
      &c, dpi, settings_w, caption_width, layout_scale(install_caption_logical, dpi), layout_scale(ADC_LAYOUT_CAPTION_H, dpi), &g);
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
  return layout_scale(ADC_LAYOUT_MIN_CLIENT_EXTRA_W, dpi) + layout_scale(ADC_LAYOUT_SETTINGS_MIN_W, dpi);
}

/**
 * @brief Every frame of the window, for the tests that walk them all
 *
 * @param g the geometry to read
 * @param out receives the rectangles
 * @param n receives how many there are
 */
static void frames_of(struct layout const *const g, RECT const **const out, size_t *const n) {
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
static void captions_of(struct layout const *const g, RECT const **const out, size_t *const n) {
  out[(*n)++] = &g->settings_caption;
  out[(*n)++] = &g->install_caption;
}

/**
 * @brief The top line of a frame runs through the middle of its caption
 *
 * A group box of the system puts the text at the top of its area and
 * draws the top line at half the height of that text, so the line
 * balances the caption.  The frames here follow the same convention:
 * the caption is asked for its height, and the line goes through its
 * middle, at every dpi and for an even and an odd height alike.  Both
 * groups are checked: they share the code that places a caption on a
 * frame.
 */
static void test_caption_line_balance(void) {
  int i = 0;
  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    int const reserved = layout_scale(ADC_LAYOUT_CAPTION_H, dpi);
    int const heights[2] = {reserved, reserved - 1};
    int k = 0;
    for (k = 0; k < 2; k++) {
      RECT const c = client_of(dpi, 0, 0);
      struct layout g;
      int const caption_h = heights[k];
      int const group_h = layout_scale(ADC_LAYOUT_GROUP_H, dpi);

      layout_compute(&c,
                     dpi,
                     layout_scale(ADC_LAYOUT_SETTINGS_MIN_W, dpi),
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
  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    struct layout const g = compute(dpi, 0, 0, layout_scale(settings_caption_logical, dpi));
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
      TEST_CHECK_(abs_of(caption_mid - frame_top) <= tolerance,
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
  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    struct layout const g = compute(dpi, 0, 0, layout_scale(settings_caption_logical, dpi));
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
  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    struct layout const g = compute(dpi, 0, 0, layout_scale(settings_caption_logical, dpi));
    int const inset = layout_scale(ADC_LAYOUT_GROUP_INSET, dpi);
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
  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    RECT c;
    struct layout g;
    SetRect(&c, 0, 0, min_client_width(dpi), layout_scale(ADC_LAYOUT_WINDOW_H, dpi));
    layout_compute(&c,
                   dpi,
                   layout_scale(ADC_LAYOUT_SETTINGS_MIN_W, dpi),
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
 * @brief The columns of the list fit into its width
 *
 * The sum of the column widths must never exceed the usable list width, that is what
 * produced a horizontal scroll bar at every dpi.  The list lives in the blockers window
 * now, so the shares are checked through layout_blockers_compute().
 */
static void test_columns_fit(void) {
  int i = 0;
  for (i = 0; i < DPI_COUNT; i++) {
    int d = 0;
    for (d = -200; d <= 400; d += 100) {
      struct blockers_layout const g = blockers_compute(dpi_list[i], d, 0);
      int sum = 0;
      int k = 0;
      int const usable = (g.list.right - g.list.left) - layout_scale(ADC_LAYOUT_SCROLL_W, dpi_list[i]);
      for (k = 0; k < ADC_LAYOUT_COLUMN_COUNT; k++) {
        sum += g.column[k];
        TEST_CHECK_(g.column[k] >= 0, "dpi %u column %d is %d", (unsigned)dpi_list[i], k, g.column[k]);
      }
      TEST_CHECK_(sum == usable, "dpi %u width %d: columns add to %d, usable is %d", (unsigned)dpi_list[i], d, sum, usable);
    }
  }
}

/**
 * @brief The proportions of the columns must not drift with the dpi
 */
static void test_column_proportions(void) {
  int i = 0;
  for (i = 0; i < DPI_COUNT; i++) {
    struct blockers_layout const g = blockers_compute(dpi_list[i], 0, 0);
    int const usable = (g.list.right - g.list.left) - layout_scale(ADC_LAYOUT_SCROLL_W, dpi_list[i]);
    TEST_CHECK_(abs_of(g.column[0] - MulDiv(usable, 100, ADC_LAYOUT_COLUMN_TOTAL)) <= 1, "dpi %u first column", (unsigned)dpi_list[i]);
    TEST_CHECK_(abs_of(g.column[1] - MulDiv(usable, 575, ADC_LAYOUT_COLUMN_TOTAL)) <= 1, "dpi %u second column", (unsigned)dpi_list[i]);
    TEST_CHECK_(g.column[1] > g.column[2],
                "dpi %u: the process column %d is not wider than the device column %d",
                (unsigned)dpi_list[i],
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
  for (i = 0; i < DPI_COUNT; i++) {
    struct blockers_layout const g = blockers_compute(dpi_list[i], 0, 0);
    TEST_CHECK_(
        g.header_height == layout_scale(ADC_LAYOUT_HEADER_H, dpi_list[i]), "dpi %u header %d", (unsigned)dpi_list[i], g.header_height);
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
  for (i = 0; i < DPI_COUNT; i++) {
    struct blockers_layout const g = blockers_compute(dpi_list[i], 0, 0);
    int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi_list[i]);
    int const label_h = layout_scale(ADC_LAYOUT_BLOCKERS_LABEL_H, dpi_list[i]);

    TEST_CHECK_(g.label.top == layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi_list[i]),
                "dpi %u want label top %ld, got %ld",
                (unsigned)dpi_list[i],
                (long)layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi_list[i]),
                (long)g.label.top);
    TEST_CHECK_(g.label.left == margin, "dpi %u want label left %d, got %ld", (unsigned)dpi_list[i], margin, (long)g.label.left);
    TEST_CHECK_(g.label.bottom - g.label.top == label_h,
                "dpi %u want label height %d, got %ld",
                (unsigned)dpi_list[i],
                label_h,
                (long)(g.label.bottom - g.label.top));
    TEST_CHECK_(g.list.top == g.label.bottom + layout_scale(ADC_LAYOUT_GAP, dpi_list[i]),
                "dpi %u want list top %ld, got %ld",
                (unsigned)dpi_list[i],
                (long)(g.label.bottom + layout_scale(ADC_LAYOUT_GAP, dpi_list[i])),
                (long)g.list.top);
  }
}

/**
 * @brief The list grows in proportion to the window
 *
 * Everything scaled twice (the width minus a logical margin, then scaled again) shows up
 * here: the list of a 144dpi window must be 1.5 times as wide as the one of a 96dpi
 * window.
 */
static void test_growth_is_proportional(void) {
  struct layout const small = compute(96, 0, 0, 120);
  struct layout const large = compute(144, 0, 0, 180);
  TEST_CHECK_(abs_of((small.task_remove.right - small.task_remove.left) * 144 / 96 - (large.task_remove.right - large.task_remove.left)) <=
                  2,
              "task button does not scale");
  TEST_CHECK_(abs_of((small.settings_frame.right - small.settings_frame.left) * 144 / 96 -
                     (large.settings_frame.right - large.settings_frame.left)) <= 2,
              "the settings group does not scale");
}

/**
 * @brief The minimum width covers what the group needs
 *
 * The minimum width is a separate number from the group geometry.  If somebody shrinks it
 * below what the group needs, the buttons overflow the frame again, and that happens
 * outside layout_compute(), so it has to be checked against the constants directly.
 */
static void test_minimum_covers_the_group(void) {
  static int const width_parts[] = {ADC_LAYOUT_MARGIN_X,
                                    ADC_LAYOUT_MARGIN_X,
                                    ADC_LAYOUT_SETTINGS_MIN_W,
                                    ADC_LAYOUT_GROUP_GAP,
                                    ADC_LAYOUT_GROUP_INSET,
                                    ADC_LAYOUT_GROUP_INSET,
                                    ADC_LAYOUT_TASK_W};
  static int const box_parts[] = {ADC_LAYOUT_GROUP_INSET, ADC_LAYOUT_CHECK_H, ADC_LAYOUT_GAP, ADC_LAYOUT_CHECK_H, ADC_LAYOUT_GROUP_INSET};
  static int const task_parts[] = {
      ADC_LAYOUT_GROUP_INSET, ADC_LAYOUT_TASK_H, ADC_LAYOUT_TASK_GAP, ADC_LAYOUT_TASK_H, ADC_LAYOUT_GROUP_INSET};
  size_t const boxes_in_the_catalog = 2;
  int const room_of_the_catalog = ADC_LAYOUT_GROUP_INSET + (int)boxes_in_the_catalog * ADC_LAYOUT_CHECK_H +
                                  (int)(boxes_in_the_catalog - 1) * ADC_LAYOUT_GAP + ADC_LAYOUT_GROUP_INSET;
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
  int const taller = (boxes > tasks) ? boxes : tasks;
  TEST_CHECK_((int)ADC_LAYOUT_GROUP_H == taller, "the group height %d, the taller group needs %d", (int)ADC_LAYOUT_GROUP_H, taller);
}

/**
 * @brief Nothing may leave the client area, not even at the smallest allowed window
 */
static void test_inside_the_client(void) {
  int i = 0;
  for (i = 0; i < DPI_COUNT; i++) {
    RECT c;
    struct layout g;
    SetRect(&c, 0, 0, min_client_width(dpi_list[i]), layout_scale(ADC_LAYOUT_WINDOW_H, dpi_list[i]));
    layout_compute(&c,
                   dpi_list[i],
                   layout_scale(ADC_LAYOUT_SETTINGS_MIN_W, dpi_list[i]),
                   layout_scale(settings_caption_logical, dpi_list[i]),
                   layout_scale(install_caption_logical, dpi_list[i]),
                   layout_scale(ADC_LAYOUT_CAPTION_H, dpi_list[i]),
                   &g);
    TEST_CHECK(g.install_frame.bottom <= c.bottom);
    TEST_CHECK(g.settings_frame.bottom <= c.bottom);
    TEST_CHECK(g.task_remove.bottom <= g.install_frame.bottom);
  }
}

/**
 * @brief The check boxes take their places in the stack without pushing anything out of
 *        the client area
 */
static void test_checks_stack(void) {
  int i = 0;
  for (i = 0; i < DPI_COUNT; i++) {
    RECT c;
    struct layout g;
    SetRect(&c, 0, 0, min_client_width(dpi_list[i]), layout_scale(ADC_LAYOUT_WINDOW_H, dpi_list[i]));
    layout_compute(&c,
                   dpi_list[i],
                   layout_scale(ADC_LAYOUT_SETTINGS_MIN_W, dpi_list[i]),
                   layout_scale(settings_caption_logical, dpi_list[i]),
                   layout_scale(install_caption_logical, dpi_list[i]),
                   layout_scale(ADC_LAYOUT_CAPTION_H, dpi_list[i]),
                   &g);
    TEST_CHECK_(g.check_audiodg.bottom > g.check_audiodg.top, "dpi %u: the audiodg check has no height", (unsigned)dpi_list[i]);
    TEST_CHECK_(g.check_mixer.bottom <= g.check_audiodg.top, "dpi %u: the mixer check overlaps the audiodg check", (unsigned)dpi_list[i]);
    TEST_CHECK(g.check_audiodg.left == g.check_mixer.left);
    TEST_CHECK(g.check_mixer.left >= g.settings_frame.left);
    TEST_CHECK(g.check_mixer.right <= g.settings_frame.right);
    TEST_CHECK(g.check_audiodg.bottom <= g.settings_frame.bottom);
    TEST_CHECK(g.check_audiodg.bottom <= c.bottom);
    {
      int const above = g.check_mixer.top - g.settings_frame.top;
      int const below = g.settings_frame.bottom - g.check_audiodg.bottom;
      TEST_CHECK_(
          abs_of(above - below) <= 1, "dpi %u: the boxes sit %d from the top and %d from the bottom", (unsigned)dpi_list[i], above, below);
    }
  }
}

/**
 * @brief The progress dialog puts the bar under the label, inside its margins
 */
static void test_progress_dialog_stacks(void) {
  int i = 0;
  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    struct progress_layout g;
    int const width = layout_scale(ADC_LAYOUT_PROGRESS_W, dpi);
    int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
    layout_progress_compute(width, dpi, &g);
    TEST_CHECK(g.label.top == layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi));
    TEST_CHECK_(g.label.left == margin, "dpi %u: label left %d, margin %d", (unsigned)dpi, (int)g.label.left, margin);
    TEST_CHECK(g.label.right == width - margin);
    TEST_CHECK(g.bar.top == g.label.bottom + layout_scale(ADC_LAYOUT_GAP, dpi));
    TEST_CHECK(g.bar.bottom == g.bar.top + layout_scale(ADC_LAYOUT_PROGRESS_BAR_H, dpi));
    TEST_CHECK(g.bar.left == g.label.left);
    TEST_CHECK(g.bar.right == g.label.right);
    TEST_CHECK(g.cancel.top == g.bar.bottom + layout_scale(ADC_LAYOUT_GAP, dpi));
    TEST_CHECK(g.cancel.bottom == g.cancel.top + layout_scale(ADC_LAYOUT_PROGRESS_CANCEL_H, dpi));
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

  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    struct progress_layout wide;
    struct progress_layout narrow;

    layout_progress_compute(layout_scale(ADC_LAYOUT_PROGRESS_W, dpi), dpi, &wide);
    layout_progress_compute(layout_scale(ADC_LAYOUT_PROGRESS_CANCEL_W, dpi), dpi, &narrow);
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
  TEST_CHECK_(ADC_LAYOUT_PROGRESS_W >= ADC_LAYOUT_PROGRESS_CANCEL_W + 2 * ADC_LAYOUT_MARGIN_X,
              "the width %d, the button and its margins need %d",
              (int)ADC_LAYOUT_PROGRESS_W,
              (int)(ADC_LAYOUT_PROGRESS_CANCEL_W + 2 * ADC_LAYOUT_MARGIN_X));
}

/**
 * @brief The geometry of the progress dialog scales with the dpi
 */
static void test_progress_dialog_scales(void) {
  struct progress_layout small;
  struct progress_layout large;
  layout_progress_compute(layout_scale(ADC_LAYOUT_PROGRESS_W, 96), 96, &small);
  layout_progress_compute(layout_scale(ADC_LAYOUT_PROGRESS_W, 144), 144, &large);
  TEST_CHECK(abs_of((small.bar.bottom - small.bar.top) * 144 / 96 - (large.bar.bottom - large.bar.top)) <= 2);
  TEST_CHECK(abs_of(small.needed_height * 144 / 96 - large.needed_height) <= scaling_slack);
}

/**
 * @brief The extra height of a window goes to the list
 *
 * The band stays anchored to the bottom edge and the list keeps room for its rows.
 */
static void test_extra_height_goes_to_the_list(void) {
  int i = 0;
  for (i = 0; i < DPI_COUNT; i++) {
    struct blockers_layout const base = blockers_compute(dpi_list[i], 0, 0);
    struct blockers_layout const tall = blockers_compute(dpi_list[i], 0, 200);
    int const base_list = base.list.bottom - base.list.top;
    int const tall_list = tall.list.bottom - tall.list.top;
    TEST_CHECK_(tall_list - base_list >= 198, "dpi %u: list grew by %d of 200", (unsigned)dpi_list[i], tall_list - base_list);
    TEST_CHECK_(abs_of((tall.ok.bottom - tall.list.bottom) - (base.ok.bottom - base.list.bottom)) <= 2,
                "dpi %u: the button drifted away from the bottom",
                (unsigned)dpi_list[i]);
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
  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    int const width = layout_scale(ADC_LAYOUT_BLOCKERS_W, dpi);
    int const height = layout_scale(ADC_LAYOUT_BLOCKERS_H, dpi);
    int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
    struct blockers_layout const g = blockers_compute(dpi, 0, 0);

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
    TEST_CHECK(g.ok.bottom == g.ok.top + layout_scale(ADC_LAYOUT_BLOCKERS_OK_H, dpi));
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
 * margin; the list is the one that takes what is left, the way the list of the old main
 * window did.  The constant ADC_LAYOUT_BLOCKERS_H has to cover the fixed parts at the
 * smallest useful list, which is what the parts sum checks.
 */
static void test_blockers_height_fits_its_parts(void) {
  int const parts = ADC_LAYOUT_MARGIN_TOP + ADC_LAYOUT_BLOCKERS_LABEL_H + ADC_LAYOUT_GAP + 2 * ADC_LAYOUT_CHECK_H + ADC_LAYOUT_GAP +
                    ADC_LAYOUT_BLOCKERS_OK_H + ADC_LAYOUT_MARGIN_BOTTOM;
  int i = 0;
  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    int const height = layout_scale(ADC_LAYOUT_BLOCKERS_H, dpi);
    struct blockers_layout const g = blockers_compute(dpi, 0, 0);

    TEST_CHECK(layout_scale(ADC_LAYOUT_BLOCKERS_H, dpi) >= layout_scale(parts, dpi));
    TEST_CHECK(g.ok.bottom - g.label.top == height - layout_scale(ADC_LAYOUT_MARGIN_TOP + ADC_LAYOUT_MARGIN_BOTTOM, dpi));
    TEST_CHECK(g.ok.top == g.list.bottom + layout_scale(ADC_LAYOUT_GAP, dpi));
    TEST_CHECK(g.ok.bottom == g.ok.top + layout_scale(ADC_LAYOUT_BLOCKERS_OK_H, dpi));
    TEST_CHECK(g.label.top == layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi));
    TEST_CHECK(g.list.bottom - g.list.top >= 2 * layout_scale(ADC_LAYOUT_CHECK_H, dpi));
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
  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    struct blockers_layout const base = blockers_compute(dpi, 0, 0);
    struct blockers_layout const big = blockers_compute(dpi, 200, 160);
    int const base_list = base.list.bottom - base.list.top;
    int const big_list = big.list.bottom - big.list.top;
    TEST_CHECK_(big_list - base_list >= 158, "dpi %u: list grew by %d of 160", (unsigned)dpi, big_list - base_list);
    TEST_CHECK_(big.ok.right == base.ok.right + 200, "dpi %u: the button does not follow the right edge", (unsigned)dpi);
    TEST_CHECK_(abs_of((big.ok.bottom - base.ok.bottom) - 160) <= 2, "dpi %u: the button does not follow the bottom edge", (unsigned)dpi);
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
    struct layout g;
    RECT const c = client_of((UINT)dpi, 400, 0);
    int const settings_w = widths[i];
    int const inset = layout_scale(ADC_LAYOUT_GROUP_INSET, (UINT)dpi);

    layout_compute(&c,
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
    struct layout g;
    RECT const c = client_of((UINT)dpi, 0, 0);
    int const expected = widths[i] + layout_scale(ADC_LAYOUT_MIN_CLIENT_EXTRA_W, (UINT)dpi);

    layout_compute(&c, (UINT)dpi, widths[i], 0, 0, 0, &g);
    TEST_CHECK_(
        (int)g.min_client_width == expected, "%d: want %d (the group and the rest), got %d", widths[i], expected, (int)g.min_client_width);
    {
      RECT narrow;
      struct layout at_limit;
      SetRect(&narrow, 0, 0, g.min_client_width, layout_scale(ADC_LAYOUT_WINDOW_H, (UINT)dpi));
      layout_compute(&narrow, (UINT)dpi, widths[i], 0, 0, 0, &at_limit);
      TEST_CHECK(at_limit.settings_frame.left >= 0);
      TEST_CHECK(at_limit.install_frame.right <= narrow.right);
      TEST_CHECK(at_limit.task_install.right <= at_limit.install_frame.right);
    }
  }
}

/** @brief The style the main window is made with, the window whose size is worked out here */
static DWORD const main_window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

/**
 * @brief The outer size of the window is its client plus the frame of its style
 */
static void test_window_rect_carries_the_frame(void) {
  int i = 0;
  int previous_w = 0;
  int previous_h = 0;

  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    int const client_w = layout_scale(ADC_LAYOUT_WINDOW_W, dpi);
    int const client_h = layout_scale(ADC_LAYOUT_WINDOW_H, dpi);
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
 * @brief The window is no bigger than the parts it holds
 */
static void test_window_is_no_bigger_than_its_parts(void) {
  int i = 0;

  for (i = 0; i < DPI_COUNT; i++) {
    UINT const dpi = dpi_list[i];
    RECT client;
    struct layout g;
    int const section = layout_scale(ADC_LAYOUT_SECTION_GAP, dpi);
    int room = 0;

    SetRect(&client, 0, 0, layout_scale(ADC_LAYOUT_WINDOW_W, dpi), layout_scale(ADC_LAYOUT_WINDOW_H, dpi));
    layout_compute(&client,
                   dpi,
                   layout_scale(ADC_LAYOUT_SETTINGS_MIN_W, dpi),
                   layout_scale(settings_caption_logical, dpi),
                   layout_scale(install_caption_logical, dpi),
                   layout_scale(ADC_LAYOUT_CAPTION_H, dpi),
                   &g);
    /* the parts are scaled one by one, the height of the window as a whole */
    room = g.settings_caption.top - g.button.bottom;
    TEST_CHECK_(abs_of(room - section) <= scaling_slack,
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
    {"caption_line_balance", test_caption_line_balance},
    {"caption_rides_the_frame", test_caption_rides_the_frame},
    {"groups_share_the_band", test_groups_share_the_band},
    {"task_buttons_stack_in_group", test_task_buttons_stack_in_group},
    {"buttons_inside_the_frame", test_buttons_inside_the_frame},
    {"columns_fit", test_columns_fit},
    {"column_proportions", test_column_proportions},
    {"header_scales", test_header_scales},
    {"blockers_label_sits_above_the_list", test_blockers_label_sits_above_the_list},
    {"growth_is_proportional", test_growth_is_proportional},
    {"minimum_covers_the_group", test_minimum_covers_the_group},
    {"inside_the_client", test_inside_the_client},
    {"checks_stack", test_checks_stack},
    {"extra_height_goes_to_the_list", test_extra_height_goes_to_the_list},
    {"blockers_window_stacks", test_blockers_window_stacks},
    {"blockers_height_fits_its_parts", test_blockers_height_fits_its_parts},
    {"blockers_follow_the_size", test_blockers_follow_the_size},
    {"group_follows_its_captions", test_group_follows_its_captions},
    {"minimum_follows_the_group", test_minimum_follows_the_group},
    {"progress_dialog_stacks", test_progress_dialog_stacks},
    {"progress_dialog_height_fits_its_parts", test_progress_dialog_height_fits_its_parts},
    {"progress_dialog_holds_its_button", test_progress_dialog_holds_its_button},
    {"progress_dialog_scales", test_progress_dialog_scales},
    {"window_rect_carries_the_frame", test_window_rect_carries_the_frame},
    {"window_is_no_bigger_than_its_parts", test_window_is_no_bigger_than_its_parts},
    {NULL, NULL},
};
