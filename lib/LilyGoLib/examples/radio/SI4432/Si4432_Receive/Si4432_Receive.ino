/*
  RadioLib Si443x Receive with Interrupts Example

  This example listens for FSK transmissions and tries to
  receive them. Once a packet is received, an interrupt is
  triggered.

  Other modules from Si443x/RFM2x family can also be used.

  For default module settings, see the wiki page
  https://github.com/jgromes/RadioLib/wiki/Default-configuration#si443xrfm2x

  For full API reference, see the GitHub Pages
  https://jgromes.github.io/RadioLib/
*/

#include <LilyGoLib.h>
#include <LV_Helper.h>

lv_obj_t *label;

// flag to indicate that a packet was received
volatile bool receivedFlag = false;

// this function is called when a complete packet
// is received by the module
// IMPORTANT: this function MUST be 'void' type
//            and MUST NOT have any arguments!
ICACHE_RAM_ATTR void setFlag(void)
{
    // we got a packet, set the flag
    receivedFlag = true;
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

    const char *example_title = "SI4432 Receive Example";
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
    // when new packet is received
    radio.setPacketReceivedAction(setFlag);

    // start listening for packets
    Serial.print(F("[Si4432] Starting to listen ... "));
    int state = radio.startReceive();
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("success!"));
    } else {
        Serial.print(F("failed, code "));
        Serial.println(state);
        while (true) {
            delay(10);
        }
    }

    // if needed, 'listen' mode can be disabled by calling
    // any of the following methods:
    //
    // radio.standby()
    // radio.sleep()
    // radio.transmit();
    // radio.receive();
    // radio.readData();


}

void loop()
{
    // check if the flag is set
    if (receivedFlag) {
        // reset flag
        receivedFlag = false;

        // you can read received data as an Arduino String
        String str;
        int state = radio.readData(str);

        // you can also read received data as byte array
        /*
          byte byteArr[8];
          int numBytes = radio.getPacketLength();
          int state = radio.readData(byteArr, numBytes);
        */

        if (state == RADIOLIB_ERR_NONE) {
            // packet was successfully received
            Serial.println(F("[Si4432] Received packet!"));

            // print data of the packet
            Serial.print(F("[Si4432] Data:\t\t\t"));
            Serial.println(str);

            lv_label_set_text_fmt(label, "Receive:\n%s", str.c_str());

        } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
            // packet was received, but is malformed
            Serial.println(F("CRC error!"));

        } else {
            // some other error occurred
            Serial.print(F("failed, code "));
            Serial.println(state);

        }

        // put module back to listen mode
        radio.startReceive();
    }

    lv_timer_handler();

}
