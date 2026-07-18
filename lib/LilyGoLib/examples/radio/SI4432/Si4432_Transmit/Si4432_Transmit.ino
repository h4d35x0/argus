/*
  RadioLib Si443x Transmit with Interrupts Example

  This example transmits packets using Si4432 FSK radio module.
  Each packet contains up to 64 bytes of data, in the form of:
  - Arduino String
  - null-terminated char array (C-string)
  - arbitrary binary data (byte array)

  Other modules from Si443x/RFM2x family can also be used.

  For default module settings, see the wiki page
  https://github.com/jgromes/RadioLib/wiki/Default-configuration#si443xrfm2x

  For full API reference, see the GitHub Pages
  https://jgromes.github.io/RadioLib/
*/

#include <LilyGoLib.h>
#include <LV_Helper.h>

lv_obj_t *label;

// save transmission state between loops
int transmissionState = RADIOLIB_ERR_NONE;

// flag to indicate that a packet was sent
volatile bool transmittedFlag = false;

// this function is called when a complete packet
// is transmitted by the module
// IMPORTANT: this function MUST be 'void' type
//            and MUST NOT have any arguments!
ICACHE_RAM_ATTR void setFlag(void)
{
    // we sent a packet, set the flag
    transmittedFlag = true;
}


void settingLoRaParams()
{
    // set carrier frequency to 433.5 MHz
    if (radio.setFrequency(433.5) == RADIOLIB_ERR_INVALID_FREQUENCY) {
        Serial.println(F("[Si4432] Selected frequency is invalid for this module!"));
        while (true) {
            delay(10);
        }
    }

    // set bit rate to 100.0 kbps
    int state = radio.setBitRate(100.0);
    if (state == RADIOLIB_ERR_INVALID_BIT_RATE) {
        Serial.println(F("[Si4432] Selected bit rate is invalid for this module!"));
        while (true) {
            delay(10);
        }
    } else if (state == RADIOLIB_ERR_INVALID_BIT_RATE_BW_RATIO) {
        Serial.println(F("[Si4432] Selected bit rate to bandwidth ratio is invalid!"));
        Serial.println(F("[Si4432] Increase receiver bandwidth to set this bit rate."));
        while (true) {
            delay(10);
        }
    }

    // set receiver bandwidth to 284.8 kHz
    state = radio.setRxBandwidth(284.8);
    if (state == RADIOLIB_ERR_INVALID_RX_BANDWIDTH) {
        Serial.println(F("[Si4432] Selected receiver bandwidth is invalid for this module!"));
        while (true) {
            delay(10);
        }
    }

    // set frequency deviation to 10.0 kHz
    if (radio.setFrequencyDeviation(10.0) == RADIOLIB_ERR_INVALID_FREQUENCY_DEVIATION) {
        Serial.println(F("[Si4432] Selected frequency deviation is invalid for this module!"));
        while (true) {
            delay(10);
        }
    }

    // set output power to 20 dBm
    if (radio.setOutputPower(20) == RADIOLIB_ERR_INVALID_OUTPUT_POWER) {
        Serial.println(F("[Si4432] Selected output power is invalid for this module!"));
        while (true) {
            delay(10);
        }
    }

    // up to 4 bytes can be set as sync word
    // set sync word to 0x01234567
    uint8_t syncWord[] = {0x01, 0x23, 0x45, 0x67};
    if (radio.setSyncWord(syncWord, 4) == RADIOLIB_ERR_INVALID_SYNC_WORD) {
        Serial.println(F("[Si4432] Selected sync word is invalid for this module!"));
        while (true) {
            delay(10);
        }
    }

    Serial.println(F("[Si4432] All settings changed successfully!"));
}


void setup()
{
    Serial.begin(115200);

    instance.begin();

    beginLvglHelper(instance);

    const char *example_title = "SI4432 Transmit Example";
    label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_label_set_text(label, example_title);
    lv_obj_center(label);
    lv_timer_handler();

    // Set brightness to MAX
    // T-LoRa-Pager brightness level is 0 ~ 16
    // T-Watch-S3 , T-Watch-S3-Plus , T-Watch-Ultra brightness level is 0 ~ 255
    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);


    settingLoRaParams();

    // set the function that will be called
    // when packet transmission is finished
    radio.setPacketSentAction(setFlag);

    // start transmitting the first packet
    Serial.print(F("[Si4432] Sending first packet ... "));

    // you can transmit C-string or Arduino string up to
    // 64 characters long
    transmissionState = radio.startTransmit("Hello World!");

    // you can also transmit byte array up to 64 bytes long
    /*
      byte byteArr[] = {0x01, 0x23, 0x45, 0x67,
                        0x89, 0xAB, 0xCD, 0xEF};
      state = radio.startTransmit(byteArr, 8);
    */
}

// counter to keep track of transmitted packets
int count = 0;

void loop()
{
    // check if the previous transmission finished
    if (transmittedFlag) {
        // reset flag
        transmittedFlag = false;

        if (transmissionState == RADIOLIB_ERR_NONE) {
            // packet was successfully sent
            Serial.println(F("transmission finished!"));

        } else {
            Serial.print(F("failed, code "));
            Serial.println(transmissionState);

        }

        // clean up after transmission is finished
        // this will ensure transmitter is disabled,
        // RF switch is powered down etc.
        radio.finishTransmit();

        // wait a second before transmitting again
        delay(1000);

        // send another one
        Serial.print(F("[Si4432] Sending another packet ... "));

        // you can transmit C-string or Arduino string up to
        // 256 characters long
        String str = "Hello World! #" + String(count++);
        transmissionState = radio.startTransmit(str);

        lv_label_set_text_fmt(label, "Sending packet:\n%s", str.c_str());

        // you can also transmit byte array up to 64 bytes long
        /*
          byte byteArr[] = {0x01, 0x23, 0x45, 0x67,
                            0x89, 0xAB, 0xCD, 0xEF};
          transmissionState = radio.startTransmit(byteArr, 8);
        */
    }

    lv_timer_handler();
}
