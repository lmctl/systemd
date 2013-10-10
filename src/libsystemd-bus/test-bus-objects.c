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

#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#include "log.h"
#include "util.h"
#include "macro.h"
#include "strv.h"

#include "sd-bus.h"
#include "bus-internal.h"
#include "bus-message.h"

/* Test:
 *
 *   sd_bus_add_object_manager()
 *   sd_bus_emit_properties_changed()
 *
 *   Add in:
 *
 *   automatic properties
 *   node hierarchy updates during dispatching
 *   emit_interfaces_added/emit_interfaces_removed
 *
 */

struct context {
        int fds[2];
        bool quit;
        char *something;
};

static int something_handler(sd_bus *bus, sd_bus_message *m, void *userdata) {
        struct context *c = userdata;
        const char *s;
        char *n = NULL;
        int r;

        r = sd_bus_message_read(m, "s", &s);
        assert_se(r > 0);

        n = strjoin("<<<", s, ">>>", NULL);
        assert_se(n);

        free(c->something);
        c->something = n;

        log_info("AlterSomething() called, got %s, returning %s", s, n);

        r = sd_bus_reply_method_return(bus, m, "s", n);
        assert_se(r >= 0);

        return 1;
}

static int exit_handler(sd_bus *bus, sd_bus_message *m, void *userdata) {
        struct context *c = userdata;
        int r;

        c->quit = true;

        log_info("Exit called");

        r = sd_bus_reply_method_return(bus, m, "");
        assert_se(r >= 0);

        return 1;
}

static int get_handler(sd_bus *bus, const char *path, const char *interface, const char *property, sd_bus_message *reply, sd_bus_error *error, void *userdata) {
        struct context *c = userdata;
        int r;

        log_info("property get for %s called", property);

        r = sd_bus_message_append(reply, "s", c->something);
        assert_se(r >= 0);

        return 1;
}

static int set_handler(sd_bus *bus, const char *path, const char *interface, const char *property, sd_bus_message *value, sd_bus_error *error, void *userdata) {
        struct context *c = userdata;
        const char *s;
        char *n;
        int r;

        log_info("property set for %s called", property);

        r = sd_bus_message_read(value, "s", &s);
        assert_se(r >= 0);

        n = strdup(s);
        assert_se(n);

        free(c->something);
        c->something = n;

        return 1;
}

static int value_handler(sd_bus *bus, const char *path, const char *interface, const char *property, sd_bus_message *reply, sd_bus_error *error, void *userdata) {
        _cleanup_free_ char *s = NULL;
        const char *x;
        int r;

        assert_se(asprintf(&s, "object %p, path %s", userdata, path) >= 0);
        r = sd_bus_message_append(reply, "s", s);
        assert_se(r >= 0);

        assert_se(x = startswith(path, "/value/"));

        assert_se(PTR_TO_UINT(userdata) == 30);


        return 1;
}

static const sd_bus_vtable vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("AlterSomething", "s", "s", 0, something_handler),
        SD_BUS_METHOD("Exit", "", "", 0, exit_handler),
        SD_BUS_WRITABLE_PROPERTY("Something", "s", get_handler, set_handler, 0, 0),
        SD_BUS_VTABLE_END
};

static const sd_bus_vtable vtable2[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Value", "s", value_handler, 10, 0),
        SD_BUS_VTABLE_END
};

static int enumerator_callback(sd_bus *b, const char *path, char ***nodes, void *userdata) {

        if (object_path_startswith("/value", path))
                assert_se(*nodes = strv_new("/value/a", "/value/b", "/value/c", NULL));

        return 1;
}

static void *server(void *p) {
        struct context *c = p;
        sd_bus *bus = NULL;
        sd_id128_t id;
        int r;

        c->quit = false;

        assert_se(sd_id128_randomize(&id) >= 0);

        assert_se(sd_bus_new(&bus) >= 0);
        assert_se(sd_bus_set_fd(bus, c->fds[0], c->fds[0]) >= 0);
        assert_se(sd_bus_set_server(bus, 1, id) >= 0);

        assert_se(sd_bus_add_object_vtable(bus, "/foo", "org.freedesktop.systemd.test", vtable, c) >= 0);
        assert_se(sd_bus_add_object_vtable(bus, "/foo", "org.freedesktop.systemd.test2", vtable, c) >= 0);
        assert_se(sd_bus_add_fallback_vtable(bus, "/value", "org.freedesktop.systemd.ValueTest", vtable2, NULL, UINT_TO_PTR(20)) >= 0);
        assert_se(sd_bus_add_node_enumerator(bus, "/value", enumerator_callback, NULL) >= 0);

        assert_se(sd_bus_start(bus) >= 0);

        log_error("Entering event loop on server");

        while (!c->quit) {
                log_error("Loop!");

                r = sd_bus_process(bus, NULL);
                if (r < 0) {
                        log_error("Failed to process requests: %s", strerror(-r));
                        goto fail;
                }

                if (r == 0) {
                        r = sd_bus_wait(bus, (uint64_t) -1);
                        if (r < 0) {
                                log_error("Failed to wait: %s", strerror(-r));
                                goto fail;
                        }

                        continue;
                }
        }

        r = 0;

fail:
        if (bus) {
                sd_bus_flush(bus);
                sd_bus_unref(bus);
        }

        return INT_TO_PTR(r);
}

static int client(struct context *c) {
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        _cleanup_bus_unref_ sd_bus *bus = NULL;
        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        const char *s;
        int r;

        assert_se(sd_bus_new(&bus) >= 0);
        assert_se(sd_bus_set_fd(bus, c->fds[1], c->fds[1]) >= 0);
        assert_se(sd_bus_start(bus) >= 0);

        r = sd_bus_call_method(bus, "org.freedesktop.systemd.test", "/foo", "org.freedesktop.systemd.test", "AlterSomething", &error, &reply, "s", "hallo");
        assert_se(r >= 0);

        r = sd_bus_message_read(reply, "s", &s);
        assert_se(r >= 0);
        assert_se(streq(s, "<<<hallo>>>"));

        sd_bus_message_unref(reply);
        reply = NULL;

        r = sd_bus_call_method(bus, "org.freedesktop.systemd.test", "/foo", "org.freedesktop.systemd.test", "Doesntexist", &error, &reply, "");
        assert_se(r < 0);
        assert_se(sd_bus_error_has_name(&error, "org.freedesktop.DBus.Error.UnknownMethod"));

        sd_bus_error_free(&error);

        r = sd_bus_call_method(bus, "org.freedesktop.systemd.test", "/foo", "org.freedesktop.systemd.test", "AlterSomething", &error, &reply, "as", 1, "hallo");
        assert_se(r < 0);
        assert_se(sd_bus_error_has_name(&error, "org.freedesktop.DBus.Error.InvalidArgs"));

        sd_bus_error_free(&error);

        r = sd_bus_get_property(bus, "org.freedesktop.systemd.test", "/foo", "org.freedesktop.systemd.test", "Something", &error, &reply, "s");
        assert_se(r >= 0);

        r = sd_bus_message_read(reply, "s", &s);
        assert_se(r >= 0);
        assert_se(streq(s, "<<<hallo>>>"));

        sd_bus_message_unref(reply);
        reply = NULL;

        r = sd_bus_set_property(bus, "org.freedesktop.systemd.test", "/foo", "org.freedesktop.systemd.test", "Something", &error, "s", "test");
        assert_se(r >= 0);

        r = sd_bus_get_property(bus, "org.freedesktop.systemd.test", "/foo", "org.freedesktop.systemd.test", "Something", &error, &reply, "s");
        assert_se(r >= 0);

        r = sd_bus_message_read(reply, "s", &s);
        assert_se(r >= 0);
        assert_se(streq(s, "test"));

        sd_bus_message_unref(reply);
        reply = NULL;

        r = sd_bus_call_method(bus, "org.freedesktop.systemd.test", "/foo", "org.freedesktop.DBus.Introspectable", "Introspect", &error, &reply, "");
        assert_se(r <= 0);

        r = sd_bus_message_read(reply, "s", &s);
        assert_se(r >= 0);
        fputs(s, stdout);

        sd_bus_message_unref(reply);
        reply = NULL;

        r = sd_bus_get_property(bus, "org.freedesktop.systemd.test", "/value/xuzz", "org.freedesktop.systemd.ValueTest", "Value", &error, &reply, "s");
        assert_se(r >= 0);

        r = sd_bus_message_read(reply, "s", &s);
        assert_se(r >= 0);
        log_info("read %s", s);

        sd_bus_message_unref(reply);
        reply = NULL;

        r = sd_bus_call_method(bus, "org.freedesktop.systemd.test", "/", "org.freedesktop.DBus.Introspectable", "Introspect", &error, &reply, "");
        assert_se(r <= 0);

        r = sd_bus_message_read(reply, "s", &s);
        assert_se(r >= 0);
        fputs(s, stdout);

        sd_bus_message_unref(reply);
        reply = NULL;

        r = sd_bus_call_method(bus, "org.freedesktop.systemd.test", "/value", "org.freedesktop.DBus.Introspectable", "Introspect", &error, &reply, "");
        assert_se(r <= 0);

        r = sd_bus_message_read(reply, "s", &s);
        assert_se(r >= 0);
        fputs(s, stdout);

        sd_bus_message_unref(reply);
        reply = NULL;

        r = sd_bus_call_method(bus, "org.freedesktop.systemd.test", "/value/a", "org.freedesktop.DBus.Introspectable", "Introspect", &error, &reply, "");
        assert_se(r <= 0);

        r = sd_bus_message_read(reply, "s", &s);
        assert_se(r >= 0);
        fputs(s, stdout);

        sd_bus_message_unref(reply);
        reply = NULL;

        r = sd_bus_call_method(bus, "org.freedesktop.systemd.test", "/foo", "org.freedesktop.DBus.Properties", "GetAll", &error, &reply, "s", "");
        assert_se(r <= 0);

        bus_message_dump(reply);

        sd_bus_message_unref(reply);
        reply = NULL;

        r = sd_bus_call_method(bus, "org.freedesktop.systemd.test", "/value/a", "org.freedesktop.DBus.Properties", "GetAll", &error, &reply, "s", "org.freedesktop.systemd.ValueTest2");
        assert_se(r < 0);
        assert_se(sd_bus_error_has_name(&error, "org.freedesktop.DBus.Error.UnknownInterface"));
        sd_bus_error_free(&error);

        r = sd_bus_call_method(bus, "org.freedesktop.systemd.test", "/foo", "org.freedesktop.systemd.test", "Exit", &error, NULL, "");
        assert_se(r >= 0);

        sd_bus_flush(bus);

        return 0;
}

int main(int argc, char *argv[]) {
        struct context c;
        pthread_t s;
        void *p;
        int r, q;

        zero(c);

        assert_se(socketpair(AF_UNIX, SOCK_STREAM, 0, c.fds) >= 0);

        r = pthread_create(&s, NULL, server, &c);
        if (r != 0)
                return -r;

        r = client(&c);

        q = pthread_join(s, &p);
        if (q != 0)
                return -q;

        if (r < 0)
                return r;

        if (PTR_TO_INT(p) < 0)
                return PTR_TO_INT(p);

        free(c.something);

        return EXIT_SUCCESS;
}