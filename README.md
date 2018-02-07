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

You can use it by typing:

```sh
platformio lib install ubsub
```

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

# API

## Ubsub(const char* deviceId, const char* deviceKey)

Create an instance of the Ubsub client with the given device info.

Device id/key can either be user id/key OR token id/key.

## Ubsub::enableAutoSyncTime(bool)

**Support**: Arduino, Particle

Enables/disables the client auto-syncing time on devices.

## bool Ubsub::connect([timeout])

**Returns:** `true` on success

Attempts to establish a connection to ubsub.io.  Will wait for WiFi first,
sync time if asked to, and negotiate NAT routing.

**Must be called prior to any other functions.**

## Ubsub::publishEvent(topicId, topicKey, msg)

Publish an event to Ubsub.io topic.

## Ubsub::listenToTopic(topicNameOrId, callback)

Listen to a topic on ubsub.io

Callback: `void callbackfunc(const char* arg)`

## Ubsub:createFunction(topicNameOrId, callback)

Same as listenToTopic, except will create the topic if not exist

## Ubsub::callFunction(name, arg)
## Ubsub::callFunction(name)

Call a topic, by name, with an optional argument.

If the arg is JSON, it will be interpreted as such, otherwise
it will be wrapped like so: `{"payload": <arg>}`

## processEvents()

Receives, pings, and retries any outstanding events.  Must be called frequently, such as in your `void loop(){}` function.

## int getLastError()

Gets the last error code that has occurred in the client. `0` is no-error.

**Errors:** (from [ubsub.h](ubsub.h))

```c
#define UBSUB_ERR_INVALID_PACKET -1
#define UBSUB_ERR_BAD_VERSION -2
#define UBSUB_ERR_USER_MISMATCH -3
#define UBSUB_ERR_BAD_SIGNATURE -4
#define UBSUB_ERR_TIMEOUT -5
#define UBSUB_ERR_EXCEEDS_MTU -6
#define UBSUB_ERR_SOCKET -7
#define UBSUB_ERR_SOCKET_BIND -8
#define UBSUB_ERR_NETWORK -9
#define UBSUB_ERR_SEND -10
#define UBSUB_ERR_BAD_REQUEST -11
#define UBSUB_ERR_NONCE_DUPE -12
#define UBSUB_MISSING_ARGS -50
#define UBSUB_ERR_UNKNOWN -1000
#define UBSUB_ERR_MALLOC -2000
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
