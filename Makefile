CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter
frame-finch: src/main.c src/ff.c src/ff.h src/stb_image_write.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -D_DARWIN_C_SOURCE -o $@ src/main.c src/ff.c -lm
clean:
	rm -f frame-finch
