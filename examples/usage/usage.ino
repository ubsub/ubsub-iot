#include "ubsub.h"

Ubsub client("MyUserId", "MyUserSecret");

float myGlobalVal = 0f;

void myMethod(const char* arg) {
  // Called method with argument
}

void setup() {
  while (!client.connect()) {
    // Attempting connect
  }

  // Listen and publish data
  client.listenToTopic("testy", myMethod);
  client.publishEvent("Byg2kKB3SZ", "HJ3ytS3SW", "Hi there");

  // Automatically watch value and send update when it changes
  client.watchVariable("myval", &myGlobalVal);
}

void loop() {
  // Do whatever else you need to do

  // Need to call this to process incoming events and managed items
  client.processEvents();
}