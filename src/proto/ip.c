// -*- c-basic-offset: 4; c-backslash-column: 79; indent-tabs-mode: nil -*-
// vim:sw=4 ts=4 sts=4 expandtab
/* Copyright 2010, SecurActive.
 *
 * This file is part of Junkie.
 *
 * Junkie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Junkie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with Junkie.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <junkie/cpp.h>
#include <junkie/tools/log.h>
#include <junkie/tools/miscmacs.h>
#include <junkie/tools/tempstr.h>
#include <junkie/tools/mallocer.h>
#include <junkie/proto/proto.h>
#include <junkie/proto/eth.h>
#include <junkie/proto/ip.h>
#include <junkie/proto/pkt_wait_list.h>
#include "proto/ip_hdr.h"

static char const Id[] = "$Id: 003ae93bbf458d21ffd9582b447feb9019b93405 $";

#undef LOG_CAT
#define LOG_CAT proto_ip_log_category

LOG_CATEGORY_DEC(proto_ip);
LOG_CATEGORY_DEF(proto_ip);

#define IP_TIMEOUT (60*60)
#define IP_HASH_SIZE 10000

/*
 * Proto Infos
 */

void const *ip_info_addr(struct proto_info const *info_, size_t *size)
{
    struct ip_proto_info const *info = DOWNCAST(info_, info, ip_proto_info);
    if (size) *size = sizeof(*info);
    return info;
}

char const *ip_info_2_str(struct proto_info const *info_)
{
    struct ip_proto_info const *info = DOWNCAST(info_, info, ip_proto_info);
    char *str = tempstr();
    snprintf(str, TEMPSTR_SIZE, "%s, version=%u, addr=%s->%s%s, proto=%u, ttl=%u",
        proto_info_2_str(info_),
        info->version,
        ip_addr_2_str(info->key.addr+0),
        ip_addr_2_str(info->key.addr+1),
        info->way ? " (hashed the other way)":"",
        info->key.protocol,
        info->ttl);
    return str;
}

static void ip_proto_info_ctor(struct ip_proto_info *info, struct parser *parser, struct proto_info *parent, size_t head_len, size_t payload, struct ip_hdr const *iphdr)
{
    proto_info_ctor(&info->info, parser, parent, head_len, payload);

    info->version = iphdr->version;
    ip_addr_ctor_from_ip4(&info->key.addr[0], iphdr->src);
    ip_addr_ctor_from_ip4(&info->key.addr[1], iphdr->dst);
    info->key.protocol = iphdr->protocol;
    info->ttl = iphdr->ttl;
    info->way = 0;  // will be set later
}

/*
 * Subproto management
 */

static LIST_HEAD(ip_subprotos, ip_subproto) ip_subprotos;

void ip_subproto_ctor(struct ip_subproto *ip_subproto, unsigned protocol, struct proto *proto)
{
    SLOG(LOG_DEBUG, "Adding proto %s for protocol value %u", proto->name, protocol);
    ip_subproto->protocol = protocol;
    ip_subproto->proto = proto;
    LIST_INSERT_HEAD(&ip_subprotos, ip_subproto, entry);
}

void ip_subproto_dtor(struct ip_subproto *ip_subproto)
{
    SLOG(LOG_DEBUG, "Removing proto %s for protocol value %u", ip_subproto->proto->name, ip_subproto->protocol);
    LIST_REMOVE(ip_subproto, entry);
}

/*
 * Parse
 */

static bool is_fragment(struct ip_hdr const *ip)
{
    // No need to set fragment offset in host byte order to test for 0
    return unlikely_(ip->frag_offset_lo) || unlikely_(ip->frag_offset_hi) || unlikely_(ip->more_fragments);
}

static unsigned fragment_offset(struct ip_hdr const *ip)
{
    return (ip->frag_offset_lo + ip->frag_offset_hi * 256) * 8;
}

// We overload the mux_subparser to fit the pkt_wait_list required for IP reassembly
struct ip_subparser {
    /* We may have a list per IP id, but we do not want to create a pkt_list whenever a new id is encountered.
     * So we only creates a list if either the MoreFrag flag is set or the offset is > 0 (if for first to last fragments).
     * Also, we check in the list iff this condition holds.
     * Each IP subparser can have at most 4 packets reassembled simultaneously, which should be more than
     * enought in all 'normal' situations. */
    /* Also, as an additional rule, we must not send fragments to the subparser before the packet is fully
     * reassembled, otherwise the subparser could receive the first fragment of id X then the first fragment
     * of id Y, which make no sense. */
    struct ip_reassembly {
        uint16_t in_use:1;
        uint16_t constructed:1;     // always 1 when in_use
        uint16_t got_last:1;        // set when we received the fragment without more_fragments flag
        uint16_t id;                // only valid when in_use flag is set
        unsigned end_offset;        // only valid when got_last flag is set
        struct pkt_wait_list wl;    // only constructed when constructed flag is set
    } reassembly[4];
    struct mux_subparser mux_subparser;
};

static int ip_subparser_ctor(struct ip_subparser *ip_subparser, struct mux_parser *mux_parser, struct parser *child, struct parser *requestor, void const *key)
{
    SLOG(LOG_DEBUG, "Construct an IP mux_subparser @%p", ip_subparser);
    CHECK_LAST_FIELD(ip_subparser, mux_subparser, struct mux_subparser);

    for (unsigned r = 0; r < NB_ELEMS(ip_subparser->reassembly); r++) {
        ip_subparser->reassembly[r].in_use = 0;
        ip_subparser->reassembly[r].constructed = 0;
        ip_subparser->reassembly[r].got_last = 0;
    }
    return mux_subparser_ctor(&ip_subparser->mux_subparser, mux_parser, child, requestor, key);
}

static struct mux_subparser *ip_subparser_new(struct mux_parser *mux_parser, struct parser *child, struct parser *requestor, void const *key)
{
    struct ip_subparser *ip_subparser = mux_subparser_alloc(mux_parser, sizeof(*ip_subparser));
    if (! ip_subparser) return NULL;

    if (0 != ip_subparser_ctor(ip_subparser, mux_parser, child, requestor, key)) {
        FREE(ip_subparser);
        return NULL;
    }

    return &ip_subparser->mux_subparser;
}

static void ip_reassembly_dtor(struct ip_reassembly *reassembly, struct timeval const *now)
{
    SLOG(LOG_DEBUG, "Destructing ip_reassembly@%p", reassembly);
    if (reassembly->constructed) {
        pkt_wait_list_dtor(&reassembly->wl, now);
        reassembly->constructed = 0;
        reassembly->in_use = 0;
    }
}

static void ip_subparser_dtor(struct ip_subparser *ip_subparser, struct timeval const *now)
{
    SLOG(LOG_DEBUG, "Destruct an IP mux_subparser @%p", ip_subparser);

    for (unsigned r = 0; r < NB_ELEMS(ip_subparser->reassembly); r++) {
        ip_reassembly_dtor(ip_subparser->reassembly+r, now);
    }
    mux_subparser_dtor(&ip_subparser->mux_subparser);
}

static void ip_subparser_del(struct mux_subparser *mux_subparser)
{
    struct timeval now; // FIXME: add a now parameter to all parser_del methods ?
    timeval_set_now(&now);
    struct ip_subparser *ip_subparser = DOWNCAST(mux_subparser, mux_subparser, ip_subparser);
    ip_subparser_dtor(ip_subparser, &now);
    FREE(ip_subparser);
}

// Really construct the waiting list
static int ip_reassembly_ctor(struct ip_reassembly *reassembly, struct parser *parser, uint16_t id)
{
    SLOG(LOG_DEBUG, "Constructing ip_reassembly@%p for parser %s", reassembly, parser_name(parser));
    assert(! reassembly->constructed);

    reassembly->constructed = 1;
    reassembly->in_use = 1;
    reassembly->id = id;
    reassembly->got_last = 0;
    if (0 != pkt_wait_list_ctor(&reassembly->wl, 0, 5, 65536, 100, 65536, parser)) return -1;

    return 0;
}

static struct ip_reassembly *ip_reassembly_lookup(struct ip_subparser *ip_subparser, uint16_t id, struct parser *parser, struct timeval const *now)
{
    SLOG(LOG_DEBUG, "Looking for ip_reassembly for id=%"PRIu16" for subparser %s", id, parser_name(parser));

    int last_unused = -1;
    for (unsigned r = 0; r < NB_ELEMS(ip_subparser->reassembly); r++) {
        struct ip_reassembly *const reassembly = ip_subparser->reassembly + r;
        if (reassembly->in_use) {
            if (reassembly->id != id) continue;
            SLOG(LOG_DEBUG, "Found id at index %u in ip_reassembly@%p", r, reassembly);
            if (! reassembly->constructed) {
                ip_reassembly_ctor(reassembly, parser, id);
            }
            return reassembly;
        } else {
            last_unused = r;
        }
    }

    if (last_unused == -1) {
        static int target = 0;
        last_unused = target;
        target = (target+1) % NB_ELEMS(ip_subparser->reassembly);
        SLOG(LOG_DEBUG, "No slot left on ip_reassembly, reusing slot at index %u", last_unused);
        ip_reassembly_dtor(ip_subparser->reassembly + last_unused, now);
    }

    struct ip_reassembly *const reassembly = ip_subparser->reassembly + last_unused;
    assert(! reassembly->in_use);
    if (0 != ip_reassembly_ctor(reassembly, parser, id)) return NULL;
    return reassembly;
}

unsigned ip_key_ctor(struct ip_key *k, unsigned protocol, struct ip_addr const *src, struct ip_addr const *dst)
{
    k->protocol = protocol;
    if (ip_addr_cmp(src, dst) <= 0) {
        k->addr[0] = *src;
        k->addr[1] = *dst;
        return 0;
    }
    k->addr[0] = *dst;
    k->addr[1] = *src;
    return 1;
}

struct mux_subparser *ip_subparser_lookup(struct parser *parser, struct proto *proto, struct parser *requestor, unsigned protocol, struct ip_addr const *src, struct ip_addr const *dst, unsigned *way, struct timeval const *now)
{
    struct mux_parser *mux_parser = DOWNCAST(parser, parser, mux_parser);
    struct ip_key key;
    *way = ip_key_ctor(&key, protocol, src, dst);
    return mux_subparser_lookup(mux_parser, proto, requestor, &key, now);
}

/* The pkt_wait_list is now complete.
 * Construct a single payload from it, then call the subparse once for this payload.
 * But we also want to acknoledge the several IP fragments that were received, so we also must
 * call okfn for each IP info. The pkt_wait_list_dtor will do this for us. */
static void reassemble(struct ip_reassembly *reassembly, struct proto_info *parent, unsigned way, struct timeval const *now, proto_okfn_t *okfn)
{
    SLOG(LOG_DEBUG, "Reassembling ip_reassembly@%p", reassembly);

    uint8_t *payload = pkt_wait_list_reassemble(&reassembly->wl, 0, reassembly->end_offset);
    if (payload) {  // an obvious reason for !payload this would be that cap_len was not big enough
        // best effort attempt
        (void)proto_parse(reassembly->wl.parser, parent, way, payload, reassembly->end_offset, reassembly->end_offset, now, okfn);
        FREE(payload);
    }
    ip_reassembly_dtor(reassembly, now);
}

static enum proto_parse_status ip_parse(struct parser *parser, struct proto_info *parent, unsigned way, uint8_t const *packet, size_t cap_len, size_t wire_len, struct timeval const *now, proto_okfn_t *okfn)
{
    struct mux_parser *mux_parser = DOWNCAST(parser, parser, mux_parser);
    struct ip_hdr const *iphdr = (struct ip_hdr *)packet;

    // Sanity checks

    if (cap_len < sizeof(*iphdr)) return PROTO_TOO_SHORT;

    SLOG(LOG_DEBUG, "New packet of %zu bytes, proto %hu, %"PRINIPQUAD"->%"PRINIPQUAD,
        wire_len, iphdr->protocol, NIPQUAD(&iphdr->src), NIPQUAD(&iphdr->dst));

    size_t ip_len = ntohs(iphdr->tot_len);
    if (ip_len > wire_len) {
        SLOG(LOG_DEBUG, "Bogus IPv4 total length : %zu > %zu", ip_len, wire_len);
        return PROTO_PARSE_ERR;
    }

    if (iphdr->version != 4) {
        SLOG(LOG_DEBUG, "Bogus IPv4 version : %u instead of 4", (unsigned)iphdr->version);
        return PROTO_PARSE_ERR;
    }

    size_t iphdr_len = iphdr->hdr_len * 4;
    if (iphdr_len > ip_len) {
        SLOG(LOG_DEBUG, "Bogus IPv4 header length : %zu > %zu", iphdr_len, ip_len);
        return PROTO_PARSE_ERR;
    }

    if (iphdr_len > cap_len) return PROTO_TOO_SHORT;

    // Parse

    struct ip_proto_info info;
    ip_proto_info_ctor(&info, parser, parent, iphdr_len, ip_len - iphdr_len, iphdr);

    // Find subparser

    struct mux_subparser *subparser = NULL;
    struct ip_subproto *subproto;
    LIST_FOREACH(subproto, &ip_subprotos, entry) {
        if (subproto->protocol == info.key.protocol) {
            // We have a subproto for this protocol value, look for a parser of this subproto in our mux_subparsers hash (or create a new one)
            struct ip_key subparser_key;
            info.way = ip_key_ctor(&subparser_key, info.key.protocol, info.key.addr+0, info.key.addr+1);
            subparser = mux_subparser_lookup(mux_parser, subproto->proto, NULL, &subparser_key, now);
            break;
        }
    }
    if (! subparser) {
        SLOG(LOG_DEBUG, "IPv4 protocol %u unknown", iphdr->protocol);
        goto fallback;
    }

    // If we have a fragment, maybe we can't parse payload right now
    if (is_fragment(iphdr)) {
        struct ip_subparser *ip_subparser = DOWNCAST(subparser, mux_subparser, ip_subparser);
        unsigned const offset = fragment_offset(iphdr);
        uint16_t id = ntohs(iphdr->id); // Not required but ease debugging
        SLOG(LOG_DEBUG, "IP packet is a fragment of id %"PRIu16", offset=%u", id, offset);
        struct ip_reassembly *reassembly = ip_reassembly_lookup(ip_subparser, id, subparser->parser, now);
        if (! reassembly) goto fallback;
        assert(reassembly->in_use && reassembly->constructed);
        size_t frag_len = wire_len - iphdr_len;
        if (! iphdr->more_fragments) {
            reassembly->got_last = 1;
            reassembly->end_offset = offset + frag_len;
        }
        if (PROTO_OK != pkt_wait_list_add(&reassembly->wl, offset, offset + frag_len, false, &info.info, info.way, packet + iphdr_len, cap_len - iphdr_len, frag_len, now, okfn)) {
            goto fallback;  // should not happen
        }
        if (reassembly->got_last && pkt_wait_list_is_complete(&reassembly->wl, 0, reassembly->end_offset)) {
            reassemble(reassembly, parent, way, now, okfn);
        }
        return PROTO_OK;
    }

    // Parse it at once
    if (PROTO_OK != proto_parse(subparser->parser, &info.info, info.way, packet + iphdr_len, cap_len - iphdr_len, wire_len - iphdr_len, now, okfn)) goto fallback;
    return PROTO_OK;

fallback:
    (void)proto_parse(NULL, &info.info, info.way, packet + iphdr_len, cap_len - iphdr_len, wire_len - iphdr_len, now, okfn);
    return PROTO_OK;
}

/*
 * Construction/Destruction
 */

static struct mux_proto mux_proto_ip;
struct proto *proto_ip = &mux_proto_ip.proto;
static struct eth_subproto eth_subproto;

void ip_init(void)
{
    log_category_proto_ip_init();

    static struct proto_ops const ops = {
        .parse = ip_parse,
        .parser_new = mux_parser_new,
        .parser_del = mux_parser_del,
        .info_2_str = ip_info_2_str,
        .info_addr  = ip_info_addr,
    };
    static struct mux_proto_ops const mux_ops = {
        .subparser_new = ip_subparser_new,
        .subparser_del = ip_subparser_del,
    };
    mux_proto_ctor(&mux_proto_ip, &ops, &mux_ops, "IPv4", IP_TIMEOUT, sizeof(struct ip_key), IP_HASH_SIZE);
    eth_subproto_ctor(&eth_subproto, ETH_PROTO_IPv4, proto_ip);
    LIST_INIT(&ip_subprotos);
}

void ip_fini(void)
{
    assert(LIST_EMPTY(&ip_subprotos));
    eth_subproto_dtor(&eth_subproto);
    mux_proto_dtor(&mux_proto_ip);
    log_category_proto_ip_fini();
}
