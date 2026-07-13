#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <sys/select.h>
#include <signal.h>
#include "structs.h"

void run_gateway(int n_players);
void run_player_manager(int rank, int n_players);

int main(int argc, char *argv[]) {
    // Ignorar la señal de tubería rota si un cliente se desconecta abruptamente
    signal(SIGPIPE, SIG_IGN);

    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (argc < 2) {
        if (rank == 0) printf("Uso: mpirun -np N+1 %s N\n", argv[0]);
        MPI_Finalize();
        return 1;
    }

    int n_players = atoi(argv[1]);
    if (n_players < 2 || n_players > MAX_PLAYERS || size != n_players + 1) {
        if (rank == 0) printf("[ERROR] Se requieren %d ranks (n+1).\n", n_players + 1);
        MPI_Finalize();
        return 1;
    }

    if (rank == 0) run_gateway(n_players);
    else run_player_manager(rank, n_players);

    MPI_Finalize();
    return 0;
}

// -----------------------------------------------------
// Rank 0: Gateway TCP
// -----------------------------------------------------
void run_gateway(int n_players) {
    int server_fd, client_socks[MAX_PLAYERS];
    struct sockaddr_in addr;
    GameState gs;
    Action actions[MAX_PLAYERS];

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT_DEFAULT);

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, n_players);

    printf("[Servidor Rank 0] Escuchando en puerto %d - esperando %d jugadores...\n", PORT_DEFAULT, n_players);

    memset(&gs, 0, sizeof(gs));
    gs.n_players = n_players;
    gs.winner_id = -1;
    for (int i = 0; i < n_players; i++) {
        gs.hp[i] = HP_INITIAL;
        gs.alive[i] = 1;
    }

    // Aceptar conexiones
    for (int i = 0; i < n_players; i++) {
        client_socks[i] = accept(server_fd, NULL, NULL);
        recv(client_socks[i], gs.names[i], MAX_NAME, 0);
        printf("[Servidor] Jugador %d conectado: %s\n", i, gs.names[i]);
    }
    printf("[Servidor] Todos conectados. ¡Comienza PULSO!\n");

    // Enviar Handshake
    for (int i = 0; i < n_players; i++) {
        Handshake hs;
        hs.player_id = i;
        hs.n_players = n_players;
        memcpy(hs.names, gs.names, sizeof(hs.names));
        send(client_socks[i], &hs, sizeof(Handshake), 0);
    }

    // Sincronizar estado inicial con Ranks de MPI
    MPI_Bcast(&gs, sizeof(GameState), MPI_BYTE, 0, MPI_COMM_WORLD);

    // Enviar estado inicial a los clientes TCP
    printf("[Servidor] Enviando estado inicial a clientes...\n");
    fflush(stdout);
    for (int i = 0; i < n_players; i++) {
        send(client_socks[i], &gs, sizeof(GameState), 0);
    }

    long lamport = 0;

    // Bucle principal de rondas
    while (gs.winner_id == -1) {
        gs.round++;
        int received[MAX_PLAYERS] = {0};

        time_t start_time = time(NULL);
        while (time(NULL) - start_time < ROUND_SECS) {
            fd_set readfds; 
            FD_ZERO(&readfds);
            int max_fd = 0, pending = 0;

            for (int i = 0; i < n_players; i++) {
                if (gs.alive[i] && !received[i]) {
                    FD_SET(client_socks[i], &readfds);
                    if (client_socks[i] > max_fd) max_fd = client_socks[i];
                    pending++;
                }
            }

            if (pending == 0) break; // Todos enviaron su acción

            struct timeval tv;
            tv.tv_sec = ROUND_SECS - (time(NULL) - start_time);
            if (tv.tv_sec < 0) tv.tv_sec = 0;
            tv.tv_usec = 0;

            int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
            if (ret > 0) {
                for (int i = 0; i < n_players; i++) {
                    if (gs.alive[i] && !received[i] && FD_ISSET(client_socks[i], &readfds)) {
                        Action tmp;
                        if (recv(client_socks[i], &tmp, sizeof(Action), 0) == sizeof(Action)) {
                            actions[i] = tmp;
                            received[i] = 1;
                            lamport = (tmp.lamport_ts > lamport ? tmp.lamport_ts : lamport) + 1;
                            actions[i].lamport_ts = tmp.lamport_ts; // Sincronizar timestamp
                        } else {
                            received[i] = 1; gs.alive[i] = 0; // Desconexión
                        }
                    }
                }
            }
        }

        // Asignar TIMEOUT a quienes no respondieron o están muertos
        for (int i = 0; i < n_players; i++) {
            if (gs.alive[i] && !received[i]) {
                actions[i].player_id = i;
                actions[i].action = ACT_TIMEOUT;
                actions[i].target_id = -1;
                actions[i].lamport_ts = ++lamport;
            } else if (!gs.alive[i]) {
                actions[i].player_id = i;
                actions[i].action = ACT_TIMEOUT;
                actions[i].lamport_ts = 999999; // Jugadores eliminados van al final
            }
        }

        // Ordenar acciones por Lamport (Bubble sort simple)
        for (int i = 0; i < n_players - 1; i++) {
            for (int j = 0; j < n_players - i - 1; j++) {
                if (actions[j].lamport_ts > actions[j+1].lamport_ts) {
                    Action tmp = actions[j];
                    actions[j] = actions[j+1];
                    actions[j+1] = tmp;
                }
            }
        }

        // Repartir acciones ordenadas a todos los ranks
        MPI_Bcast(actions, sizeof(Action) * n_players, MPI_BYTE, 0, MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);

        // Recolectar HP calculado por los managers
        int local_hp = 0; 
        int all_hp[MAX_PLAYERS + 1];
        MPI_Gather(&local_hp, 1, MPI_INT, all_hp, 1, MPI_INT, 0, MPI_COMM_WORLD);

        // Calcular vida global para la Furia Colectiva
        int total_hp = 0;
        MPI_Allreduce(&local_hp, &total_hp, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

        gs.fury_active = (total_hp < 0.30 * (n_players * HP_INITIAL));

        // Actualizar GameState e identificar ganadores
        int alive_count = 0, last_alive = -1;
        for (int i = 0; i < n_players; i++) {
            gs.hp[i] = all_hp[i + 1];
            gs.alive[i] = (gs.hp[i] > 0) ? 1 : 0;
            if (gs.alive[i]) { alive_count++; last_alive = i; }
        }

        if (alive_count <= 1) {
            gs.winner_id = (alive_count == 1) ? last_alive : -2;
        }

        // Notificar a los managers si el juego terminó
        MPI_Bcast(&gs.winner_id, 1, MPI_INT, 0, MPI_COMM_WORLD); 
        
        // Enviar estado actualizado a todos los clientes TCP
        for (int i = 0; i < n_players; i++) {
            send(client_socks[i], &gs, sizeof(GameState), 0);
        }
        
        MPI_Barrier(MPI_COMM_WORLD);

        // Imprimir Historial en consola del servidor
        printf("\n--- Ronda %d ---\n", gs.round);
        for (int i = 0; i < n_players; i++) {
            Action a;
            for (int k = 0; k < n_players; k++) if (actions[k].player_id == i) a = actions[k];
            
            char act_str[64] = "ESQUIVA";
            if (a.action == ACT_ATACAR) sprintf(act_str, "ATACA a %d", a.target_id);
            else if (a.action == ACT_SUPERATAQUE) sprintf(act_str, "SUPERATACA a %d", a.target_id);
            else if (a.action == ACT_CURAR) strcpy(act_str, "CURA");
            else if (a.action == ACT_TIMEOUT) strcpy(act_str, "TIMEOUT (ESQUIVAR)");

            if (gs.alive[i] || a.action != ACT_TIMEOUT) {
                printf("[%d] %-10s HP:%d %-18s (Lamport:%ld)\n", i, gs.names[i], gs.hp[i], act_str, a.lamport_ts);
            }
        }
    }
    
    if (gs.winner_id >= 0) printf("\n[Servidor] Ganador: %s\n", gs.names[gs.winner_id]);
    else printf("\n[Servidor] Empate total.\n");

    for (int i = 0; i < n_players; i++) close(client_socks[i]);
    close(server_fd);
}

// -----------------------------------------------------
// Rank 1..N: Manager de jugador
// -----------------------------------------------------
void run_player_manager(int rank, int n_players) {
    int my_idx = rank - 1;
    int my_hp = HP_INITIAL;
    Action actions[MAX_PLAYERS];
    GameState gs;

    MPI_Bcast(&gs, sizeof(GameState), MPI_BYTE, 0, MPI_COMM_WORLD);
    int fury_active = gs.fury_active;

    while (1) {
        MPI_Bcast(actions, sizeof(Action) * n_players, MPI_BYTE, 0, MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);

        // Verificar si este jugador decidió esquivar
        int is_dodging = 0;
        for (int i = 0; i < n_players; i++) {
            if (actions[i].player_id == my_idx && (actions[i].action == ACT_ESQUIVAR || actions[i].action == ACT_TIMEOUT)) {
                is_dodging = 1;
            }
        }

        // Resolver acciones en el orden de los relojes de Lamport
        for (int i = 0; i < n_players; i++) {
            Action a = actions[i];

            // VALIDACIÓN: Evitar daño o interacciones por auto-ataque
            if ((a.action == ACT_ATACAR || a.action == ACT_SUPERATAQUE) && a.target_id == a.player_id) {
                continue; 
            }

            // Aplicar efectos si la acción es propia
            if (a.player_id == my_idx && my_hp > 0) {
                if (a.action == ACT_CURAR && my_hp > 2) {
                    my_hp += 2;
                    if (my_hp > 10) my_hp = 10;
                }
                if (a.action == ACT_SUPERATAQUE) {
                    for (int k = 0; k < n_players; k++) {
                        if (actions[k].player_id == a.target_id) {
                            if ((actions[k].action == ACT_ATACAR || actions[k].action == ACT_SUPERATAQUE) && actions[k].target_id == my_idx) {
                                my_hp -= 2; // Riesgo del superataque si chocan ataques
                            }
                        }
                    }
                }
            }

            // Aplicar efectos si este jugador es el objetivo
            if (a.target_id == my_idx && my_hp > 0) {
                if (!is_dodging) {
                    if (a.action == ACT_ATACAR) my_hp -= (fury_active ? 4 : 3);
                    else if (a.action == ACT_SUPERATAQUE) my_hp -= (fury_active ? 7 : 6);
                }
            }
        }
        
        if (my_hp < 0) my_hp = 0;

        // Enviar HP actualizado a Rank 0
        MPI_Gather(&my_hp, 1, MPI_INT, NULL, 0, MPI_INT, 0, MPI_COMM_WORLD);

        // Participar en el cálculo de Furia Colectiva
        int total_hp = 0;
        MPI_Allreduce(&my_hp, &total_hp, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
        fury_active = (total_hp < 0.30 * (n_players * HP_INITIAL));

        // Verificar si la partida terminó
        int winner_id;
        MPI_Bcast(&winner_id, 1, MPI_INT, 0, MPI_COMM_WORLD);

        MPI_Barrier(MPI_COMM_WORLD);

        if (winner_id != -1) break; 
    }
}
