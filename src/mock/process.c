#include "mock/process.h"

#ifdef ADC_MOCK

#  include "process_manager.h"

#  include <string.h>
#  include <wchar.h>

#  include <ovarray.h>
#  include <ovprintf.h>
#  include <ovprintf_ex.h>

/** @brief One process of the installed script, and what the stand-in saw of it */
struct mock_entry {
  struct process_mock_entry entry; //!< what the script said
  bool ended;                      //!< whether a run asked for it and the stand-in let it go
};

/** @brief What the stand-in answers */
static struct process_mock_script g_script;
/** @brief The processes of the installed script, with what the stand-in saw */
static struct mock_entry *g_entries = NULL;
/** @brief How often a process was asked to end */
static size_t g_ends = 0;
/** @brief Whether a script is installed at all */
static bool g_installed = false;

/** @brief How often the audio service was paused */
static size_t g_service_pauses = 0;
/** @brief How often the audio service was started again */
static size_t g_service_resumes = 0;

/**
 * @brief The process of an id, NULL when the script has none of that id
 *
 * @param pid the process to look for
 * @return the entry, or NULL
 */
static struct mock_entry *find_entry(uint32_t const pid) {
  size_t const n = (g_entries != NULL) ? OV_ARRAY_LENGTH(g_entries) : 0;

  for (size_t i = 0; i < n; i++) {
    if (g_entries[i].entry.pid == pid) {
      return &g_entries[i];
    }
  }
  return NULL;
}

/**
 * Take one look at the processes of the installed script.
 *
 * The script is the moment: an id it holds gets its image written and is counted, and one it
 * does not hold leaves its image empty.
 */
static NODISCARD size_t take_snapshot(struct process_name *const names, size_t const count, struct ov_error *const err) {
  size_t found_count = 0;

  if (!g_installed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return SIZE_MAX;
  }
  if (count > 0 && names == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return SIZE_MAX;
  }
  for (size_t i = 0; i < count; i++) {
    struct mock_entry const *const entry = find_entry(names[i].pid);
    size_t const name_len = (entry != NULL) ? wcslen(entry->entry.image) : 0;

    if ((name_len == 0) || (name_len >= PROCESS_IMAGE_MAX)) {
      names[i].image[0] = L'\0'; // the script holds no such process, or a name too long to carry
      continue;
    }
    memcpy(names[i].image, entry->entry.image, (name_len + 1) * sizeof(names[i].image[0]));
    found_count++;
  }
  return found_count;
}

static NODISCARD bool end_process(uint32_t const pid, uint64_t const event_time_ms, struct ov_error *const err) {
  struct mock_entry *const entry = find_entry(pid);

  if (!g_installed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  g_ends++;
  if (g_script.open_fails) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_fail, "the stand-in refused the open");
    return false;
  }
  if (entry == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (entry->entry.start_ms > event_time_ms) {
    // the id moved on to a process younger than the record
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_not_found,
                  "the process behind pid %1$lu is no longer the one the record names",
                  "the process behind pid %1$lu is no longer the one the record names",
                  (unsigned long)pid);
    return false;
  }
  if (g_script.terminate_stops) {
    // the ask to stop came while the process was waited for, so the close has no answer
    OV_ERROR_SET_GENERIC(err, ov_error_generic_abort);
    return false;
  }
  if (!entry->entry.ends) {
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "the process behind pid %1$lu did not exit within 5 seconds",
                  "the process behind pid %1$lu did not exit within 5 seconds",
                  (unsigned long)pid);
    return false;
  }
  entry->ended = true;
  return true;
}

static NODISCARD bool pause_service(struct ov_error *const err) {
  if (!g_installed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  g_service_pauses++;
  if (g_script.pause_fails) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  return true;
}

static NODISCARD bool resume_service(struct ov_error *const err) {
  if (!g_installed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  g_service_resumes++;
  if (g_script.resume_fails) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  return true;
}

/** @brief One act of the stand-in, answered as soon as it is asked for */
struct process_act {
  async_notify_fn notify; //!< the caller's call, may be NULL
  void *userdata;         //!< handed to notify
  struct ov_error *err;   //!< borrowed, the caller's
  bool ok;                //!< whether the act went through
};

static void act_done(struct process_act *const act) {
  if (act->notify != NULL) {
    act->notify(act->userdata, false);
  }
}

static NODISCARD struct process_act *act_start(async_notify_fn const notify, void *const userdata, struct ov_error *const err) {
  struct process_act *act = NULL;

  if (!OV_REALLOC(&act, 1, sizeof(*act))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return NULL;
  }
  memset(act, 0, sizeof(*act));
  act->notify = notify;
  act->userdata = userdata;
  act->err = err;
  return act;
}

static NODISCARD struct process_act *terminate(
    uint32_t const pid, uint64_t const event_time_ms, async_notify_fn const notify, void *const userdata, struct ov_error *const err) {
  struct process_act *const act = act_start(notify, userdata, err);

  if (act == NULL) {
    return NULL;
  }
  act->ok = end_process(pid, event_time_ms, act->err);
  act_done(act);
  return act;
}

static NODISCARD struct process_act *pause_audio_service(async_notify_fn const notify, void *const userdata, struct ov_error *const err) {
  struct process_act *const act = act_start(notify, userdata, err);

  if (act == NULL) {
    return NULL;
  }
  act->ok = pause_service(act->err);
  act_done(act);
  return act;
}

static NODISCARD struct process_act *resume_audio_service(async_notify_fn const notify, void *const userdata, struct ov_error *const err) {
  struct process_act *const act = act_start(notify, userdata, err);

  if (act == NULL) {
    return NULL;
  }
  act->ok = resume_service(act->err);
  act_done(act);
  return act;
}

static NODISCARD bool act_release(struct process_act **const act) {
  bool ok = false;

  if ((act == NULL) || (*act == NULL)) {
    return false;
  }
  ok = (*act)->ok;
  OV_FREE(act);
  return ok;
}

static void act_cancel(struct process_act *const act) {
  (void)act; // the stand-in is already answered by the time it is asked for
}

/** @brief The stand-in machine, answering what the installed script says */
static struct process_manager const kMockProcessManager = {
    .take_snapshot = take_snapshot,
    .terminate = terminate,
    .pause_audio_service = pause_audio_service,
    .resume_audio_service = resume_audio_service,
    .act_release = act_release,
    .act_cancel = act_cancel,
};

static struct process_manager const *manager(void) { return &kMockProcessManager; }

bool process_mock_install(struct process_mock_script const *const script) {
  if (script == NULL) {
    return false;
  }
  process_mock_reset();
  if (script->entry_count > 0) {
    if (!OV_ARRAY_GROW(&g_entries, script->entry_count)) {
      return false;
    }
    for (size_t i = 0; i < script->entry_count; i++) {
      g_entries[i] = (struct mock_entry){.entry = script->entries[i]};
    }
    OV_ARRAY_SET_LENGTH(g_entries, script->entry_count);
  }
  g_script = *script;
  g_installed = true;
  process_manager_select(manager());
  return true;
}

size_t process_mock_end_count(void) { return g_ends; }

size_t process_mock_service_pause_count(void) { return g_service_pauses; }

size_t process_mock_service_resume_count(void) { return g_service_resumes; }

bool process_mock_ended(uint32_t const pid) {
  struct mock_entry const *const entry = find_entry(pid);

  return (entry != NULL) && entry->ended;
}

void process_mock_reset(void) {
  if (g_entries != NULL) {
    OV_ARRAY_DESTROY(&g_entries);
  }
  g_script = (struct process_mock_script){0};
  g_ends = 0;
  g_service_pauses = 0;
  g_service_resumes = 0;
  g_installed = false;
  process_manager_select(NULL);
}

#endif // ADC_MOCK
