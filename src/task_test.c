#include "task.c"

#include <ovtest.h>

#include <windows.h>

#include <wchar.h>

/**
 * @brief The arguments field carries the switches and nothing else
 *
 * The scheduler runs the command field with these arguments in front of it, so the path of
 * the program in here would reach the run as its first argument.
 */
static void test_arguments_carry_the_switches_only(void) {
  struct ov_error err = {0};
  wchar_t *args = NULL;

  args = build_arguments(true, false, &err);
  if (!TEST_SUCCEEDED(args != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(wcscmp(args, L"-auto-run -close-mixer -no-pause-audio-service") == 0);
  TEST_MSG("want -auto-run -close-mixer -no-pause-audio-service, got %ls", args);

cleanup:
  OV_ARRAY_DESTROY(&args);
}

/**
 * @brief Every box has an answer in the record
 *
 * A task that left one out would run the default of the program instead of the decision of
 * the window it was registered from.
 */
static void test_arguments_carry_both_boxes(void) {
  struct ov_error err = {0};
  wchar_t *on = NULL;
  wchar_t *off = NULL;

  on = build_arguments(true, true, &err);
  off = build_arguments(false, false, &err);
  if (!TEST_SUCCEEDED(on != NULL, &err) || !TEST_SUCCEEDED(off != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(wcsstr(on, L"-close-mixer") != NULL);
  TEST_CHECK(wcsstr(on, L"-pause-audio-service") != NULL);
  TEST_CHECK(wcsstr(on, L"-no-close-mixer") == NULL);
  TEST_CHECK(wcsstr(on, L"-no-pause-audio-service") == NULL);
  TEST_CHECK(wcsstr(off, L"-no-close-mixer") != NULL);
  TEST_CHECK(wcsstr(off, L"-no-pause-audio-service") != NULL);

cleanup:
  OV_ARRAY_DESTROY(&on);
  OV_ARRAY_DESTROY(&off);
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
  wchar_t *xml = NULL;

  xml = build_xml(L"C:\\a&b\\adc.exe", L"\"C:\\a&b\\adc.exe\" -auto-run -close-mixer -pause-audio-service", NULL, &err);
  if (!TEST_SUCCEEDED(xml != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(wcsstr(xml, L"C:\\a&amp;b\\adc.exe") != NULL);
  TEST_CHECK(wcsstr(xml, L"C:\\a&b\\adc.exe") == NULL);
  TEST_CHECK(wcsstr(xml, L"<Task version=\"1.2\"") != NULL);
  TEST_CHECK(wcsstr(xml, L"</Task>") != NULL);

cleanup:
  OV_ARRAY_DESTROY(&xml);
}

/**
 * @brief The trigger and the principal are the reason the task exists
 *
 * The trigger is the resume from sleep, and the principal is what makes the removal of a
 * device possible at all.  Both are checked here.
 */
static void test_xml_states_the_trigger_and_the_principal(void) {
  struct ov_error err = {0};
  wchar_t *xml = NULL;

  xml = build_xml(L"adc.exe", L"-auto-run", NULL, &err);
  if (!TEST_SUCCEEDED(xml != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(wcsstr(xml, L"<EventTrigger>") != NULL);
  TEST_CHECK(wcsstr(xml, L"Microsoft-Windows-Power-Troubleshooter") != NULL);
  TEST_CHECK(wcsstr(xml, L"(EventID=1)") != NULL);
  TEST_CHECK(wcsstr(xml, L"<RunLevel>HighestAvailable</RunLevel>") != NULL);
  TEST_CHECK(wcsstr(xml, L"<UserId>S-1-5-18</UserId>") != NULL);
  // the schema's logonType knows only S4U, Password, InteractiveToken and
  // InteractiveTokenOrPassword: ServiceAccount is a value of the logon type the registration
  // call carries, not of this node, and putting it here fails the whole document with
  // SCHED_E_INVALIDVALUE
  TEST_CHECK(wcsstr(xml, L"<LogonType>") == NULL);
  TEST_CHECK(wcsstr(xml, L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>") != NULL);
  TEST_CHECK(wcsstr(xml, L"<ExecutionTimeLimit>PT72H</ExecutionTimeLimit>") != NULL);
  TEST_CHECK(wcsstr(xml, L"&lt;QueryList&gt;") != NULL);
  TEST_CHECK(wcsstr(xml, L"<QueryList>") == NULL);

cleanup:
  OV_ARRAY_DESTROY(&xml);
}

/**
 * @brief The program is named in the command field with quotes around it
 *
 * So a path with a space in it stays one argument of the run, and the arguments field
 * carries the switches alone.
 */
static void test_xml_quotes_the_command(void) {
  struct ov_error err = {0};
  wchar_t *xml = NULL;

  xml = build_xml(L"C:\\a b\\adc.exe", L"-auto-run", NULL, &err);
  if (!TEST_SUCCEEDED(xml != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(wcsstr(xml, L"<Command>&quot;C:\\a b\\adc.exe&quot;</Command>") != NULL);
  TEST_CHECK(wcsstr(xml, L"<Arguments>-auto-run</Arguments>") != NULL);

cleanup:
  OV_ARRAY_DESTROY(&xml);
}

/**
 * @brief The description of the entry says what the task is for and when it runs
 *
 * The name of the program is not a description: a reader of the task library has to learn
 * from the entry itself what it does.
 */
static void test_xml_describes_what_the_task_is_for(void) {
  struct ov_error err = {0};
  wchar_t *xml = NULL;

  xml = build_xml(L"adc.exe", L"-auto-run", NULL, &err);
  if (!TEST_SUCCEEDED(xml != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(wcsstr(xml, L"<Description>") != NULL);
  TEST_CHECK(wcsstr(xml, L"<Description>Audient Device Closer</Description>") == NULL);
  TEST_CHECK(wcsstr(xml, L"resumes from sleep") != NULL);

cleanup:
  OV_ARRAY_DESTROY(&xml);
}

/**
 * @brief A description of the caller goes through the same escaping as the path
 *
 * A translation with an ampersand or a bracket in it must still make a document the
 * scheduler accepts.
 */
static void test_xml_takes_the_description_it_is_given(void) {
  struct ov_error err = {0};
  wchar_t *xml = NULL;

  xml = build_xml(L"adc.exe", L"-auto-run", "one & <two>", &err);
  if (!TEST_SUCCEEDED(xml != NULL, &err)) {
    goto cleanup;
  }
  TEST_CHECK(wcsstr(xml, L"<Description>one &amp; &lt;two&gt;</Description>") != NULL);
  TEST_CHECK(wcsstr(xml, L"<Description>one & <two>") == NULL);

cleanup:
  OV_ARRAY_DESTROY(&xml);
}

/**
 * @brief A task is named by a folder and a name, and neither is a path
 *
 * The two are read from the settings file as separate values, so the program never has to
 * look for a separator in one of them.  A value that could still be read as a path is
 * refused here rather than handed to the scheduler, where a separator would name a folder
 * the settings say nothing about.
 */
static void test_a_task_needs_a_folder_and_a_name(void) {
  struct settings s = {0};
  struct ov_error err = {0};
  wchar_t const *dir = NULL;
  wchar_t const *name = NULL;

  TEST_CASE("a folder and a name");
  {
    *(&s) = (struct settings){.task_dir = L"AudientDeviceCloser", .task_name = L"Device recovery after resume from sleep"};
  }
  if (TEST_SUCCEEDED(task_target(&s, &dir, &name, &err), &err)) {
    TEST_CHECK(dir != NULL && wcscmp(dir, L"AudientDeviceCloser") == 0);
    TEST_CHECK(name != NULL && wcscmp(name, L"Device recovery after resume from sleep") == 0);
  }

  TEST_CASE("no settings at all");
  if (!TEST_FAILED_WITH(task_target(NULL, &dir, &name, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument)) {
    return;
  }

  TEST_CASE("no folder");
  {
    *(&s) = (struct settings){.task_name = L"a task"};
  }
  if (!TEST_FAILED_WITH(task_target(&s, &dir, &name, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument)) {
    return;
  }

  TEST_CASE("an empty folder");
  {
    *(&s) = (struct settings){.task_dir = L"", .task_name = L"a task"};
  }
  if (!TEST_FAILED_WITH(task_target(&s, &dir, &name, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument)) {
    return;
  }

  TEST_CASE("a folder that is itself a path");
  {
    *(&s) = (struct settings){.task_dir = L"A\\B", .task_name = L"a task"};
  }
  if (!TEST_FAILED_WITH(task_target(&s, &dir, &name, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument)) {
    return;
  }

  TEST_CASE("no name");
  {
    *(&s) = (struct settings){.task_dir = L"A"};
  }
  if (!TEST_FAILED_WITH(task_target(&s, &dir, &name, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument)) {
    return;
  }

  TEST_CASE("an empty name");
  {
    *(&s) = (struct settings){.task_dir = L"A", .task_name = L""};
  }
  if (!TEST_FAILED_WITH(task_target(&s, &dir, &name, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument)) {
    return;
  }

  TEST_CASE("a name that is itself a path");
  {
    *(&s) = (struct settings){.task_dir = L"A", .task_name = L"B\\C"};
  }
  if (!TEST_FAILED_WITH(task_target(&s, &dir, &name, &err), &err, ov_error_type_generic, ov_error_generic_invalid_argument)) {
    return;
  }
}

TEST_LIST = {
    {"arguments_carry_the_switches", test_arguments_carry_the_switches_only},
    {"arguments_carry_both_boxes", test_arguments_carry_both_boxes},
    {"xml_escapes", test_xml_escapes_what_it_has_to},
    {"xml_quotes_the_command", test_xml_quotes_the_command},
    {"xml_trigger_and_principal", test_xml_states_the_trigger_and_the_principal},
    {"xml_describes_the_task", test_xml_describes_what_the_task_is_for},
    {"xml_takes_the_description", test_xml_takes_the_description_it_is_given},
    {"a_task_needs_a_folder_and_a_name", test_a_task_needs_a_folder_and_a_name},
    {NULL, NULL},
};
