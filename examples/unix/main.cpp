#include <iostream>
#include "../../src/ubsub.h"

void onLog(const char* level, const char* msg) {
  std::cout << "[" << level << "] " << msg << std::endl;
}

int main() {
  std::cout << "Hi there" << std::endl;

  Ubsub client("BJv9Dr3SW", "0d5d39b502ea228153d003a461563ec7ec31848169266c4ad04c68c72d1052d0", "127.0.0.1", 3005);
  client.setOnLog(&onLog);
  if (!client.connect(2)) {
    std::cout << "Failed to connect" << std::endl;
  }

  client.publishEvent("Byg2kKB3SZ", "HJ3ytS3SW", "Hi there");

  return 0;
}
