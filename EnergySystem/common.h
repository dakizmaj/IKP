#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <time.h>

#define MAX_CONSUMERS 10000
#define MAX_CHILDREN 3
#define BUFFER_SIZE 1024
#define TARGET_ALL 0xFF

/* Thread pool konstante */
#define THREAD_POOL_SIZE 4        /* Broj worker threadova */
#define MAX_QUEUE_SIZE 100        /* Maksimalna velicina reda zahteva */

/* Tip paketa */
typedef enum {
    PACKET_REQUEST = 1,
    PACKET_RESPONSE = 2
} PacketType;

/* Zahtev za podatke o potrosnji */
typedef struct {
    PacketType type;
    uint32_t request_id;
    uint32_t target_agr;
    time_t timestamp;
} EnergyRequest;

/* Odgovor sa agregiranim podacima */
typedef struct {
    PacketType type;
    uint32_t request_id;
    uint32_t aggregator_id;
    float total_energy;
    uint32_t consumer_count;
    uint32_t packet_count;
    time_t timestamp;
} EnergyResponse;

/* Konfiguracija agregatora */
typedef struct {
    uint32_t id;
    uint16_t listen_port;
    uint16_t parent_port;
    uint16_t children_ports[MAX_CHILDREN];
    uint32_t num_children;
} AggregatorConfig;

/* ✅ Hash mapa za brzi lookup cvorova */
typedef struct {
    uint16_t port;              /* Kljuc (port) */
    uint32_t aggregator_id;     /* Vrednost (ID agregatora) */
    int is_aggregator;          /* 1 = agregator, 0 = destination */
    int valid;                  /* 1 = slot zauzet, 0 = prazan */
} NodeMapEntry;

#define NODE_MAP_SIZE 20        /* Broj slotova u hash mapi */

static const AggregatorConfig AGR_CONFIGS[] = {
    {0, 5000, 0,    {5010, 5020, 0},    2},
    {1, 5010, 5000, {5030, 5040, 0},    2},
    {2, 5020, 5000, {5051, 5060, 5070}, 3},
    {3, 5030, 5010, {5001, 0, 0},       1},
    {4, 5040, 5010, {5002, 0, 0},       1},
    {5, 5051, 5020, {5003, 0, 0},       1},
    {6, 5060, 5020, {5004, 0, 0},       1},
    {7, 5070, 5020, {5005, 0, 0},       1}
};

#define NUM_AGGREGATORS (sizeof(AGR_CONFIGS) / sizeof(AggregatorConfig))

typedef struct {
    uint32_t id;
    uint16_t listen_port;
    uint16_t parent_port;
} DestinationConfig;

static const DestinationConfig DEST_CONFIGS[] = {
    {1, 5001, 5030},
    {2, 5002, 5040},
    {3, 5003, 5051},
    {4, 5004, 5060},
    {5, 5005, 5070}
};

#define NUM_DESTINATIONS (sizeof(DEST_CONFIGS) / sizeof(DestinationConfig))

/* ✅ Thread pool - Work item */
typedef struct WorkItem {
    EnergyRequest request;
    struct sockaddr_in from_addr;
    struct WorkItem* next;
} WorkItem;

#endif /* COMMON_H */