#include "mqtt_cmd.h"
#include <serial_log.h>

PubSubClient *MqttCmd::_mqtt = nullptr;
const char *MqttCmd::_topic = nullptr;
const MqttCmd::Entry *MqttCmd::_entries = nullptr;
size_t MqttCmd::_count = 0;

void MqttCmd::begin(PubSubClient &mqtt, const char *topic,
                    const Entry *entries, size_t count)
{
    _mqtt = &mqtt;
    _topic = topic;
    _entries = entries;
    _count = count;
    _mqtt->setCallback(_callback);
}

void MqttCmd::subscribe()
{
    if (_mqtt && _topic)
        _mqtt->subscribe(_topic);
}

void MqttCmd::_callback(char *topic, byte *payload, unsigned int length)
{
    char buf[32];
    unsigned int n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = 0;

    SerialLog::log("MQTT CMD", buf);

    for (size_t i = 0; i < _count; i++)
    {
        size_t clen = strlen(_entries[i].cmd);
        if (strncmp(buf, _entries[i].cmd, clen) == 0)
        {
            if (_entries[i].fn)
                _entries[i].fn();
            return;
        }
    }
}
