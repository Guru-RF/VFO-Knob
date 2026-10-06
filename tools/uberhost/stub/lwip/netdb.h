/* lwip/netdb.h for the PC: the host's, but a name looked up as the knob's
 * lwIP looks up one no server knows -- not found -- while an address stands
 * for itself (stubs.c). No lookup ever leaves the PC: a scenario's receiver
 * whose name is not found is not asked of anyone's DNS. */
#pragma once
#include <netdb.h>
int  uh_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void uh_freeaddrinfo(struct addrinfo *ai);
#define getaddrinfo  uh_getaddrinfo
#define freeaddrinfo uh_freeaddrinfo
