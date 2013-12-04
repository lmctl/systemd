/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

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

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <fcntl.h>
#include <malloc.h>
#include <sys/mman.h>

#include "util.h"

#include "bus-internal.h"
#include "bus-message.h"
#include "bus-kernel.h"
#include "bus-bloom.h"
#include "bus-util.h"

#define UNIQUE_NAME_MAX (3+DECIMAL_STR_MAX(uint64_t))

int bus_kernel_parse_unique_name(const char *s, uint64_t *id) {
        int r;

        assert(s);
        assert(id);

        if (!startswith(s, ":1."))
                return 0;

        r = safe_atou64(s + 3, id);
        if (r < 0)
                return r;

        return 1;
}

static void append_payload_vec(struct kdbus_item **d, const void *p, size_t sz) {
        assert(d);
        assert(sz > 0);

        *d = ALIGN8_PTR(*d);

        /* Note that p can be NULL, which encodes a region full of
         * zeroes, which is useful to optimize certain padding
         * conditions */

        (*d)->size = offsetof(struct kdbus_item, vec) + sizeof(struct kdbus_vec);
        (*d)->type = KDBUS_ITEM_PAYLOAD_VEC;
        (*d)->vec.address = PTR_TO_UINT64(p);
        (*d)->vec.size = sz;

        *d = (struct kdbus_item *) ((uint8_t*) *d + (*d)->size);
}

static void append_payload_memfd(struct kdbus_item **d, int memfd, size_t sz) {
        assert(d);
        assert(memfd >= 0);
        assert(sz > 0);

        *d = ALIGN8_PTR(*d);
        (*d)->size = offsetof(struct kdbus_item, memfd) + sizeof(struct kdbus_memfd);
        (*d)->type = KDBUS_ITEM_PAYLOAD_MEMFD;
        (*d)->memfd.fd = memfd;
        (*d)->memfd.size = sz;

        *d = (struct kdbus_item *) ((uint8_t*) *d + (*d)->size);
}

static void append_destination(struct kdbus_item **d, const char *s, size_t length) {
        assert(d);
        assert(s);

        *d = ALIGN8_PTR(*d);

        (*d)->size = offsetof(struct kdbus_item, str) + length + 1;
        (*d)->type = KDBUS_ITEM_DST_NAME;
        memcpy((*d)->str, s, length + 1);

        *d = (struct kdbus_item *) ((uint8_t*) *d + (*d)->size);
}

static void* append_bloom(struct kdbus_item **d, size_t length) {
        void *r;

        assert(d);

        *d = ALIGN8_PTR(*d);

        (*d)->size = offsetof(struct kdbus_item, data) + length;
        (*d)->type = KDBUS_ITEM_BLOOM;
        r = (*d)->data;

        *d = (struct kdbus_item *) ((uint8_t*) *d + (*d)->size);

        return r;
}

static void append_fds(struct kdbus_item **d, const int fds[], unsigned n_fds) {
        assert(d);
        assert(fds);
        assert(n_fds > 0);

        *d = ALIGN8_PTR(*d);
        (*d)->size = offsetof(struct kdbus_item, fds) + sizeof(int) * n_fds;
        (*d)->type = KDBUS_ITEM_FDS;
        memcpy((*d)->fds, fds, sizeof(int) * n_fds);

        *d = (struct kdbus_item *) ((uint8_t*) *d + (*d)->size);
}

static int bus_message_setup_bloom(sd_bus_message *m, void *bloom) {
        unsigned i;
        int r;

        assert(m);
        assert(bloom);

        memset(bloom, 0, BLOOM_SIZE);

        bloom_add_pair(bloom, "message-type", bus_message_type_to_string(m->header->type));

        if (m->interface)
                bloom_add_pair(bloom, "interface", m->interface);
        if (m->member)
                bloom_add_pair(bloom, "member", m->member);
        if (m->path) {
                bloom_add_pair(bloom, "path", m->path);
                bloom_add_pair(bloom, "path-slash-prefix", m->path);
                bloom_add_prefixes(bloom, "path-slash-prefix", m->path, '/');
        }

        r = sd_bus_message_rewind(m, true);
        if (r < 0)
                return r;

        for (i = 0; i < 64; i++) {
                char type;
                const char *t;
                char buf[sizeof("arg")-1 + 2 + sizeof("-slash-prefix")];
                char *e;

                r = sd_bus_message_peek_type(m, &type, NULL);
                if (r < 0)
                        return r;

                if (type != SD_BUS_TYPE_STRING &&
                    type != SD_BUS_TYPE_OBJECT_PATH &&
                    type != SD_BUS_TYPE_SIGNATURE)
                        break;

                r = sd_bus_message_read_basic(m, type, &t);
                if (r < 0)
                        return r;

                e = stpcpy(buf, "arg");
                if (i < 10)
                        *(e++) = '0' + i;
                else {
                        *(e++) = '0' + (i / 10);
                        *(e++) = '0' + (i % 10);
                }

                *e = 0;
                bloom_add_pair(bloom, buf, t);

                strcpy(e, "-dot-prefix");
                bloom_add_prefixes(bloom, buf, t, '.');
                strcpy(e, "-slash-prefix");
                bloom_add_prefixes(bloom, buf, t, '/');
        }

        return 0;
}

static int bus_message_setup_kmsg(sd_bus *b, sd_bus_message *m) {
        struct bus_body_part *part;
        struct kdbus_item *d;
        bool well_known;
        uint64_t unique;
        size_t sz, dl;
        unsigned i;
        int r;

        assert(b);
        assert(m);
        assert(m->sealed);

        if (m->kdbus)
                return 0;

        if (m->destination) {
                r = bus_kernel_parse_unique_name(m->destination, &unique);
                if (r < 0)
                        return r;

                well_known = r == 0;
        } else
                well_known = false;

        sz = offsetof(struct kdbus_msg, items);

        assert_cc(ALIGN8(offsetof(struct kdbus_item, vec) + sizeof(struct kdbus_vec)) ==
                  ALIGN8(offsetof(struct kdbus_item, memfd) + sizeof(struct kdbus_memfd)));

        /* Add in fixed header, fields header and payload */
        sz += (1 + m->n_body_parts) *
                ALIGN8(offsetof(struct kdbus_item, vec) + sizeof(struct kdbus_vec));

        /* Add space for bloom filter */
        sz += ALIGN8(offsetof(struct kdbus_item, data) + BLOOM_SIZE);

        /* Add in well-known destination header */
        if (well_known) {
                dl = strlen(m->destination);
                sz += ALIGN8(offsetof(struct kdbus_item, str) + dl + 1);
        }

        /* Add space for unix fds */
        if (m->n_fds > 0)
                sz += ALIGN8(offsetof(struct kdbus_item, fds) + sizeof(int)*m->n_fds);

        m->kdbus = memalign(8, sz);
        if (!m->kdbus) {
                r = -ENOMEM;
                goto fail;
        }

        m->free_kdbus = true;
        memset(m->kdbus, 0, sz);

        m->kdbus->flags =
                ((m->header->flags & BUS_MESSAGE_NO_REPLY_EXPECTED) ? 0 : KDBUS_MSG_FLAGS_EXPECT_REPLY) |
                ((m->header->flags & BUS_MESSAGE_NO_AUTO_START) ? KDBUS_MSG_FLAGS_NO_AUTO_START : 0);
        m->kdbus->dst_id =
                well_known ? 0 :
                m->destination ? unique : KDBUS_DST_ID_BROADCAST;
        m->kdbus->payload_type = KDBUS_PAYLOAD_DBUS1;
        m->kdbus->cookie = m->header->serial;

        m->kdbus->timeout_ns = m->timeout * NSEC_PER_USEC;

        d = m->kdbus->items;

        if (well_known)
                append_destination(&d, m->destination, dl);

        append_payload_vec(&d, m->header, BUS_MESSAGE_BODY_BEGIN(m));

        MESSAGE_FOREACH_PART(part, i, m) {
                if (part->is_zero) {
                        /* If this is padding then simply send a
                         * vector with a NULL data pointer which the
                         * kernel will just pass through. This is the
                         * most efficient way to encode zeroes */

                        append_payload_vec(&d, NULL, part->size);
                        continue;
                }

                if (part->memfd >= 0 && part->sealed && m->destination) {
                        /* Try to send a memfd, if the part is
                         * sealed and this is not a broadcast. Since we can only  */

                        append_payload_memfd(&d, part->memfd, part->size);
                        continue;
                }

                /* Otherwise let's send a vector to the actual data,
                 * for that we need to map it first. */
                r = bus_body_part_map(part);
                if (r < 0)
                        goto fail;

                append_payload_vec(&d, part->data, part->size);
        }

        if (m->kdbus->dst_id == KDBUS_DST_ID_BROADCAST) {
                void *p;

                p = append_bloom(&d, BLOOM_SIZE);
                r = bus_message_setup_bloom(m, p);
                if (r < 0)
                        goto fail;
        }

        if (m->n_fds > 0)
                append_fds(&d, m->fds, m->n_fds);

        m->kdbus->size = (uint8_t*) d - (uint8_t*) m->kdbus;
        assert(m->kdbus->size <= sz);

        return 0;

fail:
        m->poisoned = true;
        return r;
}

int bus_kernel_take_fd(sd_bus *b) {
        struct kdbus_cmd_hello hello;
        int r;

        assert(b);

        if (b->is_server)
                return -EINVAL;

        b->use_memfd = 1;

        zero(hello);
        hello.size = sizeof(hello);
        hello.conn_flags = b->hello_flags;
        hello.attach_flags = b->attach_flags;
        hello.pool_size = KDBUS_POOL_SIZE;

        r = ioctl(b->input_fd, KDBUS_CMD_HELLO, &hello);
        if (r < 0)
                return -errno;

        if (!b->kdbus_buffer) {
                b->kdbus_buffer = mmap(NULL, KDBUS_POOL_SIZE, PROT_READ, MAP_SHARED, b->input_fd, 0);
                if (b->kdbus_buffer == MAP_FAILED) {
                        b->kdbus_buffer = NULL;
                        return -errno;
                }
        }

        /* The higher 32bit of both flags fields are considered
         * 'incompatible flags'. Refuse them all for now. */
        if (hello.bus_flags > 0xFFFFFFFFULL ||
            hello.conn_flags > 0xFFFFFFFFULL)
                return -ENOTSUP;

        if (hello.bloom_size != BLOOM_SIZE)
                return -ENOTSUP;

        if (asprintf(&b->unique_name, ":1.%llu", (unsigned long long) hello.id) < 0)
                return -ENOMEM;

        b->unique_id = hello.id;

        b->is_kernel = true;
        b->bus_client = true;
        b->can_fds = !!(hello.conn_flags & KDBUS_HELLO_ACCEPT_FD);

        /* the kernel told us the UUID of the underlying bus */
        memcpy(b->server_id.bytes, hello.id128, sizeof(b->server_id.bytes));

        return bus_start_running(b);
}

int bus_kernel_connect(sd_bus *b) {
        assert(b);
        assert(b->input_fd < 0);
        assert(b->output_fd < 0);
        assert(b->kernel);

        if (b->is_server)
                return -EINVAL;

        b->input_fd = open(b->kernel, O_RDWR|O_NOCTTY|O_CLOEXEC);
        if (b->input_fd < 0)
                return -errno;

        b->output_fd = b->input_fd;

        return bus_kernel_take_fd(b);
}

int bus_kernel_write_message(sd_bus *bus, sd_bus_message *m) {
        int r;

        assert(bus);
        assert(m);
        assert(bus->state == BUS_RUNNING);

        /* If we can't deliver, we want room for the error message */
        r = bus_rqueue_make_room(bus);
        if (r < 0)
                return r;

        r = bus_message_setup_kmsg(bus, m);
        if (r < 0)
                return r;

        r = ioctl(bus->output_fd, KDBUS_CMD_MSG_SEND, m->kdbus);
        if (r < 0) {
                _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
                sd_bus_message *reply;

                if (errno == EAGAIN || errno == EINTR)
                        return 0;
                else if (errno == ENXIO || errno == ESRCH) {

                        /* ENXIO: unique name not known
                         * ESRCH: well-known name not known */

                        if (m->header->type == SD_BUS_MESSAGE_METHOD_CALL)
                                sd_bus_error_setf(&error, SD_BUS_ERROR_SERVICE_UNKNOWN, "Destination %s not known", m->destination);
                        else
                                return 0;

                } else if (errno == EADDRNOTAVAIL) {

                        /* EADDRNOTAVAIL: activation is possible, but turned off in request flags */

                        if (m->header->type == SD_BUS_MESSAGE_METHOD_CALL)
                                sd_bus_error_setf(&error, SD_BUS_ERROR_SERVICE_UNKNOWN, "Activation of %s not requested", m->destination);
                        else
                                return 0;
                } else
                        return -errno;

                r = bus_message_new_synthetic_error(
                                bus,
                                BUS_MESSAGE_SERIAL(m),
                                &error,
                                &reply);

                if (r < 0)
                        return r;

                r = bus_seal_synthetic_message(bus, reply);
                if (r < 0)
                        return r;

                bus->rqueue[bus->rqueue_size++] = reply;

                return 0;
        }

        return 1;
}

static void close_kdbus_msg(sd_bus *bus, struct kdbus_msg *k) {
        uint64_t off;
        struct kdbus_item *d;

        assert(bus);
        assert(k);

        off = (uint8_t *)k - (uint8_t *)bus->kdbus_buffer;
        ioctl(bus->input_fd, KDBUS_CMD_FREE, &off);

        KDBUS_PART_FOREACH(d, k, items) {

                if (d->type == KDBUS_ITEM_FDS)
                        close_many(d->fds, (d->size - offsetof(struct kdbus_item, fds)) / sizeof(int));
                else if (d->type == KDBUS_ITEM_PAYLOAD_MEMFD)
                        close_nointr_nofail(d->memfd.fd);
        }
}

static int push_name_owner_changed(sd_bus *bus, const char *name, const char *old_owner, const char *new_owner) {
        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        int r;

        assert(bus);

        r = sd_bus_message_new_signal(
                        bus,
                        "/org/freedesktop/DBus",
                        "org.freedesktop.DBus",
                        "NameOwnerChanged",
                        &m);
        if (r < 0)
                return r;

        r = sd_bus_message_append(m, "sss", name, old_owner, new_owner);
        if (r < 0)
                return r;

        m->sender = "org.freedesktop.DBus";

        r = bus_seal_synthetic_message(bus, m);
        if (r < 0)
                return r;

        bus->rqueue[bus->rqueue_size++] = m;
        m = NULL;

        return 1;
}

static int translate_name_change(sd_bus *bus, struct kdbus_msg *k, struct kdbus_item *d) {
        char new_owner[UNIQUE_NAME_MAX], old_owner[UNIQUE_NAME_MAX];

        assert(bus);
        assert(k);
        assert(d);

        if (d->name_change.flags != 0)
                return 0;

        if (d->type == KDBUS_ITEM_NAME_ADD)
                old_owner[0] = 0;
        else
                sprintf(old_owner, ":1.%llu", (unsigned long long) d->name_change.old_id);

        if (d->type == KDBUS_ITEM_NAME_REMOVE)
                new_owner[0] = 0;
        else
                sprintf(new_owner, ":1.%llu", (unsigned long long) d->name_change.new_id);

        return push_name_owner_changed(bus, d->name_change.name, old_owner, new_owner);
}

static int translate_id_change(sd_bus *bus, struct kdbus_msg *k, struct kdbus_item *d) {
        char owner[UNIQUE_NAME_MAX];

        assert(bus);
        assert(k);
        assert(d);

        sprintf(owner, ":1.%llu", d->id_change.id);

        return push_name_owner_changed(
                        bus, owner,
                        d->type == KDBUS_ITEM_ID_ADD ? NULL : owner,
                        d->type == KDBUS_ITEM_ID_ADD ? owner : NULL);
}

static int translate_reply(sd_bus *bus, struct kdbus_msg *k, struct kdbus_item *d) {
        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        int r;

        assert(bus);
        assert(k);
        assert(d);

        r = bus_message_new_synthetic_error(
                        bus,
                        k->cookie_reply,
                        d->type == KDBUS_ITEM_REPLY_TIMEOUT ?
                        &SD_BUS_ERROR_MAKE_CONST(SD_BUS_ERROR_NO_REPLY, "Method call timed out") :
                        &SD_BUS_ERROR_MAKE_CONST(SD_BUS_ERROR_NO_REPLY, "Method call peer died"),
                        &m);
        if (r < 0)
                return r;

        m->sender = "org.freedesktop.DBus";

        r = bus_seal_synthetic_message(bus, m);
        if (r < 0)
                return r;

        bus->rqueue[bus->rqueue_size++] = m;
        m = NULL;

        return 1;
}

static int bus_kernel_translate_message(sd_bus *bus, struct kdbus_msg *k) {
        struct kdbus_item *d, *found = NULL;

        static int (* const translate[])(sd_bus *bus, struct kdbus_msg *k, struct kdbus_item *d) = {
                [KDBUS_ITEM_NAME_ADD - _KDBUS_ITEM_KERNEL_BASE] = translate_name_change,
                [KDBUS_ITEM_NAME_REMOVE - _KDBUS_ITEM_KERNEL_BASE] = translate_name_change,
                [KDBUS_ITEM_NAME_CHANGE - _KDBUS_ITEM_KERNEL_BASE] = translate_name_change,

                [KDBUS_ITEM_ID_ADD - _KDBUS_ITEM_KERNEL_BASE] = translate_id_change,
                [KDBUS_ITEM_ID_REMOVE - _KDBUS_ITEM_KERNEL_BASE] = translate_id_change,

                [KDBUS_ITEM_REPLY_TIMEOUT - _KDBUS_ITEM_KERNEL_BASE] = translate_reply,
                [KDBUS_ITEM_REPLY_DEAD - _KDBUS_ITEM_KERNEL_BASE] = translate_reply,
        };

        assert(bus);
        assert(k);
        assert(k->payload_type == KDBUS_PAYLOAD_KERNEL);

        KDBUS_PART_FOREACH(d, k, items) {
                if (d->type >= _KDBUS_ITEM_KERNEL_BASE && d->type < _KDBUS_ITEM_KERNEL_BASE + ELEMENTSOF(translate)) {
                        if (found)
                                return -EBADMSG;
                        found = d;
                } else
                        log_debug("Got unknown field from kernel %llu", d->type);
        }

        if (!found) {
                log_debug("Didn't find a kernel message to translate.");
                return 0;
        }

        return translate[found->type - _KDBUS_ITEM_KERNEL_BASE](bus, k, found);
}

static int bus_kernel_make_message(sd_bus *bus, struct kdbus_msg *k) {
        sd_bus_message *m = NULL;
        struct kdbus_item *d;
        unsigned n_fds = 0;
        _cleanup_free_ int *fds = NULL;
        struct bus_header *h = NULL;
        size_t total, n_bytes = 0, idx = 0;
        const char *destination = NULL, *seclabel = NULL;
        int r;

        assert(bus);
        assert(k);
        assert(k->payload_type == KDBUS_PAYLOAD_DBUS1);

        KDBUS_PART_FOREACH(d, k, items) {
                size_t l;

                l = d->size - offsetof(struct kdbus_item, data);

                switch (d->type) {

                case KDBUS_ITEM_PAYLOAD_OFF:
                        if (!h) {
                                h = (struct bus_header *)((uint8_t *)bus->kdbus_buffer + d->vec.offset);

                                if (!bus_header_is_complete(h, d->vec.size))
                                        return -EBADMSG;
                        }

                        n_bytes += d->vec.size;
                        break;

                case KDBUS_ITEM_PAYLOAD_MEMFD:
                        if (!h)
                                return -EBADMSG;

                        n_bytes += d->memfd.size;
                        break;

                case KDBUS_ITEM_FDS: {
                        int *f;
                        unsigned j;

                        j = l / sizeof(int);
                        f = realloc(fds, sizeof(int) * (n_fds + j));
                        if (!f)
                                return -ENOMEM;

                        fds = f;
                        memcpy(fds + n_fds, d->fds, sizeof(int) * j);
                        n_fds += j;
                        break;
                }

                case KDBUS_ITEM_SECLABEL:
                        seclabel = d->str;
                        break;
                }
        }

        if (!h)
                return -EBADMSG;

        r = bus_header_message_size(h, &total);
        if (r < 0)
                return r;

        if (n_bytes != total)
                return -EBADMSG;

        r = bus_message_from_header(bus, h, sizeof(struct bus_header), fds, n_fds, NULL, seclabel, 0, &m);
        if (r < 0)
                return r;

        KDBUS_PART_FOREACH(d, k, items) {
                size_t l;

                l = d->size - offsetof(struct kdbus_item, data);

                switch (d->type) {

                case KDBUS_ITEM_PAYLOAD_OFF: {
                        size_t begin_body;

                        begin_body = BUS_MESSAGE_BODY_BEGIN(m);

                        if (idx + d->vec.size > begin_body) {
                                struct bus_body_part *part;

                                /* Contains body material */

                                part = message_append_part(m);
                                if (!part) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                /* A -1 offset is NUL padding. */
                                part->is_zero = d->vec.offset == ~0ULL;

                                if (idx >= begin_body) {
                                        if (!part->is_zero)
                                                part->data = (uint8_t *)bus->kdbus_buffer + d->vec.offset;
                                        part->size = d->vec.size;
                                } else {
                                        if (!part->is_zero)
                                                part->data = (uint8_t *)bus->kdbus_buffer + d->vec.offset + (begin_body - idx);
                                        part->size = d->vec.size - (begin_body - idx);
                                }

                                part->sealed = true;
                        }

                        idx += d->vec.size;
                        break;
                }

                case KDBUS_ITEM_PAYLOAD_MEMFD: {
                        struct bus_body_part *part;

                        if (idx < BUS_MESSAGE_BODY_BEGIN(m)) {
                                r = -EBADMSG;
                                goto fail;
                        }

                        part = message_append_part(m);
                        if (!part) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        part->memfd = d->memfd.fd;
                        part->size = d->memfd.size;
                        part->sealed = true;

                        idx += d->memfd.size;
                        break;
                }

                case KDBUS_ITEM_CREDS:
                        m->creds.pid_starttime = d->creds.starttime / NSEC_PER_USEC;
                        m->creds.uid = d->creds.uid;
                        m->creds.gid = d->creds.gid;
                        m->creds.pid = d->creds.pid;
                        m->creds.tid = d->creds.tid;
                        m->creds.mask |= (SD_BUS_CREDS_UID|SD_BUS_CREDS_GID|SD_BUS_CREDS_PID|SD_BUS_CREDS_PID_STARTTIME|SD_BUS_CREDS_TID) & bus->creds_mask;
                        break;

                case KDBUS_ITEM_TIMESTAMP:
                        m->realtime = d->timestamp.realtime_ns / NSEC_PER_USEC;
                        m->monotonic = d->timestamp.monotonic_ns / NSEC_PER_USEC;
                        break;

                case KDBUS_ITEM_PID_COMM:
                        m->creds.comm = d->str;
                        m->creds.mask |= SD_BUS_CREDS_COMM & bus->creds_mask;
                        break;

                case KDBUS_ITEM_TID_COMM:
                        m->creds.tid_comm = d->str;
                        m->creds.mask |= SD_BUS_CREDS_TID_COMM & bus->creds_mask;
                        break;

                case KDBUS_ITEM_EXE:
                        m->creds.exe = d->str;
                        m->creds.mask |= SD_BUS_CREDS_EXE & bus->creds_mask;
                        break;

                case KDBUS_ITEM_CMDLINE:
                        m->creds.cmdline = d->str;
                        m->creds.cmdline_size = l;
                        m->creds.mask |= SD_BUS_CREDS_CMDLINE & bus->creds_mask;
                        break;

                case KDBUS_ITEM_CGROUP:
                        m->creds.cgroup = d->str;
                        m->creds.mask |= (SD_BUS_CREDS_CGROUP|SD_BUS_CREDS_UNIT|SD_BUS_CREDS_USER_UNIT|SD_BUS_CREDS_SLICE|SD_BUS_CREDS_SESSION|SD_BUS_CREDS_OWNER_UID) & bus->creds_mask;
                        break;

                case KDBUS_ITEM_AUDIT:
                        m->creds.audit_session_id = d->audit.sessionid;
                        m->creds.audit_login_uid = d->audit.loginuid;
                        m->creds.mask |= (SD_BUS_CREDS_AUDIT_SESSION_ID|SD_BUS_CREDS_AUDIT_LOGIN_UID) & bus->creds_mask;
                        break;

                case KDBUS_ITEM_CAPS:
                        m->creds.capability = d->data;
                        m->creds.capability_size = l;
                        m->creds.mask |= (SD_BUS_CREDS_EFFECTIVE_CAPS|SD_BUS_CREDS_PERMITTED_CAPS|SD_BUS_CREDS_INHERITABLE_CAPS|SD_BUS_CREDS_BOUNDING_CAPS) & bus->creds_mask;
                        break;

                case KDBUS_ITEM_DST_NAME:
                        destination = d->str;
                        break;

                case KDBUS_ITEM_NAMES:
                        m->creds.well_known_names = d->str;
                        m->creds.well_known_names_size = l;
                        m->creds.mask |= SD_BUS_CREDS_WELL_KNOWN_NAMES & bus->creds_mask;
                        break;

                case KDBUS_ITEM_FDS:
                case KDBUS_ITEM_SECLABEL:
                        break;

                default:
                        log_debug("Got unknown field from kernel %llu", d->type);
                }
        }

        r = bus_message_parse_fields(m);
        if (r < 0)
                goto fail;

        if (k->src_id == KDBUS_SRC_ID_KERNEL)
                m->sender = "org.freedesktop.DBus";
        else {
                snprintf(m->sender_buffer, sizeof(m->sender_buffer), ":1.%llu", (unsigned long long) k->src_id);
                m->sender = m->creds.unique_name = m->sender_buffer;
                m->creds.mask |= SD_BUS_CREDS_UNIQUE_NAME & bus->creds_mask;
        }

        if (!m->destination) {
                if (destination)
                        m->destination = destination;
                else if (k->dst_id != KDBUS_DST_ID_NAME &&
                         k->dst_id != KDBUS_DST_ID_BROADCAST) {
                        snprintf(m->destination_buffer, sizeof(m->destination_buffer), ":1.%llu", (unsigned long long) k->dst_id);
                        m->destination = m->destination_buffer;
                }
        }

        /* We take possession of the kmsg struct now */
        m->kdbus = k;
        m->release_kdbus = true;
        m->free_fds = true;
        fds = NULL;

        bus->rqueue[bus->rqueue_size++] = m;

        return 1;

fail:
        if (m) {
                struct bus_body_part *part;
                unsigned i;

                /* Make sure the memfds are not freed twice */
                MESSAGE_FOREACH_PART(part, i, m)
                        if (part->memfd >= 0)
                                part->memfd = -1;

                sd_bus_message_unref(m);
        }

        return r;
}

int bus_kernel_read_message(sd_bus *bus) {
        struct kdbus_msg *k;
        uint64_t off;
        int r;

        assert(bus);

        r = bus_rqueue_make_room(bus);
        if (r < 0)
                return r;

        r = ioctl(bus->input_fd, KDBUS_CMD_MSG_RECV, &off);
        if (r < 0) {
                if (errno == EAGAIN)
                        return 0;

                return -errno;
        }
        k = (struct kdbus_msg *)((uint8_t *)bus->kdbus_buffer + off);

        if (k->payload_type == KDBUS_PAYLOAD_DBUS1)
                r = bus_kernel_make_message(bus, k);
        else if (k->payload_type == KDBUS_PAYLOAD_KERNEL)
                r = bus_kernel_translate_message(bus, k);
        else
                r = 0;

        if (r <= 0)
                close_kdbus_msg(bus, k);

        return r < 0 ? r : 1;
}

int bus_kernel_pop_memfd(sd_bus *bus, void **address, size_t *size) {
        struct memfd_cache *c;
        int fd;

        assert(address);
        assert(size);

        if (!bus || !bus->is_kernel)
                return -ENOTSUP;

        assert_se(pthread_mutex_lock(&bus->memfd_cache_mutex) >= 0);

        if (bus->n_memfd_cache <= 0) {
                int r;

                assert_se(pthread_mutex_unlock(&bus->memfd_cache_mutex) >= 0);

                r = ioctl(bus->input_fd, KDBUS_CMD_MEMFD_NEW, &fd);
                if (r < 0)
                        return -errno;

                *address = NULL;
                *size = 0;
                return fd;
        }

        c = &bus->memfd_cache[--bus->n_memfd_cache];

        assert(c->fd >= 0);
        assert(c->size == 0 || c->address);

        *address = c->address;
        *size = c->size;
        fd = c->fd;

        assert_se(pthread_mutex_unlock(&bus->memfd_cache_mutex) >= 0);

        return fd;
}

static void close_and_munmap(int fd, void *address, size_t size) {
        if (size > 0)
                assert_se(munmap(address, PAGE_ALIGN(size)) >= 0);

        close_nointr_nofail(fd);
}

void bus_kernel_push_memfd(sd_bus *bus, int fd, void *address, size_t size) {
        struct memfd_cache *c;
        uint64_t max_sz = PAGE_ALIGN(MEMFD_CACHE_ITEM_SIZE_MAX);

        assert(fd >= 0);
        assert(size == 0 || address);

        if (!bus || !bus->is_kernel) {
                close_and_munmap(fd, address, size);
                return;
        }

        assert_se(pthread_mutex_lock(&bus->memfd_cache_mutex) >= 0);

        if (bus->n_memfd_cache >= ELEMENTSOF(bus->memfd_cache)) {
                assert_se(pthread_mutex_unlock(&bus->memfd_cache_mutex) >= 0);

                close_and_munmap(fd, address, size);
                return;
        }

        c = &bus->memfd_cache[bus->n_memfd_cache++];
        c->fd = fd;
        c->address = address;

        /* If overly long, let's return a bit to the OS */
        if (size > max_sz) {
                assert_se(ioctl(fd, KDBUS_CMD_MEMFD_SIZE_SET, &max_sz) >= 0);
                assert_se(munmap((uint8_t*) address + max_sz, PAGE_ALIGN(size - max_sz)) >= 0);
                c->size = max_sz;
        } else
                c->size = size;

        assert_se(pthread_mutex_unlock(&bus->memfd_cache_mutex) >= 0);
}

void bus_kernel_flush_memfd(sd_bus *b) {
        unsigned i;

        assert(b);

        for (i = 0; i < b->n_memfd_cache; i++)
                close_and_munmap(b->memfd_cache[i].fd, b->memfd_cache[i].address, b->memfd_cache[i].size);
}

int kdbus_translate_request_name_flags(uint64_t flags, uint64_t *kdbus_flags) {
        uint64_t f = 0;

        assert(kdbus_flags);

        if (flags & SD_BUS_NAME_ALLOW_REPLACEMENT)
                f |= KDBUS_NAME_ALLOW_REPLACEMENT;

        if (flags & SD_BUS_NAME_REPLACE_EXISTING)
                f |= KDBUS_NAME_REPLACE_EXISTING;

        if (!(flags & SD_BUS_NAME_DO_NOT_QUEUE))
                f |= KDBUS_NAME_QUEUE;

        *kdbus_flags = f;
        return 0;
}

int kdbus_translate_attach_flags(uint64_t mask, uint64_t *kdbus_mask) {
        uint64_t m = 0;

        assert(kdbus_mask);

        if (mask & (SD_BUS_CREDS_UID|SD_BUS_CREDS_GID|SD_BUS_CREDS_PID|SD_BUS_CREDS_PID_STARTTIME|SD_BUS_CREDS_TID))
                m |= KDBUS_ATTACH_CREDS;

        if (mask & (SD_BUS_CREDS_COMM|SD_BUS_CREDS_TID_COMM))
                m |= KDBUS_ATTACH_COMM;

        if (mask & SD_BUS_CREDS_EXE)
                m |= KDBUS_ATTACH_EXE;

        if (mask & SD_BUS_CREDS_CMDLINE)
                m |= KDBUS_ATTACH_CMDLINE;

        if (mask & (SD_BUS_CREDS_CGROUP|SD_BUS_CREDS_UNIT|SD_BUS_CREDS_USER_UNIT|SD_BUS_CREDS_SLICE|SD_BUS_CREDS_SESSION|SD_BUS_CREDS_OWNER_UID))
                m |= KDBUS_ATTACH_CGROUP;

        if (mask & (SD_BUS_CREDS_EFFECTIVE_CAPS|SD_BUS_CREDS_PERMITTED_CAPS|SD_BUS_CREDS_INHERITABLE_CAPS|SD_BUS_CREDS_BOUNDING_CAPS))
                m |= KDBUS_ATTACH_CAPS;

        if (mask & SD_BUS_CREDS_SELINUX_CONTEXT)
                m |= KDBUS_ATTACH_SECLABEL;

        if (mask & (SD_BUS_CREDS_AUDIT_SESSION_ID|SD_BUS_CREDS_AUDIT_LOGIN_UID))
                m |= KDBUS_ATTACH_AUDIT;

        if (mask & SD_BUS_CREDS_WELL_KNOWN_NAMES)
                m |= KDBUS_ATTACH_NAMES;

        *kdbus_mask = m;
        return 0;
}

int bus_kernel_create_bus(const char *name, char **s) {
        struct kdbus_cmd_bus_make *make;
        struct kdbus_item *n;
        int fd;

        assert(name);
        assert(s);

        fd = open("/dev/kdbus/control", O_RDWR|O_NOCTTY|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        make = alloca0(ALIGN8(offsetof(struct kdbus_cmd_bus_make, items) +
                              offsetof(struct kdbus_item, str) +
                              DECIMAL_STR_MAX(uid_t) + 1 + strlen(name) + 1));

        n = make->items;
        sprintf(n->str, "%lu-%s", (unsigned long) getuid(), name);
        n->size = offsetof(struct kdbus_item, str) + strlen(n->str) + 1;
        n->type = KDBUS_MAKE_NAME;

        make->size = ALIGN8(offsetof(struct kdbus_cmd_bus_make, items) + n->size);
        make->flags = KDBUS_MAKE_POLICY_OPEN;
        make->bus_flags = 0;
        make->bloom_size = BLOOM_SIZE;
        assert_cc(BLOOM_SIZE % 8 == 0);

        if (ioctl(fd, KDBUS_CMD_BUS_MAKE, make) < 0) {
                close_nointr_nofail(fd);
                return -errno;
        }

        /* The higher 32bit of the flags field are considered
         * 'incompatible flags'. Refuse them all for now. */
        if (make->flags > 0xFFFFFFFFULL) {
                close_nointr_nofail(fd);
                return -ENOTSUP;
        }

        if (s) {
                char *p;

                p = strjoin("/dev/kdbus/", n->str, "/bus", NULL);
                if (!p) {
                        close_nointr_nofail(fd);
                        return -ENOMEM;
                }

                *s = p;
        }

        return fd;
}

int bus_kernel_create_starter(const char *bus, const char *name) {
        struct kdbus_cmd_hello *hello;
        struct kdbus_item *n;
        char *p;
        int fd;

        assert(bus);
        assert(name);

        p = alloca(sizeof("/dev/kdbus/") - 1 + DECIMAL_STR_MAX(uid_t) + 1 + strlen(bus) + sizeof("/bus"));
        sprintf(p, "/dev/kdbus/%lu-%s/bus", (unsigned long) getuid(), bus);

        fd = open(p, O_RDWR|O_NOCTTY|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        hello = alloca0(ALIGN8(offsetof(struct kdbus_cmd_hello, items) +
                               offsetof(struct kdbus_item, str) +
                               strlen(name) + 1));

        n = hello->items;
        strcpy(n->str, name);
        n->size = offsetof(struct kdbus_item, str) + strlen(n->str) + 1;
        n->type = KDBUS_ITEM_STARTER_NAME;

        hello->size = ALIGN8(offsetof(struct kdbus_cmd_hello, items) + n->size);
        hello->conn_flags = KDBUS_HELLO_STARTER;
        hello->pool_size = KDBUS_POOL_SIZE;

        if (ioctl(fd, KDBUS_CMD_HELLO, hello) < 0) {
                close_nointr_nofail(fd);
                return -errno;
        }

        /* The higher 32bit of both flags fields are considered
         * 'incompatible flags'. Refuse them all for now. */
        if (hello->bus_flags > 0xFFFFFFFFULL ||
            hello->conn_flags > 0xFFFFFFFFULL) {
                close_nointr_nofail(fd);
                return -ENOTSUP;
        }

        if (hello->bloom_size != BLOOM_SIZE) {
                close_nointr_nofail(fd);
                return -ENOTSUP;
        }

        return fd;
}

int bus_kernel_create_namespace(const char *name, char **s) {
        struct kdbus_cmd_ns_make *make;
        struct kdbus_item *n;
        int fd;

        assert(name);
        assert(s);

        fd = open("/dev/kdbus/control", O_RDWR|O_NOCTTY|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        make = alloca0(ALIGN8(offsetof(struct kdbus_cmd_ns_make, items) +
                              offsetof(struct kdbus_item, str) +
                              strlen(name) + 1));

        n = make->items;
        strcpy(n->str, name);
        n->size = offsetof(struct kdbus_item, str) + strlen(n->str) + 1;
        n->type = KDBUS_MAKE_NAME;

        make->size = ALIGN8(offsetof(struct kdbus_cmd_ns_make, items) + n->size);
        make->flags = KDBUS_MAKE_POLICY_OPEN | KDBUS_MAKE_ACCESS_WORLD;

        if (ioctl(fd, KDBUS_CMD_NS_MAKE, make) < 0) {
                close_nointr_nofail(fd);
                return -errno;
        }

        /* The higher 32bit of the flags field are considered
         * 'incompatible flags'. Refuse them all for now. */
        if (make->flags > 0xFFFFFFFFULL) {
                close_nointr_nofail(fd);
                return -ENOTSUP;
        }

        if (s) {
                char *p;

                p = strappend("/dev/kdbus/ns/", name);
                if (!p) {
                        close_nointr_nofail(fd);
                        return -ENOMEM;
                }

                *s = p;
        }

        return fd;
}

int bus_kernel_monitor(sd_bus *bus) {
        struct kdbus_cmd_monitor cmd_monitor;
        int r;

        assert(bus);

        cmd_monitor.id = 0;
        cmd_monitor.flags = KDBUS_MONITOR_ENABLE;

        r = ioctl(bus->input_fd, KDBUS_CMD_MONITOR, &cmd_monitor);
        if (r < 0)
                return -errno;

        return 1;
}
