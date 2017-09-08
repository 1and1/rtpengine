#include <sys/socket.h>
#include <sys/un.h>
#include "forward.h"
#include "main.h"
#include "log.h"
#include "fcntl.h"

int start_forwarding_capture(metafile_t *mf, char *meta_info) {
	int sock;
	struct sockaddr_un addr;

	if (mf->forward_fd >= 0) {
		ilog(LOG_ERR, "Connection already established");
		return -1;
	}

	if ((sock = socket(AF_UNIX, SOCK_SEQPACKET, 0)) == -1) {
		ilog(LOG_ERR, "Error creating socket: %s", strerror(errno));
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, forward_to, sizeof(addr.sun_path) - 1);

	if (connect(sock, (struct sockaddr*) &addr, sizeof(addr)) == -1) {
		ilog(LOG_ERR, "Error connecting to socket %s : %s", addr.sun_path,
				strerror(errno));
		return -1;
	}

	mf->forward_fd = sock;

	if (send(mf->forward_fd, meta_info, strlen(meta_info), 0) == -1) {
		ilog(LOG_ERR, "Error sending: %s", strerror(errno));
		return -1;
	}
	return 0;
}

int forward_packet(metafile_t *mf, unsigned char *buf, unsigned len) {
	if (!forward_to)
		return 0;

	if (mf->forward_fd == -1) {
		ilog(LOG_ERR,
				"Trying to send packets, but connection not initialized!");
		return -1;
	}

	if (send(mf->forward_fd, buf, len, 0) == -1) {
		ilog(LOG_ERR, "Error sending: %s",strerror(errno));
		return -1;
	}
	return 0;
}

