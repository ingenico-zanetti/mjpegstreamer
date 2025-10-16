// data come from stdin
// they are parsed to get the JPEG frames
// and when a TCP connection occurs
// the TCP client is feed with data starting at a frame beginning
//

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
		fprintf(stderr,
				"Usage: %s <port definition> [<port definition> [ .... ]]" "\n"
				"with <port definition> either a TCP port number or stdout," 
			        "optionally followed by a ':' and an integer decimation factor" "\n",
				argv[0]);
		exit(1);
	}
	int in  = STDIN_FILENO;
	uint8_t *buffer = (uint8_t *)malloc(BUFFER_SIZE);
	if(buffer != NULL){
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
					ssize_t lus = read(in, buffer, BUFFER_SIZE);
					if(lus <= 0){
						break;
					}
					analyze_and_forward(&context, buffer, lus);
				}
				fflush(stdout);
			}
		}
		free(buffer);
	}
	return(0);
}


