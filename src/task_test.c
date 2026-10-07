#include <ovtest.h>

#include <windows.h>

#include <string.h>

#include "task.h"

/**
 * @brief The arguments field carries the switches and nothing else
 *
 * The scheduler runs the command field with these arguments in front of it, so the path of
 * the program in here would reach the run as its first argument.
 */
static void test_arguments_carry_the_switches_only(void) {
  struct ov_error err = {0};
  char *args = NULL;

  args = task_build_arguments(true, false, &err);
  if (!TEST_CHECK(args != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(args, "-auto-run -close-mixer -no-close-audiodg") == 0);
  TEST_MSG("want -auto-run -close-mixer -no-close-audiodg, got %hs", args);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  task_string_free(&args);
}

/**
 * @brief Every box has an answer in the record
 *
 * A task that left one out would run the default of the program instead of the decision of
 * the window it was registered from.
 */
static void test_arguments_carry_both_boxes(void) {
  struct ov_error err = {0};
  char *on = NULL;
  char *off = NULL;

  on = task_build_arguments(true, true, &err);
  off = task_build_arguments(false, false, &err);
  if (!TEST_CHECK(on != NULL) || !TEST_CHECK(off != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strstr(on, "-close-mixer") != NULL);
  TEST_CHECK(strstr(on, "-close-audiodg") != NULL);
  TEST_CHECK(strstr(on, "-no-close-mixer") == NULL);
  TEST_CHECK(strstr(on, "-no-close-audiodg") == NULL);
  TEST_CHECK(strstr(off, "-no-close-mixer") != NULL);
  TEST_CHECK(strstr(off, "-no-close-audiodg") != NULL);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  task_string_free(&on);
  task_string_free(&off);
}

/**
 * @brief The document has to be well formed around the path
 *
 * A program under a folder with an ampersand in its name is common enough, and an
 * unescaped one would produce a document the scheduler refuses with a message about the
 * XML rather than about the path.
 */
static void test_xml_escapes_what_it_has_to(void) {
  struct ov_error err = {0};
  char *xml = NULL;

  xml = task_build_xml("C:\\a&b\\adc.exe", "\"C:\\a&b\\adc.exe\" -auto-run -close-mixer -close-audiodg", NULL, &err);
  if (!TEST_CHECK(xml != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strstr(xml, "C:\\a&amp;b\\adc.exe") != NULL);
  TEST_CHECK(strstr(xml, "C:\\a&b\\adc.exe") == NULL);
  TEST_CHECK(strstr(xml, "<Task version=\"1.2\"") != NULL);
  TEST_CHECK(strstr(xml, "</Task>") != NULL);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  task_string_free(&xml);
}

/**
 * @brief The trigger and the principal are the reason the task exists
 *
 * The trigger is the resume from sleep, and the principal is what makes the removal of a
 * device possible at all.  Both are checked here.
 */
static void test_xml_states_the_trigger_and_the_principal(void) {
  struct ov_error err = {0};
  char *xml = NULL;

  xml = task_build_xml("adc.exe", "-auto-run", NULL, &err);
  if (!TEST_CHECK(xml != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strstr(xml, "<EventTrigger>") != NULL);
  TEST_CHECK(strstr(xml, "Microsoft-Windows-Power-Troubleshooter") != NULL);
  TEST_CHECK(strstr(xml, "(EventID=1)") != NULL);
  TEST_CHECK(strstr(xml, "<RunLevel>HighestAvailable</RunLevel>") != NULL);
  TEST_CHECK(strstr(xml, "<LogonType>InteractiveToken</LogonType>") != NULL);
  TEST_CHECK(strstr(xml, "<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>") != NULL);
  TEST_CHECK(strstr(xml, "<ExecutionTimeLimit>PT72H</ExecutionTimeLimit>") != NULL);
  TEST_CHECK(strstr(xml, "&lt;QueryList&gt;") != NULL);
  TEST_CHECK(strstr(xml, "<QueryList>") == NULL);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  task_string_free(&xml);
}

/**
 * @brief The program is named in the command field with quotes around it
 *
 * So a path with a space in it stays one argument of the run, and the arguments field
 * carries the switches alone.
 */
static void test_xml_quotes_the_command(void) {
  struct ov_error err = {0};
  char *xml = NULL;

  xml = task_build_xml("C:\\a b\\adc.exe", "-auto-run", NULL, &err);
  if (!TEST_CHECK(xml != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strstr(xml, "<Command>&quot;C:\\a b\\adc.exe&quot;</Command>") != NULL);
  TEST_CHECK(strstr(xml, "<Arguments>-auto-run</Arguments>") != NULL);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  task_string_free(&xml);
}

/**
 * @brief The description of the entry says what the task is for and when it runs
 *
 * The name of the program is not a description: a reader of the task library has to learn
 * from the entry itself what it does.
 */
static void test_xml_describes_what_the_task_is_for(void) {
  struct ov_error err = {0};
  char *xml = NULL;

  xml = task_build_xml("adc.exe", "-auto-run", NULL, &err);
  if (!TEST_CHECK(xml != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strstr(xml, "<Description>") != NULL);
  TEST_CHECK(strstr(xml, "<Description>Audient Device Closer</Description>") == NULL);
  TEST_CHECK(strstr(xml, "resumes from sleep") != NULL);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  task_string_free(&xml);
}

/**
 * @brief A description of the caller goes through the same escaping as the path
 *
 * A translation with an ampersand or a bracket in it must still make a document the
 * scheduler accepts.
 */
static void test_xml_takes_the_description_it_is_given(void) {
  struct ov_error err = {0};
  char *xml = NULL;

  xml = task_build_xml("adc.exe", "-auto-run", "one & <two>", &err);
  if (!TEST_CHECK(xml != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(strstr(xml, "<Description>one &amp; &lt;two&gt;</Description>") != NULL);
  TEST_CHECK(strstr(xml, "<Description>one & <two>") == NULL);

cleanup:
  OV_ERROR_REPORT(&err, NULL);
  task_string_free(&xml);
}

/**
 * @brief Removing what is not there is the state the caller asked for
 *
 * So the answer of the system for it is read as done.  Any other failure is a failure.
 */
static void test_removal_of_a_missing_task_is_done(void) {
  TEST_CHECK(task_removal_is_done((long)HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) == true);
  TEST_CHECK(task_removal_is_done((long)HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) == false);
  TEST_CHECK(task_removal_is_done((long)S_OK) == false);
}

/**
 * @brief A task path splits into the folder of the library and the name inside it
 */
static void test_path_splits_into_folder_and_task(void) {
  char folder[64];
  char leaf[64];

  TEST_CASE("a task inside its own folder");
  TEST_CHECK(task_split_path("\\A\\B", folder, sizeof(folder), leaf, sizeof(leaf)) == true);
  TEST_CHECK(strcmp(folder, "\\A") == 0);
  TEST_CHECK(strcmp(leaf, "B") == 0);

  TEST_CASE("a task in the root of the library");
  TEST_CHECK(task_split_path("\\Only", folder, sizeof(folder), leaf, sizeof(leaf)) == true);
  TEST_CHECK(strcmp(folder, "\\") == 0);
  TEST_CHECK(strcmp(leaf, "Only") == 0);

  TEST_CASE("a name without a leading separator");
  TEST_CHECK(task_split_path("Loose", folder, sizeof(folder), leaf, sizeof(leaf)) == true);
  TEST_CHECK(strcmp(folder, "\\") == 0);
  TEST_CHECK(strcmp(leaf, "Loose") == 0);

  TEST_CASE("deeper nesting keeps the whole folder");
  TEST_CHECK(task_split_path("\\A\\B\\C", folder, sizeof(folder), leaf, sizeof(leaf)) == true);
  TEST_CHECK(strcmp(folder, "\\A\\B") == 0);
  TEST_CHECK(strcmp(leaf, "C") == 0);

  TEST_CASE_("names that name no task");
  TEST_CHECK(task_split_path("", folder, sizeof(folder), leaf, sizeof(leaf)) == false);
  TEST_CHECK(task_split_path("\\A\\", folder, sizeof(folder), leaf, sizeof(leaf)) == false);
  TEST_CHECK(task_split_path(NULL, folder, sizeof(folder), leaf, sizeof(leaf)) == false);
  TEST_CASE_(NULL);
}

/**
 * @brief A path that cannot be split is refused
 */
static void test_path_refuses_what_does_not_fit(void) {
  char folder[4];
  char leaf[4];

  TEST_CHECK(task_split_path("\\ABCDEF\\GHIJKL", folder, sizeof(folder), leaf, sizeof(leaf)) == false);
  TEST_CHECK(task_split_path("\\A\\BCDEFG", folder, 4, leaf, 4) == false);
}

TEST_LIST = {
    {"arguments_carry_the_switches", test_arguments_carry_the_switches_only},
    {"arguments_carry_both_boxes", test_arguments_carry_both_boxes},
    {"xml_escapes", test_xml_escapes_what_it_has_to},
    {"xml_quotes_the_command", test_xml_quotes_the_command},
    {"xml_trigger_and_principal", test_xml_states_the_trigger_and_the_principal},
    {"xml_describes_the_task", test_xml_describes_what_the_task_is_for},
    {"xml_takes_the_description", test_xml_takes_the_description_it_is_given},
    {"removal_of_a_missing_task", test_removal_of_a_missing_task_is_done},
    {"path_splits", test_path_splits_into_folder_and_task},
    {"path_refuses_what_does_not_fit", test_path_refuses_what_does_not_fit},
    {NULL, NULL},
};
