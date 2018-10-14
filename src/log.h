
#ifndef ubsub_log_h
#define ubsub_log_h

#ifdef UBSUB_LOG
  #if !(ARDUINO || PARTICLE)
    #include <iostream>
    #include <stdio.h>
    #include <stdarg.h>
  #else
    #include <Arduino.h>
    #warning Logs are enabled. This will result in more string memory being consumed
  #endif

  static void writeLog(const char* level, const char* filename, int line, const char* msg, ...) {
    static char logbuf[256];
    va_list argptr;
    va_start(argptr, msg);
    vsnprintf(logbuf, sizeof(logbuf), msg, argptr);
    va_end(argptr);
    #if ARDUINO || PARTICLE
      Serial.printf("[%s] (%s:%d) %s", level, filename, line, logbuf);
      Serial.println();
      Serial.flush();
    #else
      std::cerr << "[" << level << "] (" << filename << ":" << line << ") " << logbuf << std::endl;
    #endif
  }

  static char hextable[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  // Nieve helper that stores in local memory. Don't use more than once per log msg (or copy out)
  template <typename T> static const char* tohexstr(T val) {
    static char buf[32];
    int i=0;
    for (; i<(int)sizeof(T)*2; ++i) {
      buf[sizeof(T)*2-i-1] = hextable[val & 0xF];
      val >>= 4;
    }
    buf[i] = '\0';
    return buf;
  }

  #ifdef UBSUB_LOG_DEBUG
    #define LOG_DEBUG(msg, ...) writeLog("DEBUG", __FILE__, __LINE__, msg, ## __VA_ARGS__)
  #else
    #define LOG_DEBUG(msg, ...)
  #endif
  #define LOG_INFO(msg, ...) writeLog("INFO", __FILE__, __LINE__, msg, ## __VA_ARGS__)
  #define LOG_WARN(msg, ...) writeLog("WARN", __FILE__, __LINE__, msg, ## __VA_ARGS__)
  #define LOG_ERROR(msg, ...) writeLog("ERROR", __FILE__, __LINE__, msg, ## __VA_ARGS__)
#else
  #define LOG_DEBUG(msg, ...)
  #define LOG_INFO(msg, ...)
  #define LOG_WARN(msg, ...)
  #define LOG_ERROR(msg, ...)
#endif

#endif
