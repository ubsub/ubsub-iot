
#ifndef minijson_h
#define minijson_h

class MiniJsonBuilder {
private:
	char* buf;
	int len;
	int cur;
	int itemCount;

public:
	MiniJsonBuilder(int buflen);
	~MiniJsonBuilder();

	MiniJsonBuilder& open();
	MiniJsonBuilder& write(const char* name, const char* val, bool literal = false);
	MiniJsonBuilder& write(const char* name, int val);
	MiniJsonBuilder& write(const char* name, float val);
	MiniJsonBuilder& write(const char* name, bool val);
	MiniJsonBuilder& close();

	const char* c_str();
	int items();

protected:
	void append(char c);
	void append(const char* s, bool escape=false);
	void appendEscaped(const char* s);
	void appendQuoted(const char* s);
};

#endif
