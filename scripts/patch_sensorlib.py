from pathlib import Path

Import("env")


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise RuntimeError(f"SensorLib patch context not found in {path}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


sensorlib = (
    Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    / env.subst("$PIOENV")
    / "SensorLib"
    / "src"
)

header = sensorlib / "SensorBHI260AP.hpp"
bridge = sensorlib / "bosch" / "BoschParseStatic.cpp"
callback_manager = sensorlib / "bosch" / "BoschParseCallbackManager.hpp"

replace_once(
    header,
    """class SensorBHI260AP : public BoschVirtualSensor, BoschParseBase
{
public:
""",
    """class SensorBHI260AP : public BoschVirtualSensor, BoschParseBase
{
    // ARGUS: allow the C callback bridge to make qualified, non-virtual calls.
    // Exact core dumps showed the object's virtual target intermittently
    // corrupted during the first BHI260 FIFO callback after boot.
    friend class BoschParseStatic;

public:
""",
)

replace_once(
    bridge,
    """#include "BoschParseStatic.hpp"
""",
    """#include "BoschParseStatic.hpp"
#include "../SensorBHI260AP.hpp"
""",
)

replace_once(
    bridge,
    """    BoschParseBase *sensor = static_cast<BoschParseBase *>(user_data);
    if (sensor) {
        sensor->parseData(fifo, user_data);
    }
""",
    """    SensorBHI260AP *sensor = static_cast<SensorBHI260AP *>(user_data);
    if (sensor) {
        sensor->SensorBHI260AP::parseData(fifo, user_data);
    }
""",
)

# SensorLib's GCC < 10 path uses a custom callback vector. Its call() method
# names the FIFO payload length "size", shadowing the member "size" that tracks
# the number of registered callbacks. The loop therefore walks payload-length
# entries beyond the initialized vector and can call an uninitialized function
# pointer. A matching core dump caught that callx8 jumping to 0x3225A54A.
replace_once(
    callback_manager,
    """    void call(uint8_t sensor_id, uint8_t *data, uint32_t size, uint64_t *timestamp)
    {
#ifdef USE_CUSTOM_VECTOR
        for (uint32_t i = 0; i < size; i++) {
            if (entries[i].cb) {
                if (entries[i].id == sensor_id) {
                    entries[i].cb(sensor_id, data, size, timestamp, entries[i].user_data);
                }
            }
        }
#else
        for (uint32_t i = 0; i < entries.size(); i++) {
            if (entries[i].cb) {
                if (entries[i].id == sensor_id) {
                    entries[i].cb(sensor_id, data, size, timestamp, entries[i].user_data);
                }
            }
        }
#endif
    }
""",
    """    void call(uint8_t sensor_id, uint8_t *data, uint32_t data_size, uint64_t *timestamp)
    {
#ifdef USE_CUSTOM_VECTOR
        for (uint32_t i = 0; i < this->size; i++) {
            if (entries[i].cb) {
                if (entries[i].id == sensor_id) {
                    entries[i].cb(sensor_id, data, data_size, timestamp, entries[i].user_data);
                }
            }
        }
#else
        for (uint32_t i = 0; i < entries.size(); i++) {
            if (entries[i].cb) {
                if (entries[i].id == sensor_id) {
                    entries[i].cb(sensor_id, data, data_size, timestamp, entries[i].user_data);
                }
            }
        }
#endif
    }
""",
)

replace_once(
    bridge,
    """    BoschParseBase *sensor = static_cast<BoschParseBase *>(user_data);
    if (sensor) {
        sensor->parseMetaEvent(callback_info, user_data);
    }
""",
    """    SensorBHI260AP *sensor = static_cast<SensorBHI260AP *>(user_data);
    if (sensor) {
        sensor->SensorBHI260AP::parseMetaEvent(callback_info, user_data);
    }
""",
)

replace_once(
    bridge,
    """    BoschParseBase *sensor = static_cast<BoschParseBase *>(user_data);
    if (sensor) {
        sensor->parseDebugMessage(callback_info, user_data);
    }
""",
    """    SensorBHI260AP *sensor = static_cast<SensorBHI260AP *>(user_data);
    if (sensor) {
        sensor->SensorBHI260AP::parseDebugMessage(callback_info, user_data);
    }
""",
)
