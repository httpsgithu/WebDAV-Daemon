#include "shared.h"

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <security/pam_appl.h>
#include <libxml/xmlwriter.h>

#define BUFFER_SIZE 4096

static int authenticated = 0;
static const char * authenticatedUser;
static const char * pamService;

static size_t respond(enum RapConstant result, int fd) {
	struct Message message = { .mID = result, .fd = fd, .bufferCount = 0 };
	return sendMessage(STDOUT_FILENO, &message);
}

//////////
// Mime //
//////////

struct MimeType {
	const char * ext;
	const char * type;
	size_t typeStringSize;
};

// Mime Database.
static size_t mimeFileBufferSize;
static char * mimeFileBuffer;
static struct MimeType * mimeTypes = NULL;
static int mimeTypeCount = 0;
static struct MimeType UNKNOWN_MIME_TYPE = { .ext = "", .type = "application/octet-stream", .typeStringSize =
		sizeof("application/octet-stream") };
static struct MimeType XML_MIME_TYPE = { .ext = "", .type = "application/xml; charset=utf-8", .typeStringSize =
		sizeof("application/xml; charset=utf-8") };

static int compareExt(const void * a, const void * b) {
	return strcmp(((struct MimeType *) a)->ext, ((struct MimeType *) b)->ext);
}

static struct MimeType * findMimeType(const char * file) {

	if (!file) {
		return &UNKNOWN_MIME_TYPE;
	}
	struct MimeType type;
	type.ext = file + strlen(file) - 1;
	while (1) {
		if (*type.ext == '/') {
			return &UNKNOWN_MIME_TYPE;
		} else if (*type.ext == '.') {
			type.ext++;
			break;
		} else {
			type.ext--;
			if (type.ext < file) {
				return &UNKNOWN_MIME_TYPE;
			}
		}
	}

	struct MimeType * result = bsearch(&type, mimeTypes, mimeTypeCount, sizeof(struct MimeType), &compareExt);
	return result ? result : &UNKNOWN_MIME_TYPE;
}

static void initializeMimeTypes(const char * mimeTypesFile) {
	// Load Mime file into memory
	mimeFileBuffer = loadFileToBuffer(mimeTypesFile, &mimeFileBufferSize);
	if (!mimeFileBuffer) {
		exit(1);
	}

	// Parse mimeFile;
	char * partStartPtr = mimeFileBuffer;
	int found;
	char * type = NULL;
	do {
		found = 0;
		// find the start of the part
		while (partStartPtr < mimeFileBuffer + mimeFileBufferSize && !found) {
			switch (*partStartPtr) {
			case '#':
				// skip to the end of the line
				while (partStartPtr < mimeFileBuffer + mimeFileBufferSize && *partStartPtr != '\n') {
					partStartPtr++;
				}
				// Fall through to incrementing partStartPtr
				partStartPtr++;
				break;
			case ' ':
			case '\t':
			case '\r':
			case '\n':
				if (*partStartPtr == '\n') {
					type = NULL;
				}
				partStartPtr++;
				break;
			default:
				found = 1;
				break;
			}
		}

		// Find the end of the part
		char * partEndPtr = partStartPtr + 1;
		found = 0;
		while (partEndPtr < mimeFileBuffer + mimeFileBufferSize && !found) {
			switch (*partEndPtr) {
			case ' ':
			case '\t':
			case '\r':
			case '\n':
				if (type == NULL) {
					type = partStartPtr;
				} else {
					mimeTypes = reallocSafe(mimeTypes, sizeof(struct MimeType) * (mimeTypeCount + 1));
					mimeTypes[mimeTypeCount].type = type;
					mimeTypes[mimeTypeCount].ext = partStartPtr;
					mimeTypes[mimeTypeCount].typeStringSize = partEndPtr - partStartPtr + 1;
					mimeTypeCount++;
				}
				if (*partEndPtr == '\n') {
					type = NULL;
				}
				*partEndPtr = '\0';
				found = 1;
				break;
			default:
				partEndPtr++;
				break;
			}
		}
		partStartPtr = partEndPtr + 1;
	} while (partStartPtr < mimeFileBuffer + mimeFileBufferSize);

	qsort(mimeTypes, mimeTypeCount, sizeof(struct MimeType), &compareExt);
}

//////////////
// End Mime //
//////////////

/////////////////////
// XML Text Writer //
/////////////////////
static int xmlFdOutputCloseCallback(void * context) {
	close(*((int *) context));
	free(context);
	return 0;
}

static int xmlFdOutputWriteCallback(void * context, const char * buffer, int len) {
	ssize_t ignored = write(*((int *) context), buffer, len);
	return ignored;
}

static xmlTextWriterPtr xmlNewFdTextWriter(int out) {
	xmlOutputBufferPtr outStruct = xmlAllocOutputBuffer(NULL);
	outStruct->writecallback = &xmlFdOutputWriteCallback;
	outStruct->closecallback = &xmlFdOutputCloseCallback;
	outStruct->context = mallocSafe(sizeof(int));
	*((int *) outStruct->context) = out;
	return xmlNewTextWriter(outStruct);
}

static int xmlTextWriterWriteElementString(xmlTextWriterPtr writer, const char * elementName, const char * string) {
	int ret;
	(ret = xmlTextWriterStartElementNS(writer, "d", elementName, NULL)) < 0
			|| (ret = xmlTextWriterWriteString(writer, string)) < 0 || (ret = xmlTextWriterEndElement(writer));

	return ret;
}

/////////////////////////
// End XML Text Writer //
/////////////////////////

//////////////
// PROPFIND //
//////////////

#define PROPFIND_RESOURCE_TYPE "resourcetype"
#define PROPFIND_CREATION_DATE "creationdate"
#define PROPFIND_CONTENT_LENGTH "getcontentlength"
#define PROPFIND_LAST_MODIFIED "getlastmodified"
#define PROPFIND_DISPLAY_NAME "displayname"
#define PROPFIND_CONTENT_TYPE "getcontenttype"
#define PROPFIND_USED_BYTES "quota-used-bytes"
#define PROPFIND_AVAILABLE_BYTES "quota-available-bytes"
#define PROPFIND_ETAG "getetag"

struct PropertySet {
	char creationDate;
	char displayName;
	char contentLength;
	char contentType;
	char etag;
	char lastModified;
	char resourceType;
	char usedBytes;
	char availableBytes;
};

static int parsePropFind(int fd, struct PropertySet * properties) {
	xmlTextReaderPtr reader = xmlReaderForFd(fd, NULL, NULL, XML_PARSE_NOENT);
	suppressReaderErrors(reader);

	int readResult;
	if (!reader || !stepInto(reader)) {
		stdLogError(0, "could not create xml reader");
		close(fd);
		return 0;
	}

	if (xmlTextReaderNodeType(reader) == XML_READER_TYPE_NONE) {
		// No body has been sent
		// so assume the client is asking for everything.
		memset(properties, 1, sizeof(*properties));
		goto CLEANUP;
	} else {
		memset(properties, 0, sizeof(struct PropertySet));
	}

	if (!elementMatches(reader, "DAV:", "propfind")) {
		stdLogError(0, "Request body was not a propfind document");
		readResult = 0;
		goto CLEANUP;
	}

	readResult = stepInto(reader);
	while (readResult && xmlTextReaderDepth(reader) > 0 && !elementMatches(reader, "DAV:", "prop")) {
		stepOver(reader);
	}

	if (!readResult) {
		goto CLEANUP;
	}

	readResult = stepInto(reader);
	while (readResult && xmlTextReaderDepth(reader) > 1) {
		if (!strcmp(xmlTextReaderConstNamespaceUri(reader), "DAV:")) {
			const char * nodeName = xmlTextReaderConstLocalName(reader);
			if (!strcmp(nodeName, PROPFIND_RESOURCE_TYPE)) {
				properties->resourceType = 1;
			} else if (!strcmp(nodeName, PROPFIND_CREATION_DATE)) {
				properties->creationDate = 1;
			} else if (!strcmp(nodeName, PROPFIND_CONTENT_LENGTH)) {
				properties->contentLength = 1;
			} else if (!strcmp(nodeName, PROPFIND_LAST_MODIFIED)) {
				properties->lastModified = 1;
			} else if (!strcmp(nodeName, PROPFIND_DISPLAY_NAME)) {
				properties->displayName = 1;
			} else if (!strcmp(nodeName, PROPFIND_CONTENT_TYPE)) {
				properties->contentType = 1;
			} else if (!strcmp(nodeName, PROPFIND_AVAILABLE_BYTES)) {
				properties->availableBytes = 1;
			} else if (!strcmp(nodeName, PROPFIND_USED_BYTES)) {
				properties->usedBytes = 1;
			} else if (!strcmp(nodeName, PROPFIND_ETAG)) {
				properties->etag = 1;
			}
		}
		stepOver(reader);
	}

	if (!readResult) {
		goto CLEANUP;
	}

	// finish up
	while (stepOver(reader))
		// consume the rest of the input
		;

	CLEANUP:

	xmlFreeTextReader(reader);
	close(fd);
	return readResult;
}

static void writePropFindResponsePart(const char * fileName, const char * displayName, struct PropertySet * properties,
		struct stat * fileStat, xmlTextWriterPtr writer) {

	xmlTextWriterStartElementNS(writer, "d", "response", NULL);
	xmlTextWriterWriteElementString(writer, "href", fileName);
	xmlTextWriterStartElementNS(writer, "d", "propstat", NULL);
	xmlTextWriterStartElementNS(writer, "d", "prop", NULL);

	if (properties->etag) {
		char buffer[200];
		snprintf(buffer, sizeof(buffer), "\"%zd-%lld\"", fileStat->st_size, (long long) fileStat->st_mtime);
		xmlTextWriterWriteElementString(writer, PROPFIND_ETAG, buffer);
	}
	if (properties->creationDate) {
		char buffer[100];
		getWebDate(fileStat->st_ctime, buffer, 100);
		xmlTextWriterWriteElementString(writer, PROPFIND_CREATION_DATE, buffer);
	}
	if (properties->lastModified) {
		char buffer[100];
		getWebDate(fileStat->st_ctime, buffer, 100);
		xmlTextWriterWriteElementString(writer, PROPFIND_LAST_MODIFIED, buffer);
	}
	if (properties->resourceType) {
		xmlTextWriterStartElementNS(writer, "d", PROPFIND_RESOURCE_TYPE, NULL);
		if ((fileStat->st_mode & S_IFMT) == S_IFDIR) {
			xmlTextWriterStartElementNS(writer, "d", "collection", NULL);
			xmlTextWriterEndElement(writer);
		}
		xmlTextWriterEndElement(writer);
	}
	//if (properties->displayName) {
	//	xmlTextWriterWriteElementString(writer, PROPFIND_DISPLAY_NAME, displayName);
	//}
	if ((fileStat->st_mode & S_IFMT) == S_IFDIR) {
		if (properties->availableBytes) {
			struct statvfs fsStat;
			if (!statvfs(fileName, &fsStat)) {
				char buffer[100];
				unsigned long long size = fsStat.f_bavail * fsStat.f_bsize;
				snprintf(buffer, sizeof(buffer), "%llu", size);
				xmlTextWriterWriteElementString(writer, PROPFIND_AVAILABLE_BYTES, buffer);
				if (properties->usedBytes) {
					size = (fsStat.f_blocks - fsStat.f_bfree) * fsStat.f_bsize;
					snprintf(buffer, sizeof(buffer), "%llu", size);
					xmlTextWriterWriteElementString(writer, PROPFIND_USED_BYTES, buffer);
				}
			}
		} else if (properties->usedBytes) {
			struct statvfs fsStat;
			if (!statvfs(fileName, &fsStat)) {
				char buffer[100];
				unsigned long long size = (fsStat.f_blocks - fsStat.f_bfree) * fsStat.f_bsize;
				snprintf(buffer, sizeof(buffer), "%llu", size);
				xmlTextWriterWriteElementString(writer, PROPFIND_USED_BYTES, buffer);
			}
		}
	} else {
		if (properties->contentLength) {
			char buffer[100];
			snprintf(buffer, sizeof(buffer), "%zd", fileStat->st_size);
			xmlTextWriterWriteElementString(writer, PROPFIND_CONTENT_LENGTH, buffer);
		}
		if (properties->contentType) {
			xmlTextWriterWriteElementString(writer, PROPFIND_CONTENT_TYPE, findMimeType(fileName)->type);
		}
	}
	xmlTextWriterEndElement(writer);
	xmlTextWriterWriteElementString(writer, "status", "HTTP/1.1 200 OK");
	xmlTextWriterEndElement(writer);
	xmlTextWriterEndElement(writer);

}

static int respondToPropFind(const char * file, const char * host, struct PropertySet * properties, int depth) {
	struct stat fileStat;
	if (stat(file, &fileStat)) {
		int e = errno;
		switch (e) {
		case EACCES:
			stdLogError(e, "PROPFIND access denied %s %s %s", authenticatedUser, host, file);
			return respond(RAP_ACCESS_DENIED, -1);
		case ENOENT:
		default:
			stdLogError(e, "PROPFIND not found %s %s %s", authenticatedUser, host, file);
			return respond(RAP_NOT_FOUND, -1);
		}
	}

	int pipeEnds[2];
	if (pipe(pipeEnds)) {
		stdLogError(errno, "Could not create pipe to write content");
		return respond(RAP_INTERNAL_ERROR, -1);
	}

	char * filePath;
	size_t filePathSize = strlen(file);
	size_t fileNameSize = filePathSize;
	if ((fileStat.st_mode & S_IFMT) == S_IFDIR && file[filePathSize - 1] != '/') {
		filePath = mallocSafe(filePathSize + 2);
		memcpy(filePath, file, filePathSize);
		filePath[filePathSize] = '/';
		filePath[filePathSize + 1] = '\0';
		filePathSize++;
	} else {
		filePath = (char *) file;
	}

	const char * displayName = &file[fileNameSize - 2];
	while (displayName >= file && *displayName != '/') {
		displayName--;
	}
	displayName++;

	time_t fileTime;
	time(&fileTime);
	struct Message message = { .mID = RAP_MULTISTATUS, .fd = pipeEnds[PIPE_READ], .bufferCount = 2 };
	message.buffers[RAP_DATE_INDEX].iov_base = &fileTime;
	message.buffers[RAP_DATE_INDEX].iov_len = sizeof(fileTime);
	message.buffers[RAP_MIME_INDEX].iov_base = (void *) XML_MIME_TYPE.type;
	message.buffers[RAP_MIME_INDEX].iov_len = XML_MIME_TYPE.typeStringSize;
	message.buffers[RAP_LOCATION_INDEX].iov_base = filePath;
	message.buffers[RAP_LOCATION_INDEX].iov_len = filePathSize + 1;
	size_t messageResult = sendMessage(STDOUT_FILENO, &message);
	if (messageResult <= 0) {
		if (filePath != file) {
			free(filePath);
		}
		close(pipeEnds[PIPE_WRITE]);
		return messageResult;
	}

	// We've set up the pipe and sent read end across so now write the result
	xmlTextWriterPtr writer = xmlNewFdTextWriter(pipeEnds[PIPE_WRITE]);
	DIR * dir;
	xmlTextWriterStartDocument(writer, "1.0", "utf-8", NULL);
	xmlTextWriterStartElementNS(writer, "d", "multistatus", "DAV:");
	writePropFindResponsePart(filePath, displayName, properties, &fileStat, writer);
	if (depth > 1 && (fileStat.st_mode & S_IFMT) == S_IFDIR && (dir = opendir(filePath))) {
		struct dirent * dp;
		char * childFileName = mallocSafe(filePathSize + 257);
		size_t maxSize = 255;
		strcpy(childFileName, filePath);
		while ((dp = readdir(dir)) != NULL) {
			if (dp->d_name[0] != '.' || (dp->d_name[1] != '\0' && dp->d_name[1] != '.') || dp->d_name[2] != '\0') {
				size_t nameSize = strlen(dp->d_name);
				if (nameSize > maxSize) {
					childFileName = reallocSafe(childFileName, filePathSize + nameSize + 2);
					maxSize = nameSize;
				}
				strcpy(childFileName + filePathSize, dp->d_name);
				if (!stat(childFileName, &fileStat)) {
					if ((fileStat.st_mode & S_IFMT) == S_IFDIR) {
						childFileName[filePathSize + nameSize] = '/';
						childFileName[filePathSize + nameSize + 1] = '\0';
					}
					writePropFindResponsePart(childFileName, dp->d_name, properties, &fileStat, writer);
				}
			}
		}
		free(childFileName);
	}
	xmlTextWriterEndElement(writer);
	xmlFreeTextWriter(writer);
	if (filePath != file) {
		free(filePath);
	}
	return messageResult;

}

static size_t propfind(struct Message * requestMessage) {
	if (requestMessage->fd == -1) {
		stdLogError(0, "No body sent in propfind request");
		return respond(RAP_BAD_CLIENT_REQUEST, -1);
	}

	if (!authenticated || requestMessage->bufferCount != 3) {
		if (!authenticated) {
			stdLogError(0, "Not authenticated RAP");
		} else {
			stdLogError(0, "Get request did not provide correct buffers: %d buffer(s)", requestMessage->bufferCount);
		}
		close(requestMessage->fd);
		return respond(RAP_BAD_RAP_REQUEST, -1);
	}

	int ret = respond(RAP_CONTINUE, -1);
	if (ret < 0) {
		return ret;
	}

	const char * depthString = iovecToString(&requestMessage->buffers[RAP_DEPTH_INDEX]);

	struct PropertySet properties;
	if (!parsePropFind(requestMessage->fd, &properties)) {
		return respond(RAP_BAD_CLIENT_REQUEST, -1);
	}

	char * file = iovecToString(&requestMessage->buffers[RAP_FILE_INDEX]);
	return respondToPropFind(file, iovecToString(&requestMessage->buffers[RAP_HOST_INDEX]), &properties,
			(strcmp("0", depthString) ? 2 : 1));
}

//////////////////
// End PROPFIND //
//////////////////

static size_t writeFile(struct Message * requestMessage) {
	if (requestMessage->fd == -1) {
		stdLogError(0, "write file request sent without incoming data!");
		return respond(RAP_BAD_RAP_REQUEST, -1);
	}
	if (!authenticated || requestMessage->bufferCount != 2) {
		if (!authenticated) {
			stdLogError(0, "Not authenticated RAP");
		} else {
			stdLogError(0, "Get request did not provide correct buffers: %d buffer(s)", requestMessage->bufferCount);
		}
		return respond(RAP_BAD_RAP_REQUEST, -1);
	}
	char * host = iovecToString(&requestMessage->buffers[RAP_HOST_INDEX]);
	char * file = iovecToString(&requestMessage->buffers[RAP_FILE_INDEX]);
	int fd = open(file, O_WRONLY);
	if (fd == -1) {
		int e = errno;
		switch (e) {
		case EACCES:
			stdLogError(e, "PUT access denied %s %s %s", authenticatedUser, host, file);
			return respond(RAP_ACCESS_DENIED, -1);
		case ENOENT:
		default:
			stdLogError(e, "PUT not found %s %s %s", authenticatedUser, host, file);
			return respond(RAP_CONFLICT, -1);
		}
	}
	int ret = respond(RAP_CONTINUE, -1);
	if (ret < 0) {
		return ret;
	}

	char buffer[40960];
	ssize_t bytesRead;

	while ((bytesRead = read(requestMessage->fd, buffer, sizeof(buffer))) > 0) {
		ssize_t bytesWritten = write(fd, buffer, bytesRead);
		if (bytesWritten < bytesRead) {
			stdLogError(errno, "Could wite data to file %s", file);
			close(fd);
			close(requestMessage->fd);
			return respond(RAP_INSUFFICIENT_STORAGE, -1);
		}
	}

	close(fd);
	close(requestMessage->fd);
	return respond(RAP_SUCCESS, - 1);
}

static size_t readFile(struct Message * requestMessage) {
	if (requestMessage->fd != -1) {
		stdLogError(0, "read file request sent incoming data!");
		close(requestMessage->fd);
	}
	if (!authenticated || requestMessage->bufferCount != 2) {
		if (!authenticated) {
			stdLogError(0, "Not authenticated RAP");
		} else {
			stdLogError(0, "Get request did not provide correct buffers: %d buffer(s)", requestMessage->bufferCount);
		}
		return respond(RAP_BAD_RAP_REQUEST, -1);
	}

	char * host = iovecToString(&requestMessage->buffers[RAP_HOST_INDEX]);
	char * file = iovecToString(&requestMessage->buffers[RAP_FILE_INDEX]);
	int fd = open(file, O_RDONLY);
	if (fd == -1) {
		int e = errno;
		switch (e) {
		case EACCES:
			stdLogError(e, "GET access denied %s %s %s", authenticatedUser, host, file);
			return respond(RAP_ACCESS_DENIED, -1);
		case ENOENT:
		default:
			stdLogError(e, "GET not found %s %s %s", authenticatedUser, host, file);
			return respond(RAP_NOT_FOUND, -1);
		}
	} else {
		struct stat statinfo;
		fstat(fd, &statinfo);

		if ((statinfo.st_mode & S_IFMT) == S_IFDIR) {
			int pipeEnds[2];
			if (pipe(pipeEnds)) {
				stdLogError(errno, "Could not create pipe to write content");
				close(fd);
				return respond(RAP_INTERNAL_ERROR, -1);
			}

			time_t fileTime;
			time(&fileTime);

			struct Message message = { .mID = RAP_SUCCESS, .fd = pipeEnds[PIPE_READ], 3 };
			message.buffers[RAP_DATE_INDEX].iov_base = &fileTime;
			message.buffers[RAP_DATE_INDEX].iov_len = sizeof(fileTime);
			message.buffers[RAP_MIME_INDEX].iov_base = "text/html";
			message.buffers[RAP_MIME_INDEX].iov_len = sizeof("text/html");
			message.buffers[RAP_LOCATION_INDEX] = requestMessage->buffers[RAP_FILE_INDEX];
			size_t messageResult = sendMessage(STDOUT_FILENO, &message);
			if (messageResult <= 0) {
				close(fd);
				close(pipeEnds[PIPE_WRITE]);
				return messageResult;
			}

			// We've set up the pipe and sent read end across so now write the result
			DIR * dir = fdopendir(fd);
			FILE * outPipe = fdopen(pipeEnds[PIPE_WRITE], "w");
			char * sep = (file[strlen(file) - 1] == '/' ? "" : "/");
			fprintf(outPipe, "<html><head><title>%s%s</title></head><body><h1>%s%s</h1><ul>", file, sep, file, sep);
			struct dirent * dp;
			while ((dp = readdir(dir)) != NULL) {
				if (dp->d_name[0] != '.') {
					if (dp->d_type == DT_DIR) {
						fprintf(outPipe, "<li><a href=\"%s%s%s/\">%s/</a></li>", file, sep, dp->d_name, dp->d_name);
					} else {
						fprintf(outPipe, "<li><a href=\"%s%s%s\">%s</a></li>", file, sep, dp->d_name, dp->d_name);
					}
				}
			}
			fprintf(outPipe, "</ul></body></html>");
			closedir(dir);
			fclose(outPipe);
			return messageResult;
		} else {
			struct Message message = { .mID = RAP_SUCCESS, .fd = fd, .bufferCount = 3 };
			message.buffers[RAP_DATE_INDEX].iov_base = &statinfo.st_mtime;
			message.buffers[RAP_DATE_INDEX].iov_len = sizeof(statinfo.st_mtime);
			struct MimeType * mimeType = findMimeType(file);
			message.buffers[RAP_MIME_INDEX].iov_base = (char *) mimeType->type;
			message.buffers[RAP_MIME_INDEX].iov_len = mimeType->typeStringSize;
			message.buffers[RAP_LOCATION_INDEX] = requestMessage->buffers[RAP_FILE_INDEX];
			return sendMessage(STDOUT_FILENO, &message);
		}
	}
}

static int pamConverse(int n, const struct pam_message **msg, struct pam_response **resp, char * password) {
	struct pam_response * response = mallocSafe(sizeof(struct pam_response));
	response->resp_retcode = 0;
	size_t passSize = strlen(password) + 1;
	response->resp = mallocSafe(passSize);
	memcpy(response->resp, password, passSize);
	*resp = response;
	return PAM_SUCCESS;
}

static pam_handle_t *pamh;

static void pamCleanup() {
	int pamResult = pam_close_session(pamh, 0);
	pam_end(pamh, pamResult);
}

static int pamAuthenticate(const char * user, const char * password, const char * hostname) {
	static struct pam_conv pamc = { .conv = (int (*)(int num_msg, const struct pam_message **msg,
			struct pam_response **resp, void *appdata_ptr)) &pamConverse };
	pamc.appdata_ptr = (void *) password;
	char ** envList;

	if (pam_start(pamService, user, &pamc, &pamh) != PAM_SUCCESS) {
		stdLogError(0, "Could not start PAM");
		return 0;
	}

// Authenticate and start session
	int pamResult;
	if ((pamResult = pam_set_item(pamh, PAM_RHOST, hostname)) != PAM_SUCCESS
			|| (pamResult = pam_set_item(pamh, PAM_RUSER, user)) != PAM_SUCCESS
			|| (pamResult = pam_authenticate(pamh, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK)) != PAM_SUCCESS
			|| (pamResult = pam_acct_mgmt(pamh, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK)) != PAM_SUCCESS || (pamResult =
					pam_setcred(pamh, PAM_ESTABLISH_CRED)) != PAM_SUCCESS
			|| (pamResult = pam_open_session(pamh, 0)) != PAM_SUCCESS) {
		pam_end(pamh, pamResult);
		return 0;
	}

// Get user details
	if ((pamResult = pam_get_item(pamh, PAM_USER, (const void **) &user)) != PAM_SUCCESS
			|| (envList = pam_getenvlist(pamh)) == NULL) {

		pamResult = pam_close_session(pamh, 0);
		pam_end(pamh, pamResult);

		return 0;
	}

// Set up environment and switch user
	clearenv();
	for (char ** pam_env = envList; *pam_env != NULL; ++pam_env) {
		putenv(*pam_env);
		free(*pam_env);
	}
	free(envList);

	if (!lockToUser(user)) {
		stdLogError(errno, "Could not set uid or gid");
		pam_close_session(pamh, 0);
		pam_end(pamh, pamResult);
		return 0;
	}

	atexit(&pamCleanup);
	size_t userLen = strlen(user) + 1;
	authenticatedUser = mallocSafe(userLen);
	memcpy((char *) authenticatedUser, user, userLen);

	authenticated = 1;
	return 1;
}

static size_t authenticate(struct Message * message) {
	if (message->fd != -1) {
		stdLogError(0, "authenticate request send incoming data!");
		close(message->fd);
	}
	if (authenticated || message->bufferCount != 3) {
		if (authenticated) {
			stdLogError(0, "Login for already logged in RAP");
		} else {
			stdLogError(0, "Login provided %d buffer(s) instead of 3", message->bufferCount);
		}
		return respond(RAP_BAD_RAP_REQUEST, -1);
	}

	char * user = iovecToString(&message->buffers[RAP_USER_INDEX]);
	char * password = iovecToString(&message->buffers[RAP_PASSWORD_INDEX]);
	char * rhost = iovecToString(&message->buffers[RAP_RHOST_INDEX]);

	if (pamAuthenticate(user, password, rhost)) {
		//stdLog("Login accepted for %s", user);
		return respond(RAP_SUCCESS, -1);
	} else {
		return respond(RAP_AUTH_FAILLED, -1);
	}
}

int main(int argCount, char * args[]) {
	size_t ioResult;
	struct Message message;
	char incomingBuffer[INCOMING_BUFFER_SIZE];
	if (argCount > 1) {
		pamService = args[1];
	} else {
		pamService = "webdav";
	}

	if (argCount > 2) {
		initializeMimeTypes(args[2]);
	} else {
		initializeMimeTypes("/etc/mime.types");
	}

	do {
		// Read a message
		ioResult = recvMessage(STDIN_FILENO, &message, incomingBuffer, INCOMING_BUFFER_SIZE);
		if (ioResult <= 0) {
			if (ioResult < 0) {
				exit(1);
			} else {
				continue;
			}
		}

		// Handle the message
		if (message.mID > RAP_MAX_REQUEST || message.mID < RAP_MIN_REQUEST) {
			ioResult = respond(RAP_BAD_RAP_REQUEST, -1);
			continue;
		}
		switch (message.mID) {
		case RAP_AUTHENTICATE:
			ioResult = authenticate(&message);
			break;
		case RAP_READ_FILE:
			ioResult = readFile(&message);
			break;
		case RAP_WRITE_FILE:
			ioResult = writeFile(&message);
			break;
		case RAP_PROPFIND:
			ioResult = propfind(&message);
			break;
		}
		if (ioResult < 0) {
			ioResult = 0;
		}

	} while (ioResult);
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	return 0;
}
