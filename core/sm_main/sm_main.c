/* sm.c - Main state machine
 *
 * Copyright (C) 2018 Arribada
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <math.h>
#include "sm_main.h"
#include "sm_iot.h"
#include "buffer.h"
#include "cmd.h"
#include "config_if.h"
#include "crc32.h"
#include "debug.h"
#include "exceptions.h"
#include "fs.h"
#include "sm.h"
#include "sys_config.h"
#include "logging.h"
#include "syshal_axl.h"
#include "syshal_batt.h"
#include "syshal_button.h"
#include "syshal_gpio.h"
#include "syshal_gps.h"
#include "syshal_firmware.h"
#include "syshal_flash.h"
#include "syshal_i2c.h"
#include "syshal_pmu.h"
#include "syshal_pressure.h"
#include "syshal_qspi.h"
#include "syshal_rtc.h"
#include "syshal_spi.h"
#include "syshal_switch.h"
#include "syshal_temp.h"
#include "syshal_time.h"
#include "syshal_timer.h"
#include "syshal_uart.h"
#include "syshal_usb.h"
#include "syshal_ble.h"
#include "version.h"
#include "syshal_cellular.h"
#include "syshal_device.h"
#include "syshal_led.h"

#undef MIN
#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

#ifdef GTEST
#define __NOP() do { } while(0)
#endif

////////////////////////////////////////////////////////////////////////////////
//////////////////////////////// MAIN STATES ///////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void sm_main_boot(sm_handle_t * state_handle);
static void sm_main_error(sm_handle_t * state_handle);
static void sm_main_battery_charging(sm_handle_t * state_handle);
static void sm_main_battery_level_low(sm_handle_t * state_handle);
static void sm_main_log_file_full(sm_handle_t * state_handle);
static void sm_main_provisioning_needed(sm_handle_t * state_handle);
static void sm_main_provisioning(sm_handle_t * state_handle);
static void sm_main_operational(sm_handle_t * state_handle);

sm_state_func_t sm_main_states[] =
{
    [SM_MAIN_BOOT] = sm_main_boot,
    [SM_MAIN_ERROR] = sm_main_error,
    [SM_MAIN_BATTERY_CHARGING] = sm_main_battery_charging,
    [SM_MAIN_BATTERY_LEVEL_LOW] = sm_main_battery_level_low,
    [SM_MAIN_LOG_FILE_FULL] = sm_main_log_file_full,
    [SM_MAIN_PROVISIONING_NEEDED] = sm_main_provisioning_needed,
    [SM_MAIN_PROVISIONING] = sm_main_provisioning,
    [SM_MAIN_OPERATIONAL] = sm_main_operational
};

#ifndef DEBUG_DISABLED
static const char * sm_main_state_str[] =
{
    [SM_MAIN_BOOT]                 = "SM_MAIN_BOOT",
    [SM_MAIN_ERROR]                = "SM_MAIN_ERROR",
    [SM_MAIN_BATTERY_CHARGING]     = "SM_MAIN_BATTERY_CHARGING",
    [SM_MAIN_BATTERY_LEVEL_LOW]    = "SM_MAIN_BATTERY_LEVEL_LOW",
    [SM_MAIN_LOG_FILE_FULL]        = "SM_MAIN_LOG_FILE_FULL",
    [SM_MAIN_PROVISIONING_NEEDED]  = "SM_MAIN_PROVISIONING_NEEDED",
    [SM_MAIN_PROVISIONING]         = "SM_MAIN_PROVISIONING",
    [SM_MAIN_OPERATIONAL]          = "SM_MAIN_OPERATIONAL",
};
#endif

////////////////////////////////////////////////////////////////////////////////
////////////////////////////// MESSAGE STATES //////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

typedef enum
{
    SM_MESSAGE_STATE_IDLE,
    SM_MESSAGE_STATE_CFG_READ_NEXT,
    SM_MESSAGE_STATE_CFG_WRITE_NEXT,
    SM_MESSAGE_STATE_CFG_WRITE_ERROR,
    SM_MESSAGE_STATE_GPS_WRITE_NEXT,
    SM_MESSAGE_STATE_GPS_READ_NEXT,
    SM_MESSAGE_STATE_LOG_READ_NEXT,
    SM_MESSAGE_STATE_FW_SEND_IMAGE_NEXT,
    SM_MESSAGE_STATE_CELLULAR_WRITE_NEXT,
    SM_MESSAGE_STATE_CELLULAR_READ_NEXT,
    SM_MESSAGE_STATE_FLASH_DOWNLOAD_NEXT,
} sm_message_state_t;

static sm_message_state_t message_state = SM_MESSAGE_STATE_IDLE;

// State specific context, used for maintaining information between config_if message sub-states
typedef struct
{
    union
    {
        struct
        {
            uint32_t length;
            uint8_t error_code;
            uint8_t buffer[SYS_CONFIG_TAG_MAX_SIZE];
            uint32_t buffer_occupancy;
        } cfg_write;

        struct
        {
            uint8_t * buffer_base;
            uint32_t  length;
            uint32_t  buffer_offset;
            uint16_t  last_index;
        } cfg_read;

        struct
        {
            uint32_t  length;
        } gps_write;

        struct
        {
            uint32_t  length;
        } gps_read;

        struct
        {
            uint32_t  length;
        } cellular_write;

        struct
        {
            uint32_t  length;
        } cellular_read;

        struct
        {
            uint32_t length;
            uint32_t start_offset;
        } log_read;

        struct
        {
            uint8_t file_id;
            uint32_t length;
            uint32_t crc32_supplied;
            uint32_t crc32_calculated;
        } fw_send_image;

        struct
        {
            uint32_t length;
            uint32_t address;
        } flash_download;
    };
} sm_context_t;
static sm_context_t sm_context;

////////////////////////////////////////////////////////////////////////////////
//////////////////////////////// GPS STATES ////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

typedef enum
{
    SM_GPS_STATE_ASLEEP,
    SM_GPS_STATE_ACQUIRING,
    SM_GPS_STATE_FIXED,
} sm_gps_state_t;

typedef enum
{
    SM_TEST_OFF,
    SM_TEST_REQUEST,
    SM_TEST_WAITING,
    SM_TEST_ACTIVE,
    SM_TEST_FINISHING,
} sm_test_state_t;

static volatile sm_gps_state_t sm_gps_state; // The current operating state of the GPS

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////// GLOBALS /////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

#define LOG_FILE_FLUSH_PERIOD_SECONDS ( (1 * 60 * 60) - 60 ) // Period in seconds in which to flush the log file to FLASH

// Size of logging buffer that is used to store sensor data before it it written to FLASH
#define LOGGING_FIFO_DEPTH  (32) // Maximum number of readings that can be stored before a write to the FLASH log must be done

#define USB_ENUMERATION_TIMEOUT_MS (10000) // Time in ms to try for a USB connection interface when VUSB is connected

#define SM_MAIN_INACTIVITY_TIMEOUT_MS (5000) // How many ms until the message state machine reverts back to idle

#define GPS_WATCHDOG_TIME_SECONDS (4) // How many seconds until we deem the GPS unresponsive

#define REED_SWITCH_DEBOUNCE_TIME_S (2) // How many seconds to debounce the reed switch

#define SOFT_WATCHDOG_TIMEOUT_S     (10 * 60) // How many seconds to allow before soft watchdog trips

#define GPS_RESTART_TIME_MS (10) // How long to hold the power line low on the GPS device if it is unresponsive

#define KICK_WATCHDOG()   syshal_rtc_soft_watchdog_refresh()

#define BUTTON_SATELLITE_TEST_MS  (5000)
#define BUTTON_CELLULAR_TEST_MS   (3000)
#define BUTTON_GPS_TEST_MS        (1000)

#define UNUSED(x) ((void)(x))

#define LED_DURATION_MS (5000)


#define LED_BLINK_FAIL_DURATION_MS  (100)

#define LED_BLINK_TEST_PASSED_DURATION_MS    (2 * LED_BLINK_FAIL_DURATION_MS)

static volatile bool     config_if_tx_pending = false;
static volatile bool     config_if_rx_queued = false;
static bool              syshal_gps_bridging = false;
static bool              gps_interval_using_max = false;
static bool              syshal_ble_bridging = false;
static bool              syshal_cellular_bridging = false;
#ifdef GTEST
static bool              system_startup_log_required = false;
#else
static bool              system_startup_log_required = true;
#endif
static volatile buffer_t config_if_send_buffer;
static volatile buffer_t config_if_receive_buffer;
static volatile buffer_t logging_buffer;
static volatile uint8_t  config_if_send_buffer_pool[SYSHAL_USB_PACKET_SIZE * 2];
static volatile uint8_t  config_if_receive_buffer_pool[SYSHAL_USB_PACKET_SIZE];
static volatile uint8_t  logging_buffer_pool[(LOGGING_MAX_SIZE + sizeof(logging_date_time_t)) * LOGGING_FIFO_DEPTH];
static uint32_t          config_if_message_timeout;
static volatile bool     config_if_connected = false;
static volatile bool     tracker_above_water = true; // Is the device above water?
static volatile bool     log_file_created = false; // Does a log file exist?
static volatile bool     gps_ttff_reading_logged = false; // Have we read the most recent gps ttff reading?
static uint8_t           last_battery_reading;
static volatile bool     sensor_logging_enabled = false; // Are sensors currently allowed to log
static uint8_t           ble_state;
static volatile bool     reed_switch_debounce = false; // Debouncing on the reed switch
static uint32_t          gps_positions_counts; // Number of GPS fixes since we last connected over IoT
static uint32_t          gps_positions_this_fix; // Number of GPS fixes since this gps lock
static bool              ble_one_shot_used = true; // Only allow once ble one shot
static sm_test_state_t   test_state_cellular = SM_TEST_OFF;
static sm_test_state_t   test_state_gps = SM_TEST_OFF;
static sm_test_state_t   test_state_satellite = SM_TEST_OFF;
static uint32_t          led_finish_time;


fs_t file_system; // non-static to allow access from other files

#ifndef GTEST
static fs_handle_t       sm_main_file_handle; // The global file handle we have open. Only allow one at once
#else
fs_handle_t sm_main_file_handle; // non-static to allow unit tests access to it
#endif

// Timer handles
static timer_handle_t timer_gps_interval;
static timer_handle_t timer_gps_no_fix;
static timer_handle_t timer_gps_maximum_acquisition;
static timer_handle_t timer_gps_test_fix_hold_time;
static timer_handle_t timer_gps_watchdog;
static timer_handle_t timer_log_flush;
static timer_handle_t timer_saltwater_switch_hysteresis;
static timer_handle_t timer_reed_switch_hysteresis;
static timer_handle_t timer_pressure_interval;
static timer_handle_t timer_pressure_maximum_acquisition;
static timer_handle_t timer_axl_interval;
static timer_handle_t timer_axl_maximum_acquisition;
static timer_handle_t timer_ble_interval;
static timer_handle_t timer_ble_duration;
static timer_handle_t timer_ble_timeout;

// Timer callbacks
static void timer_gps_interval_callback(void);
static void timer_gps_no_fix_callback(void);
static void timer_gps_maximum_acquisition_callback(void);
static void timer_gps_test_fix_hold_time_callback(void);
static void timer_gps_watchdog_callback(void);
static void timer_log_flush_callback(void);
static void timer_saltwater_switch_hysteresis_callback(void);
static void timer_reed_switch_hysteresis_callback(void);
static void timer_pressure_interval_callback(void);
static void timer_pressure_maximum_acquisition_callback(void);
static void timer_axl_interval_callback(void);
static void timer_axl_maximum_acquisition_callback(void);
static void timer_ble_interval_callback(void);
static void timer_ble_duration_callback(void);
static void timer_ble_timeout_callback(void);
static void soft_watchdog_callback(unsigned int);

////////////////////////////////////////////////////////////////////////////////
///////////////////////////////// PROTOTYPES ///////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void config_if_timeout_reset(void);
static void message_set_state(sm_message_state_t s);

////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////// STARTUP ////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void setup_buffers(void)
{
    // Send buffer
    buffer_init_policy(pool, &config_if_send_buffer,
                       (uintptr_t) &config_if_send_buffer_pool[0],
                       sizeof(config_if_send_buffer_pool), 2);

    // Receive buffer
    buffer_init_policy(pool, &config_if_receive_buffer,
                       (uintptr_t) &config_if_receive_buffer_pool[0],
                       sizeof(config_if_receive_buffer_pool), 1);

    // Logging buffer
    buffer_init_policy(pool, &logging_buffer,
                       (uintptr_t) &logging_buffer_pool[0],
                       sizeof(logging_buffer_pool), LOGGING_FIFO_DEPTH);
}

// Set all global varibles to their default values
// this is used to allow unit tests to start from a clean slate
static void set_default_global_values(void)
{
    message_state = SM_MESSAGE_STATE_IDLE;
    config_if_tx_pending = false;
    config_if_rx_queued = false;
    syshal_gps_bridging = false;
    gps_interval_using_max = false;
    syshal_ble_bridging = false;
    syshal_cellular_bridging = false;
    config_if_message_timeout = 0;
    config_if_connected = false;
    tracker_above_water = true;
    log_file_created = false;
    gps_ttff_reading_logged = false;
    last_battery_reading = 0;
    sensor_logging_enabled = false;
    ble_state = 0;
    sm_main_file_handle = 0;
    reed_switch_debounce = false;
    ble_one_shot_used = true;
    test_state_cellular = SM_TEST_OFF;
    test_state_gps = SM_TEST_OFF;
    test_state_satellite = SM_TEST_OFF;

#ifdef GTEST
    system_startup_log_required = false;
#else
    system_startup_log_required = true;
#endif
    gps_positions_counts = 0;
    gps_positions_this_fix = 0;
}

////////////////////////////////////////////////////////////////////////////////
/////////////////////////////// HELPER FUNCTIONS ///////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void soft_watchdog_callback(unsigned int lr)
{
    logging_soft_watchdog_t log_wdog;
    uint32_t bytes_written;

    if (sys_config.logging_date_time_stamp_enable.contents.enable)
    {
        syshal_rtc_data_and_time_t current_time;
        logging_date_time_t log_date;

        syshal_rtc_get_date_and_time(&current_time);

        log_date.h.id = LOGGING_DATE_TIME;
        log_date.day = current_time.day;
        log_date.month = current_time.month;
        log_date.year = current_time.year;
        log_date.hours = current_time.hours;
        log_date.minutes = current_time.minutes;
        log_date.seconds = current_time.seconds;
        (void)fs_write(sm_main_file_handle, &log_date, sizeof(log_date), &bytes_written);
    }

    log_wdog.h.id = LOGGING_SOFT_WDOG;
    log_wdog.watchdog_address = lr;
    (void)fs_write(sm_main_file_handle, &log_wdog, sizeof(log_wdog), &bytes_written);

    /* Try to clean-up the log file since we are about to reset */
    (void)fs_close(sm_main_file_handle);

    /* Execute a software reset */
    for (;;)
        syshal_pmu_reset();
}

static void config_if_send_priv(volatile buffer_t * buffer)
{
    if (config_if_tx_pending)
        Throw(EXCEPTION_TX_BUSY);

    uintptr_t addr;
    uint32_t length = buffer_read(buffer, &addr);

    if (length)
    {
        config_if_tx_pending = true;
        config_if_send((uint8_t *) addr, length); // Send response
    }
    else
    {
        Throw(EXCEPTION_TX_BUFFER_FULL);
    }
}

static bool config_if_receive_byte_stream_priv(uint32_t length)
{
    if (!config_if_rx_queued)
    {
        // Queue receive
        uint8_t * receive_buffer;
        if (!buffer_write(&config_if_receive_buffer, (uintptr_t *)&receive_buffer))
            Throw(EXCEPTION_RX_BUFFER_FULL);

        // Set our flag to true before calling config_if_receive as it may internally
        // call config_if_callback which in turn would set this flag to false
        config_if_rx_queued = true;
        if (CONFIG_IF_NO_ERROR != config_if_receive_byte_stream(receive_buffer, length))
            config_if_rx_queued = false;

        return true;
    }

    return false;
}

static bool config_if_receive_priv(void)
{
    if (!config_if_rx_queued)
    {
        // Queue receive
        uint8_t * receive_buffer;
        if (!buffer_write(&config_if_receive_buffer, (uintptr_t *)&receive_buffer))
            Throw(EXCEPTION_RX_BUFFER_FULL);

        // Set our flag to true before calling config_if_receive as it may internally
        // call config_if_callback which in turn would set this flag to false
        config_if_rx_queued = true;
        if (CONFIG_IF_NO_ERROR != config_if_receive(receive_buffer, SYSHAL_USB_PACKET_SIZE))
            config_if_rx_queued = false;

        return true;
    }

    return false;
}

/**
 * @brief      Determines if any essential configuration tags are not set
 *
 * @return     false if essential configuration tags are not set
 */
static bool check_configuration_tags_set(void)
{
    uint16_t tag, last_index = 0;

#ifndef DEBUG_DISABLED
    static uint16_t last_tag_warned_about = 0xFFFF;
#endif

    while (!sys_config_iterate(&tag, &last_index))
    {
        bool tag_required;
        bool tag_set;

        sys_config_is_required(tag, &tag_required);
        sys_config_is_set(tag, &tag_set);

        if (tag_required && !tag_set)
        {
#ifndef DEBUG_DISABLED
            if (last_tag_warned_about != tag)
            {
                last_tag_warned_about = tag;
                DEBUG_PR_WARN("Configuration tag 0x%04X required but not set", tag);
            }
#endif
            return false;
        }
    }

    return true;
}

void logging_add_to_buffer(uint8_t * data, uint32_t size)
{
    uint32_t length = 0;
    uint8_t * buf_ptr;
    if (!buffer_write(&logging_buffer, (uintptr_t *)&buf_ptr))
    {
        DEBUG_PR_ERROR("LOG BUFFER FULL");
        return; // If our logging buffer is full then just ignore this data
    }

    static syshal_rtc_data_and_time_t last_log_time;

    // Are we supposed to be adding a timestamp with this value?
    if (sys_config.logging_date_time_stamp_enable.hdr.set &&
        sys_config.logging_date_time_stamp_enable.contents.enable)
    {
        syshal_rtc_data_and_time_t current_time;
        bool log_time = true;

        syshal_rtc_get_date_and_time(&current_time);

        // Are we supposed to be grouping every log entry that happens within the same second together?
        if (sys_config.logging_group_sensor_readings_enable.hdr.set &&
            sys_config.logging_group_sensor_readings_enable.contents.enable)
        {
            // Has our time changed since the last log entry?
            if (last_log_time.year == current_time.year ||
                last_log_time.month == current_time.month ||
                last_log_time.day == current_time.day ||
                last_log_time.hours == current_time.hours ||
                last_log_time.minutes == current_time.minutes ||
                last_log_time.seconds == current_time.seconds)
            {
                last_log_time = current_time;
                log_time = false; // Time has not changed, so do not log it
            }

        }

        if (log_time)
        {
            logging_date_time_t * date_time = (logging_date_time_t *) buf_ptr;

            LOGGING_SET_HDR(date_time, LOGGING_DATE_TIME);

            date_time->year = current_time.year;
            date_time->month = current_time.month;
            date_time->day = current_time.day;
            date_time->hours = current_time.hours;
            date_time->minutes = current_time.minutes;
            date_time->seconds = current_time.seconds;

            buf_ptr += sizeof(logging_date_time_t);
            length += sizeof(logging_date_time_t);
        }
    }

    if (sys_config.logging_high_resolution_timer_enable.contents.enable)
    {
        DEBUG_PR_ERROR("logging_high_resolution_timer NOT IMPLEMENTED");
    }

    // Add the supplied data to the buffer
    memcpy(buf_ptr, data, size);
    length += size;

    buffer_write_advance(&logging_buffer, length);
}

void GPS_on(void)
{
    // Start GPS watchdog
    syshal_timer_set(timer_gps_watchdog, one_shot, GPS_WATCHDOG_TIME_SECONDS);

    if (SM_GPS_STATE_ASLEEP != sm_gps_state)
        return; // GPS already awake

    sm_gps_state = SM_GPS_STATE_ACQUIRING;
    gps_ttff_reading_logged = false;
    gps_positions_this_fix = 0;

    syshal_gps_wake_up();

    // Log the GPS switched on event
    if (sys_config.gps_debug_logging_enable.hdr.set &&
        sys_config.gps_debug_logging_enable.contents.enable)
    {
        logging_log_gps_on_t gps_on_log;
        LOGGING_SET_HDR(&gps_on_log, LOGGING_GPS_ON);
        logging_add_to_buffer((uint8_t *) &gps_on_log, sizeof(gps_on_log));
    }

    // Log the battery voltage level
    if (sys_config.battery_log_enable.hdr.set &&
        sys_config.battery_log_enable.contents.enable)
    {
        logging_battery_voltage_t battery_voltage;
        LOGGING_SET_HDR(&battery_voltage, LOGGING_BATTERY_VOLTAGE);
        if (SYSHAL_BATT_NO_ERROR == syshal_batt_voltage(&battery_voltage.millivolts))
            logging_add_to_buffer((uint8_t *) &battery_voltage, sizeof(battery_voltage));
    }
}

void GPS_off(void)
{
    if (SM_GPS_STATE_ASLEEP == sm_gps_state)
        return; // GPS already asleep

    // Stop GPS watchdog
    syshal_timer_cancel(timer_gps_watchdog);

    syshal_gps_shutdown();

    sm_gps_state = SM_GPS_STATE_ASLEEP;

    // Log the GPS switched off event
    if (sys_config.gps_debug_logging_enable.hdr.set &&
        sys_config.gps_debug_logging_enable.contents.enable)
    {
        logging_log_gps_off_t gps_off_log;
        LOGGING_SET_HDR(&gps_off_log, LOGGING_GPS_OFF);
        logging_add_to_buffer((uint8_t *) &gps_off_log, sizeof(gps_off_log));
    }
}

void GPS_off_no_log(void)
{
    if (SM_GPS_STATE_ASLEEP == sm_gps_state)
        return; // GPS already asleep

    // Stop GPS watchdog
    syshal_timer_cancel(timer_gps_watchdog);

    syshal_gps_shutdown();

    sm_gps_state = SM_GPS_STATE_ASLEEP;
}

// Start or stop BLE based on ble_state triggers
void manage_ble(void)
{
    // Should we start out BLE scheduled or one_shot timer?
    if ( sys_config.tag_bluetooth_scheduled_duration.hdr.set &&
         sys_config.tag_bluetooth_trigger_control.hdr.set)
    {
        // Only run scheduled mode if schedued is set and one_shot isn't set
        if (sys_config.tag_bluetooth_scheduled_interval.hdr.set &&
            (sys_config.tag_bluetooth_trigger_control.contents.flags & SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_SCHEDULED)
            && !(sys_config.tag_bluetooth_trigger_control.contents.flags & SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_ONE_SHOT))
        {
            if (sys_config.tag_bluetooth_scheduled_interval.contents.seconds)
            {
                if (SYSHAL_TIMER_NOT_RUNNING == syshal_timer_running(timer_ble_interval))
                    syshal_timer_set(timer_ble_interval, periodic, sys_config.tag_bluetooth_scheduled_interval.contents.seconds);
            }
            else
            {
                ble_state |= SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_SCHEDULED; // As the scheduled interval = 0 this is a special case to mean bluetooth is always on
            }
        }
        else if (sys_config.tag_bluetooth_trigger_control.contents.flags & SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_ONE_SHOT)
        {
            if (!ble_one_shot_used)
            {
                ble_one_shot_used = true;
                ble_state |= SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_ONE_SHOT;
                syshal_timer_set(timer_ble_duration, one_shot, sys_config.tag_bluetooth_scheduled_duration.contents.seconds);
            }
        }
    }
    else
    {
        syshal_timer_cancel(timer_ble_interval);
    }

    if (ble_state &&
        CONFIG_IF_BACKEND_NOT_SET == config_if_current())
    {
        // Should we log this event
        if (sys_config.tag_bluetooth_log_enable.hdr.set &&
            sys_config.tag_bluetooth_log_enable.contents.enable)
        {
            logging_ble_enabled_t ble_enabled;
            LOGGING_SET_HDR(&ble_enabled, LOGGING_BLE_ENABLED);

            if (ble_state & SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_REED_SWITCH)
                ble_enabled.cause = LOGGING_BLE_ENABLED_CAUSE_REED_SWITCH;
            else if (ble_state & SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_SCHEDULED)
                ble_enabled.cause = LOGGING_BLE_ENABLED_CAUSE_SCHEDULE_TIMER;
            else if (ble_state & SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_GEOFENCE)
                ble_enabled.cause = LOGGING_BLE_ENABLED_CAUSE_GEOFENCE;
            else if (ble_state & SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_ONE_SHOT)
                ble_enabled.cause = LOGGING_BLE_ENABLED_CAUSE_ONE_SHOT;

            logging_add_to_buffer((uint8_t *) &ble_enabled, sizeof(ble_enabled));
        }

        config_if_backend_t backend = {.id = CONFIG_IF_BACKEND_BLE};
        config_if_init(backend);
    }
}

static void setup_GPS_based_on_configuration(void)
{
    // GPS switch activated trigger mode
    if (SYS_CONFIG_GPS_TRIGGER_MODE_SWITCH_TRIGGERED == sys_config.gps_trigger_mode.contents.mode)
    {
        if (tracker_above_water)
        {
            GPS_on();

            // Do we have a maximum acquisition time to adhere to?
            if (sys_config.gps_maximum_acquisition_time.contents.seconds)
                syshal_timer_set(timer_gps_maximum_acquisition, one_shot, sys_config.gps_maximum_acquisition_time.contents.seconds);
        }
        else
        {
            GPS_off();
        }
    }

    // GPS scheduled trigger mode
    if (SYS_CONFIG_GPS_TRIGGER_MODE_SCHEDULED == sys_config.gps_trigger_mode.contents.mode)
    {
        if (sys_config.gps_scheduled_acquisition_interval.contents.seconds)
        {
            GPS_off();
            syshal_timer_set(timer_gps_interval, periodic, sys_config.gps_scheduled_acquisition_interval.contents.seconds);
        }
        else
        {
            GPS_on();
        }
    }

    // GPS hybrid trigger mode
    if (SYS_CONFIG_GPS_TRIGGER_MODE_HYBRID == sys_config.gps_trigger_mode.contents.mode)
    {

        if (sys_config.gps_scheduled_acquisition_interval.contents.seconds)
        {
            // If we're above the surface than turn on the GPS
            if (tracker_above_water)
            {
                GPS_on();

                // Do we have a maximum acquisition time to adhere to?
                if (sys_config.gps_maximum_acquisition_time.contents.seconds)
                    syshal_timer_set(timer_gps_maximum_acquisition, one_shot, sys_config.gps_maximum_acquisition_time.contents.seconds);
            }
            else
            {
                GPS_off();
            }

            // Start our interval timer
            syshal_timer_set(timer_gps_interval, periodic, sys_config.gps_scheduled_acquisition_interval.contents.seconds);
        }
        else
        {
            GPS_on();
        }

    }
}

static inline bool is_test_active_or_finishing_or_led_active(void)
{
    return ((test_state_cellular > SM_TEST_WAITING)  || (test_state_gps > SM_TEST_WAITING) || (test_state_satellite >  SM_TEST_WAITING) || syshal_led_is_active());
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////// CALLBACK FUNCTIONS //////////////////////////////
////////////////////////////////////////////////////////////////////////////////

void syshal_button_callback(syshal_button_event_t event)
{
    if (SYSHAL_BUTTON_RELEASED != event.id)
        return;

    if (event.released.duration_ms > BUTTON_SATELLITE_TEST_MS)
    {
        if (SM_TEST_OFF == test_state_satellite)
        {
            test_state_satellite = SM_TEST_FINISHING;
            return;
        }
    }

    if (event.released.duration_ms > BUTTON_CELLULAR_TEST_MS)
    {
        if (SM_TEST_OFF == test_state_cellular)
        {
            test_state_cellular = SM_TEST_FINISHING;
            return;
        }
    }

    if (event.released.duration_ms > BUTTON_GPS_TEST_MS)
    {
        if (SM_TEST_OFF == test_state_gps)
        {
            test_state_gps = SM_TEST_FINISHING;
            return;
        }
    }
}

void syshal_axl_callback(syshal_axl_data_t data)
{
    // If accelerometer data logging is disabled
    if (!sys_config.axl_log_enable.contents.enable)
    {
        syshal_axl_sleep(); // Sleep the accelerometer device
        return;
    }

    if (!sensor_logging_enabled)
        return;

    switch (sys_config.axl_mode.contents.mode)
    {
        case SYS_CONFIG_AXL_MODE_PERIODIC:
            __NOP(); // NOP required to give the switch case an instruction to jump to
            logging_axl_xyz_t axl;
            LOGGING_SET_HDR(&axl, LOGGING_AXL_XYZ);
            axl.x = data.x;
            axl.y = data.y;
            axl.z = data.z;
            logging_add_to_buffer((uint8_t *) &axl, sizeof(axl));
            break;

        case SYS_CONFIG_AXL_MODE_TRIGGER_ABOVE:
            // Calculate vector magnitude
            __NOP(); // NOP required to give the switch case an instruction to jump to
            uint16_t magnitude_squared = (data.x * data.x) + (data.y * data.y) + (data.z * data.z); // WARN uint16_t maybe too small to contain true value
            // Determine if the read data is above the trigger point
            if (magnitude_squared >= sys_config.axl_g_force_high_threshold.contents.threshold)
                __NOP(); // FIXME: Log data!
            break;
    }
}

void syshal_pressure_callback(int32_t pressure)
{
    // If pressure logging is disabled
    if (!sys_config.pressure_sensor_log_enable.contents.enable)
    {
        syshal_pressure_sleep(); // Sleep the pressure device
        return;
    }

    if (!sensor_logging_enabled)
        return;

    logging_pressure_t pressure_data;
    LOGGING_SET_HDR(&pressure_data, LOGGING_PRESSURE);
    pressure_data.pressure = pressure;
    logging_add_to_buffer((uint8_t *) &pressure_data, sizeof(pressure_data));
}

void syshal_gps_callback(syshal_gps_event_t event)
{
    // Kick the GPS watchdog
    syshal_timer_reset(timer_gps_watchdog);

    // If gps data logging is disabled
    if (!sys_config.gps_log_position_enable.contents.enable &&
        !sys_config.gps_log_ttff_enable.contents.enable)
    {
        GPS_off();
        return;
    }

    if (!sensor_logging_enabled)
        return;

    switch (event.event_id)
    {
        case SYSHAL_GPS_EVENT_STATUS:
            DEBUG_PR_TRACE("SYSHAL_GPS_EVENT_STATUS - Fix: %u", event.event_data.status.gpsFix);

            if (event.event_data.status.gpsFix > 0)
            {
                if (SM_GPS_STATE_ASLEEP != sm_gps_state) // If we haven't already slept the GPS
                {
                    syshal_timer_cancel(timer_gps_no_fix); // Clear any no fix timer
                    sm_gps_state = SM_GPS_STATE_FIXED;
                }

                // If TTFF logging is enabled then log this
                if (!gps_ttff_reading_logged &&
                    sys_config.gps_log_ttff_enable.hdr.set &&
                    sys_config.gps_log_ttff_enable.contents.enable)
                {
                    logging_gps_ttff_t gps_ttff;

                    LOGGING_SET_HDR(&gps_ttff, LOGGING_GPS_TTFF);
                    gps_ttff.ttff = event.event_data.status.ttff;
                    logging_add_to_buffer((uint8_t *) &gps_ttff, sizeof(gps_ttff));

                    gps_ttff_reading_logged = true;
                }
            }
            else
            {
                if (SM_GPS_STATE_ASLEEP != sm_gps_state) // If we haven't already slept the GPS
                {
                    // Have we just lost GPS fix?
                    if (SM_GPS_STATE_FIXED == sm_gps_state)
                    {
                        // Are we supposed to be scheduling a no fix timer?
                        // If our interval time is 0 this is a special case meaning run the GPS forever
                        if (sys_config.gps_scheduled_acquisition_interval.contents.seconds)
                        {
                            // If we are and we are either in scheduled mode or hybrid + underwater then start the no-fix timer
                            if ((SYS_CONFIG_GPS_TRIGGER_MODE_SCHEDULED == sys_config.gps_trigger_mode.contents.mode) ||
                                ((SYS_CONFIG_GPS_TRIGGER_MODE_HYBRID == sys_config.gps_trigger_mode.contents.mode) && (!tracker_above_water)))
                            {
                                if (sys_config.gps_scheduled_acquisition_no_fix_timeout.hdr.set &&
                                    sys_config.gps_scheduled_acquisition_no_fix_timeout.contents.seconds) // Don't set a no fix timeout if it's zero as this is a special case
                                    syshal_timer_set(timer_gps_no_fix, one_shot, sys_config.gps_scheduled_acquisition_no_fix_timeout.contents.seconds);
                            }
                        }
                    }

                    sm_gps_state = SM_GPS_STATE_ACQUIRING;
                }
            }
            break;

        case SYSHAL_GPS_EVENT_POSLLH:
            DEBUG_PR_TRACE("SYSHAL_GPS_EVENT_POSLLH - lat,long: %ld,%ld", event.event_data.location.lat, event.event_data.location.lon);

            // If we have a GPS fix
            if (SM_GPS_STATE_FIXED == sm_gps_state)
            {
                // Store this value into our last known location configuration interface tag
                sys_config.gps_last_known_position.hdr.set = true;
                sys_config.gps_last_known_position.contents.iTOW = event.event_data.location.iTOW;
                sys_config.gps_last_known_position.contents.lon = event.event_data.location.lon;
                sys_config.gps_last_known_position.contents.lat = event.event_data.location.lat;
                sys_config.gps_last_known_position.contents.height = event.event_data.location.hMSL;
                sys_config.gps_last_known_position.contents.hAcc = event.event_data.location.hAcc;
                sys_config.gps_last_known_position.contents.vAcc = event.event_data.location.vAcc;

                syshal_rtc_data_and_time_t current_time;
                syshal_rtc_get_date_and_time(&current_time);

                sys_config.gps_last_known_position.contents.day = current_time.day;
                sys_config.gps_last_known_position.contents.month = current_time.month;
                sys_config.gps_last_known_position.contents.year = current_time.year;
                sys_config.gps_last_known_position.contents.hours = current_time.hours;
                sys_config.gps_last_known_position.contents.minutes = current_time.minutes;
                sys_config.gps_last_known_position.contents.seconds = current_time.seconds;

                // Add data to be logged
                logging_gps_position_t position;

                LOGGING_SET_HDR(&position, LOGGING_GPS_POSITION);
                position.iTOW = event.event_data.location.iTOW;
                position.lon = event.event_data.location.lon;
                position.lat = event.event_data.location.lat;
                position.height = event.event_data.location.hMSL;
                position.hAcc = event.event_data.location.hAcc;
                position.vAcc = event.event_data.location.vAcc;

                logging_add_to_buffer((uint8_t *) &position, sizeof(position));

                gps_positions_counts++;
                gps_positions_this_fix++;

                // If we are only meant to get a certain number of fixes per connection
                if (test_state_gps != SM_TEST_ACTIVE)
                {
                    if (sys_config.gps_max_fixes.hdr.set && sys_config.gps_max_fixes.contents.fixes)
                    {
                        if (gps_positions_this_fix >= sys_config.gps_max_fixes.contents.fixes)
                        {
                            GPS_off();

                            // Stop our maximum acquisition timer as this is now not needed
                            syshal_timer_cancel(timer_gps_maximum_acquisition);
                        }
                    }
                }
            }
            break;

        default:
            DEBUG_PR_WARN("Unknown GPS event in %s() : %d", __FUNCTION__, event.event_id);
            break;
    }
}

void syshal_switch_callback(syshal_switch_event_id_t event)
{
    switch (event)
    {
        case SYSHAL_SWITCH_EVENT_OPEN:
            syshal_timer_cancel(timer_saltwater_switch_hysteresis);

            // If we're in the operational state and we were previously underwater
            if (sensor_logging_enabled && !tracker_above_water)
            {
                if (sys_config.saltwater_switch_log_enable.contents.enable)
                {
                    logging_surfaced_t surfaced;
                    LOGGING_SET_HDR(&surfaced, LOGGING_SURFACED);
                    logging_add_to_buffer((uint8_t *) &surfaced, sizeof(surfaced));
                }

                // Are we supposed to be waking the GPS?
                if (SYS_CONFIG_GPS_TRIGGER_MODE_SWITCH_TRIGGERED == sys_config.gps_trigger_mode.contents.mode
                    || SYS_CONFIG_GPS_TRIGGER_MODE_HYBRID == sys_config.gps_trigger_mode.contents.mode)
                {
                    GPS_on();

                    // If our maximum acquisition time is not set to forever (0)
                    if (sys_config.gps_maximum_acquisition_time.contents.seconds)
                        syshal_timer_set(timer_gps_maximum_acquisition, one_shot, sys_config.gps_maximum_acquisition_time.contents.seconds);
                }

                syshal_timer_cancel(timer_gps_no_fix); // We ignore the no fix timeout when on the surface
            }

            tracker_above_water = true;
            break;

        case SYSHAL_SWITCH_EVENT_CLOSED:
            if (sys_config.saltwater_switch_hysteresis_period.contents.seconds &&
                sys_config.saltwater_switch_hysteresis_period.hdr.set &&
                sensor_logging_enabled)
                syshal_timer_set(timer_saltwater_switch_hysteresis, one_shot, sys_config.saltwater_switch_hysteresis_period.contents.seconds);
            else
                timer_saltwater_switch_hysteresis_callback(); // Trigger an instant switch timeout
            break;

        default:
            DEBUG_PR_WARN("Unknown switch event in %s() : %d", __FUNCTION__, event);
            break;
    }
}

static void add_iot_status_log_entry(uint8_t status)
{
    logging_iot_status_t iot_status;
    LOGGING_SET_HDR(&iot_status, LOGGING_IOT_STATUS);
    iot_status.status = status;
    logging_add_to_buffer((uint8_t *) &iot_status, sizeof(iot_status));
}

void sm_iot_callback(sm_iot_event_t * event)
{
    switch (event->id)
    {
        case SM_IOT_CELLULAR_MAX_BACKOFF_REACHED:
            // If we have reached our max cellular backoff then use a new GPS interval time
            if (SYSHAL_TIMER_NOT_RUNNING == syshal_timer_running(timer_gps_interval))
                break;

            if (sys_config.iot_cellular_settings.contents.gps_schedule_interval_on_max_backoff)
            {
                syshal_timer_set(timer_gps_interval, periodic, sys_config.iot_cellular_settings.contents.gps_schedule_interval_on_max_backoff);
                gps_interval_using_max = true;
            }
            break;

        case SM_IOT_CELLULAR_SEND_DEVICE_STATUS:
            if (SM_IOT_NO_ERROR != event->code)
                break;
            if (SYSHAL_TIMER_NOT_RUNNING == syshal_timer_running(timer_gps_interval))
                break;

            // If we have had a successful cellular connection and we are currently using the maximum gps interval
            if (SM_IOT_NO_ERROR == event->code && gps_interval_using_max)
            {
                // Then use our original gps interval
                gps_interval_using_max = false;
                syshal_timer_set(timer_gps_interval, periodic, sys_config.gps_scheduled_acquisition_interval.contents.seconds);
            }
            break;

        default:
            break;
    }

    // Handle any logging that may need to be done
    if (!sys_config.iot_general_settings.hdr.set || !sys_config.iot_general_settings.contents.log_enable)
        return;

    switch (event->id)
    {
        case SM_IOT_CELLULAR_POWER_ON:
            add_iot_status_log_entry(LOGGING_IOT_STATUS_CELLULAR_POWERED_ON);
            break;

        case SM_IOT_CELLULAR_POWER_OFF:
            add_iot_status_log_entry(LOGGING_IOT_STATUS_CELLULAR_POWERED_OFF);
            break;

        case SM_IOT_CELLULAR_CONNECT:
            add_iot_status_log_entry(LOGGING_IOT_STATUS_CELLULAR_CONNECT);
            break;

        case SM_IOT_CELLULAR_FETCH_DEVICE_SHADOW:
            add_iot_status_log_entry(LOGGING_IOT_STATUS_CELLULAR_FETCH_DEVICE_SHADOW);
            break;

        case SM_IOT_CELLULAR_SEND_LOGGING:
            add_iot_status_log_entry(LOGGING_IOT_STATUS_CELLULAR_SEND_LOGGING);
            break;

        case SM_IOT_CELLULAR_SEND_DEVICE_STATUS:
            add_iot_status_log_entry(LOGGING_IOT_STATUS_CELLULAR_SEND_DEVICE_STATUS);
            break;

        case SM_IOT_CELLULAR_DOWNLOAD_FIRMWARE_FILE:
            add_iot_status_log_entry(LOGGING_IOT_STATUS_CELLULAR_DOWNLOAD_FIRMWARE_FILE);
            break;

        case SM_IOT_CELLULAR_DOWNLOAD_CONFIG_FILE:
            add_iot_status_log_entry(LOGGING_IOT_STATUS_CELLULAR_DOWNLOAD_CONFIG_FILE);
            break;

        case SM_IOT_CELLULAR_MAX_BACKOFF_REACHED:
            add_iot_status_log_entry(LOGGING_IOT_STATUS_CELLULAR_MAX_BACKOFF_REACHED);
            break;

        case SM_IOT_APPLY_FIRMWARE_UPDATE:
            __NOP();
            logging_iot_fw_update_t iot_fw_update;
            LOGGING_SET_HDR(&iot_fw_update, LOGGING_IOT_FW_UPDATE);
            iot_fw_update.version = event->firmware_update.version;
            iot_fw_update.length = event->firmware_update.length;
            logging_add_to_buffer((uint8_t *) &iot_fw_update, sizeof(iot_fw_update));
            break;

        case SM_IOT_APPLY_CONFIG_UPDATE:
            __NOP();
            logging_iot_config_update_t iot_config_update;
            LOGGING_SET_HDR(&iot_config_update, LOGGING_IOT_CONFIG_UPDATE);
            iot_config_update.version = event->config_update.version;
            iot_config_update.length = event->config_update.length;
            logging_add_to_buffer((uint8_t *) &iot_config_update, sizeof(iot_config_update));
            break;

        default:
            break;
    }

    if (event->code)
    {
        logging_iot_error_code_t iot_error_code;
        LOGGING_SET_HDR(&iot_error_code, LOGGING_IOT_ERROR_CODE);
        if (IOT_ERROR_HTTP == event->code)
            iot_error_code.error = iot_get_last_error();
        else
            iot_error_code.error = event->code;
        logging_add_to_buffer((uint8_t *) &iot_error_code, sizeof(iot_error_code));
    }
}

static void gpio_reed_sw_callback(syshal_gpio_event_t event)
{
    DEBUG_PR_TRACE("%s() state: %d", __FUNCTION__, syshal_gpio_get_input(GPIO_REED_SW));

    if (!reed_switch_debounce)
    {
        reed_switch_debounce = true;
        syshal_timer_set(timer_reed_switch_hysteresis, one_shot, REED_SWITCH_DEBOUNCE_TIME_S);

        // Should we be using the reed switch to trigger BLE activation?
        if (sys_config.tag_bluetooth_trigger_control.hdr.set &&
            sys_config.tag_bluetooth_trigger_control.contents.flags & SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_REED_SWITCH)
        {
            if (!syshal_gpio_get_input(GPIO_REED_SW))
            {
                ble_state |= SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_REED_SWITCH;
            }
            else
            {
                ble_state &= (uint8_t) ~SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_REED_SWITCH;

                // Was the reed switch the only reason the the BLE interface was running?
                if (!ble_state &&
                    CONFIG_IF_BACKEND_BLE == config_if_current())
                {
                    // Should we log this event
                    if (sys_config.tag_bluetooth_log_enable.hdr.set &&
                        sys_config.tag_bluetooth_log_enable.contents.enable)
                    {
                        logging_ble_enabled_t ble_disabled;
                        LOGGING_SET_HDR(&ble_disabled, LOGGING_BLE_DISABLED);
                        ble_disabled.cause = LOGGING_BLE_DISABLED_CAUSE_REED_SWITCH;
                        logging_add_to_buffer((uint8_t *) &ble_disabled, sizeof(ble_disabled));
                    }

                    // If so then terminate it
                    config_if_term();

                    if (config_if_connected)
                    {
                        // If the BLE device was connected, trigger a disconnect event
                        config_if_event_t disconnect_event;
                        disconnect_event.backend = CONFIG_IF_BACKEND_BLE;
                        disconnect_event.id = CONFIG_IF_EVENT_DISCONNECTED;
                        config_if_callback(&disconnect_event);
                    }
                }
            }
        }
    }
}

static void timer_gps_interval_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    // If we are and we are either in scheduled mode or hybrid + underwater
    if ((SYS_CONFIG_GPS_TRIGGER_MODE_SCHEDULED == sys_config.gps_trigger_mode.contents.mode) ||
        ((SYS_CONFIG_GPS_TRIGGER_MODE_HYBRID == sys_config.gps_trigger_mode.contents.mode) && (!tracker_above_water)))
    {
        // Our scheduled interval has elapsed
        // So wake up the GPS
        GPS_on();

        syshal_timer_set(timer_gps_maximum_acquisition, one_shot, sys_config.gps_maximum_acquisition_time.contents.seconds);

        if (sys_config.gps_scheduled_acquisition_no_fix_timeout.hdr.set &&
            sys_config.gps_scheduled_acquisition_no_fix_timeout.contents.seconds) // Don't set a no fix timeout if it's zero as this is a special case
            syshal_timer_set(timer_gps_no_fix, one_shot, sys_config.gps_scheduled_acquisition_no_fix_timeout.contents.seconds);
    }
}

static void timer_gps_no_fix_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    // If we are and we are either in scheduled mode or hybrid + underwater
    if ((SYS_CONFIG_GPS_TRIGGER_MODE_SCHEDULED == sys_config.gps_trigger_mode.contents.mode) ||
        ((SYS_CONFIG_GPS_TRIGGER_MODE_HYBRID == sys_config.gps_trigger_mode.contents.mode) && (!tracker_above_water)))
    {
        syshal_timer_cancel(timer_gps_maximum_acquisition);
        // We have been unable to achieve a GPS fix
        // So shutdown the GPS
        GPS_off();
    }
}

static void timer_gps_maximum_acquisition_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    syshal_timer_cancel(timer_gps_no_fix);

    // We have been GPS logging for our maximum allowed time
    // So shutdown the GPS
    GPS_off();
}

static void timer_gps_test_fix_hold_time_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);
    syshal_led_set_solid(SYSHAL_LED_COLOUR_WHITE);
    led_finish_time = LED_DURATION_MS + syshal_time_get_ticks_ms();
    test_state_gps = SM_TEST_FINISHING;
    setup_GPS_based_on_configuration(); // Set the GPS to standard operation
}

static void timer_gps_watchdog_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    if (!sensor_logging_enabled)
        return;

    // The GPS has been unresponsive for a suspiciously long time. Try waking it up again
    if (SM_GPS_STATE_ASLEEP != sm_gps_state)
    {
        // Reset GPS watchdog
        syshal_timer_set(timer_gps_watchdog, one_shot, GPS_WATCHDOG_TIME_SECONDS);

        sm_gps_state = SM_GPS_STATE_ACQUIRING;
        gps_ttff_reading_logged = false;

        // Reset the GPS device
        syshal_gps_shutdown();
        syshal_time_delay_ms(GPS_RESTART_TIME_MS);
        syshal_gps_wake_up();

        // Log the GPS switched on event
        if (sys_config.gps_debug_logging_enable.hdr.set &&
            sys_config.gps_debug_logging_enable.contents.enable)
        {
            logging_log_gps_on_t gps_on_log;
            LOGGING_SET_HDR(&gps_on_log, LOGGING_GPS_ON);
            logging_add_to_buffer((uint8_t *) &gps_on_log, sizeof(gps_on_log));
        }

    }
}

static void timer_log_flush_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    // Flush and log data to the FLASH
    fs_flush(sm_main_file_handle);
}

static void timer_saltwater_switch_hysteresis_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    // The tracker has been underwater for the configured amount of time
    tracker_above_water = false;

    if (sensor_logging_enabled)
    {
        if (sys_config.saltwater_switch_log_enable.contents.enable)
        {
            logging_submerged_t submerged;
            LOGGING_SET_HDR(&submerged, LOGGING_SUBMERGED);
            logging_add_to_buffer((uint8_t *) &submerged, sizeof(submerged));
        }

        // Are we supposed to be sleeping the GPS?
        if (SYS_CONFIG_GPS_TRIGGER_MODE_SWITCH_TRIGGERED == sys_config.gps_trigger_mode.contents.mode
            || SYS_CONFIG_GPS_TRIGGER_MODE_HYBRID == sys_config.gps_trigger_mode.contents.mode)
        {
            // We are no longer aquiring GPS data so cancel any timer for this
            syshal_timer_cancel(timer_gps_maximum_acquisition);
            GPS_off();
        }
    }
}

static void timer_reed_switch_hysteresis_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    reed_switch_debounce = false;

    gpio_reed_sw_callback(0); // Check for any state changes
    syshal_timer_cancel(timer_reed_switch_hysteresis);
    reed_switch_debounce = false;
}

static void timer_pressure_interval_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    // Start the max acquisition timer
    syshal_timer_set(timer_pressure_maximum_acquisition, one_shot, sys_config.pressure_maximum_acquisition_time.contents.seconds);

    // Start the sampling timer
    syshal_pressure_wake();
}

static void timer_pressure_maximum_acquisition_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    syshal_pressure_sleep(); // Stop the pressure sensor
}

static void timer_axl_interval_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    syshal_timer_set(timer_axl_maximum_acquisition, one_shot, sys_config.axl_maximum_acquisition_time.contents.seconds);
    syshal_axl_wake();
}

static void timer_axl_maximum_acquisition_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    syshal_axl_sleep();
}

static void timer_ble_interval_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    // Should we be using the reed switch to trigger BLE activation?
    if (sys_config.tag_bluetooth_scheduled_interval.hdr.set &&
        sys_config.tag_bluetooth_scheduled_duration.hdr.set &&
        sys_config.tag_bluetooth_trigger_control.hdr.set &&
        sys_config.tag_bluetooth_trigger_control.contents.flags & SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_SCHEDULED)
    {
        ble_state |= SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_SCHEDULED;
        syshal_timer_set(timer_ble_duration, one_shot, sys_config.tag_bluetooth_scheduled_duration.contents.seconds);
    }
}

static void timer_ble_duration_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);


    ble_state &= (uint8_t) ~SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_SCHEDULED;
    ble_state &= (uint8_t) ~SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_ONE_SHOT;
    // If we've not managed to connect during the duration period
    if (!config_if_connected &&
        !ble_state) // And there is no other reason to keep the BLE interface running
    {
        // Should we log this event
        if (sys_config.tag_bluetooth_log_enable.hdr.set &&
            sys_config.tag_bluetooth_log_enable.contents.enable)
        {
            logging_ble_enabled_t ble_disabled;
            LOGGING_SET_HDR(&ble_disabled, LOGGING_BLE_DISABLED);
            ble_disabled.cause = LOGGING_BLE_DISABLED_CAUSE_SCHEDULE_TIMER;
            logging_add_to_buffer((uint8_t *) &ble_disabled, sizeof(ble_disabled));
        }

        // Then terminate it
        config_if_term();
    }
}

static void timer_ble_timeout_callback(void)
{
    DEBUG_PR_TRACE("%s() called", __FUNCTION__);

    // This has been triggered because we have a bluetooth connection but no data has been sent or received for a while

    ble_state &= (uint8_t) ~SYS_CONFIG_TAG_BLUETOOTH_TRIGGER_CONTROL_SCHEDULED;

    if (config_if_connected && // If we are currently connected
        !ble_state && // And there is no other reason to keep the BLE interface running (e.g. reed switch)
        CONFIG_IF_BACKEND_BLE == config_if_current()) // And we are currently using the bluetooth interface
    {
        // Should we log this event
        if (sys_config.tag_bluetooth_log_enable.hdr.set &&
            sys_config.tag_bluetooth_log_enable.contents.enable)
        {
            logging_ble_enabled_t ble_disabled;
            LOGGING_SET_HDR(&ble_disabled, LOGGING_BLE_DISABLED);
            ble_disabled.cause = LOGGING_BLE_DISABLED_CAUSE_INACTIVITY_TIMEOUT;
            logging_add_to_buffer((uint8_t *) &ble_disabled, sizeof(ble_disabled));
        }

        // If so then terminate it
        config_if_term();

        // And generate a disconnect event
        config_if_event_t disconnectEvent;
        disconnectEvent.backend = CONFIG_IF_BACKEND_BLE;
        disconnectEvent.id = CONFIG_IF_EVENT_DISCONNECTED;
        config_if_callback(&disconnectEvent);
    }
}

////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////// CFG_READ ///////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

void cfg_read_populate_next(uint16_t tag, void * src, uint16_t length)
{
    sm_context.cfg_read.buffer_base[sm_context.cfg_read.buffer_offset++] = (uint8_t) (tag);
    sm_context.cfg_read.buffer_base[sm_context.cfg_read.buffer_offset++] = (uint8_t) (tag >> 8);
    memcpy(&sm_context.cfg_read.buffer_base[sm_context.cfg_read.buffer_offset],
           src, length);
    sm_context.cfg_read.buffer_offset += length;
}

void cfg_read_populate_buffer(void)
{
    uint16_t tag;
    int ret;

    /* Iterate configuration tags */
    while (!sys_config_iterate(&tag, &sm_context.cfg_read.last_index))
    {
        void * src;

        ret = sys_config_get(tag, &src);
        if (ret > 0)
        {
            if ((sm_context.cfg_read.buffer_offset + (uint32_t) ret + SYS_CONFIG_TAG_ID_SIZE) > SYSHAL_USB_PACKET_SIZE)
            {
                /* Buffer is full so defer this to the next iteration */
                sm_context.cfg_read.last_index--;
                break;
            }

            cfg_read_populate_next(tag, src, (uint16_t)ret);
        }
    }
}

uint32_t cfg_read_all_calc_length(void)
{
    int ret;
    uint16_t last_index = 0, tag;
    uint32_t length = 0;

    /* Iterate all configuration tags */
    while (!sys_config_iterate(&tag, &last_index))
    {
        void * src;
        ret = sys_config_get(tag, &src);
        if (ret > 0)
            length += ( (uint32_t) ret + SYS_CONFIG_TAG_ID_SIZE);
    }

    return length;
}

void cfg_read_req(cmd_t * req, size_t size)
{
    if (!cmd_check_size(CMD_CFG_READ_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);

    CMD_SET_HDR(resp, CMD_CFG_READ_RESP);

    /* Allocate buffer for following configuration data */
    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_cfg_read_resp_t));
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&sm_context.cfg_read.buffer_base))
        Throw(EXCEPTION_TX_BUFFER_FULL);

    /* Reset buffer offset to head of buffer */
    sm_context.cfg_read.buffer_offset = 0;

    if (CFG_READ_REQ_READ_ALL == req->cmd_cfg_read_req.configuration_tag)
    {
        /* Requested all configuration items */
        resp->cmd_cfg_read_resp.error_code = CMD_NO_ERROR;
        resp->cmd_cfg_read_resp.length = cfg_read_all_calc_length();
        sm_context.cfg_read.last_index = 0;
        sm_context.cfg_read.length = resp->cmd_cfg_read_resp.length;
        if (resp->cmd_cfg_read_resp.length > 0)
        {
            cfg_read_populate_buffer();
            buffer_write_advance(&config_if_send_buffer, sm_context.cfg_read.buffer_offset);
        }
    }
    else
    {
        void * src;
        /* Requested a single configuration tag */

        int ret = sys_config_get(req->cmd_cfg_read_req.configuration_tag, &src);

        if (ret < 0)
        {
            resp->cmd_cfg_read_resp.length = 0;
            if (SYS_CONFIG_ERROR_INVALID_TAG == ret)  // Tag is not valid. Return an error code
            {
                resp->cmd_cfg_read_resp.error_code = CMD_ERROR_INVALID_CONFIG_TAG;
            }
            else if (SYS_CONFIG_ERROR_TAG_NOT_SET == ret)  // Tag is not set. Return an error code
            {
                resp->cmd_cfg_read_resp.error_code = CMD_ERROR_CONFIG_TAG_NOT_SET;
            }
            else
            {
                DEBUG_PR_ERROR("Failed to retrieve tag 0x%04X, with error: %d", req->cmd_cfg_read_req.configuration_tag, ret);
                Throw(EXCEPTION_BAD_SYS_CONFIG_ERROR_CONDITION);
            }
        }
        else
        {
            cfg_read_populate_next(req->cmd_cfg_read_req.configuration_tag, src, (uint16_t)ret);
            resp->cmd_cfg_read_resp.error_code = CMD_NO_ERROR;
            resp->cmd_cfg_read_resp.length = sm_context.cfg_read.buffer_offset;
            sm_context.cfg_read.length = sm_context.cfg_read.buffer_offset;
            buffer_write_advance(&config_if_send_buffer, sm_context.cfg_read.buffer_offset);
        }
    }

    config_if_send_priv(&config_if_send_buffer); // Send the response

    if (resp->cmd_cfg_read_resp.length > 0)
    {
        /* Another buffer must follow the initial response */
        message_set_state(SM_MESSAGE_STATE_CFG_READ_NEXT);
    }
}

void cfg_read_next_state(void)
{
    /* Send the pending buffer and prepare a new buffer */
    config_if_send_priv(&config_if_send_buffer);
    sm_context.cfg_read.length -= sm_context.cfg_read.buffer_offset;

    if (sm_context.cfg_read.length > 0)
    {
        /* Allocate buffer for following configuration data */
        if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&sm_context.cfg_read.buffer_base))
            Throw(EXCEPTION_TX_BUFFER_FULL);

        /* Reset buffer offset to head of buffer */
        sm_context.cfg_read.buffer_offset = 0;
        cfg_read_populate_buffer();

        /* Advance the buffer */
        buffer_write_advance(&config_if_send_buffer, sm_context.cfg_read.buffer_offset);
    }
    else
    {
        message_set_state(SM_MESSAGE_STATE_IDLE);
    }
}

////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////// CFG_WRITE //////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void cfg_write_req(cmd_t * req, size_t size)
{
    if (!cmd_check_size(CMD_CFG_WRITE_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    sm_context.cfg_write.length = req->cmd_cfg_write_req.length;

    // Length is zero
    if (!sm_context.cfg_write.length)
        Throw(EXCEPTION_PACKET_WRONG_SIZE);

    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);
    resp->cmd_generic_resp.error_code = CMD_NO_ERROR;

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer); // Send response

    config_if_receive_byte_stream_priv(sm_context.cfg_write.length); // Queue a receive

    sm_context.cfg_write.buffer_occupancy = 0;

    message_set_state(SM_MESSAGE_STATE_CFG_WRITE_NEXT);
}

static void cfg_write_next_state(void)
{
    uint32_t bytes_to_copy;
    uint8_t * read_buffer;
    uint32_t length = buffer_read(&config_if_receive_buffer, (uintptr_t *)&read_buffer);

    if (!length)
        return;

    buffer_read_advance(&config_if_receive_buffer, length); // Remove this packet from the receive buffer

    if (length > sm_context.cfg_write.length)
    {
        // We've received more data then we were expecting
        sm_context.cfg_write.error_code = CMD_ERROR_DATA_OVERSIZE;
        message_set_state(SM_MESSAGE_STATE_CFG_WRITE_ERROR);
        Throw(EXCEPTION_PACKET_WRONG_SIZE);
    }

    while (length)
    {

        // Do we have a tag ID in our working buffer?
        if (sm_context.cfg_write.buffer_occupancy < SYS_CONFIG_TAG_ID_SIZE)
        {
            // If not then put as much of our tag ID into our temp buffer as possible
            bytes_to_copy = MIN(length, SYS_CONFIG_TAG_ID_SIZE - sm_context.cfg_write.buffer_occupancy);

            memcpy(&sm_context.cfg_write.buffer[sm_context.cfg_write.buffer_occupancy], read_buffer, bytes_to_copy);

            sm_context.cfg_write.buffer_occupancy += bytes_to_copy;
            read_buffer += bytes_to_copy;
            length -= bytes_to_copy;
        }

        if (sm_context.cfg_write.buffer_occupancy < SYS_CONFIG_TAG_ID_SIZE)
            break; // If we still don't have at least a tag ID then wait for more data

        // Fetch the configuration tag
        uint16_t tag = 0;
        tag |= (uint16_t) sm_context.cfg_write.buffer[0] & 0x00FF;
        tag |= (uint16_t) (sm_context.cfg_write.buffer[1] << 8) & 0xFF00;

        // Determine the size of this configuration tag
        size_t tag_data_size;
        int ret = sys_config_size(tag, &tag_data_size);

        // If the tag is invalid
        if (SYS_CONFIG_NO_ERROR != ret)
        {
            // Then we should exit out and return an error
            DEBUG_PR_ERROR("sys_config_size(0x%04X) returned: %d()", tag, ret);
            sm_context.cfg_write.error_code = CMD_ERROR_INVALID_CONFIG_TAG;
            message_set_state(SM_MESSAGE_STATE_CFG_WRITE_ERROR);
            Throw(EXCEPTION_BAD_SYS_CONFIG_ERROR_CONDITION);
        }

        // Then lets put what we have into our working buffer, accounting for what we might already have in our buffer
        bytes_to_copy = MIN( length, MIN((uint32_t) tag_data_size, ((uint32_t) tag_data_size) - sm_context.cfg_write.buffer_occupancy + SYS_CONFIG_TAG_ID_SIZE));
        memcpy(&sm_context.cfg_write.buffer[sm_context.cfg_write.buffer_occupancy], read_buffer, bytes_to_copy);
        sm_context.cfg_write.buffer_occupancy += bytes_to_copy;
        read_buffer += bytes_to_copy;
        length -= bytes_to_copy;

        // Do we have all of the configuration tags data in our working buffer?
        if (sm_context.cfg_write.buffer_occupancy < tag_data_size + SYS_CONFIG_TAG_ID_SIZE)
            break; // Then wait until we do

        // Process the tag
        ret = sys_config_set(tag, &sm_context.cfg_write.buffer[SYS_CONFIG_TAG_ID_SIZE], tag_data_size); // Set tag value

        if (ret < 0)
        {
            DEBUG_PR_ERROR("sys_config_set(0x%04X) returned: %d()", tag, ret);
            message_set_state(SM_MESSAGE_STATE_IDLE);
            Throw(EXCEPTION_BAD_SYS_CONFIG_ERROR_CONDITION); // Exit and fail silent
        }

        DEBUG_PR_TRACE("sys_config_set(0x%04X)", tag);

        sm_context.cfg_write.length -= sm_context.cfg_write.buffer_occupancy;
        sm_context.cfg_write.buffer_occupancy = 0;

    }

    if (sm_context.cfg_write.length) // Is there still data to receive?
    {
        config_if_receive_byte_stream_priv(sm_context.cfg_write.length); // Queue a receive
        config_if_timeout_reset(); // Reset the message timeout counter
    }
    else
    {
        // We have received all the data
        // Then send a confirmation
        cmd_t * resp;
        if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
            Throw(EXCEPTION_TX_BUFFER_FULL);
        CMD_SET_HDR(resp, CMD_CFG_WRITE_CNF);
        resp->cmd_cfg_write_cnf.error_code = CMD_NO_ERROR;

        buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_cfg_write_cnf_t));
        config_if_send_priv(&config_if_send_buffer); // Send response

        message_set_state(SM_MESSAGE_STATE_IDLE);
    }
}

static void cfg_write_error_state(void)
{
    // Return an error code
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_CFG_WRITE_CNF);
    resp->cmd_cfg_write_cnf.error_code = sm_context.cfg_write.error_code;

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_cfg_write_cnf_t));
    config_if_send_priv(&config_if_send_buffer); // Send response

    message_set_state(SM_MESSAGE_STATE_IDLE);
}

////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////// CFG_SAVE ///////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void cfg_save_req(cmd_t * req, size_t size)
{
    UNUSED(req);

    if (!cmd_check_size(CMD_CFG_SAVE_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    int ret = sys_config_save_to_fs(file_system); // Must first delete our configuration data

    switch (ret)
    {
        case SYS_CONFIG_NO_ERROR:
            resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
            break;

        default:
            Throw(EXCEPTION_FS_ERROR);
            break;
    }

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer); // Send confirmation
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////// CFG_RESTORE /////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void cfg_restore_req(cmd_t * req, size_t size)
{
    UNUSED(req);

    if (!cmd_check_size(CMD_CFG_RESTORE_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    int ret = sys_config_load_from_fs(file_system);

    switch (ret)
    {
        case SYS_CONFIG_NO_ERROR:
            resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
            break;

        case SYS_CONFIG_ERROR_NO_VALID_CONFIG_FILE_FOUND:
            resp->cmd_generic_resp.error_code = CMD_ERROR_FILE_NOT_FOUND;
            break;

        default:
            Throw(EXCEPTION_FS_ERROR);
            break;
    }

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer); // Send confirmation
}

////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////// CFG_ERASE //////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void cfg_erase_req(cmd_t * req, size_t size)
{
    // Check request size is correct
    if (!cmd_check_size(CMD_CFG_ERASE_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    if (CFG_ERASE_REQ_ERASE_ALL == req->cmd_cfg_erase_req.configuration_tag) // Erase all configuration tags
    {
        uint16_t last_index = 0;
        uint16_t tag;

        while (!sys_config_iterate(&tag, &last_index))
        {
            sys_config_unset(tag);
        }

        resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
    }
    else
    {
        // Erase just one configuration tag
        int return_code = sys_config_unset(req->cmd_cfg_erase_req.configuration_tag);

        switch (return_code)
        {
            case SYS_CONFIG_NO_ERROR:
                resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
                break;

            case SYS_CONFIG_ERROR_INVALID_TAG:
                resp->cmd_generic_resp.error_code = CMD_ERROR_INVALID_CONFIG_TAG;
                break;

            default:
                Throw(EXCEPTION_FS_ERROR);
                break;
        }
    }

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer); // Send confirmation
}

////////////////////////////////////////////////////////////////////////////////
///////////////////////////////// CFG_PROTECT //////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void cfg_protect_req(cmd_t * req, size_t size)
{
    UNUSED(req);

    // Check request size is correct
    if (!cmd_check_size(CMD_CFG_PROTECT_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    int ret = fs_protect(file_system, FILE_ID_CONF_PRIMARY);

    switch (ret)
    {
        case FS_NO_ERROR:
            resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
            break;

        case FS_ERROR_FILE_NOT_FOUND:
            resp->cmd_generic_resp.error_code = CMD_ERROR_FILE_NOT_FOUND;
            break;

        default:
            Throw(EXCEPTION_FS_ERROR);
            break;
    }

    // Send the response
    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

////////////////////////////////////////////////////////////////////////////////
//////////////////////////////// CFG_UNPROTECT /////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void cfg_unprotect_req(cmd_t * req, size_t size)
{
    UNUSED(req);

    // Check request size is correct
    if (!cmd_check_size(CMD_CFG_UNPROTECT_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    int ret = fs_unprotect(file_system, FILE_ID_CONF_PRIMARY);

    switch (ret)
    {
        case FS_NO_ERROR:
            resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
            break;

        case FS_ERROR_FILE_NOT_FOUND:
            resp->cmd_generic_resp.error_code = CMD_ERROR_FILE_NOT_FOUND;
            break;

        default:
            Throw(EXCEPTION_FS_ERROR);
            break;
    }

    // Send the response
    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////// GPS_WRITE ///////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void gps_write_req(cmd_t * req, size_t size)
{
    // Check request size is correct
    if (!cmd_check_size(CMD_GPS_WRITE_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate and send response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    // If bridging is enabled
    if (syshal_gps_bridging)
    {
        sm_context.gps_write.length = req->cmd_gps_write_req.length;
        resp->cmd_generic_resp.error_code = CMD_NO_ERROR;

        config_if_receive_byte_stream_priv(sm_context.gps_write.length); // Queue a receive

        message_set_state(SM_MESSAGE_STATE_GPS_WRITE_NEXT);
    }
    else
    {
        // Bridging is not enabled so return an error code
        resp->cmd_generic_resp.error_code = CMD_ERROR_BRIDGING_DISABLED;
    }

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

static void gps_write_next_state(void)
{
    uint8_t * read_buffer;
    uint32_t length = buffer_read(&config_if_receive_buffer, (uintptr_t *)&read_buffer);

    if (!length)
        return;

    buffer_read_advance(&config_if_receive_buffer, length); // Remove this packet from the receive buffer

    if (length > sm_context.gps_write.length)
    {
        message_set_state(SM_MESSAGE_STATE_IDLE);
        Throw(EXCEPTION_PACKET_WRONG_SIZE);
    }

    int ret = syshal_gps_send_raw(read_buffer, length);

    // Check send worked
    if (ret < 0)
    {
        // If not we should exit out
        message_set_state(SM_MESSAGE_STATE_IDLE);
        Throw(EXCEPTION_GPS_SEND_ERROR);
    }

    sm_context.gps_write.length -= length;

    if (sm_context.gps_write.length) // Is there still data to receive?
    {
        config_if_receive_byte_stream_priv(sm_context.gps_write.length); // Queue a receive
        config_if_timeout_reset(); // Reset the message timeout counter
    }
    else
    {
        // We have received all the data
        message_set_state(SM_MESSAGE_STATE_IDLE);
    }
}

////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////// GPS_READ ///////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void gps_read_req(cmd_t * req, size_t size)
{
    // Check request size is correct
    if (!cmd_check_size(CMD_GPS_READ_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate and send response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GPS_READ_RESP);

    // If bridging is enabled
    if (syshal_gps_bridging)
    {
        // Try to match the requested length, if not return as close to it as we can
        //DEBUG_PR_TRACE("syshal_gps_available_raw() = %lu, req->cmd_gps_read_req.length = %lu", syshal_gps_available_raw(), req->cmd_gps_read_req.length);
        sm_context.gps_read.length = MIN(syshal_gps_available_raw(), req->cmd_gps_read_req.length);

        resp->cmd_gps_read_resp.length = sm_context.gps_read.length;
        resp->cmd_gps_read_resp.error_code = CMD_NO_ERROR;
    }
    else
    {
        // Bridging is not enabled so return an error code
        resp->cmd_gps_read_resp.length = 0;
        resp->cmd_gps_read_resp.error_code = CMD_ERROR_BRIDGING_DISABLED;
    }

    // Send response
    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_gps_read_resp_t));
    config_if_send_priv(&config_if_send_buffer);

    if (sm_context.gps_read.length > 0)
        message_set_state(SM_MESSAGE_STATE_GPS_READ_NEXT);
}

static void gps_read_next_state(void)
{
    // Generate and send response
    uint8_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);

    // Don't read more than the maximum packet size
    uint32_t bytes_to_read = MIN(sm_context.gps_read.length, (uint32_t) SYSHAL_USB_PACKET_SIZE);

    // Receive data from the GPS module
    uint32_t bytes_actually_read = syshal_gps_receive_raw(resp, bytes_to_read);

    sm_context.gps_read.length -= bytes_actually_read;

    // Send response
    buffer_write_advance(&config_if_send_buffer, bytes_actually_read);
    config_if_send_priv(&config_if_send_buffer);

    if (sm_context.gps_read.length) // Is there still data to send?
    {
        config_if_timeout_reset(); // Reset the message timeout counter
    }
    else
    {
        // We have sent all the data
        message_set_state(SM_MESSAGE_STATE_IDLE);
    }
}

////////////////////////////////////////////////////////////////////////////////
//////////////////////////////// GPS_CONFIG_REQ ////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void gps_config_req(cmd_t * req, size_t size)
{
    // Check request size is correct
    if (!cmd_check_size(CMD_GPS_CONFIG_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    syshal_gps_bridging = req->cmd_gps_config_req.enable; // Disable or enable GPS bridging

    // If we've just enabled bridging, remove any previous data in the GPS rx buffer
    if (syshal_gps_bridging)
    {
        uint8_t flush;
        while (syshal_gps_receive_raw(&flush, 1))
        {}
    }

    // Generate and send response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    resp->cmd_generic_resp.error_code = CMD_NO_ERROR;

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}


////////////////////////////////////////////////////////////////////////////////
//////////////////////////////// CELLULAR_CONFIG_REQ ///////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void cellular_config_req(cmd_t * req, size_t size)
{
    if (!cmd_check_size(CMD_CELLULAR_CONFIG_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate and send response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);

    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    syshal_cellular_bridging = req->cmd_cellular_config_req.enable; // Disable or enable Cellular bridging

    // If we've just enabled bridging, power on sequence and sysccoms
    if (syshal_cellular_bridging)
    {
        if (syshal_cellular_sync_comms() != SYSHAL_CELLULAR_NO_ERROR)
        {
            syshal_cellular_bridging = false;
            resp->cmd_generic_resp.error_code = CMD_ERROR_CELLULAR_COMMS;
        }
        else
        {
            resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
        }
    }
    else
    {
        resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
    }

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////// CELLULAR_WRITE //////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void cellular_write_req(cmd_t * req, size_t size)
{
    if (!cmd_check_size(CMD_CELLULAR_WRITE_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate and send response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);

    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    // If bridging is enabled
    if (syshal_cellular_bridging)
    {
        sm_context.cellular_write.length = req->cmd_cellular_write_req.length;
        resp->cmd_generic_resp.error_code = CMD_NO_ERROR;

        config_if_receive_byte_stream_priv(sm_context.cellular_write.length); // Queue a receive

        message_set_state(SM_MESSAGE_STATE_CELLULAR_WRITE_NEXT);
    }
    else
    {
        // Bridging is not enabled so return an error code
        resp->cmd_generic_resp.error_code = CMD_ERROR_BRIDGING_DISABLED;
    }

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

static void cellular_write_next_state(void)
{
    uint8_t * read_buffer;
    uint32_t length = buffer_read(&config_if_receive_buffer, (uintptr_t *)&read_buffer);
    if (!length)
        return;

    buffer_read_advance(&config_if_receive_buffer, length); // Remove this packet from the receive buffer

    if (length > sm_context.cellular_write.length)
    {
        message_set_state(SM_MESSAGE_STATE_IDLE);
        syshal_cellular_bridging = false;
        syshal_cellular_power_off();
        Throw(EXCEPTION_PACKET_WRONG_SIZE);
    }

    int ret = syshal_cellular_send_raw(read_buffer, length);

    // Check send worked
    if (ret  != SYSHAL_CELLULAR_NO_ERROR)
    {
        // If not we should exit out
        message_set_state(SM_MESSAGE_STATE_IDLE);
        syshal_cellular_bridging = false;
        syshal_cellular_power_off();
        Throw(EXCEPTION_CELLULAR_SEND_ERROR);
    }

    sm_context.cellular_write.length -= length;

    if (sm_context.cellular_write.length) // Is there still data to receive?
    {
        config_if_receive_byte_stream_priv(sm_context.cellular_write.length); // Queue a receive
        config_if_timeout_reset(); // Reset the message timeout counter
    }
    else
    {
        // We have received all the data
        message_set_state(SM_MESSAGE_STATE_IDLE);
    }
}

////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////// CELLULAR_READ //////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void cellular_read_req(cmd_t * req, size_t size)
{
    if (!cmd_check_size(CMD_CELLULAR_READ_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate and send response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);

    CMD_SET_HDR(resp, CMD_CELLULAR_READ_RESP);

    // If bridging is enabled
    if (syshal_cellular_bridging)
    {
        // Try to match the requested length, if not return as close to it as we can

        sm_context.cellular_read.length = MIN(syshal_cellular_available_raw(), req->cmd_cellular_read_req.length);

        resp->cmd_cellular_read_resp.length = sm_context.cellular_read.length;
        resp->cmd_cellular_read_resp.error_code = CMD_NO_ERROR;
    }
    else
    {
        // Bridging is not enabled so return an error code
        resp->cmd_cellular_read_resp.length = 0;
        resp->cmd_cellular_read_resp.error_code = CMD_ERROR_BRIDGING_DISABLED;
    }

    // Send response
    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_cellular_read_resp_t));
    config_if_send_priv(&config_if_send_buffer);

    if (sm_context.cellular_read.length > 0)
        message_set_state(SM_MESSAGE_STATE_CELLULAR_READ_NEXT);
}

static void cellular_read_next_state(void)
{
    // Generate and send response
    uint8_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
    {
        syshal_cellular_bridging = false;
        syshal_cellular_power_off();
        Throw(EXCEPTION_TX_BUFFER_FULL);
    }

    // Don't read more than the maximum packet size
    uint32_t bytes_to_read = MIN(sm_context.cellular_read.length, (uint32_t) SYSHAL_USB_PACKET_SIZE);

    // Receive data from the cellular module
    uint32_t bytes_actually_read = syshal_cellular_receive_raw(resp, bytes_to_read);

    sm_context.cellular_read.length -= bytes_actually_read;

    // Send response
    buffer_write_advance(&config_if_send_buffer, bytes_actually_read);
    config_if_send_priv(&config_if_send_buffer);

    if (sm_context.cellular_read.length) // Is there still data to send?
    {
        config_if_timeout_reset(); // Reset the message timeout counter
    }
    else
    {
        // We have sent all the data
        message_set_state(SM_MESSAGE_STATE_IDLE);
    }
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////// TEST_REQ ////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void test_req(cmd_t * req, size_t size)
{
    if (!cmd_check_size(CMD_TEST_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate and send response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
    {
        Throw(EXCEPTION_TX_BUFFER_FULL);
    }
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);


    if (req->cmd_test_req.test_device_flag & CMD_TEST_REQ_GPS_BIT)
        test_state_gps = SM_TEST_REQUEST;
    else if (test_state_gps == SM_TEST_REQUEST)
        test_state_gps = SM_TEST_OFF;

    if (req->cmd_test_req.test_device_flag & CMD_TEST_REQ_CELLULAR_BIT)
        test_state_cellular = SM_TEST_REQUEST;
    else if (test_state_cellular == SM_TEST_REQUEST)
        test_state_cellular = SM_TEST_OFF;

    if (req->cmd_test_req.test_device_flag & CMD_TEST_REQ_SATELLITE_BIT)
        test_state_satellite = SM_TEST_REQUEST;
    else if (test_state_satellite == SM_TEST_REQUEST)
        test_state_satellite = SM_TEST_OFF;

    resp->cmd_generic_resp.error_code = CMD_NO_ERROR;

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer);

}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////// STATUS_REQ //////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void status_req(cmd_t * req, size_t size)
{
    UNUSED(req);

    if (!cmd_check_size(CMD_STATUS_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate and send response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_STATUS_RESP);
    // The device is ready to accept command and received resp

    //////////////////// System /////////////////////
    resp->cmd_status_resp.error_code = CMD_NO_ERROR;
    resp->cmd_status_resp.stm_firmware_version = APP_FIRMWARE_VERSION;
    uint32_t version;
    syshal_ble_get_version(&version); // Get BLE version
    resp->cmd_status_resp.ble_firmware_version = version;
    resp->cmd_status_resp.configuration_format_version = SYS_CONFIG_FORMAT_VERSION;

    syshal_device_id(&(resp->cmd_status_resp.nordic_uid));

    resp->cmd_status_resp.gps_module_detected = syshal_gps_is_present() ? 1 : 0;
    resp->cmd_status_resp.cellular_module_detected = syshal_cellular_is_present() ? 1 : 0;

    memset(resp->cmd_status_resp.sim_card_imsi, 0, sizeof(resp->cmd_status_resp.sim_card_imsi));
    if (resp->cmd_status_resp.cellular_module_detected)
    {
        if (syshal_cellular_check_sim(resp->cmd_status_resp.sim_card_imsi) == SYSHAL_CELLULAR_NO_ERROR)
            resp->cmd_status_resp.sim_card_present = 1;
        else
            resp->cmd_status_resp.sim_card_present = 0 ;
    }

    resp->cmd_status_resp.satellite_module_detected = 0;

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_status_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

static void fw_send_image_req(cmd_t * req, size_t size)
{
    if (!cmd_check_size(CMD_FW_SEND_IMAGE_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate and send response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    // Store variables for use in future
    sm_context.fw_send_image.length = req->cmd_fw_send_image_req.length;
    sm_context.fw_send_image.crc32_supplied = req->cmd_fw_send_image_req.CRC32;
    sm_context.fw_send_image.crc32_calculated = 0;

    DEBUG_PR_TRACE("Supplied CRC32 = %08x", (unsigned int)sm_context.fw_send_image.crc32_supplied);

    if (FW_SEND_IMAGE_REQ_ARTIC == req->cmd_fw_send_image_req.image_type)
    {
        sm_context.fw_send_image.file_id = FILE_ID_ARTIC_FIRM_IMAGE;
        int del_ret = fs_delete(file_system, sm_context.fw_send_image.file_id); // Must first delete any current image
        int open_ret;

        switch (del_ret)
        {
            case FS_ERROR_FILE_NOT_FOUND: // If there is no image file, then make one
            case FS_NO_ERROR:
                __NOP(); // Instruct for switch case to jump to
                open_ret = fs_open(file_system, &sm_main_file_handle, sm_context.fw_send_image.file_id, FS_MODE_CREATE, NULL);
                if (FS_NO_ERROR != open_ret)
                    Throw(EXCEPTION_FS_ERROR); // An unrecoverable error has occured

                // FIXME: Check to see if there is sufficient room for the firmware image
                config_if_receive_byte_stream_priv(sm_context.fw_send_image.length); // Queue a receive
                resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
                message_set_state(SM_MESSAGE_STATE_FW_SEND_IMAGE_NEXT);
                break;

            case FS_ERROR_FILE_PROTECTED: // We never lock the fw images so this shouldn't occur
                resp->cmd_generic_resp.error_code = CMD_ERROR_CONFIG_PROTECTED;
                break;

            default:
                Throw(EXCEPTION_FS_ERROR);
                break;
        }
    }
    else
    {
        resp->cmd_generic_resp.error_code = CMD_ERROR_INVALID_FW_IMAGE_TYPE;
    }

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

static void fw_send_image_next_state(void)
{
    uint8_t * read_buffer;
    uint32_t length = buffer_read(&config_if_receive_buffer, (uintptr_t *)&read_buffer);

    if (!length)
        return;

    buffer_read_advance(&config_if_receive_buffer, length); // Remove this packet from the receive buffer

    // Is this packet larger than we were expecting?
    if (length > sm_context.fw_send_image.length)
    {
        // If it is we should exit out
        message_set_state(SM_MESSAGE_STATE_IDLE);
        fs_close(sm_main_file_handle);
        fs_delete(file_system, sm_context.fw_send_image.file_id);
        Throw(EXCEPTION_PACKET_WRONG_SIZE);
    }

    sm_context.fw_send_image.crc32_calculated = crc32(sm_context.fw_send_image.crc32_calculated, read_buffer, length);
    uint32_t bytes_written = 0;
    int ret = fs_write(sm_main_file_handle, read_buffer, length, &bytes_written);
    if (FS_NO_ERROR != ret)
    {
        fs_close(sm_main_file_handle);
        fs_delete(file_system, sm_context.fw_send_image.file_id);
        message_set_state(SM_MESSAGE_STATE_IDLE);
        Throw(EXCEPTION_FS_ERROR);
    }

    sm_context.fw_send_image.length -= length;

    if (sm_context.fw_send_image.length) // Is there still data to receive?
    {
        config_if_receive_byte_stream_priv(sm_context.fw_send_image.length); // Queue a receive
        config_if_timeout_reset(); // Reset the message timeout counter
    }
    else
    {
        // We have received all the data
        fs_close(sm_main_file_handle); // Close the file

        // Then send a confirmation
        cmd_t * resp;
        if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
            Throw(EXCEPTION_TX_BUFFER_FULL);
        CMD_SET_HDR(resp, CMD_FW_SEND_IMAGE_COMPLETE_CNF);

        // Check the CRC32 is correct
        if (sm_context.fw_send_image.crc32_calculated == sm_context.fw_send_image.crc32_supplied)
        {
            resp->cmd_fw_send_image_complete_cnf.error_code = CMD_NO_ERROR;
        }
        else
        {
            resp->cmd_fw_send_image_complete_cnf.error_code = CMD_ERROR_IMAGE_CRC_MISMATCH;
            fs_delete(file_system, sm_context.fw_send_image.file_id); // Image is invalid, so delete it
        }

        buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_fw_send_image_complete_cnf_t));
        config_if_send_priv(&config_if_send_buffer); // Send response

        message_set_state(SM_MESSAGE_STATE_IDLE);
    }
}

static void fw_apply_image_req(cmd_t * req, size_t size)
{
    if (!cmd_check_size(CMD_FW_APPLY_IMAGE_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    // Check the image type is correct
    switch (req->cmd_fw_apply_image_req.image_type)
    {
        case FW_SEND_IMAGE_REQ_ARTIC:
            resp->cmd_generic_resp.error_code = CMD_ERROR_INVALID_FW_IMAGE_TYPE; // FIXME: To implement
            break;

        default:
            resp->cmd_generic_resp.error_code = CMD_ERROR_INVALID_FW_IMAGE_TYPE;
            break;
    }

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

static void reset_req(cmd_t * req, size_t size)
{
    if (!cmd_check_size(CMD_RESET_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate and send response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    bool going_to_reset = false;

    switch (req->cmd_reset_req.reset_type)
    {
        case RESET_REQ_APP:
            resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
            going_to_reset = true;
            break;

        case RESET_REQ_FLASH_ERASE_ALL:
            resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
            fs_format(file_system);
            log_file_created = false;
            break;

        case RESET_REQ_ENTER_DFU_MODE:
            resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
            syshal_device_set_dfu_entry_flag(true);
            going_to_reset = true;
            break;

        default:
            resp->cmd_generic_resp.error_code = CMD_ERROR_INVALID_PARAMETER;
            break;
    }

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer);

    if (going_to_reset)
    {
        // Wait for response to have been sent
        // Prevent an infinite loop in unit tests
#ifndef GTEST
        while (config_if_tx_pending)
#endif
        {
            config_if_tick();
        }

        syshal_pmu_reset();
    }
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////// BATTERY_STATUS_REQ //////////////////////////////
////////////////////////////////////////////////////////////////////////////////
static void battery_status_req(cmd_t * req, size_t size)
{
    UNUSED(req);
    int ret;
    uint8_t level;
    uint16_t voltage;

    if (!cmd_check_size(CMD_BATTERY_STATUS_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate and send response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_BATTERY_STATUS_RESP);

    resp->cmd_battery_status_resp.error_code = CMD_NO_ERROR;
    resp->cmd_battery_status_resp.charging_indicator = syshal_usb_plugged_in();

    ret = syshal_batt_level(&level);
    if (ret)
        resp->cmd_battery_status_resp.charge_level = 0xFF;
    else
        resp->cmd_battery_status_resp.charge_level = level;

    ret = syshal_batt_voltage(&voltage);
    if (ret)
        resp->cmd_battery_status_resp.millivolts = 0;
    else
        resp->cmd_battery_status_resp.millivolts = voltage;

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_battery_status_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

////////////////////////////////////////////////////////////////////////////////
//////////////////////////////// LOG_CREATE_REQ ////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void log_create_req(cmd_t * req, size_t size)
{
    if (!cmd_check_size(CMD_LOG_CREATE_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate and send response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    // Get request's parameters
    uint8_t mode = req->cmd_log_create_req.mode;
    uint8_t sync_enable = req->cmd_log_create_req.sync_enable;

    // Attempt to create the log file
    if (CMD_LOG_CREATE_REQ_MODE_FILL == mode || CMD_LOG_CREATE_REQ_MODE_CIRCULAR == mode)

    {
        // Convert from create mode to fs_mode_t
        fs_mode_t fs_mode = FS_MODE_CREATE;
        if (CMD_LOG_CREATE_REQ_MODE_FILL == mode)
            fs_mode = FS_MODE_CREATE;
        else if (CMD_LOG_CREATE_REQ_MODE_CIRCULAR == mode)
            fs_mode = FS_MODE_CREATE_CIRCULAR;

        int ret = fs_open(file_system, &sm_main_file_handle, FILE_ID_LOG, fs_mode, &sync_enable);

        switch (ret)
        {
            case FS_NO_ERROR:
                log_file_created = true;
                resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
                fs_close(sm_main_file_handle); // Close the file

                buffer_reset(&logging_buffer); // Clear anything we were looking to flush to the log file
                break;

            case FS_ERROR_FILE_ALREADY_EXISTS:
                resp->cmd_generic_resp.error_code = CMD_ERROR_FILE_ALREADY_EXISTS;
                break;

            default:
                Throw(EXCEPTION_FS_ERROR);
                break;
        }
    }
    else
    {
        resp->cmd_generic_resp.error_code = CMD_ERROR_INVALID_PARAMETER;
    }

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

////////////////////////////////////////////////////////////////////////////////
//////////////////////////////// LOG_ERASE_REQ /////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void log_erase_req(cmd_t * req, size_t size)
{
    UNUSED(req);

    if (!cmd_check_size(CMD_LOG_ERASE_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_GENERIC_RESP);

    int ret = fs_delete(file_system, FILE_ID_LOG);

    switch (ret)
    {
        case FS_NO_ERROR:
            resp->cmd_generic_resp.error_code = CMD_NO_ERROR;
            log_file_created = false;
            break;

        case FS_ERROR_FILE_NOT_FOUND:
            resp->cmd_generic_resp.error_code = CMD_ERROR_FILE_NOT_FOUND;
            break;

        case FS_ERROR_FILE_PROTECTED:
            resp->cmd_generic_resp.error_code = CMD_ERROR_CONFIG_PROTECTED;
            break;

        default:
            Throw(EXCEPTION_FS_ERROR);
            break;
    }

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_generic_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

////////////////////////////////////////////////////////////////////////////////
///////////////////////////////// LOG_READ_REQ /////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void log_read_req(cmd_t * req, size_t size)
{
    if (!cmd_check_size(CMD_LOG_READ_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_LOG_READ_RESP);

    sm_context.log_read.length = 0;

    fs_stat_t stat;
    int ret = fs_stat(file_system, FILE_ID_LOG, &stat);

    switch (ret)
    {
        case FS_NO_ERROR:
            sm_context.log_read.length = req->cmd_log_read_req.length;
            sm_context.log_read.start_offset = req->cmd_log_read_req.start_offset;

            // Check if both parameters are zero. If they are the client is requesting a full log file
            if ((0 == sm_context.log_read.length) && (0 == sm_context.log_read.start_offset))
                sm_context.log_read.length = stat.size;

            if (sm_context.log_read.start_offset > stat.size) // Is the offset beyond the end of the file?
            {
                resp->cmd_log_read_resp.error_code = CMD_ERROR_INVALID_PARAMETER;
            }
            else
            {
                // Do we have this amount of data ready to read?
                if ((sm_context.log_read.length + sm_context.log_read.start_offset) > stat.size)
                {
                    // If the length requested is greater than what we have then reduce the length
                    sm_context.log_read.length = stat.size - sm_context.log_read.start_offset;
                }

                // Open the file
                ret = fs_open(file_system, &sm_main_file_handle, FILE_ID_LOG, FS_MODE_READONLY, NULL);

                if (FS_NO_ERROR == ret)
                {
                    resp->cmd_log_read_resp.error_code = CMD_NO_ERROR;
                    if (sm_context.log_read.length)
                    {
                        ret = fs_seek(sm_main_file_handle, sm_context.log_read.start_offset);
                        if (FS_NO_ERROR != ret)
                            Throw(EXCEPTION_FS_ERROR);

                        message_set_state(SM_MESSAGE_STATE_LOG_READ_NEXT);
                    }
                    else
                        fs_close(sm_main_file_handle);
                }
                else
                {
                    Throw(EXCEPTION_FS_ERROR);
                }
            }
            break;

        case FS_ERROR_FILE_NOT_FOUND:
            resp->cmd_log_read_resp.error_code = CMD_ERROR_FILE_NOT_FOUND;
            break;

        default:
            Throw(EXCEPTION_FS_ERROR);
            break;
    }

    resp->cmd_log_read_resp.length = sm_context.log_read.length;

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_log_read_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

static void log_read_next_state()
{
    uint32_t bytes_to_read;
    uint32_t bytes_actually_read;
    int ret;

    //DEBUG_PR_TRACE("Bytes left to write: %lu", sm_context.log_read.length);

    // Get write buffer
    uint8_t * read_buffer;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&read_buffer))
        Throw(EXCEPTION_TX_BUFFER_FULL);

    // Read data out
    bytes_to_read = MIN(sm_context.log_read.length, (uint32_t) SYSHAL_USB_PACKET_SIZE);
    ret = fs_read(sm_main_file_handle, read_buffer, bytes_to_read, &bytes_actually_read);
    if (FS_NO_ERROR != ret)
    {
        Throw(EXCEPTION_FS_ERROR);
    }

    sm_context.log_read.length -= bytes_actually_read;

    buffer_write_advance(&config_if_send_buffer, bytes_actually_read);
    config_if_send_priv(&config_if_send_buffer);

    if (sm_context.log_read.length) // Is there still data to send?
    {
        config_if_timeout_reset(); // Reset the message timeout counter
    }
    else
    {
        // We have sent all the data
        fs_close(sm_main_file_handle); // Close the file
        message_set_state(SM_MESSAGE_STATE_IDLE);
    }

}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////// FLASH_DOWNLOAD_REQ //////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void flash_download_req(cmd_t * req, size_t size)
{
    if (!cmd_check_size(CMD_FLASH_DOWNLOAD_REQ, size))
        Throw(EXCEPTION_REQ_WRONG_SIZE);

    // Generate response
    cmd_t * resp;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&resp))
        Throw(EXCEPTION_TX_BUFFER_FULL);
    CMD_SET_HDR(resp, CMD_FLASH_DOWNLOAD_RESP);

    sm_context.flash_download.address = 0;
    syshal_flash_get_size(0, &sm_context.flash_download.length);

    resp->cmd_flash_download_resp.length = sm_context.flash_download.length;
    resp->cmd_flash_download_resp.error_code = CMD_NO_ERROR;

    if (sm_context.flash_download.length)
        message_set_state(SM_MESSAGE_STATE_FLASH_DOWNLOAD_NEXT);

    buffer_write_advance(&config_if_send_buffer, CMD_SIZE(cmd_log_read_resp_t));
    config_if_send_priv(&config_if_send_buffer);
}

static void flash_download_next_state()
{
    uint32_t bytes_to_read;
    int ret;

    // Get write buffer
    uint8_t * read_buffer;
    if (!buffer_write(&config_if_send_buffer, (uintptr_t *)&read_buffer))
        Throw(EXCEPTION_TX_BUFFER_FULL);

    bytes_to_read = MIN(sm_context.flash_download.length, (uint32_t) SYSHAL_USB_PACKET_SIZE);

    ret = syshal_flash_read(0, read_buffer, sm_context.flash_download.address, bytes_to_read);
    if (SYSHAL_FLASH_NO_ERROR != ret)
    {
        Throw(EXCEPTION_FLASH_ERROR);
    }

    sm_context.flash_download.length -= bytes_to_read;
    sm_context.flash_download.address += bytes_to_read;

    buffer_write_advance(&config_if_send_buffer, bytes_to_read);
    config_if_send_priv(&config_if_send_buffer);

    if (sm_context.flash_download.length) // Is there still data to send?
    {
        config_if_timeout_reset(); // Reset the message timeout counter
    }
    else
    {
        // We have sent all the data
        message_set_state(SM_MESSAGE_STATE_IDLE);
    }
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////// MESSAGE STATE EXECUTION CODE ////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void config_if_session_cleanup(void)
{
    buffer_reset(&config_if_send_buffer);
    buffer_reset(&config_if_receive_buffer);
    config_if_tx_pending = false;
    config_if_rx_queued = false; // Setting this to false does not mean a receive is still not queued!

    // Close any open files
    fs_close(sm_main_file_handle);
}

int config_if_callback(config_if_event_t * event)
{
    // This is called from an interrupt so we'll keep it short
    switch (event->id)
    {

        case CONFIG_IF_EVENT_SEND_COMPLETE:
            buffer_read_advance(&config_if_send_buffer, event->send.size); // Remove it from the buffer
            config_if_tx_pending = false;

            syshal_timer_reset(timer_ble_timeout);
            break;

        case CONFIG_IF_EVENT_RECEIVE_COMPLETE:
            buffer_write_advance(&config_if_receive_buffer, event->receive.size); // Store it in the buffer
            config_if_rx_queued = false;

            // Should we be resetting a BLE inactivity timer?
            syshal_timer_reset(timer_ble_timeout);
            break;

        case CONFIG_IF_EVENT_CONNECTED:
            DEBUG_PR_TRACE("CONFIG_IF_EVENT_CONNECTED");

            // Was this a bluetooth connection?
            if (CONFIG_IF_BACKEND_BLE == event->backend)
            {
                // Should we log this event
                if (sys_config.tag_bluetooth_log_enable.hdr.set &&
                    sys_config.tag_bluetooth_log_enable.contents.enable)
                {
                    logging_ble_connected_t ble_connected;
                    LOGGING_SET_HDR(&ble_connected, LOGGING_BLE_CONNECTED);
                    logging_add_to_buffer((uint8_t *) &ble_connected, sizeof(ble_connected));
                }

                // Should we be starting a BLE inactivity timer?
                if (sys_config.tag_bluetooth_connection_inactivity_timeout.hdr.set &&
                    sys_config.tag_bluetooth_connection_inactivity_timeout.contents.seconds)
                {
                    syshal_timer_set(timer_ble_timeout, one_shot, sys_config.tag_bluetooth_connection_inactivity_timeout.contents.seconds);
                }
            }

            config_if_session_cleanup(); // Clean up any previous session
            config_if_timeout_reset(); // Reset our timeout counter
            config_if_connected = true;
            break;

        case CONFIG_IF_EVENT_DISCONNECTED:
            DEBUG_PR_TRACE("CONFIG_IF_EVENT_DISCONNECTED");

            // Should we log this event
            if (CONFIG_IF_BACKEND_BLE == event->backend)
            {
                if (sys_config.tag_bluetooth_log_enable.hdr.set &&
                    sys_config.tag_bluetooth_log_enable.contents.enable)
                {
                    logging_ble_disconnected_t ble_disconnected;
                    LOGGING_SET_HDR(&ble_disconnected, LOGGING_BLE_DISCONNECTED);
                    logging_add_to_buffer((uint8_t *) &ble_disconnected, sizeof(ble_disconnected));
                }
            }
            else if (CONFIG_IF_BACKEND_FS_SCRIPT == event->backend)
            {
                // We have finished processing our fs_script so delete it
                fs_delete(file_system, FILE_ID_CONF_COMMANDS);
            }

            syshal_timer_cancel(timer_ble_timeout);
            // Clear all pending transmissions/receptions
            config_if_session_cleanup();
            config_if_connected = false;
            syshal_gps_bridging = false;
            syshal_cellular_bridging = false;
            break;

    }

    return CONFIG_IF_NO_ERROR;
}

static void message_idle_state(void)
{
    cmd_t * req;
    uint32_t length = buffer_read(&config_if_receive_buffer, (uintptr_t *)&req);
    if (length) // Is a message waiting to be processed
    {

        // Then process any pending event here, outside of an interrupt

        // Mark this message as received. This maybe a bit preemptive but it
        // is assumed the request handlers will handle the message appropriately
        buffer_read_advance(&config_if_receive_buffer, length);

        if (CMD_SYNCWORD != req->hdr.sync)
        {
            DEBUG_PR_WARN("Incorrect sync byte, expected: 0x%02X got: 0x%02X", CMD_SYNCWORD, req->hdr.sync);
            return;
        }

        switch (req->hdr.cmd)
        {
            case CMD_CFG_READ_REQ:
                DEBUG_PR_INFO("CFG_READ_REQ");
                cfg_read_req(req, length);
                break;

            case CMD_CFG_WRITE_REQ:
                DEBUG_PR_INFO("CFG_WRITE_REQ");
                cfg_write_req(req, length);
                break;

            case CMD_CFG_SAVE_REQ:
                DEBUG_PR_INFO("CFG_SAVE_REQ");
                cfg_save_req(req, length);
                break;

            case CMD_CFG_RESTORE_REQ:
                DEBUG_PR_INFO("CFG_RESTORE_REQ");
                cfg_restore_req(req, length);
                break;

            case CMD_CFG_ERASE_REQ:
                DEBUG_PR_INFO("CFG_ERASE_REQ");
                cfg_erase_req(req, length);
                break;

            case CMD_CFG_PROTECT_REQ:
                DEBUG_PR_INFO("CFG_PROTECT_REQ");
                cfg_protect_req(req, length);
                break;

            case CMD_CFG_UNPROTECT_REQ:
                DEBUG_PR_INFO("CFG_UNPROTECT_REQ");
                cfg_unprotect_req(req, length);
                break;

            case CMD_GPS_WRITE_REQ:
                DEBUG_PR_INFO("GPS_WRITE_REQ");
                gps_write_req(req, length);
                break;

            case CMD_GPS_READ_REQ:
                DEBUG_PR_INFO("GPS_READ_REQ");
                gps_read_req(req, length);
                break;

            case CMD_GPS_CONFIG_REQ:
                DEBUG_PR_INFO("GPS_CONFIG_REQ");
                gps_config_req(req, length);
                break;

            case CMD_STATUS_REQ:
                DEBUG_PR_INFO("STATUS_REQ");
                status_req(req, length);
                break;

            case CMD_FW_SEND_IMAGE_REQ:
                DEBUG_PR_INFO("FW_SEND_IMAGE_REQ");
                fw_send_image_req(req, length);
                break;

            case CMD_FW_APPLY_IMAGE_REQ:
                DEBUG_PR_INFO("FW_APPLY_IMAGE_REQ");
                fw_apply_image_req(req, length);
                break;

            case CMD_RESET_REQ:
                DEBUG_PR_INFO("RESET_REQ");
                reset_req(req, length);
                break;

            case CMD_BATTERY_STATUS_REQ:
                DEBUG_PR_INFO("BATTERY_STATUS_REQ");
                battery_status_req(req, length);
                break;

            case CMD_LOG_CREATE_REQ:
                DEBUG_PR_INFO("LOG_CREATE_REQ");
                log_create_req(req, length);
                break;

            case CMD_LOG_ERASE_REQ:
                DEBUG_PR_INFO("LOG_ERASE_REQ");
                log_erase_req(req, length);
                break;

            case CMD_LOG_READ_REQ:
                DEBUG_PR_INFO("LOG_READ_REQ");
                log_read_req(req, length);
                break;
            case CMD_CELLULAR_WRITE_REQ:
                DEBUG_PR_INFO("CELLULAR_WRITE_REQ");
                cellular_write_req(req, length);
                break;

            case CMD_CELLULAR_READ_REQ:
                DEBUG_PR_INFO("CELLULAR_READ_REQ");
                cellular_read_req(req, length);
                break;

            case CMD_CELLULAR_CONFIG_REQ:
                DEBUG_PR_INFO("CELLULAR_CONFIG_REQ");
                cellular_config_req(req, length);
                break;

            case CMD_TEST_REQ:
                DEBUG_PR_INFO("CMD_TEST_REQ");
                test_req(req, length);
                break;

            case CMD_FLASH_DOWNLOAD_REQ:
                DEBUG_PR_INFO("CMD_FLASH_DOWNLOAD_REQ");
                flash_download_req(req, length);
                break;

            default:
                DEBUG_PR_WARN("Unhandled command: id %d", req->hdr.cmd);
                // Don't return an error. Fail silent
                break;
        }

    }
    else
    {
        config_if_receive_priv();
    }
}

void state_message_exception_handler(CEXCEPTION_T e)
{
    switch (e)
    {
        case EXCEPTION_BAD_SYS_CONFIG_ERROR_CONDITION:
            DEBUG_PR_ERROR("EXCEPTION_BAD_SYS_CONFIG_ERROR_CONDITION");
            break;

        case EXCEPTION_REQ_WRONG_SIZE:
            DEBUG_PR_ERROR("EXCEPTION_REQ_WRONG_SIZE");
            break;

        case EXCEPTION_TX_BUFFER_FULL:
            DEBUG_PR_ERROR("EXCEPTION_TX_BUFFER_FULL");
            break;

        case EXCEPTION_TX_BUSY:
            DEBUG_PR_ERROR("EXCEPTION_TX_BUSY");
            break;

        case EXCEPTION_RX_BUFFER_EMPTY:
            DEBUG_PR_ERROR("EXCEPTION_RX_BUFFER_EMPTY");
            break;

        case EXCEPTION_RX_BUFFER_FULL:
            DEBUG_PR_ERROR("EXCEPTION_RX_BUFFER_FULL");
            break;

        case EXCEPTION_PACKET_WRONG_SIZE:
            DEBUG_PR_ERROR("EXCEPTION_PACKET_WRONG_SIZE");
            break;

        case EXCEPTION_GPS_SEND_ERROR:
            DEBUG_PR_ERROR("EXCEPTION_GPS_SEND_ERROR");
            break;

        case EXCEPTION_FS_ERROR:
            DEBUG_PR_ERROR("EXCEPTION_FS_ERROR");
            break;

        case EXCEPTION_CELLULAR_SEND_ERROR:
            DEBUG_PR_ERROR("EXCEPTION_CELLULAR_SEND_ERROR");
            break;

        case EXCEPTION_BOOT_ERROR:
            DEBUG_PR_ERROR("EXCEPTION_BOOT_ERROR");
            break;

        case EXCEPTION_FLASH_ERROR:
            DEBUG_PR_ERROR("EXCEPTION_FLASH_ERROR");
            break;

        default:
            DEBUG_PR_ERROR("Unknown message exception");
            break;
    }
}

static inline void config_if_timeout_reset(void)
{
    config_if_message_timeout = syshal_time_get_ticks_ms();
}

static void message_set_state(sm_message_state_t s)
{
    config_if_timeout_reset();
    message_state = s;
}

static void handle_config_if_messages(void)
{
    CEXCEPTION_T e = CEXCEPTION_NONE;

    // Has a message timeout occured?
    if ((syshal_time_get_ticks_ms() - config_if_message_timeout) > SM_MAIN_INACTIVITY_TIMEOUT_MS)
    {
        if (SM_MESSAGE_STATE_IDLE != message_state) // Our idle state can't timeout
        {
            DEBUG_PR_WARN("State: %d, MESSAGE TIMEOUT", message_state);
            message_set_state(SM_MESSAGE_STATE_IDLE); // Return to the idle state
            config_if_session_cleanup(); // Clear any pending messages
        }
    }

    // Don't allow the processing of anymore messages until we have a free transmit buffer
    if (config_if_tx_pending)
        return;

    Try
    {

        switch (message_state)
        {
            case SM_MESSAGE_STATE_IDLE: // No message is currently being handled
                message_idle_state();
                config_if_timeout_reset(); // Reset the timeout counter
                break;

            case SM_MESSAGE_STATE_CFG_READ_NEXT:
                cfg_read_next_state();
                break;

            case SM_MESSAGE_STATE_CFG_WRITE_NEXT:
                cfg_write_next_state();
                break;

            case SM_MESSAGE_STATE_CFG_WRITE_ERROR:
                cfg_write_error_state();
                break;

            case SM_MESSAGE_STATE_GPS_WRITE_NEXT:
                gps_write_next_state();
                break;

            case SM_MESSAGE_STATE_GPS_READ_NEXT:
                gps_read_next_state();
                break;

            case SM_MESSAGE_STATE_LOG_READ_NEXT:
                log_read_next_state();
                break;

            case SM_MESSAGE_STATE_FW_SEND_IMAGE_NEXT:
                fw_send_image_next_state();
                break;
            case SM_MESSAGE_STATE_CELLULAR_WRITE_NEXT:
                cellular_write_next_state();
                break;

            case SM_MESSAGE_STATE_CELLULAR_READ_NEXT:
                cellular_read_next_state();
                break;

            case SM_MESSAGE_STATE_FLASH_DOWNLOAD_NEXT:
                flash_download_next_state();
                break;

            default:
                // TODO: add an illegal error state here for catching
                // invalid state changes
                break;
        }

    } Catch (e)
    {
        state_message_exception_handler(e);
    }
}

static void log_system_startup_event(void)
{
    while (system_startup_log_required)
    {
        system_startup_log_required = false;

        // Try to log startup event
        int ret = fs_open(file_system, &sm_main_file_handle, FILE_ID_LOG, FS_MODE_WRITEONLY, NULL); // Open the log file
        if (FS_NO_ERROR != ret)
        {
            // Unable to log this entry, so skip
            break;
        }

        logging_startup_t log_start;
        uint32_t bytes_written;

        if (sys_config.logging_date_time_stamp_enable.contents.enable)
        {
            syshal_rtc_data_and_time_t current_time;
            logging_date_time_t log_date;

            syshal_rtc_get_date_and_time(&current_time);

            log_date.h.id = LOGGING_DATE_TIME;
            log_date.day = current_time.day;
            log_date.month = current_time.month;
            log_date.year = current_time.year;
            log_date.hours = current_time.hours;
            log_date.minutes = current_time.minutes;
            log_date.seconds = current_time.seconds;
            (void)fs_write(sm_main_file_handle, &log_date, sizeof(log_date), &bytes_written);
        }

        log_start.h.id = LOGGING_STARTUP;
        log_start.cause = syshal_pmu_get_startup_status();
        (void)fs_write(sm_main_file_handle, &log_start, sizeof(log_start), &bytes_written);

        // Always flush this log entry so we can always see when a resets occurs
        fs_close(sm_main_file_handle);
    }
}

////////////////////////////////////////////////////////////////////////////////
///////////////////////////// STATE EXECUTION CODE /////////////////////////////
////////////////////////////////////////////////////////////////////////////////
static void sm_main_boot(sm_handle_t * state_handle)
{
    int ret;
    CEXCEPTION_T e = CEXCEPTION_NONE;

    // Set all our global static variables to their default values
    // This is done to ensure individual unit tests all start in the same state
    set_default_global_values();

    Try
    {
        syshal_pmu_init();

        if (syshal_rtc_init())
            Throw(EXCEPTION_BOOT_ERROR);

        if (syshal_led_init())
            Throw(EXCEPTION_BOOT_ERROR);

        if (syshal_time_init())
            Throw(EXCEPTION_BOOT_ERROR);

        setup_buffers();

        syshal_gpio_init(GPIO_REED_SW);
        syshal_gpio_enable_interrupt(GPIO_REED_SW, gpio_reed_sw_callback);

        for (uint32_t i = 0; i < UART_TOTAL_NUMBER; ++i)
            if (syshal_uart_init(i))
                Throw(EXCEPTION_BOOT_ERROR);

        for (uint32_t i = 0; i < SPI_TOTAL_NUMBER; ++i)
            if (syshal_spi_init(i))
                Throw(EXCEPTION_BOOT_ERROR);

//    for (uint32_t i = 0; i < QSPI_TOTAL_NUMBER; ++i)
//        syshal_qspi_init(i);

        for (uint32_t i = 0; i < I2C_TOTAL_NUMBER; ++i)
            if (syshal_i2c_init(i))
                Throw(EXCEPTION_BOOT_ERROR);

        // Init timers
        if (syshal_timer_init(&timer_gps_interval, timer_gps_interval_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_gps_no_fix, timer_gps_no_fix_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_gps_maximum_acquisition, timer_gps_maximum_acquisition_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_gps_test_fix_hold_time, timer_gps_test_fix_hold_time_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_gps_watchdog, timer_gps_watchdog_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_log_flush, timer_log_flush_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_saltwater_switch_hysteresis, timer_saltwater_switch_hysteresis_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_reed_switch_hysteresis, timer_reed_switch_hysteresis_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_pressure_interval, timer_pressure_interval_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_pressure_maximum_acquisition, timer_pressure_maximum_acquisition_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_axl_interval, timer_axl_interval_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_axl_maximum_acquisition, timer_axl_maximum_acquisition_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_ble_interval, timer_ble_interval_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_ble_duration, timer_ble_duration_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_timer_init(&timer_ble_timeout, timer_ble_timeout_callback))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_flash_init(0, SPI_FLASH))
            Throw(EXCEPTION_BOOT_ERROR);
        if (syshal_batt_init())
            DEBUG_PR_WARN("Battery monitoring IC failed to initialise. Percentage charge logging may not work");

        if (syshal_cellular_init())
            Throw(EXCEPTION_BOOT_ERROR);

        // Re/Set global vars
        syshal_gps_bridging = false;
        syshal_ble_bridging = false;
        syshal_cellular_bridging = false;

        // Print General System Info
        DEBUG_PR_SYS("Arribada Tracker Device");
        DEBUG_PR_SYS("Version:  %s", GIT_VERSION);
        DEBUG_PR_SYS("Compiled: %s %s With %s", COMPILE_DATE, COMPILE_TIME, COMPILER_NAME);
        DEBUG_PR_SYS("Startup/Reset reason 0x%08lX", syshal_pmu_get_startup_status());

        // Start the soft watchdog timer
        if (syshal_rtc_soft_watchdog_enable(SOFT_WATCHDOG_TIMEOUT_S, soft_watchdog_callback))
            Throw(EXCEPTION_BOOT_ERROR);

        // Load the file system
        if (fs_init(FS_DEVICE))
            Throw(EXCEPTION_BOOT_ERROR);

        if (fs_mount(FS_DEVICE, &file_system))
            Throw(EXCEPTION_BOOT_ERROR);

        // Determine if a log file exists or not
        ret = fs_open(file_system, &sm_main_file_handle, FILE_ID_LOG, FS_MODE_READONLY, NULL);

        if (FS_NO_ERROR == ret)
        {
            log_file_created = true;
            fs_close(sm_main_file_handle);
        }
        else
        {
            log_file_created = false;
        }

        ret = sys_config_load_from_fs(file_system);

        if (SYS_CONFIG_NO_ERROR != ret && SYS_CONFIG_ERROR_NO_VALID_CONFIG_FILE_FOUND != ret)
            Throw(EXCEPTION_BOOT_ERROR);


        // Attempt to log system startup event into the log file
        log_system_startup_event();

        // Delete any firmware images we may have
        fs_delete(file_system, FILE_ID_APP_FIRM_IMAGE);

        // Init the peripheral devices after configuration data has been collected
        if (syshal_gps_init())
            Throw(EXCEPTION_BOOT_ERROR);
        sm_gps_state = SM_GPS_STATE_ASLEEP; // GPS starts off
        sys_config.gps_last_known_position.hdr.set = false; // Invalidate any prior last location

        if (syshal_switch_init())
            Throw(EXCEPTION_BOOT_ERROR);
    }
    Catch(e)
    {
        syshal_led_set_blinking(SYSHAL_LED_COLOUR_RED, LED_BLINK_FAIL_DURATION_MS);
        led_finish_time = 0;
        state_message_exception_handler(e);
        sm_set_next_state(state_handle, SM_MAIN_ERROR);
        return;
    }

    tracker_above_water = !syshal_switch_get();
    ret = fs_open(file_system, &sm_main_file_handle, FILE_ID_CONF_COMMANDS, FS_MODE_READONLY, NULL);
    if (FS_NO_ERROR == ret)
    {
        // Branch to the provisioning state if there is a fs script file
        fs_close(sm_main_file_handle);
        config_if_backend_t backend;
        backend.id = CONFIG_IF_BACKEND_FS_SCRIPT;
        backend.fs_script.filesystem = file_system;
        backend.fs_script.file_id = FILE_ID_CONF_COMMANDS;
        config_if_init(backend);
        sm_set_next_state(state_handle, SM_MAIN_PROVISIONING);
    }
    else if (syshal_usb_plugged_in())
    {
        // Branch to Battery Charging state if VUSB is present
        sm_set_next_state(state_handle, SM_MAIN_BATTERY_CHARGING);
    }
    else if (check_configuration_tags_set() && log_file_created)
    {
        // Branch to Operational state if log file exists and configuration tags are set
        sm_set_next_state(state_handle, SM_MAIN_OPERATIONAL);
    }
    else
    {
        // Branch to Provisioning Needed state
        sm_set_next_state(state_handle, SM_MAIN_PROVISIONING_NEEDED);
    }

    if (sm_is_last_entry(state_handle))
    {
        syshal_led_off();
    }
}
static void sm_main_error(sm_handle_t * state_handle)
{
    while (true);
}

static void sm_main_operational(sm_handle_t * state_handle)
{
    int ret;
    KICK_WATCHDOG();

    if (sm_is_first_entry(state_handle))
    {
        DEBUG_PR_INFO("Entered state %s from %s",
                      sm_main_state_str[sm_get_current_state(state_handle)],
                      sm_main_state_str[sm_get_last_state(state_handle)]);

        syshal_button_init(GPIO_DFU_BOOT);

        GPS_off_no_log();

        // Setup the IoT layer
        sm_iot_init_t config;
        config.iot_init.iot_config = &sys_config.iot_general_settings;
        config.iot_init.iot_cellular_config = &sys_config.iot_cellular_settings;
        config.iot_init.iot_cellular_apn = &sys_config.iot_cellular_apn;
        config.iot_init.iot_cellular_aws_config = &sys_config.iot_cellular_aws_settings;
        config.iot_init.iot_sat_config = &sys_config.iot_sat_settings;
        config.iot_init.iot_sat_artic_config = &sys_config.iot_sat_artic_settings;
        config.iot_init.system_device_identifier = &sys_config.system_device_identifier;
        config.gps_last_known_position = &sys_config.gps_last_known_position;
        sm_iot_init(config);

        // Led for showing it enters in operational state
        led_finish_time = syshal_time_get_ticks_ms() + LED_DURATION_MS;
        syshal_led_set_blinking(SYSHAL_LED_COLOUR_GREEN, LED_BLINK_TEST_PASSED_DURATION_MS);

        // Allow one shot ble every time we enter in operational state from provisioning or battery_charging
        if (sm_get_last_state(state_handle) == SM_MAIN_PROVISIONING ||
            sm_get_last_state(state_handle) == SM_MAIN_BATTERY_CHARGING)
            ble_one_shot_used = false;

        ret = fs_open(file_system, &sm_main_file_handle, FILE_ID_LOG, FS_MODE_WRITEONLY, NULL); // Open the log file

        if (FS_NO_ERROR != ret)
        {
            // Fatal error
            Throw(EXCEPTION_FS_ERROR);
        }

        if (system_startup_log_required)
        {
            logging_startup_t log_start;
            uint32_t bytes_written;

            system_startup_log_required = false;

            if (sys_config.logging_date_time_stamp_enable.hdr.set &&
                sys_config.logging_date_time_stamp_enable.contents.enable)
            {
                syshal_rtc_data_and_time_t current_time;
                logging_date_time_t log_date;

                syshal_rtc_get_date_and_time(&current_time);

                log_date.h.id = LOGGING_DATE_TIME;
                log_date.day = current_time.day;
                log_date.month = current_time.month;
                log_date.year = current_time.year;
                log_date.hours = current_time.hours;
                log_date.minutes = current_time.minutes;
                log_date.seconds = current_time.seconds;
                (void)fs_write(sm_main_file_handle, &log_date, sizeof(log_date), &bytes_written);
            }

            log_start.h.id = LOGGING_STARTUP;
            log_start.cause = syshal_pmu_get_startup_status();
            (void)fs_write(sm_main_file_handle, &log_start, sizeof(log_start), &bytes_written);

            // Always flush this log entry so we can always see when a resets occurs
            fs_flush(sm_main_file_handle);
        }

        // Start the log file flushing timer
        syshal_timer_set(timer_log_flush, periodic, LOG_FILE_FLUSH_PERIOD_SECONDS);

        gps_ttff_reading_logged = false; // Make sure we read/log the first TTFF reading
        last_battery_reading = 0xFF; // Ensure the first battery reading is logged

        if (sys_config.logging_enable.hdr.set &&
            sys_config.logging_enable.contents.enable)
            sensor_logging_enabled = true;

        if ((sys_config.gps_log_position_enable.hdr.set && sys_config.gps_log_position_enable.contents.enable) ||
            (sys_config.gps_log_ttff_enable.hdr.set && sys_config.gps_log_ttff_enable.contents.enable))
        {
            // Clear the GPS buffer
            uint8_t flush;
            while (syshal_gps_receive_raw(&flush, 1))
            {}

            // AG-176: We must ensure that we don't re-enter very first fix state e.g.,
            // if a IWDG or soft reset occurs.  This state is reserved only for the
            // situation when we have just finished provisioning.
            if (sys_config.gps_test_fix_hold_time.hdr.set &&
                sys_config.gps_test_fix_hold_time.contents.seconds && (test_state_gps == SM_TEST_REQUEST) &&
                (sm_get_last_state(state_handle) == SM_MAIN_PROVISIONING ||
                 sm_get_last_state(state_handle) == SM_MAIN_BATTERY_CHARGING))
            {
                test_state_gps = SM_TEST_WAITING;
            }
            else
            {
                test_state_gps = SM_TEST_OFF;
                setup_GPS_based_on_configuration(); // Set the GPS to standard operation
            }
        }
        else
        {
            GPS_off();
        }

        gps_positions_counts = 0;
        gps_interval_using_max = false;

        if ( (!syshal_cellular_bridging) &&
             sys_config.iot_cellular_settings.hdr.set &&
             sys_config.iot_cellular_settings.contents.enable &&
             (test_state_cellular == SM_TEST_REQUEST) &&
             (sm_get_last_state(state_handle) == SM_MAIN_PROVISIONING ||
              sm_get_last_state(state_handle) == SM_MAIN_BATTERY_CHARGING))
        {
            //Waiting in case test gps is actived
            test_state_cellular = SM_TEST_WAITING;
        }
        else
        {
            test_state_cellular = SM_TEST_OFF;
        }

        // Should we be logging pressure data?
        if (sys_config.pressure_sensor_log_enable.hdr.set &&
            sys_config.pressure_sensor_log_enable.contents.enable)
        {
            syshal_pressure_init();
            if (SYS_CONFIG_PRESSURE_MODE_PERIODIC == sys_config.pressure_mode.contents.mode)
            {
                // If SYS_CONFIG_TAG_PRESSURE_SCHEDULED_ACQUISITION_INTERVAL = 0 then this is a special case meaning to always run the pressure sensor
                if (sys_config.pressure_scheduled_acquisition_interval.hdr.set &&
                    sys_config.pressure_scheduled_acquisition_interval.contents.seconds)
                    syshal_timer_set(timer_pressure_interval, periodic, sys_config.pressure_scheduled_acquisition_interval.contents.seconds);
                else
                    syshal_pressure_wake();
            }
        }

        // Should we be logging axl data?
        if (sys_config.axl_log_enable.hdr.set &&
            sys_config.axl_log_enable.contents.enable)
        {
            syshal_axl_init();
            if (SYS_CONFIG_AXL_MODE_PERIODIC == sys_config.axl_mode.contents.mode)
            {
                // If SYS_CONFIG_TAG_AXL_SCHEDULED_ACQUISITION_INTERVAL = 0 then this is a special case meaning to always run the AXL sensor
                if (sys_config.pressure_scheduled_acquisition_interval.hdr.set &&
                    sys_config.pressure_scheduled_acquisition_interval.contents.seconds)
                    syshal_timer_set(timer_axl_interval, periodic, sys_config.axl_scheduled_acquisition_interval.contents.seconds);
                else
                    syshal_axl_wake();
            }
        }
    }

    // If GPS logging enabled
    if ((sys_config.gps_log_position_enable.hdr.set && sys_config.gps_log_position_enable.contents.enable) ||
        (sys_config.gps_log_ttff_enable.hdr.set && sys_config.gps_log_ttff_enable.contents.enable))
    {
        if (test_state_gps == SM_TEST_WAITING && (!is_test_active_or_finishing_or_led_active()))
        {
            /*led gps test characteristic */
            led_finish_time = 0;
            test_state_gps = SM_TEST_ACTIVE;
            // Wake the GPS if it is asleep
            GPS_on();
        }
        else if (test_state_gps == SM_TEST_ACTIVE)
        {
            if (SM_GPS_STATE_FIXED == sm_gps_state)
            {
                syshal_led_set_solid(SYSHAL_LED_COLOUR_WHITE);
                // Wait until the GPS is fixed for the hold time before pass test
                if (SYSHAL_TIMER_NOT_RUNNING == syshal_timer_running(timer_gps_test_fix_hold_time)) // The GPS is locked. So start the first fix hold timer
                    syshal_timer_set(timer_gps_test_fix_hold_time, one_shot, sys_config.gps_test_fix_hold_time.contents.seconds);
            }
            else
            {
                bool is_blinking;
                syshal_led_get(NULL, &is_blinking);
                if (!is_blinking)
                    syshal_led_set_blinking(SYSHAL_LED_COLOUR_WHITE, LED_BLINK_TEST_PASSED_DURATION_MS);
                // We're not locked so don't start the first fix hold timer Restart hold time next time we are locked
                syshal_timer_cancel(timer_gps_test_fix_hold_time);
            }
        }

        syshal_gps_tick();
    }

    if ( (!syshal_cellular_bridging) &&
         (sys_config.iot_cellular_settings.hdr.set && sys_config.iot_cellular_settings.contents.enable &&
          (test_state_cellular == SM_TEST_WAITING) && !is_test_active_or_finishing_or_led_active()))
    {
        test_state_cellular = SM_TEST_ACTIVE;
        syshal_led_set_blinking(SYSHAL_LED_COLOUR_YELLOW, LED_BLINK_TEST_PASSED_DURATION_MS);
        led_finish_time = 0;
        // It will set to true inside sm_iot_callback

        /* Already init in the operational state */
        if (!sm_iot_trigger_force(IOT_RADIO_CELLULAR)) // Pass test
        {
            syshal_led_set_solid(SYSHAL_LED_COLOUR_YELLOW);
        }
        else // Fail test
        {
            syshal_led_set_blinking(SYSHAL_LED_COLOUR_YELLOW, LED_BLINK_FAIL_DURATION_MS);
        }

        test_state_cellular = SM_TEST_FINISHING;
        led_finish_time = syshal_time_get_ticks_ms() + LED_DURATION_MS;
    }


    if (sys_config.pressure_sensor_log_enable.hdr.set &&
        sys_config.pressure_sensor_log_enable.contents.enable)
        syshal_pressure_tick();

    if (sys_config.axl_log_enable.hdr.set &&
        sys_config.axl_log_enable.contents.enable)
        syshal_axl_tick();

    // Determine how deep a sleep we should take
    if (!syshal_pressure_awake() && !syshal_axl_awake() && !is_test_active_or_finishing_or_led_active())
    {
        syshal_pmu_sleep(SLEEP_DEEP);
    }

    // Get the battery level state
    uint8_t level;
    ret = syshal_batt_level(&level);
    if (!ret) // If we've read the battery level successfully
    {
        // Has our battery level decreased
        if (last_battery_reading > level)
        {
            // Should we log this?
            if (sys_config.battery_log_enable.hdr.set &&
                sys_config.battery_log_enable.contents.enable)
            {
                // Log the battery
                logging_battery_t battery_log;

                LOGGING_SET_HDR(&battery_log, LOGGING_BATTERY);
                battery_log.charge = level;
                logging_add_to_buffer((uint8_t *) &battery_log, sizeof(battery_log));
            }

            // Should we check to see if we should enter a low power state?
            if (sys_config.battery_low_threshold.hdr.set &&
                level <= sys_config.battery_low_threshold.contents.threshold)
                sm_set_next_state(state_handle, SM_MAIN_BATTERY_LEVEL_LOW);

            last_battery_reading = level;
        }
    }

    // Is global logging enabled?
    if (sys_config.logging_enable.hdr.set &&
        sys_config.logging_enable.contents.enable)
    {
        // Is there any data waiting to be written to the log file?
        uint8_t * read_buffer;
        uint32_t length = buffer_read(&logging_buffer, (uintptr_t *)&read_buffer);

        while (length) // Then write all of it
        {
            uint32_t bytes_written;
            ret = fs_write(sm_main_file_handle, read_buffer, length, &bytes_written);

#ifndef DEBUG_DISABLED
            DEBUG_PR_TRACE("Writing to Log File");
            printf("Contents: ");
            for (uint32_t i = 0; i < length; ++i)
                printf("%02X ", read_buffer[i]);
            printf("\r\n");
#endif

            if (FS_NO_ERROR == ret)
            {
                buffer_read_advance(&logging_buffer, length);
            }
            else if (FS_ERROR_FILESYSTEM_FULL == ret)
            {
                // Branch to Log File Full state if log file is full
                sm_set_next_state(state_handle, SM_MAIN_LOG_FILE_FULL);
                break;
            }
            else
            {
                Throw(EXCEPTION_FS_ERROR);
            }

            length = buffer_read(&logging_buffer, (uintptr_t *)&read_buffer);
        }
    }

    // Have we reached our trigger condition for sending an IoT update over cellular?
    if (!is_test_active_or_finishing_or_led_active() &&
        sys_config.iot_general_settings.hdr.set && sys_config.iot_general_settings.contents.enable &&
        sys_config.iot_cellular_settings.hdr.set && sys_config.iot_cellular_settings.contents.enable)
    {
        if (gps_positions_counts >= sys_config.iot_cellular_settings.contents.min_updates)
        {
            gps_positions_counts = 0;
            fs_flush(sm_main_file_handle);
            sm_iot_trigger(IOT_RADIO_CELLULAR);
        }
    }

    // Turn off led after led_finish_time
    if (syshal_led_is_active())
    {
        uint32_t current_time = syshal_time_get_ticks_ms();

        //If there a no finish time or the current time is less than the finish time
        if (led_finish_time != 0 && current_time  > led_finish_time)
        {
            syshal_led_off();
            if (test_state_gps == SM_TEST_FINISHING)
            {
                test_state_gps = SM_TEST_OFF;
            }
            else if (test_state_cellular == SM_TEST_FINISHING)
            {
                test_state_cellular = SM_TEST_OFF;
            }
            else if (test_state_satellite == SM_TEST_FINISHING)
            {
                test_state_satellite = SM_TEST_OFF;
            }
        }
    }

    syshal_timer_tick();

    // Branch to Battery Charging if VUSB is present
    if (syshal_usb_plugged_in())
        sm_set_next_state(state_handle, SM_MAIN_BATTERY_CHARGING);

    manage_ble();

    config_if_tick();

    // Branch to Provisioning state if config_if has connected
    if (config_if_connected)
        sm_set_next_state(state_handle, SM_MAIN_PROVISIONING);

    // Are we about to leave this state?
    if (sm_is_last_entry(state_handle))
    {
        syshal_button_term(GPIO_DFU_BOOT);

        // Close any open files
        fs_close(sm_main_file_handle);

        syshal_axl_term();
        syshal_pressure_term();

        // Disable the IoT layer
        sm_iot_term();

        // Sleep the GPS to save power
        GPS_off();

        syshal_led_off();
        test_state_cellular = SM_TEST_OFF;
        test_state_satellite = SM_TEST_OFF;
        test_state_gps = SM_TEST_OFF;

        // Stop any unnecessary timers
        syshal_timer_cancel(timer_gps_interval);
        syshal_timer_cancel(timer_gps_no_fix);
        syshal_timer_cancel(timer_gps_maximum_acquisition);
        syshal_timer_cancel(timer_gps_test_fix_hold_time);
        syshal_timer_cancel(timer_gps_watchdog);
        syshal_timer_cancel(timer_log_flush);
        syshal_timer_cancel(timer_pressure_interval);
        syshal_timer_cancel(timer_pressure_maximum_acquisition);
        syshal_timer_cancel(timer_axl_interval);
        syshal_timer_cancel(timer_axl_maximum_acquisition);

        sensor_logging_enabled = false; // Prevent any sensors from logging but still other logs eg. BLE connection events
    }
}

static void sm_main_log_file_full(sm_handle_t * state_handle)
{
    if (sm_is_first_entry(state_handle))
    {
        DEBUG_PR_INFO("Entered state %s from %s",
                      sm_main_state_str[sm_get_current_state(state_handle)],
                      sm_main_state_str[sm_get_last_state(state_handle)]);
    }

    KICK_WATCHDOG();

    syshal_timer_tick();

    manage_ble();

    config_if_tick();

    syshal_pmu_sleep(SLEEP_DEEP);

    // Branch to Provisioning state if config_if has connected
    if (config_if_connected)
        sm_set_next_state(state_handle, SM_MAIN_PROVISIONING);

    // Branch to Battery Charging if VUSB is present
    if (syshal_usb_plugged_in())
        sm_set_next_state(state_handle, SM_MAIN_BATTERY_CHARGING);

    // Branch to Battery Low state if battery is beneath threshold
    uint8_t level;
    int ret = syshal_batt_level(&level);
    if (!ret)
        if (sys_config.battery_low_threshold.hdr.set &&
            level <= sys_config.battery_low_threshold.contents.threshold)
            sm_set_next_state(state_handle, SM_MAIN_BATTERY_LEVEL_LOW);
}

static void sm_main_battery_charging(sm_handle_t * state_handle)
{
    static uint32_t usb_enumeration_timeout;

    KICK_WATCHDOG();

    if (sm_is_first_entry(state_handle))
    {
        DEBUG_PR_INFO("Entered state %s from %s",
                      sm_main_state_str[sm_get_current_state(state_handle)],
                      sm_main_state_str[sm_get_last_state(state_handle)]);

        // If we've just entered the charging state try to enumerate for USB_ENUMERATION_TIMEOUT seconds
        if (CONFIG_IF_BACKEND_USB != config_if_current())
        {
            config_if_term();
            config_if_backend_t backend = {.id = CONFIG_IF_BACKEND_USB};
            config_if_init(backend);
            usb_enumeration_timeout = syshal_time_get_ticks_ms();
        }
    }

    manage_ble();

    config_if_tick();

    syshal_timer_tick();

    // Branch to Provisioning state if config_if has connected
    if (config_if_connected)
        sm_set_next_state(state_handle, SM_MAIN_PROVISIONING);

    // Has our USB enumeration attempt timed out?
    if (syshal_time_get_ticks_ms() - usb_enumeration_timeout >= USB_ENUMERATION_TIMEOUT_MS)
    {
        if (CONFIG_IF_BACKEND_USB == config_if_current())
        {
            DEBUG_PR_TRACE("USB enumeration timed out");
            config_if_term();
        }
    }

    if (!syshal_usb_plugged_in())
    {
        // Our charging voltage has been removed

        // Branch to Operational state if log file exists and configuration tags are set
        if (check_configuration_tags_set() && log_file_created)
            sm_set_next_state(state_handle, SM_MAIN_OPERATIONAL);
        else
            // Branch to Provisioning Needed state if configuration tags are not set OR log file doesn't exist
            sm_set_next_state(state_handle, SM_MAIN_PROVISIONING_NEEDED);

        // Branch to Battery Low state if battery is beneath threshold
        uint8_t level;
        int ret = syshal_batt_level(&level);
        if (!ret)
            // If we've read the battery level successfully
            if (sys_config.battery_low_threshold.hdr.set &&
                level <= sys_config.battery_low_threshold.contents.threshold)
                sm_set_next_state(state_handle, SM_MAIN_BATTERY_LEVEL_LOW);

        // Terminate any attempt to connect over VUSB
        if (CONFIG_IF_BACKEND_USB == config_if_current())
            config_if_term();
    }
}

static void sm_main_battery_level_low(sm_handle_t * state_handle)
{
    KICK_WATCHDOG();

    if (sm_is_first_entry(state_handle))
    {
        DEBUG_PR_INFO("Entered state %s from %s",
                      sm_main_state_str[sm_get_current_state(state_handle)],
                      sm_main_state_str[sm_get_last_state(state_handle)]);

        config_if_term();

        // Sleep the GPS to save power
        GPS_off();
    }

    syshal_pmu_sleep(SLEEP_DEEP);

    // Branch to Battery Charging if VUSB is present
    if (syshal_usb_plugged_in())
        sm_set_next_state(state_handle, SM_MAIN_BATTERY_CHARGING);
}

static void sm_main_provisioning_needed(sm_handle_t * state_handle)
{
    KICK_WATCHDOG();

    if (sm_is_first_entry(state_handle))
    {
        DEBUG_PR_INFO("Entered state %s from %s",
                      sm_main_state_str[sm_get_current_state(state_handle)],
                      sm_main_state_str[sm_get_last_state(state_handle)]);

        syshal_led_set_blinking(SYSHAL_LED_COLOUR_RED, LED_BLINK_TEST_PASSED_DURATION_MS);
        // Sleep the GPS to save power
        GPS_off();
    }

    manage_ble();

    config_if_tick();

    syshal_timer_tick();

    // Branch to Battery Charging if VUSB is present
    if (syshal_usb_plugged_in())
    {
        sm_set_next_state(state_handle, SM_MAIN_BATTERY_CHARGING);
    }

    // Branch to Provisioning state if config_if has connected
    if (config_if_connected)
    {
        sm_set_next_state(state_handle, SM_MAIN_PROVISIONING);
    }

    // Branch to Battery Low state if battery is beneath threshold
    uint8_t level;
    int ret = syshal_batt_level(&level);
    if (!ret)
    {
        // If we've read the battery level successfully
        if (sys_config.battery_low_threshold.hdr.set &&
            level <= sys_config.battery_low_threshold.contents.threshold)
        {
            sm_set_next_state(state_handle, SM_MAIN_BATTERY_LEVEL_LOW);
        }
    }
    if (sm_is_last_entry(state_handle))
        syshal_led_off();

}

static void sm_main_provisioning(sm_handle_t * state_handle)
{
    KICK_WATCHDOG();

    if (sm_is_first_entry(state_handle))
    {
        DEBUG_PR_INFO("Entered state %s from %s",
                      sm_main_state_str[sm_get_current_state(state_handle)],
                      sm_main_state_str[sm_get_last_state(state_handle)]);

        // Wake the GPS so the configuration interface can communicate with it
        GPS_on();

        syshal_cellular_power_on();
    }

    bool ready_for_operational_state = check_configuration_tags_set() && log_file_created;
    // Show if device is ready for operation by using GREEN LED during 5s
    if (ready_for_operational_state)
        syshal_led_set_solid(SYSHAL_LED_COLOUR_GREEN);
    else
        syshal_led_set_solid(SYSHAL_LED_COLOUR_RED);

    manage_ble();

    config_if_tick();
    syshal_timer_tick();
    if (config_if_connected)
    {
        handle_config_if_messages();
    }
    else
    {
        // Our configuration interface has been disconnected
        if (ready_for_operational_state)
        {
            // Branch to Operational state if log file exists AND configuration tags are set
            sm_set_next_state(state_handle, SM_MAIN_OPERATIONAL);
        }
        else
        {
            // Branch to Provisioning Needed state if configuration tags are not set OR log file doesn't exist
            sm_set_next_state(state_handle, SM_MAIN_PROVISIONING_NEEDED);
        }

        // Branch to Battery Low state if battery is beneath threshold
        uint8_t level;
        int ret = syshal_batt_level(&level);
        if (!ret)
            // If we've read the battery level successfully
            if (sys_config.battery_low_threshold.hdr.set &&
                level <= sys_config.battery_low_threshold.contents.threshold)
                sm_set_next_state(state_handle, SM_MAIN_BATTERY_LEVEL_LOW);

        // Branch to Battery Charging if VUSB is present
        if (syshal_usb_plugged_in())
            sm_set_next_state(state_handle, SM_MAIN_BATTERY_CHARGING);
    }

    // Are we about to leave this state?
    if (sm_is_last_entry(state_handle))
    {
        message_set_state(SM_MESSAGE_STATE_IDLE); // Return the message handler to the idle state
        config_if_session_cleanup(); // Clear any pending messages
        syshal_cellular_bridging = false;

        syshal_led_off();
        syshal_cellular_power_off();
        // Close any open files
        fs_close(sm_main_file_handle);

        // Close the configuration interface if it's USB or fs_script
        if (CONFIG_IF_BACKEND_USB == config_if_current() ||
            CONFIG_IF_BACKEND_FS_SCRIPT == config_if_current())
            config_if_term();
    }
}

////////////////////////////////////////////////////////////////////////////////
//////////////////////////////// STATE HANDLERS ////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

void sm_main_exception_handler(CEXCEPTION_T e)
{
    switch (e)
    {
        case EXCEPTION_REQ_WRONG_SIZE:
            DEBUG_PR_ERROR("EXCEPTION_REQ_WRONG_SIZE");
            break;

        case EXCEPTION_RESP_TX_PENDING:
            DEBUG_PR_ERROR("EXCEPTION_RESP_TX_PENDING");
            break;

        case EXCEPTION_TX_BUFFER_FULL:
            DEBUG_PR_ERROR("EXCEPTION_TX_BUFFER_FULL");
            break;

        case EXCEPTION_TX_BUSY:
            DEBUG_PR_ERROR("EXCEPTION_TX_BUSY");
            break;

        case EXCEPTION_RX_BUFFER_EMPTY:
            DEBUG_PR_ERROR("EXCEPTION_RX_BUFFER_EMPTY");
            break;

        case EXCEPTION_RX_BUFFER_FULL:
            DEBUG_PR_ERROR("EXCEPTION_RX_BUFFER_FULL");
            break;

        case EXCEPTION_BAD_SYS_CONFIG_ERROR_CONDITION:
            DEBUG_PR_ERROR("EXCEPTION_BAD_SYS_CONFIG_ERROR_CONDITION");
            break;

        case EXCEPTION_PACKET_WRONG_SIZE:
            DEBUG_PR_ERROR("EXCEPTION_PACKET_WRONG_SIZE");
            break;

        case EXCEPTION_GPS_SEND_ERROR:
            DEBUG_PR_ERROR("EXCEPTION_GPS_SEND_ERROR");
            break;

        case EXCEPTION_FS_ERROR:
            DEBUG_PR_ERROR("EXCEPTION_FS_ERROR");
            break;

        case EXCEPTION_SPI_ERROR:
            DEBUG_PR_ERROR("EXCEPTION_SPI_ERROR");
            break;
        case EXCEPTION_CELLULAR_SEND_ERROR:
            DEBUG_PR_ERROR("EXCEPTION_CELLULAR_SEND_ERROR");
            break;

        default:
            DEBUG_PR_ERROR("Unknown state exception %d", e);
            break;
    }
}

void syshal_flash_busy_handler(uint32_t drive)
{
    /* Kick the software watchdog */
    KICK_WATCHDOG();

    /* We must also kick the hardware watchdog here */
    syshal_pmu_kick_watchdog();
}

void at_busy_handler(void)
{
    /* Kick the software watchdog */
    KICK_WATCHDOG();

    /* We must also kick the hardware watchdog here */
    syshal_pmu_kick_watchdog();
}