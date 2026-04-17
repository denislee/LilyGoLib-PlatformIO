/**
 * @file      peripherals.cpp
 * @brief     NFC discovery + IR remote send/receive.
 */
#include "peripherals.h"
#include "system.h"
#include "internal.h"

#ifdef ARDUINO
#include <LilyGoLib.h>
#include "../app_nfc.h"
#else
#include <climits>
#include <cstdlib>
#endif

// --- NFC ---------------------------------------------------------------

#if defined(USING_ST25R3916) && defined(ARDUINO)

extern void ui_nfc_pop_up(wifi_conn_params_t &params);

static void nrf_notify_callback()
{
    Serial.println("NDEF Detected.");
    hw_feedback();
}

static void ndef_event_callback(ndefTypeId id, void *data)
{
    static ndefTypeRtdDeviceInfo   devInfoData;
    static ndefConstBuffer         bufAarString;
    static ndefRtdUri              url;
    static ndefRtdText             text;
    static String msg = "";
    static wifi_conn_params_t params;
    msg = "";
    switch (id) {
    case NDEF_TYPE_EMPTY:
        break;
    case NDEF_TYPE_RTD_DEVICE_INFO:
        memcpy(&devInfoData, data, sizeof(ndefTypeRtdDeviceInfo));
        break;
    case NDEF_TYPE_RTD_TEXT:
        memcpy(&text, data, sizeof(ndefRtdText));
        Serial.printf("LanguageCode: %s\nSentence: %s\n", reinterpret_cast < const char * > (text.bufLanguageCode.buffer), reinterpret_cast < const char * > (text.bufSentence.buffer));
        msg.concat("LanguageCode: ");
        msg.concat(reinterpret_cast < const char * > (text.bufLanguageCode.buffer));
        msg.concat("\nSentence: ");
        msg.concat(reinterpret_cast < const char * > (text.bufSentence.buffer));
        ui_msg_pop_up("NFC Text", msg.c_str());
        break;
    case NDEF_TYPE_RTD_URI:
        memcpy(&url, data, sizeof(ndefRtdUri));
        Serial.printf("PROTOCOL:%s URL:%s\n", reinterpret_cast < const char * > (url.bufProtocol.buffer), reinterpret_cast < const char * > (url.bufUriString.buffer));
        msg.concat("PROTOCOL:");
        msg.concat(reinterpret_cast < const char * > (url.bufProtocol.buffer));
        msg.concat("URL:");
        msg.concat(reinterpret_cast < const char * > (url.bufUriString.buffer));
        ui_msg_pop_up("NFC Url", msg.c_str());
        break;
    case NDEF_TYPE_RTD_AAR:
        memcpy(&bufAarString, data, sizeof(ndefConstBuffer));
        Serial.printf("NDEF_TYPE_RTD_AAR :%s\n", (char *)bufAarString.buffer);
        break;
    case NDEF_TYPE_MEDIA:
        break;
    case NDEF_TYPE_MEDIA_VCARD:
        break;
    case NDEF_TYPE_MEDIA_WIFI: {
        ndefTypeWifi *wifi = (ndefTypeWifi *)data;
        params.ssid = std::string(reinterpret_cast < const char * > (wifi->bufNetworkSSID.buffer), wifi->bufNetworkSSID.length);
        params.password = std::string(reinterpret_cast < const char * > (wifi->bufNetworkKey.buffer), wifi->bufNetworkKey.length);
        Serial.printf("ssid:<%s> password:<%s>\n", params.ssid.c_str(), params.password.c_str());
        ui_nfc_pop_up(params);
    }
    break;
    default:
        break;
    }
}
#endif  /*USING_ST25R3916*/

bool hw_start_nfc_discovery()
{
#if  defined(USING_ST25R3916) && defined(ARDUINO)
    instance.powerControl(POWER_NFC, true);
    return beginNFC(nrf_notify_callback, ndef_event_callback);
#else
    return false;
#endif
}

void hw_stop_nfc_discovery()
{
#if  defined(USING_ST25R3916) && defined(ARDUINO)
    deinitNFC();
    instance.powerControl(POWER_NFC, false);
#endif
}

bool hw_get_nfc_enable() { return user_setting.nfc_enable; }
void hw_set_nfc_enable(bool en) {
    user_setting.nfc_enable = en;
#ifdef ARDUINO
    instance.powerControl(POWER_NFC, en);
    delay(10);
#endif
}

// --- IR remote ---------------------------------------------------------

#if defined(ARDUINO) && defined(USING_IR_REMOTE)
#include <IRsend.h>
IRsend irsend(IR_SEND); // T-Watch S3 GPIO2 pin to use.
#endif

#if defined(ARDUINO) && defined(USING_IR_RECEIVER)
#include <IRremoteESP8266.h>
#include <IRrecv.h>
IRrecv irrecv(IR_SEND); // T-Watch S3 GPIO15 pin to use.
#endif

void hw_set_remote_code(uint32_t nec_code)
{
#if defined(ARDUINO) && defined(USING_IR_REMOTE)
    static bool isBegin = false;
    if (!isBegin) {
        isBegin = true;
        irsend.begin();
    }
    irsend.sendNEC(nec_code);
#endif
}

void hw_get_remote_code(uint64_t &result)
{
#if defined(ARDUINO) && defined(USING_IR_RECEIVER)
    decode_results results;
    if (irrecv.decode(&results)) {
        Serial.print("IR Code received: ");
        Serial.println(results.value, HEX);
        result = results.value;
        irrecv.resume();  // Receive the next value
    }
#else
    result = random(0, INT_MAX);
#endif
}

void hw_ir_function_select(bool enableSend)
{
#if defined(ARDUINO) && defined(USING_IR_REMOTE) && defined(USING_IR_RECEIVER)
    if (enableSend) {
        instance.IRFunctionSelect(IR_FUNC_SENDER);
        irrecv.disableIRIn();
    } else {
        instance.IRFunctionSelect(IR_FUNC_RECEIVER);
        irrecv.enableIRIn();
    }
#endif
}
