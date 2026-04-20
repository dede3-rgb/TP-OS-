# TP3 OS User - biceps
- NOM : DIOP
- PRENOM : Dede Couro

## Structure
- `biceps.c` : boucle principale et commandes internes
- `gescom.c / gescom.h` : gestion des commandes du shell
- `creme.c / creme.h` : protocole BEUIP, thread UDP, liste chaînée des utilisateurs, bonus TCP pour les fichiers

## Fonctionnalités principales
- `beuip start <pseudo>` : démarre le serveur UDP et le serveur TCP
- `beuip stop` : arrête proprement les serveurs
- `beuip list` : affiche les utilisateurs présents au format `IP : pseudo`
- `beuip message <user> <message>` : envoie un message privé
- `beuip message all <message>` : envoie un message à tous

## Bonus réalisés
- `beuip ls <pseudo>` : affiche la liste des fichiers du répertoire partagé `reppub` d’un utilisateur
- `beuip get <pseudo> <nomfic>` : télécharge un fichier distant dans le répertoire local `reppub`

## Répertoire public
Le dossier `reppub/` contient les fichiers partagés et les fichiers téléchargés.

## Compilation
- `make` : produit `biceps`
- `make memory-leak` : produit `biceps-memory-leaks`
- `make clean` : supprime binaires et objets

## Vérification mémoire
Exemple :
```bash
valgrind --leak-check=full --track-origins=yes --errors-for-leak-kinds=all --error-exitcode=1 ./biceps-memory-leaks
