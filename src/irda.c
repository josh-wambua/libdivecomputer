#include <stdlib.h> // malloc, free
#include <stdio.h>	// snprintf
#ifdef _WIN32
	#include <winsock2.h>
	#include <windows.h>
	#include <af_irda.h>
#else
	#include <string.h>			// strerror
	#include <errno.h>			// errno
	#include <unistd.h>			// close
	#include <sys/types.h>		// socket, getsockopt
	#include <sys/socket.h>		// socket, getsockopt
	#include <linux/types.h>	// irda
	#include <linux/irda.h>		// irda
	#include <sys/select.h>		// select
	#include <sys/ioctl.h>		// ioctl
#endif

#include "irda.h"
#include "utils.h"

#ifdef _WIN32
#define TRACE(expr) \
{ \
	DWORD error = WSAGetLastError (); \
	message ("TRACE (%s:%d, %s): %s (%d)\n", __FILE__, __LINE__, \
		expr, irda_errmsg (), error); \
	WSASetLastError (error); \
}
#else
#define TRACE(expr) \
{ \
	int error = errno; \
	message ("TRACE (%s:%d, %s): %s (%d)\n", __FILE__, __LINE__, \
		expr, strerror (errno), errno); \
	errno = error; \
}
#endif


struct irda {
#ifdef _WIN32
	SOCKET fd;
#else
	int fd;
#endif
	long timeout;
};


int irda_errcode ()
{
#ifdef _WIN32
	return WSAGetLastError ();
#else
	return errno;
#endif
}


const char* irda_errmsg ()
{
#ifdef _WIN32
	static char buffer[256] = {0};
	unsigned int size = sizeof (buffer) / sizeof (char);

	DWORD errcode = WSAGetLastError ();
	DWORD rc = FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, errcode, 0, buffer, size, NULL);
	// Remove certain characters ('\r', '\n' and '.')
	// at the end of the error message.
	while (rc > 0 && (
			buffer[rc-1] == '\n' ||
			buffer[rc-1] == '\r' ||
			buffer[rc-1] == '.')) {
		buffer[rc-1] = '\0';
		rc--;
	}
	if (rc) {
		return buffer;
	} else {
		return NULL;
	}
#else
	return strerror (errno);
#endif
}


int irda_init ()
{
#ifdef _WIN32
	WSADATA wsaData = {0};
	WORD wVersionRequested = MAKEWORD (2, 2);
	if (WSAStartup (wVersionRequested, &wsaData) != 0) {
		TRACE ("WSAStartup");
		return -1;
	}

	// Confirm that the WinSock DLL supports 2.2.
	// Note that if the DLL supports versions greater
	// than 2.2 in addition to 2.2, it will still return
	// 2.2 in wVersion since that is the version we requested.
	if (LOBYTE (wsaData.wVersion) != 2 || 
		HIBYTE (wsaData.wVersion) != 2) {
		TRACE ("wsaData.wVersion");
		WSACleanup ();
		return -1; 
	}
#endif

	return 0;
}


int irda_cleanup ()
{
#ifdef _WIN32
	if (WSACleanup () != 0) {
		TRACE ("WSACleanup");
		return -1;
	}
#endif

	return 0;
}


int
irda_socket_open (irda **out)
{
	if (out == NULL)
		return -1; // EINVAL (Invalid argument)

	// Allocate memory.
	struct irda *device = malloc (sizeof (struct irda));
	if (device == NULL) {
		TRACE ("malloc");
		return -1; // ENOMEM (Not enough space)
	}

	// Default to blocking reads.
	device->timeout = -1;

	// Open the socket.
	device->fd = socket (AF_IRDA, SOCK_STREAM, 0);
#ifdef _WIN32
	if (device->fd == INVALID_SOCKET) {
#else
	if (device->fd == -1) {
#endif
		TRACE ("socket");
		free (device);
		return -1;
	}

	*out = device;

    return 0;
}


int
irda_socket_close (irda *device)
{
	if (device == NULL)
		return -1;

	// Terminate all send and receive operations.
	shutdown (device->fd, 0);

	// Close the socket.
#ifdef _WIN32
	if (closesocket (device->fd) != 0) {
		TRACE ("closesocket");
#else
	if (close (device->fd) != 0) {
		TRACE ("close");
#endif
		free (device);
		return -1;
	}

	// Free memory.	
	free (device);

	return 0;
}


int
irda_socket_set_timeout (irda *device, long timeout)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	device->timeout = timeout;

	return 0;
}


#define DISCOVER_MAX_DEVICES 16	// Maximum number of devices.
#define DISCOVER_MAX_RETRIES 4	// Maximum number of retries.

#ifdef _WIN32
#define DISCOVER_BUFSIZE sizeof (DEVICELIST) + \
				sizeof (IRDA_DEVICE_INFO) * (DISCOVER_MAX_DEVICES - 1)
#else
#define DISCOVER_BUFSIZE sizeof (struct irda_device_list) + \
				sizeof (struct irda_device_info) * (DISCOVER_MAX_DEVICES - 1)
#endif

int
irda_socket_discover (irda *device, irda_callback_t callback, void *userdata)
{
	if (device == NULL)
		return -1;

	unsigned char data[DISCOVER_BUFSIZE] = {0};
#ifdef _WIN32
	DEVICELIST *list = (DEVICELIST *) data;
	int size = sizeof (data);
#else
	struct irda_device_list *list = (struct irda_device_list *) data;
	socklen_t size = sizeof (data);
#endif

	int rc = 0;
	unsigned int n = 0;
	while ((rc = getsockopt (device->fd, SOL_IRLMP, IRLMP_ENUMDEVICES, data, &size)) != 0) {
#ifdef _WIN32
		if (WSAGetLastError() != WSAEWOULDBLOCK) {
#else
		if (errno != EAGAIN) {
#endif
			TRACE ("getsockopt");
			return -1; // Error during getsockopt call.
		}

		if (n >= DISCOVER_MAX_RETRIES)
			return 0;

		n++;

#ifdef _WIN32
		Sleep (1000);
#else
		sleep (1);
#endif
	}

	if (callback) {
#ifdef _WIN32
		for (unsigned int i = 0; i < list->numDevice; ++i) {
			unsigned int address = (list->Device[i].irdaDeviceID[0] << 24) +
									(list->Device[i].irdaDeviceID[1] << 16) +
									(list->Device[i].irdaDeviceID[2] << 8) +
									list->Device[i].irdaDeviceID[3];
			unsigned int hints = (list->Device[i].irdaDeviceHints1 << 8) + 
									list->Device[i].irdaDeviceHints2;
			callback (address, 
				list->Device[i].irdaDeviceName,
				list->Device[i].irdaCharSet,
				hints, 
				userdata);
		}
#else
		for (unsigned int i = 0; i < list->len; ++i) {
			unsigned int hints = (list->dev[i].hints[0] << 8) + 
									list->dev[i].hints[1];
			callback (list->dev[i].daddr, 
				list->dev[i].info, 
				list->dev[i].charset, 
				hints,
				userdata);
		}
#endif
	}

	return 0;
}


int
irda_socket_connect_name (irda *device, unsigned int address, const char *name)
{
	if (device == NULL)
		return -1;

#ifdef _WIN32
	SOCKADDR_IRDA peer = {0};
	peer.irdaAddressFamily = AF_IRDA;
	peer.irdaDeviceID[0] = (address >> 24) & 0xFF;
	peer.irdaDeviceID[1] = (address >> 16) & 0xFF;
	peer.irdaDeviceID[2] = (address >>  8) & 0xFF;
	peer.irdaDeviceID[3] = (address      ) & 0xFF;
    if (name)
		strncpy (peer.irdaServiceName, name, 25);
	else
		memset (peer.irdaServiceName, 0x00, 25);
#else
	struct sockaddr_irda peer = {0};
	peer.sir_family = AF_IRDA;
	peer.sir_addr = address;
	if (name)
		strncpy (peer.sir_name, name, 25);
	else
		memset (peer.sir_name, 0x00, 25);
#endif

	if (connect (device->fd, (struct sockaddr *) &peer, sizeof (peer)) != 0) {
		TRACE ("connect");
		return -1;
	}

	return 0;
}

int
irda_socket_connect_lsap (irda *device, unsigned int address, unsigned int lsap)
{
	if (device == NULL)
		return -1;

#ifdef _WIN32
	SOCKADDR_IRDA peer = {0};
	peer.irdaAddressFamily = AF_IRDA;
	peer.irdaDeviceID[0] = (address >> 24) & 0xFF;
	peer.irdaDeviceID[1] = (address >> 16) & 0xFF;
	peer.irdaDeviceID[2] = (address >>  8) & 0xFF;
	peer.irdaDeviceID[3] = (address      ) & 0xFF;
	snprintf (peer.irdaServiceName, 25, "LSAP-SEL%u", lsap);
#else
	struct sockaddr_irda peer = {0};
	peer.sir_family = AF_IRDA;
	peer.sir_addr = address;
	peer.sir_lsap_sel = lsap;
	memset (peer.sir_name, 0x00, 25);
#endif

	if (connect (device->fd, (struct sockaddr *) &peer, sizeof (peer)) != 0) {
		TRACE ("connect");
		return -1;
	}

	return 0;
}


int
irda_socket_available (irda* device)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

#ifdef _WIN32
	unsigned long bytes = 0;
	if (ioctlsocket (device->fd, FIONREAD, &bytes) != 0) {
		TRACE ("ioctlsocket");
#else
	int bytes = 0;
	if (ioctl (device->fd, FIONREAD, &bytes) != 0) {
		TRACE ("ioctl");
#endif
		return -1;
	}

	return bytes;
}


int
irda_socket_read (irda* device, void* data, unsigned int size)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	struct timeval tv = {0};
	if (device->timeout >= 0) {
		tv.tv_sec  = (device->timeout / 1000);
		tv.tv_usec = (device->timeout % 1000) * 1000;
	}

	fd_set fds;
	FD_ZERO (&fds);
	FD_SET (device->fd, &fds);

	unsigned int nbytes = 0;
	while (nbytes < size) {
		int rc = select (device->fd + 1, &fds, NULL, NULL, (device->timeout >= 0 ? &tv : NULL));
		if (rc < 0) {
			TRACE ("select");
			return -1; // Error during select call.
		} else if (rc == 0) {
			break; // Timeout.
		}

		int n = recv (device->fd, data + nbytes, size - nbytes, 0);
		if (n < 0) {
			TRACE ("recv");
			return -1; // Error during recv call.
		} else if (n == 0) {
			break; // EOF reached.
		}

		nbytes += n;
	}

	return nbytes;
}


int
irda_socket_write (irda* device, const void *data, unsigned int size)
{
	if (device == NULL)
		return -1; // EINVAL (Invalid argument)

	unsigned int nbytes = 0;
	while (nbytes < size) {
		int n = send (device->fd, data + nbytes, size - nbytes, 0);
		if (n < 0) {
			TRACE ("send");
			return -1; // Error during send call.
		}

		nbytes += n;
	}

	return nbytes;
}