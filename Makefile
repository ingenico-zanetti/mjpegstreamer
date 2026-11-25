All: mjpegstreamer mjpegstreamer2 mjpegstreamer3

mjpegstreamer3: mjpegstreamer3.c
	$(CC) -o mjpegstreamer3 mjpegstreamer3.c

mjpegstreamer2: mjpegstreamer2.c
	$(CC) -o mjpegstreamer2 mjpegstreamer2.c

mjpegstreamer: mjpegstreamer.c
	$(CC) -o mjpegstreamer mjpegstreamer.c

install: mjpegstreamer mjpegstreamer2
	strip mjpegstreamer
	cp -vf ./mjpegstreamer ~/bin
	strip mjpegstreamer2
	cp -vf ./mjpegstreamer2 ~/bin
	strip mjpegstreamer3
	cp -vf ./mjpegstreamer3 ~/bin


