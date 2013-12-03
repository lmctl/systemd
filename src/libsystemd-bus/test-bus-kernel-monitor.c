/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lukasz Skalski

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

#include <getopt.h>
#include <signal.h>

#include "log.h"
#include "sd-bus.h"
#include "bus-message.h"
#include "bus-kernel.h"
#include "bus-util.h"
#include "bus-dump.h"

#define DEFAULT_BUS_KERNEL_PATH "kernel:path=/dev/kdbus/deine-mutter/bus"

sd_bus *bus = NULL;
sd_bus_message *msg = NULL;
const char *arg_address = DEFAULT_BUS_KERNEL_PATH;
//static bool arg_user = false;


static int help(void) {

        printf("%s [OPTIONS...]\n\n"
               "Monitor the kernel bus.\n\n"
               "     --help               Show this help\n"
               "     --bus_path=PATH      Path to the kernel bus (default: %s)\n",
               //"     --system             Connect to system bus\n"
               //"     --user               Connect to user bus\n"
               program_invocation_short_name, DEFAULT_BUS_KERNEL_PATH);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_ADDRESS,
                ARG_SYSTEM,
                ARG_USER,
        };

        static const struct option options[] = {
                { "help",       no_argument,       NULL, 'h'            },
                { "bus_path",   required_argument, NULL, ARG_ADDRESS    },
                { "system",     no_argument,       NULL, ARG_SYSTEM     },
                { "user",       no_argument,       NULL, ARG_USER       },
                {},
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        return help();

                case ARG_ADDRESS:
                        arg_address = optarg;
                        break;

                /*
                case ARG_USER:
                        arg_user = true;
                        break;

                case ARG_SYSTEM:
                        arg_user = false;
                        break;
                */

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }
        }

        return 1;
}

static void do_exit(int sig_no) {

        sd_bus_unref(bus);
        exit (EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {

        int r;

        log_set_max_level(LOG_DEBUG);

        if (signal(SIGINT, do_exit) == SIG_ERR)
                return EXIT_TEST_SKIP;

        r = parse_argv(argc, argv);
        if (r <= 0)
                return EXIT_TEST_SKIP;

        if (arg_address) {
                r = sd_bus_new(&bus);
                if (r < 0) {
                        log_error("Failed to allocate bus: %s", strerror(-r));
                        return EXIT_TEST_SKIP;
                }

                r = sd_bus_set_address(bus, arg_address);
                if (r < 0) {
                        log_error("Failed to set address: %s", strerror(-r));
                        return EXIT_TEST_SKIP;
                }

                r = sd_bus_start(bus);
                if (r < 0) {
                        log_error("Failed to bus start: %s", strerror(-r));
                        return EXIT_TEST_SKIP;
                }

                r = sd_bus_start(bus);
        }
        /*
        else {
                if (arg_user)
                        r = sd_bus_default_user(&bus);
                else
                        r = sd_bus_default_system(&bus);
        }
        */

        r = bus_kernel_monitor(bus);
        if (r < 0) {
                log_error("Failed to enable monitor mode: %s", strerror(-r));
                return EXIT_TEST_SKIP;
        }

        r = sd_bus_add_match(bus,"", NULL, NULL);
        assert_se(r >= 0);

        for(;;) {
                r = sd_bus_process(bus, &msg);
                assert_se(r >= 0);

                if (r == 0)
                        assert_se(sd_bus_wait(bus, (usec_t) -1) >= 0);
                if (!msg)
                        continue;

                bus_message_dump(msg, stdout, true);
                sd_bus_message_unref(msg);
                msg = NULL;
        }

        return 0;
}
