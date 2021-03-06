#include "ubsub.h"
#include "sha256.h"
#include "salsa20.h"
#include "binio.h"
#include "log.h"
#include "minijson.h"

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error Little endian ordering required for binary serialization
#endif

#if PARTICLE
  #include <Particle.h>
#elif ARDUINO
  #include <Arduino.h>
  #include <ESP8266WiFi.h>
  #include <WiFiUdp.h>
  #include <time.h>
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

const char* DEFAULT_UBSUB_ROUTER = "iot.ubsub.io";
const int DEFAULT_UBSUB_PORT = 4001;

const char* DEFAULT_NTP_SERVER = "pool.ntp.org";

// Generates outer & inner packet, running it through cryptography
#define UBSUB_CRYPTHEADER_LEN 25
#define UBSUB_HEADER_LEN 13
#define UBSUB_FULL_HEADER_LEN (UBSUB_CRYPTHEADER_LEN + UBSUB_HEADER_LEN)
#define UBSUB_SIGNATURE_LEN 32
#define DEVICE_ID_MAX_LEN 16

#define MSG_FLAG_ACK 0x1
#define MSG_FLAG_EXTERNAL 0x2
#define MSG_FLAG_CREATE 0x4
#define MSG_ACK_FLAG_DUPE 0x1

#define SUB_FLAG_ACK 0x1
#define SUB_FLAG_UNWRAP 0x2
#define SUB_FLAG_MSG_NEED_ACK 0x4

#define SUB_ACK_FLAG_DUPE 0x1
#define SUB_ACK_FLAG_TOPIC_NOT_EXIST 0x2

#define SUB_MSG_FLAG_ACK 0x1
#define SUB_MSG_FLAG_WAS_UNWRAPPED 0x2

#define SUB_MSG_ACK_FLAG_REJECTED 0x2

#define CMD_SUB         0x1
#define CMD_SUB_ACK     0x2
#define CMD_UNSUB       0x3
#define CMD_UNSUB_ACK   0x4
#define CMD_SUB_MSG     0x5
#define CMD_SUB_MSG_ACK 0x6
#define CMD_MSG         0xA
#define CMD_MSG_ACK     0xB
#define CMD_PING        0x10
#define CMD_PONG        0x11

#define FORMAT_STRING   0x1
#define FORMAT_INT      0x2
#define FORMAT_FLOAT    0x3


//static char* getUniqueDeviceId();
static int createPacket(uint8_t* buf, int bufSize, const char *deviceId, const char *key, uint16_t cmd, uint8_t flag, const uint64_t &nonce, const uint8_t *body, int bodyLen, const uint8_t *optData, int dataLen);
static uint64_t getTime();
static uint32_t getNonce32();
static uint64_t getNonce64();
static int min(int left, int right);

// Ubsub Implementation

Ubsub::Ubsub(const char *deviceId, const char *deviceKey, const char *ubsubHost, int ubsubPort) {
  this->init(deviceId, deviceKey, ubsubHost, ubsubPort);
}

Ubsub::Ubsub(const char *deviceId, const char *deviceKey) {
  this->init(deviceId, deviceKey, DEFAULT_UBSUB_ROUTER, DEFAULT_UBSUB_PORT);
}

void Ubsub::init(const char *deviceId, const char *deviceKey, const char *ubsubHost, const int ubsubPort) {
  this->deviceId = deviceId;
  this->deviceKey = deviceKey;
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
  this->subs = NULL;
  this->watch = NULL;

  this->autoSyncTime = true;
  this->lastTimeSync = 0;
  this->watchTopic[0] = '\0';

  US_LOG_INFO("DID: %s", this->deviceId);
}

Ubsub::~Ubsub() {
  SubscribedFunc *sfunc = this->subs;
  while(sfunc != NULL) {
    SubscribedFunc *curr = sfunc;
    sfunc = sfunc->next;
    free(curr);
  }

  QueuedMessage* qmsg = this->queue;
  while(qmsg != NULL) {
    QueuedMessage *curr = qmsg;
    qmsg = qmsg->next;
    free(curr->buf);
    free(curr);
  }
}

void Ubsub::enableAutoSyncTime(bool enabled) {
  this->autoSyncTime = enabled;
  this->lastTimeSync = 0;
}

void Ubsub::enableAutoRetry(bool enabled) {
  this->autoRetry = enabled;
}

bool Ubsub::connect(int timeout) {
  US_LOG_INFO("Ubsub connecting (local: %d)...", this->localPort);

  uint64_t timeoutTime = getTime() + timeout;

  while(true) {
    #ifdef ARDUINO
    if (WiFi.status() == WL_CONNECTED)
      break;
    delay(10);
    #elif PARTICLE
    if (WiFi.ready())
      break;
    delay(10);
    #else
    break;
    #endif
    if (getTime() > timeoutTime)
      return false;
  }

  // Sync time
  if (this->autoSyncTime) {
    this->syncTime(timeout);

    // Clock might be different now
    // Pick a new timeout time
    timeoutTime = getTime() + timeout;
  }

  this->initSocket();

  this->lastPong = 0;
  while(true) {
    US_LOG_DEBUG("Attempting connect...");
    this->ping();

    // Wait for pong
    uint64_t waitEnd = getTime() + 1;
    while(getTime() < waitEnd && this->lastPong <= 0) {
      this->receiveData();
      #if ARDUINO || PARTICLE
      delay(10);
      #endif
    }

    if (this->lastPong > 0) {
      // Got ping
      break;
    }

    if (getTime() > timeoutTime) {
      return false;
    }
  }

  US_LOG_INFO("Connection established");
  return true;
}


int Ubsub::publishEvent(const char *topicNameOrId, const char *topicKey, const char *msg) {
  if (topicNameOrId == NULL) {
    return UBSUB_MISSING_ARGS;
  }

  const int COMMAND_LEN = 66;
  uint8_t command[COMMAND_LEN];
  memset(command, 0, COMMAND_LEN);
  *(uint16_t*)command = this->localPort;
  pushstr(command+2, topicNameOrId, 32);
  if (topicKey != NULL) {
    pushstr(command+34, topicKey, 32);
  }

  int msgLen = msg != NULL ? min(strlen(msg), UBSUB_MTU-COMMAND_LEN) : 0;

  US_LOG_INFO("Publishing message to topic %s with %d bytes...", topicNameOrId, msgLen);

  uint8_t flag = MSG_FLAG_CREATE;
  if (this->autoRetry)
    flag |= MSG_FLAG_ACK;

  return this->sendCommand(CMD_MSG, flag, this->autoRetry, getNonce64(), command, COMMAND_LEN, (uint8_t*)msg, msgLen);
}

int Ubsub::publishEvent(const char* topicNameOrId, const char* msg) {
  return this->publishEvent(topicNameOrId, NULL, msg);
}

void Ubsub::listenToTopic(const char *topicNameOrId, TopicCallback callback) {
  const int COMMAND_LEN = 44;
  uint8_t command[COMMAND_LEN];
  memset(command, 0, COMMAND_LEN);

  uint64_t funcId = getNonce64();

  write_le<uint16_t>(command+0, this->localPort);
  pushstr(command+2, topicNameOrId, 32);
  write_le<uint64_t>(command+34, funcId);
  write_le<uint16_t>(command+42, UBSUB_SUBSCRIPTION_TTL);

  // Register subscription in LL
  SubscribedFunc* sub = (SubscribedFunc*)malloc(sizeof(SubscribedFunc));
  memset(sub, 0, sizeof(SubscribedFunc));
  strncpy(sub->topicNameOrId, topicNameOrId, 16);
  sub->callback = callback;
  sub->next = this->subs;
  sub->funcId = funcId;
  sub->requestNonce = getNonce64();
  sub->renewTime = getTime() + 5; // Retry frequenctly. Ack will push this out
  this->subs = sub;

  US_LOG_INFO("Listening to '%s' with funcId 0x%s...", topicNameOrId, tohexstr(funcId));

  this->sendCommand(
    CMD_SUB,
    SUB_FLAG_ACK | SUB_FLAG_UNWRAP | SUB_FLAG_MSG_NEED_ACK,
    this->autoRetry,
    sub->requestNonce,
    command,
    COMMAND_LEN,
    NULL, 0);
}

void Ubsub::createFunction(const char *name, TopicCallback callback) {
  this->listenToTopic(name, callback);
}

int Ubsub::callFunction(const char *name, const char *arg) {
  return this->publishEvent(name, NULL, arg);
}

int Ubsub::callFunction(const char *name) {
  return this->callFunction(name, NULL);
}

void Ubsub::setWatchTopic(const char *topicNameOrId) {
  strncpy(this->watchTopic, topicNameOrId, sizeof(this->watchTopic)-1);
}

void Ubsub::watchVariable(const char *name, const void* ptr, int len, uint8_t format) {
  VariableWatch* watch = (VariableWatch*)malloc(sizeof(VariableWatch));
  strncpy(watch->name, name, sizeof(watch->name)-1);
  watch->format = format;
  watch->ptr = (uint8_t*)ptr;
  watch->len = len;
  watch->lastCheck = 0;

  watch->next = this->watch;
  this->watch = watch;

  US_LOG_INFO("Watching variable %s at %p (size: %d)...", name, ptr, len);
}

void Ubsub::watchVariable(const char *name, const char *s, int maxLen) {
  this->watchVariable(name, s, maxLen, FORMAT_STRING);
}

void Ubsub::watchVariable(const char *name, const int *val) {
  this->watchVariable(name, val, sizeof(int), FORMAT_INT);
}

void Ubsub::watchVariable(const char *name, const float *val) {
  this->watchVariable(name, val, sizeof(float), FORMAT_FLOAT);
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
  // Kick off time sync if needed
  if (this->autoSyncTime && getTime() >= this->lastTimeSync + UBSUB_TIME_SYNC_FREQ) {
    this->syncTime();
  }

  // Send ping if necessary
  // If we don't have any subs, no reason to constantly ping, as it's mostly for NAT negotiation
  if (this->subs != NULL) {
    uint64_t now = getTime();
    if (now - this->lastPing >= UBSUB_PING_FREQ) {
      this->lastPing = now;
      this->ping();
    }

    if (this->lastPong > 0 && now - this->lastPong > UBSUB_CONNECTION_TIMEOUT) {
      US_LOG_WARN("Haven't received pong.. lost connection?");
      // Attempt reconnection
      this->invalidateSubscriptions();
      this->connect();
    }

    // Renew any subscriptions that need it
    this->renewSubscriptions();
  }

  // Receive and process data
  this->receiveData();

  // Process queued events
  this->processQueue();

  // Watch variables for changes
  this->checkWatchedVariables();
}

int Ubsub::getQueueSize() {
  int count = 0;
  QueuedMessage* msg = this->queue;
  while(msg != NULL) {
    count++;
    msg = msg->next;
  }
  return count;
}

void Ubsub::flush(int timeout) {
  US_LOG_DEBUG("Waiting for flush...");

  uint64_t timeoutTime = getTime() + timeout;
  while(this->getQueueSize() > 0 && (timeout < 0 || getTime() <= timeoutTime) ) {
    this->processEvents();
    #if ARDUINO || PARTICLE
    delay(10); // Yield to device
    #endif
  }

  US_LOG_DEBUG("Flushed");
}





// PRIVATE methods

void Ubsub::setError(const int err) {
  // Shift errors up and set error at 0
  for (int i=UBSUB_ERROR_BUFFER_LEN-1; i>0; --i) {
    this->lastError[i] = this->lastError[i-1];
  }
  this->lastError[0] = err;
  US_LOG_ERROR("Error code: %d", err);
}

void Ubsub::processPacket(uint8_t *buf, int len) {
  US_LOG_DEBUG("Got %d bytes of data", len);
  if (len < UBSUB_HEADER_LEN + UBSUB_CRYPTHEADER_LEN + UBSUB_SIGNATURE_LEN) {
    this->setError(UBSUB_ERR_INVALID_PACKET);
    return;
  }

  uint8_t version = buf[0];
  if (version != 0x2 && version != 0x3) {
    this->setError(UBSUB_ERR_BAD_VERSION);
    return;
  }

  uint64_t nonce = read_le<uint64_t>(buf+1);
  char deviceId[17];
  pullstr(deviceId, buf+9, 16);

  if (strcmp(deviceId, this->deviceId) != 0) {
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
  Sha256.initHmac((uint8_t*)this->deviceKey, strlen(this->deviceKey));
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
    Sha256.write((uint8_t*)this->deviceKey, strlen(this->deviceKey));
    uint8_t* expandedKey = Sha256.result();
    s20_crypt(expandedKey, S20_KEYLEN_256, (uint8_t*)&nonce, 0, buf+25, len - UBSUB_CRYPTHEADER_LEN - UBSUB_SIGNATURE_LEN);
  }

  uint64_t ts = read_le<uint64_t>(buf+25);
  uint16_t cmd = read_le<uint16_t>(buf+33);
  uint16_t bodyLen = read_le<uint16_t>(buf+35);
  uint8_t flag = *(uint8_t*)(buf+37);

  uint8_t* body = buf + 38;

  // Validate timestamp is within bounds
  uint64_t now = getTime();
  int diff = (int64_t)now - (int64_t)ts; // Signed cause could be negative
  if (diff < -UBSUB_PACKET_TIMEOUT || diff > UBSUB_PACKET_TIMEOUT) {
    this->setError(UBSUB_ERR_TIMEOUT);
    return;
  }

  processCommand(cmd, flag, nonce, body, bodyLen);
}

void Ubsub::processCommand(uint16_t cmd, uint8_t flag, const uint64_t &nonce, const uint8_t* body, int bodyLen) {
  US_LOG_DEBUG("Received command %d with %d byte command. flag: %d", cmd, bodyLen, flag);

  uint64_t now = getTime();

  switch(cmd) {
    case CMD_PONG: // Pong
    {
      if (bodyLen < 8) {
        this->setError(UBSUB_ERR_BAD_REQUEST);
        return;
      }
      #ifdef UBSUB_US_LOG_DEBUG
      uint64_t pingTime = read_le<uint64_t>(body);
      int32_t roundTrip = (int32_t)((int64_t)now - (int64_t)pingTime);
      US_LOG_DEBUG("Got pong. Round trip secs: %d", roundTrip);
      #endif
      if (now > this->lastPong) {
        this->lastPong = now;
      }
      break;
    }
    case CMD_SUB_ACK:
    {
      if (bodyLen < 88) {
        this->setError(UBSUB_ERR_BAD_REQUEST);
        return;
      }
      uint64_t ackNonce = read_le<uint64_t>(body+0);

      if (!(flag & SUB_ACK_FLAG_TOPIC_NOT_EXIST)) {
        SubscribedFunc* sub = this->getSubscribedFuncByNonce(ackNonce);
        if (sub != NULL) {
          sub->requestNonce = 0;
          pullstr(sub->topicNameOrId, body+16, 16);
          pullstr(sub->subscriptionId, body+32, 16);
          pullstr(sub->subscriptionKey, body+48, 32);
          sub->renewTime = read_le<uint64_t>(body+80);

          US_LOG_INFO("Received subscription ack for func 0x%s topic %s: %s key %s", tohexstr(sub->funcId), sub->topicNameOrId, sub->subscriptionId, sub->subscriptionKey);
        } else {
          US_LOG_WARN("Received sub ack for unknown subscription 0x%s", tohexstr(ackNonce));
        }
      } else {
        US_LOG_WARN("Topic does not exist on server and did not create for nonce 0x%s", tohexstr(ackNonce));
      }

      this->removeQueue(ackNonce);
      break;
    }
    case CMD_SUB_MSG:
    {
      if (bodyLen < 40) {
        this->setError(UBSUB_ERR_BAD_REQUEST);
        return;
      }
      char subscriptionKey[33];
      char event[UBSUB_MTU-48+1];
      uint64_t funcId = read_le<uint64_t>(body+0);
      pullstr(subscriptionKey, body+8, 32);
      pullstr(event, body+40, bodyLen - 40);

      US_LOG_INFO("Received event from func 0x%s with key %s: %s", tohexstr(funcId), subscriptionKey, event);

      // Ack data, if requested. Defer sending until we know flag
      uint8_t msgAck[8];
      write_le(msgAck, nonce);

      // Call correct function to notify a message has arrived
      SubscribedFunc* sub = this->getSubscribedFuncByFuncId(funcId);
      if (sub != NULL && strcmp(sub->subscriptionKey, subscriptionKey) == 0) {
        // Send ack before processing  in case slow
        if (flag & SUB_MSG_FLAG_ACK)
          this->sendCommand(CMD_SUB_MSG_ACK, 0x0, false, msgAck, sizeof(msgAck));

        if (sub->callback != NULL)
          sub->callback(event);

      } else if (sub != NULL) {
        if (flag & SUB_MSG_FLAG_ACK)
          this->sendCommand(CMD_SUB_MSG_ACK, SUB_MSG_ACK_FLAG_REJECTED, false, msgAck, sizeof(msgAck));
        US_LOG_WARN("Received subscription message, but keys don't match: %s != %s", sub->subscriptionKey, subscriptionKey);
      } else {
        if (flag & SUB_MSG_FLAG_ACK)
          this->sendCommand(CMD_SUB_MSG_ACK, SUB_MSG_ACK_FLAG_REJECTED, false, msgAck, sizeof(msgAck));
        US_LOG_WARN("Received subscription message for unknown func 0x%s", tohexstr(funcId));
      }

      break;
    }
    case CMD_MSG_ACK:
    {
      if (bodyLen < 8) {
        this->setError(UBSUB_ERR_BAD_REQUEST);
        return;
      }
      uint64_t msgNonce = read_le<uint64_t>(body);
      US_LOG_INFO("Got message ack for 0x%s", tohexstr(msgNonce));
      if (flag & MSG_ACK_FLAG_DUPE) {
        US_LOG_WARN("Msg ack was dupe");
      }
      this->removeQueue(msgNonce);
      break;
    }
    default:
      US_LOG_WARN("Unrecognized command: %d", cmd);
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

  US_LOG_DEBUG("Queued %d bytes with nonce 0x%s for retry", bufLen, tohexstr(nonce));

  return msg;
}

void Ubsub::removeQueue(const uint64_t &nonce) {
  QueuedMessage** prevNext = &this->queue;
  QueuedMessage* msg = this->queue;
  while(msg != NULL) {
    if (msg->cancelNonce == nonce) {
      US_LOG_DEBUG("Removing 0x%s from queue", tohexstr(nonce));
      *prevNext = msg->next;
      free(msg->buf);
      free(msg);
      return;
    }

    prevNext = &msg->next;
    msg = msg->next;
  }

  US_LOG_DEBUG("Unable to remove 0x%s from queue, not found", tohexstr(nonce));
}

void Ubsub::processQueue() {
  uint64_t now = getTime();

  QueuedMessage *msg = this->queue;
  while(msg != NULL) {
    if (now >= msg->retryTime) {
      US_LOG_INFO("Retrying message 0x%s", tohexstr(msg->cancelNonce));
      msg->retryTime = now + UBSUB_PACKET_RETRY_SECONDS;
      msg->retryNumber++;

      this->sendData(msg->buf, msg->bufLen);

      if (msg->retryNumber >= UBSUB_PACKET_RETRY_ATTEMPTS) {
        US_LOG_WARN("Retried max times, timing out");
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
  write_le<uint16_t>(buf+0, this->localPort);
  this->sendCommand(CMD_PING, 0x0, false, buf, 2);
}

SubscribedFunc* Ubsub::getSubscribedFuncByNonce(const uint64_t &nonce) {
  SubscribedFunc* sub = this->subs;
  while (sub != NULL) {
    if (sub->requestNonce == nonce)
      return sub;
    sub = sub->next;
  }
  return NULL;
}

SubscribedFunc* Ubsub::getSubscribedFuncByFuncId(const uint64_t &funcId) {
  SubscribedFunc* sub = this->subs;
  while (sub != NULL) {
    if (sub->funcId == funcId)
      return sub;
    sub = sub->next;
  }
  return NULL;
}

void Ubsub::invalidateSubscriptions() {
  SubscribedFunc *sub = this->subs;
  while (sub != NULL) {
    sub->renewTime = 0;
    sub = sub->next;
  }
}

void Ubsub::renewSubscriptions() {
  uint64_t now = getTime();
  SubscribedFunc *sub = this->subs;
  while (sub != NULL) {
    if (now >= sub->renewTime) {
      // Renew
      US_LOG_INFO("Renewing subscription to %s...", sub->topicNameOrId);

      sub->requestNonce = getNonce64();
      sub->renewTime = now + 5;

      const int COMMAND_LEN = 44;
      uint8_t command[COMMAND_LEN];
      memset(command, 0, COMMAND_LEN);

      write_le<uint16_t>(command+0, this->localPort);
      pushstr(command+2, sub->topicNameOrId, 32);
      write_le<uint64_t>(command+34, sub->funcId);
      write_le<uint16_t>(command+42, UBSUB_SUBSCRIPTION_TTL);

      this->sendCommand(
        CMD_SUB,
        SUB_FLAG_ACK | SUB_FLAG_UNWRAP | SUB_FLAG_MSG_NEED_ACK,
        this->autoRetry,
        sub->requestNonce,
        command,
        COMMAND_LEN,
        NULL, 0);
    }
    sub = sub->next;
  }
}

static uint32_t hash32(const uint8_t* data, int len) {
  uint32_t hash = 0;
  const uint8_t* p = data + len;
  while( p-- != data )
    hash ^= (hash << 5) + (hash >> 2) + *p;
  return hash;
}

void Ubsub::checkWatchedVariables() {
  uint64_t now = getTime();
  
  char buf[128]; // stack buffer
  MiniJsonBuilder json(buf, sizeof(buf));
  json.open();

  VariableWatch *watch = this->watch;
  while(watch != NULL) {
    if (now >= watch->lastCheck + UBSUB_WATCH_CHECK_FREQ) {
      watch->lastCheck = now;

      // Compute hash
      uint32_t hash = hash32(watch->ptr, watch->len);
      if (hash != watch->hash) {
        // There was an update!
        US_LOG_INFO("Detected change in variable %s, updating...", watch->name);
        watch->hash = hash;

        if (watch->format == FORMAT_STRING) {
          json.write(watch->name, (char*)watch->ptr);
        } else if (watch->format == FORMAT_INT) {
          json.write(watch->name, *(int*)watch->ptr);
        } else if (watch->format == FORMAT_FLOAT) {
          json.write(watch->name, *(float*)watch->ptr);
        } else {
          US_LOG_WARN("Unable to send watched variable, unknown variable %d", watch->format);
        }
      }
    }

    watch = watch->next;
  }

  if (json.items() > 0) {
    json.close();
    if (strlen(this->watchTopic) > 0)
      this->callFunction(this->watchTopic, json.c_str());
    else
      this->callFunction("watches", json.c_str());
  }
}


int Ubsub::sendCommand(uint16_t cmd, uint8_t flag, bool retry, const uint64_t &nonce, const uint8_t *command, int commandLen, const uint8_t* optData, int dataLen) {
  static uint8_t buf[UBSUB_MTU];
  int plen = createPacket(buf, UBSUB_MTU, this->deviceId, this->deviceKey, cmd, flag, nonce, command, commandLen, optData, dataLen);
  if (plen < 0) {
    this->setError(UBSUB_ERR_SEND);
    return -1;
  }

  if (retry) {
    this->queueMessage(buf, plen, nonce);
  }

  return this->sendData(buf, plen);
}

int Ubsub::sendCommand(uint16_t cmd, uint8_t flag, bool retry, const uint8_t *command, int commandLen) {
  uint64_t nonce = getNonce64();
  return this->sendCommand(cmd, flag, retry, nonce, command, commandLen, NULL, 0);
}

int Ubsub::sendCommand(uint16_t cmd, uint8_t flag, const uint8_t *command, int commandLen) {
  return this->sendCommand(cmd, flag, this->autoRetry, command, commandLen);
}


// PRIVATE multiplatform socket code

int Ubsub::receiveData() {
  if (!this->socketInit) {
    this->setError(UBSUB_ERR_NETWORK);
    return 0;
  }

  static uint8_t buf[UBSUB_MTU];
  int received = 0;

  while (true) {
    int rlen = -1;

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
      socklen_t fromlen = 0;
      rlen = recvfrom(this->sock, buf, UBSUB_MTU, 0x0, (struct sockaddr*)&from, &fromlen);
    #endif

    if (rlen < 0)
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

  US_LOG_DEBUG("Sending %d bytes to host %s:%d...", bufSize, this->host, this->port);

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
    struct hostent *server;
    server = gethostbyname(this->host);
    if (server == NULL) {
      US_LOG_WARN("Failed to resolve hostname %s. Connected?", this->host);
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

void Ubsub::syncTime(int timeout) {
  US_LOG_INFO("Synchronizing time...");

  #ifdef PARTICLE
    uint64_t timeoutTime = getTime() + timeout;
    Particle.syncTime();
    while(Particle.syncTimePending() && getTime() < timeoutTime) delay(50);
  #elif ARDUINO
    uint64_t timeoutTime = getTime() + timeout;
    configTime(0, 0, DEFAULT_NTP_SERVER);
    while(!time(NULL) && getTime() < timeoutTime) delay(50);
  #else
    US_LOG_WARN("Time syncing not supported on this platform");
  #endif

  this->lastTimeSync = getTime();
}

static int createPacket(uint8_t* buf, int bufSize, const char *deviceId, const char *key, uint16_t cmd, uint8_t flag, const uint64_t &nonce,
    const uint8_t *body, int bodyLen, const uint8_t *optData, int dataLen) {

  if (bufSize < UBSUB_CRYPTHEADER_LEN + UBSUB_HEADER_LEN + bodyLen + dataLen + UBSUB_SIGNATURE_LEN) {
    // Buffer too short
    return -1;
  }
  #if DEBUG
  if (bodyLen < 0)
    return -1;
  if (dataLen < 0)
    return -1;
  #endif

  uint64_t ts = getTime();

  const int deviceIdLen = strlen(deviceId);
  if (deviceIdLen > DEVICE_ID_MAX_LEN) {
    return -2;
  }

  // Zero the buffer
  memset(buf, 0, bufSize);

  // Set up CrpyHeader
  buf[0] = 0x3; // UDPv3 (encrypted with salsa20)
  write_le<uint64_t>(buf+1, nonce); // 64 bit nonce
  memcpy(buf+9, deviceId, deviceIdLen);

  // Set header
  int fullDataLength = bodyLen + dataLen;
  write_le<uint64_t>(buf+25, ts);
  write_le<uint16_t>(buf+33, cmd);
  write_le<uint16_t>(buf+35, (uint16_t)fullDataLength);
  *(uint8_t*)(buf+37) = flag;

  // Copy body to buffer
  if (body != NULL && bodyLen > 0) {
    memcpy(buf+UBSUB_FULL_HEADER_LEN, body, bodyLen);
  }
  if (optData != NULL && dataLen > 0) {
    memcpy(buf+UBSUB_FULL_HEADER_LEN+bodyLen, optData, dataLen);
  }

  // Run the body though the cipher
  Sha256.init();
  Sha256.write((uint8_t*)key, strlen(key));
  uint8_t* expandedKey = Sha256.result();
  s20_crypt(expandedKey, S20_KEYLEN_256, (uint8_t*)&nonce, 0, buf+25, UBSUB_HEADER_LEN + fullDataLength);

  // Sign the entire thing
  Sha256.initHmac((uint8_t*)key, strlen(key));
  Sha256.write(buf, UBSUB_FULL_HEADER_LEN + fullDataLength);
  uint8_t* digest = Sha256.resultHmac();
  memcpy(buf + UBSUB_FULL_HEADER_LEN + fullDataLength, digest, 32);

  return UBSUB_CRYPTHEADER_LEN + UBSUB_HEADER_LEN + fullDataLength + UBSUB_SIGNATURE_LEN;
}


// STATIC HELPERS ===============

// Gets a static pointer to a cstr deviceId
/*
static char* getUniqueDeviceId() {
  #define STR_HELPER(x) #x
  #define STR(x) STR_HELPER(x)

  #if PARTICLE
    static char did[32];
    System.deviceID().to_cstr(did, 32);
  #elif __COUNTER__
    static char did[] = "CDID:" STR(__COUNTER__);
  #else
    static char did[] = "BDID:" __DATE__ " " __TIME__;
  #endif
  return did;
}
*/

// Get time in seconds
static uint64_t getTime() {
#if ARDUINO
  return time(NULL);
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
