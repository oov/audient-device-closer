#include <ovtest.h>

#include "options.h"

/**
 * @brief The defaults a plain start of this program runs with
 */
static void test_defaults(void) {
  struct options o;
  options_default(&o);
  TEST_CHECK(o.auto_run == false);
  TEST_CHECK(o.close_mixer == true);
  TEST_CHECK(o.close_audiodg == false);
}

/**
 * @brief A command line without switches keeps the defaults
 */
static void test_parse_empty(void) {
  struct options o;
  struct ov_error err = {0};
  TEST_CHECK(options_parse(L"", &o, &err) == true);
  TEST_CHECK(o.auto_run == false);
  TEST_CHECK(o.close_mixer == true);
  TEST_CHECK(o.close_audiodg == false);
}

/**
 * @brief Every switch of the command line is read
 */
static void test_parse_all_on(void) {
  struct options o;
  struct ov_error err = {0};
  TEST_CHECK(options_parse(L"-auto-run -close-mixer -close-audiodg", &o, &err) == true);
  TEST_CHECK(o.auto_run == true);
  TEST_CHECK(o.close_mixer == true);
  TEST_CHECK(o.close_audiodg == true);
}

/**
 * @brief A switch can be turned off again
 */
static void test_parse_negations(void) {
  struct options o;
  struct ov_error err = {0};
  TEST_CHECK(options_parse(L"-auto-run -no-close-mixer -no-close-audiodg", &o, &err) == true);
  TEST_CHECK(o.auto_run == true);
  TEST_CHECK(o.close_mixer == false);
  TEST_CHECK(o.close_audiodg == false);
}

/**
 * @brief A quoted token may carry spaces
 */
static void test_parse_quoted_and_extra_spaces(void) {
  struct options o;
  struct ov_error err = {0};
  TEST_CHECK(options_parse(L"   \"-auto-run\"    -no-close-audiodg  ", &o, &err) == true);
  TEST_CHECK(o.auto_run == true);
  TEST_CHECK(o.close_audiodg == false);
}

/**
 * @brief Unknown tokens must not break the launch of an already registered task
 */
static void test_parse_unknown_ignored(void) {
  struct options o;
  struct ov_error err = {0};
  TEST_CHECK(options_parse(L"-auto-run -what-is-this", &o, &err) == true);
  TEST_CHECK(o.auto_run == true);
}

/**
 * @brief A switch that is spelled out twice keeps its last answer
 */
static void test_parse_last_wins(void) {
  struct options o;
  struct ov_error err = {0};
  TEST_CHECK(options_parse(L"-no-close-mixer -close-mixer", &o, &err) == true);
  TEST_CHECK(o.close_mixer == true);
  TEST_CHECK(options_parse(L"-close-audiodg -no-close-audiodg", &o, &err) == true);
  TEST_CHECK(o.close_audiodg == false);
}

/**
 * @brief The run itself answers whether its window closes
 *
 * The box that asked for the auto-close is gone from the window, so the run itself has to
 * answer the question: a scheduled run closes its own window when the device was released,
 * a run a person started keeps the result in front of them whatever came of it.  The two
 * switches of the removed box stay accepted, an already registered task carries them and
 * must keep working.
 */
static void test_auto_close_follows_the_mode_of_the_run(void) {
  struct options o;
  struct ov_error err = {0};

  TEST_CHECK(options_parse(L"-auto-run", &o, &err) == true);
  TEST_CHECK(o.auto_run == true);

  TEST_CHECK(options_parse(L"-auto-run -auto-close", &o, &err) == true);
  TEST_CHECK(o.auto_run == true);

  TEST_CHECK(options_parse(L"-auto-run -auto-close -no-auto-close", &o, &err) == true);
  TEST_CHECK(o.auto_run == true);
}

/**
 * @brief The arguments may be absent
 */
static void test_parse_null_out(void) {
  struct ov_error err = {0};

  if (!TEST_CHECK(options_parse(L"-auto-run", NULL, &err) == false)) {
    goto cleanup;
  }
  TEST_CHECK(ov_error_is(&err, ov_error_type_generic, ov_error_generic_invalid_argument));

cleanup:
  OV_ERROR_REPORT(&err, NULL);
}

TEST_LIST = {
    {"defaults", test_defaults},
    {"parse_empty", test_parse_empty},
    {"parse_all_on", test_parse_all_on},
    {"parse_negations", test_parse_negations},
    {"parse_quoted", test_parse_quoted_and_extra_spaces},
    {"parse_unknown_ignored", test_parse_unknown_ignored},
    {"parse_last_wins", test_parse_last_wins},
    {"auto_close_follows_the_run", test_auto_close_follows_the_mode_of_the_run},
    {"parse_null_out", test_parse_null_out},
    {NULL, NULL},
};
