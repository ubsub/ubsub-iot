# UbSub - Internet of Things (IoT)

[![Build Status](https://travis-ci.org/ubsub/ubsub-iot.svg?branch=master)](https://travis-ci.org/ubsub/ubsub-iot)

C++ implementation of ubsub's UDP protocol for secure and reliable bidirectional communication for embedded devices.

# Installation

## Particle.io libraries

The latest version of this library will be kept up-to-date on particle.io

It can be found on the web IDE, or via the CLI

```sh
particle library search ubsub
```

## Platformio.org libraries

This library is also published to platformio [here](http://platformio.org/lib/show/2118/Ubsub).

## Manual

Head over to the [release tab](https://github.com/ubsub/ubsub-iot/releases), download the zip
file, and include it in your project.  With **Arduino**, you're able to do this via their IDE,
and on other projects, you can simply add all the files in `src/` to your project.

# Example Usage

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
