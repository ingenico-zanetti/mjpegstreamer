mjpegstreamer: mjpegstreamer.c
	$(CC) -o mjpegstreamer mjpegstreamer.c

install: mjpegstreamer
	strip mjpegstreamer
	cp -vf ./mjpegstreamer ~/bin


