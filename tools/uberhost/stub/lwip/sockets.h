/* lwip/sockets.h for the PC: the host's sockets, which lwIP's imitate. */
#pragma once
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define inet_ntoa_r(addr, buf, len) inet_ntop(AF_INET, &(addr), (buf), (len))
