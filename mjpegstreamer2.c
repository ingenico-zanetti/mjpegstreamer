// data come from stdin
// they are parsed to get the JPEG frames
// and when a TCP connection occurs
// the TCP client is feed with data starting at a frame beginning
// Version 2: tries to detect the frame rate and smooth the output
// We store the incoming MJPEG stream in a massive FIFO and stores index in the FIFO as whole JPEG image are parsed.
// The index also stores the arrival time and the guessed time of delivery (prediction starts at 1 fps).
// We also compute the mean time between frames to improve the prediction of delivery time interval.
// The purpose is to push the whole JPEG image at the correct interval, knowing they are compressed in 4 // threads
// in rpicam-vid. So 4 consecutive frames should be enough to avoid starvation.

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <linux/limits.h>
#include <sys/time.h>

int listenSocket(struct in_addr *address, unsigned short port){
	struct sockaddr_in local_sock_addr = {.sin_family = AF_INET, .sin_addr = *address, .sin_port = port};
	int listen_socket = socket(AF_INET, SOCK_STREAM, 0);
	// fprintf(stderr, "listen_socket=%d" "\n", listen_socket);
	const int enable = 1;
	if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0){
		// fprintf(stderr,"setsockopt(SO_REUSEADDR) failed" "\n");
	}else{
		int error = bind(listen_socket, (struct sockaddr *)&local_sock_addr, sizeof(local_sock_addr));
		if(error){
			close(listen_socket);
			listen_socket = -1;
		}
	}
	if(listen_socket >= 0){
		int error = listen(listen_socket, 1);
		if(error){
			close(listen_socket);
			listen_socket = -2;
		}
	}
	return listen_socket;
}

#define BUFFER_SIZE (60 * 1000 * 1000)
#define MAX_OUTPUTS (16)

// #define FIFO_SIZE (256 * 1024 *1024)
#define FIFO_SIZE (2 * 1024 *1024)

typedef struct {
	ssize_t readIndex;
	ssize_t writeIndex;
	ssize_t analyzeIndex;
	size_t size;
	size_t level;
	size_t remaining;
	unsigned char *buffer;
} Fifo_s;

static Fifo_s *allocateFifo(size_t size){
	Fifo_s *fifo = (Fifo_s*)malloc(sizeof(Fifo_s));
	if(fifo){
		fifo->readIndex = 0;
		fifo->writeIndex = 0;
		fifo->analyzeIndex = 0;
		fifo->level = 0;
		fifo->size = size;
		fifo->remaining = size;
		fifo->buffer = (unsigned char *)malloc(size);
		if(NULL == fifo->buffer){
			free(fifo);
			fifo = NULL;
		}
	}
	return fifo;
}

static ssize_t readToFifo(int fd, Fifo_s *fifo){
	// first read to fill up-to the end
	size_t firstReadSize = fifo->size - fifo->writeIndex;
	if(firstReadSize > fifo->remaining){
		firstReadSize = fifo->remaining;
	}
	ssize_t firstRead = read(fd, fifo->buffer + fifo->writeIndex, firstReadSize);
	if(firstRead > 0){
		// fprintf(stderr, "firstRead()=>%ld" "\n", firstRead);
		fifo->remaining -= firstRead;
		fifo->writeIndex += firstRead;
		fifo->level += firstRead;
		if(fifo->writeIndex == fifo->size){
			// end of FIFO reached, wrap around
			fifo->writeIndex = 0;
			// and try a second read
			ssize_t lus = read(fd, fifo->buffer, fifo->remaining);
			if(lus > 0){
				fprintf(stderr, "secondRead()=>%ld" "\n", lus);
				fifo->remaining -= lus;
				fifo->writeIndex = lus;
				fifo->level += lus;
			}
		}
	}
	fprintf(stderr, "remaining=%ld" "\n", fifo->remaining);
	return firstRead;
}

enum FifoParserState_e {
	FIFO_PARSER_STATE_WAITING_FOR_FIRST_FF_NO_PICTURE,
	FIFO_PARSER_STATE_WAITING_FOR_D8_NO_PICTURE,
	FIFO_PARSER_STATE_WAITING_FOR_SECOND_FF_NO_PICTURE,
	FIFO_PARSER_STATE_WAITING_FOR_E0_NO_PICTURE,
	FIFO_PARSER_STATE_WAITING_FOR_FIRST_FF,
	FIFO_PARSER_STATE_WAITING_FOR_D8,
	FIFO_PARSER_STATE_WAITING_FOR_SECOND_FF,
	FIFO_PARSER_STATE_WAITING_FOR_E0,
};


typedef struct {
	enum FifoParserState_e state;
	struct timespec firstPictureTime;
	int pictureCount;
	int pendingPicture;
} FifoParserContext_s;

#define ONE_BILLION (1000000000)

static void newPicture(FifoParserContext_s *context, struct timespec *now){
	if(0 == context->pictureCount++){
		context->firstPictureTime = *now;
		fprintf(stderr, "first picture at %ld.%09ld" "\n", now->tv_sec, now->tv_nsec);
	}else{
		int64_t deltaNano = now->tv_nsec - context->firstPictureTime.tv_nsec;
		deltaNano += ONE_BILLION * (now->tv_sec - context->firstPictureTime.tv_sec);
		fprintf(stderr, "mean delta over %ld picture(s)= %ldns" "\n", context->pictureCount, deltaNano / context->pictureCount);
	}
}

static ssize_t fifoWrapAround(Fifo_s *fifo, ssize_t index){
	if(index < 0){
		index += fifo->size;
	}else if(index >= fifo->size){
		index -= fifo->size;
	}
	return index;
}

static void fifoOffsetIndex(Fifo_s *fifo, ssize_t *index, ssize_t delta){
	*index = fifoWrapAround(fifo, *index + delta);
}

static void fifoDiscard(Fifo_s *fifo, size_t count){
	fifoOffsetIndex(fifo, &(fifo->readIndex), count);
	fifo->remaining += count;
	fifo->level -= count;
}

static int fifoParse(Fifo_s *fifo, FifoParserContext_s *context){
	while(fifo->analyzeIndex != fifo->writeIndex){
		unsigned char octet = fifo->buffer[fifo->analyzeIndex];
		fifoOffsetIndex(fifo, &(fifo->analyzeIndex), 1);
		switch(context->state){
			case FIFO_PARSER_STATE_WAITING_FOR_FIRST_FF_NO_PICTURE:
				if(octet != 0xFF){
					fifoDiscard(fifo, 1);
				}else{
					context->state = FIFO_PARSER_STATE_WAITING_FOR_D8_NO_PICTURE;
				}
				break;
			case FIFO_PARSER_STATE_WAITING_FOR_D8_NO_PICTURE:
				if(octet != 0xD8){
					fifoDiscard(fifo, 2);
				}else{
					context->state = FIFO_PARSER_STATE_WAITING_FOR_SECOND_FF_NO_PICTURE;
				}
				break;
			case FIFO_PARSER_STATE_WAITING_FOR_SECOND_FF_NO_PICTURE:
				if(octet != 0xFF){
					fifoDiscard(fifo, 3);
				}else{
					context->state = FIFO_PARSER_STATE_WAITING_FOR_E0_NO_PICTURE;
				}
				break;
			case FIFO_PARSER_STATE_WAITING_FOR_E0_NO_PICTURE:
				if(octet != 0xE0){
					fifoDiscard(fifo, 4);
				}else{
					context->state = FIFO_PARSER_STATE_WAITING_FOR_FIRST_FF;
					struct timespec now;
					int res = clock_gettime(CLOCK_MONOTONIC, &now);
					if(res){
						perror("clock_gettimer(CLOCK_MONOTONIC)");
					}else{
						fprintf(stderr, "Got SoP at %ld.%09ld, index=%ld" "\n", now.tv_sec, now.tv_nsec, fifoWrapAround(fifo, fifo->analyzeIndex - 4));
						newPicture(context, &now);
					}
				}
			case FIFO_PARSER_STATE_WAITING_FOR_FIRST_FF:
				if(octet == 0xFF){
					context->state = FIFO_PARSER_STATE_WAITING_FOR_D8;
				}
				break;
			case FIFO_PARSER_STATE_WAITING_FOR_D8:
				if(octet != 0xD8){
					context->state = FIFO_PARSER_STATE_WAITING_FOR_FIRST_FF;
				}else{
					context->state = FIFO_PARSER_STATE_WAITING_FOR_SECOND_FF;
				}
				break;
			case FIFO_PARSER_STATE_WAITING_FOR_SECOND_FF:
				if(octet != 0xFF){
					context->state = FIFO_PARSER_STATE_WAITING_FOR_FIRST_FF;
				}else{
					context->state = FIFO_PARSER_STATE_WAITING_FOR_E0;
				}
				break;
			case FIFO_PARSER_STATE_WAITING_FOR_E0:
				if(octet != 0xE0){
					context->state = FIFO_PARSER_STATE_WAITING_FOR_FIRST_FF;
				}else{
					context->state = FIFO_PARSER_STATE_WAITING_FOR_FIRST_FF;
					struct timespec now;
					int res = clock_gettime(CLOCK_MONOTONIC, &now);
					if(res){
						perror("clock_gettimer(CLOCK_MONOTONIC)");
					}else{
						fprintf(stderr, "Got SoP at %ld.%09ld, index=%ld" "\n", now.tv_sec, now.tv_nsec, fifoWrapAround(fifo, fifo->analyzeIndex - 4));
						newPicture(context, &now);
						ssize_t delta = fifoWrapAround(fifo, fifo->analyzeIndex - fifo->readIndex);
						fifo->readIndex = fifo->analyzeIndex;
						fifo->level -= delta;
						fifo->remaining += delta;
					}
				}
				break;
		}
	}
}

typedef struct {
	Fifo_s *fifo;
	size_t firstStartOffset;
	size_t firstLength;
	size_t secondStartOffset;
	size_t secondLength;
	struct timeval rxTime;
	struct timeval txTime;
} QueueElement_s;

typedef enum {
	OUTPUT_STATE_IDLE,
	OUTPUT_STATE_RUNNING
} OutputState_e;

typedef struct {
	int fd;
	OutputState_e state;
	int decimate;
	int counter;
} Output_s;

typedef struct {
	int index;
	Output_s outputs[MAX_OUTPUTS];
	uint8_t *outputBuffer;
	ssize_t outputBufferIndex;
} parserContext_s;

static void contextInitialize(parserContext_s *context){
	context->index = 0;
	int i = MAX_OUTPUTS;
	while(i--){
		context->outputs[i].fd = -1;
		context->outputs[i].state = OUTPUT_STATE_IDLE;
		context->outputs[i].decimate = 0;
		context->outputs[i].counter = 0;
	}
	context->outputBuffer = (uint8_t *)malloc(BUFFER_SIZE);
	context->outputBufferIndex = 0;
}

static int contextFirstSlotAvailable(parserContext_s *context){
	int i = 0;
	while(i < MAX_OUTPUTS){
		if(-1 == context->outputs[i].fd){
			return(i);
		}
		i++;
	}
	return(-1);
}

static void analyze_and_forward(parserContext_s *context, const uint8_t *buffer, ssize_t length){
	ssize_t i = length;
	const uint8_t *p = buffer;
	while(i--){
		int doFlush = 0;
		uint8_t octet = *p++;
		if((0 == context->index) && (0xFF == octet)){
			context->index++;
		}else if((1 == context->index) && (0xD8 == octet)){
			context->index++;
		}else if((2 == context->index) && (0xFF == octet)){
			context->index++;
		}else if((3 == context->index) && (0xE0 == octet)){
			// fprintf(stderr, "0xFFD8FFE0" "\n", octet);
			context->index = 0;
			doFlush = 1;
		}else{
			context->index = 0;
		}
		if(context->outputBufferIndex < BUFFER_SIZE){
			context->outputBuffer[context->outputBufferIndex++] = octet;
		}else{
			// the 'picture' doesn't fit into our buffer
			fprintf(stderr, "discard buffer (index=%d)" "\n", context->outputBufferIndex = 0); 
			context->outputBufferIndex = 0;
			context->index = 0;
		}
		if(doFlush){
			ssize_t lengthToFlush = context->outputBufferIndex - 4;
			if(lengthToFlush > 0){
				int i = MAX_OUTPUTS;
				while(i--){
					if(context->outputs[i].fd != -1){
						int doOutput = 0;
						context->outputs[i].state = OUTPUT_STATE_RUNNING;
						if(context->outputs[i].decimate){
							if(0 == --context->outputs[i].counter){
								context->outputs[i].counter = context->outputs[i].decimate;
								doOutput = 1;
							}
						}else{
							doOutput = 1;
						}
						if(doOutput){
							if(lengthToFlush != write(context->outputs[i].fd, context->outputBuffer, lengthToFlush)){
								fprintf(stderr, "slot %d had an error, closing fd %d" "\n", i, context->outputs[i].fd);
								context->outputs[i].state = OUTPUT_STATE_IDLE;
								close(context->outputs[i].fd);
								context->outputs[i].fd  = -1;
							}
						}
					}
				}
				// Move current "tag" to start of buffer
				// by moving nothing
				// fprintf(stderr, "flushed %d, reset index to 4" "\n", lengthToFlush);
				context->outputBufferIndex = 4;
			}else{
				// fprintf(stderr, "nothing to flush" "\n");
			}
		}
	}
}

#define MAX_LISTENING_SOCKETS (16)

int startsWith(const char *start, const char *with){
	return(start == strstr(start, with));
}
	
int main(int argc, const char *argv[]){
	if(argc < 2){
		struct timespec resolution;
		int res = clock_getres(CLOCK_MONOTONIC, &resolution);
		fprintf(stderr, "res=%i, resolution={%ld, %ld}" "\n", res, resolution.tv_sec, resolution.tv_nsec);
		res = clock_gettime(CLOCK_MONOTONIC, &resolution);
		fprintf(stderr, "res=%i, resolution={%ld, %ld}" "\n", res, resolution.tv_sec, resolution.tv_nsec);
		res = clock_gettime(CLOCK_MONOTONIC, &resolution);
		fprintf(stderr, "res=%i, resolution={%ld, %ld}" "\n", res, resolution.tv_sec, resolution.tv_nsec);
		fprintf(stderr,
				"Usage: %s <port definition> [<port definition> [ .... ]]" "\n"
				"with <port definition> either a TCP port number or stdout," 
			        "optionally followed by a ':' and an integer decimation factor" "\n",
				argv[0]);
		exit(1);
	}
	int in  = STDIN_FILENO;
	uint8_t *buffer = (uint8_t *)malloc(BUFFER_SIZE);
	Fifo_s *fifo = allocateFifo(FIFO_SIZE);
	if(buffer != NULL && fifo != NULL){
		FifoParserContext_s fifoContext = {.state = FIFO_PARSER_STATE_WAITING_FOR_FIRST_FF_NO_PICTURE,  .firstPictureTime = (0, 0), .pictureCount = 0};
		parserContext_s context;
                contextInitialize(&context);

		int listeningSockets[MAX_LISTENING_SOCKETS];
		int socketDecimation[MAX_LISTENING_SOCKETS];
		int listeningSocketCount = 0;
		for(int i = 0 ; i < MAX_LISTENING_SOCKETS ; i++){
			listeningSockets[i] = -1;
			socketDecimation[i] = 0;
		}
		struct in_addr listenAddress = {0};
		for(int i = 1 ; i < MAX_LISTENING_SOCKETS ; i++){
			if(i < argc){
				int decimation = 0;
				if(startsWith(argv[i], "stdout")){
					const char *colon = strchr(argv[i], ':');
					if(colon){
						decimation = atoi(colon + 1);
					} 
					int index  = contextFirstSlotAvailable(&context);
					if(index != -1){
						context.outputs[index].state = OUTPUT_STATE_IDLE;
						context.outputs[index].fd = STDOUT_FILENO;
						context.outputs[index].decimate = decimation;
						context.outputs[index].counter = decimation;
						fprintf(stderr, "Ouputting to stdout with decimation %i" "\n", decimation);
					}
				}else{
					int tcpPort = atoi(argv[i]);
					if((0 < tcpPort) && (tcpPort < 65535)){
						const char *colon = strchr(argv[i], ':');
						if(colon){
							decimation = atoi(colon + 1);
						} 
						listeningSockets[i] = listenSocket(&listenAddress, htons(tcpPort));
						socketDecimation[i] = decimation;
						fprintf(stderr, "Listening to TCP port %i, with decimation %i" "\n",tcpPort, decimation);
					}
				}
			}
		}
						
		for(;;){
			void updateMax(int *m, int n){
				int max = *m;
				if(n > max){
					*m = n;
				}
			}
			int max = -1;
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(STDIN_FILENO, &fds); updateMax(&max, STDIN_FILENO);
			for(int i = 0 ; i < MAX_LISTENING_SOCKETS ; i++){
				int listeningSocket = listeningSockets[i];
				if(-1 != listeningSocket && (0 <= contextFirstSlotAvailable(&context))){
					FD_SET(listeningSocket, &fds); updateMax(&max, listeningSocket);
				}
			}
			int i = MAX_OUTPUTS;
			while(i--){
				int fd = context.outputs[i].fd;
				if(-1 != fd){
					FD_SET(fd, &fds); updateMax(&max, fd);
				}
			}
			struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
			int selected = select(max + 1, &fds, NULL, NULL, &timeout);
			if(selected > 0){
				// Check forwarding socket for error (read ready should not happen)
				i = MAX_OUTPUTS;
				while(i--){
					int fd = context.outputs[i].fd;
					if(FD_ISSET(fd, &fds)){
						close(fd);
						context.outputs[i].fd = -1;
						context.outputs[i].state = OUTPUT_STATE_IDLE;
						fprintf(stderr, "slot %d had an error, closing fd %d" "\n", i, fd);
					}
				}
				// Check listening sockets for incoming connection
				for(int i = 0 ; i < MAX_LISTENING_SOCKETS ; i++){
					int listeningSocket = listeningSockets[i];
					if(-1 != listeningSocket && FD_ISSET(listeningSocket, &fds)){
						int index = contextFirstSlotAvailable(&context);
						if(-1 != index){
							context.outputs[index].state = OUTPUT_STATE_IDLE;
							context.outputs[index].fd = accept(listeningSocket, NULL, NULL);
							context.outputs[index].decimate = socketDecimation[i];
							context.outputs[index].counter = socketDecimation[i];
							fprintf(stderr, "accepted connexion to slot %d (fd=%d), decimation=%i" "\n", index, context.outputs[index].fd, context.outputs[index].decimate);
						}
					}
				}
				if(FD_ISSET(STDIN_FILENO, &fds)){
					// fprintf(stderr, "data available on stdin" "\n");
					ssize_t lus = readToFifo(in, fifo);
					if(lus <= 0){
						break;
					}
					fifoParse(fifo, &fifoContext);
				}
				fflush(stdout);
			}
		}
		free(buffer);
	}
	return(0);
}


