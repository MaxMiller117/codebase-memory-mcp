/*
 * frontend.c — Stateless stdio bridge for a daemon-backed MCP session.
 */
#include "daemon/frontend.h"

#include "daemon/application.h"
#include "foundation/compat.h"
#include "foundation/compat_thread.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/subprocess.h"
#include "mcp/mcp.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    FRONTEND_QUEUE_CAPACITY = 8,
    FRONTEND_QUEUE_BYTES_MAX = 12 * 1024 * 1024,
    FRONTEND_REQUEST_TIMEOUT_MS = 24 * 60 * 60 * 1000,
    FRONTEND_CLOSE_TIMEOUT_MS = 5000,
    FRONTEND_JOIN_WATCHDOG_MS = FRONTEND_CLOSE_TIMEOUT_MS,
    /* A regular-file/buffered client can write several complete requests and
     * close stdin before the worker is scheduled. Give already-accepted input
     * a short FIFO drain window so EOF cannot silently discard it. A genuinely
     * active long operation is still cancelled at this deadline. */
    FRONTEND_WAIT_US = 1000,
    /* An idle thin frontend owns no work and only needs to notice the next
     * queue item promptly. Ten milliseconds avoids a 1 kHz wake-up loop per
     * connected coding-agent session without perceptible request latency. */
    FRONTEND_IDLE_WAIT_US = 10 * 1000,
    /* EOF-drain progress window. Sized to cover a cold daemon spawn plus one
     * request round-trip on the slowest platform (several seconds on Windows
     * under CI load): the drain aborts only after this long with NO item
     * completing, so batch clients that close stdin after writing keep
     * receiving every response while a genuinely stuck request still gets
     * cancelled and the frontend exits cleanly. */
    FRONTEND_EOF_DRAIN_MS = 15000,
    FRONTEND_MAINTENANCE_POLL_MS = 10,
    /* Cadence for the quiet no-maintenance steady state. Every presence probe
     * try-acquires the maintenance marker lock, and on Windows that
     * revalidates the whole ancestor directory chain (a CreateFileW +
     * identity check per path component). At the old 10 ms cadence this
     * burned ~20% of a core for the life of every MCP session — 7762
     * CPU-seconds on one 9.5 h session in the 2026-08-29 incident. The
     * budget: an activation drains participants within
     * CLI_ACTIVATION_DRAIN_TIMEOUT_MS (15 s) and the monitor's own exit grace
     * is FRONTEND_MAINTENANCE_GRACE_MS (3 s), so a 250 ms detection latency
     * spends under 2% of that window while cutting the probe cost 25-fold.
     * The 10 ms cadence still paces the monitor's post-detection grace loop,
     * which does no lock probing. */
    FRONTEND_MAINTENANCE_IDLE_POLL_MS = 250,
    /* The owner thread may be draining a supervised process tree. Preserve the
     * supervisor's complete graceful + forced-settle window before the monitor
     * fail-stops the process, plus scheduling/teardown margin. */
    FRONTEND_MAINTENANCE_GRACE_MS =
        CBM_SUBPROCESS_MAX_CANCEL_GRACE_MS + CBM_SUBPROCESS_FORCE_SETTLE_MS + 1000,
    FRONTEND_PARTICIPANT_NAME_CAP = 64,
};

typedef struct {
    char *message;
    size_t length;
    bool content_length_framed;
    bool has_id;
    int64_t id;
    char *id_str;
    cbm_daemon_runtime_application_token_t request_token;
    bool cancelled;
    /* FleetHd local patch: the `initialize` request, after which extra lanes may start. */
    bool initialize;
} frontend_item_t;

/* FleetHd local patch: one lane per daemon connection. Lane 0 is the session's own
 * connection and runs every request kind in arrival order. Extra lanes
 * (CBM_FLEETHD_MCP_CONNECTIONS) run read-only tool calls from the pool queue in
 * parallel; each replays the session's MCP handshake once on its own connection. */
typedef struct {
    cbm_daemon_runtime_client_t *client;
    bool in_request;
    bool active_has_id;
    int64_t active_id;
    const char *active_id_str;
    cbm_daemon_runtime_application_token_t active_request_token;
    bool done;
    bool replayed;
} frontend_lane_t;

typedef struct {
    frontend_item_t items[FRONTEND_QUEUE_CAPACITY];
    size_t head;
    size_t count;
} frontend_queue_t;

typedef struct frontend_state frontend_state_t;

typedef struct {
    frontend_state_t *state;
    int lane;
} frontend_worker_arg_t;

struct frontend_state {
    cbm_mutex_t mutex;
    cbm_mutex_t out_mutex; /* lanes write whole responses to `out` one at a time */
    cbm_version_cohort_manager_t *cohort_manager;
    FILE *out;
    frontend_queue_t primary; /* lane 0 only, arrival order */
    frontend_queue_t pool;    /* read-only tool calls, any lane */
    size_t queued_bytes;
    bool input_closed;
    bool stopping;
    bool failed;
    frontend_lane_t lanes[CBM_DAEMON_FRONTEND_MAX_LANES];
    int lane_count; /* lanes opened so far; only the stdin reader adds one */
    int lane_limit;
    int lanes_done;
    cbm_daemon_frontend_open_lane_fn open_lane;
    void *open_context;
    cbm_thread_t workers[CBM_DAEMON_FRONTEND_MAX_LANES];
    frontend_worker_arg_t worker_args[CBM_DAEMON_FRONTEND_MAX_LANES];
    bool worker_started[CBM_DAEMON_FRONTEND_MAX_LANES];
    char *init_message; /* the session's `initialize`, replayed on extra lanes */
    char *init_note;    /* its `notifications/initialized` */
    bool init_done;
    /* Monotonic count of fully processed queue items (responses written or
     * cancellations acknowledged). The EOF drain below watches it to tell a
     * batch that is still making progress from a genuinely stuck request. */
    atomic_uint_fast64_t completed_items;
};

typedef struct {
    atomic_bool complete;
} frontend_join_watchdog_t;

struct cbm_daemon_maintenance_monitor {
    cbm_thread_t thread;
    cbm_version_cohort_manager_t *manager;
    cbm_daemon_maintenance_cancel_fn cancel;
    void *cancel_context;
    atomic_bool stopping;
    bool exit_when_cancel_not_needed;
    int exit_code;
    char participant[FRONTEND_PARTICIPANT_NAME_CAP];
};

#if defined(CBM_ENABLE_TEST_SEAMS) && CBM_ENABLE_TEST_SEAMS
static atomic_bool frontend_test_hold_monitor = ATOMIC_VAR_INIT(false);
static atomic_bool frontend_test_monitor_is_waiting = ATOMIC_VAR_INIT(false);
static atomic_uint_fast64_t frontend_test_monitor_observation_count = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t frontend_test_worker_observation_count = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t frontend_test_worker_idle_count = ATOMIC_VAR_INIT(0);

void cbm_daemon_frontend_test_observer_reset(bool hold_monitor) {
    atomic_store_explicit(&frontend_test_monitor_observation_count, 0, memory_order_release);
    atomic_store_explicit(&frontend_test_worker_observation_count, 0, memory_order_release);
    atomic_store_explicit(&frontend_test_worker_idle_count, 0, memory_order_release);
    atomic_store_explicit(&frontend_test_monitor_is_waiting, false, memory_order_release);
    atomic_store_explicit(&frontend_test_hold_monitor, hold_monitor, memory_order_release);
}

void cbm_daemon_frontend_test_observer_release(void) {
    atomic_store_explicit(&frontend_test_hold_monitor, false, memory_order_release);
}

bool cbm_daemon_frontend_test_monitor_waiting(void) {
    return atomic_load_explicit(&frontend_test_monitor_is_waiting, memory_order_acquire);
}

uint64_t cbm_daemon_frontend_test_monitor_observations(void) {
    return atomic_load_explicit(&frontend_test_monitor_observation_count, memory_order_acquire);
}

uint64_t cbm_daemon_frontend_test_worker_observations(void) {
    return atomic_load_explicit(&frontend_test_worker_observation_count, memory_order_acquire);
}

uint64_t cbm_daemon_frontend_test_worker_idle_cycles(void) {
    return atomic_load_explicit(&frontend_test_worker_idle_count, memory_order_acquire);
}
#endif

static cbm_version_cohort_maintenance_presence_t frontend_observe_maintenance(
    cbm_version_cohort_manager_t *manager, bool independent_monitor) {
#if defined(CBM_ENABLE_TEST_SEAMS) && CBM_ENABLE_TEST_SEAMS
    if (independent_monitor) {
        atomic_fetch_add_explicit(&frontend_test_monitor_observation_count, 1,
                                  memory_order_release);
        if (atomic_load_explicit(&frontend_test_hold_monitor, memory_order_acquire)) {
            atomic_store_explicit(&frontend_test_monitor_is_waiting, true, memory_order_release);
            while (atomic_load_explicit(&frontend_test_hold_monitor, memory_order_acquire)) {
                cbm_usleep(FRONTEND_WAIT_US);
            }
            atomic_store_explicit(&frontend_test_monitor_is_waiting, false, memory_order_release);
        }
    } else {
        atomic_fetch_add_explicit(&frontend_test_worker_observation_count, 1, memory_order_release);
    }
#else
    (void)independent_monitor;
#endif
    return cbm_version_cohort_maintenance_presence_terminal(manager);
}

static void *frontend_maintenance_monitor_worker(void *opaque) {
    cbm_daemon_maintenance_monitor_t *monitor = opaque;
    while (!atomic_load_explicit(&monitor->stopping, memory_order_acquire)) {
        cbm_version_cohort_maintenance_presence_t presence =
            frontend_observe_maintenance(monitor->manager, true);
        if (presence == CBM_VERSION_COHORT_MAINTENANCE_ABSENT) {
            cbm_usleep(FRONTEND_MAINTENANCE_IDLE_POLL_MS * 1000U);
            continue;
        }

        if (presence == CBM_VERSION_COHORT_MAINTENANCE_REQUESTED) {
            bool cancel_requested = false;
            if (monitor->cancel) {
                cancel_requested = monitor->cancel(monitor->cancel_context);
            }
            /* Never log, write to, or flush agent stdio from this monitor.
             * Structured logging itself writes to stderr, and this thread's
             * reason for existing is to remain runnable when another frontend
             * thread is blocked on a full stdout/stderr pipe. The activation
             * owner records the maintenance event durably. */

            if (monitor->exit_when_cancel_not_needed && !cancel_requested) {
                _Exit(monitor->exit_code);
            }
            uint64_t now = cbm_now_ms();
            uint64_t deadline = now > UINT64_MAX - FRONTEND_MAINTENANCE_GRACE_MS
                                    ? UINT64_MAX
                                    : now + FRONTEND_MAINTENANCE_GRACE_MS;
            while (!atomic_load_explicit(&monitor->stopping, memory_order_acquire) &&
                   cbm_now_ms() < deadline) {
                cbm_usleep(FRONTEND_MAINTENANCE_POLL_MS * 1000U);
            }
            if (atomic_load_explicit(&monitor->stopping, memory_order_acquire)) {
                return NULL;
            }
            _Exit(monitor->exit_code);
        }

        /* An observer that cannot prove absence must not let local work
         * survive into a binary mutation window. Fail closed and let native
         * process teardown release SQLite and cohort ownership. */
        _Exit(EXIT_FAILURE);
    }
    return NULL;
}

static cbm_daemon_maintenance_monitor_t *frontend_maintenance_monitor_start(
    cbm_version_cohort_manager_t *manager, cbm_daemon_maintenance_cancel_fn cancel,
    void *cancel_context, int exit_code, const char *participant,
    bool exit_when_cancel_not_needed) {
    if (!manager || exit_code < 0 || !participant || !participant[0]) {
        return NULL;
    }
    cbm_daemon_maintenance_monitor_t *monitor = calloc(1, sizeof(*monitor));
    if (!monitor) {
        return NULL;
    }
    monitor->manager = manager;
    monitor->cancel = cancel;
    monitor->cancel_context = cancel_context;
    monitor->exit_when_cancel_not_needed = exit_when_cancel_not_needed;
    monitor->exit_code = exit_code;
    atomic_init(&monitor->stopping, false);
    int written = snprintf(monitor->participant, sizeof(monitor->participant), "%s", participant);
    if (written <= 0 || written >= (int)sizeof(monitor->participant) ||
        cbm_thread_create(&monitor->thread, 0, frontend_maintenance_monitor_worker, monitor) != 0) {
        free(monitor);
        return NULL;
    }
    return monitor;
}

cbm_daemon_maintenance_monitor_t *cbm_daemon_maintenance_monitor_start(
    cbm_version_cohort_manager_t *manager, cbm_daemon_maintenance_cancel_fn cancel,
    void *cancel_context, int exit_code, const char *participant) {
    return frontend_maintenance_monitor_start(manager, cancel, cancel_context, exit_code,
                                              participant, false);
}

bool cbm_daemon_maintenance_monitor_stop(cbm_daemon_maintenance_monitor_t **monitor_io) {
    if (!monitor_io || !*monitor_io) {
        return false;
    }
    cbm_daemon_maintenance_monitor_t *monitor = *monitor_io;
    atomic_store_explicit(&monitor->stopping, true, memory_order_release);
    if (cbm_thread_join(&monitor->thread) != 0) {
        return false;
    }
    free(monitor);
    *monitor_io = NULL;
    return true;
}

static void frontend_item_free(frontend_item_t *item) {
    if (item) {
        free(item->message);
        free(item->id_str);
        memset(item, 0, sizeof(*item));
    }
}

static bool frontend_any_in_request(const frontend_state_t *state) {
    for (int i = 0; i < state->lane_count; i++) {
        if (state->lanes[i].in_request) {
            return true;
        }
    }
    return false;
}

static bool frontend_should_stop(frontend_state_t *state) {
    cbm_mutex_lock(&state->mutex);
    /* The input-closed leg must also require that no item is IN FLIGHT: the
     * worker consults this while deciding whether a request failure was an
     * expected shutdown, and the final queued item after EOF has count == 0
     * precisely while its own forward is outstanding. Without the in_request
     * term, any daemon-side failure of that last item was classified as an
     * expected stop and its response silently dropped with a success exit —
     * an unanswerable failure mode for the client. */
    bool stopping = state->stopping ||
                    (state->input_closed && state->primary.count == 0 &&
                     state->pool.count == 0 && !frontend_any_in_request(state));
    cbm_mutex_unlock(&state->mutex);
    return stopping;
}

static void frontend_worker_mark_done(frontend_state_t *state, int lane) {
    cbm_mutex_lock(&state->mutex);
    if (!state->lanes[lane].done) {
        state->lanes[lane].done = true;
        state->lanes_done++;
    }
    cbm_mutex_unlock(&state->mutex);
}

static bool frontend_worker_is_done(frontend_state_t *state) {
    cbm_mutex_lock(&state->mutex);
    bool done = state->lanes_done >= state->lane_count;
    cbm_mutex_unlock(&state->mutex);
    return done;
}

static void frontend_input_closed(frontend_state_t *state) {
    cbm_mutex_lock(&state->mutex);
    state->input_closed = true;
    cbm_mutex_unlock(&state->mutex);
}

static void *frontend_join_watchdog(void *opaque) {
    frontend_join_watchdog_t *watchdog = opaque;
    uint64_t now = cbm_now_ms();
    uint64_t deadline =
        now > UINT64_MAX - FRONTEND_JOIN_WATCHDOG_MS ? UINT64_MAX : now + FRONTEND_JOIN_WATCHDOG_MS;
    while (!atomic_load_explicit(&watchdog->complete, memory_order_acquire) &&
           cbm_now_ms() < deadline) {
        cbm_usleep(FRONTEND_WAIT_US);
    }
    if (!atomic_load_explicit(&watchdog->complete, memory_order_acquire)) {
        /* A thin frontend owns no durable state. If stdout is backpressured,
         * fclose/IPC cancellation cannot portably wake its worker on every
         * platform. Fail-stop releases the authenticated connection and lets
         * the daemon cancel this exact session instead of hanging forever. */
        _Exit(EXIT_FAILURE);
    }
    return NULL;
}

/* Lane 0 takes the primary queue first, then the pool. An extra lane takes only
 * the pool, and only after the session's `initialize` completed on lane 0. */
static frontend_queue_t *frontend_lane_queue(frontend_state_t *state, int lane) {
    if (lane == 0) {
        if (state->primary.count > 0) {
            return &state->primary;
        }
        return state->pool.count > 0 ? &state->pool : NULL;
    }
    return state->init_done && state->pool.count > 0 ? &state->pool : NULL;
}

/* Pop and publish the active request identity under one mutex acquisition, so
 * a cancellation reader can never observe the item as neither queued nor
 * active. */
static bool frontend_pop_begin(frontend_state_t *state, int lane_index, frontend_item_t *item) {
    bool popped = false;
    cbm_mutex_lock(&state->mutex);
    frontend_queue_t *queue = state->stopping ? NULL : frontend_lane_queue(state, lane_index);
    if (queue) {
        frontend_lane_t *lane = &state->lanes[lane_index];
        frontend_item_t *queued = &queue->items[queue->head];
        cbm_daemon_runtime_application_token_t request_token =
            CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID;
        bool token_ready = queued->cancelled || cbm_daemon_runtime_client_application_token_reserve(
                                                    lane->client, &request_token);
        if (!token_ready) {
            state->failed = true;
            state->stopping = true;
            cbm_mutex_unlock(&state->mutex);
            return false;
        }
        *item = *queued;
        memset(queued, 0, sizeof(*queued));
        queue->head = (queue->head + 1U) % FRONTEND_QUEUE_CAPACITY;
        queue->count--;
        state->queued_bytes -= item->length;
        lane->in_request = true;
        lane->active_has_id = item->has_id;
        lane->active_id = item->id;
        lane->active_id_str = item->id_str;
        item->request_token = request_token;
        lane->active_request_token = request_token;
        popped = true;
    }
    cbm_mutex_unlock(&state->mutex);
    return popped;
}

static void frontend_end_request(frontend_state_t *state, int lane_index, bool failed) {
    cbm_mutex_lock(&state->mutex);
    frontend_lane_t *lane = &state->lanes[lane_index];
    lane->in_request = false;
    lane->active_has_id = false;
    lane->active_id = 0;
    lane->active_id_str = NULL;
    lane->active_request_token = CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID;
    state->failed = state->failed || failed;
    cbm_mutex_unlock(&state->mutex);
}

static bool frontend_write_response(FILE *out, const uint8_t *response, uint32_t response_length,
                                    bool content_length_framed) {
    bool written = false;
    if (content_length_framed) {
        written = fprintf(out, "Content-Length: %u\r\n\r\n", response_length) >= 0 &&
                  fwrite(response, 1, response_length, out) == response_length;
    } else {
        written =
            fwrite(response, 1, response_length, out) == response_length && fputc('\n', out) != EOF;
    }
    return fflush(out) == 0 && written;
}

const char *cbm_daemon_frontend_cancelled_error_message(const char *request_message) {
    static const char plain[] = "Request cancelled";
    static const char index_call[] = "Request cancelled. " CBM_MCP_INDEX_ASYNC_HINT;
    cbm_jsonrpc_request_t request = {0};
    if (!request_message || cbm_jsonrpc_parse(request_message, &request) != 0) {
        return plain;
    }
    char *tool = request.method && strcmp(request.method, "tools/call") == 0 && request.params_raw
                     ? cbm_mcp_get_string_arg(request.params_raw, "name")
                     : NULL;
    bool index_repository = tool && strcmp(tool, "index_repository") == 0;
    safe_free(tool);
    cbm_jsonrpc_request_free(&request);
    return index_repository ? index_call : plain;
}

static bool frontend_write_cancelled_response(FILE *out, const frontend_item_t *item) {
    char cancelled_error[512];
    (void)snprintf(cancelled_error, sizeof(cancelled_error), "{\"code\":-32800,\"message\":\"%s\"}",
                   cbm_daemon_frontend_cancelled_error_message(item->message));
    cbm_jsonrpc_response_t response = {
        .id = item->id,
        .id_str = item->id_str,
        .error_json = cancelled_error,
        .error_code = -32800,
    };
    char *encoded = cbm_jsonrpc_format_response(&response);
    bool written =
        encoded && frontend_write_response(out, (const uint8_t *)encoded, (uint32_t)strlen(encoded),
                                           item->content_length_framed);
    free(encoded);
    return written;
}

static bool frontend_parse_cancellation(const char *message, cbm_jsonrpc_request_t *request_out) {
    if (!message || !request_out) {
        return false;
    }
    memset(request_out, 0, sizeof(*request_out));
    if (cbm_jsonrpc_parse(message, request_out) != 0) {
        return false;
    }
    bool cancellation = !request_out->has_id && request_out->method &&
                        strcmp(request_out->method, "notifications/cancelled") == 0;
    if (!cancellation) {
        cbm_jsonrpc_request_free(request_out);
    }
    return cancellation;
}

bool cbm_daemon_frontend_is_cancellation_notification(const char *message) {
    cbm_jsonrpc_request_t request = {0};
    bool cancellation = frontend_parse_cancellation(message, &request);
    if (cancellation) {
        cbm_jsonrpc_request_free(&request);
    }
    return cancellation;
}

bool cbm_daemon_frontend_cancellation_matches_request(const char *message, int64_t active_id,
                                                      const char *active_id_str) {
    cbm_jsonrpc_request_t request = {0};
    bool cancellation = frontend_parse_cancellation(message, &request);
    bool matches = cancellation &&
                   cbm_mcp_cancel_request_matches(request.params_raw, active_id, active_id_str);
    if (cancellation) {
        cbm_jsonrpc_request_free(&request);
    }
    return matches;
}

typedef enum {
    FRONTEND_CANCELLATION_NONE = 0,
    FRONTEND_CANCELLATION_STALE = 1,
    FRONTEND_CANCELLATION_QUEUED = 2,
    FRONTEND_CANCELLATION_ACTIVE = 3,
} frontend_cancellation_route_t;

static bool frontend_mark_queued_cancelled(frontend_queue_t *queue, const char *params_raw) {
    for (size_t offset = 0; offset < queue->count; offset++) {
        size_t index = (queue->head + offset) % FRONTEND_QUEUE_CAPACITY;
        frontend_item_t *item = &queue->items[index];
        if (item->has_id && cbm_mcp_cancel_request_matches(params_raw, item->id, item->id_str)) {
            item->cancelled = true;
            return true;
        }
    }
    return false;
}

/* Cancellation is an out-of-band frontend control message. A matching active
 * request routes its exact runtime token (and the lane that runs it) without
 * closing the authenticated session. A queued request is marked and receives a
 * cancellation error without ever reaching the daemon. Stale/invalid targets are
 * ignored. */
static frontend_cancellation_route_t frontend_route_cancellation(
    frontend_state_t *state, const char *message,
    cbm_daemon_runtime_application_token_t *request_token_out, int *lane_out) {
    if (request_token_out) {
        *request_token_out = CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID;
    }
    if (lane_out) {
        *lane_out = 0;
    }
    cbm_jsonrpc_request_t request = {0};
    if (!frontend_parse_cancellation(message, &request)) {
        return FRONTEND_CANCELLATION_NONE;
    }

    frontend_cancellation_route_t route = FRONTEND_CANCELLATION_STALE;
    cbm_mutex_lock(&state->mutex);
    for (int i = 0; i < state->lane_count; i++) {
        frontend_lane_t *lane = &state->lanes[i];
        if (lane->in_request && lane->active_has_id &&
            lane->active_request_token != CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID &&
            cbm_mcp_cancel_request_matches(request.params_raw, lane->active_id,
                                           lane->active_id_str)) {
            if (request_token_out) {
                *request_token_out = lane->active_request_token;
            }
            if (lane_out) {
                *lane_out = i;
            }
            route = FRONTEND_CANCELLATION_ACTIVE;
            break;
        }
    }
    if (route != FRONTEND_CANCELLATION_ACTIVE &&
        (frontend_mark_queued_cancelled(&state->primary, request.params_raw) ||
         frontend_mark_queued_cancelled(&state->pool, request.params_raw))) {
        route = FRONTEND_CANCELLATION_QUEUED;
    }
    cbm_mutex_unlock(&state->mutex);
    cbm_jsonrpc_request_free(&request);
    return route;
}

/* An extra lane's connection is a fresh daemon session: replay the MCP handshake
 * that lane 0 already completed, discarding the replies. */
static bool frontend_lane_replay_handshake(frontend_state_t *state, frontend_lane_t *lane) {
    cbm_mutex_lock(&state->mutex);
    const char *messages[2] = {state->init_message, state->init_note};
    cbm_mutex_unlock(&state->mutex);
    for (int i = 0; i < 2; i++) {
        if (!messages[i]) {
            continue;
        }
        cbm_daemon_runtime_application_token_t token = CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID;
        if (!cbm_daemon_runtime_client_application_token_reserve(lane->client, &token)) {
            return false;
        }
        uint8_t *response = NULL;
        uint32_t response_length = 0;
        cbm_daemon_runtime_application_status_t status = cbm_daemon_application_client_mcp_tagged(
            lane->client, token, messages[i], &response, &response_length,
            FRONTEND_REQUEST_TIMEOUT_MS);
        free(response);
        if (status != CBM_DAEMON_RUNTIME_APPLICATION_OK) {
            return false;
        }
    }
    return true;
}

static void *frontend_worker(void *opaque) {
    frontend_worker_arg_t *arg = opaque;
    frontend_state_t *state = arg->state;
    int lane_index = arg->lane;
    frontend_lane_t *lane = &state->lanes[lane_index];
    for (;;) {
        /* An extra lane replays the handshake before it claims an item: claiming
         * reserves the connection's request token, and the replay needs its own. */
        if (lane_index > 0 && !lane->replayed) {
            cbm_mutex_lock(&state->mutex);
            bool ready = state->init_done && !state->stopping;
            cbm_mutex_unlock(&state->mutex);
            if (ready) {
                lane->replayed = frontend_lane_replay_handshake(state, lane);
                if (!lane->replayed) {
                    break; /* retire this lane; lane 0 still serves the pool */
                }
            } else if (frontend_should_stop(state)) {
                break;
            } else {
                cbm_usleep(FRONTEND_IDLE_WAIT_US);
                continue;
            }
        }
        frontend_item_t item = {0};
        if (!frontend_pop_begin(state, lane_index, &item)) {
            if (frontend_should_stop(state)) {
                break;
            }
#if defined(CBM_ENABLE_TEST_SEAMS) && CBM_ENABLE_TEST_SEAMS
            atomic_fetch_add_explicit(&frontend_test_worker_idle_count, 1, memory_order_release);
#endif
            cbm_usleep(FRONTEND_IDLE_WAIT_US);
            continue;
        }
        uint8_t *response = NULL;
        uint32_t response_length = 0;
        bool failed = false;
        if (item.cancelled) {
            cbm_mutex_lock(&state->out_mutex);
            failed = !frontend_write_cancelled_response(state->out, &item);
            cbm_mutex_unlock(&state->out_mutex);
        } else {
            cbm_daemon_runtime_application_status_t status =
                cbm_daemon_application_client_mcp_tagged(lane->client, item.request_token,
                                                         item.message, &response, &response_length,
                                                         FRONTEND_REQUEST_TIMEOUT_MS);
            cbm_mutex_lock(&state->out_mutex);
            if (status == CBM_DAEMON_RUNTIME_APPLICATION_CANCELLED) {
                failed = !frontend_write_cancelled_response(state->out, &item);
            } else {
                failed = status != CBM_DAEMON_RUNTIME_APPLICATION_OK;
            }
            if (status == CBM_DAEMON_RUNTIME_APPLICATION_OK && !failed && response &&
                response_length > 0) {
                failed = !frontend_write_response(state->out, response, response_length,
                                                  item.content_length_framed);
            }
            cbm_mutex_unlock(&state->out_mutex);
        }
        if (lane_index == 0 && item.initialize && !failed) {
            cbm_mutex_lock(&state->mutex);
            state->init_done = true;
            cbm_mutex_unlock(&state->mutex);
        }
        free(response);
        bool expected_stop = failed && frontend_should_stop(state);
        frontend_end_request(state, lane_index, failed && !expected_stop);
        frontend_item_free(&item);
        atomic_fetch_add_explicit(&state->completed_items, 1, memory_order_release);
        if (failed) {
            if (!expected_stop) {
                /* A failed daemon transport cannot wake a thread blocked in
                 * stdio portably. This frontend owns no state: immediate
                 * process exit closes the kernel IPC handle, which cancels
                 * daemon session ownership without a detached reader or an
                 * unsafe cross-thread fclose. Logging or flushing here could
                 * itself block on agent-owned stdout/stderr. */
                _Exit(EXIT_FAILURE);
            }
            break;
        }
    }
    frontend_worker_mark_done(state, lane_index);
    return NULL;
}

/* FleetHd local patch: tools that only read the index may run on any lane. */
static bool frontend_tool_is_read_only(const char *tool) {
    static const char *const read_only[] = {
        "search_graph",     "query_graph",         "trace_path",      "trace_call_path",
        "get_code_snippet", "get_file_outline",    "search_code",     "get_architecture",
        "get_graph_schema", "check_index_coverage", "list_projects",  "index_status",
        "detect_changes",   "compare_graphs",      NULL,
    };
    for (int i = 0; tool && read_only[i]; i++) {
        if (strcmp(tool, read_only[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool frontend_enqueue(frontend_state_t *state, char *message, bool content_length_framed) {
    size_t length = strlen(message);
    bool has_id = false;
    int64_t id = 0;
    char *id_str = NULL;
    bool pooled = false;
    bool initialize = false;
    cbm_jsonrpc_request_t request = {0};
    if (cbm_jsonrpc_parse(message, &request) == 0) {
        has_id = request.has_id;
        id = request.id;
        if (request.id_str) {
            id_str = cbm_strdup(request.id_str);
        }
        bool identity_copied = !request.id_str || id_str;
        const char *method = request.method;
        if (method && strcmp(method, "initialize") == 0) {
            initialize = true;
        }
        if (state->lane_limit > 1 && method) {
            char **slot = strcmp(method, "initialize") == 0                  ? &state->init_message
                          : strcmp(method, "notifications/initialized") == 0 ? &state->init_note
                                                                             : NULL;
            if (slot) {
                char *copy = cbm_strdup(message);
                cbm_mutex_lock(&state->mutex);
                if (!*slot) {
                    *slot = copy;
                    copy = NULL;
                }
                cbm_mutex_unlock(&state->mutex);
                free(copy);
            } else if (strcmp(method, "tools/call") == 0 && request.params_raw) {
                char *tool = cbm_mcp_get_string_arg(request.params_raw, "name");
                pooled = frontend_tool_is_read_only(tool);
                safe_free(tool);
            }
        }
        cbm_jsonrpc_request_free(&request);
        if (!identity_copied) {
            return false;
        }
    }
    /* A frame that exceeds the whole byte budget can never be admitted no
     * matter how long we wait; only that case is a hard failure. */
    if (length > FRONTEND_QUEUE_BYTES_MAX) {
        cbm_mutex_lock(&state->mutex);
        if (!state->stopping && !state->failed) {
            state->failed = true;
            state->stopping = true;
        }
        cbm_mutex_unlock(&state->mutex);
        free(id_str);
        return false;
    }
    /* A full queue is BACKPRESSURE, not failure. This runs on the stdin reader
     * thread, so blocking here stops further reads and lets the kernel pipe
     * absorb the client's burst — a pipelining client is a client going fast,
     * not a client going wrong. The old code failed the whole session at frame
     * capacity+1, killing it with every buffered response unwritten: any 7+
     * pipelined requests (e.g. an agent issuing parallel tool calls) died with
     * rc=1 and zero output (#1522 follow-up). Waits are bounded only by the
     * stop/fail flags, which every teardown path already sets — the same
     * condition the polling worker and watchdogs key on. */
    for (;;) {
        cbm_mutex_lock(&state->mutex);
        bool stopped = state->stopping || state->failed;
        if (stopped) {
            cbm_mutex_unlock(&state->mutex);
            free(id_str);
            return false;
        }
        frontend_queue_t *queue = pooled ? &state->pool : &state->primary;
        bool capacity = queue->count < FRONTEND_QUEUE_CAPACITY &&
                        state->queued_bytes <= FRONTEND_QUEUE_BYTES_MAX &&
                        length <= FRONTEND_QUEUE_BYTES_MAX - state->queued_bytes;
        if (capacity) {
            size_t tail = (queue->head + queue->count) % FRONTEND_QUEUE_CAPACITY;
            queue->items[tail] = (frontend_item_t){
                .message = message,
                .length = length,
                .content_length_framed = content_length_framed,
                .has_id = has_id,
                .id = id,
                .id_str = id_str,
                .initialize = initialize,
            };
            queue->count++;
            state->queued_bytes += length;
            cbm_mutex_unlock(&state->mutex);
            return true;
        }
        cbm_mutex_unlock(&state->mutex);
        cbm_usleep(FRONTEND_WAIT_US);
    }
}

static bool frontend_stop_begin(frontend_state_t *state) {
    cbm_mutex_lock(&state->mutex);
    state->stopping = true;
    cbm_mutex_unlock(&state->mutex);
    /* Retain the client allocation until the worker is joined. This covers the
     * boundary where the worker has claimed an item but has not yet entered the
     * runtime exchange: a late call observes closing instead of freed memory. */
    bool begun = true;
    for (int i = 0; i < state->lane_count; i++) {
        begun = cbm_daemon_runtime_client_close_begin(state->lanes[i].client) && begun;
    }
    return begun;
}

static bool frontend_cancel_for_maintenance(void *opaque) {
    frontend_state_t *state = opaque;
    cbm_daemon_runtime_application_token_t tokens[CBM_DAEMON_FRONTEND_MAX_LANES];
    int count = 0;
    cbm_mutex_lock(&state->mutex);
    state->stopping = true;
    for (int i = 0; i < state->lane_count; i++) {
        tokens[i] = state->lanes[i].in_request ? state->lanes[i].active_request_token
                                               : CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID;
        count = i + 1;
    }
    cbm_mutex_unlock(&state->mutex);
    bool accepted = false;
    for (int i = 0; i < count; i++) {
        if (tokens[i] != CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID &&
            cbm_daemon_runtime_client_application_cancel(state->lanes[i].client, tokens[i]) ==
                CBM_DAEMON_RUNTIME_CANCEL_ACCEPTED) {
            accepted = true;
        }
    }
    return accepted;
}

/* Opens one more lane when read-only calls are queued and every open lane is busy. Runs
 * on the stdin reader thread, which is the only thread that adds lanes. */
static void frontend_maybe_open_lane(frontend_state_t *state) {
    cbm_mutex_lock(&state->mutex);
    int busy = 0;
    for (int i = 0; i < state->lane_count; i++) {
        busy += state->lanes[i].in_request ? 1 : 0;
    }
    bool wanted = state->open_lane && state->init_done && !state->stopping &&
                  state->lane_count < state->lane_limit &&
                  state->pool.count > (size_t)(state->lane_count - busy);
    cbm_mutex_unlock(&state->mutex);
    if (!wanted) {
        return;
    }
    cbm_daemon_runtime_client_t *client = state->open_lane(state->open_context);
    cbm_mutex_lock(&state->mutex);
    if (!client || state->stopping) {
        /* A refused connection (the daemon is near its client limit) stops further
         * attempts for this session. */
        state->lane_limit = state->lane_count;
        cbm_mutex_unlock(&state->mutex);
        if (client) {
            (void)cbm_daemon_runtime_client_close(client, FRONTEND_CLOSE_TIMEOUT_MS);
        }
        return;
    }
    int index = state->lane_count;
    state->lanes[index] = (frontend_lane_t){.client = client};
    state->worker_args[index] = (frontend_worker_arg_t){.state = state, .lane = index};
    state->lane_count++;
    cbm_mutex_unlock(&state->mutex);
    char lanes[16];
    (void)snprintf(lanes, sizeof(lanes), "%d", index + 1);
    cbm_log_info("mcp.frontend_lane_opened", "lanes", lanes);
    state->worker_started[index] = cbm_thread_create(&state->workers[index], 0, frontend_worker,
                                                     &state->worker_args[index]) == 0;
    if (!state->worker_started[index]) {
        frontend_worker_mark_done(state, index);
    }
}

int cbm_daemon_frontend_mcp_run(cbm_daemon_runtime_client_t *client,
                                cbm_version_cohort_manager_t *cohort_manager, FILE *in, FILE *out) {
    return cbm_daemon_frontend_mcp_run_pool(client, NULL, NULL, 1, cohort_manager, in, out);
}

int cbm_daemon_frontend_mcp_run_pool(cbm_daemon_runtime_client_t *client,
                                     cbm_daemon_frontend_open_lane_fn open_lane,
                                     void *open_context, int lane_limit,
                                     cbm_version_cohort_manager_t *cohort_manager, FILE *in,
                                     FILE *out) {
    if (!client || !cohort_manager || !in || !out || lane_limit < 1 ||
        lane_limit > CBM_DAEMON_FRONTEND_MAX_LANES) {
        return -1;
    }
    frontend_state_t state = {
        .cohort_manager = cohort_manager,
        .out = out,
        .lane_count = 1,
        .lane_limit = open_lane ? lane_limit : 1,
        .open_lane = open_lane,
        .open_context = open_context,
    };
    state.lanes[0].client = client;
    cbm_mutex_init(&state.mutex);
    cbm_mutex_init(&state.out_mutex);
    atomic_init(&state.completed_items, 0);
    cbm_daemon_maintenance_monitor_t *maintenance_monitor =
        frontend_maintenance_monitor_start(cohort_manager, frontend_cancel_for_maintenance, &state,
                                           EXIT_SUCCESS, "MCP frontend", true);
    if (!maintenance_monitor) {
        cbm_mutex_destroy(&state.out_mutex);
        cbm_mutex_destroy(&state.mutex);
        for (int i = 0; i < state.lane_count; i++) {
            (void)cbm_daemon_runtime_client_close(state.lanes[i].client, FRONTEND_CLOSE_TIMEOUT_MS);
        }
        return -1;
    }
    state.worker_args[0] = (frontend_worker_arg_t){.state = &state, .lane = 0};
    state.worker_started[0] =
        cbm_thread_create(&state.workers[0], 0, frontend_worker, &state.worker_args[0]) == 0;
    if (!state.worker_started[0]) {
        if (!cbm_daemon_maintenance_monitor_stop(&maintenance_monitor)) {
            _Exit(EXIT_FAILURE);
        }
        cbm_mutex_destroy(&state.out_mutex);
        cbm_mutex_destroy(&state.mutex);
        (void)cbm_daemon_runtime_client_close(client, FRONTEND_CLOSE_TIMEOUT_MS);
        return -1;
    }

    int result = 0;
    bool close_begun = false;
    bool clean_eof = false;
    for (;;) {
        char *message = NULL;
        bool content_length_framed = false;
        int read_status = cbm_mcp_read_message(in, &message, &content_length_framed);
        if (read_status <= 0) {
            result = read_status < 0 ? -1 : 0;
            clean_eof = read_status == 0;
            free(message);
            break;
        }
        cbm_daemon_runtime_application_token_t cancel_token =
            CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID;
        int cancel_lane = 0;
        frontend_cancellation_route_t cancellation =
            frontend_route_cancellation(&state, message, &cancel_token, &cancel_lane);
        if (cancellation != FRONTEND_CANCELLATION_NONE) {
            free(message);
            if (cancellation == FRONTEND_CANCELLATION_ACTIVE) {
                cbm_daemon_runtime_cancel_result_t cancelled =
                    cbm_daemon_runtime_client_application_cancel(state.lanes[cancel_lane].client,
                                                                 cancel_token);
                if (cancelled == CBM_DAEMON_RUNTIME_CANCEL_ERROR) {
                    result = -1;
                    close_begun = frontend_stop_begin(&state);
                    break;
                }
            }
            /* A queued cancellation is represented on that queue item; stale
             * and malformed targets are intentionally side-effect free. */
            continue;
        }
        if (!frontend_enqueue(&state, message, content_length_framed)) {
            free(message);
            result = -1;
            break;
        }
        frontend_maybe_open_lane(&state);
    }

    if (clean_eof && !close_begun) {
        frontend_input_closed(&state);
        /* EOF ends INPUT, not accepted work: as long as queued items keep
         * completing, every already-enqueued request still receives its
         * response, exactly like the pre-daemon stdio server, which
         * processed the whole stream before exiting. The window resets on
         * each completed item, so it bounds STUCKNESS, not batch length: a
         * request making no progress for the full window is cancelled by the
         * stop path below and the frontend still exits cleanly. */
        uint64_t drained = atomic_load_explicit(&state.completed_items, memory_order_acquire);
        uint64_t drain_deadline = cbm_now_ms() + FRONTEND_EOF_DRAIN_MS;
        while (!frontend_worker_is_done(&state) && cbm_now_ms() < drain_deadline) {
            cbm_usleep(FRONTEND_WAIT_US);
            uint64_t now_drained =
                atomic_load_explicit(&state.completed_items, memory_order_acquire);
            if (now_drained != drained) {
                drained = now_drained;
                drain_deadline = cbm_now_ms() + FRONTEND_EOF_DRAIN_MS;
            }
        }
    }
    if (!close_begun) {
        close_begun = frontend_stop_begin(&state);
    }
    frontend_join_watchdog_t watchdog;
    cbm_thread_t watchdog_thread;
    bool watchdog_started = false;
    if (!frontend_worker_is_done(&state)) {
        atomic_init(&watchdog.complete, false);
        if (cbm_thread_create(&watchdog_thread, 0, frontend_join_watchdog, &watchdog) != 0) {
            _Exit(EXIT_FAILURE);
        }
        watchdog_started = true;
    }
    for (int i = 0; i < state.lane_count; i++) {
        if (state.worker_started[i] && cbm_thread_join(&state.workers[i]) != 0) {
            /* Preserve every object the worker may still reference. */
            _Exit(EXIT_FAILURE);
        }
    }
    if (watchdog_started) {
        atomic_store_explicit(&watchdog.complete, true, memory_order_release);
        if (cbm_thread_join(&watchdog_thread) != 0) {
            _Exit(EXIT_FAILURE);
        }
    }
    if (!cbm_daemon_maintenance_monitor_stop(&maintenance_monitor)) {
        _Exit(EXIT_FAILURE);
    }
    if (close_begun) {
        for (int i = 0; i < state.lane_count; i++) {
            (void)cbm_daemon_runtime_client_close_finish(state.lanes[i].client,
                                                         FRONTEND_CLOSE_TIMEOUT_MS);
        }
    } else {
        result = -1;
    }
    for (size_t i = 0; i < FRONTEND_QUEUE_CAPACITY; i++) {
        frontend_item_free(&state.primary.items[i]);
        frontend_item_free(&state.pool.items[i]);
    }
    free(state.init_message);
    free(state.init_note);
    if (state.failed) {
        result = -1;
    }
    cbm_mutex_destroy(&state.out_mutex);
    cbm_mutex_destroy(&state.mutex);
    return result;
}
