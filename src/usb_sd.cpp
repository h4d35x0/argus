#include "usb_sd.h"
#include <Arduino.h>
#include <SD.h>
#include <LilyGoLib.h>
#include "USB.h"
#include "USBMSC.h"

#if !CONFIG_TINYUSB_MSC_ENABLED
#error "TinyUSB MSC is not enabled in this build - USB SD cannot be compiled."
#endif

// Constructed at static-init time: the USBMSC constructor registers the MSC
// interface with TinyUSB, which must happen before USB.begin() builds the
// composite device descriptor.
static USBMSC           s_msc;
static bool             s_inited  = false;
static bool             s_running = false;
static volatile uint32_t s_last_io_ms = 0;

// MSC read/write callbacks run in the TinyUSB task. TinyUSB hands us an LBA
// that is already block-aligned and a buffer holding a whole number of
// sectors (offset is 0 for normal block transfers). SD.readRAW/writeRAW move
// exactly one 512-byte sector per call, so we loop across the buffer.

static int32_t on_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    uint32_t sec = SD.sectorSize();
    if (sec == 0) return -1;
    uint32_t start = lba + offset / sec;
    uint32_t count = bufsize / sec;
    uint8_t *out   = (uint8_t *)buffer;
    for (uint32_t i = 0; i < count; i++) {
        if (!SD.readRAW(out + i * sec, start + i)) return -1;
    }
    s_last_io_ms = millis();
    return (int32_t)(count * sec);
}

static int32_t on_write(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    uint32_t sec = SD.sectorSize();
    if (sec == 0) return -1;
    uint32_t start = lba + offset / sec;
    uint32_t count = bufsize / sec;
    for (uint32_t i = 0; i < count; i++) {
        if (!SD.writeRAW(buffer + i * sec, start + i)) return -1;
    }
    s_last_io_ms = millis();
    return (int32_t)(count * sec);
}

// Host issued a START STOP UNIT command — typically an OS "eject". Drop media
// presence so the watch UI reflects that the host has let go of the card.
static bool on_start_stop(uint8_t power_condition, bool start, bool load_eject)
{
    (void)power_condition;
    if (load_eject && !start) {
        s_msc.mediaPresent(false);
        s_running = false;
    }
    return true;
}

void usb_sd_init()
{
    if (s_inited) return;

    s_msc.vendorID("LilyGo");
    s_msc.productID("T-Watch SD");
    s_msc.productRevision("1.0");
    s_msc.onRead(on_read);
    s_msc.onWrite(on_write);
    s_msc.onStartStop(on_start_stop);
    s_msc.mediaPresent(false);          // start hidden — usb_sd_start() reveals it

    // block_count/block_size are re-read by usb_sd_start(); seeding them here
    // is harmless even if no card is present yet.
    s_msc.begin(SD.numSectors(), SD.sectorSize());

    // Start the TinyUSB stack. The MSC and CDC interfaces were both registered
    // at static-init time, so this builds a composite CDC+MSC device. begin()
    // is guarded internally, so a second call is a no-op.
    USB.begin();

    s_inited = true;
}

bool usb_sd_start()
{
    if (!s_inited) return false;
    if (s_running) return true;

    // Re-read geometry in case the card was inserted after boot.
    uint32_t sectors = SD.numSectors();
    uint16_t secsize = (uint16_t)SD.sectorSize();
    if (!instance.isCardReady() || sectors == 0 || secsize == 0)
        return false;

    s_msc.begin(sectors, secsize);
    s_msc.mediaPresent(true);           // host now sees the card "inserted"
    s_running    = true;
    s_last_io_ms = 0;
    return true;
}

void usb_sd_stop()
{
    if (!s_running) return;
    s_msc.mediaPresent(false);          // host sees the card "removed"
    s_running = false;
}

bool usb_sd_is_running() { return s_running; }

bool usb_sd_host_active()
{
    return s_running && s_last_io_ms != 0 && (millis() - s_last_io_ms) < 2000;
}

uint64_t usb_sd_card_bytes()
{
    return (uint64_t)SD.numSectors() * SD.sectorSize();
}
