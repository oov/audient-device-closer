#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef ADC_MOCK

/**
 * @brief One record a stand-in log hands back
 */
struct veto_log_mock_record {
  uint32_t pid;                   //!< the process the record names
  uint64_t event_time_ms;         //!< the moment of the record
  wchar_t const *path;            //!< what the process was called then, may be NULL
  wchar_t const *device_instance; //!< the device the record names, may be NULL
};

/**
 * @brief What a stand-in log answers
 */
struct veto_log_mock_script {
  struct veto_log_mock_record const *records;     //!< what the first read hands back, may be NULL
  size_t record_count;                            //!< how many records that is
  struct veto_log_mock_record const *second_read; //!< what a later read hands back, NULL to keep answering with records
  size_t second_read_count;                       //!< how many records that is
  bool read_fails;                                //!< whether the read itself fails
};

/**
 * @brief Make this program read a stand-in log from here on
 *
 * A read is answered with the records the script names, and the second_read ones from the
 * second read on: that is how a run sees a record written after the one it started from.
 * The records are read as they are, so they and the strings they name must outlive the
 * stand-in.
 *
 * @param script what the log answers, may not be NULL
 * @return false when the script could not be taken
 */
bool veto_log_mock_install(struct veto_log_mock_script const *script);

/**
 * @brief How often the log was read since the stand-in was installed
 *
 * @return the number of reads
 */
size_t veto_log_mock_read_count(void);

/**
 * @brief The moment the last read was made with
 *
 * @return the moment the caller handed over as since_ms
 */
uint64_t veto_log_mock_since_ms(void);

/**
 * @brief The bound the last read was made with
 *
 * @return the bound the caller handed over as max_wait_ms
 */
size_t veto_log_mock_max_wait_ms(void);

/**
 * @brief Go back to the log of the system
 */
void veto_log_mock_reset(void);

#endif // ADC_MOCK
