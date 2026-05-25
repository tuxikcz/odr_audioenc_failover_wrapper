APP := odr_audioenc_wrapper
SRC := odr_audioenc_wrapper.cpp

CXX ?= g++
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDFLAGS ?=
LDLIBS := -lcurl

.PHONY: all clean install uninstall check

all: $(APP)

$(APP): $(SRC)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: $(APP)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(APP) $(DESTDIR)$(BINDIR)/$(APP)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(APP)

clean:
	rm -f $(APP)

check: $(APP)
	./$(APP) --help || true

