#include "task.h"

#ifndef COBJMACROS
#  define COBJMACROS
#endif
#include <taskschd.h>

#include <ovarray.h>
#include <ovbase.h>
#include <ovmo.h>
#include <ovprintf_ex.h>

#include "paths.h"
#include "settings.h"

static char const kEventSubscription[] = "<QueryList><Query Id=\"0\" Path=\"System\"><Select Path=\"System\">"
                                         "*[System[Provider[@Name='Microsoft-Windows-Power-Troubleshooter'] and (EventID=1)]]"
                                         "</Select></Query></QueryList>";

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void task_string_free(char **const value) {
  if ((value != NULL) && (*value != NULL)) {
    OV_ARRAY_DESTROY(value); // made by ov_sprintf_*
  }
}

/**
 * @brief Append a text to a document, with the XML entities it needs
 *
 * @param out the document to append to
 * @param text the text to append, UTF-8
 * @param err receives the failure of the append
 * @return false when the document could not grow
 */
static bool xml_append_escaped(char **const out, char const *const text, struct ov_error *const err) {
  size_t i = 0;

  for (i = 0; (text != NULL) && (text[i] != '\0'); i++) {
    char const *const replacement = (text[i] == '&')   ? "&amp;"
                                    : (text[i] == '<') ? "&lt;"
                                    : (text[i] == '>') ? "&gt;"
                                    : (text[i] == '"') ? "&quot;"
                                                       : NULL;
    if (replacement != NULL) {
      if (!ov_sprintf_append_char(out, err, NULL, "%hs", replacement)) {
        return false;
      }
    } else if (!ov_sprintf_append_char(out, err, NULL, "%c", text[i])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief The switches the task runs with, as the arguments field of the action
 *
 * -auto-run is always in it: a run that does not know it was started by the scheduler
 * would keep its window and wait for a person who is not there.  The two close flags
 * follow the boxes of the caller.  The program is not named here: the command field names
 * it and the scheduler runs it with these arguments in front of it.
 *
 * @param close_mixer the box of the caller that allows closing the mixer
 * @param close_audiodg the box of the caller that allows closing the audio engine
 * @param err receives a failure of the build
 * @return the arguments in UTF-8, NULL when they could not be built
 * @note Release with task_string_free.
 */
char *task_build_arguments(bool const close_mixer, bool const close_audiodg, struct ov_error *const err) {
  char *args = NULL;
  bool success = false;

  if (!ov_sprintf_char(&args,
                       err,
                       NULL,
                       "-auto-run%hs%hs",
                       close_mixer ? " -close-mixer" : " -no-close-mixer",
                       close_audiodg ? " -close-audiodg" : " -no-close-audiodg")) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success && (args != NULL)) {
    OV_ARRAY_DESTROY(&args);
    args = NULL;
  }
  return args;
}

/**
 * @brief The description the document carries when the caller has none
 *
 * The properties of the entry show it, so it says what the task is for and when it runs:
 * the name of the program on its own says neither.
 *
 * @return a static string
 */
static char const *default_description(void) {
  return gettext("Starts Audient Device Closer after the computer resumes from sleep and "
                 "tries to fix the problem when the Audient device did not come back properly.");
}

/**
 * @brief The document the task is registered from
 *
 * The trigger is the resume from sleep, the principal asks for the highest privileges of
 * the account, and the settings are the ones this program needs: one instance at a time,
 * no demand for a battery or a network.  Command, arguments and description are escaped
 * here, and the command is written with quotes around it so a path with a space in it
 * stays one argument of the run.
 *
 * @param command the program to run
 * @param arguments the switches of the action
 * @param description the description the properties show, NULL takes the default of this
 *                    program
 * @param err receives a failure of the build
 * @return the document in UTF-8, NULL when it could not be built
 * @note Release with task_string_free.
 */
char *task_build_xml(char const *const command, char const *const arguments, char const *const description, struct ov_error *const err) {
  char *escaped_command = NULL;
  char *escaped_args = NULL;
  char *escaped_description = NULL;
  char *escaped_subscription = NULL;
  char *xml = NULL;
  bool success = false;

  if (!xml_append_escaped(&escaped_args, arguments, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!xml_append_escaped(&escaped_command, command, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!xml_append_escaped(&escaped_subscription, kEventSubscription, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!xml_append_escaped(&escaped_description, (description != NULL) ? description : default_description(), err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!ov_sprintf_char(&xml,
                       err,
                       NULL,
                       "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
                       "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
                       "  <RegistrationInfo>\n"
                       "    <Description>%hs</Description>\n"
                       "  </RegistrationInfo>\n"
                       "  <Triggers>\n"
                       "    <EventTrigger>\n"
                       "      <Enabled>true</Enabled>\n"
                       "      <Subscription>%hs</Subscription>\n"
                       "    </EventTrigger>\n"
                       "  </Triggers>\n"
                       "  <Settings>\n"
                       "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
                       "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
                       "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
                       "    <AllowHardTerminate>true</AllowHardTerminate>\n"
                       "    <StartWhenAvailable>false</StartWhenAvailable>\n"
                       "    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n"
                       "    <AllowStartOnDemand>true</AllowStartOnDemand>\n"
                       "    <Enabled>true</Enabled>\n"
                       "    <Hidden>false</Hidden>\n"
                       "    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n"
                       "    <WakeToRun>false</WakeToRun>\n"
                       "    <ExecutionTimeLimit>PT72H</ExecutionTimeLimit>\n"
                       "    <Priority>7</Priority>\n"
                       "  </Settings>\n"
                       "  <Principals>\n"
                       "    <Principal id=\"Author\">\n"
                       "      <LogonType>InteractiveToken</LogonType>\n"
                       "      <RunLevel>HighestAvailable</RunLevel>\n"
                       "    </Principal>\n"
                       "  </Principals>\n"
                       "  <Actions Context=\"Author\">\n"
                       "    <Exec>\n"
                       "      <Command>&quot;%hs&quot;</Command>\n"
                       "      <Arguments>%hs</Arguments>\n"
                       "    </Exec>\n"
                       "  </Actions>\n"
                       "</Task>\n",
                       escaped_description,
                       escaped_subscription,
                       escaped_command,
                       escaped_args)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (escaped_subscription != NULL) {
    OV_ARRAY_DESTROY(&escaped_subscription);
    escaped_subscription = NULL;
  }
  if (escaped_description != NULL) {
    OV_ARRAY_DESTROY(&escaped_description);
    escaped_description = NULL;
  }
  if (escaped_command != NULL) {
    OV_ARRAY_DESTROY(&escaped_command);
    escaped_command = NULL;
  }
  if (escaped_args != NULL) {
    OV_ARRAY_DESTROY(&escaped_args);
    escaped_args = NULL;
  }
  if (!success && (xml != NULL)) {
    OV_ARRAY_DESTROY(&xml);
    xml = NULL;
  }
  return xml;
}

/**
 * @brief Does the answer of the system mean that the task is gone
 *
 * Removing a task that is not there is that state, so the caller is done even though
 * the call failed.
 *
 * @param result the HRESULT of the removal call
 * @return true when the task is not there any more
 */
bool task_removal_is_done(long const result) { return (HRESULT)result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); }

/**
 * @brief Split a task path into the folder of the library and the name inside it
 *
 * A path without a separator is a task in the root of the library, and the folder of
 * it is the root itself.  Both buffers are the caller's.
 *
 * @param path the task path
 * @param folder receives the folder of the library
 * @param folder_size room of folder
 * @param leaf receives the name inside the folder
 * @param leaf_size room of leaf
 * @return false when the path is empty, too long for a buffer, or ends in a separator
 */
bool task_split_path(char const *const path, char *const folder, size_t const folder_size, char *const leaf, size_t const leaf_size) {
  size_t len = 0;
  size_t last = 0;
  size_t i = 0;
  size_t folder_len = 0;
  bool separated = false;
  char const *folder_text = NULL;
  char const *leaf_text = NULL;

  if ((path == NULL) || (folder == NULL) || (leaf == NULL) || (folder_size == 0) || (leaf_size == 0)) {
    return false;
  }
  len = strlen(path);
  if ((len == 0) || (path[len - 1] == '\\')) {
    return false; // an empty name, or one that names the folder it would live in
  }
  last = len;
  for (i = len; i > 0; i--) {
    if (path[i - 1] == '\\') {
      last = i - 1;
      separated = true;
      break;
    }
  }
  if (separated && (last > 0)) {
    folder_len = last;
    folder_text = path;
    leaf_text = path + last + 1;
  } else {
    folder_len = 1;
    folder_text = "\\";
    leaf_text = path + (separated ? 1 : 0);
  }
  if ((folder_len + 1 > folder_size) || (strlen(leaf_text) + 1 > leaf_size)) {
    return false;
  }
  memcpy(folder, folder_text, folder_len);
  folder[folder_len] = '\0';
  strcpy(leaf, leaf_text);
  return true;
}

/**
 * @brief A string of the COM library out of a UTF-8 one
 *
 * SysAllocStringLen takes the length in characters and copies it, so the buffer can go as
 * soon as it returns.
 *
 * @param text the string to convert, UTF-8
 * @return the BSTR, NULL when there is no memory for it
 * @note Release with SysFreeString.
 */
static BSTR to_bstr(char const *const text) {
  BSTR result = NULL;
  wchar_t *wide = NULL;

  if (text == NULL) {
    return NULL;
  }
  if (!ov_sprintf_wchar(&wide, NULL, NULL, L"%s", text)) {
    return NULL;
  }
  result = SysAllocStringLen(wide, (UINT)wcslen(wide));
  OV_ARRAY_DESTROY(&wide);
  return result;
}

/**
 * @brief The VARIANT that means "nothing" to the calls of the task scheduler
 *
 * This machine, this account and no password.  A fresh one every time, because the caller
 * owns what it passes.
 *
 * @return the VARIANT to hand in
 */
static VARIANT empty_variant(void) {
  VARIANT v;
  VariantInit(&v);
  return v;
}

/**
 * @brief The folder of the library that holds the task of this path
 *
 * The name of the settings is a path inside the library, the way the PowerToys entry of
 * this machine is spelled: the last separator of it divides the path into the folder and
 * the task inside it.
 *
 * @param path the task path
 * @return the folder as a BSTR, NULL when there is no memory for it
 * @note Release with SysFreeString.
 */
static BSTR folder_of_path(char const *const path) {
  char folder[512];
  char leaf[256];

  if (!task_split_path(path, folder, sizeof(folder), leaf, sizeof(leaf))) {
    return to_bstr("\\");
  }
  return to_bstr(folder);
}

/**
 * @brief The service and the root folder of the task scheduler
 *
 * The two handles every call below needs, and a caller that has one has no use for it
 * without the other.
 */
struct scheduler {
  ITaskService *service;
  ITaskFolder *root;
};

/**
 * @brief Give the two handles back
 *
 * @param s the connection to close, NULL is allowed
 */
static void scheduler_close(struct scheduler *const s) {
  if (s->root != NULL) {
    ITaskFolder_Release(s->root);
    s->root = NULL;
  }
  if (s->service != NULL) {
    ITaskService_Release(s->service);
    s->service = NULL;
  }
}

/**
 * @brief Connect to the task scheduler of this machine
 *
 * @param s receives the two handles, release with scheduler_close
 * @param err receives the refusal of the system
 * @return false when the connection could not be made
 */
static bool scheduler_open(struct scheduler *const s, struct ov_error *const err) {
  VARIANT empty;
  HRESULT hr = S_OK;
  BSTR root_path = NULL;
  bool success = false;

  memset(s, 0, sizeof(*s));
  VariantInit(&empty);
  hr = CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskService, (void **)&s->service);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  hr = ITaskService_Connect(s->service, empty, empty, empty, empty);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  root_path = to_bstr("\\");
  if (root_path == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  hr = ITaskService_GetFolder(s->service, root_path, &s->root);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  success = true;

cleanup:
  if (root_path != NULL) {
    SysFreeString(root_path);
    root_path = NULL;
  }
  if (!success) {
    scheduler_close(s);
  }
  return success;
}

/**
 * @brief The folder the task of this path lives in
 *
 * Made when it is not there yet.  A caller that already did this gets the folder it made;
 * one that did not gets a new one.  The task library has one folder per program that
 * schedules anything, and registering into it is what keeps the root of the library for
 * the tasks that belong to nobody in particular.
 *
 * @param s the connection to work through
 * @param path the task path
 * @param create make the folder when it is not there
 * @param out receives the folder, release with ITaskFolder_Release
 * @param err receives the refusal of the system
 * @return false when the folder could not be reached
 */
static bool scheduler_folder_for(
    struct scheduler *const s, char const *const path, bool const create, ITaskFolder **const out, struct ov_error *const err) {
  BSTR folder_path = NULL;
  ITaskFolder *folder = NULL;
  HRESULT hr = S_OK;
  bool success = false;

  *out = NULL;
  folder_path = folder_of_path(path);
  if (folder_path == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  if (wcscmp(folder_path, L"\\") == 0) {
    *out = s->root;
    ITaskFolder_AddRef(s->root);
    success = true;
    goto cleanup;
  }
  if (create) {
    hr = ITaskFolder_CreateFolder(s->root, folder_path, empty_variant(), &folder);
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
      hr = ITaskFolder_GetFolder(s->root, folder_path, &folder);
    }
  } else {
    hr = ITaskFolder_GetFolder(s->root, folder_path, &folder);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
      success = true;
      goto cleanup;
    }
  }
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  *out = folder;
  folder = NULL;
  success = true;

cleanup:
  if (folder != NULL) {
    ITaskFolder_Release(folder);
    folder = NULL;
  }
  if (folder_path != NULL) {
    SysFreeString(folder_path);
    folder_path = NULL;
  }
  return success;
}

/**
 * @brief Make sure this thread has the COM library ready
 *
 * The calls below run on the thread of the window, which has to have the COM library
 * ready.  A second initialisation of the same thread is answered with S_FALSE and is no
 * problem: what matters is that the library is loaded.  Every other answer is a failure,
 * and the scheduler cannot be reached without COM.  Each successful call is answered with
 * a CoUninitialize of the caller: the balance belongs to the thread that made it.
 *
 * @param err receives the refusal of the system
 * @return false when COM could not be initialised
 */
static bool com_ready(struct ov_error *const err) {
  HRESULT const hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    return false;
  }
  return true;
}

/**
 * @brief Register a document of the caller under the task name of the settings
 *
 * The same call with the document handed in instead of built here.  The two steps are
 * split so that the document and the registration can be looked at on their own.
 *
 * @param settings the settings that name the task
 * @param xml the document to register
 * @param err receives the refusal of the system
 * @return false when the registration did not happen
 */
bool task_install_document(struct settings const *const settings, char const *const xml, struct ov_error *const err) {
  struct scheduler sch;
  BSTR path = NULL;
  BSTR document = NULL;
  VARIANT empty;
  ITaskFolder *folder = NULL;
  char name[256];
  char folder_name[512];
  HRESULT hr = S_OK;
  bool success = false;
  bool com_initialised = false;

  if ((settings == NULL) || (settings->task_name == NULL) || (xml == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!task_split_path(settings->task_name, folder_name, sizeof(folder_name), name, sizeof(name))) {
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_invalid_argument,
                  "%1$hs",
                  gettext("The name of the scheduled task is not valid."),
                  NULL);
    return false;
  }
  if (!com_ready(err)) {
    goto cleanup;
  }
  com_initialised = true;
  if (!scheduler_open(&sch, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!scheduler_folder_for(&sch, settings->task_name, true, &folder, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  empty = empty_variant();
  path = to_bstr(name);
  document = to_bstr(xml);
  if ((path == NULL) || (document == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  hr = ITaskFolder_RegisterTask(folder, path, document, TASK_CREATE_OR_UPDATE, empty, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, NULL);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  success = true;

cleanup:
  if (path != NULL) {
    SysFreeString(path);
    path = NULL;
  }
  if (document != NULL) {
    SysFreeString(document);
    document = NULL;
  }
  if (folder != NULL) {
    ITaskFolder_Release(folder);
    folder = NULL;
  }
  scheduler_close(&sch);
  if (com_initialised) {
    CoUninitialize();
  }
  return success;
}

/**
 * @brief Register the task of this program
 *
 * Registers the task, or overwrites the one that is already registered.  Fails only when
 * the system refuses the registration.
 *
 * @param settings the settings that name the task
 * @param close_mixer the box of the caller that allows closing the mixer
 * @param close_audiodg the box of the caller that allows closing the audio engine
 * @param err receives the refusal of the system
 * @return false when the registration did not happen
 */
bool task_install(struct settings const *const settings, bool const close_mixer, bool const close_audiodg, struct ov_error *const err) {
  char *exe = NULL;
  char *args = NULL;
  char *xml = NULL;
  bool success = false;

  if ((settings == NULL) || (settings->task_name == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  exe = paths_executable(err);
  if (exe == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  args = task_build_arguments(close_mixer, close_audiodg, err);
  if (args == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  xml = task_build_xml(exe, args, NULL, err);
  if (xml == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = task_install_document(settings, xml, err);

cleanup:
  if (xml != NULL) {
    OV_ARRAY_DESTROY(&xml);
    xml = NULL;
  }
  if (args != NULL) {
    OV_ARRAY_DESTROY(&args);
    args = NULL;
  }
  paths_string_free(&exe);
  return success;
}

/**
 * @brief Remove the task of this program
 *
 * A task that is not there is not a failure: the answer the caller asked for is "no
 * entry", and that is what holds afterwards either way.
 *
 * @param settings the settings that name the task
 * @param err receives a failure that leaves the entry behind
 * @return false when the task could not be removed
 */
bool task_uninstall(struct settings const *const settings, struct ov_error *const err) {
  struct scheduler sch;
  ITaskFolder *folder = NULL;
  BSTR path = NULL;
  char name[256];
  char folder_name[512];
  HRESULT hr = S_OK;
  bool success = false;
  bool com_initialised = false;

  if ((settings == NULL) || (settings->task_name == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!task_split_path(settings->task_name, folder_name, sizeof(folder_name), name, sizeof(name))) {
    return true;
  }
  if (!com_ready(err)) {
    goto cleanup;
  }
  com_initialised = true;
  if (!scheduler_open(&sch, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!scheduler_folder_for(&sch, settings->task_name, false, &folder, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (folder == NULL) {
    success = true;
    goto cleanup;
  }
  path = to_bstr(name);
  if (path == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  hr = ITaskFolder_DeleteTask(folder, path, 0);
  if (FAILED(hr) && !task_removal_is_done(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  success = true;

cleanup:
  if (success && (folder_name[0] != '\0') && (strcmp(folder_name, "\\") != 0)) {
    BSTR folder_path = to_bstr(folder_name);
    if (folder_path != NULL) {
      HRESULT const folder_hr = ITaskFolder_DeleteFolder(sch.root, folder_path, 0);
      if (FAILED(folder_hr) && (folder_hr != HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY))) {
        struct ov_error folder_err = {0};
        OV_ERROR_SET_HRESULT(&folder_err, folder_hr);
        OV_ERROR_REPORT(&folder_err, NULL);
      }
      SysFreeString(folder_path);
      folder_path = NULL;
    }
  }
  if (path != NULL) {
    SysFreeString(path);
    path = NULL;
  }
  if (folder != NULL) {
    ITaskFolder_Release(folder);
    folder = NULL;
  }
  scheduler_close(&sch);
  if (com_initialised) {
    CoUninitialize(); // the balance of com_ready, on the thread that made it
  }
  return success;
}
