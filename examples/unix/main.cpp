#include <iostream>
#include <unistd.h>
#include "../../src/ubsub.h"

void myMethod(const char* arg) {
  std::cout << "RECEIVED: " << arg << std::endl;
}

int main() {
  std::cout << "Hi there" << std::endl;

  Ubsub client("BJv9Dr3SW", "0d5d39b502ea228153d003a461563ec7ec31848169266c4ad04c68c72d1052d0", "127.0.0.1", 3005);
  if (!client.connect(2)) {
    std::cout << "Failed to connect" << std::endl;
  }

  client.listenToTopic("testy", myMethod);

  client.publishEvent("Byg2kKB3SZ", "HJ3ytS3SW", "Hi there");
  client.callFunction("test-autocreate", "hi there direct");

  while(true) {
    client.processEvents();
    usleep(5 * 1000);
  }

  return 0;
}
