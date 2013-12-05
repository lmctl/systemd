/***
  This file is part of systemd.

  Copyright 2013 Karol Lewandowski

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

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "config.h"
#include "log.h"
#include "sd-id128.h"
#include "sd-messages.h"
#include "sd-event.h"
#include "sd-bus.h"


#define DBUS_BUS_PATH    "org.freedesktop.DBus"
#define DBUS_IFACE       "org.freedesktop.DBus"
#define DBUS_OBJ_PATH    "/org/freedesktop/DBus"

static bool arg_system = true;


static int dbus_hello(sd_bus *bus, sd_bus_message *m, void *userdata, sd_bus_error *error) {

     assert(bus);
     assert(m);

     return sd_bus_reply_method_return(m, "s", "Test. Test. Test. This function shall not be used in kdbus");

}

static int dbus_list_names(sd_bus *bus, sd_bus_message *m, void *userdata, sd_bus_error *error) {

     assert(bus);
     assert(m);

     return sd_bus_reply_method_return(m, "as", "test1", "test2");
}

static const sd_bus_vtable dbus_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Hello", "s", NULL, dbus_hello, 0),
        SD_BUS_METHOD("ListNames", "s", NULL, dbus_list_names, 0),
        SD_BUS_VTABLE_END,
};


static int help(void) {

        printf("%s [OPTIONS...]\n\n"
               "Provide compatibility org.freedesktop.DBus service.\n\n"
               "  -h --help               Show this help\n"
               "     --version            Show package version\n"
               "     --system             Connect to system bus\n"
               "     --user               Connect to user bus\n",
               program_invocation_short_name);

        return 0;
}


static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION,
                ARG_SYSTEM,
                ARG_USER,
        };
        static const struct option options[] = {
                { "system",      no_argument,     NULL,   ARG_SYSTEM  },
                { "user",        no_argument,     NULL,   ARG_USER    },
                { "version",     no_argument,     NULL,   ARG_VERSION },
                { "help",        no_argument,     NULL,   'h'         },
                { NULL,          0,               NULL,   0           }
        };
        int c;

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0) {
                switch (c) {

                case ARG_SYSTEM:
                        arg_system = true;
                        break;

                case ARG_USER:
                        arg_system = false;
                        break;

                case ARG_VERSION:
                        puts(PACKAGE_STRING);
                        return 0;

                case 'h':
                        return help();

                default:
                        return -EINVAL;
                }
        }

        return 1;
}

static int bus_get(sd_bus **_bus) {
        sd_bus *bus;
        int r;

        r = arg_system ? sd_bus_default_system(&bus) : sd_bus_default_user(&bus);
        if (r < 0) {
                log_error("Failed to get %s bus: %s", arg_system ? "system" : "user",
                          strerror(-r));
                return -r;
        }

        *_bus = bus;

        return 1;
}

int main(int argc, char *argv[]) {
        int r;
        sd_bus *bus = NULL;

        log_set_target(LOG_TARGET_CONSOLE);
        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return EXIT_FAILURE;

        r = bus_get(&bus);
        if (r <= 0)
                return EXIT_FAILURE;

        r = sd_bus_add_object_vtable(bus, DBUS_OBJ_PATH, DBUS_IFACE, dbus_vtable, NULL);
        if (r < 0) {
                log_error("Failed to register object: %s", strerror(-r));
                return EXIT_FAILURE;
        }

        r = sd_bus_request_name(bus, DBUS_BUS_PATH, SD_BUS_NAME_DO_NOT_QUEUE);
        if (r < 0) {
                log_error("Failed to register name: %s", strerror(-r));
                return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;

}
