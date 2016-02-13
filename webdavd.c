// TODO ssl
// TODO auth modes other than basic?
// TODO correct failure codes on collections
// TODO configuration file & getopt
// TODO single root parent with multiple configured server processes
// TODO protect RAP sessions from DOS attack using a lock per user
// TODO find client IP and integrate this both into logging and authentication

#include "shared.h"

#include <errno.h>
#include <fcntl.h>
#include <gnutls/abstract.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <limits.h>
#include <microhttpd.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>

////////////////
// Structures //
////////////////

struct RestrictedAccessProcessor {
	int rapSessionInUse;
	time_t rapCreated;
	int pid;
	int socketFd;
	const char * user;
	int writeDataFd;
	int readDataFd;
	int responseAlreadyGiven;
};

struct RapGroup {
	const char * user;
	const char * password;
	struct RestrictedAccessProcessor * rapSession;
};

struct Header {
	const char * key;
	const char * value;
};

struct SSLCertificate {
	const char * hostname;
	int certCount;
	gnutls_pcert_st * certs;
	gnutls_privkey_t key;
};

////////////////////
// End Structures //
////////////////////

//////////////////////////////////////
// Webdavd Configuration Structures //
//////////////////////////////////////

struct DaemonConfig {
	int port;
	const char * host;
	int sslEnabled;
};

struct SSLConfig {
	int chainFileCount;
	const char * keyFile;
	const char * certificateFile;
	const char ** chainFiles;

};

struct WebdavdConfiguration {
	const char * restrictedUser;

	// Daemons
	int daemonCount;
	struct DaemonConfig * daemons;

	// RAP
	time_t rapMaxSessionLife;
	int rapMaxSessionsPerUser;
	const char * pamServiceName;

	// files
	const char * mimeTypesFile;
	const char * rapBinary;
	const char * accessLog;
	const char * errorLog;

	// Add static files

	// SSL
	int sslCertCount;
	struct SSLConfig * sslCerts;
}static config;

//////////////////////////////////////////
// End Webdavd Configuration Structures //
//////////////////////////////////////////

// TODO remove REPORT or implement it
#define ACCEPT_HEADER "OPTIONS, GET, HEAD, DELETE, PROPFIND, PUT, PROPPATCH, COPY, MOVE, REPORT, LOCK, UNLOCK"

static struct MHD_Response * INTERNAL_SERVER_ERROR_PAGE;
static struct MHD_Response * UNAUTHORIZED_PAGE;
static struct MHD_Response * METHOD_NOT_SUPPORTED_PAGE;

// Used as a place holder for failed auth requests which failed due to invalid credentials
static const struct RestrictedAccessProcessor AUTH_FAILED_RAP = { .pid = 0, .socketFd = -1, .user = "<auth failed>",
		.writeDataFd = -1, .readDataFd = -1, .responseAlreadyGiven = 1 };

// Used as a place holder for failed auth requests which failed due to errors
static const struct RestrictedAccessProcessor AUTH_ERROR_RAP = { .pid = 0, .socketFd = -1, .user = "<auth error>",
		.writeDataFd = -1, .readDataFd = -1, .responseAlreadyGiven = 1 };

static const struct RestrictedAccessProcessor AUTH_ERROR_BACKOFF = { .pid = 0, .socketFd = -1, .user = "<backoff>",
		.writeDataFd = -1, .readDataFd = -1, .responseAlreadyGiven = 1 };

#define AUTH_FAILED ((struct RestrictedAccessProcessor *)&AUTH_FAILED_RAP)
#define AUTH_ERROR ((struct RestrictedAccessProcessor *)&AUTH_ERROR_RAP)
#define AUTH_BACKOFF ((struct RestrictedAccessProcessor *)&AUTH_ERROR_BACKOFF)

/////////////
// Utility //
/////////////

static FILE * accessLog;

static void logAccess(int statusCode, const char * method, const char * user, const char * url, const char * client) {
	char t[100];
	fprintf(accessLog, "%s %s %s %d %s %s\n", timeNow(t), client, user, statusCode, method, url);
	fflush(accessLog);
}

static void initializeLogs() {
	// Error log first
	int errorLog = open(config.errorLog, O_CREAT | O_APPEND | O_WRONLY, 416);
	if (errorLog == -1 || dup2(errorLog, STDERR_FILENO) == -1) {
		stdLogError(errno, "Could not open error log file %s", config.errorLog);
		exit(1);
	}
	close(errorLog);

	int accessLogFd = open(config.accessLog, O_CREAT | O_APPEND | O_WRONLY, 416);
	if (accessLogFd == -1) {
		stdLogError(errno, "Could not open access log file %s", config.accessLog);
		exit(1);
	}

	accessLog = fdopen(accessLogFd, "w");
}

static char * copyString(const char * string) {
	if (!string) {
		return NULL;
	}
	size_t stringSize = strlen(string) + 1;
	char * newString = mallocSafe(stringSize);
	memcpy(newString, string, stringSize);
	return newString;
}

static void getRequestIP(char * buffer, size_t bufferSize, struct MHD_Connection * request) {
	const struct sockaddr * addressInfo =
			MHD_get_connection_info(request, MHD_CONNECTION_INFO_CLIENT_ADDRESS)->client_addr;
	static unsigned char IPV4_PREFIX[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
	unsigned char * address;
	switch (addressInfo->sa_family) {
	case AF_INET: {
		struct sockaddr_in * v4Address = (struct sockaddr_in *) addressInfo;
		unsigned char * address = (unsigned char *) (&v4Address->sin_addr);
		PRINT_IPV4: snprintf(buffer, bufferSize, "%d.%d.%d.%d", address[0], address[1], address[2], address[3]);
		break;
	}

	case AF_INET6: {
		struct sockaddr_in6 * v6Address = (struct sockaddr_in6 *) addressInfo;
		// See RFC 5952 section 4 for formatting rules
		// find 0 run
		unsigned char * address = (unsigned char *) (&v6Address->sin6_addr);
		if (!memcmp(IPV4_PREFIX, address, sizeof(IPV4_PREFIX))) {
			snprintf(buffer, bufferSize, "%d.%d.%d.%d", address[sizeof(IPV4_PREFIX)], address[sizeof(IPV4_PREFIX) + 1],
					address[sizeof(IPV4_PREFIX) + 2], address[sizeof(IPV4_PREFIX) + 3]);
			break;
		}

		unsigned char * longestRun = NULL;
		int longestRunSize = 0;
		unsigned char * currentRun = NULL;
		int currentRunSize = 0;
		for (int i = 0; i < 16; i += 2) {
			if (*(address + i) == 0 && *(address + i + 1) == 0) {
				if (currentRunSize == 0) {
					currentRunSize = 2;
					currentRun = (address + i);
				} else {
					currentRunSize += 2;
					if (currentRunSize > longestRunSize) {
						longestRun = currentRun;
						longestRunSize = currentRunSize;
					}
				}
			} else {
				currentRunSize = 0;
			}
		}

		int bytesWritten;
		if (longestRunSize == 16) {
			bytesWritten = snprintf(buffer, bufferSize, "::");
			buffer += bytesWritten;
			bufferSize -= bytesWritten;
		} else {
			for (int i = 0; i < 16; i += 2) {
				if (&address[i] == longestRun) {
					bytesWritten = snprintf(buffer, bufferSize, i > 0 ? ":" : "::");
					buffer += bytesWritten;
					bufferSize -= bytesWritten;
					i += longestRunSize - 2;
				} else {
					if (*(address + i) == 0) {
						bytesWritten = snprintf(buffer, bufferSize, "%x%s", *(address + i + 1), i < 14 ? ":" : "");
						buffer += bytesWritten;
						bufferSize -= bytesWritten;
					} else {
						bytesWritten = snprintf(buffer, bufferSize, "%x%02x%s", *(address + i), *(address + i + 1),
								i < 14 ? ":" : "");
						buffer += bytesWritten;
						bufferSize -= bytesWritten;
					}
				}
			}
		}

		break;
	}

		// TODO find the unix user of the socket
		/*
		 case AF_UNIX: {
		 struct sockaddr_un * unixAddress = (struct sockaddr_in6 *)addressInfo;
		 break;
		 }*/

	default:
		snprintf(buffer, bufferSize, "<unknown address>");
	}
}

/////////////////
// End Utility //
/////////////////

/////////
// SSL //
/////////

static int sslCertificateCount;
static struct SSLCertificate * sslCertificates = NULL;

int sslCertificateCompareHost(const void * a, const void * b) {
	struct SSLCertificate * lhs = (struct SSLCertificate *) a;
	struct SSLCertificate * rhs = (struct SSLCertificate *) b;
	return strcmp(lhs->hostname, rhs->hostname);
}

struct SSLCertificate * findCertificateForHost(const char * hostname) {
	// TODO deal with wildcard certificates.
	struct SSLCertificate toFind = { .hostname = hostname };
	return bsearch(&toFind, sslCertificates, sslCertificateCount, sizeof(struct SSLCertificate),
			&sslCertificateCompareHost);
}

int sslSNICallback(gnutls_session_t session, const gnutls_datum_t* req_ca_dn, int nreqs,
		const gnutls_pk_algorithm_t* pk_algos, int pk_algos_length, gnutls_pcert_st** pcert, unsigned int *pcert_length,
		gnutls_privkey_t * pkey) {

	struct SSLCertificate * found = NULL;

	char name[1024];
	size_t name_len = sizeof(name) - 1;
	unsigned int type;
	if (GNUTLS_E_SUCCESS == gnutls_server_name_get(session, name, &name_len, &type, 0)) {
		name[name_len] = '\0';
		found = findCertificateForHost(name);
	}

	// Returning certificate
	if (!found) {
		found = &sslCertificates[0];
	}
	*pkey = found->key;
	*pcert_length = found->certCount;
	*pcert = found->certs;
	return 0;
}

int loadSSLCertificateFile(const char * fileName, gnutls_x509_crt_t * x509Certificate, gnutls_pcert_st * cert) {
	gnutls_datum_t certData;

	memset(cert, 0, sizeof(*cert));
	memset(x509Certificate, 0, sizeof(*x509Certificate));

	int ret;
	if ((ret = gnutls_load_file(fileName, &certData)) < 0) {
		return ret;
	}

	if ((ret = gnutls_x509_crt_init(x509Certificate)) < 0) {
		return ret;
	}

	ret = gnutls_x509_crt_import(*x509Certificate, &certData, GNUTLS_X509_FMT_PEM);
	gnutls_free(certData.data);
	if (ret < 0) {
		gnutls_x509_crt_deinit(*x509Certificate);
		return ret;
	}

	if ((ret = gnutls_pcert_import_x509(cert, *x509Certificate, 0)) < 0) {
		gnutls_x509_crt_deinit(*x509Certificate);
		return ret;
	}
	return ret;
}

int loadSSLKeyFile(const char * fileName, gnutls_privkey_t * key) {
	gnutls_datum_t keyData;
	int ret;
	if ((ret = gnutls_load_file(fileName, &keyData)) < 0) {
		return ret;
	}

	ret = gnutls_privkey_init(key);
	if (ret < 0) {
		gnutls_free(keyData.data);
		return ret;
	}

	ret = gnutls_privkey_import_x509_raw(*key, &keyData, GNUTLS_X509_FMT_PEM, NULL, 0);
	gnutls_free(keyData.data);
	if (ret < 0) {
		gnutls_privkey_deinit(*key);
	}

	return ret;
}

int loadSSLCertificate(struct SSLConfig * sslConfig) {
	struct SSLCertificate newCertificate;
	gnutls_x509_crt_t x509Certificate;
	int ret;
	ret = loadSSLKeyFile(sslConfig->keyFile, &newCertificate.key);
	if (ret < 0) {
		stdLogError(0, "Could not load %s: %s", sslConfig->keyFile, gnutls_strerror(ret));
		return 0;
	}
	newCertificate.certCount = sslConfig->chainFileCount + 1;
	newCertificate.certs = mallocSafe(newCertificate.certCount);
	for (int i = 0; i < sslConfig->chainFileCount; i++) {
		ret = loadSSLCertificateFile(sslConfig->chainFiles[i], &x509Certificate, &newCertificate.certs[i + 1]);
		if (ret < 0) {
			stdLogError(0, "Could not load %s: %s", sslConfig->chainFiles[i], gnutls_strerror(ret));
			gnutls_privkey_deinit(newCertificate.key);
			for (int j = 0; j < i; j++) {
				gnutls_pcert_deinit(&newCertificate.certs[j + 1]);
			}
			free(newCertificate.certs);
			return ret;
		}
		gnutls_x509_crt_deinit(x509Certificate);
	}
	ret = loadSSLCertificateFile(sslConfig->certificateFile, &x509Certificate, &newCertificate.certs[0]);
	if (ret < 0) {
		stdLogError(0, "Could not load %s: %s", sslConfig->certificateFile, gnutls_strerror(ret));
		gnutls_privkey_deinit(newCertificate.key);
		for (int i = 1; i < newCertificate.certCount; i++) {
			gnutls_pcert_deinit(&newCertificate.certs[i]);
		}
		free(newCertificate.certs);
	}

	int found = 0;
	for (int i = 0; ret != GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE; i++) {
		char domainName[1024];
		int critical;
		size_t dataSize = sizeof(domainName);
		int sanType;
		ret = gnutls_x509_crt_get_subject_alt_name2(x509Certificate, i, domainName, &dataSize, &sanType, &critical);
		if (ret != GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE && ret != GNUTLS_E_SHORT_MEMORY_BUFFER
				&& sanType == GNUTLS_SAN_DNSNAME) {

			stdLog("ssl domain %s --> %s", domainName, sslConfig->certificateFile);
			int index = sslCertificateCount++;
			sslCertificates = reallocSafe(sslCertificates, sslCertificateCount);
			sslCertificates[index] = newCertificate;
			sslCertificates[index].hostname = copyString(domainName);
			found = 1;
		}
	}

	gnutls_x509_crt_deinit(x509Certificate);

	if (!found) {
		stdLogError(0, "No subject alternative name found in %s", sslConfig->certificateFile);
		gnutls_privkey_deinit(newCertificate.key);
		for (int i = 0; i < newCertificate.certCount; i++) {
			gnutls_pcert_deinit(&newCertificate.certs[i]);
		}
		free(newCertificate.certs);
		return -1;
	}

	return 0;
}

void initializeSSL() {
	sslCertificates = mallocSafe(sslCertificateCount * sizeof(*sslCertificates));
	for (int i = 0; i < config.sslCertCount; i++) {
		if (loadSSLCertificate(&config.sslCerts[i])) {
			exit(1);
		}
	}
	qsort(sslCertificates, sslCertificateCount, sizeof(*sslCertificates), &sslCertificateCompareHost);
}

/////////////
// End SSL //
/////////////

///////////////////////
// Response Creation //
///////////////////////

static void addHeaderSafe(struct MHD_Response * response, const char * headerKey, const char * headerValue) {
	if (headerValue == NULL) {
		stdLogError(0, "Attempt to add null value as header %s:", headerKey);
		return;
	}
	if (MHD_add_response_header(response, headerKey, headerValue) != MHD_YES) {
		stdLogError(errno, "Could not add response header %s: %s", headerKey, headerValue);
		exit(255);
	}
}

static void addStaticHeaders(struct MHD_Response * response) {
	// TODO corect this header
	addHeaderSafe(response, "DAV", "1");
	// addHeaderSafe(response, "MS-Author-Via", "DAV");
	addHeaderSafe(response, "Accept-Ranges", "bytes");
	addHeaderSafe(response, "Keep-Alive", "timeout=30");
	addHeaderSafe(response, "Connection", "Keep-Alive");
	addHeaderSafe(response, "Server", "couling-webdavd");
	addHeaderSafe(response, "Expires", "Thu, 19 Nov 1981 08:52:00 GMT");
	addHeaderSafe(response, "Cache-Control", "no-store, no-cache, must-revalidate, post-check=0, pre-check=0");
	addHeaderSafe(response, "Pragma", "no-cache");
}

static ssize_t fdContentReader(int *fd, uint64_t pos, char *buf, size_t max) {
	size_t bytesRead = read(*fd, buf, max);
	if (bytesRead < 0) {
		stdLogError(errno, "Could not read content from fd");
		return MHD_CONTENT_READER_END_WITH_ERROR;
	}
	if (bytesRead == 0) {
		return MHD_CONTENT_READER_END_OF_STREAM;
	}
	while (bytesRead < max) {
		size_t newBytesRead = read(*fd, buf + bytesRead, max - bytesRead);
		if (newBytesRead <= 0) {
			break;
		}
		bytesRead += newBytesRead;
	}
	return bytesRead;
}

static void fdContentReaderCleanup(int *fd) {
	close(*fd);
	free(fd);
}

static struct MHD_Response * createFdStreamResponse(int fd, const char * mimeType, time_t date) {
	int * fdAllocated = mallocSafe(sizeof(int));
	*fdAllocated = fd;
	struct MHD_Response * response = MHD_create_response_from_callback(-1, 4096,
			(MHD_ContentReaderCallback) &fdContentReader, fdAllocated,
			(MHD_ContentReaderFreeCallback) &fdContentReaderCleanup);
	if (!response) {
		free(fdAllocated);
		return NULL;
	}
	char dateBuf[100];
	getWebDate(date, dateBuf, 100);
	addHeaderSafe(response, "Date", dateBuf);
	if (mimeType != NULL) {
		addHeaderSafe(response, "Content-Type", mimeType);
	}
	addStaticHeaders(response);
	return response;
}

static struct MHD_Response * createFdFileResponse(size_t size, int fd, const char * mimeType, time_t date) {
	struct MHD_Response * response = MHD_create_response_from_fd(size, fd);
	if (!response) {
		close(fd);
		return NULL;
	}
	char dateBuf[100];
	getWebDate(date, dateBuf, 100);
	addHeaderSafe(response, "Date", dateBuf);
	if (mimeType != NULL) {
		addHeaderSafe(response, "Content-Type", mimeType);
	}
	addStaticHeaders(response);
	return response;
}

static struct MHD_Response * createFileResponse(struct MHD_Connection *request, const char * fileName,
		const char * mimeType) {
	int fd = open(fileName, O_RDONLY);
	if (fd == -1) {
		stdLogError(errno, "Could not open file for response", fileName);
		return NULL;
	}

	struct stat statBuffer;
	fstat(fd, &statBuffer);
	return createFdFileResponse(statBuffer.st_size, fd, mimeType, statBuffer.st_mtime);
}

static int createRapResponse(struct MHD_Connection *request, struct Message * message, struct MHD_Response ** response) {
	// Queue the response
	switch (message->mID) {
	case RAP_MULTISTATUS:
	case RAP_SUCCESS: {
		// Get Mime type and date
		const char * mimeType = iovecToString(&message->buffers[RAP_FILE_INDEX]);
		time_t date = *((time_t *) message->buffers[RAP_DATE_INDEX].iov_base);
		const char * location =
				message->bufferCount > RAP_LOCATION_INDEX ? iovecToString(&message->buffers[RAP_LOCATION_INDEX]) : NULL;

		struct stat stat;
		fstat(message->fd, &stat);
		if (mimeType[0] == '\0') {
			mimeType = NULL;
		}

		if ((stat.st_mode & S_IFMT) == S_IFREG) {
			*response = createFdFileResponse(stat.st_size, message->fd, mimeType, date);
		} else {
			*response = createFdStreamResponse(message->fd, mimeType, date);
		}

		if (location) {
			addHeaderSafe(*response, "Location", location);
		}

		return (message->mID == RAP_SUCCESS ? MHD_HTTP_OK : 207);
	}

	case RAP_ACCESS_DENIED:
		*response = createFileResponse(request, "/usr/share/webdav/HTTP_FORBIDDEN.html", "text/html");
		return MHD_HTTP_FORBIDDEN;

	case RAP_NOT_FOUND:
		*response = createFileResponse(request, "/usr/share/webdav/HTTP_NOT_FOUND.html", "text/html");
		return MHD_HTTP_NOT_FOUND;

	case RAP_BAD_CLIENT_REQUEST:
		*response = createFileResponse(request, "/usr/share/webdav/HTTP_BAD_REQUEST.html", "text/html");
		return MHD_HTTP_BAD_REQUEST;

	default:
		stdLogError(0, "invalid response from RAP %d", (int) message->mID);
		/* no break */

	case RAP_BAD_RAP_REQUEST:
	case RAP_INTERNAL_ERROR:
		return MHD_HTTP_INTERNAL_SERVER_ERROR;
	}

}

///////////////////////////
// End Response Queueing //
///////////////////////////

////////////////////
// RAP Processing //
////////////////////

static sem_t rapDBLock;
static int rapDBSize;
static struct RapGroup * rapDB;

static int forkRapProcess(const char * path, int * newSockFd) {
	// Create unix domain socket for
	int sockFd[2];
	int result = socketpair(PF_LOCAL, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockFd);
	if (result != 0) {
		stdLogError(errno, "Could not create socket pair");
		return 0;
	}

	result = fork();

	if (result) {
		// parent
		close(sockFd[1]);
		if (result != -1) {
			*newSockFd = sockFd[0];
			//stdLog("New RAP %d on %d", result, sockFd[0]);
			return result;
		} else {
			// fork failed so close parent pipes and return non-zero
			close(sockFd[0]);
			stdLogError(errno, "Could not fork");
			return 0;
		}
	} else {
		// child
		// Sort out socket
		//stdLog("Starting rap: %s", path);
		if (dup2(sockFd[1], STDIN_FILENO) == -1 || dup2(sockFd[1], STDOUT_FILENO) == -1) {
			stdLogError(errno, "Could not assign new socket (%d) to stdin/stdout", newSockFd[1]);
			exit(255);
		}
		char * argv[] =
				{ (char *) config.rapBinary, (char *) config.pamServiceName, (char *) config.mimeTypesFile, NULL };
		execv(path, argv);
		stdLogError(errno, "Could not start rap: %s", path);
		exit(255);
	}
}

static void destroyRap(struct RestrictedAccessProcessor * processor) {
	close(processor->socketFd);
	processor->socketFd = -1;
}

static struct RestrictedAccessProcessor * createRap(struct RestrictedAccessProcessor * processor, const char * user,
		const char * password, const char * rhost) {

	processor->pid = forkRapProcess(config.rapBinary, &(processor->socketFd));
	if (!processor->pid) {
		return AUTH_ERROR;
	}

	struct Message message;
	message.mID = RAP_AUTHENTICATE;
	message.fd = -1;
	message.bufferCount = 3;
	message.buffers[RAP_USER_INDEX].iov_len = strlen(user) + 1;
	message.buffers[RAP_USER_INDEX].iov_base = (void *) user;
	message.buffers[RAP_PASSWORD_INDEX].iov_len = strlen(password) + 1;
	message.buffers[RAP_PASSWORD_INDEX].iov_base = (void *) password;
	message.buffers[RAP_RHOST_INDEX].iov_len = strlen(rhost) + 1;
	message.buffers[RAP_RHOST_INDEX].iov_base = (void *) rhost;

	if (sendMessage(processor->socketFd, &message) <= 0) {
		destroyRap(processor);
		return AUTH_ERROR;
	}

	char incomingBuffer[INCOMING_BUFFER_SIZE];
	ssize_t readResult = recvMessage(processor->socketFd, &message, incomingBuffer, INCOMING_BUFFER_SIZE);
	if (readResult <= 0 || message.mID != RAP_SUCCESS) {
		destroyRap(processor);
		if (readResult < 0) {
			stdLogError(0, "Could not read result from RAP ");
			return AUTH_ERROR;
		} else if (readResult == 0) {
			stdLogError(0, "RAP closed socket unexpectedly");
			return AUTH_ERROR;
		} else {
			stdLogError(0, "Access denied for user %s", user);
			return AUTH_FAILED;
		}
	}

	processor->user = user;
	time(&processor->rapCreated);

	return processor;
}

static int compareRapGroup(const void * rapA, const void * rapB) {
	int result = strcmp(((struct RapGroup *) rapA)->user, ((struct RapGroup *) rapB)->user);
	if (result == 0) {
		result = strcmp(((struct RapGroup *) rapA)->password, ((struct RapGroup *) rapB)->password);
	}
	return result;
}

static struct RestrictedAccessProcessor * acquireRapFromDb(const char * user, const char * password,
		int * activeSessions) {
	struct RapGroup groupToFind = { .user = user, .password = password };
	sem_wait(&rapDBLock);
	struct RapGroup *groupFound = bsearch(&groupToFind, rapDB, rapDBSize, sizeof(struct RapGroup), &compareRapGroup);
	struct RestrictedAccessProcessor * rapSessionFound = NULL;
	*activeSessions = 0;
	if (groupFound) {
		time_t expireTime;
		time(&expireTime);
		expireTime -= config.rapMaxSessionLife;
		for (int i = 0; i < config.rapMaxSessionsPerUser; i++) {
			if (groupFound->rapSession[i].socketFd != -1 && !groupFound->rapSession[i].rapSessionInUse
					&& groupFound->rapSession[i].rapCreated >= expireTime) {
				rapSessionFound = &groupFound->rapSession[i];
				groupFound->rapSession[i].rapSessionInUse = 1;
				(*activeSessions)++;
				break;
			} else if (groupFound->rapSession[i].rapSessionInUse) {
				(*activeSessions)++;
			}
		}
	}
	sem_post(&rapDBLock);
	return rapSessionFound;
}

static struct RestrictedAccessProcessor * addRapToDb(struct RestrictedAccessProcessor * rapSession,
		const char * password) {
	struct RestrictedAccessProcessor * newRapSession;
	struct RapGroup groupToFind;
	groupToFind.user = rapSession->user;
	groupToFind.password = password;
	sem_wait(&rapDBLock);
	struct RapGroup *groupFound = bsearch(&groupToFind, rapDB, rapDBSize, sizeof(struct RapGroup), &compareRapGroup);
	if (groupFound) {
		newRapSession = NULL;
		time_t expireTime;
		time(&expireTime);
		expireTime -= config.rapMaxSessionLife;
		for (int i = 0; i < config.rapMaxSessionsPerUser; i++) {
			if (groupFound->rapSession[i].socketFd == -1) {
				newRapSession = &groupFound->rapSession[i];
				break;
			} else if (groupFound->rapSession[i].rapCreated < expireTime
					&& !groupFound->rapSession[i].rapSessionInUse) {
				destroyRap(&groupFound->rapSession[i]);
				newRapSession = &groupFound->rapSession[i];
			}
		}
		if (!newRapSession) {
			destroyRap(rapSession);
			sem_post(&rapDBLock);
			return AUTH_BACKOFF;
		}
	} else {
		rapDBSize++;
		rapDB = reallocSafe(rapDB, rapDBSize * sizeof(struct RapGroup));
		groupFound = &rapDB[rapDBSize - 1];
		size_t userSize = strlen(groupToFind.user) + 1;
		size_t passwordSize = strlen(groupToFind.password) + 1;
		size_t bufferSize = userSize + passwordSize;
		char * buffer = mallocSafe(bufferSize);
		memcpy(buffer, groupToFind.user, userSize);
		memcpy(buffer + userSize, groupToFind.password, passwordSize);
		groupFound->user = buffer;
		groupFound->password = buffer + userSize;
		groupFound->rapSession = mallocSafe(sizeof(struct RestrictedAccessProcessor) * config.rapMaxSessionsPerUser);
		memset(groupFound->rapSession, 0, sizeof(struct RestrictedAccessProcessor) * config.rapMaxSessionsPerUser);
		for (int i = 1; i < config.rapMaxSessionsPerUser; i++) {
			groupFound->rapSession[i].socketFd = -1;
		}
		newRapSession = &groupFound->rapSession[0];

		for (int i = 1; i < config.rapMaxSessionsPerUser; i++) {

		}
		qsort(rapDB, rapDBSize, sizeof(struct RapGroup), &compareRapGroup);
	}
	*newRapSession = *rapSession;
	newRapSession->user = groupFound->user;
	newRapSession->rapSessionInUse = 1;
	sem_post(&rapDBLock);
	return newRapSession;
}

static void releaseRap(struct RestrictedAccessProcessor * processor) {
	processor->rapSessionInUse = 0;
}

static struct RestrictedAccessProcessor * acquireRap(struct MHD_Connection *request) {
	char * user;
	char * password;
	user = MHD_basic_auth_get_username_password(request, &password);
	if (user && password) {
		int sessionCount;
		struct RestrictedAccessProcessor * rapSession = acquireRapFromDb(user, password, &sessionCount);
		if (rapSession) {
			return rapSession;
		} else {
			if (sessionCount < config.rapMaxSessionsPerUser) {
				char rhost[100];
				getRequestIP(rhost, sizeof(rhost), request);

				struct RestrictedAccessProcessor newSession;
				rapSession = createRap(&newSession, user, password, rhost);
				if (rapSession != &newSession) {
					return rapSession;
				} else {
					return addRapToDb(rapSession, password);
				}
			} else {
				return AUTH_BACKOFF;
			}
		}
	} else {
		stdLogError(0, "Rejecting request without auth");
		return AUTH_FAILED;
	}
}

static void cleanupAfterRap(int sig, siginfo_t *siginfo, void *context) {
	int status;
	waitpid(siginfo->si_pid, &status, 0);
	if (status == 139) {
		stdLogError(0, "RAP %d failed with segmentation fault", siginfo->si_pid);
	}
	//stdLog("Child finished PID: %d staus: %d", siginfo->si_pid, status);
}

static void * rapTimeoutWorker(void * ignored) {
	// TODO actually free() something
	while (1) {
		sleep(config.rapMaxSessionLife / 2);
		time_t expireTime;
		time(&expireTime);
		expireTime -= config.rapMaxSessionLife;
		sem_wait(&rapDBLock);
		for (int group = 0; group < rapDBSize; group++) {
			for (int rap = 0; rap < config.rapMaxSessionsPerUser; rap++) {
				if (!rapDB[group].rapSession[rap].rapSessionInUse && rapDB[group].rapSession[rap].socketFd != -1
						&& rapDB[group].rapSession[rap].rapCreated < expireTime) {
					destroyRap(&rapDB[group].rapSession[rap]);
				}
			}
		}
		sem_post(&rapDBLock);
	}
	return NULL;
}

////////////////////////
// End RAP Processing //
////////////////////////

///////////////////////////////////////
// Low Level HTTP handling (Signpost //
///////////////////////////////////////

static int filterGetHeader(struct Header * header, enum MHD_ValueKind kind, const char *key, const char *value) {
	if (!strcmp(key, header->key)) {
		header->value = value;
		return MHD_NO;
	}
	return MHD_YES;
}

static const char * getHeader(struct MHD_Connection *request, const char * headerKey) {
	struct Header header = { .key = headerKey, .value = NULL };
	MHD_get_connection_values(request, MHD_HEADER_KIND, (MHD_KeyValueIterator) &filterGetHeader, &header);
	return header.value;
}

static int completeUpload(struct MHD_Connection *request, struct RestrictedAccessProcessor * processor,
		struct MHD_Response ** response) {

	if (processor->writeDataFd == -1) {
		*response = createFileResponse(request, "/usr/share/webdav/HTTP_INSUFFICIENT_STORAGE.html", "text/html");
		return MHD_HTTP_INSUFFICIENT_STORAGE;
	} else {
		// Closing this pipe signals to the rap that there is no more data
		// This MUST happen before the recvMessage a few lines below or the RAP
		// will NOT send a message and recvMessage will hang.
		close(processor->writeDataFd);
		processor->writeDataFd = -1;
		struct Message message;
		char incomingBuffer[INCOMING_BUFFER_SIZE];
		int readResult = recvMessage(processor->socketFd, &message, incomingBuffer, INCOMING_BUFFER_SIZE);
		if (readResult <= 0) {
			if (readResult == 0) {
				stdLogError(0, "RAP closed socket unexpectedly while waiting for response");
			}
			return MHD_HTTP_INTERNAL_SERVER_ERROR;
		}

		if (readResult > 0) {
			return createRapResponse(request, &message, response);
		} else {
			return MHD_HTTP_INTERNAL_SERVER_ERROR;
		}
	}
}

static void processUploadData(struct MHD_Connection * request, const char * upload_data, size_t upload_data_size,
		struct RestrictedAccessProcessor * processor) {

	if (processor->writeDataFd != -1) {
		size_t bytesWritten = write(processor->writeDataFd, upload_data, upload_data_size);
		if (bytesWritten < upload_data_size) {
			// not all data could be written to the file handle and therefore
			// the operation has now failed. There's nothing we can do now but report the error
			// This may not actually be desirable and so we need to consider slamming closed the connection.
			close(processor->writeDataFd);
			processor->writeDataFd = -1;
		}
	}
}

static int processNewRequest(struct MHD_Connection * request, const char * url, const char * host, const char * method,
		struct RestrictedAccessProcessor * rapSession, struct MHD_Response ** response) {

	// Interpret the method
	struct Message message;
	message.fd = rapSession->readDataFd;
	message.buffers[RAP_HOST_INDEX].iov_len = strlen(host) + 1;
	message.buffers[RAP_HOST_INDEX].iov_base = (void *) host;
	message.buffers[RAP_FILE_INDEX].iov_len = strlen(url) + 1;
	message.buffers[RAP_FILE_INDEX].iov_base = (void *) url;
	// TODO PUT
	// TODO PROPPATCH
	// TODO MKCOL
	// TODO HEAD
	// TODO DELETE
	// TODO COPY
	// TODO MOVE
	// TODO LOCK
	// TODO UNLOCK
	//stdLog("%s %s data", method, writeHandle ? "with" : "without");
	if (!strcmp("GET", method)) {
		message.mID = RAP_READ_FILE;
		message.bufferCount = 2;
	} else if (!strcmp("PROPFIND", method)) {
		message.mID = RAP_PROPFIND;
		const char * depth = getHeader(request, "Depth");
		if (depth) {
			message.buffers[RAP_DEPTH_INDEX].iov_base = (void *) depth;
			message.buffers[RAP_DEPTH_INDEX].iov_len = strlen(depth) + 1;
		} else {
			message.buffers[RAP_DEPTH_INDEX].iov_base = "infinity";
			message.buffers[RAP_DEPTH_INDEX].iov_len = sizeof("infinity");
		}
		message.bufferCount = 3;
	} else if (!strcmp("OPTIONS", method)) {
		*response = createFileResponse(request, "/usr/share/webdav/OPTIONS.html", "text/html");
		addHeaderSafe(*response, "Accept", ACCEPT_HEADER);
		return MHD_HTTP_OK;
	} else {
		stdLogError(0, "Can not cope with method: %s (%s data)", method,
				(rapSession->writeDataFd != -1 ? "with" : "without"));
		return MHD_HTTP_METHOD_NOT_ALLOWED;
	}

	// Send the request to the RAP
	size_t ioResult = sendMessage(rapSession->socketFd, &message);
	rapSession->readDataFd = -1; // this will always be closed by sendMessage even on failure!
	if (ioResult <= 0) {
		return MHD_HTTP_INTERNAL_SERVER_ERROR;
	}

	// Get result from RAP
	char incomingBuffer[INCOMING_BUFFER_SIZE];
	ioResult = recvMessage(rapSession->socketFd, &message, incomingBuffer, INCOMING_BUFFER_SIZE);
	if (ioResult <= 0) {
		if (ioResult == 0) {
			stdLogError(0, "RAP closed socket unexpectedly while waiting for response");
		}
		return MHD_HTTP_INTERNAL_SERVER_ERROR;
	}

	if (message.mID == RAP_CONTINUE) {
		return MHD_HTTP_CONTINUE;
	} else {
		return createRapResponse(request, &message, response);
	}
}

static int requestHasData(struct MHD_Connection *request) {
	if (getHeader(request, "Content-Length")) {
		return 1;
	} else {
		const char * te = getHeader(request, "Transfer-Encoding");
		return te && !strcmp(te, "chunked");
	}
}

static int sendResponse(struct MHD_Connection *request, int statusCode, struct MHD_Response * response,
		struct RestrictedAccessProcessor * rapSession, const char * method, const char * url) {

	// This doesn't really belong here but its a good safty check. We should never try to send a response
	// when the data pipes are still open
	if (rapSession->readDataFd != -1) {
		stdLogError(0, "readDataFd was not properly closed before sending response");
		close(rapSession->readDataFd);
		rapSession->readDataFd = -1;
	}
	if (rapSession->writeDataFd != -1) {
		stdLogError(0, "writeDataFd was not properly closed before sending response");
		close(rapSession->writeDataFd);
		rapSession->writeDataFd = -1;
	}

	char clientIp[100];
	getRequestIP(clientIp, sizeof(clientIp), request);
	logAccess(statusCode, method, rapSession->user, url, clientIp);
	switch (statusCode) {
	case MHD_HTTP_INTERNAL_SERVER_ERROR:
		return MHD_queue_response(request, MHD_HTTP_INTERNAL_SERVER_ERROR, INTERNAL_SERVER_ERROR_PAGE);
	case MHD_HTTP_UNAUTHORIZED:
		return MHD_queue_response(request, MHD_HTTP_UNAUTHORIZED, UNAUTHORIZED_PAGE);
	case MHD_HTTP_METHOD_NOT_ALLOWED:
		return MHD_queue_response(request, MHD_HTTP_METHOD_NOT_ALLOWED, METHOD_NOT_SUPPORTED_PAGE);
	default: {
		int queueResult = MHD_queue_response(request, statusCode, response);
		MHD_destroy_response(response);
		return queueResult;
	}
	}
}

static int answerToRequest(void *cls, struct MHD_Connection *request, const char *url, const char *method,
		const char *version, const char *upload_data, size_t *upload_data_size, void ** s) {

	struct RestrictedAccessProcessor ** rapSession = (struct RestrictedAccessProcessor **) s;

	if (*rapSession) {
		if (*upload_data_size) {
			// Finished uploading data
			if (!(*rapSession)->responseAlreadyGiven) {
				processUploadData(request, upload_data, *upload_data_size, *rapSession);
			}
			*upload_data_size = 0;
			return MHD_YES;
		} else {
			// Uploading more data
			if ((*rapSession)->responseAlreadyGiven) {
				releaseRap(*rapSession);
				return MHD_YES;
			} else {
				struct MHD_Response * response;
				int statusCode = completeUpload(request, *rapSession, &response);
				int result = sendResponse(request, statusCode, response, *rapSession, method, url);
				if (*rapSession != AUTH_ERROR && *rapSession != AUTH_FAILED) {
					releaseRap(*rapSession);
				}
				return result;
			}
		}
	} else {
		const char * host = getHeader(request, "Host");
		if (host == NULL) {
			// TODO something more meaningful here.
			host = "";
		}

		// Authenticate all new requests regardless of anything else
		*rapSession = acquireRap(request);
		if (*rapSession == AUTH_FAILED || *rapSession == AUTH_BACKOFF) {
			return sendResponse(request, MHD_HTTP_UNAUTHORIZED, NULL, *rapSession, method, url);
		} else if (*rapSession == AUTH_ERROR) {
			return sendResponse(request, MHD_HTTP_INTERNAL_SERVER_ERROR, NULL, *rapSession, method, url);
		} else {
			if (requestHasData(request)) {
				// If we have data to send then create a pipe to pump it through
				int pipeEnds[2];
				if (pipe(pipeEnds)) {
					stdLogError(errno, "Could not create write pipe");
					return sendResponse(request, MHD_HTTP_INTERNAL_SERVER_ERROR, NULL, *rapSession, method, url);
				}
				(*rapSession)->readDataFd = pipeEnds[PIPE_READ];
				(*rapSession)->writeDataFd = pipeEnds[PIPE_WRITE];
				struct MHD_Response * response;

				int statusCode = processNewRequest(request, url, host, method, *rapSession, &response);

				if (statusCode == MHD_HTTP_CONTINUE) {
					// do not queue a response for contiune
					(*rapSession)->responseAlreadyGiven = 0;
					//logAccess(statusCode, method, (*rapSession)->user, url);
					return MHD_YES;
				} else {
					(*rapSession)->responseAlreadyGiven = 1;
					return sendResponse(request, statusCode, response, *rapSession, method, url);
				}
			} else {
				(*rapSession)->readDataFd = -1;
				(*rapSession)->writeDataFd = -1;
				struct MHD_Response * response;

				int statusCode = processNewRequest(request, url, host, method, *rapSession, &response);

				if (statusCode == MHD_HTTP_CONTINUE) {
					stdLogError(0, "RAP returned CONTINUE when there is no data");
					int ret = sendResponse(request, MHD_HTTP_INTERNAL_SERVER_ERROR, NULL, *rapSession, method, url);
					releaseRap(*rapSession);
					return ret;
				} else {
					int ret = sendResponse(request, statusCode, response, *rapSession, method, url);
					releaseRap(*rapSession);
					return ret;
				}
			}
		}
	}
}

///////////////////////////////////////////
// End Low Level HTTP handling (Signpost //
///////////////////////////////////////////

////////////////////
// Initialisation //
////////////////////

static void initializeStaticResponse(struct MHD_Response ** response, const char * fileName, const char * mimeType) {
	size_t bufferSize;
	char * buffer;

	buffer = loadFileToBuffer(fileName, &bufferSize);
	if (buffer == NULL) {
		exit(1);
	}
	*response = MHD_create_response_from_buffer(bufferSize, buffer, MHD_RESPMEM_MUST_FREE);
	if (!*response) {
		stdLogError(errno, "Could not create response buffer");
		exit(255);
	}

	if (mimeType) {
		addHeaderSafe(*response, "Content-Type", mimeType);
	}
}

static void initializeStaticResponses() {
	initializeStaticResponse(&INTERNAL_SERVER_ERROR_PAGE, "/usr/share/webdav/HTTP_INTERNAL_SERVER_ERROR.html", "text/html");
	initializeStaticResponse(&UNAUTHORIZED_PAGE, "/usr/share/webdav/HTTP_UNAUTHORIZED.html", "text/html");
	addHeaderSafe(UNAUTHORIZED_PAGE, "WWW-Authenticate", "Basic realm=\"My Server\"");
	initializeStaticResponse(&METHOD_NOT_SUPPORTED_PAGE, "/usr/share/webdav/HTTP_METHOD_NOT_SUPPORTED.html", "text/html");
	addHeaderSafe(METHOD_NOT_SUPPORTED_PAGE, "Allow",
			"OPTIONS, GET, HEAD, DELETE, PROPFIND, PUT, PROPPATCH, COPY, MOVE, LOCK, UNLOCK");
}

static void initializeRapDatabase() {
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_sigaction = &cleanupAfterRap;
	act.sa_flags = SA_SIGINFO;
	if (sigaction(SIGCHLD, &act, NULL) < 0) {
		stdLogError(errno, "Could not set handler method for finished child threads");
		exit(255);
	}

	sem_init(&rapDBLock, 0, 1);

	rapDBSize = 0;
	rapDB = NULL;

	pthread_t newThread;
	if (pthread_create(&newThread, NULL, &rapTimeoutWorker, NULL)) {
		stdLogError(errno, "Could not create worker thread for rap db");
		exit(255);
	}
}

////////////////////////
// End Initialisation //
////////////////////////

///////////////////
// Configuration //
///////////////////

#define CONFIG_NAMESPACE "http://couling.me/webdavd"

static int configureServer(xmlTextReaderPtr reader, const char * configFile, struct WebdavdConfiguration * config) {
	config->restrictedUser = NULL;
	config->daemonCount = 0;
	config->daemons = NULL;
	config->rapMaxSessionLife = 60 * 5;
	config->rapMaxSessionsPerUser = 10;
	config->rapBinary = NULL;
	config->pamServiceName = NULL;
	config->mimeTypesFile = NULL;
	config->accessLog = NULL;
	config->errorLog = NULL;
	config->sslCertCount = 0;
	config->sslCerts = NULL;

	int result = stepInto(reader);
	while (result && xmlTextReaderDepth(reader) == 2) {
		if (xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT
				&& !strcmp(xmlTextReaderConstNamespaceUri(reader), CONFIG_NAMESPACE)) {

			//<listen><port>80</port><host>localhost</host><encryption>disabled</encryption></listen>
			if (!strcmp(xmlTextReaderConstLocalName(reader), "listen")) {
				int index = config->daemonCount++;
				config->daemons = reallocSafe(config->daemons, sizeof(*config->daemons) * config->daemonCount);
				config->daemons[index].host = NULL;
				config->daemons[index].sslEnabled = 0;
				config->daemons[index].port = -1;
				result = stepInto(reader);
				while (result && xmlTextReaderDepth(reader) == 3) {
					if (xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT
							&& !strcmp(xmlTextReaderConstNamespaceUri(reader), CONFIG_NAMESPACE)) {
						if (!strcmp(xmlTextReaderConstLocalName(reader), "port")) {
							if (config->daemons[index].port != -1) {
								stdLogError(0, "port specified for listen more than once int %s", configFile);
								exit(1);
							}
							const char * portString;
							result = stepOverText(reader, &portString);
							if (portString != NULL) {
								char * endP;
								long int parsedPort = strtol(portString, &endP, 10);
								if (!*endP && parsedPort >= 0 && parsedPort <= 0xFFFF) {
									config->daemons[index].port = parsedPort;
								} else {
									stdLogError(0, "%s is not a valid port in %s", portString, configFile);
									exit(1);
								}
							}
						} else if (!strcmp(xmlTextReaderConstLocalName(reader), "host")) {
							if (config->daemons[index].host != NULL) {
								stdLogError(0, "host specified for listen more than once int %s", configFile);
								exit(1);
							}
							const char * hostString;
							result = stepOverText(reader, &hostString);
							config->daemons[index].host = copyString(hostString);
						} else if (!strcmp(xmlTextReaderConstLocalName(reader), "encryption")) {
							const char * encryptionString;
							result = stepOverText(reader, &encryptionString);
							if (encryptionString) {
								if (!strcmp(encryptionString, "none")) {
									config->daemons[index].sslEnabled = 0;
								} else if (!strcmp(encryptionString, "ssl")) {
									config->daemons[index].sslEnabled = 1;
								} else {
									stdLogError(0, "invalid encryption method %s in %s", encryptionString, configFile);
									exit(1);
								}
							}
						}
					} else {
						result = stepOver(reader);
					}
				}
				if (config->daemons[index].port == -1) {
					stdLogError(0, "port not specified for listen in %s", configFile);
					exit(1);
				}
			}

			//<session-timeout>5:00</session-timeout>
			else if (!strcmp(xmlTextReaderConstLocalName(reader), "session-timeout")) {
				const char * sessionTimeoutString;
				result = stepOverText(reader, &sessionTimeoutString);
				if (sessionTimeoutString) {
					long int hour = 0, minute = 0, second;
					char * endPtr;
					second = strtol(sessionTimeoutString, &endPtr, 10);
					if (*endPtr) {
						if (*endPtr != ':' || endPtr == sessionTimeoutString) {
							stdLogError(0, "Invalid session timeout length %s in %s", sessionTimeoutString, configFile);
							exit(1);
						}
						minute = second;

						char * endPtr2;
						endPtr++;
						second = strtol(endPtr, &endPtr2, 10);
						if (*endPtr2) {
							if (*endPtr2 != ':' || endPtr2 == endPtr) {
								stdLogError(0, "Invalid session timeout length %s in %s", sessionTimeoutString,
										configFile);
								exit(1);
							}
							hour = minute;
							minute = second;
							endPtr2++;
							second = strtol(endPtr2, &endPtr, 10);
							if (*endPtr != '\0') {
								stdLogError(0, "Invalid session timeout length %s in %s", sessionTimeoutString,
										configFile);
								exit(1);
							}
						}
					}
					config->rapMaxSessionLife = (((hour * 60) + minute) * 60) + second;
				}
			}

			//<max-user-sessions>10</max-user-sessions>
			else if (!strcmp(xmlTextReaderConstLocalName(reader), "max-user-sessions")) {
				const char * sessionCountString;
				result = stepOverText(reader, &sessionCountString);
				if (sessionCountString) {
					char * endPtr;
					long int maxUserSessions = strtol(sessionCountString, &endPtr, 10);
					if (*endPtr || maxUserSessions < 0 || maxUserSessions > 0xFFFFFFF) {
						stdLogError(0, "Invalid max-user-sessions %s in %s", maxUserSessions, configFile);
						exit(1);
					}
					config->rapMaxSessionsPerUser = maxUserSessions;
				}
			}

			//<restricted>nobody</restricted>
			else if (!strcmp(xmlTextReaderConstLocalName(reader), "restricted")) {
				if (config->restrictedUser) {
					stdLogError(0, "restricted-user specified more than once in %s", configFile);
					exit(1);
				}
				const char * restrictedUser;
				result = stepOverText(reader, &restrictedUser);
				config->restrictedUser = copyString(restrictedUser);
			}

			//<mime-file>/etc/mime.types</mime-file>
			else if (!strcmp(xmlTextReaderConstLocalName(reader), "mime-file")) {
				if (config->mimeTypesFile) {
					stdLogError(0, "restricted-user specified more than once in %s", configFile);
					exit(1);
				}
				const char * mimeTypesFile;
				result = stepOverText(reader, &mimeTypesFile);
				config->mimeTypesFile = copyString(mimeTypesFile);
			}

			//<rap-binary>/usr/sbin/rap</rap-binary>
			else if (!strcmp(xmlTextReaderConstLocalName(reader), "rap-binary")) {
				if (config->rapBinary) {
					stdLogError(0, "restricted-user specified more than once in %s", configFile);
					exit(1);
				}
				const char * rapBinary;
				result = stepOverText(reader, &rapBinary);
				config->rapBinary = copyString(rapBinary);
			}

			//<static-error-dir>/usr/share/webdavd</static-error-dir>
			// TODO <static-error-dir>/usr/share/webdavd</static-error-dir>

			//<pam-service>webdavd</pam-service>
			else if (!strcmp(xmlTextReaderConstLocalName(reader), "pam-service")) {
				if (config->pamServiceName) {
					stdLogError(0, "restricted-user specified more than once in %s", configFile);
					exit(1);
				}
				const char * pamServiceName;
				result = stepOverText(reader, &pamServiceName);
				config->pamServiceName = copyString(pamServiceName);
			}

			//<access-log>/var/log/access.log</access-log>
			else if (!strcmp(xmlTextReaderConstLocalName(reader), "access-log")) {
				if (config->accessLog) {
					stdLogError(0, "restricted-user specified more than once in %s", configFile);
					exit(1);
				}
				const char * accessLog;
				result = stepOverText(reader, &accessLog);
				config->accessLog = copyString(accessLog);
			}

			//<error-log>/var/log/error.log</error-log>
			else if (!strcmp(xmlTextReaderConstLocalName(reader), "error-log")) {
				if (config->errorLog) {
					stdLogError(0, "restricted-user specified more than once in %s", configFile);
					exit(1);
				}
				const char * errorLog;
				result = stepOverText(reader, &errorLog);
				config->errorLog = copyString(errorLog);
			}

			//<ssl-cert>...</ssl-cert>
			else if (!strcmp(xmlTextReaderConstLocalName(reader), "ssl-cert")) {
				int index = config->sslCertCount++;
				config->sslCerts = reallocSafe(config->sslCerts, sizeof(*config->sslCerts) * config->sslCertCount);
				config->sslCerts[index].certificateFile = NULL;
				config->sslCerts[index].chainFileCount = 0;
				config->sslCerts[index].chainFiles = NULL;
				config->sslCerts[index].keyFile = NULL;
				result = stepInto(reader);
				while (result && xmlTextReaderDepth(reader) == 3) {
					if (xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT
							&& !strcmp(xmlTextReaderConstNamespaceUri(reader), CONFIG_NAMESPACE)) {
						if (!strcmp(xmlTextReaderConstLocalName(reader), "certificate")) {
							if (config->sslCerts[index].certificateFile) {
								stdLogError(0, "more than one certificate specified in ssl-cert %s", configFile);
								exit(1);
							}
							const char * certificate;
							result = stepOverText(reader, &certificate);
							config->sslCerts[index].certificateFile = copyString(certificate);
						} else if (!strcmp(xmlTextReaderConstLocalName(reader), "key")) {
							if (config->sslCerts[index].keyFile) {
								stdLogError(0, "more than one key specified in ssl-cert %s", configFile);
								exit(1);
							}
							const char * keyFile;
							result = stepOverText(reader, &keyFile);
							config->sslCerts[index].keyFile = copyString(keyFile);
						} else if (!strcmp(xmlTextReaderConstLocalName(reader), "chain")) {
							const char * chainFile;
							result = stepOverText(reader, &chainFile);
							if (chainFile) {
								int chainFileIndex = config->sslCerts[index].chainFileCount++;
								config->sslCerts[index].chainFiles = reallocSafe(config->sslCerts[index].chainFiles,
										config->sslCerts[index].chainFileCount
												* sizeof(*config->sslCerts[index].chainFiles));
								config->sslCerts[index].chainFiles[chainFileIndex] = copyString(chainFile);
							}
						} else {
							result = stepOver(reader);
						}
					} else {
						result = stepOver(reader);
					}
				}
				if (!config->sslCerts[index].certificateFile) {
					stdLogError(0, "certificate not specified in ssl-cert in %s", configFile);
				}
				if (!config->sslCerts[index].keyFile) {
					stdLogError(0, "key not specified in ssl-cert in %s", configFile);
				}
			} else {
				result = stepOver(reader);
			}

		} else {
			result = stepOver(reader);
		}
	}

	if (!config->rapBinary) {
		config->rapBinary = "/usr/sbin/rap";
	}
	if (!config->mimeTypesFile) {
		config->mimeTypesFile = "/etc/mime.types";
	}
	if (!config->accessLog) {
		config->accessLog = "/var/log/webdavd-access.log";
	}
	if (!config->errorLog) {
		config->errorLog = "/var/log/webdavd-error.log";
	}
	if (!config->pamServiceName) {
		config->pamServiceName = "webdav";
	}

	return result;
}

void configure(const char * configFile) {
	xmlTextReaderPtr reader = xmlReaderForFile(configFile, NULL, XML_PARSE_NOENT);
	suppressReaderErrors(reader);
	if (!reader || !stepInto(reader)) {
		stdLogError(0, "could not create xml reader for %s", configFile);
		exit(1);
	}
	if (!elementMatches(reader, CONFIG_NAMESPACE, "server-config")) {
		stdLogError(0, "root node is not server-config in namespace %s %s", CONFIG_NAMESPACE, configFile);
		exit(1);
	}

	int result = stepInto(reader);

	while (result && xmlTextReaderDepth(reader) == 1) {
		if (elementMatches(reader, CONFIG_NAMESPACE, "server")) {
			result = configureServer(reader, configFile, &config);
			break;
		} else {
			stdLog("Warning: skipping %s:%s in %s", xmlTextReaderConstNamespaceUri(reader),
					xmlTextReaderConstLocalName(reader), configFile);
			result = stepOver(reader);
		}
	}

	xmlFreeTextReader(reader);
}

///////////////////////
// End Configuration //
///////////////////////

//////////
// Main //
//////////

// All Daemons
// Not sure why we keep these, they're not used for anything
static struct MHD_Daemon **daemons;

int main(int argCount, char ** args) {
	if (argCount > 1) {
		for (int i = 1; i < argCount; i++) {
			configure(args[i]);
		}
	} else {
		configure("/etc/webdavd");
	}

	initializeLogs();
	initializeStaticResponses();
	initializeRapDatabase();
	initializeSSL();

	// Start up the daemons
	daemons = mallocSafe(sizeof(*daemons) * config.daemonCount);
	for (int i = 0; i < config.daemonCount; i++) {
		// TODO bind to specific ip
		if (config.daemons[i].sslEnabled) {
			daemons[i] = MHD_start_daemon(
					MHD_USE_SELECT_INTERNALLY | MHD_USE_DUAL_STACK | MHD_USE_SSL | MHD_USE_PEDANTIC_CHECKS,
					config.daemons[i].port, NULL, NULL, &answerToRequest, NULL, MHD_OPTION_HTTPS_CERT_CALLBACK,
					&sslSNICallback, MHD_OPTION_END);
		} else {
			daemons[i] = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY | MHD_USE_DUAL_STACK | MHD_USE_PEDANTIC_CHECKS,
					config.daemons[i].port,
					NULL, NULL, &answerToRequest, NULL, MHD_OPTION_END);
		}
		if (!daemons[i]) {
			stdLogError(errno, "Unable to initialise daemon on port %d", config.daemons[i].port);
			exit(255);
		}
	}

	pthread_exit(NULL);
}

//////////////
// End Main //
//////////////
