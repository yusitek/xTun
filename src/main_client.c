#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include "util.h"
#include "logger.h"
#include "crypto.h"
#include "daemon.h"
#include "tun.h"
#include "http.h"
#include "xTun.h"


static int mtu = MTU;
static int port = 8080;
static int daemon_mode = 1;
static uint32_t parallel = 1;
static char *iface = "tun0";
static char *ifconf;
static char *addrbuf;
static char *pidfile = "/var/run/xTun.pid";
static char *password = NULL;
static char *httpheader = NULL;
static char *xsignal;

int signal_process(char *signal, const char *pidfile);

static const char *_optString = "i:I:k:c:p:nVvhH:";
static const struct option _lopts[] = {
    { "",        required_argument,   NULL, 'i' },
    { "",        required_argument,   NULL, 'I' },
    { "",        required_argument,   NULL, 'k' },
    { "client",  required_argument,   NULL, 'c' },
    { "port",    required_argument,   NULL, 'p' },
    { "mtu",     required_argument,   NULL,  0  },
    { "pid",     required_argument,   NULL,  0  },
    { "signal",  required_argument,   NULL,  0  },
    { "",        no_argument,         NULL, 'n' },
    { "",        no_argument,         NULL, 'V' },
    { "version", no_argument,         NULL, 'v' },
    { "help",    no_argument,         NULL, 'h' },
    { "header",  required_argument,   NULL, 'H' },
    { NULL,      no_argument,         NULL,  0  }
};


static void
print_usage(const char *prog) {
    printf("xTun Version: %s Maintained by lparam\n", xTun_VER);
    printf("Usage:\n  %s [options]\n", prog);
    printf("Options:\n");
    puts(""
         "  -I <ifconf>\t\t CIDR of interface (e.g. 10.3.0.1/16)\n"
         "  -k <encryption_key>\t shared password for data encryption\n"
         "  -c --client <host>\t run in client mode, connecting to <host>\n"
         "  [-p --port <port>]\t server port to listen on/connect to (default: 8080)\n"
         "  [-i <iface>]\t\t interface name (default: tun0)\n"
         "  [--pid <pid>]\t\t PID file of daemon (default: /var/run/xTun.pid)\n"
         "  [--mtu <mtu>]\t\t MTU size (default: 1426)\n"
         "  [--signal <signal>]\t send signal to xTun: quit, stop\n"
         "  [-n]\t\t\t non daemon mode\n"
         "  [-h, --help]\t\t this help\n"
         "  [-H, --header]\t add http header\n"
         "  [-v, --version]\t show version\n"
         "  [-V] \t\t\t verbose mode\n");

    exit(1);
}

static void
parse_opts(int argc, char *argv[]) {
    int opt = 0, longindex = 0;

    while ((opt = getopt_long(argc, argv, _optString, _lopts, &longindex)) != -1) {
        switch (opt) {
        case 'i':
            iface = optarg;
            break;
        case 'I':
            ifconf = optarg;
            break;
        case 'c':
            mode = xTUN_CLIENT;
            addrbuf = optarg;
            break;

        case 'k':
            password = optarg;
            break;

        case 'p':
            port = strtol(optarg, NULL, 10);
            break;




        case 'n':
            daemon_mode = 0;
            break;
        case 'V':
            verbose = 1;
            break;
        case 'v':
            printf("xTun version: %s \n", xTun_VER);
            exit(0);
            break;

        case 'H':
            httpheader = optarg;
            break;

        case 'h':
        case '?':
            print_usage(argv[0]);
            break;
		case 0: /* long option without a short arg */
            if (strcmp("signal", _lopts[longindex].name) == 0) {
                xsignal = optarg;
                if (strcmp(xsignal, "stop") == 0
                  || strcmp(xsignal, "quit") == 0) {
                    break;
                }
                fprintf(stderr, "invalid option: --signal %s\n", xsignal);
                print_usage(argv[0]);
            }
            if (strcmp("mtu", _lopts[longindex].name) == 0) {
                mtu = strtol(optarg, NULL, 10);
                if(!mtu || mtu < 0 || mtu > 4096) {
                    mtu = MTU;
                }
            }
            if (strcmp("pid", _lopts[longindex].name) == 0) {
                pidfile = optarg;
            }
			break;
        default:
            print_usage(argv[0]);
            break;
        }
    }
}

static int
init() {
    logger_init(daemon_mode);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGABRT, SIG_IGN);

    if (crypto_init(password)) {
        logger_stderr("crypto init failed");
        exit(1);
    }

    return 0;
}

int
main(int argc, char *argv[]) {
    protocol = xTUN_TCP;

    parse_opts(argc, argv);

    if (xsignal) {
        return signal_process(xsignal, pidfile);
    }

    if (!mode || !iface || !ifconf || !password || !httpheader || !addrbuf) {
        print_usage(argv[0]);
        return 1;
    }

    init_client_header(httpheader);

    if (daemon_mode) {
        if (daemonize()) {
            return 1;
        }
        if (already_running(pidfile)) {
            logger_stderr("xTun already running.");
            return 1;
        }
    }

    init();

    struct sockaddr addr;
    int rc = resolve_addr(addrbuf, port, &addr);
    if (rc) {
        logger_stderr("invalid address");
        return 1;
    }

	struct tundev *tun = tun_alloc(iface, parallel);
    if (!tun) {
        return 1;
    }

    tun_config(tun, ifconf, mtu, &addr);
    tun_start(tun);

    tun_free(tun);
    if (daemon_mode) {
        delete_pidfile(pidfile);
    }
    logger_exit();

    return 0;
}
