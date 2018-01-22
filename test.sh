#!/bin/bash
g++ src/*.cpp examples/unix/main.cpp -Wall -DUBSUB_LOG $@ && ./a.out
