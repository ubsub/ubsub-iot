#!/bin/bash
g++ src/*.cpp examples/unix/main.cpp -Wall -Werror -DUBSUB_LOG $@ && ./a.out
