/************************************************************\
 * Copyright 2019 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* event.c - job state machine and eventlog commit batching
 *
 * event_job_update() implements the job state machine described
 * in RFC 21.  This function is called when an event occurs for a job,
 * to drive changes to job state and flags.  For example, an "alloc"
 * event transitions a job from SCHED to RUN state.
 *
 * event_job_action() is called after event_job_update().  It takes actions
 * appropriate for job state and flags.  For example, in RUN state,
 * job shells are started.
 *
 * Events are logged in the job eventlog in the KVS.  For performance,
 * multiple updates may be combined into one commit.  The location of
 * the job eventlog and its contents are described in RFC 16 and RFC 18.
 *
 * The function event_job_post_pack() posts an event to a job, running
 * event_job_update(), event_job_action(), and committing the event to
 * the job eventlog, in a delayed batch.
 *
 * Notes:
 * - A KVS commit failure is handled as fatal to the job-manager
 * - event_job_action() is idempotent
 * - event_ctx_destroy() flushes batched eventlog updates before returning
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <jansson.h>
#include <flux/core.h>
#include <time.h>

#include "src/common/libczmqcontainers/czmq_containers.h"
#include "src/common/libeventlog/eventlog.h"
#include "ccan/ptrint/ptrint.h"

#include "alloc.h"
#include "start.h"
#include "drain.h"
#include "journal.h"
#include "wait.h"
#include "prioritize.h"
#include "annotate.h"
#include "jobtap-internal.h"

#include "event.h"

const double batch_timeout = 0.01;

struct event {
    struct job_manager *ctx;
    struct event_batch *batch;
    flux_watcher_t *timer;
    zlist_t *pending;
    zlist_t *pub_futures;
    zhashx_t *evindex;
};

struct event_batch {
    struct event *event;
    flux_kvs_txn_t *txn;
    flux_future_t *f;
    json_t *state_trans;
    zlist_t *responses; // responses deferred until batch complete
};

static struct event_batch *event_batch_create (struct event *event);
static void event_batch_destroy (struct event_batch *batch);

/* Batch commit has completed.
 * If there was a commit error, log it and stop the reactor.
 * Destroy 'batch'.
 */
static void commit_continuation (flux_future_t *f, void *arg)
{
    struct event_batch *batch = arg;
    struct event *event = batch->event;
    struct job_manager *ctx = event->ctx;

    if (flux_future_get (batch->f, NULL) < 0) {
        flux_log_error (ctx->h, "%s: eventlog update failed", __FUNCTION__);
        flux_reactor_stop_error (flux_get_reactor (ctx->h));
    }
    zlist_remove (event->pending, batch);
    event_batch_destroy (batch);
}

/* job-state event publish has completed.
 * If there was a publish error, log it and stop the reactor.
 * Destroy 'f'.
 */
static void publish_continuation (flux_future_t *f, void *arg)
{
    struct event *event = arg;
    struct job_manager *ctx = event->ctx;

    if (flux_future_get (f, NULL) < 0) {
        flux_log_error (ctx->h, "%s: event publish failed", __FUNCTION__);
        flux_reactor_stop_error (flux_get_reactor (ctx->h));
    }
    zlist_remove (event->pub_futures, f);
    flux_future_destroy (f);
}

/* Close the current batch, if any, and commit it.
 */
static void event_batch_commit (struct event *event)
{
    struct event_batch *batch = event->batch;
    struct job_manager *ctx = event->ctx;

    if (batch) {
        event->batch = NULL;
        /* note that job-state events will be sent after the KVS
         * commit, as we want to ensure anyone who receives a
         * job-state transition event will be able to read the
         * corresponding event in the KVS.
         */
        if (batch->txn) {
            if (!(batch->f = flux_kvs_commit (ctx->h, NULL, 0, batch->txn)))
                goto error;
            if (flux_future_then (batch->f, -1., commit_continuation, batch) < 0)
                goto error;
            if (zlist_append (event->pending, batch) < 0)
                goto nomem;
        }
        else { // just publish events & send responses and be done
            event_batch_destroy (batch);
        }
    }
    return;
nomem:
    errno = ENOMEM;
error: // unlikely (e.g. ENOMEM)
    flux_log_error (ctx->h, "%s: aborting reactor", __FUNCTION__);
    flux_reactor_stop_error (flux_get_reactor (ctx->h));
    event_batch_destroy (batch);
}

static void timer_cb (flux_reactor_t *r, flux_watcher_t *w, int revents, void *arg)
{
    struct job_manager *ctx = arg;
    event_batch_commit (ctx->event);
}

void event_publish (struct event *event, const char *topic,
                    const char *key, json_t *o)
{
    struct job_manager *ctx = event->ctx;
    flux_future_t *f;

    if (!(f = flux_event_publish_pack (ctx->h, topic, 0, "{s:O?}", key, o))) {
        flux_log_error (ctx->h, "%s: flux_event_publish_pack", __FUNCTION__);
        goto error;
    }
    if (flux_future_then (f, -1., publish_continuation, event) < 0) {
        flux_future_destroy (f);
        flux_log_error (ctx->h, "%s: flux_future_then", __FUNCTION__);
        goto error;
    }
    if (zlist_append (event->pub_futures, f) < 0) {
        flux_future_destroy (f);
        flux_log_error (ctx->h, "%s: zlist_append", __FUNCTION__);
        goto error;
    }
    return;
error:
    flux_reactor_stop_error (flux_get_reactor (ctx->h));
}

/* Besides cleaning up, this function has the following side effects:
 * - publish state transition event (if any)
 * - send listener responses (only under error scenarios, should be
 *   sent in event_batch_commit()).
 * - respond to deferred responses (if any)
 */
static void event_batch_destroy (struct event_batch *batch)
{
    if (batch) {
        int saved_errno = errno;

        flux_kvs_txn_destroy (batch->txn);
        if (batch->f)
            (void)flux_future_wait_for (batch->f, -1);
        if (batch->state_trans) {
            if (json_array_size (batch->state_trans) > 0)
                event_publish (batch->event,
                               "job-state",
                               "transitions",
                               batch->state_trans);
            json_decref (batch->state_trans);
        }
        if (batch->responses) {
            flux_msg_t *msg;
            flux_t *h = batch->event->ctx->h;
            while ((msg = zlist_pop (batch->responses))) {
                if (flux_send (h, msg, 0) < 0)
                    flux_log_error (h, "error sending batch response");
                flux_msg_decref (msg);
            }
            zlist_destroy (&batch->responses);
        }
        flux_future_destroy (batch->f);
        free (batch);
        errno = saved_errno;
    }
}

static struct event_batch *event_batch_create (struct event *event)
{
    struct event_batch *batch;

    if (!(batch = calloc (1, sizeof (*batch))))
        return NULL;
    batch->event = event;
    return batch;
}

/* Create a new "batch" if there is none.
 * No-op if batch already started.
 */
static int event_batch_start (struct event *event)
{
    if (!event->batch) {
        if (!(event->batch = event_batch_create (event)))
            return -1;
        flux_timer_watcher_reset (event->timer, batch_timeout, 0.);
        flux_watcher_start (event->timer);
    }
    return 0;
}

static int event_batch_commit_event (struct event *event,
                                     struct job *job,
                                     json_t *entry)
{
    char key[64];
    char *entrystr = NULL;

    if (event_batch_start (event) < 0)
        return -1;
    if (flux_job_kvs_key (key, sizeof (key), job->id, "eventlog") < 0)
        return -1;
    if (!event->batch->txn && !(event->batch->txn = flux_kvs_txn_create ()))
        return -1;
    if (!(entrystr = eventlog_entry_encode (entry)))
        return -1;
    if (flux_kvs_txn_put (event->batch->txn,
                          FLUX_KVS_APPEND,
                          key,
                          entrystr) < 0) {
        free (entrystr);
        return -1;
    }
    free (entrystr);
    return 0;
}

int event_batch_pub_state (struct event *event, struct job *job,
                           double timestamp)
{
    json_t *o;

    if (event_batch_start (event) < 0)
        goto error;
    if (!event->batch->state_trans) {
        if (!(event->batch->state_trans = json_array ()))
            goto nomem;
    }
    if (!(o = json_pack ("[I,s,f]",
                         job->id,
                         flux_job_statetostr (job->state, "L"),
                         timestamp)))
        goto nomem;
    if (json_array_append_new (event->batch->state_trans, o)) {
        json_decref (o);
        goto nomem;
    }
    return 0;
nomem:
    errno = ENOMEM;
error:
    return -1;
}

int event_batch_respond (struct event *event, const flux_msg_t *msg)
{
    if (event_batch_start (event) < 0)
        return -1;
    if (!event->batch->responses) {
        if (!(event->batch->responses = zlist_new ()))
            goto nomem;
    }
    if (zlist_append (event->batch->responses,
                      (void *)flux_msg_incref (msg)) < 0) {
        flux_msg_decref (msg);
        goto nomem;
    }
    return 0;
nomem:
    errno = ENOMEM;
    return -1;
}

int event_job_action (struct event *event, struct job *job)
{
    struct job_manager *ctx = event->ctx;

    switch (job->state) {
        case FLUX_JOB_STATE_NEW:
            break;
        case FLUX_JOB_STATE_DEPEND:
            /*  Post the "depend" event when the job has no more dependency
             *   references outstanding and a depend event hasn't already
             *   been posted.
             *
             *  The job->depend_posted flag is required in the case that
             *   events are being queued and handled asynchronously, and
             *   therefore the post of the "depend" event does not immediately
             *   transition the job to the PRIORITY state.
             */
            if (job_dependency_count (job) == 0
                && !job->depend_posted) {
                if (event_job_post_pack (event, job, "depend", 0, NULL) < 0)
                    return -1;
                job->depend_posted = 1;
            }
            break;
        case FLUX_JOB_STATE_PRIORITY:
            /*
             * In the event we have re-entered this state from the
             * SCHED state, dequeue the job first.
             */
            alloc_dequeue_alloc_request (ctx->alloc, job);
            break;
        case FLUX_JOB_STATE_SCHED:
            if (alloc_enqueue_alloc_request (ctx->alloc, job) < 0)
                return -1;
            if (alloc_queue_recalc_pending (ctx->alloc) < 0)
                return -1;
            break;
        case FLUX_JOB_STATE_RUN:
            /*
             *  If job->request_refcount is nonzero then a prolog action
             *   is still in progress so do not send start request.
             */
            if (!job->perilog_active
                && start_send_request (ctx->start, job) < 0)
                return -1;
            break;
        case FLUX_JOB_STATE_CLEANUP:
            if (job->alloc_pending)
                alloc_cancel_alloc_request (ctx->alloc, job);
            if (job->alloc_queued)
                alloc_dequeue_alloc_request (ctx->alloc, job);

            /* N.B. start_pending indicates that the start request is still
             * expecting responses.  The final response is the 'release'
             * response with final=true.  Thus once the flag is clear,
             * it is safe to release all resources to the scheduler.
             */
            if (job->has_resources
                && !job->perilog_active
                && !job->alloc_bypass
                && !job->start_pending
                && !job->free_pending) {
                if (alloc_send_free_request (ctx->alloc, job) < 0)
                    return -1;
            }

            /* Post cleanup event when cleanup is complete.
             */
            if (!job->alloc_queued && !job->alloc_pending
                                   && !job->free_pending
                                   && !job->start_pending
                                   && !job->has_resources) {

                if (event_job_post_pack (event, job, "clean", 0, NULL) < 0)
                    return -1;
            }
            break;
        case FLUX_JOB_STATE_INACTIVE:
            if ((job->flags & FLUX_JOB_WAITABLE))
                wait_notify_inactive (ctx->wait, job);
            zhashx_delete (ctx->active_jobs, &job->id);
            drain_check (ctx->drain);
            break;
    }
    return 0;
}

static int event_submit_context_decode (json_t *context,
                                        int *urgency,
                                        uint32_t *userid,
                                        int *flags)
{
    if (json_unpack (context, "{ s:i s:i s:i }",
                     "urgency", urgency,
                     "userid", userid,
                     "flags", flags) < 0) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int event_priority_context_decode (json_t *context,
                                          int64_t *priority)
{
    if (json_unpack (context, "{ s:I }", "priority", priority) < 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static int event_urgency_context_decode (json_t *context,
                                         int *urgency)
{
    if (json_unpack (context, "{ s:i }", "urgency", urgency) < 0) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int event_exception_context_decode (json_t *context,
                                           int *severity)
{
    if (json_unpack (context, "{ s:i }", "severity", severity) < 0) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int event_release_context_decode (json_t *context,
                                         int *final)
{
    *final = 0;

    if (json_unpack (context, "{ s:b }", "final", &final) < 0) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int event_handle_dependency (struct job *job,
                                    const char *cmd,
                                    json_t *context)
{
    const char *desc;

    if (json_unpack (context, "{s:s}", "description", &desc) < 0) {
        errno = EPROTO;
        return -1;
    }
    if (strcmp (cmd, "add") == 0)
        return job_dependency_add (job, desc);
    else if (strcmp (cmd, "remove") == 0)
        return job_dependency_remove (job, desc);
    else {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int event_handle_set_flags (struct job *job,
                                   json_t *context)
{
    json_t *o = NULL;
    size_t index;
    json_t *value;

    if (json_unpack (context, "{s:o}", "flags", &o) < 0) {
        errno = EPROTO;
        return -1;
    }
    json_array_foreach (o, index, value) {
        if (job_flag_set (job, json_string_value (value)) < 0) {
            errno = EPROTO;
            return -1;
        }
    }
    return 0;
}

/*  Handle an prolog-* or epilog-* event
 */
static int event_handle_perilog (struct job *job,
                                 const char *cmd,
                                 json_t *context)
{
    if (strcmp (cmd, "start") == 0) {
        if (job->perilog_active == UINT8_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        job->perilog_active++;
    }
    else if (strcmp (cmd, "finish") == 0) {
        if (job->perilog_active > 0)
            job->perilog_active--;
    }
    else {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static int event_handle_memo (struct job *job, json_t *o)
{
    return annotations_update (job, "user", o);
}

/*  Return a callback topic string for the current job state
 *
 *   NOTE: 'job.state.new' and 'job.state.depend' are not currently used
 *    since jobs do not transition through these states in
 *    event_job_post_pack().
 */
static const char *state_topic (struct job *job)
{
    switch (job->state) {
        case FLUX_JOB_STATE_NEW:
            return "job.state.new";
        case FLUX_JOB_STATE_DEPEND:
            return "job.state.depend";
        case FLUX_JOB_STATE_PRIORITY:
            return "job.state.priority";
        case FLUX_JOB_STATE_SCHED:
            return "job.state.sched";
        case FLUX_JOB_STATE_RUN:
            return "job.state.run";
        case FLUX_JOB_STATE_CLEANUP:
            return "job.state.cleanup";
        case FLUX_JOB_STATE_INACTIVE:
            return "job.state.inactive";
    }
    /* NOTREACHED */
    return "job.state.none";
}

/* This function implements state transitions per RFC 21.
 * On a fatal exception or cleanup event, capture the event in job->end_event.
 */
int event_job_update (struct job *job, json_t *event)
{
    double timestamp;
    const char *name;
    json_t *context;

    if (eventlog_entry_parse (event, &timestamp, &name, &context) < 0)
        goto error;

    if (!strcmp (name, "submit")) {
        if (job->state != FLUX_JOB_STATE_NEW)
            goto inval;
        job->t_submit = timestamp;
        if (event_submit_context_decode (context,
                                         &job->urgency,
                                         &job->userid,
                                         &job->flags) < 0)
            goto error;
        job->state = FLUX_JOB_STATE_DEPEND;
    }
    else if (!strncmp (name, "dependency-", 11)) {
        if (job->state != FLUX_JOB_STATE_DEPEND)
            goto inval;
        if (event_handle_dependency (job, name+11, context) < 0)
            goto error;
    }
    else if (!strcmp (name, "set-flags")) {
        if (event_handle_set_flags (job, context) < 0)
            goto error;
    }
    else if (!strcmp (name, "memo")) {
        if (event_handle_memo (job, context) < 0)
            goto error;
    }
    else if (!strcmp (name, "depend")) {
        if (job->state != FLUX_JOB_STATE_DEPEND)
            goto inval;
        job->state = FLUX_JOB_STATE_PRIORITY;
    }
    else if (!strcmp (name, "priority")) {
        if (job->state != FLUX_JOB_STATE_PRIORITY
            && job->state != FLUX_JOB_STATE_SCHED)
            goto inval;
        if (event_priority_context_decode (context, &job->priority) < 0)
            goto error;
        job->state = FLUX_JOB_STATE_SCHED;
    }
    else if (!strcmp (name, "urgency")) {
        if (event_urgency_context_decode (context, &job->urgency) < 0)
            goto error;
    }
    else if (!strcmp (name, "exception")) {
        int severity;
        if (job->state == FLUX_JOB_STATE_NEW
            || job->state == FLUX_JOB_STATE_INACTIVE)
            goto inval;
        if (event_exception_context_decode (context, &severity) < 0)
            goto error;
        if (severity == 0) {
            if (!job->end_event)
                job->end_event = json_incref (event);

            job->state = FLUX_JOB_STATE_CLEANUP;
        }
    }
    else if (!strcmp (name, "alloc")) {
        if (job->state != FLUX_JOB_STATE_SCHED
            && job->state != FLUX_JOB_STATE_CLEANUP)
            goto inval;
        job->has_resources = 1;
        if (job->state == FLUX_JOB_STATE_SCHED)
            job->state = FLUX_JOB_STATE_RUN;
    }
    else if (!strcmp (name, "free")) {
        if (job->state != FLUX_JOB_STATE_CLEANUP
            || !job->has_resources)
            goto inval;
        job->has_resources = 0;
    }
    else if (!strcmp (name, "finish")) {
        if (job->state != FLUX_JOB_STATE_RUN
            && job->state != FLUX_JOB_STATE_CLEANUP)
            goto inval;
        if (job->state == FLUX_JOB_STATE_RUN) {
            if (!job->end_event)
                job->end_event = json_incref (event);

            job->state = FLUX_JOB_STATE_CLEANUP;
        }
    }
    else if (!strcmp (name, "release")) {
        int final;
        if (job->state != FLUX_JOB_STATE_RUN
            && job->state != FLUX_JOB_STATE_CLEANUP)
            goto inval;
        if (event_release_context_decode (context, &final) < 0)
            goto error;
        if (final && job->state == FLUX_JOB_STATE_RUN)
            goto inval;
    }
    else if (!strcmp (name, "clean")) {
        if (job->state != FLUX_JOB_STATE_CLEANUP)
            goto inval;
        job->state = FLUX_JOB_STATE_INACTIVE;
    }
    else if (!strncmp (name, "prolog-", 7)) {
        if (job->start_pending)
            goto inval;
        if (event_handle_perilog (job, name+7, context) < 0)
            goto error;
    }
    else if (!strncmp (name, "epilog-", 7)) {
        if (job->state != FLUX_JOB_STATE_CLEANUP)
            goto inval;
        if (event_handle_perilog (job, name+7, context) < 0)
            goto error;
    }
    else if (!strcmp (name, "flux-restart")) {
        /* The flux-restart event is currently only posted to jobs in
         * SCHED state since that is the only state transition defined
         * for the event in RFC21.  In the future, other transitions
         * may be defined.
         */
        if (job->state == FLUX_JOB_STATE_SCHED)
            job->state = FLUX_JOB_STATE_PRIORITY;
    }
    return 0;
inval:
    errno = EINVAL;
error:
    return -1;
}

/*  Call jobtap plugin for event if necessary.
 *  Currently jobtap plugins are called only on state transitions or
 *   update of job urgency via "urgency" event.
 */
static int event_jobtap_call (struct event *event,
                              struct job *job,
                              const char *name,
                              json_t *entry,
                              flux_job_state_t old_state)
{

    /*  Notify any subscribers of all events, separately from
     *   special cases for state change and urgency events below.
     */
    if (jobtap_notify_subscribers (event->ctx->jobtap,
                                   job,
                                   name,
                                   "{s:O}",
                                   "entry", entry) < 0)
            flux_log (event->ctx->h, LOG_ERR,
                      "jobtap: event.%s callback failed for job %ju",
                      name,
                      (uintmax_t) job->id);

    if (job->state != old_state) {
        /*
         *  Call plugin callback on state change
         */
        return jobtap_call (event->ctx->jobtap,
                            job,
                            state_topic (job),
                            "{s:O s:i}",
                            "entry", entry,
                            "prev_state", old_state);
    }
    else if (strcmp (name, "urgency") == 0) {
        /*
         *  An urgency update ocurred. Get new priority value from plugin
         *   and reprioritize job if there was a change.
         *   (Note: reprioritize_job() is a noop if job is not in PRIORITY
         *    or SCHED state)
         */
        int64_t priority = -1;
        if (jobtap_get_priority (event->ctx->jobtap, job, &priority) < 0
            || reprioritize_job (event->ctx, job, priority) < 0) {
            flux_log_error (event->ctx->h,
                            "jobtap: urgency: %ju: priority update failed",
                            (uintmax_t) job->id);
            return -1;
        }
    }
    return 0;
}

static int event_job_cache (struct event *event,
                            struct job *job,
                            const char *name)
{
    int id;
    /*  Get a unique event id for event 'name' and stash it with the job */
    if ((id = event_index (event, name)) < 0)
        return -1;
    return job_event_id_set (job, id);
}

int event_job_post_entry (struct event *event,
                          struct job *job,
                          const char *name,
                          int flags,
                          json_t *entry)
{
    int rc;
    flux_job_state_t old_state = job->state;
    int eventlog_seq = job->eventlog_seq;

    /*  Journal event sequence should match actual sequence of events
     *   in the job eventlog, so set eventlog_seq to -1 with
     *   EVENT_NO_COMMIT and do not advance job->eventlog_seq.
     *
     *  However, if EVENT_FORCE_SEQUENCE flag is supplied, then we
     *   do set and advance an actual sequence number (the event may
     *   already be in the eventlog such as the "submit" event)
     */
    if ((flags & EVENT_NO_COMMIT) && !(flags & EVENT_FORCE_SEQUENCE))
        eventlog_seq = -1;

    /* call before eventlog_seq increment below */
    if (journal_process_event (event->ctx->journal,
                               job->id,
                               eventlog_seq,
                               name,
                               entry) < 0)
        return -1;
    if (event_job_update (job, entry) < 0) // modifies job->state
        return -1;
    /*
     *  Only advance eventlog_seq if one was set for this job
     */
    if (eventlog_seq != -1)
        job->eventlog_seq++;
    if (event_job_cache (event, job, name) < 0)
        return -1;
    if (!(flags & EVENT_NO_COMMIT)
        && event_batch_commit_event (event, job, entry) < 0)
        return -1;
    if (job->state != old_state) {
        double timestamp;
        if (json_unpack (entry, "{s:f}", "timestamp", &timestamp) < 0) {
            errno = EINVAL;
            return -1;
        }
        if (event_batch_pub_state (event, job, timestamp) < 0)
            return -1;
    }

    /* Keep track of running job count.
     * If queue reaches idle state, event_job_action() triggers any waiters.
     */
    if ((job->state & FLUX_JOB_STATE_RUNNING)
        && !(old_state & FLUX_JOB_STATE_RUNNING))
        event->ctx->running_jobs++;
    else if (!(job->state & FLUX_JOB_STATE_RUNNING)
             && (old_state & FLUX_JOB_STATE_RUNNING))
        event->ctx->running_jobs--;

    /*  N.B. Job may recursively call this function from event_jobtap_call()
     *   which may end up destroying the job before returning from the
     *   function. Until the recursive nature of these functions is
     *   fixed via a true event queue, we must take a reference on the
     *   job and release after event_job_action() to avoid the potential
     *   for use-after-free.
     */
    job_incref (job);

    /*  Ensure jobtap call happens after the current state is published,
     *   in case any plugin callback causes a transition to a new state,
     *   but the call needs to occur before event_job_action() which may
     *   itself cause the job to recursively enter a new state.
     *
     *  Note: Failure from the jobtap call is currently ignored, but will
     *   be logged in jobtap_call(). The goal is to do something with the
     *   errors at some point (perhaps raise a job exception).
     */
    (void) event_jobtap_call (event, job, name, entry, old_state);

    rc = event_job_action (event, job);
    job_decref (job);
    return rc;
}

int event_job_post_vpack (struct event *event,
                          struct job *job,
                          const char *name,
                          int flags,
                          const char *context_fmt,
                          va_list ap)
{
    json_t *entry = NULL;
    int saved_errno;
    int rc;

    if (job->state == FLUX_JOB_STATE_NEW) {
        errno = EAGAIN;
        return -1;
    }
    if (!(entry = eventlog_entry_vpack (0., name, context_fmt, ap)))
        return -1;
    rc = event_job_post_entry (event, job, name, flags, entry);

    saved_errno = errno;
    json_decref (entry);
    errno = saved_errno;
    return rc;
}

int event_job_post_pack (struct event *event,
                         struct job *job,
                         const char *name,
                         int flags,
                         const char *context_fmt,
                         ...)
{
    int rc;
    va_list ap;

    va_start (ap, context_fmt);
    rc = event_job_post_vpack (event, job, name, flags, context_fmt, ap);
    va_end (ap);
    return rc;
}

/* Finalizes in-flight batch KVS commits and event pubs (synchronously).
 */
void event_ctx_destroy (struct event *event)
{
    if (event) {
        int saved_errno = errno;
        flux_watcher_destroy (event->timer);
        event_batch_commit (event);
        if (event->pending) {
            struct event_batch *batch;
            while ((batch = zlist_pop (event->pending)))
                event_batch_destroy (batch); // N.B. can append to pub_futures
        }
        zlist_destroy (&event->pending);
        if (event->pub_futures) {
            flux_future_t *f;
            while ((f = zlist_pop (event->pub_futures))) {
                if (flux_future_get (f, NULL) < 0)
                    flux_log_error (event->ctx->h,
                                    "error publishing job-state event");
                flux_future_destroy (f);
            }
        }
        zlist_destroy (&event->pub_futures);
        zhashx_destroy (&event->evindex);
        free (event);
        errno = saved_errno;
    }
}

struct event *event_ctx_create (struct job_manager *ctx)
{
    struct event *event;

    if (!(event = calloc (1, sizeof (*event))))
        return NULL;
    event->ctx = ctx;
    if (!(event->timer = flux_timer_watcher_create (flux_get_reactor (ctx->h),
                                                    0.,
                                                    0.,
                                                    timer_cb,
                                                    ctx)))
        goto error;
    if (!(event->pending = zlist_new ()))
        goto nomem;
    if (!(event->pub_futures = zlist_new ()))
        goto nomem;
    if (!(event->evindex = zhashx_new ()))
        goto nomem;

    return event;
nomem:
    errno = ENOMEM;
error:
    event_ctx_destroy (event);
    return NULL;
}

int event_index (struct event *event, const char *name)
{
    void *entry = zhashx_lookup (event->evindex, name);
    if (!entry) {
        entry = int2ptr (((int) zhashx_size (event->evindex) + 1));
        if (zhashx_insert (event->evindex, name, entry) < 0) {
            /*
             *  insertion only fails on duplicate entry, which we know
             *   is not possible in this case. However, cover ENOMEM
             *   case here in case assert-on-malloc failure is fixed
             *   in the future for zhashx.
             */
            errno = ENOMEM;
            return -1;
        }
    }
    return ptr2int (entry);
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */

