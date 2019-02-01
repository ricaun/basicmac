// Copyright (C) 2016-2019 Semtech (International) AG. All rights reserved.
//
// This file is subject to the terms and conditions defined in file 'LICENSE',
// which is part of this source code package.

#include "lwmux.h"
#include "svcdefs.h"

DECL_ON_LMIC_EVENT;

enum {
    FLAG_MODESWITCH	= (1 << 0),	// mode switch pending
    FLAG_BUSY		= (1 << 1),	// radio is busy
    FLAG_JOINING	= (1 << 2),	// trying to join
};

static struct {
    unsigned int flags;		// flags (FLAG_*)
    unsigned int mode;		// current mode (LWM_MODE_*)
    unsigned int nextmode;	// next mode (if FLAG_MODESWITCH is set)

    lwm_job* queue;		// job queue head
    unsigned int runprio;       // minimum priority level for runnning jobs
    lwm_complete completefunc;	// current job completion function
    osjob_t job;		// tx opportunity job

    unsigned int jcnt;		// join attempt counter

#ifdef LWM_SLOTTED
    struct {
        ostime_t interval;	// beacon interval
        u4_t freq;		// beacon frequency
        dr_t dndr;		// beacon datarate

        u1_t slotsz;		// slot size (payload length in bytes)
        ostime_t t_slots;	// time span available for uplink slots
        ostime_t off_slots;	// offset to first slot relative to beacon start

        ostime_t nextrx;	// time of next beacon

        int missed;		// number of missed beacons
        int missed_max;		// max. number of missed beacons before going back to scanning

        int timeouts;		// number of beacon scan timeouts
        int timeouts_max;	// max. number of beacon scan timeouts before creating an unaligned TX opportunity
    } bcn;
#endif
} state;


// ------------------------------------------------
// Forward declarations

static bool mode_switch (void);
static void tx_opportunity (osjob_t* j);


#ifdef LWM_SLOTTED
// ------------------------------------------------
// Beacon-based slotting

/*
   <----------      beacon interval      ---------->

   +------------+---+------------------- - - --+---+
   | beacon     |   | uplink slots ...         |   |
   +------------+---+------------------- - - --+---+
   airtime(255) 10ms                           10ms

*/

static void bcn_continue (void) {
    ASSERT(!(state.flags & (FLAG_BUSY | FLAG_JOINING)));
    state.flags |= FLAG_BUSY;
    lwm_bcn_setup();
    LMIC.freq = state.bcn.freq;
    LMIC.dndr = state.bcn.dndr;
    ostime_t now = os_getTime();
again:
    if (state.bcn.missed < state.bcn.missed_max) {
        if ((state.bcn.nextrx - now) < 0) {
            state.bcn.missed += 1;
            state.bcn.nextrx += state.bcn.interval;
            goto again;
        }
        LMIC_track(state.bcn.nextrx);
    } else {
        LMIC_scan(state.bcn.interval + state.bcn.off_slots);
        debug_printf("scanning %.1F MHz...\r\n", LMIC.freq, 6);
    }
}

static ostime_t slot_offset (void) {
    ostime_t t_slot = calcAirTime(updr2rps(LMIC.datarate), 13 + state.bcn.slotsz); // XXX do we need the 13?
    unsigned int nslots = state.bcn.t_slots / t_slot;
    ASSERT(nslots != 0);
    return (lwm_slot() % nslots) * t_slot;
}

static void bcn_event (ev_t ev) {
    state.flags &= ~FLAG_BUSY;
    if (mode_switch()) {
        return;
    }
    ostime_t t0;
    switch (ev) {
        case EV_SCAN_FOUND:
        case EV_BEACON_TRACKED:
            state.bcn.missed = 0;
            state.bcn.timeouts = 0;
            t0 = LMIC.rxtime0;
            break;

        case EV_BEACON_MISSED:
            state.bcn.missed += 1;
            t0 = state.bcn.nextrx;
            break;

        case EV_SCAN_TIMEOUT:
            state.bcn.timeouts += 1;
            if (state.bcn.timeouts < state.bcn.timeouts_max) {
                // scan again
                bcn_continue();
                return;
            }
            // can't find a beacon -- create an unaligned tx opportunity
            state.bcn.timeouts = 0;
            t0 = os_getTime();
            break;

        default:
            ASSERT(0);
            // not reached
            return;
    }

    // schedule next tx opportunity
    state.bcn.nextrx = t0 + state.bcn.interval;
    os_setTimedCallback(&state.job, t0 + state.bcn.off_slots + slot_offset(), tx_opportunity);
}

void lwm_slotparams (u4_t freq, dr_t dr, ostime_t interval, int slotsz, int missed_max, int timeouts_max) {
    state.bcn.interval = interval;
    state.bcn.freq = freq;
    state.bcn.dndr = dr;

    state.bcn.slotsz = slotsz;

    ostime_t t_bcn = calcAirTime(updr2rps(dr), 255);
    state.bcn.t_slots = interval - (ms2osticks(20) + t_bcn);
    ASSERT(state.bcn.t_slots > 0);
    state.bcn.off_slots = t_bcn + ms2osticks(10);

    state.bcn.missed_max = missed_max;
    state.bcn.timeouts_max = timeouts_max;
}
#endif


// ------------------------------------------------
// TX opportunity

// TODO: this should be provided by the region definition
static int max_plen (dr_t dr) {
#if defined(CFG_eu868)
    static const uint8_t MAX_PLEN[4] = { 51, 51, 51, 115 };
    return (dr < 4) ? MAX_PLEN[dr] : 242;
#elif defined(CFG_us915)
    static const uint8_t MAX_PLEN[3] = { 11, 53, 125 };
    return (dr < 3) ? MAX_PLEN[dr] : 242;
#endif
}

static void tx_opportunity (osjob_t* j) {
    ASSERT(!(state.flags & (FLAG_BUSY | FLAG_JOINING)));
    lwm_job* job;
    while ((job = state.queue) != NULL && job->prio >= state.runprio) {
        state.queue = job->next;
        lwm_txinfo txinfo;
        memset(&txinfo, 0, sizeof(txinfo));
        txinfo.data = LMIC.pendTxData;
        txinfo.dlen = max_plen(LMIC.datarate);
        if (job->txfunc(&txinfo)) {
            ASSERT((unsigned int) txinfo.dlen < MAX_LEN_PAYLOAD);
            if (txinfo.data != LMIC.pendTxData) {
                os_copyMem(LMIC.pendTxData, txinfo.data, txinfo.dlen);
            }
            LMIC.pendTxConf = txinfo.confirmed;
            LMIC.pendTxPort = txinfo.port;
            LMIC.pendTxLen = txinfo.dlen;
            LMIC_setTxData();
            state.flags |= FLAG_BUSY;
            state.completefunc = txinfo.txcomplete;
            return;
        }
    }
    // nobody is sending
#ifdef LWM_SLOTTED
    if (state.mode == LWM_MODE_SLOTTED) {
        bcn_continue();
    }
#endif
}

// Schedule TX opportunity right now
static void tx_now (void) {
    os_setCallback(&state.job, tx_opportunity);
}

// TX complete handler
static void tx_complete (void) {
    state.flags &= ~FLAG_BUSY;
    if (mode_switch()) {
        return;
    }
    if (state.mode == LWM_MODE_NORMAL) {
        tx_now();
    }
#ifdef LWM_SLOTTED
    else if (state.mode == LWM_MODE_SLOTTED) {
        bcn_continue();
    }
#endif
}


// ------------------------------------------------
// Mode switching

static void join (osjob_t* job) {
    ASSERT((state.flags & (FLAG_BUSY | FLAG_JOINING)) == FLAG_JOINING);
    LMIC_reset();
    LMIC_startJoining();
    state.flags |= FLAG_BUSY;
}

static void reschedule_join (void) {
    os_setApproxTimedCallback(&state.job,
            os_getTime() + (
#if defined(CFG_eu868)
                (state.jcnt < 10) ? sec2osticks(360) :	// first hour:    every 6 minutes
                (state.jcnt < 20) ? sec2osticks(3600) :	// next 10 hours: every hour
                sec2osticks(3600 * 12)			// after:         every 12 hours
#elif defined(CFG_us915)
                (state.jcnt < 6) ? sec2osticks(600) :	// first hour:    every 10 minutes
                (state.jcnt < 12) ? sec2osticks(6000) :	// next 10 hours: every 100 minutes
                sec2osticks(3600 * 12)			// after:         every 12 hours
#endif
                ), join);
}

// Note: only call this function when idle and
// when no tx_opportunity is scheduled!!
static bool mode_switch (void) {
    ASSERT(!(state.flags & FLAG_BUSY));
    if (state.flags & FLAG_MODESWITCH) {
        debug_printf("switching mode: ");
        if (state.nextmode == LWM_MODE_SHUTDOWN) {
            debug_printf("shutdown\r\n");
            state.flags &= ~FLAG_JOINING;
            LMIC_shutdown();
        } else {
            if (state.mode == LWM_MODE_SHUTDOWN) {
                state.flags |= FLAG_JOINING;
                state.jcnt = 0;
                os_setCallback(&state.job, join);
            }
            if (state.nextmode == LWM_MODE_NORMAL) {
                debug_printf("normal\r\n");
                if (!(state.flags & (FLAG_BUSY | FLAG_JOINING))) {
                    tx_now();
                }
            }
#ifdef LWM_SLOTTED
            else if (state.nextmode == LWM_MODE_SLOTTED) {
                debug_printf("slotted\r\n");
                state.bcn.missed = state.bcn.missed_max;	// start with scanning
                state.bcn.timeouts = 0;
                if (!(state.flags & (FLAG_BUSY | FLAG_JOINING))) {
                    bcn_continue();
                }
            }
#endif
        }
        state.mode = state.nextmode;
        state.flags &= ~FLAG_MODESWITCH;
        return true;
    } else {
        return false;
    }
}


// ------------------------------------------------
// Public API

void lwm_clear_send (lwm_job* job) {
    lwm_job** pnext = &state.queue;
    while (*pnext) {
        if (*pnext == job) {
            *pnext = job->next;
            break;
        }
        pnext = &((*pnext)->next);
    }
}

void lwm_request_send (lwm_job* job, unsigned int priority, lwm_tx txfunc) {
    lwm_clear_send(job);

    job->prio = priority;
    job->txfunc = txfunc;

    lwm_job** pnext = &state.queue;
    while (*pnext) {
        if ((*pnext)->prio < priority) {
            break;
        }
        pnext = &((*pnext)->next);
    }
    job->next = *pnext;
    *pnext = job;

    if (state.mode != LWM_MODE_SHUTDOWN
            && !(state.flags & (FLAG_BUSY | FLAG_JOINING))) {
        tx_now();
    }
}

void lwm_setmode (int mode) {
    state.nextmode = mode;
    if (state.nextmode != state.mode) {
        state.flags |= FLAG_MODESWITCH;
        if (!(state.flags & FLAG_BUSY)) {
            os_clearCallback(&state.job);
            mode_switch();
        }
    } else {
        state.flags &= ~FLAG_MODESWITCH;
    }
}

void lwm_setpriority (unsigned int priority) {
    state.runprio = priority;
    if (state.mode != LWM_MODE_SHUTDOWN
            && !(state.flags & (FLAG_BUSY | FLAG_JOINING))) {
        tx_now();
    }
}


// ------------------------------------------------
// LMiC event callback

DECL_ON_LMIC_EVENT {
    debug_printf("lwm: %e\r\n", e);

    if (e == EV_TXCOMPLETE) {
        if (state.completefunc) {
            state.completefunc();
            state.completefunc = NULL;
        }
    }

    SVCHOOK_lwm_event(e);

    if (e == EV_TXCOMPLETE || e == EV_RXCOMPLETE) {
        if ((LMIC.txrxFlags & TXRX_PORT) && LMIC.frame[LMIC.dataBeg-1]) {
            SVCHOOK_lwm_downlink(LMIC.frame[LMIC.dataBeg-1],
                    LMIC.frame+LMIC.dataBeg, LMIC.dataLen, LMIC.txrxFlags);
        }
    }

    switch (e) {
        case EV_JOIN_FAILED:
            LMIC_shutdown(); // stop joining
            state.jcnt += 1;
            state.flags &= ~FLAG_BUSY;
            reschedule_join();
            break;
        case EV_JOINED:
            state.flags &= ~FLAG_JOINING;
            // fall-thru
        case EV_TXCOMPLETE:
            tx_complete();
            break;
#ifdef LWM_SLOTTED
        case EV_SCAN_FOUND:
        case EV_BEACON_TRACKED:
        case EV_BEACON_MISSED:
        case EV_SCAN_TIMEOUT:
            if (state.mode == LWM_MODE_SLOTTED) {
                bcn_event(e);
            }
            break;
#endif
        default:
            // ignore and make compiler happy
            break;
    }
}


// ------------------------------------------------
// Convenience APIs

void lwm_process_dl (lwm_downlink dlfunc) {
    dlfunc( (LMIC.txrxFlags & TXRX_PORT) ? LMIC.frame[LMIC.dataBeg-1] : -1,
            (LMIC.txrxFlags & TXRX_PORT) ? LMIC.frame+LMIC.dataBeg : NULL,
            (LMIC.txrxFlags & TXRX_PORT) ? LMIC.dataLen :  0,
             LMIC.txrxFlags);
}
