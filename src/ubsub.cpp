#include "ubsub.h"
#include "sha256.h"
#include "salsa20.h"

#if PARTICLE
  #include <Particle.h>
#elif ARDUINO
  #include <Arduino.h>
  #include <ESP8266WiFi.h>
  #include <WiFiUdp.h>
#else
  #include <string.h>
  #include <stdio.h>
  #include <ctime>
  #include <stdlib.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <math.h>
#endif

const char* DEFAULT_UBSUB_ROUTER = "udp.ubsub.io";
const int DEFAULT_UBSUB_PORT = 3005;

// Generates outer & inner packet, running it through cryptography
#define UBSUB_CRYPTHEADER_LEN 25
#define UBSUB_HEADER_LEN 13
#define UBSUB_SIGNATURE_LEN 32

#define USER_ID_MAX_LEN 16

// Get time in seconds
static uint64_t getTime() {
#if ARDUINO
  return (uint64_t)(millis() / 1000);
#elif PARTICLE
  return now();
#else
  return std::time(NULL);
#endif
}

// Get random nonce
static uint32_t getNonce32() {
#if ARDUINO || PARTICLE
  return (random(256) << 24) | (random(256) << 16) | (random(256) << 8) | random(256);
#else
  srand(getTime()); //FIXME: This isn't great
  return (uint32_t)rand() + (uint32_t)rand();
#endif
}

static uint64_t getNonce64() {
  return (uint64_t)getNonce32() << 32 | getNonce32();
}

static int min(int left, int right) {
  return left < right ? left : right;
}

static int createPacket(uint8_t* buf, int bufSize, const char *userId, const char *key, uint16_t cmd, uint8_t flag, const uint8_t *body, int bodyLen) {
  if (bufSize < UBSUB_CRYPTHEADER_LEN + UBSUB_HEADER_LEN + bodyLen + UBSUB_SIGNATURE_LEN) {
    // Buffer too short
    return -1;
  }

  uint64_t nonce = getNonce64();
  uint64_t ts = getTime();

  const int userIdLen = strlen(userId);
  if (userIdLen > USER_ID_MAX_LEN) {
    return -2;
  }

  // Zero the buffer
  memset(buf, 0, bufSize);

  // Set up CrpyHeader
  buf[0] = 0x3; // UDPv3 (encrypted with salsa20)
  *(uint64_t*)(buf+1) = nonce; // 64 bit nonce
  memcpy(buf+9, userId, userIdLen);

  // Set header
  *(uint64_t*)(buf+25) = ts;
  *(uint16_t*)(buf+33) = cmd;
  *(uint16_t*)(buf+35) = (uint16_t)bodyLen;
  *(uint8_t*)(buf+37) = flag;

  // Copy body to buffer
  memcpy(buf+38, body, bodyLen);

  // Run the body though the cipher
  Sha256.init();
  Sha256.write((uint8_t*)key, strlen(key));
  uint8_t* expandedKey = Sha256.result();
  s20_crypt(expandedKey, S20_KEYLEN_256, (uint8_t*)&nonce, 0, buf+25, UBSUB_HEADER_LEN + bodyLen);

  // Sign the entire thing
  Sha256.initHmac((uint8_t*)key, strlen(key));
  Sha256.write(buf, UBSUB_CRYPTHEADER_LEN + UBSUB_HEADER_LEN + bodyLen);
  uint8_t* digest = Sha256.resultHmac();
  memcpy(buf + UBSUB_CRYPTHEADER_LEN + UBSUB_HEADER_LEN + bodyLen, digest, 32);

  return UBSUB_CRYPTHEADER_LEN + UBSUB_HEADER_LEN + bodyLen + UBSUB_SIGNATURE_LEN;
}


// Ubsub Implementation

Ubsub::Ubsub(const char *userId, const char *userKey, const char *ubsubHost, int ubsubPort) {
  this->userId = userId;
  this->userKey = userKey;
  this->host = ubsubHost;
  this->port = ubsubPort;
  for (int i=0; i<ERROR_BUFFER_LEN; ++i) {
    this->lastError[i] = NULL;
  }
  this->onLog = NULL;
  this->initSocket();
}

Ubsub::Ubsub(const char *userId, const char *userKey) {
  this->userId = userId;
  this->userKey = userKey;
  this->host = DEFAULT_UBSUB_ROUTER;
  this->port = DEFAULT_UBSUB_PORT;
  for (int i=0; i<ERROR_BUFFER_LEN; ++i) {
    this->lastError[i] = NULL;
  }
  this->onLog = NULL;
  this->initSocket();
}

bool Ubsub::connect(int timeout) {
  this->log("DEBUG", "Attempting connect...");
  return false;
}


void Ubsub::publishEvent(const char *topicId, const char *topicKey, const char *msg) {
  static uint8_t command[1024+64];
  memset(command, 0, 64);
  memcpy(command, topicId, min(strlen(topicId), 32));
  memcpy(command+32, topicKey, min(strlen(topicKey), 32));

  int msgLen = min(strlen(msg), 1024);
  memcpy(command+64, msg, msgLen);

  static uint8_t buf[UBSUB_MTU];
  int plen = createPacket(buf, UBSUB_MTU, this->userId, this->userKey, 0x0A, 0x0, command, msgLen+64);
  if (plen < 0) {
    this->setError("Error creating packet");
    return;
  }

  this->sendData(buf, plen);
}

void Ubsub::createTopic(const char *topicName) {

}

void Ubsub::listenToTopic(const char *topicNameOrId, topicCallback callback) {

}

void Ubsub::createFunction(const char *name, topicCallback callback) {

}

void Ubsub::callFunction(const char *name, const char *arg) {

}

void Ubsub::callFunction(const char *name) {
  this->callFunction(name, NULL);
}

const char* Ubsub::getLastError() {
  const char* err = this->lastError[0];
  for (int i=0; i<ERROR_BUFFER_LEN-1; ++i) {
    this->lastError[i] = this->lastError[i+1];
  }
  this->lastError[ERROR_BUFFER_LEN-1] = NULL;
  return err;
}

void Ubsub::setError(const char* err) {
  // Shift errors up and set error at 0
  for (int i=ERROR_BUFFER_LEN-1; i>0; --i) {
    this->lastError[i] = this->lastError[i-1];
  }
  this->lastError[0] = err;
  this->log("ERROR", err);
}

void Ubsub::setOnLog(logCallback callback) {
  this->onLog = callback;
}

void Ubsub::log(const char* level, const char* msg) {
  if (this->onLog != NULL) {
    this->onLog(level, msg);
  }
}

void Ubsub::processEvents() {

}

void Ubsub::sendData(const uint8_t* buf, int bufSize) {
  if (bufSize > UBSUB_MTU) {
    this->setError("Send data buffer exceeds MTU");
    return;
  }
  if (this->sock < 0) {
    this->setError("Socket not established");
    return;
  }

  #if ARDUINO
  #warning
  #elif PARTICLE
  #warning
  #else
    this->log("DEBUG", "Sending bytes to host...");

    struct hostent *server;
    server = gethostbyname(this->host);
    if (server == NULL) {
      this->log("WARN", "Failed to resolve hostname. Connected?");
      //TODO: Queue outgoing data (if required.. flag?)
      return;
    }

    struct sockaddr_in serveraddr;
    bzero((char*)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char*)server->h_addr, (char*)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(this->port);

    int ret = sendto(this->sock, buf, bufSize, 0, (sockaddr*)&serveraddr, sizeof(serveraddr));
    if (ret != bufSize) {
      this->setError("Failed to send data to host");
      return;
    }
  #endif
}

void Ubsub::initSocket() {
  #if ARDUINO
    #warning No implementation for arduino initSocket
  #elif PARTICLE
    #warning No implementation of particle initSocket
  #else
    this->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (this->sock < 0) {
      this->setError("Unable to create new DGRAM socket");
      return;
    }

    struct sockaddr_in bindAddr;
    memset((char*)&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    // bindAddr.sin_port = htons(this->port); // TODO: Do i need this? Anyport?
    bindAddr.sin_port = 0; //anyport

    if (bind(this->sock, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
      this->setError("Error binding to local port");
      this->closeSocket();
      return;
    }
  #endif
}

void Ubsub::closeSocket() {
  if (this->sock > 0) {
    close(this->sock);
    this->sock = -1;
  }
}
