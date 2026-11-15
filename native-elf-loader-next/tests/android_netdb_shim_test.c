#define _GNU_SOURCE
#include "../android_netdb_shim.h"
#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
int main(void){
 struct android_addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM,.ai_flags=4|8|2},*list=NULL;
 assert(android_getaddrinfo("127.0.0.1","443",&hints,&list)==0);
 assert(list&&list->ai_addr&&list->ai_addrlen==sizeof(struct sockaddr_in));
 const struct sockaddr_in *ip=(const void*)list->ai_addr;
 assert(ip->sin_family==AF_INET&&ntohs(ip->sin_port)==443&&ntohl(ip->sin_addr.s_addr)==0x7f000001);
 assert(list->ai_canonname&&!strcmp(list->ai_canonname,"127.0.0.1"));
 android_freeaddrinfo(list);list=NULL;
 hints.ai_flags=4|8;
 assert(android_getaddrinfo("not-a-numeric-address","443",&hints,&list)==8&&!list);
 hints.ai_flags=0x10000;
 assert(android_getaddrinfo("127.0.0.1","443",&hints,&list)==3&&!list);
 assert(strstr(android_gai_strerror(8),"name")||strstr(android_gai_strerror(8),"Name"));
 android_freeaddrinfo(NULL);
 puts("ANDROID_NETDB_TEST PASS numeric_ipv4=1 port=443 canonical=1 numeric_flags=1 bad_flags=1 guest_EAI=1 paired_free=1");
}
