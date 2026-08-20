#pragma once

#include <Arduino.h>
#include <PubSubClient.h>

class SerialLog
{
private:
    static PubSubClient *&_mqttRef()
    {
        static PubSubClient *p = nullptr;
        return p;
    }
    static const char *&_topicRef()
    {
        static const char *t = nullptr;
        return t;
    }

    static void _append(String &s, bool v) { s += v ? "true" : "false"; }
    template <typename T>
    static void _append(String &s, T v) { s += v; }

    template <typename T>
    static void _appendAll(String &s, T t) { _append(s, t); }

    template <typename T, typename... Args>
    static void _appendAll(String &s, T first, Args... rest)
    {
        _append(s, first);
        s += ',';
        _appendAll(s, rest...);
    }

    static void _emit(const String &s)
    {
        if (Serial)
            Serial.println(s);
        PubSubClient *m = _mqttRef();
        const char *t = _topicRef();
        if (m && t && m->connected())
            m->publish(t, s.c_str());
    }

public:
    static void setMqtt(PubSubClient &mqtt, const char *topic)
    {
        _mqttRef() = &mqtt;
        _topicRef() = topic;
    }

    static void log(const char *message)
    {
        String s = "[LOG] ";
        s += message;
        _emit(s);
    }

    template <typename... Args>
    static void log(const char *message, Args... args)
    {
        String s = "[LOG] ";
        s += message;
        s += ':';
        _appendAll(s, args...);
        _emit(s);
    }
};
