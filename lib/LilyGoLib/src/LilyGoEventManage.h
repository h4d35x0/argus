/**
 * @file      LilyGoEventManage.h
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-03-18
 *
 */
#include <Arduino.h>
#include <vector>

typedef enum ButtonEvent {
    BUTTON_EVENT_NONE,
    BUTTON_EVENT_RELEASED,
    BUTTON_EVENT_PRESSED,
    BUTTON_EVENT_CLICK,
    BUTTON_EVENT_LONG_PRESSED,
    BUTTON_EVENT_DOUBLE_CLICK,
} ButtonEvent_t;

typedef struct ButtonEventParam {
    uint8_t id;
    uint8_t event;
} ButtonEventParam_t;

typedef enum TrackballDir {
    TRACKBALL_DIR_NONE,
    TRACKBALL_DIR_UP,
    TRACKBALL_DIR_DOWN,
    TRACKBALL_DIR_LEFT,
    TRACKBALL_DIR_RIGHT
} TrackballDir_t;

typedef enum SDEvent {
    SDCARD_EVENT_NONE,
    SDCARD_EVENT_REMOVE,
    SDCARD_EVENT_INSERT
} SDEvent_t;

typedef enum PMUEventType {
    PMU_EVENT_NONE,
    PMU_EVENT_BATTERY_LOW_TEMP,
    PMU_EVENT_BATTERY_HIGH_TEMP,
    PMU_EVENT_CHARGE_LOW_TEMP,
    PMU_EVENT_CHARGE_HIGH_TEMP,
    PMU_EVENT_LOW_VOLTAGE_LEVEL1,
    PMU_EVENT_LOW_VOLTAGE_LEVEL2,
    PMU_EVENT_KEY_CLICKED,
    PMU_EVENT_KEY_LONG_PRESSED,
    PMU_EVENT_BATTERY_REMOVE,
    PMU_EVENT_BATTERY_INSERT,
    PMU_EVENT_USBC_REMOVE,
    PMU_EVENT_USBC_INSERT,
    PMU_EVENT_BATTERY_OVER_VOLTAGE,
    PMU_EVENT_CHARGE_TIMEOUT,
    PMU_EVENT_CHARGE_STARTED,
    PMU_EVENT_CHARGE_FINISH,
    PMU_EVENT_BAT_FET_OVER_CURRENT,
} PMUEventType_t;

typedef enum SensorEventType {
    SENSOR_EVENT_NONE,
    SENSOR_EVENT_INTERRUPT,
    SENSOR_STEPS_UPDATED,
    SENSOR_ACTIVITY_DETECTED,
    SENSOR_TILT_DETECTED,
    SENSOR_DOUBLE_TAP_DETECTED,
    SENSOR_ANY_MOTION_DETECTED
} SensorEventType_t;


typedef enum {
    NONE_EVENT,
    POWER_EVENT,
    RTC_EVENT_INTERRUPT,
    SENSOR_EVENT,
    BUTTON_EVENT,
    TRACKBALL_EVENT,
    SDCARD_EVENT,
    ALL_EVENT_MAX,
} DeviceEvent_t;




using DeviceEventCb_t = void (*)(DeviceEvent_t event, void *params, void * user_data);

typedef struct DeviceEventCbList {
    DeviceEventCb_t cb;
    DeviceEvent_t event;
    void *user_data;
    DeviceEventCbList() :  cb(NULL), event(NONE_EVENT), user_data(NULL) {}
} DeviceEventCbList_t;

class LilyGoEventManage
{
private:
    std::vector < DeviceEventCbList_t > cbEventList;
public:
    LilyGoEventManage()
    {
    }
    ~LilyGoEventManage()
    {
    }

    uint32_t findEvent(DeviceEventCb_t cbEvent, DeviceEvent_t event)
    {
        uint32_t i;

        if (!cbEvent) {
            return cbEventList.size();
        }
        for (i = 0; i < cbEventList.size(); i++) {
            DeviceEventCbList_t entry = cbEventList[i];
            if (entry.cb == cbEvent && entry.event == event) {
                break;
            }
        }
        return i;
    }

    void onEvent(DeviceEventCb_t cbEvent, void*user_data = NULL, DeviceEvent_t event = ALL_EVENT_MAX)
    {
        onEvent(cbEvent, event, user_data);
    }

    void onEvent(DeviceEventCb_t cbEvent, DeviceEvent_t event, void*user_data)
    {
        if (!cbEvent) {
            return;
        }
        if (findEvent(cbEvent, event) < cbEventList.size()) {
            log_e("Attempt to add duplicate event handler!");
            return;
        }
        DeviceEventCbList_t newEventHandler;
        newEventHandler.cb = cbEvent;
        newEventHandler.user_data = user_data;
        newEventHandler.event = event;
        cbEventList.push_back(newEventHandler);
    }

    void removeEvent(DeviceEventCb_t cbEvent, DeviceEvent_t event)
    {
        uint32_t i;
        if (!cbEvent) {
            return;
        }
        i = findEvent(cbEvent, event);
        if (i >= cbEventList.size()) {
            return;
        }
        cbEventList.erase(cbEventList.begin() + i);
    }

    void sendEvent(DeviceEvent_t event, void * params = NULL)
    {
        for (uint32_t i = 0; i < cbEventList.size(); i++) {
            DeviceEventCbList_t entry = cbEventList[i];
            if (entry.cb && entry.event == event || entry.event == ALL_EVENT_MAX) {
                entry.cb(event, params, entry.user_data);
            }
        }
    }

    PMUEventType_t getPMUEventType(void *params)
    {
        if (!params) {
            return PMU_EVENT_NONE;
        }
        return *(static_cast < PMUEventType_t* > (params));
    }

    SensorEventType_t getSensorEventType(void *params)
    {
        if (!params) {
            return SENSOR_EVENT_NONE;
        }
        return *(static_cast < SensorEventType_t* > (params));
    }

    SDEvent_t getSDEventType(void *params)
    {
        if (!params) {
            return SDCARD_EVENT_NONE;
        }
        return *(static_cast < SDEvent_t* > (params));
    }

    ButtonEvent_t getButtonEventType(void *params)
    {
        if (!params) {
            return BUTTON_EVENT_NONE;
        }
        return *(static_cast < ButtonEvent_t * > (params));
    }

    TrackballDir_t getTrackballDirType(void *params)
    {
        if (!params) {
            return TRACKBALL_DIR_NONE;
        }
        TrackballDir_t tp = *(static_cast < TrackballDir_t * > (params));
        if (tp >= TRACKBALL_DIR_NONE && tp <= TRACKBALL_DIR_RIGHT) {
            return tp;
        }
        return TRACKBALL_DIR_NONE;
    }
};

