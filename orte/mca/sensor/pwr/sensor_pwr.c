/*
 * Copyright (c) 2013-2014 Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"
#include "orte/types.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif  /* HAVE_STRING_H */
#include <stdio.h>
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif  /* HAVE_DIRENT_H */
#include <ctype.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <math.h>

#include "opal_stdint.h"
#include "opal/class/opal_list.h"
#include "opal/dss/dss.h"
#include "opal/util/os_path.h"
#include "opal/util/output.h"
#include "opal/mca/db/db.h"

#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/errmgr/errmgr.h"

#include "orte/mca/sensor/base/base.h"
#include "orte/mca/sensor/base/sensor_private.h"
#include "sensor_pwr.h"

/* declare the API functions */
static int init(void);
static void finalize(void);
static void start(orte_jobid_t job);
static void stop(orte_jobid_t job);
static void pwr_sample(void);
static void pwr_log(opal_buffer_t *buf);

/* instantiate the module */
orte_sensor_base_module_t orte_sensor_pwr_module = {
    init,
    finalize,
    start,
    stop,
    pwr_sample,
    pwr_log
};

#define MSR_RAPL_POWER_UNIT		0x606

/*
 * Platform specific RAPL bitmasks.
 */
#define MSR_PKG_POWER_INFO	0x614
#define POWER_UNIT_OFFSET	0
#define POWER_UNIT_MASK		0x0F


typedef struct {
    opal_list_item_t super;
    char *file;
    int core;
    double units;
} corepwr_tracker_t;
static void ctr_con(corepwr_tracker_t *trk)
{
    trk->file = NULL;
}
static void ctr_des(corepwr_tracker_t *trk)
{
    if (NULL != trk->file) {
        free(trk->file);
    }
}
OBJ_CLASS_INSTANCE(corepwr_tracker_t,
                   opal_list_item_t,
                   ctr_con, ctr_des);

static bool log_enabled = true;
static opal_list_t tracking;

static int read_msr(int fd, long long *value, int offset)
{
    uint64_t data;

    if (pread(fd, &data, sizeof data, offset) != sizeof(data)) {
        return ORTE_ERROR;
    }
    *value = (long long)data;
    return ORTE_SUCCESS;
}


static int init(void)
{
    int fd;
    DIR *cur_dirp = NULL;
    struct dirent *entry;
    corepwr_tracker_t *trk;
    long long units;

    OBJ_CONSTRUCT(&tracking, opal_list_t);

    /*
     * Open up the base directory so we can get a listing
     */
    if (NULL == (cur_dirp = opendir("/dev/cpu"))) {
        OBJ_DESTRUCT(&tracking);
        return ORTE_ERROR;
    }

    /*
     * For each directory
     */
    while (NULL != (entry = readdir(cur_dirp))) {
        
        /*
         * Skip the obvious
         */
        if (0 == strncmp(entry->d_name, ".", strlen(".")) ||
            0 == strncmp(entry->d_name, "..", strlen(".."))) {
            continue;
        }

        /* if it contains anything other than a digit, then it isn't a cpu directory */
        if (!isdigit(entry->d_name[strlen(entry->d_name)-1])) {
            continue;
        }

        /* track the info for this core */
        trk = OBJ_NEW(corepwr_tracker_t);
        trk->core = strtoul(entry->d_name, NULL, 10);
        trk->file = opal_os_path(false, "/dev/cpu", entry->d_name, "msr", NULL);
        
        /* get the power units for this core */
        if (0 >= (fd = open(trk->file, O_RDONLY))) {
            /* can't access file */
            OBJ_RELEASE(trk);
            continue;
        }
        if (ORTE_SUCCESS != read_msr(fd, &units, MSR_RAPL_POWER_UNIT)) {
            /* can't read required info */
            OBJ_RELEASE(trk);
            continue;
        }
        trk->units = pow(0.5,(double)(units & POWER_UNIT_MASK));

        /* add to our list */
        opal_list_append(&tracking, &trk->super);
    }
    closedir(cur_dirp);

    if (0 == opal_list_get_size(&tracking)) {
        /* nothing to read */
        return ORTE_ERROR;
    }

    return ORTE_SUCCESS;
}

static void finalize(void)
{
    OPAL_LIST_DESTRUCT(&tracking);
}

/*
 * Start monitoring of local temps
 */
static void start(orte_jobid_t jobid)
{
    return;
}


static void stop(orte_jobid_t jobid)
{
    return;
}

static void pwr_sample(void)
{
    corepwr_tracker_t *trk, *nxt;
    opal_buffer_t data, *bptr;
    int32_t ncores;
    time_t now;
    char time_str[40];
    char *timestamp_str;
    long long value;
    int fd, ret;
    float power;

    opal_output_verbose(2, orte_sensor_base_framework.framework_output,
                        "%s sampling power",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

    /* prep to store the results */
    OBJ_CONSTRUCT(&data, opal_buffer_t);

    /* store our hostname */
    if (OPAL_SUCCESS != (ret = opal_dss.pack(&data, &orte_process_info.nodename, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&data);
        return;
    }

    /* store the number of cores */
    ncores = (int32_t)opal_list_get_size(&tracking);
    if (OPAL_SUCCESS != (ret = opal_dss.pack(&data, &ncores, 1, OPAL_INT32))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&data);
        return;
    }

    /* get the sample time */
    now = time(NULL);
    /* pass the time along as a simple string */
    strftime(time_str, sizeof(time_str), "%F %T%z", localtime(&now));
    asprintf(&timestamp_str, "%s", time_str);
    if (OPAL_SUCCESS != (ret = opal_dss.pack(&data, &timestamp_str, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&data);
        free(timestamp_str);
        return;
    }
    free(timestamp_str);

    OPAL_LIST_FOREACH_SAFE(trk, nxt, &tracking, corepwr_tracker_t) {
        if (0 >= (fd = open(trk->file, O_RDONLY))) {
            /* disable this one - cannot read the file */
            opal_list_remove_item(&tracking, &trk->super);
            OBJ_RELEASE(trk);
            continue;
        }
        if (ORTE_SUCCESS != read_msr(fd, &value, MSR_PKG_POWER_INFO)) {
            /* disable this one - cannot read the file */
            opal_list_remove_item(&tracking, &trk->super);
            OBJ_RELEASE(trk);
            close(fd);
            continue;
        }
        power = trk->units * (double)(value & 0x7fff);
        if (OPAL_SUCCESS != (ret = opal_dss.pack(&data, &power, 1, OPAL_FLOAT))) {
            ORTE_ERROR_LOG(ret);
            OBJ_DESTRUCT(&data);
            close(fd);
            return;
        }
        close(fd);
    }

    /* xfer the data for transmission */
    bptr = &data;
    if (OPAL_SUCCESS != (ret = opal_dss.pack(orte_sensor_base.samples, &bptr, 1, OPAL_BUFFER))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&data);
        return;
    }
    OBJ_DESTRUCT(&data);
}

static void pwr_log(opal_buffer_t *sample)
{
    char *hostname=NULL;
    char *sampletime;
    int rc;
    int32_t n, ncores;
    opal_value_t *kv=NULL;
    float fval;
    int i;

    if (!log_enabled) {
        return;
    }

    /* unpack the host this came from */
    n=1;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &hostname, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    /* and the number of cores on that host */
    n=1;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &ncores, &n, OPAL_INT32))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* sample time */
    n=1;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &sampletime, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    opal_output_verbose(3, orte_sensor_base_framework.framework_output,
                        "%s Received log from host %s with %d cores",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        (NULL == hostname) ? "NULL" : hostname, ncores);

    /* xfr to storage */
    kv = malloc((ncores+1) * sizeof(opal_value_t));

    /* load the sample time at the start */
    OBJ_CONSTRUCT(&kv[0], opal_value_t);
    kv[0].key = strdup("ctime");
    kv[0].type = OPAL_STRING;
    kv[0].data.string = strdup(sampletime);
    free(sampletime);

    for (i=0; i < ncores; i++) {
        OBJ_CONSTRUCT(&kv[i+1], opal_value_t);
        asprintf(&kv[i+1].key, "core%d", i);
        kv[i+1].type = OPAL_FLOAT;
        n=1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(sample, &fval, &n, OPAL_FLOAT))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        kv[i+1].data.fval = fval;
    }

    /* store it */
    if (ORTE_SUCCESS != (rc = opal_db.add_log("pwr", kv, ncores+1))) {
        /* don't bark about it - just quietly disable the log */
        log_enabled = false;
    }

 cleanup:
    /* cleanup the xfr storage */
    for (i=0; i < ncores+1; i++) {
        OBJ_DESTRUCT(&kv[i]);
    }
    if (NULL != hostname) {
        free(hostname);
    }

}