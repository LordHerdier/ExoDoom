#include "fault_record.h"

static exo_fault_info_t records[EXO_FAULT_LOG_MAX];
static uint64_t last_seq = 0;   /* seq of the newest record; 0 = none */

void fault_record_add(uint64_t ctx_id, const char *name, uint32_t vector,
                      uint32_t reason, uint64_t rip, uint64_t addr)
{
    /* Slot of seq n is (n - 1) % MAX: the oldest record is overwritten. */
    exo_fault_info_t *r = &records[last_seq % EXO_FAULT_LOG_MAX];

    r->seq    = ++last_seq;
    r->ctx_id = ctx_id;
    r->rip    = rip;
    r->addr   = addr;
    r->vector = vector;
    r->reason = reason;
    unsigned i = 0;
    if (name != 0)
        for (; i < sizeof r->name - 1 && name[i] != '\0'; i++)
            r->name[i] = name[i];
    for (; i < sizeof r->name; i++)
        r->name[i] = '\0';
}

int fault_record_next(uint64_t after_seq, exo_fault_info_t *out)
{
    if (after_seq == EXO_FAULT_NEWEST) {
        if (last_seq == 0)
            return -1;
        *out = records[(last_seq - 1) % EXO_FAULT_LOG_MAX];
        return 0;
    }
    if (after_seq >= last_seq)
        return -1;

    /* Oldest seq still held is last_seq - MAX + 1 (or 1 while the ring is
     * not yet full); anything older than that has been overwritten. */
    uint64_t oldest = last_seq > EXO_FAULT_LOG_MAX
                          ? last_seq - EXO_FAULT_LOG_MAX + 1 : 1;
    uint64_t want = after_seq + 1 > oldest ? after_seq + 1 : oldest;

    *out = records[(want - 1) % EXO_FAULT_LOG_MAX];
    return 0;
}

uint64_t fault_record_last_seq(void)
{
    return last_seq;
}

#ifdef TESTING
void fault_record_reset(void)
{
    last_seq = 0;
    for (unsigned i = 0; i < EXO_FAULT_LOG_MAX; i++)
        records[i] = (exo_fault_info_t){ 0 };
}
#endif
