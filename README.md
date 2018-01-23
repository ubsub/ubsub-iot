# UbSub - Internet of Things (IoT)

C++ implementation of ubsub's UDP protocol for secure and reliable bidirectional communication for embedded devices.

# Usage

## Simple Unix Example

```cpp
#include <iostream>
#include <unistd.h>
#include "ubsub.h"

Ubsub client("MyUserId", "MyUserSecret");

void myMethod(const char* arg) {
  std::cout << "RECEIVED: " << arg << std::endl;
}

int main() {
  std::cout << "Hi there" << std::endl;

  if (!client.connect()) {
    std::cout << "Failed to connect before timeout" << std::endl;
  }

  client.listenToTopic("testy", myMethod);

  client.publishEvent("Byg2kKB3SZ", "HJ3ytS3SW", "Hi there");

  while(true) {
    client.processEvents(); // Need to call this to process incoming events and managed items
    usleep(5 * 1000);
  }

  return 0;
}

```

## Simple Arduino/Particle Example
```cpp
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
```

# Compatability

 * Unix/Linux
 * Arduino
 * Particle
 * ESP8266 Boards

# Third Party

## CryptoSuite

Sha256 implementation is from [Cathedrow/Cryptosuite](https://github.com/Cathedrow/Cryptosuite) with small modifications
for compatibility.

## Salsa20

Salsa Implementation from: https://github.com/alexwebr/salsa20

# License

See: [LICENSE.txt](LICENSE.txt)
