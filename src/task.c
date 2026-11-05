#include "task.h"

#ifndef COBJMACROS
#  define COBJMACROS
#endif
#include <taskschd.h>

#include <ovarray.h>
#include <ovbase.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include <wchar.h>

#include "error.h"
#include "install.h"
#include "paths.h"
#include "settings.h"

static wchar_t const kEventSubscription[] = L"<QueryList><Query Id=\"0\" Path=\"System\"><Select Path=\"System\">"
                                            L"*[System[Provider[@Name='Microsoft-Windows-Power-Troubleshooter'] and (EventID=1)]]"
                                            L"</Select></Query></QueryList>";

/**
 * @brief Append a text to a document, with the XML entities it needs
 *
 * The document is what the scheduler reads, and the scheduler takes it as a wide text, so
 * the document is built as one: a document that is not wide cannot be handed in at all.
 *
 * @param out the document to append to
 * @param text the text to append
 * @param err receives the failure of the append
 * @return false when the document could not grow
 */
static NODISCARD bool xml_append_escaped(wchar_t **const out, wchar_t const *const text, struct ov_error *const err) {
  for (size_t i = 0; (text != NULL) && (text[i] != L'\0'); i++) {
    wchar_t const *const replacement = (text[i] == L'&')   ? L"&amp;"
                                       : (text[i] == L'<') ? L"&lt;"
                                       : (text[i] == L'>') ? L"&gt;"
                                       : (text[i] == L'"') ? L"&quot;"
                                                           : NULL;
    if (replacement != NULL) {
      if (!ov_sprintf_append_wchar(out, err, NULL, L"%ls", replacement)) {
        return false;
      }
    } else if (!ov_sprintf_append_wchar(out, err, NULL, L"%c", text[i])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief The switches the task runs with, as the arguments field of the action
 *
 * -auto-run is always in it: a run that does not know it was started by the scheduler
 * would keep its window and wait for a person who is not there.  The close flag and the
 * pause flag follow the boxes of the caller.  The program is not named here: the command
 * field names it and the scheduler runs it with these arguments in front of it.
 *
 * @param close_mixer the box of the caller that allows closing the mixer
 * @param allow_audio_service_pause the box of the caller that allows pausing the Windows
 *        Audio service
 * @param err receives a failure of the build
 * @return the arguments, NULL when they could not be built
 * @note Release with OV_ARRAY_DESTROY.
 */
static NODISCARD wchar_t *build_arguments(bool const close_mixer, bool const allow_audio_service_pause, struct ov_error *const err) {
  wchar_t *args = NULL;
  bool success = false;

  if (!ov_sprintf_wchar(&args,
                        err,
                        NULL,
                        L"-auto-run%ls%ls",
                        close_mixer ? L" -close-mixer" : L" -no-close-mixer",
                        allow_audio_service_pause ? L" -pause-audio-service" : L" -no-pause-audio-service")) {
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
 * @param description the description the properties show, UTF-8, NULL takes the default of
 *                    this program
 * @param err receives a failure of the build
 * @return the document, NULL when it could not be built
 * @note Release with OV_ARRAY_DESTROY.
 */
static NODISCARD wchar_t *
build_xml(wchar_t const *const command, wchar_t const *const arguments, char const *const description, struct ov_error *const err) {
  wchar_t *wide_description = NULL;
  wchar_t *escaped_command = NULL;
  wchar_t *escaped_args = NULL;
  wchar_t *escaped_description = NULL;
  wchar_t *escaped_subscription = NULL;
  wchar_t *xml = NULL;
  bool success = false;

  if (!ov_sprintf_wchar(&wide_description, err, NULL, L"%s", (description != NULL) ? description : default_description())) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
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
  if (!xml_append_escaped(&escaped_description, wide_description, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!ov_sprintf_wchar(&xml,
                        err,
                        NULL,
                        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
                        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
                        L"  <RegistrationInfo>\n"
                        L"    <Description>%ls</Description>\n"
                        L"  </RegistrationInfo>\n"
                        L"  <Triggers>\n"
                        L"    <EventTrigger>\n"
                        L"      <Enabled>true</Enabled>\n"
                        L"      <Subscription>%ls</Subscription>\n"
                        L"    </EventTrigger>\n"
                        L"  </Triggers>\n"
                        L"  <Settings>\n"
                        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
                        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
                        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
                        L"    <AllowHardTerminate>true</AllowHardTerminate>\n"
                        L"    <StartWhenAvailable>false</StartWhenAvailable>\n"
                        L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n"
                        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\n"
                        L"    <Enabled>true</Enabled>\n"
                        L"    <Hidden>false</Hidden>\n"
                        L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n"
                        L"    <WakeToRun>false</WakeToRun>\n"
                        L"    <ExecutionTimeLimit>PT72H</ExecutionTimeLimit>\n"
                        L"    <Priority>7</Priority>\n"
                        L"  </Settings>\n"
                        L"  <Principals>\n"
                        L"    <Principal id=\"Author\">\n"
                        L"      <UserId>S-1-5-18</UserId>\n"
                        L"      <RunLevel>HighestAvailable</RunLevel>\n"
                        L"    </Principal>\n"
                        L"  </Principals>\n"
                        L"  <Actions Context=\"Author\">\n"
                        L"    <Exec>\n"
                        L"      <Command>&quot;%ls&quot;</Command>\n"
                        L"      <Arguments>%ls</Arguments>\n"
                        L"    </Exec>\n"
                        L"  </Actions>\n"
                        L"</Task>\n",
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
  if (wide_description != NULL) {
    OV_ARRAY_DESTROY(&wide_description);
    wide_description = NULL;
  }
  if (!success && (xml != NULL)) {
    OV_ARRAY_DESTROY(&xml);
    xml = NULL;
  }
  return xml;
}

/**
 * @brief The service and the root folder of the task scheduler
 *
 * The two handles every call below needs, and a caller that has one has no use for it
 * without the other.  com is among them: the library is readied for the thread while the
 * connection is up, and let go with it.
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
  CoUninitialize();
}

/**
 * @brief Connect to the task scheduler of this machine
 *
 * @param s receives the two handles, release with scheduler_close
 * @param err receives the refusal of the system
 * @return false when the connection could not be made
 */
static NODISCARD bool scheduler_open(struct scheduler *const s, struct ov_error *const err) {
  HRESULT hr = S_OK;
  ITaskService *service = NULL;
  ITaskFolder *root = NULL;
  VARIANT empty;
  BSTR root_path = NULL;
  bool success = false;
  bool com_initialized = false;

  hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  com_initialized = true;

  hr = CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskService, (void **)&service);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  VariantInit(&empty);
  hr = ITaskService_Connect(service, empty, empty, empty, empty);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  root_path = SysAllocString(L"\\");
  if (root_path == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  hr = ITaskService_GetFolder(service, root_path, &root);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }

  *s = (struct scheduler){
      .service = service,
      .root = root,
  };
  service = NULL;
  root = NULL;
  success = true;

cleanup:
  if (root_path != NULL) {
    SysFreeString(root_path);
    root_path = NULL;
  }
  if (!success) {
    if (root != NULL) {
      ITaskFolder_Release(root);
      root = NULL;
    }
    if (service != NULL) {
      ITaskService_Release(service);
      service = NULL;
    }
    if (com_initialized) {
      CoUninitialize();
    }
  }
  return success;
}

/**
 * @brief The folder and the name the task of this program is registered under
 *
 * The two are separate values in the settings file, so neither is looked through for a
 * separator.  Both are needed to register or to remove the entry, and a settings file that
 * leaves one of them out is an answer this program cannot work with.
 *
 * @param settings the settings that name the task
 * @param out_dir receives the folder, borrowed from the settings
 * @param out_name receives the name, borrowed from the settings
 * @param err receives the refusal of a settings file that does not name one task
 * @return false when the settings do not carry a folder and a name to use
 */
NODISCARD bool task_target(struct settings const *const settings,
                           wchar_t const **const out_dir,
                           wchar_t const **const out_name,
                           struct ov_error *const err) {
  *out_dir = NULL;
  *out_name = NULL;
  if ((settings == NULL) || (settings->task_dir == NULL) || (settings->task_name == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if ((settings->task_dir[0] == L'\0') || (wcschr(settings->task_dir, L'\\') != NULL) || (settings->task_name[0] == L'\0') ||
      (wcschr(settings->task_name, L'\\') != NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  *out_dir = settings->task_dir;
  *out_name = settings->task_name;
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
static NODISCARD bool install_document(struct settings const *const settings, wchar_t const *const xml, struct ov_error *const err) {
  // the handles are zeroed here: a failure before they are filled in still reaches cleanup,
  // which reads every one of them
  struct scheduler sch = {0};
  wchar_t const *dir = NULL;
  wchar_t const *name = NULL;
  BSTR folder_path = NULL;
  BSTR path = NULL;
  BSTR document = NULL;
  VARIANT empty;
  ITaskFolder *folder = NULL;
  HRESULT hr = S_OK;
  bool success = false;
  bool opened = false;

  if (xml == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!task_target(settings, &dir, &name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!scheduler_open(&sch, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  opened = true;
  VariantInit(&empty);
  folder_path = SysAllocString(dir);
  path = SysAllocString(name);
  document = SysAllocString(xml);
  if ((folder_path == NULL) || (path == NULL) || (document == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  hr = ITaskService_GetFolder(sch.service, folder_path, &folder);
  if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
    hr = ITaskFolder_CreateFolder(sch.root, folder_path, empty, &folder);
  }
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  hr = ITaskFolder_RegisterTask(folder, path, document, TASK_CREATE_OR_UPDATE, empty, empty, TASK_LOGON_SERVICE_ACCOUNT, empty, NULL);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  success = true;

cleanup:
  if (folder_path != NULL) {
    SysFreeString(folder_path);
    folder_path = NULL;
  }
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
  if (opened) {
    scheduler_close(&sch);
  }
  return success;
}

/**
 * @brief Register the task of this program
 *
 * Registers the task, or overwrites the one that is already registered.  Fails only when
 * the system refuses the registration, or when this copy is not the one the installer put
 * on the machine: a task that runs a file anyone can replace would run it with the
 * privileges of the system.  The second refusal is app_error_task_not_installed.
 *
 * @param settings the settings that name the task
 * @param close_mixer the box of the caller that allows closing the mixer
 * @param allow_audio_service_pause the box of the caller that allows pausing the Windows
 *        Audio service
 * @param err receives the refusal of the system
 * @return false when the registration did not happen
 */
NODISCARD bool task_install(struct settings const *const settings,
                            bool const close_mixer,
                            bool const allow_audio_service_pause,
                            struct ov_error *const err) {
  wchar_t *exe = NULL;
  wchar_t *command = NULL;
  wchar_t *args = NULL;
  wchar_t *xml = NULL;
  bool success = false;

  if (settings == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!install_is_installed()) {
    OV_ERROR_SET(
        err, ov_error_type_app, app_error_task_not_installed, "the program is not the installed copy, so the task is not registered");
    return false;
  }
  exe = paths_executable(err);
  if (exe == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  command = paths_final_path(exe, err);
  if (command == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  args = build_arguments(close_mixer, allow_audio_service_pause, err);
  if (args == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  xml = build_xml(command, args, NULL, err);
  if (xml == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = install_document(settings, xml, err);

cleanup:
  if (xml != NULL) {
    OV_ARRAY_DESTROY(&xml);
    xml = NULL;
  }
  if (args != NULL) {
    OV_ARRAY_DESTROY(&args);
    args = NULL;
  }
  if (command != NULL) {
    OV_ARRAY_DESTROY(&command);
    command = NULL;
  }
  if (exe != NULL) {
    OV_ARRAY_DESTROY(&exe);
  }
  return success;
}

/**
 * @brief Remove the task of this program
 *
 * A task that is not there is not a failure: the answer the caller asked for is "no entry",
 * and that is what holds afterwards either way.  The caller is told the difference so that a
 * window can say that there was nothing to remove.
 *
 * The folder that held the entry goes with it when nothing else is in it.  A folder that
 * other tasks still use stays, and that is an answer of its own rather than a failure of the
 * removal, which happened.
 *
 * @param settings the settings that name the task
 * @param err receives the failure.  HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) says that there
 *            was no entry to remove; HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY) says that the
 *            entry went and the folder stayed
 * @return false when the entry could not be removed
 */
NODISCARD bool task_uninstall(struct settings const *const settings, struct ov_error *const err) {
  struct scheduler sch = {0};
  wchar_t const *dir = NULL;
  wchar_t const *name = NULL;
  ITaskFolder *folder = NULL;
  BSTR folder_path = NULL;
  BSTR path = NULL;
  HRESULT hr = S_OK;
  bool success = false;
  bool opened = false;

  if (!task_target(settings, &dir, &name, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!scheduler_open(&sch, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  opened = true;
  folder_path = SysAllocString(dir);
  path = SysAllocString(name);
  if ((folder_path == NULL) || (path == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  hr = ITaskService_GetFolder(sch.service, folder_path, &folder);
  if (FAILED(hr)) {
    // no folder of that name, so no entry of that name either
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  hr = ITaskFolder_DeleteTask(folder, path, 0);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  // the folder goes with the entry it held, unless something else is still in it
  hr = ITaskFolder_DeleteFolder(sch.root, folder_path, 0);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  success = true;

cleanup:
  if (folder_path != NULL) {
    SysFreeString(folder_path);
    folder_path = NULL;
  }
  if (path != NULL) {
    SysFreeString(path);
    path = NULL;
  }
  if (folder != NULL) {
    ITaskFolder_Release(folder);
    folder = NULL;
  }
  if (opened) {
    scheduler_close(&sch);
  }
  return success;
}
