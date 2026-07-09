
#define TINY_GSM_MODEM_BG96

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PubSubClient.h>
#include <TinyGsmClient.h>
#include <serial_log.h>
#include "icon.h"

#define GPS_RX 5
#define GPS_TX 4
#define GPS_BAUD 115200

#define MODEM_RX 9
#define MODEM_TX 8

#define SURVEY_PIN 10
#define LED_SURVEY_IN 11
#define LED_PING 7

#define OLED_SDA 2
#define OLED_SCL 1
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C

#define SURVEY_SEC 20
#define SURVEY_ACC 4000 // svinAccLimit, đơn vị 0.1mm (4000 = 40cm)

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

HardwareSerial modem(1);
TinyGsm modemGsm(modem);
TinyGsmClient gsmClient(modemGsm);
PubSubClient mqtt(gsmClient);

HardwareSerial F9P(2);
unsigned long lastModemReset = 0;

const char *MQTT_HOST = "103.82.22.78";
const int MQTT_PORT = 1883;
const char *MQTT_CLIENT_ID = "cacao_2_rtk_base";
const char *MQTT_TOPIC = "rtk/rtcm3-2";

enum ParseState
{
    WAIT_SYNC,
    RTCM_COLLECT,
    UBX_COLLECT,
    WAIT_UBX_SYNC2,
};
ParseState parseState = WAIT_SYNC;
uint16_t expectedLen = 0;
unsigned long lastFlush = 0;

uint8_t ubxBuf[512];
uint16_t ubxIdx = 0;

enum BaseState
{
    IDLE,
    SURVEY,
    FIXED
};

BaseState baseState = IDLE;

int32_t ecefX_cm, ecefY_cm, ecefZ_cm, fixedAcc;
bool surveyValid = false;
bool surveyActive = false;
float currentAccM = 0.0f;
uint32_t currentDur = 0;

// ---------- OLED ----------

void oledDrawHeader(const char *line1, const char *line2)
{
    oled.fillRect(0, 0, OLED_WIDTH, 22, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    oled.setTextSize(1);

    int w1 = strlen(line1) * 6;
    oled.setCursor((OLED_WIDTH - w1) / 2, 2);
    oled.print(line1);

    int w2 = strlen(line2) * 6;
    oled.setCursor((OLED_WIDTH - w2) / 2, 13);
    oled.print(line2);

    oled.setTextColor(SSD1306_WHITE);
}

void updateOLED()
{
    char buf[24];
    char sub[22];
    oled.clearDisplay();
    oled.setTextSize(1);

    if (baseState == FIXED)
    {
        oledDrawHeader("FIXED", "(v)  Position locked");

        oled.setCursor(0, 28);
        snprintf(buf, sizeof(buf), "Acc : %.3f m", fixedAcc / 10000.0f);
        oled.print(buf);

        oled.setCursor(0, 40);
        snprintf(buf, sizeof(buf), "Time: %d s", SURVEY_SEC);
        oled.print(buf);

        oled.drawFastHLine(0, 52, OLED_WIDTH, SSD1306_WHITE);
        oled.setCursor(0, 55);
        oled.print(">> Sending RTCM3");
    }
    else if (baseState == SURVEY)
    {
        snprintf(sub, sizeof(sub), "(*) %lu s / %d s", currentDur, SURVEY_SEC);
        oledDrawHeader("SURVEY-IN", sub);

        oled.setCursor(0, 28);
        snprintf(buf, sizeof(buf), "Acc : %.3f m", currentAccM);
        oled.print(buf);

        oled.setCursor(0, 40);
        snprintf(buf, sizeof(buf), "Tgt : %.3f m", SURVEY_ACC / 10000.0f);
        oled.print(buf);
    }
    else
    {
        oledDrawHeader("BASE RTK STATION", "MODE: IDLE");

        oled.setCursor(0, 32);
        oled.print("Waiting RTK...");
    }

    oled.display();
}

// ---------- OLED connection screens ----------

void oledConnecting(const char *title, const char *detail)
{
    char line1[22];
    snprintf(line1, sizeof(line1), "(~) %s", title);

    oled.clearDisplay();
    oledDrawHeader(line1, "Connecting...");

    if (detail && strlen(detail) > 0)
    {
        oled.setCursor(0, 28);
        oled.print(detail);
    }

    oled.setCursor(0, 52);
    oled.print("Please wait...");

    oled.display();
}

void oledConnected(const char *title, bool ok, const char *detail1, const char *detail2)
{
    char line1[22];
    snprintf(line1, sizeof(line1), "%s %s", ok ? "[OK]" : "[!!]", title);

    oled.clearDisplay();
    oledDrawHeader(line1, ok ? "Connected" : "Failed");

    if (detail1 && strlen(detail1) > 0)
    {
        oled.setCursor(0, 28);
        oled.print(detail1);
    }

    if (detail2 && strlen(detail2) > 0)
    {
        oled.setCursor(0, 40);
        oled.print(detail2);
    }

    oled.drawFastHLine(0, 52, OLED_WIDTH, SSD1306_WHITE);
    oled.setCursor(0, 55);
    oled.print(ok ? ">> Ready" : ">> Check hardware");

    oled.display();
    delay(1500);
}

// ---------- GSM / MQTT ----------

void gsmConnect()
{
    oledConnecting("4G NETWORK", "APN: v-internet");
    SerialLog::log("gsm connecting...");

    modem.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(2000);
    modemGsm.restart();
    modemGsm.gprsConnect("v-internet", "", "");

    bool ok = modemGsm.isNetworkConnected();
    SerialLog::log("gsm connected", ok);
    oledConnected("4G NETWORK", ok, "APN: v-internet", nullptr);
}

void resetModem()
{
    modemGsm.restart();
    modemGsm.gprsConnect("v-internet", "", "");
    lastModemReset = millis();
}

void mqttConnect()
{
    oledConnecting("MQTT SERVER", MQTT_HOST);
    SerialLog::log("MQTT connecting...");

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(1024);
    while (!mqtt.connected())
    {
        mqtt.connect(MQTT_CLIENT_ID);
        delay(1000);
    }

    SerialLog::log("MQTT connected");
    oledConnected("MQTT SERVER", true, MQTT_HOST, MQTT_CLIENT_ID);
}

void mqttReconnect()
{
    SerialLog::log("GPRS connected:", modemGsm.isGprsConnected());
    SerialLog::log("Attempting MQTT connection...");
    int retry = 1;
    while (!mqtt.connected())
    {
        if (!mqtt.connect(MQTT_CLIENT_ID))
        {
            SerialLog::log("MQTT failed, state:", mqtt.state(), "retry:", retry);
            retry++;
            delay(1000);
        }
    }
}

// ---------- UBX ----------

void ubxChecksum(const uint8_t *data, uint16_t len, uint8_t &ckA, uint8_t &ckB)
{
    for (uint16_t i = 0; i < len; i++)
    {
        ckA += data[i];
        ckB += ckA;
    }
}

void sendUBX(uint8_t cls, uint8_t id, uint8_t *payload, uint16_t payloadLen)
{
    uint8_t header[6] = {
        0xB5, 0x62,
        cls, id,
        (uint8_t)(payloadLen & 0xFF),
        (uint8_t)(payloadLen >> 8)};

    uint8_t ckA = 0, ckB = 0;
    ubxChecksum(&header[2], 4, ckA, ckB);
    if (payloadLen > 0)
        ubxChecksum(payload, payloadLen, ckA, ckB);

    F9P.write(header, sizeof(header));
    if (payloadLen > 0)
        F9P.write(payload, payloadLen);
    F9P.write(ckA);
    F9P.write(ckB);
    F9P.flush();
}

void ubxUart1Enable(bool isOn)
{
    if (isOn)
    {
        uint8_t msg[] = {
            0xB5, 0x62, 0x06, 0x8A, 0x0E, 0x00,
            0x00, 0x01, 0x00, 0x00, 0x01, 0x00,
            0x74, 0x10, 0x01, 0x04, 0x00, 0x74,
            0x10, 0x00, 0xAD, 0x33};
        F9P.write(msg, sizeof(msg)); // fix: thiếu dòng này trong bản cũ
    }
    else
    {
        uint8_t msg[] = {
            0xB5, 0x62, 0x06, 0x8A, 0x0E, 0x00,
            0x00, 0x01, 0x00, 0x00, 0x01, 0x00,
            0x74, 0x10, 0x00, 0x04, 0x00, 0x74,
            0x10, 0x01, 0xAD, 0x2E};
        F9P.write(msg, sizeof(msg));
    }
}

void setBaseState(BaseState state)
{
    baseState = state;
    bool isSurvey = state == SURVEY;
    ubxUart1Enable(isSurvey);
    digitalWrite(LED_SURVEY_IN, isSurvey);
    updateOLED();
}

void switchToFixed()
{
    int32_t x = ecefX_cm;
    int32_t y = ecefY_cm;
    int32_t z = ecefZ_cm;

    uint8_t msg[128];
    uint8_t *p = msg;

    *p++ = 0x00;
    *p++ = 0x05;
    *p++ = 0x00;
    *p++ = 0x00;

    auto addU1 = [&](uint32_t key, uint8_t v)
    {
        memcpy(p, &key, 4);
        p += 4;
        *p++ = v;
    };

    auto addI4 = [&](uint32_t key, int32_t v)
    {
        memcpy(p, &key, 4);
        p += 4;
        memcpy(p, &v, 4);
        p += 4;
    };

    auto addU4 = [&](uint32_t key, uint32_t v)
    {
        memcpy(p, &key, 4);
        p += 4;
        memcpy(p, &v, 4);
        p += 4;
    };

    addU1(0x20030001, 2); // CFG-TMODE-MODE = FIXED
    addI4(0x40030003, x); // ECEF X
    addI4(0x40030004, y); // ECEF Y
    addI4(0x40030005, z); // ECEF Z
    addU4(0x4003000F, fixedAcc);
    addU4(0x40030010, 0);
    addU4(0x40030011, 0);

    uint16_t len = p - msg;
    sendUBX(0x06, 0x8A, msg, len);

    setBaseState(FIXED);
}

void switchToSurveyIn()
{
    uint8_t msg[128];
    uint8_t *p = msg;

    *p++ = 0x00;
    *p++ = 0x05;
    *p++ = 0x00;
    *p++ = 0x00;

    auto addU1 = [&](uint32_t key, uint8_t v)
    {
        memcpy(p, &key, 4);
        p += 4;
        *p++ = v;
    };

    auto addU4 = [&](uint32_t key, uint32_t v)
    {
        memcpy(p, &key, 4);
        p += 4;
        memcpy(p, &v, 4);
        p += 4;
    };

    addU1(0x20030001, 1); // CFG-TMODE-MODE = Survey-In
    addU4(0x40030003, 0);
    addU4(0x40030004, 0);
    addU4(0x40030005, 0);
    addU4(0x4003000F, 0);
    addU4(0x40030010, SURVEY_SEC); // svinMinDur
    addU4(0x40030011, SURVEY_ACC); // svinAccLimit

    uint16_t len = p - msg;
    sendUBX(0x06, 0x8A, msg, len);

    setBaseState(SURVEY);
}

void parseUBX(uint8_t *buf, uint16_t len)
{
    SerialLog::log("UBX", len);
    if (len < 8)
        return;

    if (buf[0] != 0xB5 || buf[1] != 0x62)
        return;

    uint8_t cls = buf[2];
    uint8_t id = buf[3];
    uint16_t payloadLen = buf[4] | (buf[5] << 8);
    const uint8_t *p = buf + 6;

    if (cls == 0x06 && id == 0x71 && payloadLen >= 40)
    {
        uint8_t tmodeMode = p[2];
        if (tmodeMode == 1)
            setBaseState(SURVEY);
        else if (tmodeMode == 2)
            setBaseState(FIXED);
        else
            switchToSurveyIn();
        return;
    }

    // UBX-NAV-SVIN
    if (cls == 0x01 && id == 0x3B && payloadLen >= 40)
    {
        uint32_t dur = *(uint32_t *)(p + 8);  // s
        int32_t meanX = *(int32_t *)(p + 12); // cm
        int32_t meanY = *(int32_t *)(p + 16); // cm
        int32_t meanZ = *(int32_t *)(p + 20); // cm
        uint32_t acc = *(uint32_t *)(p + 28); // 0.1 mm
        uint8_t valid = p[36];
        uint8_t active = p[37];

        surveyValid = valid;
        surveyActive = active;

        ecefX_cm = meanX;
        ecefY_cm = meanY;
        ecefZ_cm = meanZ;
        fixedAcc = acc;

        currentAccM = acc / 10000.0f;
        currentDur = dur;

        SerialLog::log("SVIN", ecefX_cm, ecefY_cm, ecefZ_cm, surveyValid, dur, acc);

        updateOLED();

        if (surveyValid && surveyActive == 0 && dur >= SURVEY_SEC)
        {
            switchToFixed();
        }
    }
}

// ---------- Data sync ----------

void rtcm3Sync(uint8_t *payload, unsigned int plength)
{
    digitalWrite(LED_PING, HIGH);
    mqtt.publish(MQTT_TOPIC, payload, plength);
    digitalWrite(LED_PING, LOW);
}

void ubxSync(uint8_t *payload, unsigned int plength)
{
    digitalWrite(LED_PING, HIGH);
    parseUBX(payload, plength);
    digitalWrite(LED_PING, LOW);
}

void f9pLoop()
{
    if (baseState == FIXED)
    {
        int len = F9P.available();
        if (len == 0)
            return;
        uint8_t *frame = (uint8_t *)malloc(len);
        if (!frame)
            return;
        int n = F9P.readBytes(frame, len);
        rtcm3Sync(frame, n);
        free(frame);
        return;
    }

    while (F9P.available())
    {
        uint8_t b = F9P.read();

        switch (parseState)
        {
        case WAIT_SYNC:
            if (b == 0xB5)
            {
                ubxIdx = 0;
                ubxBuf[ubxIdx++] = b;
                parseState = WAIT_UBX_SYNC2;
            }
            break;

        case WAIT_UBX_SYNC2:
            if (b == 0x62)
            {
                ubxBuf[ubxIdx++] = b;
                parseState = UBX_COLLECT;
            }
            else
            {
                parseState = WAIT_SYNC;
                ubxIdx = 0;
            }
            break;

        case UBX_COLLECT:
            if (ubxIdx < sizeof(ubxBuf))
                ubxBuf[ubxIdx++] = b;

            if (ubxIdx == 6)
                expectedLen = ubxBuf[4] | (ubxBuf[5] << 8);

            if (ubxIdx >= 8 && ubxIdx == (uint16_t)(6 + expectedLen + 2))
            {
                ubxSync(ubxBuf, ubxIdx);
                parseState = WAIT_SYNC;
                ubxIdx = 0;
            }
            else if (ubxIdx >= sizeof(ubxBuf))
            {
                parseState = WAIT_SYNC;
                ubxIdx = 0;
            }
            break;

        default:
            parseState = WAIT_SYNC;
            ubxIdx = 0;
            break;
        }
    }
}

// ---------- Command task ----------

void _cmdTask(void *pvParameters)
{
    bool surveyPinState = HIGH;
    while (1)
    {
        bool currentState = digitalRead(SURVEY_PIN);

        if (surveyPinState == LOW && currentState == HIGH)
        {
            SerialLog::log("Start Survey-In");
            switchToSurveyIn();
        }

        surveyPinState = currentState;

        if (baseState == IDLE)
        {
            static unsigned long lastToggle = 0;
            static bool ledState = false;
            unsigned long now = millis();
            if (now - lastToggle >= 1000)
            {
                lastToggle = now;
                ledState = !ledState;
                digitalWrite(LED_SURVEY_IN, ledState);
                sendUBX(0x06, 0x71, nullptr, 0); // ping CFG-TMODE3
            }
        }

#ifdef DEBUG
        {
            auto cmd = Serial.readStringUntil('\n');
            if (cmd.startsWith("FIXED"))
                switchToFixed();
            else if (cmd.startsWith("SURVEY"))
                switchToSurveyIn();
        }
        while (modem.available())
            Serial.write(modem.read());

        static bool lastState = HIGH;
        static unsigned long lastDebounceTime = 0;
        bool reading = digitalRead(0);
        if ((millis() - lastDebounceTime) > 50)
        {
            if (lastState == LOW && reading == HIGH)
                esp_restart();
        }
        if (reading != lastState)
            lastDebounceTime = millis();
        lastState = reading;
#endif
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// ---------- Setup / Loop ----------

void setup()
{
    Serial.begin(115200); // luôn bật để SerialLog hoạt động

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
    {
        SerialLog::log("OLED init failed");
    }
    else
    {
        oled.clearDisplay();
        oled.drawBitmap(
            (OLED_WIDTH - LOGO_W) / 2,
            (OLED_HEIGHT - LOGO_H) / 2,
            logo_bitmap, LOGO_W, LOGO_H,
            SSD1306_WHITE);
        oled.display();
        delay(2000);
        oled.clearDisplay();
        oled.display();
    }

    F9P.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);

    pinMode(LED_PING, OUTPUT);
    pinMode(LED_SURVEY_IN, OUTPUT);
    pinMode(SURVEY_PIN, INPUT_PULLUP);

    for (int i = 0; i < 5; i++)
    {
        digitalWrite(LED_PING, HIGH);
        delay(50);
        digitalWrite(LED_PING, LOW);
        delay(50);
    }

    updateOLED();

    xTaskCreatePinnedToCore(_cmdTask, "CMDTask", 8192, NULL, 1, NULL, PRO_CPU_NUM);

    gsmConnect();
    mqttConnect();
}

void loop()
{
    if (!mqtt.connected())
        mqttReconnect();

    mqtt.loop();
    f9pLoop();

    if (millis() - lastModemReset > 6UL * 3600 * 1000)
        resetModem();

    if (ESP.getFreeHeap() < 20000)
        esp_restart();

    yield();
}
