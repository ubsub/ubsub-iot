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
  #include <fcntl.h>
#endif

const char* DEFAULT_UBSUB_ROUTER = "udp.ubsub.io";
const int DEFAULT_UBSUB_PORT = 3005;

// Generates outer & inner packet, running it through cryptography
#define UBSUB_CRYPTHEADER_LEN 25
#define UBSUB_HEADER_LEN 13
#define UBSUB_SIGNATURE_LEN 32

#define USER_ID_MAX_LEN 16

#define UBSUB_PACKET_TIMEOUT 10

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
  if (body != NULL && bodyLen > 0) {
    memcpy(buf+38, body, bodyLen);
  }

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
  this->localPort = getNonce32() % 32768 + 32767;
  for (int i=0; i<ERROR_BUFFER_LEN; ++i) {
    this->lastError[i] = NULL;
  }
  this->onLog = NULL;
  this->lastPong = 0;
  this->initSocket();
}

Ubsub::Ubsub(const char *userId, const char *userKey) {
  this->userId = userId;
  this->userKey = userKey;
  this->host = DEFAULT_UBSUB_ROUTER;
  this->port = DEFAULT_UBSUB_PORT;
  this->localPort = getNonce32() % 32768 + 32767;
  for (int i=0; i<ERROR_BUFFER_LEN; ++i) {
    this->lastError[i] = NULL;
  }
  this->onLog = NULL;
  this->lastPong = 0;
  this->initSocket();
}

bool Ubsub::connect(int timeout) {
  this->lastPong = 0;
  uint64_t start = getTime();
  while(getTime() < start + timeout) {
    this->log("DEBUG", "Attempting connect...");
    this->ping();

    uint64_t waitStart = getTime();
    while(getTime() < waitStart + 1) {
      this->receiveData();
      if (this->lastPong > 0) {
        return true;
      }
    }
  }
  return false;
}


void Ubsub::publishEvent(const char *topicId, const char *topicKey, const char *msg) {
  static uint8_t command[UBSUB_MTU];
  memset(command, 0, 64);
  memcpy(command, topicId, min(strlen(topicId), 32));
  memcpy(command+32, topicKey, min(strlen(topicKey), 32));

  int msgLen = min(strlen(msg), UBSUB_MTU-64);
  memcpy(command+64, msg, msgLen);

  this->sendCommand(0x0A, 0x0, command, msgLen + 64);
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
  this->receiveData();
}

int Ubsub::receiveData() {
  static uint8_t buf[UBSUB_MTU];
  int received = 0;
  int rlen = 0;

  while (true) {
    #if ARDUINO
      #warning
    #elif PARTICLE
      #warning
    #else
      struct sockaddr_in from;
      socklen_t fromlen;
      rlen = recvfrom(this->sock, buf, UBSUB_MTU, 0x0, (struct sockaddr*)&from, &fromlen);
    #endif

    if (rlen <= 0)
      break;

    this->processPacket(buf, rlen);

    received++;
  }

  return received;
}

void Ubsub::processPacket(uint8_t *buf, int len) {
  this->log("INFO", "Got data");
  if (len < UBSUB_HEADER_LEN + UBSUB_CRYPTHEADER_LEN + UBSUB_SIGNATURE_LEN) {
    this->setError("Packet too small");
    return;
  }

  uint8_t version = buf[0];
  if (version != 0x2 && version != 0x3) {
    this->setError("Bad version");
    return;
  }

  uint64_t nonce = *(uint64_t*)(buf+1);
  char userId[17];
  strncpy(userId, (char*)(buf+9), 16);

  if (strcmp(userId, this->userId) != 0) {
    this->setError("User mismatch");
    return;
  }

  // Test the signature
  Sha256.initHmac((uint8_t*)this->userKey, strlen(this->userKey));
  Sha256.write(buf, len - UBSUB_SIGNATURE_LEN);
  uint8_t* digest = Sha256.resultHmac();
  uint8_t* signature = buf + len - UBSUB_SIGNATURE_LEN;
  if (memcmp(digest, signature, 32) != 0) {
    this->setError("Bad signature");
    return;
  }

  // If version 0x3, need to run through cipher
  if (version == 0x3) {
    Sha256.init();
    Sha256.write((uint8_t*)this->userKey, strlen(this->userKey));
    uint8_t* expandedKey = Sha256.result();
    s20_crypt(expandedKey, S20_KEYLEN_256, (uint8_t*)&nonce, 0, buf+25, len - UBSUB_CRYPTHEADER_LEN - UBSUB_SIGNATURE_LEN);
  }

  uint64_t ts = *(uint64_t*)(buf + 25);
  uint16_t cmd = *(uint16_t*)(buf + 33);
  uint16_t bodyLen = *(uint16_t*)(buf+35);
  uint8_t flag = *(uint8_t*)(buf+37);

  uint8_t* body = buf + 38;

  // Validate timestamp is within bounds
  int diff = (int64_t)getTime() - (int64_t)ts;
  if (diff < -UBSUB_PACKET_TIMEOUT || diff > UBSUB_PACKET_TIMEOUT) {
    this->setError("Packet timeout");
    return;
  }

  //TODO: Validate Nonce hasn't already been used (dupe)


  switch(cmd) {
    case 0x11: // Pong
      this->log("INFO", "GOT PONG");
      this->lastPong = getTime();
      break;
    default:
      this->log("WARN", "Unknown packet");
      break;
  }
}

void Ubsub::ping() {
  uint8_t buf[2];
  *(uint16_t*)buf = (uint16_t)this->localPort;
  this->sendCommand(0x10, 0x0, buf, 2);
}

int Ubsub::sendCommand(uint16_t cmd, uint8_t flag, const uint8_t *command, int commandLen) {
  static uint8_t buf[UBSUB_MTU];
  int plen = createPacket(buf, UBSUB_MTU, this->userId, this->userKey, cmd, flag, command, commandLen);
  if (plen < 0) {
    this->setError("Error creating packet");
    return -1;
  }

  return this->sendData(buf, plen);
}

int Ubsub::sendData(const uint8_t* buf, int bufSize) {
  if (bufSize > UBSUB_MTU) {
    this->setError("Send data buffer exceeds MTU");
    return -1;
  }
  if (this->sock < 0) {
    this->setError("Socket not established");
    return -1;
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
      return -1;
    }

    struct sockaddr_in serveraddr;
    bzero((char*)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char*)server->h_addr, (char*)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(this->port);

    int ret = sendto(this->sock, buf, bufSize, 0, (sockaddr*)&serveraddr, sizeof(serveraddr));
    if (ret != bufSize) {
      this->setError("Failed to send data to host");
      return -1;
    }
    return ret;
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
    bindAddr.sin_port = htons(this->localPort);

    if (bind(this->sock, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
      this->setError("Error binding to local port");
      this->closeSocket();
      return;
    }

    if (fcntl(this->sock, F_SETFL, fcntl(this->sock, F_GETFL) | O_NONBLOCK) < 0) {
      this->setError("Failed to put socket in non-blocking mode");
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
