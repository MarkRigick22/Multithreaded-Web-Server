CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -D_GNU_SOURCE
WARN = -std=c11 -Wall -Wextra -Wpedantic -Werror
ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -D_DARWIN_C_SOURCE
endif
.PHONY: all clean test bench
all: server client public/output.cgi public/home.html public/favicon.ico
server: server.o request.o segel.o log.o
	$(CC) $(CFLAGS) $(LDFLAGS) -pthread $^ -o $@
client: client.o segel.o
	$(CC) $(CFLAGS) $(LDFLAGS) -pthread $^ -o $@
%.o: %.c
	$(CC) $(CPPFLAGS) $(WARN) $(CFLAGS) -pthread -MMD -MP -c $< -o $@
public:
	mkdir -p $@
public/output.cgi: output.c | public
	$(CC) $(CPPFLAGS) $(WARN) $(CFLAGS) $(LDFLAGS) $< -o $@
public/%: % | public
	cp $< $@
tests/monitor: tests/monitor.c log.c log.h segel.h segel.o
	$(CC) $(CPPFLAGS) $(WARN) $(CFLAGS) $(LDFLAGS) -pthread tests/monitor.c segel.o -o $@
test: all tests/monitor
	./tests/monitor
	python3 tests/integration.py
bench: all
	python3 bench/run.py
clean:
	rm -f *.o *.d server client tests/monitor public/output.cgi public/home.html public/favicon.ico
-include $(wildcard *.d)
