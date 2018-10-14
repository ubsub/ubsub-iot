
#ifndef ubsub_minijson_h
#define ubsub_minijson_h

class MiniJsonBuilder {
private:
	char* buf;
	int len;
	bool foreignBuf;
	
	int cur;
	int itemCount;

public:
	MiniJsonBuilder(int buflen);
	MiniJsonBuilder(char* buf, int len);
	~MiniJsonBuilder();

	MiniJsonBuilder& open();
	MiniJsonBuilder& write(const char* name, const char* val, bool literal = false);
	MiniJsonBuilder& write(const char* name, int val);
	MiniJsonBuilder& write(const char* name, float val);
	MiniJsonBuilder& write(const char* name, bool val);
	MiniJsonBuilder& close();

	void clear();

	const char* c_str();
	int items();
	int length();

protected:
	void append(char c);
	void append(const char* s, bool escape=false);
	void appendEscaped(const char* s);
	void appendQuoted(const char* s);
};

#endif
