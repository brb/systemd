/* SPDX-License-Identifier: LGPL-2.1+ */

#include <getopt.h>
#include <linux/if_addrlabel.h>
#include <net/if.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "sd-bus.h"
#include "sd-device.h"
#include "sd-hwdb.h"
#include "sd-lldp.h"
#include "sd-netlink.h"
#include "sd-network.h"

#include "alloc-util.h"
#include "arphrd-list.h"
#include "bus-common-errors.h"
#include "bus-error.h"
#include "bus-util.h"
#include "device-util.h"
#include "ether-addr-util.h"
#include "fd-util.h"
#include "format-table.h"
#include "format-util.h"
#include "hwdb-util.h"
#include "local-addresses.h"
#include "locale-util.h"
#include "macro.h"
#include "main-func.h"
#include "netlink-util.h"
#include "pager.h"
#include "parse-util.h"
#include "pretty-print.h"
#include "set.h"
#include "socket-util.h"
#include "sort-util.h"
#include "sparse-endian.h"
#include "stdio-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "strxcpyx.h"
#include "terminal-util.h"
#include "verbs.h"

static PagerFlags arg_pager_flags = 0;
static bool arg_legend = true;
static bool arg_all = false;
static bool arg_stats = false;

static char *link_get_type_string(unsigned short iftype, sd_device *d) {
        const char *t, *devtype;
        char *p;

        if (d &&
            sd_device_get_devtype(d, &devtype) >= 0 &&
            !isempty(devtype))
                return strdup(devtype);

        t = arphrd_to_name(iftype);
        if (!t)
                return NULL;

        p = strdup(t);
        if (!p)
                return NULL;

        ascii_strlower(p);
        return p;
}

static void operational_state_to_color(const char *state, const char **on, const char **off) {
        assert(on);
        assert(off);

        if (STRPTR_IN_SET(state, "routable", "enslaved")) {
                *on = ansi_highlight_green();
                *off = ansi_normal();
        } else if (streq_ptr(state, "degraded")) {
                *on = ansi_highlight_yellow();
                *off = ansi_normal();
        } else
                *on = *off = "";
}

static void setup_state_to_color(const char *state, const char **on, const char **off) {
        assert(on);
        assert(off);

        if (streq_ptr(state, "configured")) {
                *on = ansi_highlight_green();
                *off = ansi_normal();
        } else if (streq_ptr(state, "configuring")) {
                *on = ansi_highlight_yellow();
                *off = ansi_normal();
        } else if (STRPTR_IN_SET(state, "failed", "linger")) {
                *on = ansi_highlight_red();
                *off = ansi_normal();
        } else
                *on = *off = "";
}

typedef struct LinkInfo {
        char name[IFNAMSIZ+1];
        int ifindex;
        unsigned short iftype;
        struct ether_addr mac_address;
        uint32_t mtu;
        uint32_t min_mtu;
        uint32_t max_mtu;
        uint32_t tx_queues;
        uint32_t rx_queues;

        union {
                struct rtnl_link_stats64 stats64;
                struct rtnl_link_stats stats;
        };

        double tx_bitrate;
        double rx_bitrate;

        bool has_mac_address:1;
        bool has_tx_queues:1;
        bool has_rx_queues:1;
        bool has_stats64:1;
        bool has_stats:1;
        bool has_bitrates:1;
} LinkInfo;

static int link_info_compare(const LinkInfo *a, const LinkInfo *b) {
        return CMP(a->ifindex, b->ifindex);
}

static int decode_link(sd_netlink_message *m, LinkInfo *info, char **patterns) {
        const char *name;
        uint16_t type;
        int ifindex, r;

        assert(m);
        assert(info);

        r = sd_netlink_message_get_type(m, &type);
        if (r < 0)
                return r;

        if (type != RTM_NEWLINK)
                return 0;

        r = sd_rtnl_message_link_get_ifindex(m, &ifindex);
        if (r < 0)
                return r;

        r = sd_netlink_message_read_string(m, IFLA_IFNAME, &name);
        if (r < 0)
                return r;

        if (patterns) {
                char str[DECIMAL_STR_MAX(int)];

                xsprintf(str, "%i", ifindex);

                if (!strv_fnmatch(patterns, str, 0) && !strv_fnmatch(patterns, name, 0))
                        return 0;
        }

        r = sd_rtnl_message_link_get_type(m, &info->iftype);
        if (r < 0)
                return r;

        strscpy(info->name, sizeof info->name, name);
        info->ifindex = ifindex;

        info->has_mac_address =
                sd_netlink_message_read_ether_addr(m, IFLA_ADDRESS, &info->mac_address) >= 0 &&
                memcmp(&info->mac_address, &ETHER_ADDR_NULL, sizeof(struct ether_addr)) != 0;

        (void) sd_netlink_message_read_u32(m, IFLA_MTU, &info->mtu);
        (void) sd_netlink_message_read_u32(m, IFLA_MIN_MTU, &info->min_mtu);
        (void) sd_netlink_message_read_u32(m, IFLA_MAX_MTU, &info->max_mtu);

        info->has_rx_queues =
                sd_netlink_message_read_u32(m, IFLA_NUM_RX_QUEUES, &info->rx_queues) >= 0 &&
                info->rx_queues > 0;

        info->has_tx_queues =
                sd_netlink_message_read_u32(m, IFLA_NUM_TX_QUEUES, &info->tx_queues) >= 0 &&
                info->tx_queues > 0;

        if (sd_netlink_message_read(m, IFLA_STATS64, sizeof info->stats64, &info->stats64) >= 0)
                info->has_stats64 = true;
        else if (sd_netlink_message_read(m, IFLA_STATS, sizeof info->stats, &info->stats) >= 0)
                info->has_stats = true;

        return 1;
}

static int acquire_link_bitrates(sd_bus *bus, LinkInfo *link) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_free_ char *path = NULL, *ifindex_str = NULL;
        int r;

        if (asprintf(&ifindex_str, "%i", link->ifindex) < 0)
                return -ENOMEM;

        r = sd_bus_path_encode("/org/freedesktop/network1/link", ifindex_str, &path);
        if (r < 0)
                return r;

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.network1",
                        path,
                        "org.freedesktop.DBus.Properties",
                        "Get",
                        &error,
                        &reply,
                        "ss",
                        "org.freedesktop.network1.Link",
                        "BitRates");
        if (r < 0) {
                bool quiet = sd_bus_error_has_name(&error, SD_BUS_ERROR_UNKNOWN_PROPERTY) ||
                             sd_bus_error_has_name(&error, BUS_ERROR_SPEED_METER_INACTIVE);

                return log_full_errno(quiet ? LOG_DEBUG : LOG_WARNING,
                                      r, "Failed to query link bit rates: %s", bus_error_message(&error, r));
        }

        r = sd_bus_message_enter_container(reply, 'v', "(dd)");
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_read(reply, "(dd)", &link->tx_bitrate, &link->rx_bitrate);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        link->has_bitrates = link->tx_bitrate >= 0 && link->rx_bitrate >= 0;

        return 0;
}

static int acquire_link_info(sd_bus *bus, sd_netlink *rtnl, char **patterns, LinkInfo **ret) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        _cleanup_free_ LinkInfo *links = NULL;
        size_t allocated = 0, c = 0, j;
        sd_netlink_message *i;
        int r;

        assert(rtnl);
        assert(ret);

        r = sd_rtnl_message_new_link(rtnl, &req, RTM_GETLINK, 0);
        if (r < 0)
                return rtnl_log_create_error(r);

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return rtnl_log_create_error(r);

        r = sd_netlink_call(rtnl, req, 0, &reply);
        if (r < 0)
                return log_error_errno(r, "Failed to enumerate links: %m");

        for (i = reply; i; i = sd_netlink_message_next(i)) {
                if (!GREEDY_REALLOC0(links, allocated, c+1))
                        return -ENOMEM;

                r = decode_link(i, links + c, patterns);
                if (r < 0)
                        return r;
                if (r > 0)
                        c++;
        }

        typesafe_qsort(links, c, link_info_compare);

        if (bus)
                for (j = 0; j < c; j++)
                        (void) acquire_link_bitrates(bus, links + j);

        *ret = TAKE_PTR(links);

        return (int) c;
}

static int list_links(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        _cleanup_free_ LinkInfo *links = NULL;
        _cleanup_(table_unrefp) Table *table = NULL;
        TableCell *cell;
        int c, i, r;

        r = sd_netlink_open(&rtnl);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to netlink: %m");

        c = acquire_link_info(NULL, rtnl, argc > 1 ? argv + 1 : NULL, &links);
        if (c < 0)
                return c;

        (void) pager_open(arg_pager_flags);

        table = table_new("IDX", "LINK", "TYPE", "OPERATIONAL", "SETUP");
        if (!table)
                return log_oom();

        table_set_header(table, arg_legend);

        assert_se(cell = table_get_cell(table, 0, 0));
        (void) table_set_minimum_width(table, cell, 3);
        (void) table_set_weight(table, cell, 0);
        (void) table_set_ellipsize_percent(table, cell, 100);
        (void) table_set_align_percent(table, cell, 100);

        assert_se(cell = table_get_cell(table, 0, 1));
        (void) table_set_ellipsize_percent(table, cell, 100);

        for (i = 0; i < c; i++) {
                _cleanup_free_ char *setup_state = NULL, *operational_state = NULL;
                _cleanup_(sd_device_unrefp) sd_device *d = NULL;
                const char *on_color_operational, *off_color_operational,
                           *on_color_setup, *off_color_setup;
                char devid[2 + DECIMAL_STR_MAX(int)];
                _cleanup_free_ char *t = NULL;

                (void) sd_network_link_get_operational_state(links[i].ifindex, &operational_state);
                operational_state_to_color(operational_state, &on_color_operational, &off_color_operational);

                r = sd_network_link_get_setup_state(links[i].ifindex, &setup_state);
                if (r == -ENODATA) /* If there's no info available about this iface, it's unmanaged by networkd */
                        setup_state = strdup("unmanaged");
                setup_state_to_color(setup_state, &on_color_setup, &off_color_setup);

                xsprintf(devid, "n%i", links[i].ifindex);
                (void) sd_device_new_from_device_id(&d, devid);

                t = link_get_type_string(links[i].iftype, d);

                r = table_add_cell(table, NULL, TABLE_INT, &links[i].ifindex);
                if (r < 0)
                        return r;

                r = table_add_many(table,
                                   TABLE_STRING, links[i].name,
                                   TABLE_STRING, strna(t));
                if (r < 0)
                        return r;

                r = table_add_cell(table, &cell, TABLE_STRING, strna(operational_state));
                if (r < 0)
                        return r;

                (void) table_set_color(table, cell, on_color_operational);

                r = table_add_cell(table, &cell, TABLE_STRING, strna(setup_state));
                if (r < 0)
                        return r;

                (void) table_set_color(table, cell, on_color_setup);
        }

        r = table_print(table, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to print table: %m");

        if (arg_legend)
                printf("\n%i links listed.\n", c);

        return 0;
}

/* IEEE Organizationally Unique Identifier vendor string */
static int ieee_oui(sd_hwdb *hwdb, const struct ether_addr *mac, char **ret) {
        const char *description;
        char modalias[STRLEN("OUI:XXYYXXYYXXYY") + 1], *desc;
        int r;

        assert(ret);

        if (!hwdb)
                return -EINVAL;

        if (!mac)
                return -EINVAL;

        /* skip commonly misused 00:00:00 (Xerox) prefix */
        if (memcmp(mac, "\0\0\0", 3) == 0)
                return -EINVAL;

        xsprintf(modalias, "OUI:" ETHER_ADDR_FORMAT_STR,
                 ETHER_ADDR_FORMAT_VAL(*mac));

        r = sd_hwdb_get(hwdb, modalias, "ID_OUI_FROM_DATABASE", &description);
        if (r < 0)
                return r;

        desc = strdup(description);
        if (!desc)
                return -ENOMEM;

        *ret = desc;

        return 0;
}

static int get_gateway_description(
                sd_netlink *rtnl,
                sd_hwdb *hwdb,
                int ifindex,
                int family,
                union in_addr_union *gateway,
                char **gateway_description) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        sd_netlink_message *m;
        int r;

        assert(rtnl);
        assert(ifindex >= 0);
        assert(IN_SET(family, AF_INET, AF_INET6));
        assert(gateway);
        assert(gateway_description);

        r = sd_rtnl_message_new_neigh(rtnl, &req, RTM_GETNEIGH, ifindex, family);
        if (r < 0)
                return r;

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call(rtnl, req, 0, &reply);
        if (r < 0)
                return r;

        for (m = reply; m; m = sd_netlink_message_next(m)) {
                union in_addr_union gw = IN_ADDR_NULL;
                struct ether_addr mac = ETHER_ADDR_NULL;
                uint16_t type;
                int ifi, fam;

                r = sd_netlink_message_get_errno(m);
                if (r < 0) {
                        log_error_errno(r, "got error: %m");
                        continue;
                }

                r = sd_netlink_message_get_type(m, &type);
                if (r < 0) {
                        log_error_errno(r, "could not get type: %m");
                        continue;
                }

                if (type != RTM_NEWNEIGH) {
                        log_error("type is not RTM_NEWNEIGH");
                        continue;
                }

                r = sd_rtnl_message_neigh_get_family(m, &fam);
                if (r < 0) {
                        log_error_errno(r, "could not get family: %m");
                        continue;
                }

                if (fam != family) {
                        log_error("family is not correct");
                        continue;
                }

                r = sd_rtnl_message_neigh_get_ifindex(m, &ifi);
                if (r < 0) {
                        log_error_errno(r, "could not get ifindex: %m");
                        continue;
                }

                if (ifindex > 0 && ifi != ifindex)
                        continue;

                switch (fam) {
                case AF_INET:
                        r = sd_netlink_message_read_in_addr(m, NDA_DST, &gw.in);
                        if (r < 0)
                                continue;

                        break;
                case AF_INET6:
                        r = sd_netlink_message_read_in6_addr(m, NDA_DST, &gw.in6);
                        if (r < 0)
                                continue;

                        break;
                default:
                        continue;
                }

                if (!in_addr_equal(fam, &gw, gateway))
                        continue;

                r = sd_netlink_message_read_ether_addr(m, NDA_LLADDR, &mac);
                if (r < 0)
                        continue;

                r = ieee_oui(hwdb, &mac, gateway_description);
                if (r < 0)
                        continue;

                return 0;
        }

        return -ENODATA;
}

static int dump_gateways(
                sd_netlink *rtnl,
                sd_hwdb *hwdb,
                Table *table,
                int ifindex) {
        _cleanup_free_ struct local_address *local = NULL;
        int r, n, i;

        assert(rtnl);
        assert(table);

        n = local_gateways(rtnl, ifindex, AF_UNSPEC, &local);
        if (n < 0)
                return n;

        for (i = 0; i < n; i++) {
                _cleanup_free_ char *gateway = NULL, *description = NULL, *with_description = NULL;

                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;

                r = table_add_cell(table, NULL, TABLE_STRING, i == 0 ? "Gateway:" : "");
                if (r < 0)
                        return r;

                r = in_addr_to_string(local[i].family, &local[i].address, &gateway);
                if (r < 0)
                        return r;

                r = get_gateway_description(rtnl, hwdb, local[i].ifindex, local[i].family, &local[i].address, &description);
                if (r < 0)
                        log_debug_errno(r, "Could not get description of gateway: %m");

                if (description) {
                        with_description = strjoin(gateway, " (", description, ")");
                        if (!with_description)
                                return -ENOMEM;
                }

                /* Show interface name for the entry if we show
                 * entries for all interfaces */
                if (ifindex <= 0) {
                        char name[IF_NAMESIZE+1];

                        if (format_ifname(local[i].ifindex, name))
                                r = table_add_cell_stringf(table, NULL, "%s on %s", with_description ?: gateway, name);
                        else
                                r = table_add_cell_stringf(table, NULL, "%s on %%%i", with_description ?: gateway, local[i].ifindex);
                } else
                        r = table_add_cell(table, NULL, TABLE_STRING, with_description ?: gateway);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int dump_addresses(
                sd_netlink *rtnl,
                Table *table,
                int ifindex) {

        _cleanup_free_ struct local_address *local = NULL;
        int r, n, i;

        assert(rtnl);
        assert(table);

        n = local_addresses(rtnl, ifindex, AF_UNSPEC, &local);
        if (n < 0)
                return n;

        for (i = 0; i < n; i++) {
                _cleanup_free_ char *pretty = NULL;

                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;

                r = table_add_cell(table, NULL, TABLE_STRING, i == 0 ? "Address:" : "");
                if (r < 0)
                        return r;

                r = in_addr_to_string(local[i].family, &local[i].address, &pretty);
                if (r < 0)
                        return r;

                if (ifindex <= 0) {
                        char name[IF_NAMESIZE+1];

                        if (format_ifname(local[i].ifindex, name))
                                r = table_add_cell_stringf(table, NULL, "%s on %s", pretty, name);
                        else
                                r = table_add_cell_stringf(table, NULL, "%s on %%%i", pretty, local[i].ifindex);
                } else
                        r = table_add_cell(table, NULL, TABLE_STRING, pretty);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int dump_address_labels(sd_netlink *rtnl) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        _cleanup_(table_unrefp) Table *table = NULL;
        sd_netlink_message *m;
        TableCell *cell;
        int r;

        assert(rtnl);

        r = sd_rtnl_message_new_addrlabel(rtnl, &req, RTM_GETADDRLABEL, 0, AF_INET6);
        if (r < 0)
                return log_error_errno(r, "Could not allocate RTM_GETADDRLABEL message: %m");

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_netlink_call(rtnl, req, 0, &reply);
        if (r < 0)
                return r;

        table = table_new("Label", "Prefix/Prefixlen");
        if (!table)
                return -ENOMEM;

        r = table_set_sort(table, 0, SIZE_MAX);
        if (r < 0)
                return r;

        assert_se(cell = table_get_cell(table, 0, 0));
        (void) table_set_align_percent(table, cell, 100);
        (void) table_set_ellipsize_percent(table, cell, 100);

        assert_se(cell = table_get_cell(table, 0, 1));
        (void) table_set_align_percent(table, cell, 100);

        for (m = reply; m; m = sd_netlink_message_next(m)) {
                _cleanup_free_ char *pretty = NULL;
                union in_addr_union prefix = IN_ADDR_NULL;
                uint8_t prefixlen;
                uint32_t label;

                r = sd_netlink_message_get_errno(m);
                if (r < 0) {
                        log_error_errno(r, "got error: %m");
                        continue;
                }

                r = sd_netlink_message_read_u32(m, IFAL_LABEL, &label);
                if (r < 0 && r != -ENODATA) {
                        log_error_errno(r, "Could not read IFAL_LABEL, ignoring: %m");
                        continue;
                }

                r = sd_netlink_message_read_in6_addr(m, IFAL_ADDRESS, &prefix.in6);
                if (r < 0)
                        continue;

                r = in_addr_to_string(AF_INET6, &prefix, &pretty);
                if (r < 0)
                        continue;

                r = sd_rtnl_message_addrlabel_get_prefixlen(m, &prefixlen);
                if (r < 0)
                        continue;

                r = table_add_cell(table, NULL, TABLE_UINT32, &label);
                if (r < 0)
                        return r;

                r = table_add_cell_stringf(table, &cell, "%s/%u", pretty, prefixlen);
                if (r < 0)
                        return r;
        }

        return table_print(table, NULL);
}

static int list_address_labels(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        int r;

        r = sd_netlink_open(&rtnl);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to netlink: %m");

        dump_address_labels(rtnl);

        return 0;
}

static int open_lldp_neighbors(int ifindex, FILE **ret) {
        _cleanup_free_ char *p = NULL;
        FILE *f;

        if (asprintf(&p, "/run/systemd/netif/lldp/%i", ifindex) < 0)
                return -ENOMEM;

        f = fopen(p, "re");
        if (!f)
                return -errno;

        *ret = f;
        return 0;
}

static int next_lldp_neighbor(FILE *f, sd_lldp_neighbor **ret) {
        _cleanup_free_ void *raw = NULL;
        size_t l;
        le64_t u;
        int r;

        assert(f);
        assert(ret);

        l = fread(&u, 1, sizeof(u), f);
        if (l == 0 && feof(f))
                return 0;
        if (l != sizeof(u))
                return -EBADMSG;

        /* each LLDP packet is at most MTU size, but let's allow up to 4KiB just in case */
        if (le64toh(u) >= 4096)
                return -EBADMSG;

        raw = new(uint8_t, le64toh(u));
        if (!raw)
                return -ENOMEM;

        if (fread(raw, 1, le64toh(u), f) != le64toh(u))
                return -EBADMSG;

        r = sd_lldp_neighbor_from_raw(ret, raw, le64toh(u));
        if (r < 0)
                return r;

        return 1;
}

static int dump_lldp_neighbors(Table *table, const char *prefix, int ifindex) {
        _cleanup_fclose_ FILE *f = NULL;
        int r, c = 0;

        assert(table);
        assert(prefix);
        assert(ifindex > 0);

        r = open_lldp_neighbors(ifindex, &f);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return r;

        for (;;) {
                const char *system_name = NULL, *port_id = NULL, *port_description = NULL;
                _cleanup_(sd_lldp_neighbor_unrefp) sd_lldp_neighbor *n = NULL;
                _cleanup_free_ char *str = NULL;

                r = next_lldp_neighbor(f, &n);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;

                r = table_add_cell(table, NULL, TABLE_STRING, c == 0 ? prefix : "");
                if (r < 0)
                        return r;

                (void) sd_lldp_neighbor_get_system_name(n, &system_name);
                (void) sd_lldp_neighbor_get_port_id_as_string(n, &port_id);
                (void) sd_lldp_neighbor_get_port_description(n, &port_description);

                if (asprintf(&str, "%s on port %s%s%s%s",
                             strna(system_name), strna(port_id),
                             isempty(port_description) ? "" : " (",
                             port_description,
                             isempty(port_description) ? "" : ")") < 0)
                        return -ENOMEM;

                r = table_add_cell(table, NULL, TABLE_STRING, str);
                if (r < 0)
                        return r;

                c++;
        }

        return c;
}

static int dump_ifindexes(Table *table, const char *prefix, const int *ifindexes) {
        unsigned c;
        int r;

        assert(prefix);

        if (!ifindexes || ifindexes[0] <= 0)
                return 0;

        for (c = 0; ifindexes[c] > 0; c++) {
                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;

                r = table_add_cell(table, NULL, TABLE_STRING, c == 0 ? prefix : "");
                if (r < 0)
                        return r;

                r = table_add_cell(table, NULL, TABLE_IFINDEX, ifindexes + c);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int dump_list(Table *table, const char *prefix, char **l) {
        char **i;
        int r;

        if (strv_isempty(l))
                return 0;

        STRV_FOREACH(i, l) {
                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;

                r = table_add_cell(table, NULL, TABLE_STRING, i == l ? prefix : "");
                if (r < 0)
                        return r;

                r = table_add_cell(table, NULL, TABLE_STRING, *i);
                if (r < 0)
                        return r;
        }

        return 0;
}

#define DUMP_STATS_ONE(name, val_name)                                  \
        r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);             \
        if (r < 0)                                                      \
                return r;                                               \
        r = table_add_cell(table, NULL, TABLE_STRING, name ":");   \
        if (r < 0)                                                      \
                return r;                                               \
        r = table_add_cell(table, NULL, info->has_stats64 ? TABLE_UINT64 : TABLE_UINT32, \
                           info->has_stats64 ? (void*) &info->stats64.val_name : (void*) &info->stats.val_name); \
        if (r < 0)                                                      \
                return r;

static int dump_statistics(Table *table, const LinkInfo *info) {
        int r;

        if (!arg_stats)
                return 0;

        if (!info->has_stats64 && !info->has_stats)
                return 0;

        DUMP_STATS_ONE("Rx Packets", rx_packets);
        DUMP_STATS_ONE("Tx Packets", tx_packets);
        DUMP_STATS_ONE("Rx Bytes", rx_bytes);
        DUMP_STATS_ONE("Tx Bytes", tx_bytes);
        DUMP_STATS_ONE("Rx Errors", rx_errors);
        DUMP_STATS_ONE("Tx Errors", tx_errors);
        DUMP_STATS_ONE("Rx Dropped", rx_dropped);
        DUMP_STATS_ONE("Tx Dropped", tx_dropped);
        DUMP_STATS_ONE("Multicast Packets", multicast);
        DUMP_STATS_ONE("Collisions", collisions);

        return 0;
}

static const struct {
        double val;
        const char *str;
} prefix_table[] = {
        { .val = 1e15, .str = "P" },
        { .val = 1e12, .str = "T" },
        { .val = 1e9,  .str = "G" },
        { .val = 1e6,  .str = "M" },
        { .val = 1e3,  .str = "k" },
};

static void get_prefix(double val, double *ret_div, const char **ret_prefix) {
        assert(ret_div);
        assert(ret_prefix);

        for (size_t i = 0; i < ELEMENTSOF(prefix_table); i++)
                if (val > prefix_table[i].val) {
                        *ret_div = prefix_table[i].val;
                        *ret_prefix = prefix_table[i].str;
                        return;
                }

        *ret_div = 1;
        *ret_prefix = NULL;
}

static int link_status_one(
                sd_netlink *rtnl,
                sd_hwdb *hwdb,
                const LinkInfo *info) {

        _cleanup_strv_free_ char **dns = NULL, **ntp = NULL, **search_domains = NULL, **route_domains = NULL;
        _cleanup_free_ char *setup_state = NULL, *operational_state = NULL, *tz = NULL;
        _cleanup_(sd_device_unrefp) sd_device *d = NULL;
        char devid[2 + DECIMAL_STR_MAX(int)];
        _cleanup_free_ char *t = NULL, *network = NULL;
        const char *driver = NULL, *path = NULL, *vendor = NULL, *model = NULL, *link = NULL;
        const char *on_color_operational, *off_color_operational,
                *on_color_setup, *off_color_setup;
        _cleanup_free_ int *carrier_bound_to = NULL, *carrier_bound_by = NULL;
        _cleanup_(table_unrefp) Table *table = NULL;
        TableCell *cell;
        int r;

        assert(rtnl);
        assert(info);

        (void) sd_network_link_get_operational_state(info->ifindex, &operational_state);
        operational_state_to_color(operational_state, &on_color_operational, &off_color_operational);

        r = sd_network_link_get_setup_state(info->ifindex, &setup_state);
        if (r == -ENODATA) /* If there's no info available about this iface, it's unmanaged by networkd */
                setup_state = strdup("unmanaged");
        setup_state_to_color(setup_state, &on_color_setup, &off_color_setup);

        (void) sd_network_link_get_dns(info->ifindex, &dns);
        (void) sd_network_link_get_search_domains(info->ifindex, &search_domains);
        (void) sd_network_link_get_route_domains(info->ifindex, &route_domains);
        (void) sd_network_link_get_ntp(info->ifindex, &ntp);

        xsprintf(devid, "n%i", info->ifindex);

        (void) sd_device_new_from_device_id(&d, devid);

        if (d) {
                (void) sd_device_get_property_value(d, "ID_NET_LINK_FILE", &link);
                (void) sd_device_get_property_value(d, "ID_NET_DRIVER", &driver);
                (void) sd_device_get_property_value(d, "ID_PATH", &path);

                if (sd_device_get_property_value(d, "ID_VENDOR_FROM_DATABASE", &vendor) < 0)
                        (void) sd_device_get_property_value(d, "ID_VENDOR", &vendor);

                if (sd_device_get_property_value(d, "ID_MODEL_FROM_DATABASE", &model) < 0)
                        (void) sd_device_get_property_value(d, "ID_MODEL", &model);
        }

        t = link_get_type_string(info->iftype, d);

        (void) sd_network_link_get_network_file(info->ifindex, &network);

        (void) sd_network_link_get_carrier_bound_to(info->ifindex, &carrier_bound_to);
        (void) sd_network_link_get_carrier_bound_by(info->ifindex, &carrier_bound_by);

        table = table_new("DOT", "KEY", "VALUE");
        if (!table)
                return -ENOMEM;

        assert_se(cell = table_get_cell(table, 0, 0));
        (void) table_set_ellipsize_percent(table, cell, 100);

        assert_se(cell = table_get_cell(table, 0, 1));
        (void) table_set_ellipsize_percent(table, cell, 100);

        table_set_header(table, false);

        r = table_add_cell(table, &cell, TABLE_STRING, special_glyph(SPECIAL_GLYPH_BLACK_CIRCLE));
        if (r < 0)
                return r;
        (void) table_set_color(table, cell, on_color_operational);
        r = table_add_cell_stringf(table, &cell, "%i: %s", info->ifindex, info->name);
        if (r < 0)
                return r;
        (void) table_set_align_percent(table, cell, 0);
        r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
        if (r < 0)
                return r;

        r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
        if (r < 0)
                return r;
        r = table_add_cell(table, &cell, TABLE_STRING, "Link File:");
        if (r < 0)
                return r;
        (void) table_set_align_percent(table, cell, 100);
        r = table_add_cell(table, NULL, TABLE_STRING, strna(link));
        if (r < 0)
                return r;

        r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
        if (r < 0)
                return r;
        r = table_add_cell(table, NULL, TABLE_STRING, "Network File:");
        if (r < 0)
                return r;
        r = table_add_cell(table, NULL, TABLE_STRING, strna(network));
        if (r < 0)
                return r;

        r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
        if (r < 0)
                return r;
        r = table_add_cell(table, NULL, TABLE_STRING, "Type:");
        if (r < 0)
                return r;
        r = table_add_cell(table, NULL, TABLE_STRING, strna(t));
        if (r < 0)
                return r;

        r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
        if (r < 0)
                return r;
        r = table_add_cell(table, NULL, TABLE_STRING, "State:");
        if (r < 0)
                return r;
        r = table_add_cell_stringf(table, NULL, "%s%s%s (%s%s%s)",
                                  on_color_operational, strna(operational_state), off_color_operational,
                                  on_color_setup, strna(setup_state), off_color_setup);
        if (r < 0)
                return r;

        if (path) {
                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, "Path:");
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, path);
                if (r < 0)
                        return r;
        }
        if (driver) {
                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, "Driver:");
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, driver);
                if (r < 0)
                        return r;
        }
        if (vendor) {
                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, "Vendor:");
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, vendor);
                if (r < 0)
                        return r;
        }
        if (model) {
                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, "Model:");
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, model);
                if (r < 0)
                        return r;
        }

        if (info->has_mac_address) {
                _cleanup_free_ char *description = NULL;
                char ea[ETHER_ADDR_TO_STRING_MAX];

                (void) ieee_oui(hwdb, &info->mac_address, &description);

                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, "HW Address:");
                if (r < 0)
                        return r;
                r = table_add_cell_stringf(table, NULL, "%s%s%s%s",
                                          ether_addr_to_string(&info->mac_address, ea),
                                          description ? " (" : "",
                                          description,
                                          description ? ")" : "");
                if (r < 0)
                        return r;
        }

        if (info->mtu > 0) {
                char min_str[DECIMAL_STR_MAX(uint32_t)], max_str[DECIMAL_STR_MAX(uint32_t)];

                xsprintf(min_str, "%" PRIu32, info->min_mtu);
                xsprintf(max_str, "%" PRIu32, info->max_mtu);

                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, "MTU:");
                if (r < 0)
                        return r;
                r = table_add_cell_stringf(table, NULL, "%" PRIu32 "%s%s%s%s%s%s%s",
                                           info->mtu,
                                           info->min_mtu > 0 || info->max_mtu > 0 ? " (" : "",
                                           info->min_mtu > 0 ? "min: " : "",
                                           info->min_mtu > 0 ? min_str : "",
                                           info->min_mtu > 0 && info->max_mtu > 0 ? ", " : "",
                                           info->max_mtu > 0 ? "max: " : "",
                                           info->max_mtu > 0 ? max_str : "",
                                           info->min_mtu > 0 || info->max_mtu > 0 ? ")" : "");
                if (r < 0)
                        return r;
        }

        if (info->has_bitrates) {
                const char *tx_prefix, *rx_prefix;
                double tx_div, rx_div;

                get_prefix(info->tx_bitrate, &tx_div, &tx_prefix);
                get_prefix(info->rx_bitrate, &rx_div, &rx_prefix);

                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, "Bit Rate (Tx/Rx):");
                if (r < 0)
                        return r;

                r = table_add_cell_stringf(table, NULL, "%.4g %sbps/%.4g %sbps",
                                           info->tx_bitrate / tx_div, strempty(tx_prefix),
                                           info->rx_bitrate / rx_div, strempty(rx_prefix));
                if (r < 0)
                        return r;
        }

        if (info->has_tx_queues || info->has_rx_queues) {
                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, "Queue Length (Tx/Rx):");
                if (r < 0)
                        return r;
                r = table_add_cell_stringf(table, NULL, "%" PRIu32 "/%" PRIu32, info->tx_queues, info->rx_queues);
                if (r < 0)
                        return r;
        }

        r = dump_addresses(rtnl, table, info->ifindex);
        if (r < 0)
                return r;
        r = dump_gateways(rtnl, hwdb, table, info->ifindex);
        if (r < 0)
                return r;
        r = dump_list(table, "DNS:", dns);
        if (r < 0)
                return r;
        r = dump_list(table, "Search Domains:", search_domains);
        if (r < 0)
                return r;
        r = dump_list(table, "Route Domains:", route_domains);
        if (r < 0)
                return r;
        r = dump_list(table, "NTP:", ntp);
        if (r < 0)
                return r;
        r = dump_ifindexes(table, "Carrier Bound To:", carrier_bound_to);
        if (r < 0)
                return r;
        r = dump_ifindexes(table, "Carrier Bound By:", carrier_bound_by);
        if (r < 0)
                return r;

        (void) sd_network_link_get_timezone(info->ifindex, &tz);
        if (tz) {
                r = table_add_cell(table, NULL, TABLE_EMPTY, NULL);
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, "Time Zone:");
                if (r < 0)
                        return r;
                r = table_add_cell(table, NULL, TABLE_STRING, tz);
                if (r < 0)
                        return r;
        }

        r = dump_lldp_neighbors(table, "Connected To:", info->ifindex);
        if (r < 0)
                return r;

        r = dump_statistics(table, info);
        if (r < 0)
                return r;

        return table_print(table, NULL);
}

static int system_status(sd_netlink *rtnl, sd_hwdb *hwdb) {
        _cleanup_free_ char *operational_state = NULL;
        _cleanup_strv_free_ char **dns = NULL, **ntp = NULL, **search_domains = NULL, **route_domains = NULL;
        const char *on_color_operational, *off_color_operational;
        _cleanup_(table_unrefp) Table *table = NULL;
        TableCell *cell;
        int r;

        assert(rtnl);

        (void) sd_network_get_operational_state(&operational_state);
        operational_state_to_color(operational_state, &on_color_operational, &off_color_operational);

        table = table_new("DOT", "KEY", "VALUE");
        if (!table)
                return -ENOMEM;

        assert_se(cell = table_get_cell(table, 0, 0));
        (void) table_set_ellipsize_percent(table, cell, 100);

        assert_se(cell = table_get_cell(table, 0, 1));
        (void) table_set_align_percent(table, cell, 100);
        (void) table_set_ellipsize_percent(table, cell, 100);

        table_set_header(table, false);

        r = table_add_cell(table, &cell, TABLE_STRING, special_glyph(SPECIAL_GLYPH_BLACK_CIRCLE));
        if (r < 0)
                return r;
        (void) table_set_color(table, cell, on_color_operational);

        r = table_add_cell(table, NULL, TABLE_STRING, "State:");
        if (r < 0)
                return r;

        r = table_add_cell(table, &cell, TABLE_STRING, strna(operational_state));
        if (r < 0)
                return r;
        (void) table_set_color(table, cell, on_color_operational);

        r = dump_addresses(rtnl, table, 0);
        if (r < 0)
                return r;
        r = dump_gateways(rtnl, hwdb, table, 0);
        if (r < 0)
                return r;

        (void) sd_network_get_dns(&dns);
        r = dump_list(table, "DNS:", dns);
        if (r < 0)
                return r;

        (void) sd_network_get_search_domains(&search_domains);
        r = dump_list(table, "Search Domains:", search_domains);
        if (r < 0)
                return r;

        (void) sd_network_get_route_domains(&route_domains);
        r = dump_list(table, "Route Domains:", route_domains);
        if (r < 0)
                return r;

        (void) sd_network_get_ntp(&ntp);
        r = dump_list(table, "NTP:", ntp);
        if (r < 0)
                return r;

        return table_print(table, NULL);
}

static int link_status(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        _cleanup_(sd_hwdb_unrefp) sd_hwdb *hwdb = NULL;
        _cleanup_free_ LinkInfo *links = NULL;
        int r, c, i;

        (void) pager_open(arg_pager_flags);

        r = sd_bus_open_system(&bus);
        if (r < 0)
                return log_error_errno(r, "Failed to connect system bus: %m");

        r = sd_netlink_open(&rtnl);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to netlink: %m");

        r = sd_hwdb_new(&hwdb);
        if (r < 0)
                log_debug_errno(r, "Failed to open hardware database: %m");

        if (arg_all)
                c = acquire_link_info(bus, rtnl, NULL, &links);
        else if (argc <= 1)
                return system_status(rtnl, hwdb);
        else
                c = acquire_link_info(bus, rtnl, argv + 1, &links);
        if (c < 0)
                return c;

        for (i = 0; i < c; i++) {
                if (i > 0)
                        fputc('\n', stdout);

                link_status_one(rtnl, hwdb, links + i);
        }

        return 0;
}

static char *lldp_capabilities_to_string(uint16_t x) {
        static const char characters[] = {
                'o', 'p', 'b', 'w', 'r', 't', 'd', 'a', 'c', 's', 'm',
        };
        char *ret;
        unsigned i;

        ret = new(char, ELEMENTSOF(characters) + 1);
        if (!ret)
                return NULL;

        for (i = 0; i < ELEMENTSOF(characters); i++)
                ret[i] = (x & (1U << i)) ? characters[i] : '.';

        ret[i] = 0;
        return ret;
}

static void lldp_capabilities_legend(uint16_t x) {
        unsigned w, i, cols = columns();
        static const char* const table[] = {
                "o - Other",
                "p - Repeater",
                "b - Bridge",
                "w - WLAN Access Point",
                "r - Router",
                "t - Telephone",
                "d - DOCSIS cable device",
                "a - Station",
                "c - Customer VLAN",
                "s - Service VLAN",
                "m - Two-port MAC Relay (TPMR)",
        };

        if (x == 0)
                return;

        printf("\nCapability Flags:\n");
        for (w = 0, i = 0; i < ELEMENTSOF(table); i++)
                if (x & (1U << i) || arg_all) {
                        bool newline;

                        newline = w + strlen(table[i]) + (w == 0 ? 0 : 2) > cols;
                        if (newline)
                                w = 0;
                        w += printf("%s%s%s", newline ? "\n" : "", w == 0 ? "" : "; ", table[i]);
                }
        puts("");
}

static int link_lldp_status(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        _cleanup_free_ LinkInfo *links = NULL;
        _cleanup_(table_unrefp) Table *table = NULL;
        int i, r, c, m = 0;
        uint16_t all = 0;
        TableCell *cell;

        r = sd_netlink_open(&rtnl);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to netlink: %m");

        c = acquire_link_info(NULL, rtnl, argc > 1 ? argv + 1 : NULL, &links);
        if (c < 0)
                return c;

        (void) pager_open(arg_pager_flags);

        table = table_new("LINK",
                          "CHASSIS ID",
                          "SYSTEM NAME",
                          "CAPS",
                          "PORT ID",
                          "PORT DESCRIPTION");
        if (!table)
                return -ENOMEM;

        table_set_header(table, arg_legend);

        assert_se(cell = table_get_cell(table, 0, 0));
        table_set_minimum_width(table, cell, 16);

        assert_se(cell = table_get_cell(table, 0, 1));
        table_set_minimum_width(table, cell, 17);

        assert_se(cell = table_get_cell(table, 0, 2));
        table_set_minimum_width(table, cell, 16);

        assert_se(cell = table_get_cell(table, 0, 3));
        table_set_minimum_width(table, cell, 11);

        assert_se(cell = table_get_cell(table, 0, 4));
        table_set_minimum_width(table, cell, 17);

        assert_se(cell = table_get_cell(table, 0, 5));
        table_set_minimum_width(table, cell, 16);

        for (i = 0; i < c; i++) {
                _cleanup_fclose_ FILE *f = NULL;

                r = open_lldp_neighbors(links[i].ifindex, &f);
                if (r == -ENOENT)
                        continue;
                if (r < 0) {
                        log_warning_errno(r, "Failed to open LLDP data for %i, ignoring: %m", links[i].ifindex);
                        continue;
                }

                for (;;) {
                        _cleanup_free_ char *cid = NULL, *pid = NULL, *sname = NULL, *pdesc = NULL, *capabilities = NULL;
                        const char *chassis_id = NULL, *port_id = NULL, *system_name = NULL, *port_description = NULL;
                        _cleanup_(sd_lldp_neighbor_unrefp) sd_lldp_neighbor *n = NULL;
                        uint16_t cc;

                        r = next_lldp_neighbor(f, &n);
                        if (r < 0) {
                                log_warning_errno(r, "Failed to read neighbor data: %m");
                                break;
                        }
                        if (r == 0)
                                break;

                        (void) sd_lldp_neighbor_get_chassis_id_as_string(n, &chassis_id);
                        (void) sd_lldp_neighbor_get_port_id_as_string(n, &port_id);
                        (void) sd_lldp_neighbor_get_system_name(n, &system_name);
                        (void) sd_lldp_neighbor_get_port_description(n, &port_description);

                        if (chassis_id) {
                                cid = ellipsize(chassis_id, 17, 100);
                                if (cid)
                                        chassis_id = cid;
                        }

                        if (port_id) {
                                pid = ellipsize(port_id, 17, 100);
                                if (pid)
                                        port_id = pid;
                        }

                        if (system_name) {
                                sname = ellipsize(system_name, 16, 100);
                                if (sname)
                                        system_name = sname;
                        }

                        if (port_description) {
                                pdesc = ellipsize(port_description, 16, 100);
                                if (pdesc)
                                        port_description = pdesc;
                        }

                        if (sd_lldp_neighbor_get_enabled_capabilities(n, &cc) >= 0) {
                                capabilities = lldp_capabilities_to_string(cc);
                                all |= cc;
                        }

                        r = table_add_many(table,
                                           TABLE_STRING, links[i].name,
                                           TABLE_STRING, strna(chassis_id),
                                           TABLE_STRING, strna(system_name),
                                           TABLE_STRING, strna(capabilities),
                                           TABLE_STRING, strna(port_id),
                                           TABLE_STRING, strna(port_description));
                        if (r < 0)
                                return r;

                        m++;
                }
        }

        r = table_print(table, NULL);
        if (r < 0)
                return r;

        if (arg_legend) {
                lldp_capabilities_legend(all);
                printf("\n%i neighbors listed.\n", m);
        }

        return 0;
}

static int link_delete_send_message(sd_netlink *rtnl, int index) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL;
        int r;

        assert(rtnl);

        r = sd_rtnl_message_new_link(rtnl, &req, RTM_DELLINK, index);
        if (r < 0)
                return rtnl_log_create_error(r);

        r = sd_netlink_call(rtnl, req, 0, NULL);
        if (r < 0)
                return r;

        return 0;
}

static int link_delete(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        _cleanup_set_free_ Set *indexes = NULL;
        int index, r, i;
        Iterator j;
        void *p;

        r = sd_netlink_open(&rtnl);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to netlink: %m");

        indexes = set_new(NULL);
        if (!indexes)
                return log_oom();

        for (i = 1; i < argc; i++) {
                r = parse_ifindex_or_ifname(argv[i], &index);
                if (r < 0)
                        return log_error_errno(r, "Failed to resolve interface %s", argv[i]);

                r = set_put(indexes, INT_TO_PTR(index));
                if (r < 0)
                        return log_oom();
        }

        SET_FOREACH(p, indexes, j) {
                r = link_delete_send_message(rtnl, PTR_TO_INT(p));
                if (r < 0) {
                        char ifname[IF_NAMESIZE + 1];

                        if (format_ifname(index, ifname))
                                return log_error_errno(r, "Failed to delete interface %s: %m", ifname);
                        else
                                return log_error_errno(r, "Failed to delete interface %d: %m", index);
                }
        }

        return r;
}

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("networkctl", "1", &link);
        if (r < 0)
                return log_oom();

        printf("%s [OPTIONS...]\n\n"
               "Query and control the networking subsystem.\n\n"
               "  -h --help             Show this help\n"
               "     --version          Show package version\n"
               "     --no-pager         Do not pipe output into a pager\n"
               "     --no-legend        Do not show the headers and footers\n"
               "  -a --all              Show status for all links\n"
               "  -s --stats            Show detailed link statics\n"
               "\nCommands:\n"
               "  list [PATTERN...]     List links\n"
               "  status [PATTERN...]   Show link status\n"
               "  lldp [PATTERN...]     Show LLDP neighbors\n"
               "  label                 Show current address label entries in the kernel\n"
               "  delete DEVICES        Delete virtual netdevs\n"
               "\nSee the %s for details.\n"
               , program_invocation_short_name
               , link
        );

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_NO_PAGER,
                ARG_NO_LEGEND,
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, 'h'           },
                { "version",   no_argument,       NULL, ARG_VERSION   },
                { "no-pager",  no_argument,       NULL, ARG_NO_PAGER  },
                { "no-legend", no_argument,       NULL, ARG_NO_LEGEND },
                { "all",       no_argument,       NULL, 'a'           },
                { "stats",     no_argument,       NULL, 's'           },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "has", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case ARG_NO_PAGER:
                        arg_pager_flags |= PAGER_DISABLE;
                        break;

                case ARG_NO_LEGEND:
                        arg_legend = false;
                        break;

                case 'a':
                        arg_all = true;
                        break;

                case 's':
                        arg_stats = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }
        }

        return 1;
}

static int networkctl_main(int argc, char *argv[]) {
        static const Verb verbs[] = {
                { "list",   VERB_ANY, VERB_ANY, VERB_DEFAULT, list_links          },
                { "status", VERB_ANY, VERB_ANY, 0,            link_status         },
                { "lldp",   VERB_ANY, VERB_ANY, 0,            link_lldp_status    },
                { "label",  VERB_ANY, VERB_ANY, 0,            list_address_labels },
                { "delete", 2,        VERB_ANY, 0,            link_delete         },
                {}
        };

        return dispatch_verb(argc, argv, verbs, NULL);
}

static void warn_networkd_missing(void) {

        if (access("/run/systemd/netif/state", F_OK) >= 0)
                return;

        fprintf(stderr, "WARNING: systemd-networkd is not running, output will be incomplete.\n\n");
}

static int run(int argc, char* argv[]) {
        int r;

        log_show_color(true);
        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        warn_networkd_missing();

        return networkctl_main(argc, argv);
}

DEFINE_MAIN_FUNCTION(run);
