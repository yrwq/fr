EXE = fr
SRC = main.c
OBJ = $(SRC:.c=.o)

CPPFLAGS =
WARNFLAGS = -Wall -Wextra -Werror -pedantic
CFLAGS = -std=c99 $(WARNFLAGS) -pthread
LDFLAGS =
LDLIBS = -lgit2

ifdef DEBUG
CFLAGS += -g -O0 -fsanitize=address -fsanitize=undefined \
          -fno-omit-frame-pointer -fno-optimize-sibling-calls
LDFLAGS += -fsanitize=address -fsanitize=undefined
else
CFLAGS += -O2
endif

.PHONY: all
all: $(EXE)

$(EXE): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<


.PHONY: run
run: $(EXE)
	./$(EXE)

.PHONY: debug
debug:
	$(MAKE) DEBUG=1 $(EXE)

.PHONY: clean
clean:
	$(RM) $(EXE) $(OBJ)

.PHONY: install
install: $(EXE)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(EXE) $(DESTDIR)$(PREFIX)/bin/
