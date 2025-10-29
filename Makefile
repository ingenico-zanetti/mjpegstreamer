mjpegstreamer2: mjpegstreamer2.c
	$(CC) -o mjpegstreamer2 mjpegstreamer2.c

mjpegstreamer: mjpegstreamer.c
	$(CC) -o mjpegstreamer mjpegstreamer.c

install: mjpegstreamer mjpegstreamer2
	strip mjpegstreamer
	cp -vf ./mjpegstreamer ~/bin
	strip mjpegstreamer2
	cp -vf ./mjpegstreamer2 ~/bin


