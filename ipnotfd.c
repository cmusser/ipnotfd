#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>

#include <errno.h>
#include <ifaddrs.h>
#ifdef __FreeBSD__
#include <limits.h>
#endif
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

typedef enum {
	UNKNOWN,
	INITIAL,
	CHANGED,
	UNCHANGED,
	UPDATE_STATUS_LAST_PLUS_ONE,
}		update_status;

const char     *update_status_string_array[UPDATE_STATUS_LAST_PLUS_ONE] =
{
	"UNKNOWN",
	"initial address",
	"changed to",
	"remains",
};

#define UPDATE_STATUS_STR(status) \
	(((status) >= UPDATE_STATUS_LAST_PLUS_ONE) \
	    ? "INVALID" : update_status_string_array[(status)])


struct ipnotf_state {
	bool		foreground;
	bool		check_on_start;
	sa_family_t	notify_af;
	char		script_path[PATH_MAX];
	char		intf_name [IFNAMSIZ];
	struct sockaddr_in addr;
	struct sockaddr_in6 addr6;
};

#define ROUNDUP(a, size) (((a) & ((size) - 1)) ? (1 + ((a) | ((size) - 1))) : (a))
#define NEXT_SA(ap) ap = (struct sockaddr *) \
	    ((caddr_t) ap + (ap->sa_len ? ROUNDUP(ap->sa_len, sizeof(u_long)) : \
		sizeof(u_long)))

void		log_msg   (struct ipnotf_state *ipnotf, int priority, const char *msg,...);
void		spawn_subprocess(struct ipnotf_state *ipnotf, char *cmd);
update_status	update_addr(sa_family_t cur_af, void *cur_addr, struct sockaddr *addr, uint8_t addr_len);
void		handle_intf_addr(struct ipnotf_state *ipnotf, char *intf_name, struct sockaddr *addr);
int		run        (struct ipnotf_state *ipnotf);

void
log_msg(struct ipnotf_state *ipnotf, int priority, const char *msg,...)
{
	va_list		ap;
	time_t		now;
	struct tm	now_tm;
	char		timestamp_str[32];
	char		msg_str   [1024];

	va_start(ap, msg);
	if (ipnotf->foreground) {
		if (priority <= LOG_NOTICE) {
			time(&now);
			localtime_r(&now, &now_tm);
			strftime(timestamp_str, sizeof(timestamp_str), "%c", &now_tm);
			vsnprintf(msg_str, sizeof(msg_str), msg, ap);
			fprintf(stderr, "%s %s", timestamp_str, msg_str);
			fprintf(stderr, "\n");
		}
	} else {
		vsyslog(priority, msg, ap);
	}
	va_end(ap);
}

void
spawn_subprocess(struct ipnotf_state *ipnotf, char *cmd)
{
	char		cmd_with_stderr_redirect[512];
	FILE           *cmd_fd;
	char		cmd_out   [256];
	char           *newline;

	snprintf(cmd_with_stderr_redirect, sizeof(cmd_with_stderr_redirect),
		 "%s 2>&1", cmd);
	if ((cmd_fd = popen(cmd_with_stderr_redirect, "r")) == NULL) {
		log_msg(ipnotf, LOG_ERR, "spawn of \"%s\" failed: %s", cmd_with_stderr_redirect,
			strerror(errno));
	} else {

		while (fgets(cmd_out, sizeof(cmd_out), cmd_fd) != NULL) {
			newline = strrchr(cmd_out, '\n');
			if (newline)
				*newline = '\0';
			log_msg(ipnotf, LOG_NOTICE, "==> %s", cmd_out);
		}

		if (ferror(cmd_fd))
			log_msg(ipnotf, LOG_ERR, "reading subprocess output: %s", strerror(errno));

		pclose(cmd_fd);
	}
}

update_status
update_addr(sa_family_t cur_af, void *cur_addr, struct sockaddr *addr, uint8_t addr_len)
{
	update_status	status = UNKNOWN;
	bool		changed = true;

	if (cur_af == AF_UNSPEC)
		status = INITIAL;
	else if (memcmp(cur_addr, addr, addr_len) != 0)
		status = CHANGED;
	else
		changed = false;

	if (changed)
		memcpy(cur_addr, addr, addr_len);
	else
		status = UNCHANGED;

	return status;
}

void
handle_intf_addr(struct ipnotf_state *ipnotf, char *intf_name, struct sockaddr *addr)
{
	void           *addr_p = NULL;
	char		addr_str  [INET6_ADDRSTRLEN];
	update_status   update_status = UNKNOWN;
	char           cmd[512];

	if (addr->sa_family == AF_INET &&
	    (ipnotf->notify_af == AF_INET || ipnotf->notify_af == AF_UNSPEC)) {
		addr_p = &((struct sockaddr_in *)addr)->sin_addr;
		update_status = update_addr(ipnotf->addr.sin_family, &ipnotf->addr,
					addr_p, sizeof(struct sockaddr_in));
	} else if (addr->sa_family == AF_INET6 &&
	    (ipnotf->notify_af == AF_INET6 || ipnotf->notify_af == AF_UNSPEC)) {
		addr_p = &((struct sockaddr_in6 *)addr)->sin6_addr;
		update_status = update_addr(ipnotf->addr6.sin6_family, &ipnotf->addr,
				       addr_p, sizeof(struct sockaddr_in6));
	}
	if (addr_p != NULL) {
		inet_ntop(addr->sa_family, addr_p, addr_str, sizeof(addr_str));
		log_msg(ipnotf, LOG_NOTICE, "%s %s: %s", intf_name,
		    UPDATE_STATUS_STR(update_status), addr_str);
		if (strlen(ipnotf->script_path) > 0 &&
		    (update_status == INITIAL || update_status == CHANGED)) {
			snprintf(cmd, sizeof(cmd), "%s %s", ipnotf->script_path, addr_str);
			spawn_subprocess(ipnotf, cmd);
		}
	}
}

int
run(struct ipnotf_state *ipnotf)
{
	bool		ok = true;
	int		sock;
	struct ifaddrs *ifa, *cur;
	char		buf       [1024];
	struct ifa_msghdr *ifa_msg;
	char		updated_intf_name[IFNAMSIZ] = {'\0'};
	struct sockaddr *sa;

	if ((sock = socket(AF_ROUTE, SOCK_RAW, 0)) == -1) {
		ok = false;
		log_msg(ipnotf, LOG_ERR, "couldn't create routing socket -- %s",
			strerror(errno));
	}
	if (ok) {
		if (ipnotf->check_on_start) {
			if (getifaddrs(&ifa) != 0) {
				ok = false;
				log_msg(ipnotf, LOG_ERR, "couldn't get interface addrs -- %s",
					strerror(errno));
			} else {
				for (cur = ifa; cur != NULL; cur = cur->ifa_next) {
					if (strcmp(ipnotf->intf_name, cur->ifa_name) == 0) {
						handle_intf_addr(ipnotf, cur->ifa_name, cur->ifa_addr);
					}
				}
			}
			if (ifa)
				freeifaddrs(ifa);
		}
	}
	if (ok) {
		ifa_msg = (struct ifa_msghdr *)buf;
		while (ok) {
			bzero(buf, sizeof(buf));
			if ((read(sock, ifa_msg, sizeof(buf))) == -1) {
				ok = false;
				log_msg(ipnotf, LOG_ERR, "couldn't read from routing socket -- %s",
					strerror(errno));
			} else if (ifa_msg->ifam_type == RTM_NEWADDR) {
				sa = (struct sockaddr *)(ifa_msg + 1);
				if (if_indextoname(ifa_msg->ifam_index, updated_intf_name) != NULL
				    && strcmp(ipnotf->intf_name, updated_intf_name) == 0) {
					for (int i = 0; i < RTAX_MAX; i++) {
						if (ifa_msg->ifam_addrs & (1 << i)) {
							sa = (struct sockaddr *)sa;
							if (i == RTAX_IFA)
								handle_intf_addr(ipnotf, updated_intf_name, sa);
							NEXT_SA(sa);
						}
					}
				}
			}
		}
	}
	close(sock);

	return ok;
}

int
main(int argc, char *argv[])
{
	const char     *opts;
	int		ch;
	struct ipnotf_state ipnotf;

	bzero(&ipnotf, sizeof(ipnotf));
	ipnotf.check_on_start = true;
	ipnotf.notify_af = AF_INET;

	opts = "nfha:s:";

	while ((ch = getopt(argc, argv, opts)) != -1) {
		switch (ch) {
		case 'n':
			ipnotf.check_on_start = false;
			break;
		case 'f':
			ipnotf.foreground = true;
			break;
		case 'a':
			if (strcmp("4", optarg) == 0)
				ipnotf.notify_af = AF_INET;
			else if (strcmp("6", optarg) == 0)
				ipnotf.notify_af = AF_INET6;
			else if (strcmp("all", optarg) == 0)
				ipnotf.notify_af = AF_UNSPEC;
			else {
				fprintf(stderr, "-i must be one of \"4\" for IPv4, \"6\" for IPv6, or \"all\" for both address families\n");
				exit(EXIT_FAILURE);
			}

			break;
		case 's':
			strlcpy(ipnotf.script_path, optarg, sizeof(ipnotf.script_path));
			break;
		case 'h':
		default:
			fprintf(stderr, "usage: ipnotd [-nfh] [-a address-family] [-s script] <interface>\n");
			fprintf(stderr, "  -n: no address check on startup (default: check addresses at start)\n");
			fprintf(stderr, "  -f: foreground mode (default: run as a daemon)\n");
			fprintf(stderr, "  -a: address family to monitor (4|6|all) (default: act on IPv4 address)\n");
			fprintf(stderr, "  -s: script to run on address add (default: don't run a script, just report changes)\n");
			exit(EXIT_FAILURE);
		}
	}

	if (argv[optind] == NULL) {
		fprintf(stderr, "interface not specified\n");
		exit(EXIT_FAILURE);
	} else {
		strlcpy(ipnotf.intf_name, argv[optind], sizeof(ipnotf.intf_name));
	}

	if (!ipnotf.foreground)
		daemon(0, 0);

	return run(&ipnotf) ? EXIT_SUCCESS : EXIT_FAILURE;
}
