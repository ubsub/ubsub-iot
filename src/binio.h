#include <cstring>

#ifndef binio_h
#define binio_h

/**
Particle/arduino can't use pointer-arithmatic to copy memory of certain types
(namely 64 bit ones) into or out of non-aligned memory.  This helpers
were provided as a method to work around that issue using memcpy() if necessary
**/

// Enable byte-by-byte read/write, even on platforms where there are faster alternatives
// #define UBSUB_MANUAL_RW true

template <typename T> static inline T read_le(const uint8_t* at) {
  #if PARTICLE || ARDUINO || UBSUB_MANUAL_RW
    T ret = 0;
    memcpy(&ret, at, sizeof(T));
    return ret;
  #else
    return *(T*)at;
  #endif
}

template <typename T> static inline void write_le(uint8_t* to, const T& val) {
  #if PARTICLE || ARDUINO || UBSUB_MANUAL_RW
  memcpy(to, &val, sizeof(T));
  #else
  *(T*)to = val;
  #endif
}

#endif
