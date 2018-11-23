/**
Ubsub log wrapper to send log events to ubsub (or other sources)

Configuration:
ULOG_DISABLE        Disable all logging if defined (macro to nothing)
ULOG_BUF_SIZE       Override default buffer size (default: 512)
ULOG_TOPIC          Override topic to write log messages to (default: "log")
ULOG_SERIAL         Enable serial output (or stdout if unix)
**/

#ifndef ubsub_log_h
#define ubsub_log_h

#ifndef ULOG_DISABLE

  #if (!ARDUINO || PARTICLE)
    #include <iostream>
    #include <stdio.h>
    #include <stdarg.h>
  #else
    #warning Logs are enabled on IoT device, may consume extra string memory!
    #include <Arduino.h>
  #endif
  #include <string.h>
  #include "ubsub.h"
  #include "minijson.h"

  #ifndef ULOG_BUF_SIZE
  #define ULOG_BUF_SIZE 512
  #endif

  #ifndef ULOG_TOPIC
  #define ULOG_TOPIC "log"
  #endif

  static const char* _deviceId = NULL;
  static Ubsub* _conn = NULL;

  void initUbsubLogger(const char *deviceId, const char *deviceKey) {
    if (_conn == NULL) {
      _conn = new Ubsub(deviceId, deviceKey);
      _conn->enableAutoRetry(false);
      _conn->connect();
    }
  }
  void initUbsubLogger(Ubsub* conn) {
    _conn = conn;
  }
  void setLoggerDeviceId(const char* deviceId) {
    _deviceId = strdup(deviceId);
  }
  void shutdownLogger() {
    if (_conn != NULL) {
      delete _conn;
      _conn = NULL;
    }
  }

  static void writeULog(const char* level, const char* filename, int line, const char* msg, ...) {
    static char logbuf[ULOG_BUF_SIZE];
    va_list argptr;
    va_start(argptr, msg);
    vsnprintf(logbuf, sizeof(logbuf), msg, argptr);
    va_end(argptr);

    #ifdef ULOG_SERIAL
      #if ARDUINO || PARTICLE
        Serial.printf("[%s] (%s:%d) %s\n", level, filename, line, logbuf);
        Serial.flush();
      #else
        std::cerr << "[" << level << "] (" << filename << ":" << line << ") " << logbuf << std::endl;
      #endif
    #endif

    if (_conn != NULL) {
      char jsonBuf[ULOG_BUF_SIZE]; // Buffer on stack
      MiniJsonBuilder json(jsonBuf, ULOG_BUF_SIZE);
      json.open()
        .write("level", level)
        .write("filename", filename)
        .write("line", line)
        .write("msg", logbuf);
      if (_deviceId != NULL) {
        json.write("device", _deviceId);
      }
      json.close();

      _conn->callFunction(ULOG_TOPIC, json.c_str());
    }
  }

  #define UINFO(msg, ...)  writeULog("INFO", __FILE__, __LINE__, msg, ## __VA_ARGS__)
  #define UWARN(msg, ...)  writeULog("WARN", __FILE__, __LINE__, msg, ## __VA_ARGS__)
  #define UERROR(msg, ...) writeULog("ERROR", __FILE__, __LINE__, msg, ## __VA_ARGS__)
  #define UDEBUG(msg, ...) writeULog("DEBUG", __FILE__, __LINE__, msg, ## __VA_ARGS__)

#else
  // Ubsub log disabled, nullify log statements
  #define initUbsubLogger
  #define setLoggerDeviceId
  #define shutdownLogger

  #define UINFO(msg, ...)
  #define UWARN(msg, ...)
  #define UERROR(msg, ...)
  #define UDEBUG(msg, ...)
#endif

#endif
