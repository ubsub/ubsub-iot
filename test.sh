#!/bin/bash
g++ src/*.cpp examples/unix/main.cpp -Wall -Werror -DUBSUB_LOG -DUBSUB_LOG_DEBUG $@ && ./a.out
