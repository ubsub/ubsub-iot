#include <stdint.h>

#ifndef ubsub_h
#define ubsub_h

#if ARDUINO
  typedef WiFiUDP UDPSocket;
#elif PARTICLE
  typedef UDP UDPSocket;
#else
  typedef int UDPSocket;
#endif

typedef void (*topicCallback)(const char* arg);
typedef void (*logCallback)(const char *level, const char* msg);

#define ERROR_BUFFER_LEN 16
#define UBSUB_MTU 256

class Ubsub {
private:
  const char* userId;
  const char* userKey;
  const char* host;
  int port;
  int localPort;

  UDPSocket sock;

  logCallback onLog;
  const char *lastError[ERROR_BUFFER_LEN];

private:
  void initSocket();
  void closeSocket();
  int sendData(const uint8_t* buf, int bufSize);
  int sendCommand(uint16_t cmd, uint8_t flag, const uint8_t *command, int commandLen);

  int receiveData();
  void processPacket(uint8_t *buf, int len);

  void ping();

  void setError(const char* err);
  void log(const char* level, const char* msg);

  uint64_t lastPong;

public:
  Ubsub(const char *userId, const char *userKey, const char *ubsubHost, int ubsubPort);

  Ubsub(const char* userId, const char* userKey);

  // Attempts to establish a connection with UbSub.io
  // If succeeds returns true.  If fails after timeout, returns false
  // NOT REQUIRED to call, but will guarantee a connection has been established
  bool connect(int timeout = 10);

  // Send message to a given topic on ubsub. Topic does not have to belong
  // to the user.
  // The msg can either be serialized JSON OR a simple string that will be encapsulated on the server
  void publishEvent(const char *topicId, const char *topicKey, const char *msg);

  // Creates a new topic, but does NOT listen to it
  void createTopic(const char *topicName);

  // Listen to a given topic for events. Similar to creating a function
  // but will listen to an existing topic
  void listenToTopic(const char *topicNameOrId, topicCallback callback);

  // Create a new function that can be invoked by another caller, and immediately listen to it
  // This function will be a topic in ubsub
  void createFunction(const char *name, topicCallback callback);

  // Call function on another device
  void callFunction(const char *name, const char *arg);
  void callFunction(const char *name);

  // processEvents() needs to be called frequently.  It takes care of things such as:
  // - Re-sending any queued outbound messages
  // - Checking for, and processing, incoming data
  // - Occassionally handling pings to keep connection alive (or reconnecting if needed)
  void processEvents();

  // Gets the last error, or NULL if no error
  const char* getLastError();

  // Sets a function to be called when a log event occurs
  void setOnLog(logCallback logger);
};

#endif