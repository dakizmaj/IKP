#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#include "../EnergySystem/common.h"

#define ROOT_AGR_PORT 5000
#define SOURCE_PORT 5999  /* ✅ Source port */
#define RESPONSE_TIMEOUT_MS 10000  /* ✅ Povećan timeout na 10s */

uint32_t next_request_id = 1;

void print_menu() {
    uint32_t i;
    printf("\n========================================\n");
    printf("  ENERGY DISTRIBUTION SYSTEM - SOURCE\n");
    printf("========================================\n");
    printf("  Izaberite cilj za zahtev:\n\n");
    printf("  [0] Svi agregatori (broadcast)\n");
    for (i = 0; i < NUM_AGGREGATORS; i++) {
        printf("  [%u] Agregator %u (port %u)\n", 
               AGR_CONFIGS[i].id + 1, AGR_CONFIGS[i].id, AGR_CONFIGS[i].listen_port);
    }
    printf("  [99] Izlaz\n");
    printf("========================================\n");
    printf("Izbor: ");
}

void send_request(SOCKET sock, uint32_t target_agr) {
    EnergyRequest req;
    struct sockaddr_in agr_addr;
    
    req.type = PACKET_REQUEST;
    req.request_id = next_request_id++;
    req.target_agr = target_agr;
    req.timestamp = time(NULL);
    
    memset(&agr_addr, 0, sizeof(agr_addr));
    agr_addr.sin_family = AF_INET;
    agr_addr.sin_port = htons(ROOT_AGR_PORT);
    agr_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    printf("\n[SOURCE] Saljem zahtev #%u ka Agr 0 (cilj: ", req.request_id);
    if (target_agr == TARGET_ALL) {
        printf("SVI)\n");
    } else {
        printf("Agr %u)\n", target_agr);
    }
    
    if (sendto(sock, (const char*)&req, sizeof(req), 0,
               (struct sockaddr*)&agr_addr, sizeof(agr_addr)) == SOCKET_ERROR) {
        printf("[GRESKA] Slanje zahteva: %d\n", WSAGetLastError());
    }
}

void wait_for_response(SOCKET sock, uint32_t request_id) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in from_addr;
    int from_len = sizeof(from_addr);
    int recv_len;
    fd_set readfds;
    struct timeval timeout;
    PacketType* packet_type;
    
    printf("[SOURCE] Cekam odgovor za request #%u...\n", request_id);
    printf("[DEBUG] Listening on port 5999\n");
    
    timeout.tv_sec = RESPONSE_TIMEOUT_MS / 1000;
    timeout.tv_usec = (RESPONSE_TIMEOUT_MS % 1000) * 1000;
    
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    
    int select_result = select(0, &readfds, NULL, NULL, &timeout);
    printf("[DEBUG] select() returned: %d\n", select_result);
    
    if (select_result > 0) {
        recv_len = recvfrom(sock, buffer, BUFFER_SIZE, 0,
                           (struct sockaddr*)&from_addr, &from_len);
        
        printf("[DEBUG] Received %d bytes from %s:%d\n", 
               recv_len, inet_ntoa(from_addr.sin_addr), ntohs(from_addr.sin_port));
        
        if (recv_len == sizeof(EnergyResponse)) {
            packet_type = (PacketType*)buffer;
            
            if (*packet_type == PACKET_RESPONSE) {
                EnergyResponse* resp = (EnergyResponse*)buffer;
                
                printf("[DEBUG] Response: ID=%u, Agr=%u, Energy=%.2f, Consumers=%u\n",
                       resp->request_id, resp->aggregator_id, 
                       resp->total_energy, resp->consumer_count);
                
                if (resp->request_id == request_id) {
                    printf("\n========================================\n");
                    printf("  ODGOVOR PRIMLJEN\n");
                    printf("========================================\n");
                    printf("  Request ID: %u\n", resp->request_id);
                    printf("  Aggregator ID: %u\n", resp->aggregator_id);
                    printf("  Ukupna energija: %.2f kW\n", resp->total_energy);
                    printf("  Broj potrosaca: %u\n", resp->consumer_count);
                    printf("  Broj paketa: %u\n", resp->packet_count);
                    printf("========================================\n");
                } else {
                    printf("[UPOZORENJE] Wrong request ID: %u (expected %u)\n",
                           resp->request_id, request_id);
                }
            } else {
                printf("[GRESKA] Wrong packet type: %d\n", *packet_type);
            }
        } else {
            printf("[GRESKA] Wrong size: %d (expected %zu)\n", 
                   recv_len, sizeof(EnergyResponse));
        }
    } else if (select_result == 0) {
        printf("[TIMEOUT] No response after %d ms\n", RESPONSE_TIMEOUT_MS);
    } else {
        printf("[GRESKA] select() error: %d\n", WSAGetLastError());
    }
}

int main() {
    WSADATA wsa;
    SOCKET sock;
    struct sockaddr_in local_addr;
    int choice;
    uint32_t target_agr;
    uint32_t request_id;
    
    printf("========================================\n");
    printf("  DATA SOURCE - Kontrolna stanica\n");
    printf("========================================\n\n");
    
    /* Inicijalizacija Winsock */
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[GRESKA] Winsock: %d\n", WSAGetLastError());
        return 1;
    }
    
    /* Kreiranje soketa */
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        printf("[GRESKA] Soket: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }
    
    /* ✅ Bind na SOURCE_PORT */
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(SOURCE_PORT);
    local_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr)) == SOCKET_ERROR) {
        printf("[GRESKA] Bind na port %d: %d\n", SOURCE_PORT, WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    
    printf("[OK] Pokrenut na portu %d\n", SOURCE_PORT);
    
    /* Interaktivna petlja */
    while (1) {
        print_menu();
        
        if (scanf("%d", &choice) != 1) {
            while (getchar() != '\n');
            printf("[GRESKA] Neispravan unos!\n");
            continue;
        }
        
        if (choice == 99) {
            break;
        }
        
        if (choice == 0) {
            target_agr = TARGET_ALL;
        } else if (choice >= 1 && choice <= (int)NUM_AGGREGATORS) {
            target_agr = choice - 1;
        } else {
            printf("[GRESKA] Neispravan izbor!\n");
            continue;
        }
        
        request_id = next_request_id;
        send_request(sock, target_agr);
        wait_for_response(sock, request_id);
    }
    
    closesocket(sock);
    WSACleanup();
    printf("\n[SOURCE] Zavrsen rad.\n");
    return 0;
}
