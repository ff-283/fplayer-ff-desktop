extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/version.h>
}

#include <iostream>

int main() {
	std::cout << "avcodec version: " << avcodec_version() << std::endl;
	std::cout << "avformat version: " << avformat_version() << std::endl;
	std::cout << "avutil version: " << avutil_version() << std::endl;
	return 0;
}