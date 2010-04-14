/*
 * ADIOS is freely available under the terms of the BSD license described
 * in the COPYING file in the top level directory of this source distribution.
 *
 * Copyright (c) 2008 - 2009.  UT-BATTELLE, LLC. All rights reserved.
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "config.h"
#include "mpi.h"
#include "adios.h"
#include "adios_types.h"
#include "adios_bp_v1.h"
#include "adios_transport_hooks.h"
#include "adios_internals.h"
#ifdef HAVE_NSSI
#include "nssi_client.h"
#include "adios_nssi_args.h"
#include "adios_nssi_config.h"
#include "nssi_logger.h"
#endif

#include "io_timer.h"


#define NUM_GP 24
void adios_nssi_get_write_buffer(
        struct adios_file_struct *f,
        struct adios_var_struct *v,
        uint64_t *size,
        void **buffer,
        struct adios_method_struct *method)
{
}

#ifndef HAVE_NSSI
void adios_nssi_init(
        const char *parameters,
        struct adios_method_struct * method)
{
}
void adios_nssi_finalize(
        int mype,
        struct adios_method_struct * method)
{
}
enum ADIOS_FLAG adios_nssi_should_buffer(
        struct adios_file_struct *f,
        struct adios_method_struct *method)
{
    return adios_flag_unknown;
}
int adios_nssi_open(
        struct adios_file_struct *f,
        struct adios_method_struct *method,
        void * comm)
{
    return -1;
}
void adios_nssi_close(
        struct adios_file_struct *f,
        struct adios_method_struct *method)
{
}
void adios_nssi_write(
        struct adios_file_struct *f,
        struct adios_var_struct *v,
        void *data,
        struct adios_method_struct *method)
{
}
void adios_nssi_read(
        struct adios_file_struct *f,
        struct adios_var_struct *v,
        void *buffer,
        uint64_t buffersize,
        struct adios_method_struct *method)
{
}
#else

///////////////////////////
// Datatypes
///////////////////////////
struct adios_nssi_data_struct
{
    int      fd;
    MPI_Comm group_comm;
    int      rank;
    int      size;

    void * comm; // temporary until moved from should_buffer to open
};

/* Need a struct to encapsulate var offset info.
 */
struct var_offset {
    char      opath[ADIOS_PATH_MAX];
    char      oname[ADIOS_PATH_MAX];
    void     *ovalue;
    uint64_t  osize;
};

/* list of variable offsets */
List var_offset_list;

/* Need a struct to encapsulate var dim info.
 */
struct var_dim {
    char      dpath[ADIOS_PATH_MAX];
    char      dname[ADIOS_PATH_MAX];
    void     *dvalue;
    uint64_t  dsize;
};

/* list of variable offsets */
List var_dim_list;

/* Need a struct to encapsulate open file info
 */
struct open_file {
    char                           fpath[ADIOS_PATH_MAX];
    char                           fname[ADIOS_PATH_MAX];
    struct adios_nssi_data_struct *md;
    struct adios_file_struct      *f;
    nssi_request                   start_calc_req;
//    List                           outstanding_reqs;
};

/* list of variable offsets */
List open_file_list;

///////////////////////////
// Global Variables
///////////////////////////
static int adios_nssi_initialized = 0;

static int default_svc;
static MPI_Comm ncmpi_collective_op_comm;
static int global_rank=-1;
static int collective_op_size=-1;
static int collective_op_rank=-1;
static nssi_service *svcs;
struct adios_nssi_config nssi_cfg;

//static log_level adios_nssi_debug_level;
static int DEBUG=3;


///////////////////////////
// Function Declarations
///////////////////////////


///////////////////////////
// Function Definitions
///////////////////////////

static struct open_file *open_file_create(const char *path, const char *name, struct adios_nssi_data_struct *method_private_data)
{
    struct open_file *of=calloc(1,sizeof(struct open_file));

    strcpy(of->fpath, path);
    strcpy(of->fname, name);
    of->md = method_private_data;
//    list_init(&(of->outstanding_reqs), free);

    return(of);
}
static void open_file_free(void *of)
{
//    list_destroy((List *)&(of->outstanding_reqs));
    free(of);
}
static int open_file_equal(const struct open_file *of1, const struct open_file *of2)
{
    if ((strcmp(of1->fpath, of2->fpath) == 0) && (strcmp(of1->fname, of2->fname) == 0)) return TRUE;

    return FALSE;
}
static struct open_file *open_file_find(const char *path, const char *name)
{
    ListElmt *elmt;
    struct open_file *of;

    if (DEBUG>3) printf("looking for fpath(%s) fname(%s)\n", path, name);

    elmt = list_head(&open_file_list);
    while(elmt) {
        of = list_data(elmt);
        if (DEBUG>3) printf("comparing to fpath(%s) fname(%s)\n", of->fpath, of->fname);
        if ((strcmp(path, of->fpath) == 0) && (strcmp(name, of->fname) == 0)) {
            if (DEBUG>3) printf("fpath(%s) fname(%s) matches search\n", of->fpath, of->fname);
            return of;
        }
        elmt = list_next(elmt);
    }

    return NULL;
}
static void open_file_printall(void)
{
    ListElmt *elmt;
    struct open_file *of;

    elmt = list_head(&open_file_list);
    while(elmt) {
        of = list_data(elmt);
        if (DEBUG>3) printf("fpath(%s) fname(%s)\n", of->fpath, of->fname);
        elmt = list_next(elmt);
    }
}

static struct var_offset *var_offset_create(const char *path, const char *name, void *value, uint64_t size)
{
    struct var_offset *vo=calloc(1,sizeof(struct var_offset));

    strcpy(vo->opath, path);
    strcpy(vo->oname, name);
    vo->ovalue = value;
    vo->osize  = size;

    return(vo);
}
static void var_offset_free(void *vo)
{
    free(vo);
}
static int var_offset_equal(const struct var_offset *vo1, const struct var_offset *vo2)
{
    if ((strcmp(vo1->opath, vo2->opath) == 0) && (strcmp(vo1->oname, vo2->oname) == 0)) return TRUE;

    return FALSE;
}
static struct var_offset *var_offset_find(const char *path, const char *name)
{
    ListElmt *elmt;
    struct var_offset *vo;

    if (DEBUG>3) printf("looking for opath(%s) oname(%s)\n", path, name);

    elmt = list_head(&var_offset_list);
    while(elmt) {
        vo = list_data(elmt);
        if (DEBUG>3) printf("comparing to opath(%s) oname(%s)\n", vo->opath, vo->oname);
        if ((strcmp(path, vo->opath) == 0) && (strcmp(name, vo->oname) == 0)) {
            if (DEBUG>3) printf("opath(%s) oname(%s) matches search\n", vo->opath, vo->oname);
            return vo;
        }
        elmt = list_next(elmt);
    }

    return NULL;
}
static void var_offset_printall(void)
{
    ListElmt *elmt;
    struct var_offset *vo;

    elmt = list_head(&var_offset_list);
    while(elmt) {
        vo = list_data(elmt);
        if (DEBUG>3) printf("opath(%s) oname(%s)\n", vo->opath, vo->oname);
        elmt = list_next(elmt);
    }
}

static struct var_dim *var_dim_create(const char *path, const char *name, void *value, uint64_t size)
{
    struct var_dim *vd=calloc(1,sizeof(struct var_dim));

    strcpy(vd->dpath, path);
    strcpy(vd->dname, name);
    vd->dvalue = value;
    vd->dsize  = size;

    return(vd);
}
static void var_dim_free(void *vd)
{
    free(vd);
}
static int var_dim_equal(const struct var_dim *vd1, const struct var_dim *vd2)
{
    if ((strcmp(vd1->dpath, vd2->dpath) == 0) && (strcmp(vd1->dname, vd2->dname) == 0)) return TRUE;

    return FALSE;
}
static struct var_dim *var_dim_find(const char *path, const char *name)
{
    ListElmt *elmt;
    struct var_dim *vd;

    if (DEBUG>3) printf("looking for opath(%s) oname(%s)\n", path, name);

    elmt = list_head(&var_dim_list);
    while(elmt) {
        vd = list_data(elmt);
        if (DEBUG>3) printf("comparing to opath(%s) oname(%s)\n", vd->dpath, vd->dname);
        if ((strcmp(path, vd->dpath) == 0) && (strcmp(name, vd->dname) == 0)) {
            if (DEBUG>3) printf("opath(%s) oname(%s) matches search\n", vd->dpath, vd->dname);
            return vd;
        }
        elmt = list_next(elmt);
    }

    return NULL;
}
static void var_dim_printall(void)
{
    ListElmt *elmt;
    struct var_dim *vd;

    elmt = list_head(&var_dim_list);
    while(elmt) {
        vd = list_data(elmt);
        if (DEBUG>3) printf("dpath(%s) dname(%s)\n", vd->dpath, vd->dname);
        elmt = list_next(elmt);
    }
}

static void parse_dimension_size(
        struct adios_group_struct *group,
        struct adios_var_struct *pvar_root,
        struct adios_attribute_struct *patt_root,
        struct adios_dimension_item_struct *dim,
        size_t *dimsize)
{
    struct adios_var_struct *var_linked = NULL;
    struct adios_attribute_struct *attr_linked;
    if (dim->id) {
        var_linked = adios_find_var_by_id (pvar_root , dim->id);
        if (!var_linked) {
            attr_linked = adios_find_attribute_by_id (patt_root, dim->id);
            if (!attr_linked->var) {
                switch (attr_linked->type) {
                case adios_unsigned_byte:
                    *dimsize = *(uint8_t *)attr_linked->value;
                    break;
                case adios_byte:
                    *dimsize = *(int8_t *)attr_linked->value;
                    break;
                case adios_unsigned_short:
                    *dimsize = *(uint16_t *)attr_linked->value;
                    break;
                case adios_short:
                    *dimsize = *(int16_t *)attr_linked->value;
                    break;
                case adios_unsigned_integer:
                    *dimsize = *(uint32_t *)attr_linked->value;
                    break;
                case adios_integer:
                    *dimsize = *(int32_t *)attr_linked->value;
                    break;
                case adios_unsigned_long:
                    *dimsize = *(uint64_t *)attr_linked->value;
                    break;
                case adios_long:
                    *dimsize = *(int64_t *)attr_linked->value;
                    break;
                default:
                    fprintf (stderr, "Invalid datatype for array dimension on "
                            "var %s: %s\n"
                            ,attr_linked->name
                            ,adios_type_to_string_int (var_linked->type)
                    );
                    break;
                }
            } else {
                var_linked = attr_linked->var;
            }
        }
        if (var_linked && var_linked->data) {
            *dimsize = *(int *)var_linked->data;
        }
    } else {
        if (dim->time_index == adios_flag_yes) {
            *dimsize = 1;
        } else {
            *dimsize = dim->rank;
        }
    }

    return;
}
static void parse_dimension_name(
        struct adios_group_struct *group,
        struct adios_var_struct *pvar_root,
        struct adios_attribute_struct *patt_root,
        struct adios_dimension_item_struct *dim,
        char *dimname)
{
    struct adios_var_struct *var_linked = NULL;
    struct adios_attribute_struct *attr_linked;
    if (dim->id) {
        var_linked = adios_find_var_by_id (pvar_root , dim->id);
        if (!var_linked) {
            attr_linked = adios_find_attribute_by_id (patt_root, dim->id);
            if (!attr_linked->var) {
//				strcpy(dimname, attr_linked->name);
                sprintf(dimname, "%s", attr_linked->name);
            } else {
                var_linked = attr_linked->var;
            }
        }
        if (var_linked && var_linked->name) {
//			strcpy(dimname, var_linked->name);
            sprintf(dimname, "%s", var_linked->name);
        }
    } else {
        if (dim->time_index == adios_flag_yes) {
//			strcpy(dimname, group->time_index_name);
            sprintf(dimname, "%s", group->time_index_name);
        } else {
            dimname[0] = '\0';
        }
    }

    return;
}


static int gen_offset_list(
        struct adios_group_struct *group,
        struct adios_var_struct *pvar_root,
        struct adios_attribute_struct *patt_root)
{
    struct adios_var_struct *v;
    struct adios_dimension_struct *dims;
    struct var_info *vi;
    char offset_name[255];
    uint64_t *value;

    v = pvar_root;
    while (v) {
        dims=v->dimensions;
        int loffs_idx=0;
        while (dims) {
            parse_dimension_name(group, pvar_root, patt_root, &dims->local_offset, offset_name);
            if (offset_name[0] == '\0') {
                sprintf(offset_name, "offset_%d", loffs_idx);
            }
            if (DEBUG>3) printf("gen: offset_name(%s)\n", offset_name);
            value=calloc(1,sizeof(uint64_t));
            parse_dimension_size(group, pvar_root, patt_root, &dims->local_offset, value);

            uint64_t vsize = 4; /* adios_get_var_size(v, group, value); */
            vi = var_offset_create(v->path, offset_name, value, vsize);
            list_ins_next(&var_offset_list, list_head(&var_offset_list), vi);

            loffs_idx++;
            dims = dims->next;
        }
        v = v->next;
    }
}

static void create_offset_list_for_var(
        struct adios_write_args *args,
        struct adios_var_struct *v,
        struct adios_group_struct *group,
        struct adios_var_struct *pvar_root,
        struct adios_attribute_struct *patt_root)
{
    struct adios_dimension_struct *dims;
    char offset_name[255];

    args->offsets.offsets_len=0;
    args->offsets.offsets_val=NULL;

    if ((v) && (v->dimensions)) {
        int local_offset_count=0;
        dims=v->dimensions;
        while (dims) {
            if (dims->dimension.time_index == adios_flag_yes) {
                dims = dims->next;
                continue;
            }
            parse_dimension_name(group, pvar_root, patt_root, &dims->local_offset, offset_name);
            local_offset_count++;
            dims = dims->next;
        }

        args->offsets.offsets_len=local_offset_count;
        args->offsets.offsets_val=calloc(local_offset_count, sizeof(struct adios_var));

        dims=v->dimensions;
        int loffs_idx=0;
        while (dims) {
            if (dims->dimension.time_index == adios_flag_yes) {
                dims = dims->next;
                continue;
            }
            parse_dimension_name(group, pvar_root, patt_root, &dims->local_offset, offset_name);
            if (offset_name[0] == '\0') {
                sprintf(offset_name, "offset_%d", loffs_idx);
            }
            args->offsets.offsets_val[loffs_idx].vpath=strdup(v->path);
            args->offsets.offsets_val[loffs_idx].vname=strdup(offset_name);
//            struct var_offset *vo=var_offset_find("", offset_name);
//            memcpy(&(args->offsets.offsets_val[loffs_idx].vdata), vo->ovalue, vo->osize);
//            args->offsets.offsets_val[loffs_idx].vdatasize=vo->osize;
//            printf("create: offset_name(%s) offset_value(%lu)\n", offset_name, vo->ovalue);
            uint64_t value=0;
            parse_dimension_size(group, pvar_root, patt_root, &dims->local_offset, &value);
            memcpy(&(args->offsets.offsets_val[loffs_idx].vdata), &value, 4);
            args->offsets.offsets_val[loffs_idx].vdatasize=4;
            if (DEBUG>3) printf("create: offset_name(%s) offset_value(%lu)\n", offset_name, value);

            loffs_idx++;
            dims = dims->next;
        }
    }
}

static int gen_dim_list(
        struct adios_group_struct *group,
        struct adios_var_struct *pvar_root,
        struct adios_attribute_struct *patt_root)
{
    struct adios_var_struct *v;
    struct adios_dimension_struct *dims;
    struct var_info *vi;
    char dim_name[255];
    uint64_t *value;

    v = pvar_root;
    while (v) {
        dims=v->dimensions;
        int dim_idx=0;
        while (dims) {
            parse_dimension_name(group, pvar_root, patt_root, &dims->dimension, dim_name);
            if (dim_name[0] == '\0') {
                sprintf(dim_name, "dim_%d", dim_idx);
            }
            if (DEBUG>3) printf("gen: dim_name(%s)\n", dim_name);
            value=calloc(1,sizeof(uint64_t));
            parse_dimension_size(group, pvar_root, patt_root, &dims->dimension, value);
            if (global_rank==0) {
                if (DEBUG>3) printf(":o(%d)", *value);
            }

            uint64_t vsize = 4; /* adios_get_var_size(v, group, value); */
            vi = var_dim_create(v->path, dim_name, value, vsize);
            list_ins_next(&var_dim_list, list_head(&var_dim_list), vi);

            dim_idx++;
            dims = dims->next;
        }
        v = v->next;
    }
}

static void create_dim_list_for_var(
        struct adios_write_args *args,
        struct adios_var_struct *v,
        struct adios_group_struct *group,
        struct adios_var_struct *pvar_root,
        struct adios_attribute_struct *patt_root)
{
    struct adios_dimension_struct *dims;
    char dim_name[255];

    args->dims.dims_len=0;
    args->dims.dims_val=NULL;

    if ((v) && (v->dimensions)) {
        int dim_count=0;
        dims=v->dimensions;
        while (dims) {
            if (dims->dimension.time_index == adios_flag_yes) {
                dims = dims->next;
                continue;
            }
            parse_dimension_name(group, pvar_root, patt_root, &dims->dimension, dim_name);
            dim_count++;
            dims = dims->next;
        }

        args->dims.dims_len=dim_count;
        args->dims.dims_val=calloc(dim_count, sizeof(struct adios_var));

        dims=v->dimensions;
        int dim_idx=0;
        while (dims) {
            if (dims->dimension.time_index == adios_flag_yes) {
                dims = dims->next;
                continue;
            }
            parse_dimension_name(group, pvar_root, patt_root, &dims->dimension, dim_name);
            if (dim_name[0] == '\0') {
                sprintf(dim_name, "dim_%d", dim_idx);
            }
            args->dims.dims_val[dim_idx].vpath=strdup(v->path);
            args->dims.dims_val[dim_idx].vname=strdup(dim_name);
//            struct var_dim *vd=var_dim_find("", dim_name);
//            memcpy(&(args->dims.dims_val[dim_idx].vdata), vd->dvalue, vd->dsize);
//            args->dims.dims_val[dim_idx].vdatasize=vd->dsize;
            uint64_t value=0;
            parse_dimension_size(group, pvar_root, patt_root, &dims->dimension, &value);
            memcpy(&(args->dims.dims_val[dim_idx].vdata), &value, 4);
            args->dims.dims_val[dim_idx].vdatasize=4;
            if (DEBUG>3) printf("create: dim_name(%s) dvalue(%lu)\n", dim_name, value);
            if (global_rank==0) {
                if (DEBUG>3) printf(":o(%d)", value);
            }

            dim_idx++;
            dims = dims->next;
        }
    }
}

static int read_var(
        int fd,
        struct adios_var_struct *pvar,
        int myrank,
        int nproc,
        MPI_Comm group_comm)
{
    int return_code=0;
    int i, rc;

    adios_read_args args;
    adios_read_res  res;

    args.fd       = fd;
    args.max_read = pvar->data_size;
    args.vpath = strdup(pvar->path);
    args.vname = strdup(pvar->name);

    Func_Timer("ADIOS_READ_OP",
            rc = nssi_call_rpc_sync(&svcs[default_svc],
            ADIOS_READ_OP,
            &args,
            pvar->data,
            pvar->data_size,
            &res););
    if (rc != NSSI_OK) {
        //log_error(adios_nssi_debug_level, "unable to call remote adios_read");
        return_code=-2;
    }

    free(args.vpath);
    free(args.vname);

    return return_code;
}
static int write_var(
        int fd,
        struct adios_group_struct *group,
        struct adios_var_struct *pvar_root,
        struct adios_attribute_struct *patt_root,
        struct adios_var_struct *pvar,
        uint64_t var_size,
        enum ADIOS_FLAG fortran_flag,
        int myrank,
        int nproc,
        MPI_Comm group_comm)
{
    int i;
    int rc;
    int return_code=0;

    adios_write_args args;
    adios_write_res  res;

//    var_offset_printall();

    memset(&args, 0, sizeof(adios_write_args));
    args.fd    = fd;
    args.vpath = strdup(pvar->path);
    args.vname = strdup(pvar->name);
    args.vsize = var_size;
    args.atype = pvar->type;
    if (pvar->dimensions) {
        args.is_scalar = FALSE;
    } else {
        args.is_scalar = TRUE;
    }
    args.writer_rank=myrank;
    args.offsets.offsets_len=0;
    args.offsets.offsets_val=NULL;
    args.dims.dims_len=0;
    args.dims.dims_val=NULL;
    if (pvar->dimensions) {
        create_offset_list_for_var(
                 &args,
                 pvar,
                 group,
                 group->vars,
                 group->attributes);
        create_dim_list_for_var(
                 &args,
                 pvar,
                 group,
                 group->vars,
                 group->attributes);
     }

    MPI_Barrier(group_comm);
    Func_Timer("ADIOS_WRITE_OP",
            rc = nssi_call_rpc_sync(&svcs[default_svc],
            ADIOS_WRITE_OP,
            &args,
            pvar->data,
            var_size,
            &res););
    if (rc != NSSI_OK) {
        //log_error(adios_nssi_debug_level, "unable to call remote adios_write");
        return_code=-2;
    }
    MPI_Barrier(group_comm);

    free(args.vpath);
    free(args.vname);
    free(args.offsets.offsets_val);

    return return_code;
}


static void adios_var_to_comm_nssi(
        enum ADIOS_FLAG host_language_fortran,
        void *data,
        MPI_Comm *comm)
{
    if (data) {
        int t = *(int *) data;
        if (host_language_fortran == adios_flag_yes) {
            *comm = MPI_Comm_f2c (t);
        } else {
            *comm = *(MPI_Comm *) data;
        }
    } else {
        fprintf (stderr, "coordination-communication not provided. "
                "Using md->group_comm instead\n");
        *comm = MPI_COMM_WORLD;
    }

    return;
}

void adios_nssi_init(
        const char *parameters,
        struct adios_method_struct *method)
{
    int rc=NSSI_OK;
    int verbose=5;
    char logfile[1024];
    int log_rank;

    if (!adios_nssi_initialized) {
        adios_nssi_initialized = 1;
    }

    MPI_Comm_rank(MPI_COMM_WORLD, &global_rank);

    if (DEBUG>3) printf("rank(%d) enter adios_nssi_init\n", global_rank);

//    MPI_Comm_rank(md->group_comm, &log_rank);
//    sprintf(logfile, "%s.%04d", "adios_nssi_client.log", log_rank);
//    logger_init((log_level)verbose, logfile);

//    logger_init((log_level)verbose, NULL);

#ifdef HAVE_PORTALS
    nssi_ptl_init(PTL_IFACE_CLIENT, getpid() + 1000);
    nssi_rpc_init(NSSI_RPC_PTL, NSSI_RPC_XDR);
#endif
#ifdef HAVE_INFINIBAND
    nssi_ib_init(NULL);
    rc = nssi_rpc_init(NSSI_RPC_IB, NSSI_RPC_XDR);
#endif

    /* Register the client operations */
    NSSI_REGISTER_CLIENT_STUB(ADIOS_OPEN_OP, adios_open_args, void, adios_open_res);
    NSSI_REGISTER_CLIENT_STUB(ADIOS_GROUP_SIZE_OP, adios_group_size_args, void, void);
    NSSI_REGISTER_CLIENT_STUB(ADIOS_READ_OP, adios_read_args, void, adios_read_res);
    NSSI_REGISTER_CLIENT_STUB(ADIOS_WRITE_OP, adios_write_args, void, adios_write_res);
    NSSI_REGISTER_CLIENT_STUB(ADIOS_END_ITER_OP, adios_end_iter_args, void, void);
    NSSI_REGISTER_CLIENT_STUB(ADIOS_START_CALC_OP, adios_start_calc_args, void, void);
    NSSI_REGISTER_CLIENT_STUB(ADIOS_STOP_CALC_OP, adios_stop_calc_args, void, void);
    NSSI_REGISTER_CLIENT_STUB(ADIOS_CLOSE_OP, adios_close_args, void, void);

    list_init(&open_file_list, open_file_free);
    list_init(&var_offset_list, var_offset_free);
    list_init(&var_dim_list, var_dim_free);

    parse_nssi_config(getenv("ADIOS_NSSI_CONFIG_FILE"), &nssi_cfg);

    return;
}


enum ADIOS_FLAG adios_nssi_should_buffer(
        struct adios_file_struct *f,
        struct adios_method_struct *method)
{
    int rc=NSSI_OK;

    adios_group_size_args args;

    struct open_file *of=NULL;
    struct adios_nssi_data_struct *md=NULL;

    of=open_file_find(method->base_path, f->name);
    if (of == NULL) {
        fprintf(stderr, "file is not open.  FAIL.");
        return adios_flag_no;
    }
    md=of->md;

    if (DEBUG>3) printf("rank(%d) enter adios_nssi_should_buffer\n", global_rank);

    args.fd = md->fd;
    args.data_size = f->write_size_bytes*collective_op_size;

    if (collective_op_rank == 0) {
        Func_Timer("ADIOS_GROUP_SIZE_OP",
                rc = nssi_call_rpc_sync(&svcs[default_svc],
                        ADIOS_GROUP_SIZE_OP,
                        &args,
                        NULL,
                        0,
                        NULL););
    }

    return adios_flag_no;
}

int adios_nssi_open(
        struct adios_file_struct *f,
        struct adios_method_struct *method,
        void *comm)
{
    int rc=NSSI_OK;

    adios_open_args args;
    adios_open_res  res;

    struct open_file *of=NULL;
    struct adios_nssi_data_struct *md=NULL;

    if (DEBUG>3) printf("rank(%d) enter adios_nssi_open\n", global_rank);

    of=open_file_find(method->base_path, f->name);
    if (of == NULL) {
        md             = malloc(sizeof(struct adios_nssi_data_struct));
        md->fd         = -1;
        md->rank       = -1;
        md->size       = 0;
        md->group_comm = MPI_COMM_NULL;

        of=open_file_create(method->base_path, f->name, md);
    } else {
        // sanity check
        if (md->fd == -1) {
            if (DEBUG>3) printf("open: %s is open but fd==-1.  sanity check failed.  attempting reopen.\n", f->name);
            list_rem_next(&open_file_list, of, &md);
        } else {
            // file already open
            return adios_flag_no;
        }
    }

    if (DEBUG>3) printf("global_rank(%d): enter adios_nssi_open (%s)\n", global_rank, f->name);

    md->comm = comm;
    if (DEBUG>3) printf("global_rank(%d): adios_nssi_open: setup group_comm\n", global_rank);
    adios_var_to_comm_nssi(f->group->adios_host_language_fortran, md->comm, &md->group_comm);
    if (md->group_comm != MPI_COMM_NULL) {
        if (DEBUG>3) printf("global_rank(%d): adios_nssi_open: get rank and size\n", global_rank);
        MPI_Comm_rank(md->group_comm, &md->rank);
        MPI_Comm_size(md->group_comm, &md->size);
    } else {
        md->group_comm=MPI_COMM_SELF;
    }
    f->group->process_id = md->rank;

    if (md->size <= nssi_cfg.num_servers) {
        default_svc = md->rank;
    } else {
        if ((md->size%nssi_cfg.num_servers) > 0) {
            default_svc = md->rank/((md->size/nssi_cfg.num_servers)+1);
        } else {
            default_svc = md->rank/(md->size/nssi_cfg.num_servers);
        }
    }

    /* create a new communicator for just those clients, who share a default service. */
    if (DEBUG>3) printf("global_rank(%d): adios_nssi_open: before MPI_Comm_split\n", global_rank);
    MPI_Comm_split(md->group_comm, default_svc, md->rank, &ncmpi_collective_op_comm);
    if (DEBUG>3) printf("global_rank(%d): adios_nssi_open: after MPI_Comm_split\n", global_rank);
    /* find my rank in the new communicator */
    if (DEBUG>3) printf("global_rank(%d): adios_nssi_open: before MPI_Comm_size\n", global_rank);
    MPI_Comm_size(ncmpi_collective_op_comm, &collective_op_size);
    if (DEBUG>3) printf("global_rank(%d): adios_nssi_open: before MPI_Comm_rank\n", global_rank);
    MPI_Comm_rank(ncmpi_collective_op_comm, &collective_op_rank);

    if (DEBUG>3) printf("global_rank(%d) collective_op_rank(%d) default_service(%d)\n", md->rank, collective_op_rank, default_svc);

    svcs=(nssi_service *)calloc(nssi_cfg.num_servers, sizeof(nssi_service));
    /* !global_rank0 has a preferred server for data transfers.  connect to preferred server.
     * connect to other servers on-demand.
     */
    double GetSvcTime=MPI_Wtime();
    if (DEBUG>3) printf("get staging-service: default_svc(%d) nid(%lld) pid(%llu) hostname(%s) port(%d)\n",
            default_svc,
            nssi_cfg.nssi_server_ids[default_svc].nid,
            nssi_cfg.nssi_server_ids[default_svc].pid,
            nssi_cfg.nssi_server_ids[default_svc].hostname,
            nssi_cfg.nssi_server_ids[default_svc].port);
    rc = nssi_get_service(nssi_cfg.nssi_server_ids[default_svc], -1, &svcs[default_svc]);
    if (rc != NSSI_OK) {
        //log_error(adios_nssi_debug_level, "Couldn't connect to netcdf master: %s", nssi_err_str(rc));
        return;
    }


    gen_offset_list(
            f->group,
            f->group->vars,
            f->group->attributes);
    gen_dim_list(
            f->group,
            f->group->vars,
            f->group->attributes);
    var_offset_printall();

    args.fname = malloc(sizeof(char) * (strlen(method->base_path) + strlen(f->name) + 1));
    sprintf(args.fname, "%s%s", method->base_path, f->name);
    args.gname = strdup(method->group->name);
    switch(f->mode) {
        case adios_mode_read:
            args.mode = ADIOS_MODE_READ;
            break;
        case adios_mode_write:
            args.mode = ADIOS_MODE_WRITE;
            break;
        case adios_mode_append:
            args.mode = ADIOS_MODE_APPEND;
            break;
        case adios_mode_update:
            args.mode = ADIOS_MODE_UPDATE;
            break;
    }

    Func_Timer("ADIOS_OPEN_OP",
            rc = nssi_call_rpc_sync(&svcs[default_svc],
            ADIOS_OPEN_OP,
            &args,
            NULL,
            0,
            &res););

    md->fd = res.fd;

    list_ins_next(&open_file_list, list_head(&open_file_list), of);

    free(args.fname);
    free(args.gname);

    return 1;
}

void adios_nssi_start_calculation(
        struct adios_method_struct * method)
{
    int rc=NSSI_OK;
    int myrank;

    adios_start_calc_args args;

    ListElmt *of_elmt;
//    ListElmt *req_elmt;
    struct open_file *of=NULL;
    struct adios_nssi_data_struct *md=NULL;
//    nssi_request *req;

    if (DEBUG>3) printf("rank(%d) enter adios_nssi_start_calc\n", global_rank);


    of_elmt = list_head(&open_file_list);
    while(of_elmt) {
        of = list_data(of_elmt);
        if (of == NULL) {
            fprintf(stderr, "file is not open.  FAIL.");
            return;
        }
        md=of->md;
        myrank=md->rank;

//        MPI_Barrier(md->group_comm);
        if (collective_op_rank == 0) {
//            nssi_request *req=calloc(1, nssi_request);
            args.fd = md->fd;
            Func_Timer("ADIOS_START_CALC_OP",
                    rc = nssi_call_rpc(&svcs[default_svc],
                    ADIOS_START_CALC_OP,
                    &args,
                    NULL,
                    0,
                    NULL,
                    &of->start_calc_req););
            if (rc != NSSI_OK) {
                //log_error(adios_nssi_debug_level, "unable to call remote adios_read");
            }
        }
//        MPI_Barrier(md->group_comm);

        of_elmt = list_next(of_elmt);
    }


    return;
}

void adios_nssi_end_iteration(
        struct adios_method_struct * method)
{
    int rc=NSSI_OK;
    int myrank;

    if (DEBUG>3) printf("rank(%d) enter adios_nssi_end_iter\n", global_rank);


//    adios_end_iter_args args;
//
//    struct open_file *of=NULL;
//    struct adios_nssi_data_struct *md=NULL;
//
//    of=open_file_find(method->base_path, f->name);
//    if (of == NULL) {
//        fprintf(stderr, "file is not open.  FAIL.");
//        return;
//    }
//    md=of->md;
//
//    myrank=md->rank;
//
//    MPI_Barrier(md->group_comm);
//    if (collective_op_rank == 0) {
//        args.fd = md->fd;
//        Func_Timer("ADIOS_END_ITER_OP",
//                rc = nssi_call_rpc_sync(&svcs[default_svc],
//                ADIOS_END_ITER_OP,
//                &args,
//                NULL,
//                0,
//                NULL););
//        if (rc != NSSI_OK) {
//            //log_error(adios_nssi_debug_level, "unable to call remote adios_read");
//        }
//    }
//    MPI_Barrier(md->group_comm);

    return;
}

void adios_nssi_stop_calculation(
        struct adios_method_struct * method)
{
    int rc=NSSI_OK;
    int remote_rc=NSSI_OK;
    int myrank;

    adios_stop_calc_args args;

    ListElmt *of_elmt;
//    ListElmt *req_elmt;
    struct open_file *of=NULL;
    struct adios_nssi_data_struct *md=NULL;
//    nssi_request *req;

    if (DEBUG>3) printf("rank(%d) enter adios_nssi_stop_calc\n", global_rank);


    of_elmt = list_head(&open_file_list);
    while(of_elmt) {
        of = list_data(of_elmt);
        if (of == NULL) {
            fprintf(stderr, "file is not open.  FAIL.");
            return;
        }
        md=of->md;
        myrank=md->rank;

        // wait for any async writes to finish
//        req_elmt = list_head(&(of->outstanding_reqs));
//        while(req_elmt) {
//        	req = list_data(req_elmt);
//        	if (req == NULL) {
//        		fprintf(stderr, "file is not open.  FAIL.");
//        		return;
//        	}
//
//        	nssi_wait(req, &remote_rc);
//
//        	req_elmt = list_next(req_elmt);
//        }

        if (collective_op_rank == 0) {
            nssi_wait(&of->start_calc_req, &remote_rc);
        }

        of_elmt = list_next(of_elmt);
    }


    of_elmt = list_head(&open_file_list);
    while(of_elmt) {
        of = list_data(of_elmt);
        if (of == NULL) {
            fprintf(stderr, "file is not open.  FAIL.");
            return;
        }
        md=of->md;
        myrank=md->rank;

//        MPI_Barrier(md->group_comm);
        if (collective_op_rank == 0) {
            args.fd = md->fd;
            Func_Timer("ADIOS_STOP_CALC_OP",
                    rc = nssi_call_rpc_sync(&svcs[default_svc],
                            ADIOS_STOP_CALC_OP,
                            &args,
                            NULL,
                            0,
                            NULL););
            if (rc != NSSI_OK) {
                //log_error(adios_nssi_debug_level, "unable to call remote adios_read");
            }
        }
//        MPI_Barrier(md->group_comm);

        of_elmt = list_next(of_elmt);
    }

    if (DEBUG>3) printf("rank(%d) exit adios_nssi_stop_calc\n", global_rank);

    return;
}

void adios_nssi_write(
        struct adios_file_struct *f,
        struct adios_var_struct *v,
        void *data,
        struct adios_method_struct *method)
{
    static int first_write = 1;

    struct open_file *of=NULL;
    struct adios_nssi_data_struct *md=NULL;

    if (DEBUG>3) printf("rank(%d) enter adios_nssi_write\n", global_rank);

    of=open_file_find(method->base_path, f->name);
    if (of == NULL) {
        fprintf(stderr, "file is not open.  FAIL.");
        return;
    }
    md=of->md;

    if (f->mode == adios_mode_write || f->mode == adios_mode_append) {

        if (md->rank==0) {
            if (DEBUG>3) fprintf(stderr, "-------------------------\n");
            if (DEBUG>3) fprintf(stderr, "write var: %s start!\n", v->name);
        }
        uint64_t var_size = adios_get_var_size (v, f->group, data);
        if (DEBUG>3) printf("vname(%s) vsize(%ld)\n", v->name, var_size);
        write_var(md->fd,
                f->group,
                f->group->vars,
                f->group->attributes,
                v,
                var_size,
                f->group->adios_host_language_fortran,
                md->rank,
                md->size,
                md->group_comm);
    } else {
        if (DEBUG>3) fprintf(stderr, "entering unknown nc4 mode %d!\n", f->mode);
    }
    if (md->rank==0) {
        if (DEBUG>3) fprintf(stderr, "write var: %s end!\n", v->name);
        if (DEBUG>3) fprintf(stderr, "-------------------------\n");
    }

    return;
}


void adios_nssi_read(
        struct adios_file_struct *f,
        struct adios_var_struct *v,
        void *buffer,
        uint64_t buffersize,
        struct adios_method_struct *method)
{
    struct open_file *of=NULL;
    struct adios_nssi_data_struct *md=NULL;

    if (DEBUG>3) printf("rank(%d) enter adios_nssi_read\n", global_rank);

    of=open_file_find(method->base_path, f->name);
    if (of == NULL) {
        fprintf(stderr, "file is not open.  FAIL.");
        return;
    }
    md=of->md;

    if(f->mode == adios_mode_read) {
        v->data = buffer;
        v->data_size = buffersize;

        if (md->rank==0) {
            if (DEBUG>3) fprintf(stderr, "-------------------------\n");
            if (DEBUG>3) fprintf(stderr, "read var: %s! start\n", v->name);
        }
        read_var(md->fd,
                v,
                md->rank,
                md->size,
                md->group_comm);
        if (md->rank==0) {
            if (DEBUG>3) fprintf(stderr, "read var: %s! end\n", v->name);
            if (DEBUG>3) fprintf(stderr, "-------------------------\n");
        }
    }

    return;
}

static void adios_nssi_do_read(
        struct adios_file_struct *f,
        struct adios_method_struct *method)
{
    // This function is not useful for nc4 since adios_read/write do real read/write
}

void adios_nssi_close(
        struct adios_file_struct *f,
        struct adios_method_struct *method)
{
    int rc=NSSI_OK;
    struct adios_attribute_struct * a = f->group->attributes;
    int myrank;

    adios_start_calc_args start_calc_args;
    adios_close_args close_args;

    struct open_file *of=NULL;
    struct adios_nssi_data_struct *md=NULL;

    if (DEBUG>3) printf("global_rank(%d) enter adios_nssi_close\n", global_rank);

    of=open_file_find(method->base_path, f->name);
    if (of == NULL) {
        fprintf(stderr, "file is not open.  FAIL.");
        return;
    }
    md=of->md;
    myrank=md->rank;

    if (DEBUG>3) printf("myrank(%d) enter adios_nssi_close\n", myrank);

    if (f->mode == adios_mode_read) {
        if (md->rank==0) {
            if (DEBUG>1) fprintf(stderr, "-------------------------\n");
            if (DEBUG>1) fprintf(stderr, "reading done, NSSI file is virtually closed;\n");
            if (DEBUG>1) fprintf(stderr, "-------------------------\n");
        }
    } else if (f->mode == adios_mode_write || f->mode == adios_mode_append) {
        //fprintf(stderr, "entering nc4 write attribute mode!\n");
        if (md->rank==0) {
            if (DEBUG>1) fprintf(stderr, "-------------------------\n");
            if (DEBUG>1) fprintf(stderr, "writing done, NSSI file is virtually closed;\n");
            if (DEBUG>1) fprintf(stderr, "-------------------------\n");
        }
    }

//    MPI_Barrier(md->group_comm);
//    if (collective_op_rank == 0) {
//        start_calc_args.fd = md->fd;
//        Func_Timer("ADIOS_START_CALC_OP",
//                rc = nssi_call_rpc_sync(&svcs[default_svc],
//                ADIOS_START_CALC_OP,
//                &start_calc_args,
//                NULL,
//                0,
//                NULL););
//        if (rc != NSSI_OK) {
//            //log_error(adios_nssi_debug_level, "unable to call remote adios_read");
//        }
//    }
//    MPI_Barrier(md->group_comm);

    MPI_Barrier(md->group_comm);
    if (collective_op_rank == 0) {
        close_args.fname = malloc(sizeof(char) * (strlen(method->base_path) + strlen(f->name) + 1));
        sprintf(close_args.fname, "%s%s", method->base_path, f->name);
        close_args.fd = md->fd;
        if (DEBUG>3) printf("rank(%d) sending ADIOS_CLOSE_OP\n", myrank);
        Func_Timer("ADIOS_CLOSE_OP",
                rc = nssi_call_rpc_sync(&svcs[default_svc],
                ADIOS_CLOSE_OP,
                &close_args,
                NULL,
                0,
                NULL););
        if (rc != NSSI_OK) {
            //log_error(adios_nssi_debug_level, "unable to call remote adios_read");
        }
        free(close_args.fname);
    }
    MPI_Barrier(md->group_comm);

    md->group_comm = MPI_COMM_NULL;
    md->fd = -1;
    md->rank = -1;
    md->size = 0;

    if (DEBUG>3) printf("global_rank(%d) exit adios_nssi_close\n", global_rank);

    return;
}

void adios_nssi_finalize(
        int mype,
        struct adios_method_struct *method)
{
    int rc=NSSI_OK;
    int myrank;

    if (DEBUG>3) printf("rank(%d) enter adios_nssi_finalize\n", global_rank);

//    adios_close_args close_args;
//
//    ListElmt *of_elmt;
////    ListElmt *req_elmt;
//    struct open_file *of=NULL;
//    struct adios_nssi_data_struct *md=NULL;
////    nssi_request *req;
//
//    of_elmt = list_head(&open_file_list);
//    while(of_elmt) {
//        of = list_data(of_elmt);
//        if (of == NULL) {
//            fprintf(stderr, "file is not open.  FAIL.");
//            return;
//        }
//        md=of->md;
//        myrank=md->rank;
//
////        req_elmt = list_head(&(of->outstanding_reqs));
////        while(req_elmt) {
////        	req = list_data(req_elmt);
////        	if (req == NULL) {
////        		fprintf(stderr, "file is not open.  FAIL.");
////        		return;
////        	}
////
////        	req_elmt = list_next(req_elmt);
////        }
//
////        MPI_Barrier(md->group_comm);
//        if (collective_op_rank == 0) {
//            close_args.fname = malloc(sizeof(char) * (strlen(of->fpath) + strlen(of->fname) + 1));
//            sprintf(close_args.fname, "%s%s", of->fpath, of->fname);
//            close_args.fd = md->fd;
//            Func_Timer("ADIOS_CLOSE_OP",
//                    rc = nssi_call_rpc_sync(&svcs[default_svc],
//                    ADIOS_CLOSE_OP,
//                    &close_args,
//                    NULL,
//                    0,
//                    NULL););
//            if (rc != NSSI_OK) {
//                //log_error(adios_nssi_debug_level, "unable to call remote adios_read");
//            }
//            free(close_args.fname);
//        }
////        MPI_Barrier(md->group_comm);
//
//        md->group_comm = MPI_COMM_NULL;
//        md->fd = -1;
//        md->rank = -1;
//        md->size = 0;
//
//        of_elmt = list_next(of_elmt);
//    }


    free_nssi_config(&nssi_cfg);

    if (adios_nssi_initialized)
        adios_nssi_initialized = 0;
}

#endif