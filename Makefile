all:
	g++ -std=c++17 *.cpp `pkg-config --libs --cflags libavcodec libavformat libavutil` -g && ./a.out
