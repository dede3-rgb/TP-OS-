#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <signal.h>

#include "creme.h"

static struct elt *liste = NULL;
static pthread_mutex_t mutex_liste = PTHREAD_MUTEX_INITIALIZER;

static pthread_t thread_udp;
static pthread_t thread_tcp;

static int udp_actif = 0;
static int tcp_actif = 0;

static volatile sig_atomic_t stop_udp = 0;
static volatile sig_atomic_t stop_tcp = 0;

static int udp_sock = -1;
static int tcp_sock = -1;

static char my_pseudo[MAX_PSEUDO];

/* =========================
   Liste chaînée
   ========================= */

static void videListe(void) {
    pthread_mutex_lock(&mutex_liste);

    struct elt *cur = liste;
    while (cur != NULL) {
        struct elt *suiv = cur->next;
        free(cur);
        cur = suiv;
    }
    liste = NULL;

    pthread_mutex_unlock(&mutex_liste);
}

void ajouteElt(char *pseudo, char *adip) {
    if (pseudo == NULL || adip == NULL) {
        return;
    }

    pthread_mutex_lock(&mutex_liste);

    struct elt *cur = liste;
    while (cur != NULL) {
        if (strcmp(cur->nom, pseudo) == 0 || strcmp(cur->adip, adip) == 0) {
            pthread_mutex_unlock(&mutex_liste);
            return;
        }
        cur = cur->next;
    }

    struct elt *nv = malloc(sizeof(struct elt));
    if (nv == NULL) {
        perror("malloc");
        pthread_mutex_unlock(&mutex_liste);
        return;
    }

    strncpy(nv->nom, pseudo, LPSEUDO);
    nv->nom[LPSEUDO] = '\0';

    strncpy(nv->adip, adip, 15);
    nv->adip[15] = '\0';

    nv->next = NULL;

    if (liste == NULL || strcmp(nv->nom, liste->nom) < 0) {
        nv->next = liste;
        liste = nv;
        pthread_mutex_unlock(&mutex_liste);
        return;
    }

    struct elt *prec = liste;
    cur = liste->next;

    while (cur != NULL && strcmp(cur->nom, nv->nom) < 0) {
        prec = cur;
        cur = cur->next;
    }

    nv->next = cur;
    prec->next = nv;

    pthread_mutex_unlock(&mutex_liste);
}

void supprimeElt(char *adip) {
    if (adip == NULL) {
        return;
    }

    pthread_mutex_lock(&mutex_liste);

    struct elt *cur = liste;
    struct elt *prec = NULL;

    while (cur != NULL) {
        if (strcmp(cur->adip, adip) == 0) {
            if (prec == NULL) {
                liste = cur->next;
            } else {
                prec->next = cur->next;
            }
            free(cur);
            pthread_mutex_unlock(&mutex_liste);
            return;
        }
        prec = cur;
        cur = cur->next;
    }

    pthread_mutex_unlock(&mutex_liste);
}

void listeElts(void) {
    pthread_mutex_lock(&mutex_liste);

    struct elt *cur = liste;
    while (cur != NULL) {
        printf("%s : %s\n", cur->adip, cur->nom);
        cur = cur->next;
    }

    pthread_mutex_unlock(&mutex_liste);
}

static int chercheIpParPseudo(const char *pseudo, char *ip_out, size_t ip_out_size) {
    int trouve = 0;

    pthread_mutex_lock(&mutex_liste);

    struct elt *cur = liste;
    while (cur != NULL) {
        if (strcmp(cur->nom, pseudo) == 0) {
            strncpy(ip_out, cur->adip, ip_out_size - 1);
            ip_out[ip_out_size - 1] = '\0';
            trouve = 1;
            break;
        }
        cur = cur->next;
    }

    pthread_mutex_unlock(&mutex_liste);
    return trouve;
}

static int cherchePseudoParIp(const char *ip, char *pseudo_out, size_t pseudo_out_size) {
    int trouve = 0;

    pthread_mutex_lock(&mutex_liste);

    struct elt *cur = liste;
    while (cur != NULL) {
        if (strcmp(cur->adip, ip) == 0) {
            strncpy(pseudo_out, cur->nom, pseudo_out_size - 1);
            pseudo_out[pseudo_out_size - 1] = '\0';
            trouve = 1;
            break;
        }
        cur = cur->next;
    }

    pthread_mutex_unlock(&mutex_liste);
    return trouve;
}

/* =========================
   Outils réseau
   ========================= */

static int get_local_ipv4(char *out, size_t out_size) {
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;

    if (getifaddrs(&ifaddr) == -1) {
        return 0;
    }

    int found = 0;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
            continue;
        }

        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)out_size) != NULL) {
            found = 1;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return found;
}

static int valid_msg(const char *buf, ssize_t len) {
    if (len < 6) {
        return 0;
    }

    if (memcmp(buf + 1, BEUIP_MAGIC, 5) != 0) {
        return 0;
    }

    return 1;
}

static void send_packet(int sock, const struct sockaddr_in *dst,
                        char code, const void *payload, size_t payload_len) {
    char buf[MAX_MSG];

    if (6 + payload_len > sizeof(buf)) {
        return;
    }

    buf[0] = code;
    memcpy(buf + 1, BEUIP_MAGIC, 5);

    if (payload_len > 0) {
        memcpy(buf + 6, payload, payload_len);
    }

    (void)sendto(sock, buf, 6 + payload_len, 0,
                 (const struct sockaddr *)dst, sizeof(*dst));
}

static void send_broadcast_code(char code, const char *pseudo) {
    if (udp_sock < 0 || pseudo == NULL) {
        return;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(BEUIP_PORT);

    if (inet_aton(DEFAULT_BROADCAST_IP, &dst.sin_addr) == 0) {
        return;
    }

    send_packet(udp_sock, &dst, code, pseudo, strlen(pseudo) + 1);
}

/* =========================
   Partie messagerie
   ========================= */

static void send_private_message(const char *pseudo, const char *message) {
    char ip[16];

    if (!chercheIpParPseudo(pseudo, ip, sizeof(ip))) {
        printf("Pseudo %s introuvable\n", pseudo);
        return;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(BEUIP_PORT);

    if (inet_aton(ip, &dst.sin_addr) == 0) {
        fprintf(stderr, "IP invalide pour %s\n", pseudo);
        return;
    }

    send_packet(udp_sock, &dst, '9', message, strlen(message) + 1);
}

static void send_all_message(const char *message) {
    char local_ip[16] = "";

    (void)get_local_ipv4(local_ip, sizeof(local_ip));

    pthread_mutex_lock(&mutex_liste);

    struct elt *cur = liste;
    while (cur != NULL) {
        if (local_ip[0] == '\0' || strcmp(cur->adip, local_ip) != 0) {
            struct sockaddr_in dst;
            memset(&dst, 0, sizeof(dst));
            dst.sin_family = AF_INET;
            dst.sin_port = htons(BEUIP_PORT);

            if (inet_aton(cur->adip, &dst.sin_addr) != 0) {
                send_packet(udp_sock, &dst, '9', message, strlen(message) + 1);
            }
        }
        cur = cur->next;
    }

    pthread_mutex_unlock(&mutex_liste);
}

static void commande(char octet1, char *message, char *pseudo) {
    if (!udp_actif) {
        fprintf(stderr, "beuip: serveur non actif\n");
        return;
    }

    if (octet1 == '3') {
        listeElts();
    } else if (octet1 == '4') {
        if (pseudo == NULL || message == NULL) {
            fprintf(stderr, "beuip: paramètres invalides\n");
            return;
        }
        send_private_message(pseudo, message);
    } else if (octet1 == '5') {
        if (message == NULL) {
            fprintf(stderr, "beuip: message manquant\n");
            return;
        }
        send_all_message(message);
    }
}

/* =========================
   Thread UDP
   ========================= */

static void *serveur_udp(void *arg) {
    char *pseudo = (char *)arg;
    struct sockaddr_in servaddr;
    struct sockaddr_in srcaddr;
    socklen_t srclen = sizeof(srcaddr);
    char buf[MAX_MSG];
    ssize_t n;
    int opt = 1;

    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("socket");
        free(pseudo);
        udp_actif = 0;
        return NULL;
    }

    if (setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
    }

    if (setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(udp_sock);
        udp_sock = -1;
        free(pseudo);
        udp_actif = 0;
        return NULL;
    }

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    if (setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt");
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(BEUIP_PORT);

    if (bind(udp_sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        close(udp_sock);
        udp_sock = -1;
        free(pseudo);
        udp_actif = 0;
        return NULL;
    }

    char local_ip[16];
    if (get_local_ipv4(local_ip, sizeof(local_ip))) {
        ajouteElt(my_pseudo, local_ip);
    }

    send_broadcast_code('1', pseudo);

    while (!stop_udp) {
        n = recvfrom(udp_sock, buf, sizeof(buf) - 1, 0,
                     (struct sockaddr *)&srcaddr, &srclen);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("recvfrom");
            continue;
        }

        buf[n] = '\0';

        if (!valid_msg(buf, n)) {
            continue;
        }

        char code = buf[0];
        char *payload = buf + 6;

        char ip_source[16];
        if (inet_ntop(AF_INET, &srcaddr.sin_addr, ip_source, sizeof(ip_source)) == NULL) {
            continue;
        }

        if (code == '1') {
            ajouteElt(payload, ip_source);
            send_packet(udp_sock, &srcaddr, '2', my_pseudo, strlen(my_pseudo) + 1);
        } else if (code == '2') {
            ajouteElt(payload, ip_source);
        } else if (code == '9') {
            char pseudo_src[LPSEUDO + 1];
            if (cherchePseudoParIp(ip_source, pseudo_src, sizeof(pseudo_src))) {
                printf("Message de %s : %s\n", pseudo_src, payload);
            } else {
                printf("Message de %s : %s\n", ip_source, payload);
            }
        } else if (code == '0') {
            supprimeElt(ip_source);
        } else {
            fprintf(stderr, "BEUIP: code %c refusé\n", code);
        }
    }

    send_broadcast_code('0', my_pseudo);

    close(udp_sock);
    udp_sock = -1;
    udp_actif = 0;

    free(pseudo);
    return NULL;
}

/* =========================
   Bonus TCP : ls / get
   ========================= */

static void send_error_tcp(int fd, const char *msg) {
    dprintf(fd, "ERR %s\n", msg);
}

static int nom_fichier_valide(const char *nomfic) {
    if (nomfic == NULL || nomfic[0] == '\0') {
        return 0;
    }
    if (strchr(nomfic, '/') != NULL) {
        return 0;
    }
    if (strstr(nomfic, "..") != NULL) {
        return 0;
    }
    return 1;
}

static void envoiContenu(int fd) {
    char cmd;
    ssize_t r = read(fd, &cmd, 1);
    if (r <= 0) {
        return;
    }

    if (cmd == 'L') {
        pid_t pid = fork();
        if (pid < 0) {
            send_error_tcp(fd, "fork impossible");
            return;
        }

        if (pid == 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
            execlp("ls", "ls", "-l", PUBLIC_DIR, (char *)NULL);
            perror("execlp");
            _exit(EXIT_FAILURE);
        }

        waitpid(pid, NULL, 0);
        return;
    }

    if (cmd == 'F') {
        char nomfic[256];
        int i = 0;
        char c;

        while (i < (int)sizeof(nomfic) - 1) {
            ssize_t n = read(fd, &c, 1);
            if (n <= 0) {
                break;
            }
            if (c == '\n') {
                break;
            }
            nomfic[i++] = c;
        }
        nomfic[i] = '\0';

        if (!nom_fichier_valide(nomfic)) {
            send_error_tcp(fd, "nom de fichier invalide");
            return;
        }

        char chemin[512];
        snprintf(chemin, sizeof(chemin), "%s/%s", PUBLIC_DIR, nomfic);

        if (access(chemin, R_OK) != 0) {
            send_error_tcp(fd, "fichier distant inexistant");
            return;
        }

        pid_t pid = fork();
        if (pid < 0) {
            send_error_tcp(fd, "fork impossible");
            return;
        }

        if (pid == 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
            execlp("cat", "cat", chemin, (char *)NULL);
            perror("execlp");
            _exit(EXIT_FAILURE);
        }

        waitpid(pid, NULL, 0);
        return;
    }

    send_error_tcp(fd, "commande inconnue");
}

static void *serveur_tcp(void *arg) {
    (void)arg;

    int opt = 1;
    struct sockaddr_in servaddr;

    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) {
        perror("socket");
        tcp_actif = 0;
        return NULL;
    }

    if (setsockopt(tcp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(BEUIP_PORT);

    if (bind(tcp_sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        close(tcp_sock);
        tcp_sock = -1;
        tcp_actif = 0;
        return NULL;
    }

    if (listen(tcp_sock, 5) < 0) {
        perror("listen");
        close(tcp_sock);
        tcp_sock = -1;
        tcp_actif = 0;
        return NULL;
    }

    while (!stop_tcp) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(tcp_sock, &rfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(tcp_sock + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            continue;
        }

        if (ret == 0) {
            continue;
        }

        int fd = accept(tcp_sock, NULL, NULL);
        if (fd < 0) {
            perror("accept");
            continue;
        }

        envoiContenu(fd);
        close(fd);
    }

    close(tcp_sock);
    tcp_sock = -1;
    tcp_actif = 0;
    return NULL;
}

static int connect_tcp_pseudo(const char *pseudo, int *sock_out) {
    char ip[16];

    if (!chercheIpParPseudo(pseudo, ip, sizeof(ip))) {
        printf("Pseudo %s introuvable\n", pseudo);
        return 0;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 0;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(BEUIP_PORT);

    if (inet_aton(ip, &dst.sin_addr) == 0) {
        fprintf(stderr, "IP invalide pour %s\n", pseudo);
        close(sock);
        return 0;
    }

    if (connect(sock, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        perror("connect");
        close(sock);
        return 0;
    }

    *sock_out = sock;
    return 1;
}

static void demandeListe(const char *pseudo) {
    int sock;
    if (!connect_tcp_pseudo(pseudo, &sock)) {
        return;
    }

    char demande = 'L';
    if (write(sock, &demande, 1) != 1) {
        perror("write");
        close(sock);
        return;
    }

    char buf[1024];
    ssize_t n;

    while ((n = read(sock, buf, sizeof(buf))) > 0) {
        (void)write(STDOUT_FILENO, buf, (size_t)n);
    }

    if (n < 0) {
        perror("read");
    }

    close(sock);
}

static void demandeFichier(const char *pseudo, const char *nomfic) {
    if (!nom_fichier_valide(nomfic)) {
        fprintf(stderr, "Nom de fichier invalide\n");
        return;
    }

    char chemin_local[512];
    snprintf(chemin_local, sizeof(chemin_local), "%s/%s", PUBLIC_DIR, nomfic);

    if (access(chemin_local, F_OK) == 0) {
        fprintf(stderr, "Le fichier local %s existe déjà\n", chemin_local);
        return;
    }

    int sock;
    if (!connect_tcp_pseudo(pseudo, &sock)) {
        return;
    }

    char requete[300];
    snprintf(requete, sizeof(requete), "F%s\n", nomfic);

    if (write(sock, requete, strlen(requete)) < 0) {
        perror("write");
        close(sock);
        return;
    }

    char firstbuf[1024];
    ssize_t nfirst = read(sock, firstbuf, sizeof(firstbuf));

    if (nfirst < 0) {
        perror("read");
        close(sock);
        return;
    }

    if (nfirst == 0) {
        fprintf(stderr, "Aucune donnée reçue\n");
        close(sock);
        return;
    }

    if (nfirst >= 4 && strncmp(firstbuf, "ERR ", 4) == 0) {
        (void)write(STDERR_FILENO, firstbuf, (size_t)nfirst);
        close(sock);
        return;
    }

    int fd = open(chemin_local, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        close(sock);
        return;
    }

    if (write(fd, firstbuf, (size_t)nfirst) != nfirst) {
        perror("write");
        close(fd);
        close(sock);
        unlink(chemin_local);
        return;
    }

    char buf[1024];
    ssize_t n;

    while ((n = read(sock, buf, sizeof(buf))) > 0) {
        if (write(fd, buf, (size_t)n) != n) {
            perror("write");
            close(fd);
            close(sock);
            unlink(chemin_local);
            return;
        }
    }

    if (n < 0) {
        perror("read");
        close(fd);
        close(sock);
        unlink(chemin_local);
        return;
    }

    close(fd);
    close(sock);

    printf("Fichier récupéré dans %s\n", chemin_local);
}

/* =========================
   Start / stop / cleanup
   ========================= */

static int beuip_start(const char *pseudo) {
    if (udp_actif || tcp_actif) {
        fprintf(stderr, "beuip: serveur déjà lancé\n");
        return 1;
    }

    struct stat st;
    if (stat(PUBLIC_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "beuip: le répertoire %s n'existe pas\n", PUBLIC_DIR);
        return 1;
    }

    videListe();
    stop_udp = 0;
    stop_tcp = 0;

    strncpy(my_pseudo, pseudo, MAX_PSEUDO - 1);
    my_pseudo[MAX_PSEUDO - 1] = '\0';

    char *pseudo_copy = malloc(strlen(pseudo) + 1);
    if (pseudo_copy == NULL) {
        perror("malloc");
        return 1;
    }
    strcpy(pseudo_copy, pseudo);

    if (pthread_create(&thread_udp, NULL, serveur_udp, pseudo_copy) != 0) {
        perror("pthread_create");
        free(pseudo_copy);
        return 1;
    }
    udp_actif = 1;

    if (pthread_create(&thread_tcp, NULL, serveur_tcp, NULL) != 0) {
        perror("pthread_create");
        stop_udp = 1;
        (void)pthread_join(thread_udp, NULL);
        udp_actif = 0;
        return 1;
    }
    tcp_actif = 1;

    return 1;
}

static int beuip_stop(void) {
    if (!udp_actif && !tcp_actif) {
        fprintf(stderr, "beuip: aucun serveur actif\n");
        return 1;
    }

    stop_udp = 1;
    stop_tcp = 1;

    if (udp_actif) {
        if (pthread_join(thread_udp, NULL) != 0) {
            perror("pthread_join");
            return 1;
        }
        udp_actif = 0;
    }

    if (tcp_actif) {
        if (pthread_join(thread_tcp, NULL) != 0) {
            perror("pthread_join");
            return 1;
        }
        tcp_actif = 0;
    }

    videListe();
    return 1;
}

void creme_cleanup(void) {
    stop_udp = 1;
    stop_tcp = 1;

    if (udp_actif) {
        (void)pthread_join(thread_udp, NULL);
        udp_actif = 0;
    }

    if (tcp_actif) {
        (void)pthread_join(thread_tcp, NULL);
        tcp_actif = 0;
    }

    if (udp_sock >= 0) {
        close(udp_sock);
        udp_sock = -1;
    }

    if (tcp_sock >= 0) {
        close(tcp_sock);
        tcp_sock = -1;
    }

    videListe();
}

/* =========================
   Commande beuip
   ========================= */

static void build_message_from_args(int start, int n, char **p, char *out, size_t out_size) {
    out[0] = '\0';

    for (int i = start; i < n; i++) {
        size_t left = out_size - strlen(out) - 1;
        if (left == 0) {
            break;
        }

        strncat(out, p[i], left);

        if (i + 1 < n) {
            left = out_size - strlen(out) - 1;
            if (left > 0) {
                strncat(out, " ", left);
            }
        }
    }
}

int BeuipCmd(int n, char **p) {
    if (n < 2) {
        fprintf(stderr,
                "usage: beuip start <pseudo> | beuip stop | beuip list | "
                "beuip message <user|all> <message> | beuip ls <pseudo> | "
                "beuip get <pseudo> <nomfic>\n");
        return 1;
    }

    if (strcmp(p[1], "start") == 0) {
        if (n != 3) {
            fprintf(stderr, "usage: beuip start <pseudo>\n");
            return 1;
        }
        return beuip_start(p[2]);
    }

    if (strcmp(p[1], "stop") == 0) {
        return beuip_stop();
    }

    if (strcmp(p[1], "list") == 0) {
        commande('3', NULL, NULL);
        return 1;
    }

    if (strcmp(p[1], "message") == 0) {
        if (n < 4) {
            fprintf(stderr, "usage: beuip message <user|all> <message>\n");
            return 1;
        }

        char msg[512];
        build_message_from_args(3, n, p, msg, sizeof(msg));

        if (strcmp(p[2], "all") == 0) {
            commande('5', msg, NULL);
        } else {
            commande('4', msg, p[2]);
        }
        return 1;
    }

    if (strcmp(p[1], "ls") == 0) {
        if (n != 3) {
            fprintf(stderr, "usage: beuip ls <pseudo>\n");
            return 1;
        }
        demandeListe(p[2]);
        return 1;
    }

    if (strcmp(p[1], "get") == 0) {
        if (n != 4) {
            fprintf(stderr, "usage: beuip get <pseudo> <nomfic>\n");
            return 1;
        }
        demandeFichier(p[2], p[3]);
        return 1;
    }

    fprintf(stderr,
            "usage: beuip start <pseudo> | beuip stop | beuip list | "
            "beuip message <user|all> <message> | beuip ls <pseudo> | "
            "beuip get <pseudo> <nomfic>\n");
    return 1;
}