/*
  RadioLib nRF24 Transmit with Interrupts Example

  This example transmits packets using nRF24 2.4 GHz radio module.
  Each packet contains up to 32 bytes of data, in the form of:
  - Arduino String
  - null-terminated char array (C-string)
  - arbitrary binary data (byte array)

  Packet delivery is automatically acknowledged by the receiver.

  For default module settings, see the wiki page
  https://github.com/jgromes/RadioLib/wiki/Default-configuration#nrf24

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

ICACHE_RAM_ATTR void setFlag(void)
{
    // we sent a packet, set the flag
    transmittedFlag = true;
}

lv_obj_t *btn1;
lv_obj_t *btn2, *btn3, *btn4;
lv_obj_t *status_label;

enum nRF24_State {
    NRF24_STOP,
    NRF24_CW_RUNNING,
    NRF24_PKG_RECV,
    NRF24_PKG_SENDER,
};

nRF24_State STATE;
// counter to keep track of transmitted packets
int count = 0;
uint32_t interval = 0;
uint32_t timeout = 0;

static void enableCw(lv_event_t *e)
{
    lv_label_set_text(status_label, "STATE:Running");
    instance.io.digitalWrite(EXPANDS_GPIO_EN, HIGH);
    nrf24.transmitDirect();
    STATE = NRF24_CW_RUNNING;
}

static void disableCw(lv_event_t *e)
{
    lv_label_set_text(status_label, "STATE:Stop");
    instance.io.digitalWrite(EXPANDS_GPIO_EN, LOW);
    nrf24.standby();
    STATE = NRF24_STOP;
}

static void enableRecv(lv_event_t *e)
{
    lv_label_set_text(status_label, "STATE:Receive");
    instance.io.digitalWrite(EXPANDS_GPIO_EN, LOW);
    nrf24.startReceive();
    STATE = NRF24_PKG_RECV;
    timeout = millis();
}

static void enableTransmit(lv_event_t *e)
{
    lv_label_set_text(status_label, "STATE:Transmit");
    instance.io.digitalWrite(EXPANDS_GPIO_EN, HIGH);
    nrf24.startTransmit("123456");
    STATE = NRF24_PKG_SENDER;
    timeout = millis();
}

void setup()
{
    Serial.begin(115200);

    instance.begin();

    beginLvglHelper(instance);

    // Create intput device group , only T-LoRa-Pager need.
    lv_group_t *group = lv_group_create();
    lv_set_default_group(group);

    const char *example_title = "nRF24_Transmit";
    label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_label_set_text(label, example_title);
    lv_obj_center(label);
    lv_timer_handler();

    // Set brightness to MAX
    // T-LoRa-Pager brightness level is 0 ~ 16
    // T-Watch-S3 , T-Watch-S3-Plus , T-Watch-Ultra brightness level is 0 ~ 255
    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);

    // initialize nRF24
    int16_t freq = (int16_t)2400;
    int16_t dr = (int16_t)1000;
    int8_t pwr = (int8_t)(0);
    uint8_t addrWidth = (uint8_t)5U;
    Serial.print(F("[nRF24] Initializing ... "));
    int state = nrf24.begin(freq, dr, pwr, addrWidth);
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("success!"));
    } else {
        Serial.print(F("failed, code "));
        Serial.println(state);
        lv_label_set_text_fmt(label, "nrf24 init failed : %d", state);
        while (true) {
            lv_timer_handler();
            delay(10);
        }
    }

    // Set PA control IO to output function
    instance.io.pinMode(EXPANDS_GPIO_EN, OUTPUT);

    // set transmit address
    // NOTE: address width in bytes MUST be equal to the
    //       width set in begin() or setAddressWidth()
    //       methods (5 by default)
    byte addr[] = {0x01, 0x23, 0x45, 0x67, 0x89};
    Serial.print(F("[nRF24] Setting transmit pipe ... "));
    state = nrf24.setTransmitPipe(addr);
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("success!"));
    } else {
        Serial.print(F("failed, code "));
        Serial.println(state);
        while (true) {
            delay(10);
        }
    }

    // set the function that will be called
    // when packet transmission is finished
    nrf24.setPacketSentAction(setFlag);

    status_label = lv_label_create(lv_scr_act());
    lv_label_set_text(status_label, "STATE:STOP");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 10);

    btn1 = lv_btn_create(lv_scr_act());
    lv_obj_add_event_cb(btn1, enableCw, LV_EVENT_CLICKED, NULL);
    lv_obj_set_width(btn1,  LV_PCT(80));
    lv_obj_align_to(btn1, status_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_group_add_obj(lv_group_get_default(), btn1);

    lv_obj_t *label;
    label = lv_label_create(btn1);
    lv_label_set_text(label, "Enable");
    lv_obj_center(label);

    btn2 = lv_btn_create(lv_scr_act());
    lv_obj_add_event_cb(btn2, disableCw, LV_EVENT_CLICKED, NULL);
    lv_obj_set_width(btn2,  LV_PCT(80));
    lv_obj_align_to(btn2, btn1, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_group_add_obj(lv_group_get_default(), btn2);

    label = lv_label_create(btn2);
    lv_label_set_text(label, "Disable");
    lv_obj_center(label);

    btn3 = lv_btn_create(lv_scr_act());
    lv_obj_add_event_cb(btn3, enableRecv, LV_EVENT_CLICKED, NULL);
    lv_obj_set_width(btn3,  LV_PCT(80));
    lv_obj_align_to(btn3, btn2, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_group_add_obj(lv_group_get_default(), btn3);

    label = lv_label_create(btn3);
    lv_label_set_text(label, "Receive");
    lv_obj_center(label);


    btn4 = lv_btn_create(lv_scr_act());
    lv_obj_add_event_cb(btn4, enableTransmit, LV_EVENT_CLICKED, NULL);
    lv_obj_set_width(btn4,  LV_PCT(80));
    lv_obj_align_to(btn4, btn3, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_group_add_obj(lv_group_get_default(), btn4);

    label = lv_label_create(btn4);
    lv_label_set_text(label, "Transmit");
    lv_obj_center(label);

}



void loop()
{
    instance.loop();

    lv_task_handler();

    if (millis() < interval) {
        return;
    }

    Serial.printf("STATE:%d\n", STATE);

    switch (STATE) {
    case NRF24_PKG_SENDER:
        // check if the previous transmission finished
        if (transmittedFlag || ((millis() - timeout) > 2000)) {
            // reset flag
            transmittedFlag = false;

            if (transmissionState == RADIOLIB_ERR_NONE) {
                // packet was successfully sent
                Serial.println(F("transmission finished!"));

                // NOTE: when using interrupt-driven transmit method,
                //       it is not possible to automatically measure
                //       transmission data rate using getDataRate()

            } else {
                Serial.print(F("failed, code "));
                Serial.println(transmissionState);
                lv_label_set_text_fmt(label, "nrf24 transmission failed : %d", transmissionState);
            }

            // clean up after transmission is finished
            // this will ensure transmitter is disabled,
            // RF switch is powered down etc.
            nrf24.finishTransmit();

            // wait a second before transmitting again

            // send another one
            Serial.print(F("[nRF24] Sending another packet ... "));

            // you can transmit C-string or Arduino string up to
            // 32 characters long
            String str = "Hello World! #" + String(count++);
            transmissionState = nrf24.startTransmit(str);

            lv_label_set_text_fmt(label, "nrf24 transmission\nPayload:%s", str.c_str());


            // you can also transmit byte array up to 256 bytes long
            /*
              byte byteArr[] = {0x01, 0x23, 0x45, 0x67,
                                0x89, 0xAB, 0xCD, 0xEF};
              int state = nrf24.startTransmit(byteArr, 8);
            */
            timeout = millis();
        }
        break;

    case NRF24_PKG_RECV:
        // check if the flag is set
        if (transmittedFlag || ((millis() - timeout) > 2000)) {
            // reset flag
            transmittedFlag = false;

            // you can read received data as an Arduino String
            String str;
            int state = nrf24.readData(str);

            // you can also read received data as byte array
            /*
              byte byteArr[8];
              int numBytes = nrf24.getPacketLength();
              int state = nrf24.readData(byteArr, numBytes);
            */

            if (state == RADIOLIB_ERR_NONE) {
                // packet was successfully received
                Serial.println(F("[nRF24] Received packet!"));

                // print data of the packet
                Serial.print(F("[nRF24] Data:\t\t"));
                Serial.println(str);

                lv_label_set_text_fmt(label, "Received:\nData:%s", str.c_str());

            } else {
                // some other error occurred
                Serial.print(F("[nRF24] Failed, code "));
                Serial.println(state);

                lv_label_set_text_fmt(label, "Receive failed:%d", state);
            }

            // put module back to listen mode
            nrf24.startReceive();
            timeout = millis();
        }
        break;
    default:
        break;
    }

    interval = millis() + 1000;

}
