#include "catch.hpp"
#include <string.h>
#include "../src/minijson.h"

TEST_CASE("Json empty", "[MJB]") {
  MiniJsonBuilder j(16);
  CHECK(j.items() == 0);
}

TEST_CASE("Empty object", "[MJB]") {
  MiniJsonBuilder j(16);
  j.open().close();
  CHECK(j.items() == 0);
  CHECK(j.length() == 2);
  CHECK(strcmp(j.c_str(), "{}") == 0);
}

TEST_CASE("Single item", "[MJB]") {
  MiniJsonBuilder j(16);
  j.open().write("hi", "there").close();
  CHECK(j.items() == 1);
  CHECK(strcmp(j.c_str(), "{\"hi\":\"there\"}") == 0);
}

TEST_CASE("Multi Items", "[MJB]") {
  MiniJsonBuilder j(32);
  j.open()
    .write("hi", "there")
    .write("mr", "bear")
    .close();
  CHECK(j.items() == 2);
  CAPTURE(j.c_str());
  CHECK(strcmp(j.c_str(), "{\"hi\":\"there\",\"mr\":\"bear\"}") == 0);
}

TEST_CASE("Escaping", "[MJB]") {
  MiniJsonBuilder j(32);
  j.open().write("hi", "the\tre").close();
  CHECK(j.items() == 1);
  CAPTURE(j.c_str());
  CHECK(strcmp(j.c_str(), "{\"hi\":\"the\\tre\"}") == 0);
}

TEST_CASE("Number", "[MJB]") {
  MiniJsonBuilder j(16);
  j.open().write("hi", 123).close();
  CHECK(j.items() == 1);
  CAPTURE(j.c_str());
  CHECK(strcmp(j.c_str(), "{\"hi\":123}") == 0);
}

TEST_CASE("Boolean", "[MJB]") {
  MiniJsonBuilder j(16);
  j.open().write("hi", true).close();
  CHECK(j.items() == 1);
  CAPTURE(j.c_str());
  CHECK(strcmp(j.c_str(), "{\"hi\":true}") == 0);
}

TEST_CASE("Json overflow protection", "[MJB]") {
  MiniJsonBuilder j(8);
  j.open().write("hi", "there").close();
  CHECK(j.items() == 1);
  CHECK(j.length() == 7);
  CAPTURE(j.c_str());
  CHECK(strcmp(j.c_str(), "{\"hi\":\"") == 0);
}

TEST_CASE("Test clearing", "[MJB]") {
  MiniJsonBuilder j(16);
  j.open().write("hi", "there").close();
  CHECK(j.items() == 1);
  CHECK(j.length() > 0);

  j.clear();
  CHECK(j.items() == 0);
  CHECK(j.length() == 0);
  CHECK(strcmp(j.c_str(), "") == 0);
}

TEST_CASE("Test foreign buf", "[MJB]") {
  char buf[128];
  memset(buf, 0, sizeof(buf));
  
  {
    MiniJsonBuilder j(buf, sizeof(buf));
    j.open().write("hi", "there").close();
  }
  CHECK(strcmp(buf, "{\"hi\":\"there\"}") == 0);
}

TEST_CASE("Test NaN", "[MJB]") {
  MiniJsonBuilder j(16);
  j.open().write("hi", NAN).close();
  CAPTURE(j.c_str());
  CHECK(j.items() == 1);
  CHECK(strcmp(j.c_str(), "{\"hi\":\"NaN\"}") == 0);
}

TEST_CASE("Test Inf", "[MJB]") {
  MiniJsonBuilder j(16);
  j.open().write("hi", INFINITY).close();
  CAPTURE(j.c_str());
  CHECK(j.items() == 1);
  CHECK(strcmp(j.c_str(), "{\"hi\":\"Inf\"}") == 0);
}
