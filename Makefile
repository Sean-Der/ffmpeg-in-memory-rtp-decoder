self-demux:
	g++ -std=c++17 self-demux.cpp `pkg-config --libs --cflags libavcodec libavformat libavutil` -ldatachannel -rpath /usr/local/lib && ./a.out

ffmpeg-demux:
	g++ -std=c++17 ffmpeg-demux.cpp `pkg-config --libs --cflags libavcodec libavformat libavutil` && ./a.out
