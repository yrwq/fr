EXE = fr
SRC = main.c

# debug with sanitizers
ifdef DEBUG
CFLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -g -O0 \
         -fsanitize=address -fsanitize=undefined \
         -fno-omit-frame-pointer -fno-optimize-sibling-calls \
		 -pthread -lgit2
else
CFLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -O2 -pthread -lgit2
endif

.PHONY: all
all: $(EXE)

$(EXE): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<


.PHONY: run
run: $(EXE)
	./$(EXE)

.PHONY: debug
debug:
	$(MAKE) DEBUG=1 $(EXE)

.PHONY: clean
clean:
	$(RM) $(EXE)

.PHONY: install
install: $(EXE)
	install -m 755 $(EXE) $(PREFIX)/bin/
