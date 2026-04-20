#ifndef CREME_H
#define CREME_H

#include <pthread.h>

#define BEUIP_PORT 9998
#define BEUIP_MAGIC "BEUIP"
#define MAX_MSG 1024
#define MAX_PSEUDO 64
#define LPSEUDO 23
#define DEFAULT_BROADCAST_IP "192.168.88.255"
#define PUBLIC_DIR "reppub"

struct elt {
    char nom[LPSEUDO + 1];
    char adip[16];
    struct elt *next;
};

int BeuipCmd(int n, char **p);
void creme_cleanup(void);

void ajouteElt(char *pseudo, char *adip);
void supprimeElt(char *adip);
void listeElts(void);

#endif