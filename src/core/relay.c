#include "relay.h"
#include "core/platform.h"
#include "core/transfer.h"
#include "core/wire.h"
#include "data/history.h"
#include "data/chat.h"
#include "util/output.h"
#include <easyfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define QUEUE_HEADER "# mailbox relay queue v1"
#define QUEUE_FIELDS_V1 14
#define QUEUE_FIELDS 17

IMPL_ARRLIST(RelayJob);

static const char* g_job_names[] = { "forward", "await", "report" };

typedef enum {
    JOB_KEEP = 0,
    JOB_DONE
} JobAction;

static int64_t Now() {
    return (int64_t)time(NULL);
}

static void CleanField(char* str) {
    for (char* c = str; *c; c++) {
        if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
    }
}

static size_t SplitFields(char* line, char** fields, size_t max) {
    size_t count = 0;
    fields[count++] = line;
    for (char* c = line; *c != '\0' && count < max; c++) {
        if (*c == '\t') {
            *c = '\0';
            fields[count++] = c + 1;
        }
    }
    return count;
}

static BOOL CopyChecked(char* out, size_t size, const char* str) {
    if (strlen(str) >= size) return FALSE;
    strcpy(out, str);
    return TRUE;
}

static BOOL ParseJob(char* line, RelayJob* job) {
    char* fields[QUEUE_FIELDS] = { 0 };
    size_t count = SplitFields(line, fields, QUEUE_FIELDS);
    if (count != QUEUE_FIELDS && count != QUEUE_FIELDS_V1) return FALSE;
    memset(job, 0, sizeof(RelayJob));
    BOOL kind = FALSE;
    for (int i = 0; i < 3; i++) {
        if (strcmp(fields[0], g_job_names[i]) == 0) {
            job->kind = (JobKind)i;
            kind = TRUE;
        }
    }
    if (!kind) return FALSE;
    if (!CopyChecked(job->tracking, sizeof(job->tracking), fields[1]) || !ValidTracking(job->tracking) ||
        !CopyChecked(job->origin, sizeof(job->origin), fields[2]) ||
        !CopyChecked(job->target, sizeof(job->target), fields[3]) ||
        !CopyChecked(job->upstream, sizeof(job->upstream), fields[4]) ||
        !CopyChecked(job->filename, sizeof(job->filename), fields[6]) ||
        !CopyChecked(job->detail, sizeof(job->detail), fields[10]) ||
        !CopyChecked(job->saved, sizeof(job->saved), fields[11])) return FALSE;
    unsigned long port = strtoul(fields[5], NULL, 10);
    if (port > 65535) return FALSE;
    job->upstreamport = (uint16_t)port;
    job->size = (uint64_t)strtoull(fields[7], NULL, 10);
    job->hops = (uint8_t)strtoul(fields[8], NULL, 10);
    job->outcome = (uint8_t)strtoul(fields[9], NULL, 10);
    job->due = (int64_t)strtoll(fields[12], NULL, 10);
    job->attempts = (uint32_t)strtoul(fields[13], NULL, 10);
    if (count == QUEUE_FIELDS) {
        job->content = (uint8_t)strtoul(fields[14], NULL, 10);
        job->chattime = (int64_t)strtoll(fields[15], NULL, 10);
        if (!CopyChecked(job->text, sizeof(job->text), fields[16])) return FALSE;
    }
    return TRUE;
}

static void SaveQueue(Relay* relay) {
    if (relay->jobs.size == 0) {
        if (ez_file_exists(relay->queuepath)) RemoveFile(relay->queuepath);
        return;
    }
    char temp[MAX_PATH_SIZE + 16] = { 0 };
    snprintf(temp, sizeof(temp), "%s.tmp", relay->queuepath);
    FILE* file = fopen(temp, "w");
    if (file == NULL) {
        LogEvent("Unable to save relay queue \"%s\"", relay->queuepath);
        return;
    }
    fprintf(file, "%s\n", QUEUE_HEADER);
    for (size_t i = 0; i < relay->jobs.size; i++) {
        RelayJob* job = &relay->jobs.data[i];
        CleanField(job->detail);
        CleanField(job->text);
        fprintf(file, "%s\t%s\t%s\t%s\t%s\t%u\t%s\t%llu\t%u\t%u\t%s\t%s\t%lld\t%u\t%u\t%lld\t%s\n",
            g_job_names[job->kind], job->tracking, job->origin, job->target, job->upstream,
            (unsigned int)job->upstreamport, job->filename, (unsigned long long)job->size,
            (unsigned int)job->hops, (unsigned int)job->outcome, job->detail, job->saved,
            (long long)job->due, (unsigned int)job->attempts,
            (unsigned int)job->content, (long long)job->chattime, job->text);
    }
    BOOL ok = fclose(file) == 0;
    if (!ok || !MoveReplace(temp, relay->queuepath)) {
        RemoveFile(temp);
        LogEvent("Unable to save relay queue \"%s\"", relay->queuepath);
    }
}

static BOOL IsLocal(const RelayJob* job) {
    return job->upstream[0] == '\0';
}

static void RemoveStored(Relay* relay, const RelayJob* job) {
    if (job->content == CONTENT_CHAT) return;
    char path[MAX_PATH_SIZE] = { 0 };
    if (RelayStoragePath(relay, job->tracking, FALSE, path, sizeof(path)) && ez_file_exists(path)) RemoveFile(path);
}

static void ScheduleRetry(Relay* relay, RelayJob* job) {
    job->attempts++;
    // a little jitter so two proxies that both owe each other something don't keep colliding
    uint8_t random = 0;
    RandomBytes(&random, 1);
    uint32_t jitter = relay->config->pinterval >= 4 ? (uint32_t)random % (relay->config->pinterval / 4 + 1) : 0;
    job->due = Now() + (int64_t)relay->config->pinterval + (int64_t)jitter;
}

static void BecomeReport(Relay* relay, RelayJob* job, DeliveryOutcome outcome, const char* detail, const char* saved) {
    RemoveStored(relay, job);
    job->kind = JOB_REPORT;
    job->outcome = (uint8_t)outcome;
    snprintf(job->detail, sizeof(job->detail), "%s", detail);
    CleanField(job->detail);
    snprintf(job->saved, sizeof(job->saved), "%s", saved);
    job->attempts = 0;
    job->due = Now();
}

BOOL ValidTracking(const char* tracking) {
    if (strlen(tracking) != TRACKING_SIZE) return FALSE;
    for (const char* c = tracking; *c; c++) {
        if (!isxdigit((unsigned char)*c) || isupper((unsigned char)*c)) return FALSE;
    }
    return TRUE;
}

BOOL GenerateTracking(char* out, size_t size) {
    uint8_t bytes[TRACKING_SIZE / 2];
    if (size < TRACKING_SIZE + 1 || !RandomBytes(bytes, sizeof(bytes))) return FALSE;
    for (size_t i = 0; i < sizeof(bytes); i++) snprintf(out + i * 2, 3, "%02x", bytes[i]);
    return TRUE;
}

BOOL RelayStoragePath(Relay* relay, const char* tracking, BOOL part, char* out, size_t size) {
    char name[TRACKING_SIZE + 32] = { 0 };
    snprintf(name, sizeof(name), "%s%s", tracking, part ? PART_EXTENSION : RELAY_EXTENSION);
    return JoinPath(out, size, relay->config->proxydir, name);
}

void InitializeRelay(Relay* relay, const Config* config) {
    memset(relay, 0, sizeof(Relay));
    relay->config = config;
    RuntimePath(config, RELAY_QUEUE_NAME, relay->queuepath, sizeof(relay->queuepath));
    if (config->proxy) PurgeExtension(config->proxydir, PART_EXTENSION);
    if (!ez_file_exists(relay->queuepath)) return;
    ez_File* file = ez_load_file(relay->queuepath);
    if (file == NULL) return;
    ez_FileParser parser = ez_parser(file);
    char line[MAX_LINE_SIZE] = { 0 };
    size_t dropped = 0;
    while (ez_next_line(&parser, line, sizeof(line) - 1)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        RelayJob job;
        if (!ParseJob(line, &job)) {
            dropped++;
            continue;
        }
        if (job.kind == JOB_FORWARD && job.content == CONTENT_FILE) {
            char path[MAX_PATH_SIZE] = { 0 };
            if (!RelayStoragePath(relay, job.tracking, FALSE, path, sizeof(path)) || !ez_file_exists(path)) {
                // the file is gone, so the best we can do is tell the sender
                BecomeReport(relay, &job, OUTCOME_FAILED, "relayed file went missing", "");
                snprintf(job.detail, sizeof(job.detail), "%s: relayed file went missing", config->id);
            }
        }
        if (job.kind != JOB_AWAIT) job.due = Now();
        ARRLIST_RelayJob_add(&relay->jobs, job);
    }
    ez_free_file(file);
    if (relay->jobs.size > 0 || dropped > 0) {
        LogEvent("Resumed %llu relay job%s%s", (unsigned long long)relay->jobs.size,
            relay->jobs.size == 1 ? "" : "s", dropped > 0 ? " (dropped unreadable entries)" : "");
    }
    SaveQueue(relay);
}

void CleanRelay(Relay* relay) {
    ARRLIST_RelayJob_clear(&relay->jobs);
}

int64_t RelayWaitMs(Relay* relay) {
    int64_t soonest = -1;
    for (size_t i = 0; i < relay->jobs.size; i++) {
        RelayJob* job = &relay->jobs.data[i];
        if (job->kind == JOB_AWAIT) continue;
        if (soonest < 0 || job->due < soonest) soonest = job->due;
    }
    if (soonest < 0) return -1;
    int64_t wait = soonest - Now();
    return wait <= 0 ? 0 : wait * 1000;
}

BOOL RelayHasJob(Relay* relay, const char* tracking) {
    for (size_t i = 0; i < relay->jobs.size; i++) {
        if (strcmp(relay->jobs.data[i].tracking, tracking) == 0) return TRUE;
    }
    return FALSE;
}

void QueueForward(Relay* relay, const RelayJob* job) {
    ARRLIST_RelayJob_add(&relay->jobs, *job);
    SaveQueue(relay);
}

static JobAction ProcessForward(Relay* relay, RelayJob* job) {
    const Config* config = relay->config;
    char path[MAX_PATH_SIZE] = { 0 };
    char detail[MAX_DETAIL_SIZE] = { 0 };
    BOOL chat = job->content == CONTENT_CHAT;
    if (!chat && (!RelayStoragePath(relay, job->tracking, FALSE, path, sizeof(path)) || !ez_file_exists(path))) {
        snprintf(detail, sizeof(detail), "%s: relayed file went missing", config->id);
        BecomeReport(relay, job, OUTCOME_FAILED, detail, "");
        return JOB_KEEP;
    }
    char payload[MAX_CHAT_PAYLOAD + 1] = { 0 };
    if (chat) snprintf(payload, sizeof(payload), "%lld\n%s", (long long)job->chattime, job->text);
    Route route;
    if (!LookupRoute(config, job->target, &route)) {
        snprintf(detail, sizeof(detail), "%s: no route to %s", config->id, job->target);
        LogEvent("No route to %s for \"%s\" [%.8s] - reporting failure", job->target, job->filename, job->tracking);
        BecomeReport(relay, job, OUTCOME_FAILED, detail, "");
        return JOB_KEEP;
    }

    if (!chat || job->attempts == 0) {
        LogEvent("Trying to %s \"%s\" [%.8s] for %s to %s:%u (attempt %u)",
            route.proxy ? "pass on" : "deliver", job->filename, job->tracking, job->target,
            route.addressstr, (unsigned int)route.port, (unsigned int)job->attempts + 1);
    }
    Transmission transmission = { 0 };
    transmission.sender = job->origin;
    transmission.filepath = path;
    transmission.filename = job->filename;
    transmission.size = chat ? strlen(payload) : job->size;
    transmission.address = route.address;
    transmission.port = route.port;
    transmission.proxy = route.proxy || chat; // chats always carry a tracking header
    transmission.chat = chat;
    transmission.data = chat ? (const uint8_t*)payload : NULL;
    transmission.target = job->target;
    transmission.tracking = job->tracking;
    transmission.replyport = config->port;
    transmission.hops = (uint8_t)(job->hops + 1);
    transmission.interactive = FALSE;
    TransmitOutcome outcome;
    TransmitResult result = Transmit(&transmission, &outcome);

    char reason[MAX_DETAIL_SIZE - MAX_ID_SIZE - 2] = { 0 };
    switch (result) {
        case TRANSMIT_DELIVERED:
            LogEvent("Delivered \"%s\" [%.8s] to %s as \"%s\"", job->filename, job->tracking, outcome.server, outcome.saved);
            BecomeReport(relay, job, OUTCOME_DELIVERED, "", outcome.saved);
            return JOB_KEEP;
        case TRANSMIT_RELAYED:
            LogEvent("Passed \"%s\" [%.8s] on to proxy %s - waiting for its report", job->filename, job->tracking, outcome.server);
            RemoveStored(relay, job);
            job->kind = JOB_AWAIT;
            job->attempts = 0;
            return JOB_KEEP;
        case TRANSMIT_REJECTED:
            VerdictReason(outcome.verdict, outcome.message, reason, sizeof(reason));
            snprintf(detail, sizeof(detail), "%s: %s", outcome.server, reason);
            LogEvent("%s rejected \"%s\" [%.8s] - %s", outcome.server, job->filename, job->tracking, reason);
            BecomeReport(relay, job, OUTCOME_FAILED, detail, "");
            return JOB_KEEP;
        case TRANSMIT_PASSWORD:
            snprintf(detail, sizeof(detail), "%s: requires a password, which can't be relayed", outcome.server);
            LogEvent("%s needs a password for \"%s\" [%.8s] - reporting failure", outcome.server, job->filename, job->tracking);
            BecomeReport(relay, job, OUTCOME_FAILED, detail, "");
            return JOB_KEEP;
        case TRANSMIT_LOCAL:
            snprintf(detail, sizeof(detail), "%s: could not read the relayed file", config->id);
            BecomeReport(relay, job, OUTCOME_FAILED, detail, "");
            return JOB_KEEP;
        default:
            ScheduleRetry(relay, job);
            LogEvent("Could not reach %s:%u for \"%s\" [%.8s] (%s) - retrying in %llds", route.addressstr,
                (unsigned int)route.port, job->filename, job->tracking, outcome.message, (long long)(job->due - Now()));
            return JOB_KEEP;
    }
}

static JobAction ProcessReport(Relay* relay, RelayJob* job) {
    const char* outcome = job->outcome == OUTCOME_DELIVERED ? "delivered" : "failed";
    if (IsLocal(job)) {
        // our own outgoing chat - the result goes into our chat log for the chat window to pick up
        if (!AppendChatAck(relay->config, job->target, job->tracking, job->outcome == OUTCOME_DELIVERED, job->detail)) {
            LogEvent("Unable to record that chat [%.8s] to %s %s", job->tracking, job->target, outcome);
        }
        if (job->outcome != OUTCOME_DELIVERED) {
            LogEvent("Chat [%.8s] to %s failed - %s", job->tracking, job->target, job->detail);
        }
        return JOB_DONE;
    }
    if (SendStatus(job->upstream, job->upstreamport, job->tracking, job->outcome, job->detail, job->saved)) {
        LogEvent("Told %s:%u that \"%s\" [%.8s] %s", job->upstream, (unsigned int)job->upstreamport,
            job->filename, job->tracking, outcome);
        return JOB_DONE;
    }
    ScheduleRetry(relay, job);
    LogEvent("Could not report \"%s\" [%.8s] %s to %s:%u - retrying in %llds", job->filename, job->tracking,
        outcome, job->upstream, (unsigned int)job->upstreamport, (long long)(job->due - Now()));
    return JOB_KEEP;
}

void RunRelayJobs(Relay* relay) {
    // reports produced by a forward go out in the same pass, so loop until nothing is due
    BOOL changed = TRUE;
    for (int pass = 0; changed && pass < 4; pass++) {
        changed = FALSE;
        size_t i = 0;
        while (i < relay->jobs.size) {
            RelayJob* job = &relay->jobs.data[i];
            if (job->kind == JOB_AWAIT || job->due > Now()) {
                i++;
                continue;
            }
            JobKind before = job->kind;
            JobAction action = job->kind == JOB_FORWARD ? ProcessForward(relay, job) : ProcessReport(relay, job);
            if (action == JOB_DONE) {
                ARRLIST_RelayJob_remove(&relay->jobs, i);
                changed = TRUE;
            } else {
                if (relay->jobs.data[i].kind != before) changed = TRUE;
                i++;
            }
            SaveQueue(relay);
        }
    }
}

uint64_t HeldChatBytes(Relay* relay) {
    uint64_t total = 0;
    for (size_t i = 0; i < relay->jobs.size; i++) {
        RelayJob* job = &relay->jobs.data[i];
        if (job->content == CONTENT_CHAT && !IsLocal(job) && job->kind == JOB_FORWARD) total += strlen(job->text) + 64;
    }
    return total;
}

int QueueChat(Relay* relay, const char* host, const char* text, char* message, size_t size) {
    message[0] = '\0';
    size_t pending = 0;
    for (size_t i = 0; i < relay->jobs.size; i++) {
        if (IsLocal(&relay->jobs.data[i])) pending++;
    }
    if (pending >= MAX_PENDING_CHATS) {
        snprintf(message, size, "too many chats waiting to be delivered");
        return VERDICT_FAILED;
    }
    RelayJob job = { 0 };
    if (!GenerateTracking(job.tracking, sizeof(job.tracking))) {
        snprintf(message, size, "could not generate a tracking ID");
        return VERDICT_FAILED;
    }
    job.kind = JOB_FORWARD;
    job.content = CONTENT_CHAT;
    job.chattime = Now();
    snprintf(job.origin, sizeof(job.origin), "%s", relay->config->id);
    snprintf(job.target, sizeof(job.target), "%s", host);
    snprintf(job.filename, sizeof(job.filename), "chat");
    snprintf(job.text, sizeof(job.text), "%s", text);
    CleanField(job.text);
    job.size = strlen(job.text);
    job.due = Now();
    AppendResult appended = AppendChatMessage(relay->config, host, TRUE, job.tracking, job.chattime, job.text);
    if (appended != APPEND_OK) {
        snprintf(message, size, "%s", appended == APPEND_FULL ? "your chat storage (CHATSIZE) is full" : "could not save the chat");
        return appended == APPEND_FULL ? VERDICT_NO_SPACE : VERDICT_FAILED;
    }
    QueueForward(relay, &job);
    return VERDICT_ACCEPT;
}

void HandleStatus(Relay* relay, const char* tracking, uint8_t outcome, const char* detail, const char* saved) {
    const char* word = outcome == OUTCOME_DELIVERED ? "delivered" : "failed";
    for (size_t i = 0; i < relay->jobs.size; i++) {
        RelayJob* job = &relay->jobs.data[i];
        if (strcmp(job->tracking, tracking) != 0 || job->kind != JOB_AWAIT) continue;
        if (IsLocal(job)) {
            LogEvent("Downstream says your %s [%.8s] to %s %s", job->filename, tracking, job->target, word);
        } else {
            LogEvent("Downstream says \"%s\" [%.8s] %s - passing that back to %s:%u", job->filename, tracking, word,
                job->upstream, (unsigned int)job->upstreamport);
        }
        BecomeReport(relay, job, (DeliveryOutcome)outcome, detail, saved);
        SaveQueue(relay);
        return;
    }
    // not one we're relaying, so it must be one we sent
    BOOL found = FALSE;
    SentStatus status = outcome == OUTCOME_DELIVERED ? SENT_DELIVERED : SENT_FAILED;
    if (!UpdateSent(relay->config->history, tracking, status, detail, saved, &found)) {
        LogEvent("Unable to record status for [%.8s] in \"%s\"", tracking, relay->config->history);
    } else if (found) {
        LogEvent("Your delivery [%.8s] %s%s%s", tracking, word, detail[0] ? " - " : "", detail);
    } else {
        LogEvent("Ignored a status report for unknown delivery [%.8s]", tracking);
    }
}
