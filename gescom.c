#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "gescom.h"

char *Mots[MAXMOTS];
int NMots = 0;

static CommandInt tabComInt[NBMAXC];
static int nbComActual = 0;

char *copyString(const char *s) {
    if (s == NULL) {
        return NULL;
    }

    char *copy = malloc(strlen(s) + 1);
    if (copy == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    strcpy(copy, s);
    return copy;
}

void freeMots(void) {
    for (int i = 0; i < NMots; i++) {
        free(Mots[i]);
        Mots[i] = NULL;
    }
    NMots = 0;
}

int analyseCom(char *b) {
    freeMots();

    char *token;
    while ((token = strsep(&b, " \t\n")) != NULL) {
        if (*token == '\0') {
            continue;
        }

        if (NMots >= MAXMOTS - 1) {
            fprintf(stderr, "Trop d'arguments\n");
            break;
        }

        Mots[NMots++] = copyString(token);
    }

    Mots[NMots] = NULL;
    return NMots;
}

void ajouteCom(const char *nom, int (*f)(int, char **)) {
    if (nbComActual >= NBMAXC) {
        fprintf(stderr, "Trop de commandes internes\n");
        exit(EXIT_FAILURE);
    }

    tabComInt[nbComActual].nom = nom;
    tabComInt[nbComActual].fonction = f;
    nbComActual++;
}

int execComInt(int n, char **p) {
    if (n == 0 || p[0] == NULL) {
        return 0;
    }

    for (int i = 0; i < nbComActual; i++) {
        if (strcmp(p[0], tabComInt[i].nom) == 0) {
            return tabComInt[i].fonction(n, p);
        }
    }

    return 0;
}

int execComExt(char **p) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        execvp(p[0], p);
        perror("execvp");
        _exit(EXIT_FAILURE);
    }

    waitpid(pid, NULL, 0);
    return 1;
}

int ChangeDir(int n, char **p) {
    const char *dest;

    if (n < 2) {
        dest = getenv("HOME");
        if (dest == NULL) {
            fprintf(stderr, "cd: HOME non défini\n");
            return 1;
        }
    } else {
        dest = p[1];
    }

    if (chdir(dest) != 0) {
        perror("cd");
    }

    return 1;
}

int PrintDir(int n, char **p) {
    (void)n;
    (void)p;

    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd");
        return 1;
    }

    printf("%s\n", cwd);
    return 1;
}

int Version(int n, char **p) {
    (void)n;
    (void)p;
    printf("biceps version 3.0\n");
    return 1;
}