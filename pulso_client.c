#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>
#include "structs.h"

// Función para leer la entrada del usuario con un timeout (usando select)
int read_with_timeout(char *buf, int maxlen, int timeout_secs) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {timeout_secs, 0};
    
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        return fgets(buf, maxlen, stdin) != NULL;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Uso: %s <IP> <PUERTO>\n", argv[0]);
        return 1;
    }

    // 1. Conexión TCP
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(atoi(argv[2]));
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Fallo en la conexion");
        return 1;
    }
    printf("Conectado al servidor %s:%s\n", argv[1], argv[2]);

    // 2. Handshake y envío de nombre
    char name[MAX_NAME];
    printf("Ingresa tu nombre (max. 31 caracteres): ");
    fgets(name, MAX_NAME, stdin);
    name[strcspn(name, "\n")] = '\0'; // Eliminar salto de linea
    send(sock, name, MAX_NAME, 0);

    Handshake hs;
    recv(sock, &hs, sizeof(Handshake), 0);

    printf("\nBienvenido, %s\nEres el Jugador #%d\nPartida con %d jugadores\n", name, hs.player_id, hs.n_players);
    printf("Jugadores en la arena:\n");
    for (int i = 0; i < hs.n_players; i++) {
        printf("[%d] %s %s\n", i, hs.names[i], (i == hs.player_id) ? "<- TÚ" : "");
    }
    printf("\nEsperando inicio de partida...\n");

    long lamport = 0; // Reloj de Lamport local del cliente

    // 3. Bucle principal de juego
    while (1) {
        GameState gs;
        // Recibir estado del servidor
        if (recv(sock, &gs, sizeof(GameState), 0) <= 0) {
            printf("Desconectado del servidor.\n");
            break;
        }

        // Verificar condición de término de partida
        if (gs.winner_id >= 0) {
            printf("\n======= FIN DE LA PARTIDA =======\n");
            for (int i = 0; i < gs.n_players; i++) {
                printf("[%d] %-15s HP: %d\n", i, gs.names[i], gs.hp[i]);
            }
            if (gs.winner_id == hs.player_id) printf("\n*** GANASTE! Eres el ultimo en pie. ***\n");
            else printf("\nGanador: %s\n", gs.names[gs.winner_id]);
            break;
        } else if (gs.winner_id == -2) {
            printf("\n*** EMPATE! Todos murieron ***\n");
            break;
        }

        // Mostrar HUD (Estado de la ronda)
        printf("\n--- Ronda %d %s ---\n", gs.round, gs.fury_active ? "[FURIA ACTIVA]" : "");
        for (int i = 0; i < gs.n_players; i++) {
            printf("[%d] %-15s HP: %2d %s %s\n", i, gs.names[i], gs.hp[i], 
                   (i == hs.player_id) ? "<- TÚ" : "", 
                   gs.alive[i] ? "" : "(eliminado)");
        }

        // Leer acción solo si el jugador está vivo
        if (gs.alive[hs.player_id]) {
            printf("\nAcciones:\n A <id> Atacar (-3 HP)\n E      Esquivar (inmune)\n S <id> Superataque (-6 HP)\n C      Curar (+2 HP)\n");
            printf("\nAcción [%d s]: ", ROUND_SECS);
            fflush(stdout);

            Action act;
            act.player_id = hs.player_id;
            act.target_id = -1;
            act.lamport_ts = ++lamport;

            char input[32];
            // Capturar input o procesar timeout
            if (read_with_timeout(input, sizeof(input), ROUND_SECS)) {
                char type = 'E';
                int target = -1;
                sscanf(input, "%c %d", &type, &target);
                
                if (type == 'A' || type == 'a') act.action = ACT_ATACAR;
                else if (type == 'S' || type == 's') act.action = ACT_SUPERATAQUE;
                else if (type == 'C' || type == 'c') act.action = ACT_CURAR;
                else act.action = ACT_ESQUIVAR;
                
                act.target_id = target;

                // --- VALIDACIÓN: Evitar auto-ataque ---
                if ((act.action == ACT_ATACAR || act.action == ACT_SUPERATAQUE) && act.target_id == hs.player_id) {
                    printf("\n[!] Movimiento inválido: No puedes atacarte a ti mismo. Tu acción cambia a ESQUIVAR.\n");
                    act.action = ACT_ESQUIVAR;
                }
                
                send(sock, &act, sizeof(Action), 0);
            } else {
                // Si el tiempo expira
                printf("\n[TIMEOUT] Sin respuesta - acción automática: ESQUIVAR\n");
                lamport++; // Incremento por la acción local nula
            }
        } else {
            // Si está eliminado, solo espera como espectador
            printf("\nEstas eliminado. Esperando a que termine la partida...\n");
        }
    }

    close(sock);
    return 0;
}
