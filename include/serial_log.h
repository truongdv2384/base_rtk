#pragma once

#include <Arduino.h>

class SerialLog
{
private:
    static void _printVal(bool v) { Serial.print(v ? "true" : "false"); }
    template <typename T>
    static void _printVal(T v) { Serial.print(v); }

    template <typename T>
    static void _printAll(T t) { _printVal(t); }

    template <typename T, typename... Args>
    static void _printAll(T first, Args... rest)
    {
        _printVal(first);
        Serial.print(',');
        _printAll(rest...);
    }

public:
    static void log(const char *message)
    {
#if defined(DEBUG)
        if (Serial)
        {
            Serial.print("[LOG] ");
            Serial.println(message);
        }
#endif
    }

    template <typename... Args>
    static void log(const char *message, Args... args)
    {
#if defined(DEBUG)
        if (Serial)
        {
            Serial.print("[LOG] ");
            Serial.print(message);
            Serial.print(':');
            _printAll(args...);
            Serial.println();
        }
#endif
    }
};
