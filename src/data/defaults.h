#ifndef DEFAULTS_H
#define DEFAULTS_H

#include <easybool.h>

#ifdef _WIN32
#define OPSYS "WINDOWS"
#define PATH_SEPARATOR '\\'
#elif defined(__APPLE__)
#define OPSYS "MACOS"
#define PATH_SEPARATOR '/'
#else
#define OPSYS "LINUX"
#define PATH_SEPARATOR '/'
#endif

#define MAILBOX_VERSION "1.0.0"

// protocol
#define PROTOCOL_MAGIC 0x584F424DU // "MBOX"
#define PROTOCOL_VERSION 1
#define MAX_PACKET_SIZE 4096
#define CHUNK_SIZE (64 * 1024)
#define NONCE_SIZE 32
#define DIGEST_SIZE 32
#define TOKEN_SIZE 32
#define HANDSHAKE_TIMEOUT_MS 120000
#define TRANSFER_TIMEOUT_MS 30000

// limits
#define MAX_ID_SIZE 64
#define MAX_FILENAME_SIZE 256
#define MAX_PASSWORD_SIZE 256
#define MAX_PATH_SIZE 1024
#define MAX_LINE_SIZE 2048
#define MAX_COPY_INDEX 100000

// defaults
#define DEFAULT_PORT 7570
#define DEFAULT_INBOXSIZE (1024ULL * 1024ULL * 1024ULL)
#define DEFAULT_CONFIG_DIRECTORY ".mailbox"
#define DEFAULT_CONFIG_NAME "config"
#define DEFAULT_INBOX_NAME "inbox"
#define DEFAULT_DNS_NAME "dns"
#define DEFAULT_STATE_NAME "state"
#define RUNTIME_FILE_NAME "mailbox.pid"
#define LOG_FILE_NAME "mailbox.log"
#define PART_EXTENSION ".mailbox-part"
#define CONFIG_ENV_VARIABLE "MAILBOX_CONFIG"
#define PASSWORD_ENV_VARIABLE "MAILBOX_PASSWORD"
#define DAEMON_COMMAND "__daemon"
#define DAEMON_STARTUP_WAIT_MS 3000

#endif
