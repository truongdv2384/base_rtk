#pragma once

#include <Arduino.h>
#include <PubSubClient.h>

class MqttCmd
{
public:
    using Handler = void (*)();
    // Handler nhan phan payload CON LAI sau tu khoa lenh (da bo khoang trang
    // dau). Dung cho lenh mang tham so, vd: OTA {"url":...}
    using DataHandler = void (*)(const char *arg, size_t len);

    struct Entry
    {
        const char *cmd;
        Handler fn;      // lenh khong tham so
        DataHandler dfn; // lenh co tham so (de nullptr neu khong dung)
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
