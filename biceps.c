#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#include "gescom.h"
#include "creme.h"

static int Sortie(int n, char **p) {
    (void)n;
    (void)p;
    creme_cleanup();
    freeMots();
    printf("Exiting biceps... Goodbye!\n");
    exit(0);
}

static void handle_sigint(int sig) {
    (void)sig;
    write(STDOUT_FILENO, "\n", 1);
}

static void majComInt(void) {
    ajouteCom("exit", Sortie);
    ajouteCom("cd", ChangeDir);
    ajouteCom("pwd", PrintDir);
    ajouteCom("vers", Version);
    ajouteCom("beuip", BeuipCmd);
}

static char *build_prompt(void) {
    char hostname[256];
    char *user = getenv("USER");
    char symbol = (geteuid() == 0) ? '#' : '$';

    if (gethostname(hostname, sizeof(hostname)) != 0) {
        strcpy(hostname, "machine");
    }

    if (user == NULL) {
        user = "user";
    }

    size_t size = strlen(user) + strlen(hostname) + 4;
    char *prompt = malloc(size);
    if (prompt == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    snprintf(prompt, size, "%s@%s%c ", user, hostname, symbol);
    return prompt;
}

int main(void) {
    char buffer[1024];
    char *prompt_str;

    signal(SIGINT, handle_sigint);
    majComInt();

    while (1) {
        prompt_str = build_prompt();
        printf("%s", prompt_str);
        fflush(stdout);

        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            free(prompt_str);
            Sortie(0, NULL);
        }

        free(prompt_str);

        buffer[strcspn(buffer, "\n")] = '\0';

        if (buffer[0] == '\0') {
            continue;
        }

        int count = analyseCom(buffer);
        if (count > 0 && !execComInt(count, Mots)) {
            execComExt(Mots);
        }
    }

    return 0;
}