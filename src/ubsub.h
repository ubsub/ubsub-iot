#include <stdint.h>

#ifndef ubsub_h
#define ubsub_h

#if ARDUINO
  #include <WiFiUdp.h>
  typedef WiFiUDP UDPSocket;
#elif PARTICLE
  #include <Particle.h>
  typedef UDP UDPSocket;
#else
  typedef int UDPSocket;
#endif

typedef void (*topicCallback)(const char* arg);

// Configurable settings
#define UBSUB_ERROR_BUFFER_LEN 16
#define UBSUB_MTU 256
#define UBSUB_PACKET_RETRY_SECONDS 2
#define UBSUB_PACKET_RETRY_ATTEMPTS 5
#define UBSUB_PACKET_TIMEOUT 10
#define UBSUB_PING_FREQ 5
#define UBSUB_CONNECTION_TIMEOUT 120

// If defined, will log to stderr on unix, and Serial on embedded
// Not enabled by default on embedded, but feel free to enable!
// If enabled, will output logs to serial
#if !(ARDUINO || PARTICLE)
#define UBSUB_LOG
#endif

// Error codes
#define UBSUB_ERR_INVALID_PACKET -1
#define UBSUB_ERR_BAD_VERSION -2
#define UBSUB_ERR_USER_MISMATCH -3
#define UBSUB_ERR_BAD_SIGNATURE -4
#define UBSUB_ERR_TIMEOUT -5
#define UBSUB_ERR_EXCEEDS_MTU -6
#define UBSUB_ERR_SOCKET -7
#define UBSUB_ERR_SOCKET_BIND -8
#define UBSUB_ERR_NETWORK -9
#define UBSUB_ERR_SEND -10
#define UBSUB_ERR_BAD_REQUEST -11
#define UBSUB_MISSING_ARGS -50
#define UBSUB_ERR_UNKNOWN -1000
#define UBSUB_ERR_MALLOC -2000

typedef struct QueuedMessage {
  uint8_t* buf;
  int bufLen;
  uint64_t retryTime;
  int retryNumber;
  uint64_t cancelNonce;
  QueuedMessage* next;
} QueuedMessage;

class Ubsub {
private: // Config
  const char* userId;
  const char* userKey;
  const char* host;
  int port;
  int localPort;
  bool autoRetry;

  UDPSocket sock;
  bool socketInit;

  int lastError[UBSUB_ERROR_BUFFER_LEN];

private: // State
  uint64_t lastPong;
  uint64_t lastPing;

  QueuedMessage* queue;

private:
  void init(const char *userId, const char *userKey, const char *ubsubHost, const int ubsubPort);

  void initSocket();
  void closeSocket();
  int sendData(const uint8_t* buf, int bufSize);
  int sendCommand(uint16_t cmd, uint8_t flag, bool retry, const uint8_t *command, int commandLen);

  int receiveData();
  void processPacket(uint8_t *buf, int len);

  void ping();

  void setError(int errcode);

  QueuedMessage* queueMessage(const uint8_t* buf, int bufLen, const uint64_t &nonce);
  void removeQueue(const uint64_t &nonce);
  void processQueue();

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
  int publishEvent(const char *topicId, const char *topicKey, const char *msg);

  // Creates a new topic, but does NOT listen to it
  void createTopic(const char *topicName);

  // Listen to a given topic for events. Similar to creating a function
  // but will listen to an existing topic
  void listenToTopic(const char *topicNameOrId, topicCallback callback);

  // Create a new function that can be invoked by another caller, and immediately listen to it
  // This function will be a topic in ubsub
  void createFunction(const char *name, topicCallback callback);

  // Call function on another device
  int callFunction(const char *name, const char *arg);
  int callFunction(const char *name);

  // processEvents() needs to be called frequently.  It takes care of things such as:
  // - Re-sending any queued outbound messages
  // - Checking for, and processing, incoming data
  // - Occassionally handling pings to keep connection alive (or reconnecting if needed)
  void processEvents();

  // Gets the last error, or NULL if no error
  const int getLastError();
};

#endif
