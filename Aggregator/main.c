#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

#include "../EnergySystem/common.h"

#ifndef AGR_ID
#define AGR_ID 0
#endif

#define MAX_PENDING_REQUESTS 100

/* Pracenje aktivnih zahteva */
typedef struct {
    uint32_t request_id;
    uint32_t responses_received;
    uint32_t responses_expected;
    EnergyResponse aggregated;
    time_t timeout;
    int active;
} PendingRequest;

/* ✅ Hash mapa cvorova */
NodeMapEntry node_map[NODE_MAP_SIZE];

/* ✅ Thread pool */
typedef struct {
    WorkItem* head;
    WorkItem* tail;
    int count;
    CRITICAL_SECTION lock;
    HANDLE semaphore;
    int shutdown;
} WorkQueue;

WorkQueue work_queue;
HANDLE worker_threads[THREAD_POOL_SIZE];

AggregatorConfig config;
PendingRequest pending_requests[MAX_PENDING_REQUESTS];
CRITICAL_SECTION cs;
SOCKET global_sock;
volatile int running = 1;

/* ✅ Hash funkcija */
uint32_t hash_port(uint16_t port) {
    return port % NODE_MAP_SIZE;
}

/* ✅ Inicijalizacija hash mape */
void init_node_map() {
    uint32_t i, j, hash;
    
    memset(node_map, 0, sizeof(node_map));
    
    /* Dodaj sve agregatore */
    for (i = 0; i < NUM_AGGREGATORS; i++) {
        hash = hash_port(AGR_CONFIGS[i].listen_port);
        
        /* Linear probing ako je kolizija */
        while (node_map[hash].valid) {
            hash = (hash + 1) % NODE_MAP_SIZE;
        }
        
        node_map[hash].port = AGR_CONFIGS[i].listen_port;
        node_map[hash].aggregator_id = AGR_CONFIGS[i].id;
        node_map[hash].is_aggregator = 1;
        node_map[hash].valid = 1;
    }
    
    /* Dodaj sve destinacije */
    for (i = 0; i < NUM_DESTINATIONS; i++) {
        hash = hash_port(DEST_CONFIGS[i].listen_port);
        
        while (node_map[hash].valid) {
            hash = (hash + 1) % NODE_MAP_SIZE;
        }
        
        node_map[hash].port = DEST_CONFIGS[i].listen_port;
        node_map[hash].aggregator_id = DEST_CONFIGS[i].id;
        node_map[hash].is_aggregator = 0;
        node_map[hash].valid = 1;
    }
    
    printf("[AGR%u] Hash mapa inicijalizovana: %u cvorova\n", 
           config.id, NUM_AGGREGATORS + NUM_DESTINATIONS);
}

/* ✅ Lookup u hash mapi (O(1) u proseku!) */
NodeMapEntry* find_node(uint16_t port) {
    uint32_t hash = hash_port(port);
    uint32_t original_hash = hash;
    
    while (node_map[hash].valid) {
        if (node_map[hash].port == port) {
            return &node_map[hash];
        }
        hash = (hash + 1) % NODE_MAP_SIZE;
        if (hash == original_hash) break; /* Prosli ceo niz */
    }
    return NULL;
}

/* ✅ Inicijalizacija thread pool-a */
void init_work_queue() {
    InitializeCriticalSection(&work_queue.lock);
    work_queue.semaphore = CreateSemaphore(NULL, 0, MAX_QUEUE_SIZE, NULL);
    work_queue.head = NULL;
    work_queue.tail = NULL;
    work_queue.count = 0;
    work_queue.shutdown = 0;
}

/* ✅ Dodaj work item u red */
void enqueue_work(EnergyRequest* req, struct sockaddr_in* from_addr) {
    WorkItem* item = (WorkItem*)malloc(sizeof(WorkItem));
    if (!item) return;
    
    item->request = *req;
    item->from_addr = *from_addr;
    item->next = NULL;
    
    EnterCriticalSection(&work_queue.lock);
    
    if (work_queue.count >= MAX_QUEUE_SIZE) {
        LeaveCriticalSection(&work_queue.lock);
        free(item);
        printf("[AGR%u] Work queue pun - odbacujem zahtev\n", config.id);
        return;
    }
    
    if (work_queue.tail) {
        work_queue.tail->next = item;
    } else {
        work_queue.head = item;
    }
    work_queue.tail = item;
    work_queue.count++;
    
    LeaveCriticalSection(&work_queue.lock);
    ReleaseSemaphore(work_queue.semaphore, 1, NULL);
}

/* ✅ Uzmi work item iz reda */
WorkItem* dequeue_work() {
    WorkItem* item = NULL;
    
    EnterCriticalSection(&work_queue.lock);
    
    if (work_queue.head) {
        item = work_queue.head;
        work_queue.head = item->next;
        if (!work_queue.head) {
            work_queue.tail = NULL;
        }
        work_queue.count--;
    }
    
    LeaveCriticalSection(&work_queue.lock);
    return item;
}

void init_pending_requests() {
    int i;
    InitializeCriticalSection(&cs);
    for (i = 0; i < MAX_PENDING_REQUESTS; i++) {
        pending_requests[i].active = 0;
    }
}

PendingRequest* create_pending_request(uint32_t request_id) {
    int i;
    PendingRequest* req = NULL;
    
    EnterCriticalSection(&cs);
    for (i = 0; i < MAX_PENDING_REQUESTS; i++) {
        if (!pending_requests[i].active) {
            req = &pending_requests[i];
            req->request_id = request_id;
            req->responses_received = 0;
            req->responses_expected = 0;
            req->aggregated.type = PACKET_RESPONSE;
            req->aggregated.request_id = request_id;
            req->aggregated.aggregator_id = config.id;
            req->aggregated.total_energy = 0.0f;
            req->aggregated.consumer_count = 0;
            req->aggregated.packet_count = 0;
            req->aggregated.timestamp = time(NULL);
            req->timeout = time(NULL) + 10;
            req->active = 1;
            break;
        }
    }
    LeaveCriticalSection(&cs);
    return req;
}

PendingRequest* find_pending_request(uint32_t request_id) {
    int i;
    PendingRequest* req = NULL;
    
    EnterCriticalSection(&cs);
    for (i = 0; i < MAX_PENDING_REQUESTS; i++) {
        if (pending_requests[i].active && 
            pending_requests[i].request_id == request_id) {
            req = &pending_requests[i];
            break;
        }
    }
    LeaveCriticalSection(&cs);
    return req;
}

/* ✅ OPTIMIZOVANA provera - koristi hash mapu */
int is_target_in_subtree(uint32_t target_agr, uint32_t current_agr) {
    uint32_t i, j;
    NodeMapEntry* node;
    
    if (target_agr == TARGET_ALL || target_agr == current_agr) {
        return 1;
    }
    
    for (i = 0; i < NUM_AGGREGATORS; i++) {
        if (AGR_CONFIGS[i].id == current_agr) {
            for (j = 0; j < AGR_CONFIGS[i].num_children; j++) {
                /* ✅ O(1) lookup umesto O(n) */
                node = find_node(AGR_CONFIGS[i].children_ports[j]);
                if (node && node->is_aggregator) {
                    if (is_target_in_subtree(target_agr, node->aggregator_id)) {
                        return 1;
                    }
                }
            }
            break;
        }
    }
    
    return 0;
}

void send_response_to_parent(SOCKET sock, EnergyResponse* resp) {
    struct sockaddr_in parent_addr;
    
    if (config.parent_port == 0) {
        memset(&parent_addr, 0, sizeof(parent_addr));
        parent_addr.sin_family = AF_INET;
        parent_addr.sin_port = htons(5999);
        parent_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        
        printf("    [AGR%u] Saljem finalni odgovor #%u Source-u\n",
               config.id, resp->request_id);
    } else {
        memset(&parent_addr, 0, sizeof(parent_addr));
        parent_addr.sin_family = AF_INET;
        parent_addr.sin_port = htons(config.parent_port);
        parent_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        
        printf("    [AGR%u] Saljem odgovor #%u roditelju (port %d)\n",
               config.id, resp->request_id, config.parent_port);
    }
    
    printf("      -> Energija: %.2f kW | Potrosaci: %u\n",
           resp->total_energy, resp->consumer_count);
    
    sendto(sock, (const char*)resp, sizeof(*resp), 0,
           (struct sockaddr*)&parent_addr, sizeof(parent_addr));
}

/* ✅ Handler zahteva (poziva ga worker thread) */
void handle_request(SOCKET sock, EnergyRequest* req) {
    PendingRequest* pending;
    struct sockaddr_in dest_addr;
    uint32_t i, j;
    uint32_t sent_count = 0;
    int should_process = 0;
    int is_target = 0;
    EnergyRequest forwarded_req;
    
    printf("    [AGR%u Thread] Obradjujem zahtev #%u (cilj: ", 
           config.id, req->request_id);
    
    if (req->target_agr == TARGET_ALL) {
        printf("SVI)\n");
        should_process = 1;
        is_target = 1;
    } else {
        printf("Agr %u)\n", req->target_agr);
        
        if (req->target_agr == config.id) {
            is_target = 1;
            should_process = 1;
        } else {
            should_process = is_target_in_subtree(req->target_agr, config.id);
        }
    }
    
    if (!should_process) {
        printf("    [AGR%u] Ignorisujem\n", config.id);
        return;
    }
    
    pending = create_pending_request(req->request_id);
    if (!pending) {
        printf("    [GRESKA] Nema mesta za pending request!\n");
        return;
    }
    
    forwarded_req = *req;
    
    if (is_target && req->target_agr != TARGET_ALL) {
        forwarded_req.target_agr = TARGET_ALL;
    }
    
    if (is_target || req->target_agr == TARGET_ALL) {
        for (i = 0; i < config.num_children; i++) {
            memset(&dest_addr, 0, sizeof(dest_addr));
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(config.children_ports[i]);
            dest_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            
            if (sendto(sock, (const char*)&forwarded_req, sizeof(forwarded_req), 0,
                       (struct sockaddr*)&dest_addr, sizeof(dest_addr)) != SOCKET_ERROR) {
                sent_count++;
            }
        }
    } else {
        for (i = 0; i < config.num_children; i++) {
            int should_forward = 0;
            NodeMapEntry* node = find_node(config.children_ports[i]);
            
            if (node && node->is_aggregator) {
                if (is_target_in_subtree(req->target_agr, node->aggregator_id)) {
                    should_forward = 1;
                }
            }
            
            if (should_forward) {
                memset(&dest_addr, 0, sizeof(dest_addr));
                dest_addr.sin_family = AF_INET;
                dest_addr.sin_port = htons(config.children_ports[i]);
                dest_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
                
                if (sendto(sock, (const char*)&forwarded_req, sizeof(forwarded_req), 0,
                           (struct sockaddr*)&dest_addr, sizeof(dest_addr)) != SOCKET_ERROR) {
                    sent_count++;
                }
            }
        }
    }
    
    EnterCriticalSection(&cs);
    pending->responses_expected = sent_count;
    
    if (sent_count == 0) {
        send_response_to_parent(sock, &pending->aggregated);
        pending->active = 0;
    }
    LeaveCriticalSection(&cs);
}

void handle_response(SOCKET sock, EnergyResponse* resp) {
    PendingRequest* pending;
    
    pending = find_pending_request(resp->request_id);
    if (!pending) return;
    
    EnterCriticalSection(&cs);
    pending->aggregated.total_energy += resp->total_energy;
    pending->aggregated.consumer_count += resp->consumer_count;
    pending->aggregated.packet_count += resp->packet_count;
    pending->responses_received++;
    
    if (pending->responses_received >= pending->responses_expected) {
        send_response_to_parent(sock, &pending->aggregated);
        pending->active = 0;
    }
    LeaveCriticalSection(&cs);
}

/* ✅ Worker thread funkcija */
unsigned __stdcall worker_thread(void* arg) {
    while (1) {
        WaitForSingleObject(work_queue.semaphore, INFINITE);
        
        if (work_queue.shutdown) break;
        
        WorkItem* item = dequeue_work();
        if (item) {
            handle_request(global_sock, &item->request);
            free(item);
        }
    }
    return 0;
}

/* ✅ Receiver thread */
unsigned __stdcall receiver_thread(void* arg) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    int client_len = sizeof(client_addr);
    int recv_len;
    PacketType* packet_type;
    
    printf("[AGR%u] Receiver thread pokrenut\n", config.id);
    
    while (running) {
        recv_len = recvfrom(global_sock, buffer, BUFFER_SIZE, 0,
                           (struct sockaddr*)&client_addr, &client_len);
        
        if (recv_len < sizeof(PacketType)) continue;
        
        packet_type = (PacketType*)buffer;
        
        if (*packet_type == PACKET_REQUEST && recv_len == sizeof(EnergyRequest)) {
            /* ✅ Dodaj u thread pool umesto direktne obrade */
            enqueue_work((EnergyRequest*)buffer, &client_addr);
        }
        else if (*packet_type == PACKET_RESPONSE && recv_len == sizeof(EnergyResponse)) {
            handle_response(global_sock, (EnergyResponse*)buffer);
        }
    }
    
    printf("[AGR%u] Receiver thread zaustavljen\n", config.id);
    return 0;
}

int main(int argc, char* argv[]) {
    WSADATA wsa;
    struct sockaddr_in server_addr;
    uint32_t agr_id = AGR_ID;
    HANDLE recv_thread_handle;
    int i;
    
    if (argc > 1) {
        agr_id = (uint32_t)atoi(argv[1]);
        if (agr_id >= NUM_AGGREGATORS) {
            printf("[GRESKA] Nevazeci ID agregatora: %u\n", agr_id);
            return 1;
        }
    }
    
    config = AGR_CONFIGS[agr_id];
    init_pending_requests();
    init_node_map();           /* ✅ Hash mapa */
    init_work_queue();         /* ✅ Thread pool */
    
    printf("========================================\n");
    printf("  AGGREGATOR %u (Multi-threaded)\n", config.id);
    printf("========================================\n");
    printf("  Port: %u | Roditelj: %u | Deca: %u\n",
           config.listen_port, config.parent_port, config.num_children);
    printf("  Thread pool: %d worker-a\n", THREAD_POOL_SIZE);
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
    server_addr.sin_port = htons(config.listen_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(global_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("[GRESKA] Bind: %d\n", WSAGetLastError());
        closesocket(global_sock);
        WSACleanup();
        return 1;
    }
    
    printf("[OK] Slusa na portu %d\n\n", config.listen_port);
    
    /* ✅ Pokreni worker threadove */
    for (i = 0; i < THREAD_POOL_SIZE; i++) {
        worker_threads[i] = (HANDLE)_beginthreadex(NULL, 0, worker_thread, NULL, 0, NULL);
    }
    
    /* ✅ Pokreni receiver thread */
    recv_thread_handle = (HANDLE)_beginthreadex(NULL, 0, receiver_thread, NULL, 0, NULL);
    
    printf("[AGR%u] Sistem pokrenut sa %d worker threadova\n", config.id, THREAD_POOL_SIZE);
    printf("[AGR%u] Pritisnite Enter za zaustavljanje...\n", config.id);
    getchar();
    
    /* Cleanup */
    running = 0;
    work_queue.shutdown = 1;
    
    for (i = 0; i < THREAD_POOL_SIZE; i++) {
        ReleaseSemaphore(work_queue.semaphore, 1, NULL);
    }
    
    WaitForMultipleObjects(THREAD_POOL_SIZE, worker_threads, TRUE, 5000);
    WaitForSingleObject(recv_thread_handle, 5000);
    
    for (i = 0; i < THREAD_POOL_SIZE; i++) {
        CloseHandle(worker_threads[i]);
    }
    CloseHandle(recv_thread_handle);
    
    closesocket(global_sock);
    WSACleanup();
    DeleteCriticalSection(&cs);
    DeleteCriticalSection(&work_queue.lock);
    CloseHandle(work_queue.semaphore);
    
    printf("[AGR%u] Clean shutdown.\n", config.id);
    return 0;
}
