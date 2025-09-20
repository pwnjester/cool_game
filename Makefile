build:
	gcc -IC:/SDL2/include -LC:/SDL2/lib -Wall ./src/*.c -lSDL2 -lSDL2_image -lSDL2_ttf -lm -o game

run:
	./game

clean:
	rm game