#include <iostream>
#include <unistd.h>
#include "../../src/ubsub.h"

void myMethod(const char* arg) {
  std::cout << "RECEIVED: " << arg << std::endl;
}

int main() {
  std::cout << "Hi there" << std::endl;

  Ubsub client("BJv9Dr3SW", "0d5d39b502ea228153d003a461563ec7ec31848169266c4ad04c68c72d1052d0", "127.0.0.1", 4001);
  if (!client.connect(2)) {
    std::cout << "Failed to connect" << std::endl;
  }
  // client.enableAutoRetry(false);

  client.listenToTopic("testy", myMethod);

  client.publishEvent("Byg2kKB3SZ", "HJ3ytS3SW", "Hi there");
  client.callFunction("test-autocreate", "hi there direct");

  client.flush();

  int test = 50000;
  client.setWatchTopic("iot-watches");
  client.watchVariable("other", &test);
  client.watchVariable("var", &test); // Any time this value changes, an event will be emitted

  while(true) {
    test++;
    client.processEvents();
    usleep(5 * 1000);
  }

  return 0;
}
