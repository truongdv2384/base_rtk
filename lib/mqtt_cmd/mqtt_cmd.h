#pragma once

#include <Arduino.h>
#include <PubSubClient.h>

class MqttCmd
{
public:
    using Handler = void (*)();

    struct Entry
    {
        const char *cmd;
        Handler fn;
    };

    static void begin(PubSubClient &mqtt, const char *topic,
                      const Entry *entries, size_t count);

    static void subscribe();

private:
    static void _callback(char *topic, byte *payload, unsigned int length);

    static PubSubClient *_mqtt;
    static const char *_topic;
    static const Entry *_entries;
    static size_t _count;
};
