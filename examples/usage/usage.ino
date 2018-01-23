#include "ubsub.h"

Ubsub client("MyUserId", "MyUserSecret");

void myMethod(const char* arg) {
  // Called method with argument
}

void setup() {
  while (!client.connect()) {
    // Attempting connect
  }

  client.listenToTopic("testy", myMethod);
  client.publishEvent("Byg2kKB3SZ", "HJ3ytS3SW", "Hi there");
}

void loop() {
  // Do whatever else you need to do

  // Need to call this to process incoming events and managed items
  client.processEvents();
}