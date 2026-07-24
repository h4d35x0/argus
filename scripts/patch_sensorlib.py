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
