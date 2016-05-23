/***
  This file is part of systemd.

  Copyright (C) 2014 Axis Communications AB. All rights reserved.
  Copyright (C) 2015 Tom Gundersen

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sd-ipv4acd.h"

#include "alloc-util.h"
#include "arp-util.h"
#include "ether-addr-util.h"
#include "fd-util.h"
#include "in-addr-util.h"
#include "list.h"
#include "random-util.h"
#include "siphash24.h"
#include "string-util.h"
#include "util.h"

/* Constants from the RFC */
#define PROBE_WAIT_USEC (1U * USEC_PER_SEC)
#define PROBE_NUM 3U
#define PROBE_MIN_USEC (1U * USEC_PER_SEC)
#define PROBE_MAX_USEC (2U * USEC_PER_SEC)
#define ANNOUNCE_WAIT_USEC (2U * USEC_PER_SEC)
#define ANNOUNCE_NUM 2U
#define ANNOUNCE_INTERVAL_USEC (2U * USEC_PER_SEC)
#define MAX_CONFLICTS 10U
#define RATE_LIMIT_INTERVAL_USEC (60U * USEC_PER_SEC)
#define DEFEND_INTERVAL_USEC (10U * USEC_PER_SEC)

typedef enum IPv4ACDState {
        IPV4ACD_STATE_INIT,
        IPV4ACD_STATE_STARTED,
        IPV4ACD_STATE_WAITING_PROBE,
        IPV4ACD_STATE_PROBING,
        IPV4ACD_STATE_WAITING_ANNOUNCE,
        IPV4ACD_STATE_ANNOUNCING,
        IPV4ACD_STATE_RUNNING,
        _IPV4ACD_STATE_MAX,
        _IPV4ACD_STATE_INVALID = -1
} IPv4ACDState;

struct sd_ipv4acd {
        unsigned n_ref;

        IPv4ACDState state;
        int ifindex;
        int fd;

        unsigned n_iteration;
        unsigned n_conflict;

        sd_event_source *receive_message_event_source;
        sd_event_source *timer_event_source;

        usec_t defend_window;
        be32_t address;

        /* External */
        struct ether_addr mac_addr;

        sd_event *event;
        int event_priority;
        sd_ipv4acd_callback_t callback;
        void* userdata;
};

#define log_ipv4acd_errno(ll, error, fmt, ...) log_internal(LOG_DEBUG, error, __FILE__, __LINE__, __func__, "IPV4ACD: " fmt, ##__VA_ARGS__)
#define log_ipv4acd(ll, fmt, ...) log_ipv4acd_errno(ll, 0, fmt, ##__VA_ARGS__)

static void ipv4acd_set_state(sd_ipv4acd *ll, IPv4ACDState st, bool reset_counter) {
        assert(ll);
        assert(st < _IPV4ACD_STATE_MAX);

        if (st == ll->state && !reset_counter)
                ll->n_iteration++;
        else {
                ll->state = st;
                ll->n_iteration = 0;
        }
}

static void ipv4acd_reset(sd_ipv4acd *ll) {
        assert(ll);

        ll->timer_event_source = sd_event_source_unref(ll->timer_event_source);
        ll->receive_message_event_source = sd_event_source_unref(ll->receive_message_event_source);

        ll->fd = safe_close(ll->fd);

        ipv4acd_set_state(ll, IPV4ACD_STATE_INIT, true);
}

sd_ipv4acd *sd_ipv4acd_ref(sd_ipv4acd *ll) {
        if (!ll)
                return NULL;

        assert_se(ll->n_ref >= 1);
        ll->n_ref++;

        return ll;
}

sd_ipv4acd *sd_ipv4acd_unref(sd_ipv4acd *ll) {
        if (!ll)
                return NULL;

        assert_se(ll->n_ref >= 1);
        ll->n_ref--;

        if (ll->n_ref > 0)
                return NULL;

        ipv4acd_reset(ll);
        sd_ipv4acd_detach_event(ll);

        free(ll);

        return NULL;
}

int sd_ipv4acd_new(sd_ipv4acd **ret) {
        _cleanup_(sd_ipv4acd_unrefp) sd_ipv4acd *ll = NULL;

        assert_return(ret, -EINVAL);

        ll = new0(sd_ipv4acd, 1);
        if (!ll)
                return -ENOMEM;

        ll->n_ref = 1;
        ll->state = IPV4ACD_STATE_INIT;
        ll->ifindex = -1;
        ll->fd = -1;

        *ret = ll;
        ll = NULL;

        return 0;
}

static void ipv4acd_client_notify(sd_ipv4acd *ll, int event) {
        assert(ll);

        if (!ll->callback)
                return;

        ll->callback(ll, event, ll->userdata);
}

int sd_ipv4acd_stop(sd_ipv4acd *ll) {
        assert_return(ll, -EINVAL);

        ipv4acd_reset(ll);

        log_ipv4acd(ll, "STOPPED");

        ipv4acd_client_notify(ll, SD_IPV4ACD_EVENT_STOP);

        return 0;
}

static int ipv4acd_on_timeout(sd_event_source *s, uint64_t usec, void *userdata);

static int ipv4acd_set_next_wakeup(sd_ipv4acd *ll, usec_t usec, usec_t random_usec) {
        _cleanup_(sd_event_source_unrefp) sd_event_source *timer = NULL;
        usec_t next_timeout, time_now;
        int r;

        assert(ll);

        next_timeout = usec;

        if (random_usec > 0)
                next_timeout += (usec_t) random_u64() % random_usec;

        assert_se(sd_event_now(ll->event, clock_boottime_or_monotonic(), &time_now) >= 0);

        r = sd_event_add_time(ll->event, &timer, clock_boottime_or_monotonic(), time_now + next_timeout, 0, ipv4acd_on_timeout, ll);
        if (r < 0)
                return r;

        r = sd_event_source_set_priority(timer, ll->event_priority);
        if (r < 0)
                return r;

        (void) sd_event_source_set_description(timer, "ipv4acd-timer");

        sd_event_source_unref(ll->timer_event_source);
        ll->timer_event_source = timer;
        timer = NULL;

        return 0;
}

static bool ipv4acd_arp_conflict(sd_ipv4acd *ll, struct ether_arp *arp) {
        assert(ll);
        assert(arp);

        /* see the BPF */
        if (memcmp(arp->arp_spa, &ll->address, sizeof(ll->address)) == 0)
                return true;

        /* the TPA matched instead of the SPA, this is not a conflict */
        return false;
}

static int ipv4acd_on_timeout(sd_event_source *s, uint64_t usec, void *userdata) {
        sd_ipv4acd *ll = userdata;
        int r = 0;

        assert(ll);

        switch (ll->state) {

        case IPV4ACD_STATE_STARTED:
                ipv4acd_set_state(ll, IPV4ACD_STATE_WAITING_PROBE, true);

                if (ll->n_conflict >= MAX_CONFLICTS) {
                        char ts[FORMAT_TIMESPAN_MAX];
                        log_ipv4acd(ll, "Max conflicts reached, delaying by %s", format_timespan(ts, sizeof(ts), RATE_LIMIT_INTERVAL_USEC, 0));

                        r = ipv4acd_set_next_wakeup(ll, RATE_LIMIT_INTERVAL_USEC, PROBE_WAIT_USEC);
                        if (r < 0)
                                goto fail;

                        ll->n_conflict = 0;
                } else {
                        r = ipv4acd_set_next_wakeup(ll, 0, PROBE_WAIT_USEC);
                        if (r < 0)
                                goto fail;
                }

                break;

        case IPV4ACD_STATE_WAITING_PROBE:
        case IPV4ACD_STATE_PROBING:
                /* Send a probe */
                r = arp_send_probe(ll->fd, ll->ifindex, ll->address, &ll->mac_addr);
                if (r < 0) {
                        log_ipv4acd_errno(ll, r, "Failed to send ARP probe: %m");
                        goto fail;
                } else {
                        _cleanup_free_ char *address = NULL;
                        union in_addr_union addr = { .in.s_addr = ll->address };

                        (void) in_addr_to_string(AF_INET, &addr, &address);
                        log_ipv4acd(ll, "Probing %s", strna(address));
                }

                if (ll->n_iteration < PROBE_NUM - 2) {
                        ipv4acd_set_state(ll, IPV4ACD_STATE_PROBING, false);

                        r = ipv4acd_set_next_wakeup(ll, PROBE_MIN_USEC, (PROBE_MAX_USEC-PROBE_MIN_USEC));
                        if (r < 0)
                                goto fail;
                } else {
                        ipv4acd_set_state(ll, IPV4ACD_STATE_WAITING_ANNOUNCE, true);

                        r = ipv4acd_set_next_wakeup(ll, ANNOUNCE_WAIT_USEC, 0);
                        if (r < 0)
                                goto fail;
                }

                break;

        case IPV4ACD_STATE_ANNOUNCING:
                if (ll->n_iteration >= ANNOUNCE_NUM - 1) {
                        ipv4acd_set_state(ll, IPV4ACD_STATE_RUNNING, false);
                        break;
                }

                /* fall through */

        case IPV4ACD_STATE_WAITING_ANNOUNCE:
                /* Send announcement packet */
                r = arp_send_announcement(ll->fd, ll->ifindex, ll->address, &ll->mac_addr);
                if (r < 0) {
                        log_ipv4acd_errno(ll, r, "Failed to send ARP announcement: %m");
                        goto fail;
                } else
                        log_ipv4acd(ll, "ANNOUNCE");

                ipv4acd_set_state(ll, IPV4ACD_STATE_ANNOUNCING, false);

                r = ipv4acd_set_next_wakeup(ll, ANNOUNCE_INTERVAL_USEC, 0);
                if (r < 0)
                        goto fail;

                if (ll->n_iteration == 0) {
                        ll->n_conflict = 0;
                        ipv4acd_client_notify(ll, SD_IPV4ACD_EVENT_BIND);
                }

                break;

        default:
                assert_not_reached("Invalid state.");
        }

        return 0;

fail:
        sd_ipv4acd_stop(ll);
        return 0;
}

static void ipv4acd_on_conflict(sd_ipv4acd *ll) {
        _cleanup_free_ char *address = NULL;
        union in_addr_union addr = { .in.s_addr = ll->address };

        assert(ll);

        ll->n_conflict++;

        (void) in_addr_to_string(AF_INET, &addr, &address);
        log_ipv4acd(ll, "Conflict on %s (%u)", strna(address), ll->n_conflict);

        ipv4acd_reset(ll);
        ipv4acd_client_notify(ll, SD_IPV4ACD_EVENT_CONFLICT);
}

static int ipv4acd_on_packet(
                sd_event_source *s,
                int fd,
                uint32_t revents,
                void *userdata) {

        sd_ipv4acd *ll = userdata;
        struct ether_arp packet;
        ssize_t n;
        int r;

        assert(s);
        assert(ll);
        assert(fd >= 0);

        n = recv(fd, &packet, sizeof(struct ether_arp), 0);
        if (n < 0) {
                if (errno == EAGAIN || errno == EINTR)
                        return 0;

                log_ipv4acd_errno(ll, errno, "Failed to read ARP packet: %m");
                goto fail;
        }
        if ((size_t) n != sizeof(struct ether_arp)) {
                log_ipv4acd(ll, "Ignoring too short ARP packet.");
                return 0;
        }

        switch (ll->state) {

        case IPV4ACD_STATE_ANNOUNCING:
        case IPV4ACD_STATE_RUNNING:

                if (ipv4acd_arp_conflict(ll, &packet)) {
                        usec_t ts;

                        assert_se(sd_event_now(ll->event, clock_boottime_or_monotonic(), &ts) >= 0);

                        /* Defend address */
                        if (ts > ll->defend_window) {
                                ll->defend_window = ts + DEFEND_INTERVAL_USEC;
                                r = arp_send_announcement(ll->fd, ll->ifindex, ll->address, &ll->mac_addr);
                                if (r < 0) {
                                        log_ipv4acd_errno(ll, r, "Failed to send ARP announcement: %m");
                                        goto fail;
                                } else
                                        log_ipv4acd(ll, "DEFEND");

                        } else
                                ipv4acd_on_conflict(ll);
                }
                break;

        case IPV4ACD_STATE_WAITING_PROBE:
        case IPV4ACD_STATE_PROBING:
        case IPV4ACD_STATE_WAITING_ANNOUNCE:
                /* BPF ensures this packet indicates a conflict */
                ipv4acd_on_conflict(ll);
                break;

        default:
                assert_not_reached("Invalid state.");
        }

        return 0;

fail:
        sd_ipv4acd_stop(ll);
        return 0;
}

int sd_ipv4acd_set_ifindex(sd_ipv4acd *ll, int ifindex) {
        assert_return(ll, -EINVAL);
        assert_return(ifindex > 0, -EINVAL);
        assert_return(ll->state == IPV4ACD_STATE_INIT, -EBUSY);

        ll->ifindex = ifindex;

        return 0;
}

int sd_ipv4acd_set_mac(sd_ipv4acd *ll, const struct ether_addr *addr) {
        assert_return(ll, -EINVAL);
        assert_return(addr, -EINVAL);
        assert_return(ll->state == IPV4ACD_STATE_INIT, -EBUSY);

        ll->mac_addr = *addr;

        return 0;
}

int sd_ipv4acd_detach_event(sd_ipv4acd *ll) {
        assert_return(ll, -EINVAL);

        ll->event = sd_event_unref(ll->event);

        return 0;
}

int sd_ipv4acd_attach_event(sd_ipv4acd *ll, sd_event *event, int64_t priority) {
        int r;

        assert_return(ll, -EINVAL);
        assert_return(!ll->event, -EBUSY);

        if (event)
                ll->event = sd_event_ref(event);
        else {
                r = sd_event_default(&ll->event);
                if (r < 0)
                        return r;
        }

        ll->event_priority = priority;

        return 0;
}

int sd_ipv4acd_set_callback(sd_ipv4acd *ll, sd_ipv4acd_callback_t cb, void *userdata) {
        assert_return(ll, -EINVAL);

        ll->callback = cb;
        ll->userdata = userdata;

        return 0;
}

int sd_ipv4acd_set_address(sd_ipv4acd *ll, const struct in_addr *address) {
        assert_return(ll, -EINVAL);
        assert_return(address, -EINVAL);
        assert_return(ll->state == IPV4ACD_STATE_INIT, -EBUSY);

        ll->address = address->s_addr;

        return 0;
}

int sd_ipv4acd_is_running(sd_ipv4acd *ll) {
        assert_return(ll, false);

        return ll->state != IPV4ACD_STATE_INIT;
}

int sd_ipv4acd_start(sd_ipv4acd *ll) {
        int r;

        assert_return(ll, -EINVAL);
        assert_return(ll->event, -EINVAL);
        assert_return(ll->ifindex > 0, -EINVAL);
        assert_return(ll->address != 0, -EINVAL);
        assert_return(!ether_addr_is_null(&ll->mac_addr), -EINVAL);
        assert_return(ll->state == IPV4ACD_STATE_INIT, -EBUSY);

        r = arp_network_bind_raw_socket(ll->ifindex, ll->address, &ll->mac_addr);
        if (r < 0)
                return r;

        safe_close(ll->fd);
        ll->fd = r;
        ll->defend_window = 0;
        ll->n_conflict = 0;

        r = sd_event_add_io(ll->event, &ll->receive_message_event_source, ll->fd, EPOLLIN, ipv4acd_on_packet, ll);
        if (r < 0)
                goto fail;

        r = sd_event_source_set_priority(ll->receive_message_event_source, ll->event_priority);
        if (r < 0)
                goto fail;

        (void) sd_event_source_set_description(ll->receive_message_event_source, "ipv4acd-receive-message");

        r = ipv4acd_set_next_wakeup(ll, 0, 0);
        if (r < 0)
                goto fail;

        ipv4acd_set_state(ll, IPV4ACD_STATE_STARTED, true);
        return 0;

fail:
        ipv4acd_reset(ll);
        return r;
}
