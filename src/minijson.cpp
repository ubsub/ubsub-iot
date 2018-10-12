#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "minijson.h"

MiniJsonBuilder::MiniJsonBuilder(int buflen) {
	this->len = buflen;
	this->buf = (char*)malloc(buflen);
	this->foreignBuf = false;
	this->clear();
}

MiniJsonBuilder::MiniJsonBuilder(char* buf, int len) {
	this->len = len;
	this->buf = buf;
	this->foreignBuf = true;
	this->cur = 0;
	this->itemCount = 0;
}

MiniJsonBuilder::~MiniJsonBuilder() {
	if (!this->foreignBuf)
		free(this->buf);
}

MiniJsonBuilder& MiniJsonBuilder::open() {
	this->append("{");
	return *this;
}

MiniJsonBuilder& MiniJsonBuilder::write(const char* name, const char* val, bool literal) {
	if (this->itemCount > 0)
		this->append(",");
	this->appendQuoted(name);
	this->append(":");
	if (literal)
		this->append(val);
	else
		this->appendQuoted(val);
	this->itemCount++;
	return *this;
}

MiniJsonBuilder& MiniJsonBuilder::write(const char* name, int val) {
	char num[20];
	snprintf(num, sizeof(num), "%d", val);
	this->write(name, num, true);
	return *this;
}

MiniJsonBuilder& MiniJsonBuilder::write(const char* name, float val) {
	char num[20];
	snprintf(num, sizeof(num), "%f", val);
	this->write(name, num, true);
	return *this;
}

MiniJsonBuilder& MiniJsonBuilder::write(const char* name, bool val) {
	if (val)
		this->write(name, "true", true);
	else
		this->write(name, "false", true);
	return *this;
}


MiniJsonBuilder& MiniJsonBuilder::close() {
	this->append("}");
	return *this;
}

void MiniJsonBuilder::clear() {
	this->cur = 0;
	this->itemCount = 0;
	memset(this->buf, 0, this->len);
}

int MiniJsonBuilder::items() {
	return this->itemCount;
}

int MiniJsonBuilder::length() {
	return this->cur;
}

const char* MiniJsonBuilder::c_str() {
	return this->buf;
}

void MiniJsonBuilder::append(char c) {
	if (this->cur >= this->len-1)
		return;
	this->buf[this->cur++] = c;
}

void MiniJsonBuilder::append(const char* s, bool escape) {
	for (unsigned int i=0; i<strlen(s); ++i) {
		char c = s[i];
		if (escape) {
			if (c == '"' || c == '\\') {
				this->append('\\');
				this->append(c);
			} else if (c == '\t') {
				this->append('\\');
				this->append('t');
			} else if (c == '\r') {
				this->append('\\');
				this->append('r');
			} else if (c == '\n') {
				this->append('\\');
				this->append('n');
			} else {
				this->append(c);
			}
		} else {
			this->append(c);
		}
	}
}

void MiniJsonBuilder::appendQuoted(const char* s) {
	this->append('"');
	this->append(s, true);
	this->append('"');
}
