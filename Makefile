mjpegstreamer: mjpegstreamer.c
	$(CC) -o mjpegstreamer mjpegstreamer.c

install: mjpegstreamer
	cp -vf ./mjpegstreamer ~/bin


