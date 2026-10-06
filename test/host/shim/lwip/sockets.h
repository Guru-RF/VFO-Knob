#ifndef SHIM_LWIP_SOCKETS_H
#define SHIM_LWIP_SOCKETS_H
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define inet_ntoa_r(addr, buf, len) inet_ntop(AF_INET, &(addr), (buf), (len))
#endif
