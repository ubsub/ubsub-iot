#include <stdint.h>

#ifndef ubsub_h
#define ubsub_h

const char* DEFAULT_UBSUB_ROUTER = "https://router.ubsub.io";

class Ubsub {
private:
  const char* userId;
  const char* userKey;
  const char* host;

private:
  void initSocket() {
    // TODO: Create socket (random port number)
  }

public:
  Ubsub(const char *userId, const char *userKey, const char *ubsubHost) {
    this.userId = userId;
    this.userKey = userKey;
    this.host = ubsubHost;
    this.initSocket();
  }

  Ubsub(const char* userId, const char* userKey) {
    this.userId = userId;
    this.userKey = userKey;
    this.host = DEFAULT_UBSUB_ROUTER;
    this.initSocket();
  }

  // Attempts to establish a connection with UbSub.io
  // If succeeds returns true.  If fails after timeout, returns false
  // NOT REQUIRED to call, but will guarantee a connection has been established
  bool connect(int timeout = 10) {

  }

  // Send message to a given topic on ubsub. Topic does not have to belong
  // to the user.
  // The msg can either be serialized JSON OR a simple string that will be encapsulated on the server
  void publishEvent(const char *topicId, const char *topicKey, const char *msg) {

  }

  // Creates a new topic, but does NOT listen to it
  void createTopic(const char *topicName) {

  }

  // Listen to a given topic for events. Similar to creating a function
  // but will listen to an existing topic
  void listenToTopic(const char *topicIdOrName, void callback(const char* arg)) {

  }

  // Create a new function that can be invoked by another caller, and immediately listen to it
  // This function will be a topic in ubsub
  void createFunction(const char *name, void callback(const char* arg)) {

  }

  // Call function on another device
  void callFunction(const char *name, const char *arg) {

  }

  void callFunction(const char *name) {
    this.callFunction(name, NULL);
  }

  // processEvents() needs to be called frequently.  It takes care of things such as:
  // - Re-sending any queued outbound messages
  // - Checking for, and processing, incoming data
  // - Occassionally handling pings to keep connection alive (or reconnecting if needed)
  void processEvents() {

  }
}

#endif