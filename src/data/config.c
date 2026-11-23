#include "config.h"
#include "core/platform.h"
#include "util/output.h"
#include <easyfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static BOOL ParseInboxSize(const char* str, uint64_t* out) {
    char* end = NULL;
    double value = strtod(str, &end);
    if (end == str || value <= 0.0) return FALSE;
    while (*end == ' ' || *end == '\t') end++;
    char unit = (char)toupper((unsigned char)*end);
    double multiplier = 1.0;
    switch (unit) {
        case '\0': case 'B': multiplier = 1.0; break;
        case 'K': multiplier = 1024.0; break;
        case 'M': multiplier = 1024.0 * 1024.0; break;
        case 'G': multiplier = 1024.0 * 1024.0 * 1024.0; break;
        case 'T': multiplier = 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
        default: return FALSE;
    }
    if (unit != '\0') {
        end++;
        if (toupper((unsigned char)*end) == 'I') end++;
        if (unit != 'B' && toupper((unsigned char)*end) == 'B') end++;
    }
    if (*end != '\0') return FALSE;
    *out = (uint64_t)(value * multiplier);
    return *out > 0;
}

static BOOL ParseFlag(const char* str, BOOL* out) {
    char lower[16] = { 0 };
    if (strlen(str) >= sizeof(lower)) return FALSE;
    for (size_t i = 0; str[i]; i++) lower[i] = (char)tolower((unsigned char)str[i]);
    if (strcmp(lower, "true") == 0 || strcmp(lower, "yes") == 0 || strcmp(lower, "on") == 0 || strcmp(lower, "1") == 0) {
        *out = TRUE;
        return TRUE;
    }
    if (strcmp(lower, "false") == 0 || strcmp(lower, "no") == 0 || strcmp(lower, "off") == 0 || strcmp(lower, "0") == 0) {
        *out = FALSE;
        return TRUE;
    }
    return FALSE;
}

static BOOL ParseInterval(const char* str, uint32_t* out) {
    char* end = NULL;
    long value = strtol(str, &end, 10);
    if (end == str || *end != '\0' || value < 1 || value > 86400) return FALSE;
    *out = (uint32_t)value;
    return TRUE;
}

static BOOL ParsePort(const char* str, uint16_t* out) {
    char* end = NULL;
    long value = strtol(str, &end, 10);
    if (end == str || *end != '\0' || value < 1024 || value > 65535) return FALSE;
    *out = (uint16_t)value;
    return TRUE;
}

static BOOL ResolvePath(const Config* config, const char* value, char* out, size_t size) {
    if (value[0] == '~' && (value[1] == '/' || value[1] == '\\' || value[1] == '\0')) {
        char home[MAX_PATH_SIZE] = { 0 };
        if (!HomeDirectory(home, sizeof(home))) return FALSE;
        if (value[1] == '\0') return snprintf(out, size, "%s", home) < (int)size;
        return JoinPath(out, size, home, value + 2);
    }
    if (IsAbsolutePath(value)) return snprintf(out, size, "%s", value) < (int)size;
    return JoinPath(out, size, config->home, value);
}

static void DefaultID(char* out, size_t size) {
    char name[MAX_ID_SIZE] = { 0 };
    if (!MachineName(name, sizeof(name))) strcpy(name, "mailbox");
    size_t n = 0;
    for (size_t i = 0; name[i] != '\0' && n < size - 1; i++) {
        char c = name[i];
        if (c == '.') break;
        out[n++] = (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '-';
    }
    out[n] = '\0';
    if (!ValidID(out)) snprintf(out, size, "mailbox");
}

static BOOL GenerateConfig(const char* path) {
    char parent[MAX_PATH_SIZE] = { 0 };
    if (!ParentDirectory(parent, sizeof(parent), path) || !MakeDirectories(parent)) return FALSE;
    char id[MAX_ID_SIZE] = { 0 };
    DefaultID(id, sizeof(id));
    FILE* file = fopen(path, "w");
    if (file == NULL) return FALSE;
    fprintf(file, "# mailbox configuration - relative paths are relative to this file's folder\n");
    fprintf(file, "PORT: %d\n", DEFAULT_PORT);
    fprintf(file, "ID: %s\n", id);
    fprintf(file, "INBOXSIZE: 1G\n");
    fprintf(file, "DIRECTORY: %s\n", DEFAULT_INBOX_NAME);
    fprintf(file, "DNS: %s\n", DEFAULT_DNS_NAME);
    fprintf(file, "STATE: %s\n", DEFAULT_STATE_NAME);
    fprintf(file, "HISTORY: %s\n", DEFAULT_HISTORY_NAME);
    fprintf(file, "PROXY: false\n");
    fprintf(file, "PROXYPATH: %s\n", DEFAULT_PROXYPATH_NAME);
    fprintf(file, "PROXYDIR: %s\n", DEFAULT_PROXYDIR_NAME);
    fprintf(file, "PROXYDIRSIZE: 1G\n");
    fprintf(file, "PINTERVAL: %d\n", DEFAULT_PINTERVAL);
    fprintf(file, "CHATPATH: %s\n", DEFAULT_CHATPATH_NAME);
    fprintf(file, "CHATSIZE: 10M\n");
    fprintf(file, "PASSWORD:\n");
    fclose(file);
    RestrictFile(path);
    return TRUE;
}

void TrimString(char* str) {
    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) str[--len] = '\0';
    size_t start = 0;
    while (str[start] != '\0' && isspace((unsigned char)str[start])) start++;
    if (start > 0) memmove(str, str + start, len - start + 1);
}

BOOL ValidID(const char* id) {
    size_t len = strlen(id);
    if (len == 0 || len >= MAX_ID_SIZE) return FALSE;
    BOOL ipish = TRUE;
    for (size_t i = 0; i < len; i++) {
        char c = id[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') return FALSE;
        if (!isdigit((unsigned char)c) && c != '.') ipish = FALSE;
    }
    return !ipish;
}

BOOL LocateConfig(char* out, size_t size) {
    const char* env = getenv(CONFIG_ENV_VARIABLE);
    if (env != NULL && env[0] != '\0') return AbsolutePath(out, size, env);
    char home[MAX_PATH_SIZE] = { 0 };
    char directory[MAX_PATH_SIZE] = { 0 };
    if (!HomeDirectory(home, sizeof(home))) return FALSE;
    if (!JoinPath(directory, sizeof(directory), home, DEFAULT_CONFIG_DIRECTORY)) return FALSE;
    return JoinPath(out, size, directory, DEFAULT_CONFIG_NAME);
}

BOOL RuntimePath(const Config* config, const char* name, char* out, size_t size) {
    return JoinPath(out, size, config->home, name);
}

BOOL LoadConfig(Config* config) {
    memset(config, 0, sizeof(Config));
    if (!LocateConfig(config->path, sizeof(config->path))) {
        PrintError("Unable to locate a home directory for the mailbox config - set %s to a config path", CONFIG_ENV_VARIABLE);
        return FALSE;
    }
    if (!ParentDirectory(config->home, sizeof(config->home), config->path)) {
        PrintError("Invalid config path \"%s\"", config->path);
        return FALSE;
    }
    if (!ez_file_exists(config->path)) {
        if (!GenerateConfig(config->path)) {
            PrintError("Unable to create a default config at \"%s\"", config->path);
            return FALSE;
        }
        PrintNote("Created a default config at \"%s\"", config->path);
    }

    BOOL seen_port = FALSE, seen_id = FALSE, seen_size = FALSE;
    config->port = DEFAULT_PORT;
    config->inboxsize = DEFAULT_INBOXSIZE;
    config->proxy = FALSE;
    config->proxydirsize = DEFAULT_PROXYDIRSIZE;
    config->pinterval = DEFAULT_PINTERVAL;
    config->chatsize = DEFAULT_CHATSIZE;
    char chatpath[MAX_PATH_SIZE] = DEFAULT_CHATPATH_NAME;
    char proxypath[MAX_PATH_SIZE] = DEFAULT_PROXYPATH_NAME;
    char proxydir[MAX_PATH_SIZE] = DEFAULT_PROXYDIR_NAME;
    char inbox[MAX_PATH_SIZE] = DEFAULT_INBOX_NAME;
    char dns[MAX_PATH_SIZE] = DEFAULT_DNS_NAME;
    char state[MAX_PATH_SIZE] = DEFAULT_STATE_NAME;
    char history[MAX_PATH_SIZE] = DEFAULT_HISTORY_NAME;

    ez_File* file = ez_load_file(config->path);
    if (file == NULL) {
        PrintError("Unable to read config \"%s\"", config->path);
        return FALSE;
    }
    ez_FileParser parser = ez_parser(file);
    char line[MAX_LINE_SIZE] = { 0 };
    BOOL valid = TRUE;
    while (ez_next_line(&parser, line, sizeof(line) - 1)) {
        TrimString(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char* colon = strchr(line, ':');
        if (colon == NULL) {
            PrintError("%s:%d - expected \"KEY: value\"", config->path, (int)parser.line);
            valid = FALSE;
            continue;
        }
        *colon = '\0';
        char* key = line;
        char* value = colon + 1;
        TrimString(key);
        TrimString(value);
        for (char* k = key; *k; k++) *k = (char)toupper((unsigned char)*k);
        if (strcmp(key, "PORT") == 0) {
            if (!ParsePort(value, &config->port)) {
                PrintError("%s:%d - PORT must be a number between 1024 and 65535", config->path, (int)parser.line);
                valid = FALSE;
            }
            seen_port = TRUE;
        } else if (strcmp(key, "ID") == 0) {
            if (!ValidID(value)) {
                PrintError("%s:%d - ID must be 1-%d characters of letters, digits, '-', '_' or '.' and not look like an IP", config->path, (int)parser.line, MAX_ID_SIZE - 1);
                valid = FALSE;
            } else {
                strcpy(config->id, value);
            }
            seen_id = TRUE;
        } else if (strcmp(key, "INBOXSIZE") == 0) {
            if (!ParseInboxSize(value, &config->inboxsize)) {
                PrintError("%s:%d - INBOXSIZE must be a size like 1073741824, 500M or 2G", config->path, (int)parser.line);
                valid = FALSE;
            }
            seen_size = TRUE;
        } else if (strcmp(key, "DIRECTORY") == 0) {
            if (value[0] != '\0') snprintf(inbox, sizeof(inbox), "%s", value);
        } else if (strcmp(key, "DNS") == 0) {
            if (value[0] != '\0') snprintf(dns, sizeof(dns), "%s", value);
        } else if (strcmp(key, "STATE") == 0) {
            if (value[0] != '\0') snprintf(state, sizeof(state), "%s", value);
        } else if (strcmp(key, "PROXY") == 0) {
            if (!ParseFlag(value, &config->proxy)) {
                PrintError("%s:%d - PROXY must be true or false", config->path, (int)parser.line);
                valid = FALSE;
            }
        } else if (strcmp(key, "PROXYPATH") == 0) {
            if (value[0] != '\0') snprintf(proxypath, sizeof(proxypath), "%s", value);
        } else if (strcmp(key, "PROXYDIR") == 0) {
            if (value[0] != '\0') snprintf(proxydir, sizeof(proxydir), "%s", value);
        } else if (strcmp(key, "PROXYDIRSIZE") == 0) {
            if (!ParseInboxSize(value, &config->proxydirsize)) {
                PrintError("%s:%d - PROXYDIRSIZE must be a size like 1073741824, 500M or 2G", config->path, (int)parser.line);
                valid = FALSE;
            }
        } else if (strcmp(key, "CHATPATH") == 0) {
            if (value[0] != '\0') snprintf(chatpath, sizeof(chatpath), "%s", value);
        } else if (strcmp(key, "CHATSIZE") == 0) {
            if (!ParseInboxSize(value, &config->chatsize)) {
                PrintError("%s:%d - CHATSIZE must be a size like 1048576, 500K or 10M", config->path, (int)parser.line);
                valid = FALSE;
            }
        } else if (strcmp(key, "PINTERVAL") == 0) {
            if (!ParseInterval(value, &config->pinterval)) {
                PrintError("%s:%d - PINTERVAL must be a whole number of seconds between 1 and 86400", config->path, (int)parser.line);
                valid = FALSE;
            }
        } else if (strcmp(key, "HISTORY") == 0) {
            if (value[0] != '\0') snprintf(history, sizeof(history), "%s", value);
        } else if (strcmp(key, "PASSWORD") == 0) {
            if (strlen(value) >= MAX_PASSWORD_SIZE) {
                PrintError("%s:%d - PASSWORD is too long", config->path, (int)parser.line);
                valid = FALSE;
            } else {
                strcpy(config->password, value);
                config->protected = value[0] != '\0';
            }
        } else {
            PrintWarn("%s:%d - unknown config key \"%s\" ignored", config->path, (int)parser.line, key);
        }
    }
    ez_free_file(file);
    if (!valid) return FALSE;
    if (!seen_port) PrintWarn("No PORT in config - defaulting to %d", DEFAULT_PORT);
    if (!seen_size) PrintWarn("No INBOXSIZE in config - defaulting to 1G");
    if (!seen_id) {
        DefaultID(config->id, sizeof(config->id));
        PrintWarn("No ID in config - defaulting to \"%s\"", config->id);
    }

    if (!ResolvePath(config, inbox, config->inbox, sizeof(config->inbox)) ||
        !ResolvePath(config, dns, config->dns, sizeof(config->dns)) ||
        !ResolvePath(config, state, config->state, sizeof(config->state)) ||
        !ResolvePath(config, history, config->history, sizeof(config->history)) ||
        !ResolvePath(config, proxypath, config->proxypath, sizeof(config->proxypath)) ||
        !ResolvePath(config, proxydir, config->proxydir, sizeof(config->proxydir)) ||
        !ResolvePath(config, chatpath, config->chatpath, sizeof(config->chatpath))) {
        PrintError("Config paths are too long");
        return FALSE;
    }
    char parent[MAX_PATH_SIZE] = { 0 };
    if (!MakeDirectories(config->inbox)) {
        PrintError("Unable to create mailbox directory \"%s\"", config->inbox);
        return FALSE;
    }
    if (!ParentDirectory(parent, sizeof(parent), config->dns) || !MakeDirectories(parent) ||
        !ParentDirectory(parent, sizeof(parent), config->state) || !MakeDirectories(parent) ||
        !ParentDirectory(parent, sizeof(parent), config->history) || !MakeDirectories(parent)) {
        PrintError("Unable to create folders for the DNS/STATE/HISTORY files");
        return FALSE;
    }
    if (!MakeDirectories(config->chatpath)) {
        PrintError("Unable to create chat directory \"%s\"", config->chatpath);
        return FALSE;
    }
    if (config->proxy && !MakeDirectories(config->proxydir)) {
        PrintError("Unable to create proxy directory \"%s\"", config->proxydir);
        return FALSE;
    }
    return TRUE;
}
