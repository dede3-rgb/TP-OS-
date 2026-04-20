#ifndef GESCOM_H
#define GESCOM_H

#define NBMAXC 16
#define MAXMOTS 128

typedef struct {
    const char *nom;
    int (*fonction)(int, char **);
} CommandInt;

extern char *Mots[MAXMOTS];
extern int NMots;

char *copyString(const char *s);
void freeMots(void);
int analyseCom(char *b);

void ajouteCom(const char *nom, int (*f)(int, char **));
int execComInt(int n, char **p);
int execComExt(char **p);

int ChangeDir(int n, char **p);
int PrintDir(int n, char **p);
int Version(int n, char **p);

#endif