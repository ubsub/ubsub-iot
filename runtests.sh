#!/bin/bash
set -ex
g++ -std=c++11 -Wall -Werror tests/*.cpp src/minijson.cpp -o tests.out
./tests.out