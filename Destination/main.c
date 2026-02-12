#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#pragma warning(disable: 6328)
#pragma warning(disable: 6031)
#pragma warning(disable: 28183)
#pragma warning(disable: 6001)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

#include "../EnergySystem/common.h"

#ifndef DEST_ID
#define DEST_ID 1
#endif

#define RESPONSE_THREAD_COUNT 2  /* ✅ 2 threada za generisanje odgovora */

typedef struct {
    uint32_t total_packets;
    float total_energy;
    uint32_t consumer_count;
} Stats;

/* ✅ Thread-safe request queue */
typedef struct RequestQueueItem {
    EnergyRequest request;
    struct sockaddr_in from_addr;
    struct RequestQueueItem* next;
} RequestQueueItem;

typedef struct {
    RequestQueueItem* head;
    RequestQueueItem* tail;
    int count;
    CRITICAL_SECTION lock;
    HANDLE semaphore;
    int shutdown;
} RequestQueue;

RequestQueue req_queue;
SOCKET global_sock;
DestinationConfig global_config;
volatile int running = 1;
HANDLE response_threads[RESPONSE_THREAD_COUNT];

Stats stats = {0};

float generate_random_consumption() {
    return 0.5f + ((float)(rand() % 950)) / 100.0f;
}

void simulate_consumers() {
    uint32_t i;
    uint32_t num_consumers = 5 + (rand() % 15); /* Mali broj - 5-20 */
    
    stats.consumer_count = num_consumers;
    stats.total_energy = 0.0f;
    stats.total_packets = num_consumers;
    
    for (i = 0; i < num_consumers; i++) {
        stats.total_energy += generate_random_consumption();
    }
}

/* ✅ Inicijalizacija request queue */
void init_request_queue() {
    InitializeCriticalSection(&req_queue.lock);
    req_queue.semaphore = CreateSemaphore(NULL, 0, 100, NULL);
    req_queue.head = NULL;
    req_queue.tail = NULL;
    req_queue.count = 0;
    req_queue.shutdown = 0;
}

/* ✅ Dodaj request u red */
void enqueue_request(EnergyRequest* req, struct sockaddr_in* from_addr) {
    RequestQueueItem* item = (RequestQueueItem*)malloc(sizeof(RequestQueueItem));
    if (!item) return;
    
    item->request = *req;
    item->from_addr = *from_addr;
    item->next = NULL;
    
    EnterCriticalSection(&req_queue.lock);
    
    if (req_queue.tail) {
        req_queue.tail->next = item;
    } else {
        req_queue.head = item;
    }
    req_queue.tail = item;
    req_queue.count++;
    
    LeaveCriticalSection(&req_queue.lock);
    ReleaseSemaphore(req_queue.semaphore, 1, NULL);
}

/* ✅ Uzmi request iz reda */
RequestQueueItem* dequeue_request() {
    RequestQueueItem* item = NULL;
    
    EnterCriticalSection(&req_queue.lock);
    
    if (req_queue.head) {
        item = req_queue.head;
        req_queue.head = item->next;
        if (!req_queue.head) {
            req_queue.tail = NULL;
        }
        req_queue.count--;
    }
    
    LeaveCriticalSection(&req_queue.lock);
    return item;
}

/* ✅ Slanje odgovora (thread-safe) */
void send_response(SOCKET sock, EnergyRequest* req, DestinationConfig* config) {
    EnergyResponse resp;
    struct sockaddr_in parent_addr;
    
    /* Generisi podatke */
    simulate_consumers();
    
    resp.type = PACKET_RESPONSE;
    resp.request_id = req->request_id;
    resp.aggregator_id = config->id + 100;
    resp.total_energy = stats.total_energy;
    resp.consumer_count = stats.consumer_count;
    resp.packet_count = stats.total_packets;
    resp.timestamp = time(NULL);
    
    memset(&parent_addr, 0, sizeof(parent_addr));
    parent_addr.sin_family = AF_INET;
    parent_addr.sin_port = htons(config->parent_port);
    parent_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    printf("    [DEST%u Thread] Saljem odgovor #%u roditelju (port %d)\n",
           config->id, req->request_id, config->parent_port);
    printf("      -> Energija: %.2f kW | Potrosaci: %u\n",
           resp.total_energy, resp.consumer_count);
    
    sendto(sock, (const char*)&resp, sizeof(resp), 0,
           (struct sockaddr*)&parent_addr, sizeof(parent_addr));
}

/* ✅ Response worker thread */
unsigned __stdcall response_worker(void* arg) {
    printf("[DEST%u] Response worker thread pokrenut\n", global_config.id);
    
    while (1) {
        WaitForSingleObject(req_queue.semaphore, INFINITE);
        
        if (req_queue.shutdown) break;
        
        RequestQueueItem* item = dequeue_request();
        if (item) {
            send_response(global_sock, &item->request, &global_config);
            free(item);
        }
    }
    
    printf("[DEST%u] Response worker thread zaustavljen\n", global_config.id);
    return 0;
}

/* ✅ Receiver thread */
unsigned __stdcall receiver_thread(void* arg) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    int client_len = sizeof(client_addr);
    int recv_len;
    PacketType* packet_type;
    
    printf("[DEST%u] Receiver thread pokrenut\n", global_config.id);
    
    while (running) {
        recv_len = recvfrom(global_sock, buffer, BUFFER_SIZE, 0,
                           (struct sockaddr*)&client_addr, &client_len);
        
        if (recv_len == sizeof(EnergyRequest)) {
            packet_type = (PacketType*)buffer;
            
            if (*packet_type == PACKET_REQUEST) {
                EnergyRequest* req = (EnergyRequest*)buffer;
                printf("    [DEST%u] Primljen zahtev #%u\n", global_config.id, req->request_id);
                
                /* ✅ Dodaj u queue za obradu */
                enqueue_request(req, &client_addr);
            }
        }
    }
    
    printf("[DEST%u] Receiver thread zaustavljen\n", global_config.id);
    return 0;
}

int main(int argc, char* argv[]) {
    WSADATA wsa;
    struct sockaddr_in server_addr;
    uint32_t dest_id = DEST_ID;
    HANDLE recv_thread_handle;
    int i;
    
    srand((unsigned int)time(NULL));
    
    if (argc > 1) {
        dest_id = (uint32_t)atoi(argv[1]);
        if (dest_id < 1 || dest_id > NUM_DESTINATIONS) {
            printf("[GRESKA] Nevazeci ID destinacije: %u\n", dest_id);
            return 1;
        }
    }
    
    global_config = DEST_CONFIGS[dest_id - 1];
    init_request_queue();
    
    printf("========================================\n");
    printf("  DATA DESTINATION %u (Multi-threaded)\n", global_config.id);
    printf("========================================\n");
    printf("  Port: %u | Roditelj: %u\n", global_config.listen_port, global_config.parent_port);
    printf("  Response threads: %d\n", RESPONSE_THREAD_COUNT);
    printf("========================================\n\n");
    
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[GRESKA] Winsock: %d\n", WSAGetLastError());
        return 1;
    }
    
    global_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (global_sock == INVALID_SOCKET) {
        printf("[GRESKA] Soket: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(global_config.listen_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(global_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("[GRESKA] Bind: %d\n", WSAGetLastError());
        closesocket(global_sock);
        WSACleanup();
        return 1;
    }
    
    printf("[OK] Slusa na portu %d\n\n", global_config.listen_port);
    
    /* ✅ Pokreni response worker threadove */
    memset(response_threads, 0, sizeof(response_threads));
    for (i = 0; i < RESPONSE_THREAD_COUNT; i++) {
        response_threads[i] = (HANDLE)_beginthreadex(NULL, 0, response_worker, NULL, 0, NULL);
        if (response_threads[i] == 0) {
            printf("[GRESKA] Ne mogu pokrenuti response thread %d\n", i);
            return 1;
        }
    }
    
    /* ✅ Pokreni receiver thread */
    recv_thread_handle = (HANDLE)_beginthreadex(NULL, 0, receiver_thread, NULL, 0, NULL);
    if (recv_thread_handle == 0) {
        printf("[GRESKA] Ne mogu pokrenuti receiver thread\n");
        return 1;
    }
    
    printf("[DEST%u] Sistem pokrenut sa %d response threadova\n", global_config.id, RESPONSE_THREAD_COUNT);
    printf("[DEST%u] Pritisnite Enter za zaustavljanje...\n", global_config.id);
    getchar();
    
    /* ✅ Cleanup */
    running = 0;
    req_queue.shutdown = 1;
    
    for (i = 0; i < RESPONSE_THREAD_COUNT; i++) {
        ReleaseSemaphore(req_queue.semaphore, 1, NULL);
    }
    
    WaitForMultipleObjects(RESPONSE_THREAD_COUNT, response_threads, TRUE, 5000);
    WaitForSingleObject(recv_thread_handle, 5000);
    
    for (i = 0; i < RESPONSE_THREAD_COUNT; i++) {
        CloseHandle(response_threads[i]);
    }
    CloseHandle(recv_thread_handle);
    
    closesocket(global_sock);
    WSACleanup();
    DeleteCriticalSection(&req_queue.lock);
    CloseHandle(req_queue.semaphore);
    
    printf("[DEST%u] Clean shutdown.\n", global_config.id);
    return 0;
}
