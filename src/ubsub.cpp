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
#define SUB_FLAG_MSG_NEED_ACK 0x4

#define CMD_SUB 0x1
#define CMD_SUB_ACK 0x2
#define CMD_UNSUB 0x3
#define CMD_UNSUB_ACK 0x4
#define CMD_SUB_MSG 0x5
#define CMD_SUB_MSG_ACK 0x6
#define CMD_MSG 0xA
#define CMD_MSG_ACK 0xB
#define CMD_PING 0x10
#define CMD_PONG 0x11

#ifdef UBSUB_LOG
  #if !(ARDUINO || PARTICLE)
    #include <iostream>
    #include <stdarg.h>
  #else
    #warning Logs are enabled. This will result in more string memory being consumed
  #endif

  static void log(const char* level, const char* msg, ...) {
    static char logbuf[256];
    va_list argptr;
    va_start(argptr, msg);
    vsprintf(logbuf, msg, argptr);
    va_end(argptr);
    #if ARDUINO || PARTICLE
      Serial.printf("[%s] %s", level, logbuf);
      Serial.println();
    #else
      std::cerr << "[" << level << "] " << logbuf << std::endl;
    #endif
  }
#endif

static char* getDeviceId();
static int createPacket(uint8_t* buf, int bufSize, const char *userId, const char *key, uint16_t cmd, uint8_t flag, const uint64_t &nonce, const uint8_t *body, int bodyLen);
static uint64_t getTime();
static uint32_t getNonce32();
static uint64_t getNonce64();
static int min(int left, int right);

// Like strncpy, but null-terminates. dst should be maxLen+1 for null term
static int strfixedcopy(char* dst, const char *src, int maxLen);

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
  for (int i=0; i<UBSUB_NONCE_RR_COUNT; ++i) {
    this->rrnonce[i] = 0;
  }
  this->lastNonceIdx = 0;
  this->lastPong = 0;
  this->lastPing = 0;
  this->queue = NULL;
  this->autoRetry = true;
  this->deviceId = getDeviceId();
  this->initSocket();

  #ifdef UBSUB_LOG
  log("INFO", "DID: %s", this->deviceId);
  #endif
}

bool Ubsub::connect(int timeout) {
  this->lastPong = 0;
  uint64_t start = getTime();
  while(getTime() < start + timeout) {
    #ifdef UBSUB_LOG
    log("DEBUG", "Attempting connect...");
    #endif
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

void Ubsub::createTopic(const char *topicName, bool subscribe) {
  const int COMMAND_LEN = 68;
  uint8_t command[COMMAND_LEN];
  memset(command, 0, COMMAND_LEN);

  *(uint16_t*)command = this->localPort;
  strncpy((char*)command+2, topicName, 32); // Don't make strfixedcopy (Don't want to null term here)
  if (subscribe)
    strncpy((char*)command+34, this->deviceId, 32);
  *(uint16_t*)(command+66) = UBSUB_SUBSCRIPTION_TTL; // Max TTL (5 minutes)

  this->sendCommand(CMD_SUB, SUB_FLAG_ACK | SUB_FLAG_UNWRAP, this->autoRetry, command, COMMAND_LEN);
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

void Ubsub::processEvents() {
  // Send ping if necessary
  uint64_t now = getTime();
  if (now - this->lastPing >= UBSUB_PING_FREQ) {
    this->lastPing = now;
    this->ping();
  }

  if (this->lastPong > 0 && now - this->lastPong > UBSUB_CONNECTION_TIMEOUT) {
    // TODO: Implement re-subscribe/re-connect
    #ifdef UBSUB_LOG
    log("WARN", "Haven't received pong.. lost connection?");
    #endif
  }

  // Receive and process data
  this->receiveData();

  // Process queued events
  this->processQueue();
}





// PRIVATE methods

void Ubsub::setError(const int err) {
  // Shift errors up and set error at 0
  for (int i=UBSUB_ERROR_BUFFER_LEN-1; i>0; --i) {
    this->lastError[i] = this->lastError[i-1];
  }
  this->lastError[0] = err;
  #ifdef UBSUB_LOG
  log("ERROR", "Error code: %d", err);
  #endif
}

void Ubsub::processPacket(uint8_t *buf, int len) {
  #ifdef UBSUB_LOG
  log("INFO", "Got %d bytes of data", len);
  #endif
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
  strfixedcopy(userId, (char*)(buf+9), 16);

  if (strcmp(userId, this->userId) != 0) {
    this->setError(UBSUB_ERR_USER_MISMATCH);
    return;
  }

  //Validate Nonce hasn't already been used (dupe)
  if (this->hasNonce(nonce)) {
    this->setError(UBSUB_ERR_NONCE_DUPE);
    return;
  }
  this->writeNonce(nonce);

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
  uint64_t now = getTime();
  int diff = (int64_t)now - (int64_t)ts; // Signed cause could be negative
  if (diff < -UBSUB_PACKET_TIMEOUT || diff > UBSUB_PACKET_TIMEOUT) {
    this->setError(UBSUB_ERR_TIMEOUT);
    return;
  }

  #ifdef UBSUB_LOG
  log("INFO", "Received command %d with %d byte command. flag: %d", cmd, bodyLen, flag);
  #endif

  switch(cmd) {
    case CMD_PONG: // Pong
    {
      if (bodyLen < 8) {
        this->setError(UBSUB_ERR_BAD_REQUEST);
        return;
      }
      #ifdef UBSUB_LOG
      uint64_t pingTime = *(uint64_t*)body;
      int64_t roundTrip = (int64_t)now - (int64_t)pingTime;
      log("DEBUG", "Got pong. Round trip secs: %d", roundTrip);
      #endif
      if (now > this->lastPong) {
        this->lastPong = now;
      }
      break;
    }
    case CMD_SUB_ACK:
    {
      if (bodyLen < 72) {
        this->setError(UBSUB_ERR_BAD_REQUEST);
        return;
      }
      char topicId[17];
      char subId[17];
      char subKey[33];
      uint64_t ackNonce = *(uint64_t*)(body+0);
      strfixedcopy(topicId, (char*)body+8, 16);
      strfixedcopy(subId, (char*)body+24, 16);
      strfixedcopy(subKey, (char*)body+40, 32);

      this->removeQueue(ackNonce);
      #ifdef UBSUB_LOG
      log("DEBUG", "Received subscription ack for topic %s: %s key %s", topicId, subId, subKey);
      #endif
      break;
    }
    case CMD_SUB_MSG:
    {
      if (bodyLen < 48) {
        this->setError(UBSUB_ERR_BAD_REQUEST);
        return;
      }
      char subscriptionId[17];
      char subscriptionKey[33];
      char event[UBSUB_MTU-48+1];
      strfixedcopy(subscriptionId, (char*)body, 16);
      strfixedcopy(subscriptionKey, (char*)body+16, 32);
      strfixedcopy(event, (char*)body+48, bodyLen - 48);

      #ifdef UBSUB_LOG
      log("INFO", "Received event from subscription %s with key %s: %s", subscriptionId, subscriptionKey, event);
      #endif
      break;
    }
    case CMD_MSG_ACK:
    {
      if (bodyLen < 8) {
        this->setError(UBSUB_ERR_BAD_REQUEST);
        return;
      }
      uint64_t msgNonce = *(uint64_t*)body;
      #ifdef UBSUB_LOG
        log("INFO", "Got message ack for %d", msgNonce);
        if (flag & MSG_ACK_FLAG_DUPE) {
          log("WARN", "Msg ack was dupe");
        }
      #endif
      this->removeQueue(msgNonce);
      break;
    }
    default:
      #ifdef UBSUB_LOG
      log("WARN", "Unrecognized command: %d", cmd);
      #endif
      this->setError(UBSUB_ERR_BAD_REQUEST);
      break;
  }
}

QueuedMessage* Ubsub::queueMessage(const uint8_t* buf, int bufLen, const uint64_t &nonce) {
  QueuedMessage *msg = (QueuedMessage*)malloc(sizeof(QueuedMessage));
  if (msg == NULL) {
    this->setError(UBSUB_ERR_MALLOC);
    return NULL;
  }

  msg->buf = (uint8_t*)malloc(bufLen);
  if (msg->buf == NULL) {
    this->setError(UBSUB_ERR_MALLOC);
    free(msg);
    return NULL;
  }
  msg->bufLen = bufLen;
  msg->retryTime = getTime() + UBSUB_PACKET_RETRY_SECONDS;
  msg->retryNumber = 0;
  msg->cancelNonce = nonce;
  msg->next = this->queue;

  memcpy(msg->buf, buf, bufLen);

  this->queue = msg;

  #ifdef UBSUB_LOG
  log("INFO", "Queued %d bytes with nonce %d for retry", bufLen, nonce);
  #endif

  return msg;
}

void Ubsub::removeQueue(const uint64_t &nonce) {
  QueuedMessage** prevNext = &this->queue;
  QueuedMessage* msg = this->queue;
  while(msg != NULL) {
    if (msg->cancelNonce == nonce) {
      #ifdef UBSUB_LOG
      log("INFO", "Removing %d from queue", nonce);
      #endif
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
      #ifdef UBSUB_LOG
      log("INFO", "Retrying message %d", msg->cancelNonce);
      #endif
      msg->retryTime = now + UBSUB_PACKET_RETRY_SECONDS;
      msg->retryNumber++;

      this->sendData(msg->buf, msg->bufLen);

      if (msg->retryNumber >= UBSUB_PACKET_RETRY_ATTEMPTS) {
        #ifdef UBSUB_LOG
        log("WARN", "Retried max times, timing out");
        #endif
        this->removeQueue(msg->cancelNonce);
        return; // Pointer is no longer valid, abort so we don't get memory issues
      }
    }

    msg = msg->next;
  }
}

void Ubsub::writeNonce(const uint64_t &nonce) {
  this->rrnonce[this->lastNonceIdx] = nonce;
  this->lastNonceIdx = (this->lastNonceIdx + 1) % UBSUB_NONCE_RR_COUNT;
}

bool Ubsub::hasNonce(const uint64_t &nonce) {
  for (int i=0; i<UBSUB_NONCE_RR_COUNT; ++i) {
    if (this->rrnonce[i] == nonce)
      return true;
  }
  return false;
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



// PRIVATE multiplatform socket code

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
    #ifdef UBSUB_LOG
    log("DEBUG", "Sending %d bytes to host...", bufSize);
    #endif

    struct hostent *server;
    server = gethostbyname(this->host);
    if (server == NULL) {
      #ifdef UBSUB_LOG
      log("WARN", "Failed to resolve hostname %s. Connected?", this->host);
      #endif
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


// STATIC HELPERS ===============

// Gets a static pointer to a cstr deviceId
static char* getDeviceId() {
  #define STR_HELPER(x) #x
  #define STR(x) STR_HELPER(x)

  #if PARTICLE
    static char did[32];
    System.deviceID().to_cstr(did, 32);
  #elif __COUNTER__
    static char did[] = STR(__COUNTER__);
  #else
    static char did[] = "BDID:" __DATE__ " " __TIME__;
  #endif
  return did;
}

// Get time in seconds
static uint64_t getTime() {
#if ARDUINO
  return (uint64_t)(millis() / 1000);
#elif PARTICLE
  return Time.now();
#else
  return std::time(NULL);
#endif
}

// Get random nonce
static uint32_t getNonce32() {
#if ARDUINO || PARTICLE
  return (random(256) << 24) | (random(256) << 16) | (random(256) << 8) | random(256);
#else
  static bool init = false;
  if (!init) {
    srand(getTime()); //FIXME: This isn't great
    init = true;
  }
  return (uint32_t)rand() + (uint32_t)rand();
#endif
}

static uint64_t getNonce64() {
  return (uint64_t)getNonce32() << 32 | getNonce32();
}

static int min(int left, int right) {
  return left < right ? left : right;
}

static int strfixedcopy(char* dst, const char *src, int maxLen) {
  int n = 0;
  for (; n<maxLen; ++n) {
    char c = src[n];
    if (c == '\0')
      break;
    dst[n] = c;
  }
  dst[n] = '\0';
  return n;
}
