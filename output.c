/*
 *  Copyright (c) 2018 Thierry Leconte
 *
 *   
 *   This code is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License version 2
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <inttypes.h>
#include <pthread.h>
#include <math.h>

extern int initAirspy(void);
extern int startAirspy(void);
extern void stopAirspy(void);
extern void closeAirspy(void);
extern char* filename;
extern void *fileInput(void *arg);


char *Rawaddr = NULL;
int outmode = 0;
int outformat;

static int sockfd = -1;
static int do_exit=0;

typedef struct blk_s blk_t;
struct blk_s {
        uint8_t frame[112];
        int len;
        uint64_t ts;
	uint8_t lvl;
        blk_t *next;
};
static blk_t *blkhead;
static blk_t *blkend;
static pthread_mutex_t blkmtx;
static pthread_cond_t blkwcd;


static int initNet(void)
{
	char *addr;
	char *port;
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char *Caddr;

	if(Rawaddr==NULL)
		return -1;

	Caddr=strdup(Rawaddr);

	memset(&hints, 0, sizeof hints);
	if (Caddr[0] == '[') {
		hints.ai_family = AF_INET6;
		addr = Caddr + 1;
		port = strstr(addr, "]");
		if (port == NULL) {
			fprintf(stderr, "Invalid IPV6 address\n");
			free(Caddr);
			return -1;
		}
		*port = 0;
		port++;
		if (*port != ':')
			port = (outmode==1)?"30001":"30002";
		else
			port++;
	} else {
		hints.ai_family = AF_UNSPEC;
		addr = Caddr;
		port = strstr(addr, ":");
		if (port == NULL)
			port = (outmode==1)?"30001":"30002";
		else {
			*port = 0;
			port++;
		}
	}

	hints.ai_socktype = SOCK_STREAM;

	if ((rv = getaddrinfo(addr, port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "Invalid/unknown address %s\n", addr);
		free(Caddr);
		return -1;
	}	
	free(Caddr);

	for (p = servinfo; p != NULL; p = p->ai_next) {
		int lsock;

		if ((lsock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
			continue;
		}

		if(outmode == 1) {
			if (connect(lsock, p->ai_addr, p->ai_addrlen) == -1) {
				close(lsock);
				sockfd=-1;
				continue;
			}
			sockfd=lsock;
			fprintf(stderr, "connected\n");
		}

		if(outmode == 2) {
			if (bind(lsock, p->ai_addr, p->ai_addrlen) == -1) {
				close(lsock);
				continue;
			}

			if (listen(lsock, 1) == -1) {
				close(lsock);
				continue;
			}
	
			fprintf(stderr, "listening\n");
			sockfd=accept(lsock,NULL,NULL);
			if(sockfd == -1) {
				close(lsock);
				sockfd=-1;
				continue;
			}
			close(lsock);
			fprintf(stderr, "connected\n");
		}
		break;
	}

	freeaddrinfo(servinfo);

	if (p == NULL) {
		return 1;
	}

	return 0;
}

void netout(const uint8_t *frame, const int len, const uint64_t ts,const uint32_t pw)
{
   blk_t *blk;
   uint32_t lvl;

   blk=malloc(sizeof(blk_t));

   memcpy(blk->frame,frame,len);
   blk->len=len;
   blk->ts=ts;
   lvl=(uint32_t)sqrt((float)pw)>>6;
   if(lvl>255) lvl=255;
   blk->lvl=(uint8_t)lvl;
   blk->next=NULL;
	
   pthread_mutex_lock(&blkmtx);

   if(blkend) 
   	blkend->next=blk;
   blkend=blk;
   if(blkhead==NULL)
	blkhead=blk;

   pthread_cond_signal(&blkwcd);
   pthread_mutex_unlock(&blkmtx);

}


static void freelist(void)
{

   blk_t *blk;

   pthread_mutex_lock(&blkmtx);

   blk=blkhead;
   while(blk) {
   	blk_t *blknext;
	blknext=blk->next;
	free(blk);
	blk=blknext;	
   }

   blkhead=blkend=NULL;
   pthread_mutex_unlock(&blkmtx);
}

int formatpkt(blk_t *blk,char *pkt)
{
    int i,o,len;
    char *p;
    char ch;

    p=pkt;
    switch (outformat) {
	case 0:
		sprintf(pkt, "*");
		o = 1;
		break;
	case 1:
		sprintf(pkt, "@%012" PRIX64, (12*(blk->ts & 0xffffffffffffff)/20) & 0xffffffffffff);
		o = 13;
		break;
	default:
		o=0;
		*p++ = 0x1a;
		if(blk->len==7) *p++ = '2'; else *p++ = '3';	// msg type
		for(i=40;i>=0;i-=8) {
			*p++ = (ch = (blk->ts >> i));
			if (0x1a == ch) {
        			*p++ = ch;
    			}
		}
		*p++ = blk->lvl; 
		break;
      }

      if(outformat<2)  {
      	for (i = 0; i < blk->len; i++) {
			sprintf(&(pkt[2 * i + o]), "%02X", blk->frame[i]);
      	}
      	strcat(pkt, ";\n");
	len=strlen(pkt);
     } else {
        for(i=0;i<blk->len;i++) {
                *p++ = (ch = blk->frame[i]);
                if (0x1a == ch) {
                        *p++ = ch;
                }
        }
	len=p-pkt;
     }

     return len;
}

int runOutput(void)
{
   blk_t *blk;
   char pkt[256];
   int res,len;

   pthread_mutex_init(&blkmtx, NULL);
   pthread_cond_init(&blkwcd, NULL);
   blkhead=blkend=NULL;

   if(filename) {
        pthread_t th;
        pthread_create(&th, NULL, fileInput, NULL);
    } else {
     if(initAirspy()<0)
	return -1;
     if(startAirspy()<0)
	return -1;
    }


   do {

	if(do_exit) 
		break;

        if (outmode && sockfd < 0) {
		if(initNet()<0)
			return -1;
		if (sockfd < 0) {
			sleep(3);
			continue;
		}
		if(startAirspy()<0)
			return -1;
	}

      pthread_mutex_lock(&blkmtx);
      while (blkhead == NULL && do_exit==0)
          pthread_cond_wait(&blkwcd, &blkmtx);

	if(do_exit)
                break;

      blk = blkhead;
      if(blkend==blk)
        blkend=NULL;

      blkhead=blk->next;

      pthread_mutex_unlock(&blkmtx);

      len=formatpkt(blk,pkt);

      if (outmode) {
		while(len) {
			res = write(sockfd, pkt, len);
			if (res <= 0) {
				fprintf(stderr,"disconnected\n");
				close(sockfd);
				sockfd = -1;
				stopAirspy();
				freelist();
				break;
			}
			len-=res;
		}
	} else {
		fwrite(pkt, strlen(pkt), 1, stdout);
		fflush(stdout);
	}

     free(blk);
   } while (1);

   closeAirspy();
   return 0;
}

void handlerExit(int sig)
{

   do_exit=1;
   pthread_cond_signal(&blkwcd);

   stopAirspy();
   if(sockfd>0)  {
        close(sockfd);
	sockfd=-1;
   }

}
