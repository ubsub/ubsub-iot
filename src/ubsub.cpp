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

const char* DEFAULT_UBSUB_ROUTER = "router.ubsub.io";
const int DEFAULT_UBSUB_PORT = 3005;

// Generates outer & inner packet, running it through cryptography
#define UBSUB_CRYPTHEADER_LEN 25
#define UBSUB_HEADER_LEN 13
#define UBSUB_SIGNATURE_LEN 32
#define USER_ID_MAX_LEN 16

#define MSG_FLAG_ACK 0x1
#define MSG_ACK_FLAG_DUPE 0x1

#define SUB_FLAG_ACK 0x1
#define SUB_FLAG_UNWRAP 0x2

#define CMD_SUB 0x1
#define CMD_ACK 0x2
#define CMD_UNSUB 0x3
#define CMD_UNSUB_ACK 0x4
#define CMD_SUB_MSG 0x5
#define CMD_SUB_MSG_ACK 0x6
#define CMD_MSG 0xA
#define CMD_MSG_ACK 0xB
#define CMD_PING 0x10
#define CMD_PONG 0x11

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

static int createPacket(uint8_t* buf, int bufSize, const char *userId, const char *key, uint16_t cmd, uint8_t flag, const uint64_t &nonce, const uint8_t *body, int bodyLen) {
  if (bufSize < UBSUB_CRYPTHEADER_LEN + UBSUB_HEADER_LEN + bodyLen + UBSUB_SIGNATURE_LEN) {
    // Buffer too short
    return -1;
  }

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
  this->init(userId, userKey, ubsubHost, ubsubPort);
}

Ubsub::Ubsub(const char *userId, const char *userKey) {
  this->init(userId, userKey, DEFAULT_UBSUB_ROUTER, DEFAULT_UBSUB_PORT);
}

void Ubsub::init(const char *userId, const char *userKey, const char *ubsubHost, const int ubsubPort) {
  this->userId = userId;
  this->userKey = userKey;
  this->host = ubsubHost;
  this->port = ubsubPort;
  this->socketInit = false;
  this->localPort = getNonce32() % 32768 + 32767;
  for (int i=0; i<UBSUB_ERROR_BUFFER_LEN; ++i) {
    this->lastError[i] = 0;
  }
  this->onLog = NULL;
  this->lastPong = 0;
  this->lastPing = 0;
  this->queue = NULL;
  this->autoRetry = true;
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


int Ubsub::publishEvent(const char *topicId, const char *topicKey, const char *msg) {
  if (topicId == NULL) {
    return UBSUB_MISSING_ARGS;
  }

  static uint8_t command[UBSUB_MTU];
  memset(command, 0, 64);
  memcpy(command, topicId, min(strlen(topicId), 32));
  if (topicKey != NULL) {
    memcpy(command+32, topicKey, min(strlen(topicKey), 32));
  }

  int msgLen = msg != NULL ? min(strlen(msg), UBSUB_MTU-64) : 0;
  if (msgLen > 0) {
    memcpy(command+64, msg, msgLen);
  }

  return this->sendCommand(CMD_MSG, MSG_FLAG_ACK, this->autoRetry, command, msgLen + 64);
}

void Ubsub::createTopic(const char *topicName) {
  const int COMMAND_LEN = 100;
  uint8_t command[COMMAND_LEN];
  memset(command, 0, COMMAND_LEN);

  *(uint16_t*)command = this->localPort;
  strncpy((char*)command+2, "HARDCODED_DID", 32);
  strncpy((char*)command+34, topicName, 32);
  strncpy((char*)command+66, "HARDCODED_SUB_ID", 32);
  *(uint16_t*)(command+98) = 60 * 5; // Max TTL (5 minutes)

  this->sendCommand(CMD_SUB, SUB_FLAG_ACK | SUB_FLAG_ACK, this->autoRetry, command, COMMAND_LEN);
}

void Ubsub::listenToTopic(const char *topicNameOrId, topicCallback callback) {

}

void Ubsub::createFunction(const char *name, topicCallback callback) {

}

int Ubsub::callFunction(const char *name, const char *arg) {
  return this->publishEvent(name, NULL, arg);
}

int Ubsub::callFunction(const char *name) {
  return this->callFunction(name, NULL);
}

const int Ubsub::getLastError() {
  const int err = this->lastError[0];
  for (int i=0; i<UBSUB_ERROR_BUFFER_LEN-1; ++i) {
    this->lastError[i] = this->lastError[i+1];
  }
  this->lastError[UBSUB_ERROR_BUFFER_LEN-1] = 0;
  return err;
}

void Ubsub::setError(const int err) {
  // Shift errors up and set error at 0
  for (int i=UBSUB_ERROR_BUFFER_LEN-1; i>0; --i) {
    this->lastError[i] = this->lastError[i-1];
  }
  this->lastError[0] = err;

  char logbuf[128];
  sprintf(logbuf, "Error code: %d", err);
  this->log("ERROR", logbuf);
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
  // Send ping if necessary
  uint64_t now = getTime();
  if (now - this->lastPing >= UBSUB_PING_FREQ) {
    this->lastPing = now;
    this->ping();
  }

  if (this->lastPong > 0 && now - this->lastPong > UBSUB_CONNECTION_TIMEOUT) {
    // TODO: Implement re-subscribe/re-connect
    this->log("WARN", "Haven't received pong.. lost connection?");
  }

  // Receive and process data
  this->receiveData();

  // Process queued events
  this->processQueue();
}

int Ubsub::receiveData() {
  static uint8_t buf[UBSUB_MTU];
  int received = 0;
  int rlen = 0;

  while (true) {
    #if ARDUINO
      if (this->sock.parsePacket() > 0) {
        rlen = this->sock.read(buf, UBSUB_MTU);
      }
    #elif PARTICLE
      if (this->sock.parsePacket() > 0) {
        rlen = this->sock.read(buf, UBSUB_MTU);
      }
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
    this->setError(UBSUB_ERR_INVALID_PACKET);
    return;
  }

  uint8_t version = buf[0];
  if (version != 0x2 && version != 0x3) {
    this->setError(UBSUB_ERR_BAD_VERSION);
    return;
  }

  uint64_t nonce = *(uint64_t*)(buf+1);
  char userId[17];
  strncpy(userId, (char*)(buf+9), 16);

  if (strcmp(userId, this->userId) != 0) {
    this->setError(UBSUB_ERR_USER_MISMATCH);
    return;
  }

  // Test the signature
  Sha256.initHmac((uint8_t*)this->userKey, strlen(this->userKey));
  Sha256.write(buf, len - UBSUB_SIGNATURE_LEN);
  uint8_t* digest = Sha256.resultHmac();
  uint8_t* signature = buf + len - UBSUB_SIGNATURE_LEN;
  if (memcmp(digest, signature, 32) != 0) {
    this->setError(UBSUB_ERR_BAD_SIGNATURE);
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
    this->setError(UBSUB_ERR_TIMEOUT);
    return;
  }

  //TODO: Validate Nonce hasn't already been used (dupe)


  switch(cmd) {
    case CMD_PONG: // Pong
      this->log("INFO", "GOT PONG");
      this->lastPong = getTime();
      break;
    case CMD_MSG_ACK:
    {
      this->log("INFO", "GOT MSG ACK");
      if (flag & MSG_ACK_FLAG_DUPE) {
        this->log("WARN", "Msg ack was dupe");
      }
      uint64_t msgNonce = *(uint64_t*)body;
      this->removeQueue(msgNonce);
      break;
    }
    default:
      this->setError(UBSUB_ERR_BAD_REQUEST);
      break;
  }
}

QueuedMessage* Ubsub::queueMessage(uint8_t* buf, int bufLen, const uint64_t &nonce) {
  QueuedMessage *msg = (QueuedMessage*)malloc(sizeof(QueuedMessage));
  if (msg == NULL) {
    this->setError(UBSUB_ERR_MALLOC);
    return NULL;
  }

  msg->buf = (uint8_t*)malloc(bufLen);
  msg->bufLen = bufLen;
  msg->retryTime = getTime() + UBSUB_PACKET_RETRY_SECONDS;
  msg->retryNumber = 0;
  msg->cancelNonce = nonce;
  msg->next = this->queue;

  memcpy(msg->buf, buf, bufLen);

  this->queue = msg;

  this->log("INFO", "Queued for retry");

  return msg;
}

void Ubsub::removeQueue(const uint64_t &nonce) {
  this->log("INFO", "Removing from queue");

  QueuedMessage** prevNext = &this->queue;
  QueuedMessage* msg = this->queue;
  while(msg != NULL) {
    if (msg->cancelNonce == nonce) {
      this->log("INFO", "NONCE MATCHED!");
      *prevNext = msg->next;
      free(msg->buf);
      free(msg);
      break;
    }

    prevNext = &msg->next;
    msg = msg->next;
  }
}

void Ubsub::processQueue() {
  uint64_t now = getTime();

  QueuedMessage *msg = this->queue;
  while(msg != NULL) {
    if (now >= msg->retryTime) {
      this->log("INFO", "Retrying message");
      msg->retryTime = now + UBSUB_PACKET_RETRY_SECONDS;
      msg->retryNumber++;

      this->sendData(msg->buf, msg->bufLen);

      if (msg->retryNumber >= UBSUB_PACKET_RETRY_ATTEMPTS) {
        this->log("WARN", "Retried max times, timing out");
        this->removeQueue(msg->cancelNonce);
        return; // Pointer is no longer valid, abort so we don't get memory issues
      }
    }

    msg = msg->next;
  }
}

void Ubsub::ping() {
  uint8_t buf[2];
  *(uint16_t*)buf = (uint16_t)this->localPort;
  this->sendCommand(CMD_PING, 0x0, false, buf, 2);
}

int Ubsub::sendCommand(uint16_t cmd, uint8_t flag, bool retry, const uint8_t *command, int commandLen) {
  static uint8_t buf[UBSUB_MTU];
  uint64_t nonce = getNonce64();
  int plen = createPacket(buf, UBSUB_MTU, this->userId, this->userKey, cmd, flag, nonce, command, commandLen);
  if (plen < 0) {
    this->setError(UBSUB_ERR_SEND);
    return -1;
  }

  if (retry) {
    this->queueMessage(buf, plen, nonce);
  }

  return this->sendData(buf, plen);
}

int Ubsub::sendData(const uint8_t* buf, int bufSize) {
  if (bufSize > UBSUB_MTU) {
    this->setError(UBSUB_ERR_EXCEEDS_MTU);
    return -1;
  }
  if (!this->socketInit) {
    this->setError(UBSUB_ERR_NETWORK);
    return -1;
  }

  #if ARDUINO
    if (this->sock.beginPacket(this->host, this->port) != 1)
      return -1;
    if (this->sock.write(buf, bufSize) != bufSize)
      return -1;
    if (this->sock.endPacket() != 1)
      return -1;
    return bufSize;
  #elif PARTICLE
    this->sock.beginPacket(this->host, this->port);
    this->sock.write(buf, bufSize);
    return this->sock.endPacket();
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
      this->setError(UBSUB_ERR_SEND);
      return -1;
    }
    return ret;
  #endif
}

void Ubsub::initSocket() {
  if (this->socketInit)
    return;

  #if ARDUINO
    this->sock.begin(this->localPort);
  #elif PARTICLE
    this->sock.begin(this->localPort);
    this->sock.setBuffer(UBSUB_MTU);
  #else
    this->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (this->sock < 0) {
      this->setError(UBSUB_ERR_SOCKET);
      return;
    }

    struct sockaddr_in bindAddr;
    memset((char*)&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(this->localPort);

    if (bind(this->sock, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
      this->setError(UBSUB_ERR_SOCKET_BIND);
      this->closeSocket();
      return;
    }

    if (fcntl(this->sock, F_SETFL, fcntl(this->sock, F_GETFL) | O_NONBLOCK) < 0) {
      this->setError(UBSUB_ERR_SOCKET_BIND);
      this->closeSocket();
      return;
    }
  #endif

  this->socketInit = true;
}

void Ubsub::closeSocket() {
  if (this->socketInit) {
    #if ARDUINO
      this->sock.stop();
    #elif PARTICLE
      this->sock.stop();
    #else
      close(this->sock);
      this->sock = -1;
    #endif
    this->socketInit = false;
  }
}
