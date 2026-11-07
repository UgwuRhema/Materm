CFLAGS := -g -Wall -Wextra -O2 -march=native

SRC := ./main.c
OUT := ./materm

all: $(SRC)
	cc $(CFLAGS) -o $(OUT) $(SRC) -lSDL2 -lSDL2_ttf

run:
	$(OUT)

clean:
	rm -rf $(OUT)

.PHONY: all clean run
