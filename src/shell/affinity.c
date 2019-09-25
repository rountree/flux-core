/************************************************************\
 * Copyright 2019 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* builtin cpu-affinity processing
 */
#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <hwloc.h>
#include <jansson.h>

#include <flux/core.h>
#include <flux/shell.h>

#include "src/common/libutil/log.h"

#include "builtins.h"

struct shell_affinity {
    hwloc_topology_t topo;
    int ntasks;
    json_t *rankinfo;
    const char *cores;
    hwloc_cpuset_t cpuset;
    hwloc_cpuset_t *pertask;
};

/*  Run hwloc_topology_restrict() with common flags for this module.
 */
static int topology_restrict (hwloc_topology_t topo, hwloc_cpuset_t set)
{
    int flags = HWLOC_RESTRICT_FLAG_ADAPT_DISTANCES |
                HWLOC_RESTRICT_FLAG_ADAPT_MISC |
                HWLOC_RESTRICT_FLAG_ADAPT_IO;
    flags = 0;
    if (hwloc_topology_restrict (topo, set, flags) < 0)
        return (-1);
    return (0);
}

/*  Restrict hwloc topology object to current processes binding.
 */
static int topology_restrict_current (hwloc_topology_t topo)
{
    int rc = -1;
    hwloc_bitmap_t rset = hwloc_bitmap_alloc ();
    if (!rset || hwloc_get_cpubind (topo, rset, HWLOC_CPUBIND_PROCESS) < 0)
        goto out;
    rc = topology_restrict (topo, rset);
out:
    if (rset)
        hwloc_bitmap_free (rset);
    return (rc);
}

/*  Distribute ntasks over the topology 'topo', restricted to the
 *   cpuset give in 'cset' if non-NULL.
 *
 *  Returns a hwloc_cpuset_t array of size ntasks.
 */
static hwloc_cpuset_t *distribute_tasks (hwloc_topology_t topo,
                                         hwloc_cpuset_t cset,
                                         int ntasks)
{
    hwloc_obj_t obj[1];
    hwloc_cpuset_t *cpusetp = NULL;

    /* restrict topology to current cpuset */
    if (cset && topology_restrict (topo, cset) < 0)
        return NULL;
    /* create cpuset array for ntasks */
    if (!(cpusetp = calloc (ntasks, sizeof (hwloc_cpuset_t))))
        return NULL;
    /* Distribute starting at root over remaining objects */
    obj[0] = hwloc_get_root_obj (topo);

    /* NB: hwloc_distrib() will alloc ntasks cpusets in cpusetp, which
     *     later need to be destroyed with hwloc_bitmap_free().
     */
    hwloc_distrib (topo, obj, 1, cpusetp, ntasks, HWLOC_OBJ_PU, 0);
    return (cpusetp);
}

/*  Return the cpuset that is the union of cpusets contained in "cores" list.
 */
static hwloc_cpuset_t shell_affinity_get_cpuset (struct shell_affinity *sa,
                                                 const char *cores)
{
    int depth, i;
    hwloc_cpuset_t coreset = NULL;
    hwloc_cpuset_t resultset = NULL;

    if (!(coreset = hwloc_bitmap_alloc ())
        || !(resultset = hwloc_bitmap_alloc ())) {
        log_err ("hwloc_bitmap_alloc");
        goto err;
    }

    /*  Parse cpus as bitmap list
     */
    if (hwloc_bitmap_list_sscanf (coreset, cores) < 0) {
        log_msg ("affinity: failed to read core list: %s", cores);
        goto err;
    }

    /*  Find depth of type core in this topology:
     */
    depth = hwloc_get_type_depth (sa->topo, HWLOC_OBJ_CORE);
    if (depth == HWLOC_TYPE_DEPTH_UNKNOWN
        || depth == HWLOC_TYPE_DEPTH_MULTIPLE) {
        log_msg ("hwloc_get_type_depth (CORE) returned nonsense");
        goto err;
    }

    /*  Get the union of all allocated cores' cpusets into sa->cpuset
     */
    i = hwloc_bitmap_first (coreset);
    while (i >= 0) {
        hwloc_obj_t core = hwloc_get_obj_by_depth (sa->topo, depth, i);
        if (!core) {
            log_msg ("affinity: core%d not in topology", i);
            goto err;
        }
        hwloc_bitmap_or (resultset, resultset, core->cpuset);
        i = hwloc_bitmap_next (coreset, i);
    }
    hwloc_bitmap_free (coreset);
    return resultset;
err:
    if (coreset)
        hwloc_bitmap_free (coreset);
    if (resultset)
        hwloc_bitmap_free (resultset);
    return NULL;
}

/*  Get shell rankinfo json_t object from the shell API.
 */
static json_t *shell_rankinfo (flux_shell_t *shell)
{
    char *json_str = NULL;
    json_t *o = NULL;
    if (flux_shell_get_rank_info (shell, -1, &json_str) < 0)
        log_err ("flux_shell_get_rank_info");
    else if (!(o = json_loads (json_str, 0, NULL)))
        log_err ("json_loads");
    free (json_str);
    return o;
}

static void shell_affinity_destroy (void *arg)
{
    struct shell_affinity *sa = arg;
    if (sa->topo)
        hwloc_topology_destroy (sa->topo);
    if (sa->cpuset)
        hwloc_bitmap_free (sa->cpuset);
    if (sa->pertask) {
        for (int i = 0; i < sa->ntasks; i++) {
            if (sa->pertask[i] != NULL)
                hwloc_bitmap_free (sa->pertask[i]);
        }
        free (sa->pertask);
    }
    json_decref (sa->rankinfo);
    free (sa);
}

/*  Initialize topology object for affinity processing.
 */
static int shell_affinity_topology_init (struct shell_affinity *sa)
{
    if (hwloc_topology_init (&sa->topo) < 0) {
        log_err ("hwloc_topology_init");
        return -1;
    }
    if (hwloc_topology_load (sa->topo) < 0) {
        log_err ("hwloc_topology_load");
        return -1;
    }
    if (topology_restrict_current (sa->topo) < 0) {
        log_err ("topology_restrict_current");
        return -1;
    }
    return 0;
}

/*  Create shell affinity context, including reading in hwloc
 *   topology, gathering number of local tasks and assigned core list,
 *   and getting the resulting cpuset for the entire shell.
 */
static struct shell_affinity * shell_affinity_create (flux_shell_t *shell)
{
    struct shell_affinity *sa = calloc (1, sizeof (*sa));
    if (!sa)
        return NULL;
    if (shell_affinity_topology_init (sa) < 0)
        goto err;
    if (!(sa->rankinfo = shell_rankinfo (shell)))
        goto err;
    if (json_unpack (sa->rankinfo, "{ s:i s:{s:s} }",
                                   "ntasks", &sa->ntasks,
                                   "resources",
                                     "cores", &sa->cores) < 0) {
        log_err ("json_unpack");
        goto err;
    }
    return sa;
err:
    shell_affinity_destroy (sa);
    return NULL;
}

/*  Parse any shell 'cpu-affinity' and return true if shell affinity
 *   is enabled. Return any string option setting in resultp.
 *  By default, affinity is enabled unless cpu-affinity="off".
 */
static bool affinity_getopt (flux_shell_t *shell, const char **resultp)
{
    int rc;
    /* Default if not set is "on" */
    *resultp = "on";
    rc = flux_shell_getopt_unpack (shell, "cpu-affinity", "s", resultp);
    if (rc == 0) {
        return true;
    }
    else if (rc < 0) {
        log_msg ("cpu-affinity: invalid option");
        return true;
    }
    else if (strcmp (*resultp, "off") == 0)
        return false;
    return true;
}


/*  Return task id for a shell task
 */
static int flux_shell_task_getid (flux_shell_task_t *task)
{
    int id = -1;
    char *s = NULL;
    json_t *o = NULL;

    if (flux_shell_task_get_info (task, &s) < 0)
        return -1;
    if (!(o = json_loads (s, 0, NULL)))
        goto out;
    if (json_unpack (o, "{ s:i }", "localid", &id) < 0)
        goto out;
out:
    json_decref (o);
    free (s);
    return id;
}

/*  Return the current task id when running in task.* context.
 */
static int get_taskid (flux_plugin_t *p)
{
    flux_shell_t *shell;
    flux_shell_task_t *task;

    if (!(shell = flux_plugin_get_shell (p)))
        return -1;
    if (!(task = flux_shell_current_task (shell)))
        return -1;
    return flux_shell_task_getid (task);
}

#if CODE_COVERAGE_ENABLED
void __gcov_flush (void);
#endif

static int task_affinity (flux_plugin_t *p,
                          const char *topic,
                          flux_plugin_arg_t *args,
                          void *data)
{
    struct shell_affinity *sa = data;
    int i = get_taskid (p);
    if (sa->pertask)
        hwloc_set_cpubind (sa->topo, sa->pertask[i], 0);
    shell_affinity_destroy (sa);
#if CODE_COVERAGE_ENABLED
    __gcov_flush ();
#endif
    return 0;
}

static int affinity_init (flux_plugin_t *p,
                          const char *topic,
                          flux_plugin_arg_t *args,
                          void *data)
{
    const char *option;
    struct shell_affinity *sa = NULL;
    flux_shell_t *shell = flux_plugin_get_shell (p);

    if (!shell) {
        log_err ("flux_plugin_get_shell");
        return -1;
    }
    if (!affinity_getopt (shell, &option))
        return 0;
    if (!(sa = shell_affinity_create (shell))) {
        log_err ("shell_affinity_create");
        return -1;
    }
    /*  Attempt to get cpuset union of all allocated cores. If this
     *   fails, then it might be because the allocated cores exceeds
     *   the real cores available on this machine, so just log an
     *   informational message and skip setting affinity.
     */
    if (!(sa->cpuset = shell_affinity_get_cpuset (sa, sa->cores))) {
        log_msg ("unable to get cpuset for cores %s. Disabling affinity",
                 sa->cores);
        return 0;
    }
    if (flux_plugin_aux_set (p, "affinity", sa, shell_affinity_destroy) < 0) {
        shell_affinity_destroy (sa);
        return -1;
    }
    if (hwloc_set_cpubind (sa->topo, sa->cpuset, 0) < 0) {
        log_err ("shell_affinity_bind");
        return -1;
    }

    /*  If cpu-affinity=per-task, then distribute ntasks over whatever
     *   resources to which the shell is now bound (from above)
     *  Set a 'task.exec' callback to actually make the per-task binding.
     */
    if (strcmp (option, "per-task") == 0) {
        if (!(sa->pertask = distribute_tasks (sa->topo,
                                              sa->cpuset,
                                              sa->ntasks)))
            log_err ("distribute_tasks failed");
        if (flux_plugin_add_handler (p, "task.exec",
                                     task_affinity,
                                     sa) < 0)
            log_err ("failed to add task.exec handler");
    }

    return 0;
}

struct shell_builtin builtin_affinity = {
    .name = "affinity",
    .init = affinity_init,
};

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
