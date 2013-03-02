/* 
 * ADIOS is freely available under the terms of the BSD license described
 * in the COPYING file in the top level directory of this source distribution.
 *
 * Copyright (c) 2008 - 2009.  UT-BATTELLE, LLC. All rights reserved.
 */

/*************************************************************/
/*          Example of reading arrays in ADIOS               */
/*    which were written from the same number of processors  */
/*                                                           */
/*        Similar example is manual/2_adios_read.c           */
/*************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "mpi.h"
#include "adios.h"
#include "public/adios_read.h"

int main (int argc, char ** argv) 
{
    /* application data structures */
    char        filename [256];
    int         rank, size, i, j;
    int         NX, NY; 
    double      *t;
    int         *p;

    /* MPI and ADIOS data structures */
    MPI_Comm    comm = MPI_COMM_WORLD;
    int         adios_err;
    int64_t     adios_handle, adios_buf_size;

    /* MPI and ADIOS setup */
    MPI_Init (&argc, &argv);
    MPI_Comm_rank (comm, &rank);
    adios_read_init_method(ADIOS_READ_METHOD_FLEXPATH, comm, "");

    /* First read in the scalars to calculate the size of the arrays */
    ADIOS_FILE* afile = adios_read_open_file("arrays", ADIOS_READ_METHOD_FLEXPATH, comm);
    /* get everything from single process - rank 0 for now*/
    ADIOS_SELECTION process_select;
    process_select.type=ADIOS_SELECTION_WRITEBLOCK;
    process_select.u.block.index = 0;

    /* read the size of arrays using local inq_var */
    ADIOS_VARINFO* nx_info = adios_inq_var( afile, "NX");
    if(nx_info->value) {
        NX = *((int *)nx_info->value);
    }

    ADIOS_VARINFO* ny_info = adios_inq_var( afile, "NY");
    if(ny_info->value) {
        NY = *((int *)ny_info->value);
    }

    /* Allocate space for the arrays */
    t = (double *) malloc (NX*NY*sizeof(double));    
    p = (int *) malloc (NX*sizeof(int));
    memset(t, 0, NX*NY*sizeof(double));
    memset(p, 0, NX*sizeof(int));

    /* Read arrays for each time step */
    int ii=0;
    for(ii=0; ii<200; ii++) {        

        /* schedule a read of the arrays */
        adios_schedule_read (afile, &process_select, "var_double_2Darray", 0, 1, t);
        adios_schedule_read (afile, &process_select, "var_int_1Darray", 0, 1, p);
        
        /* commit request and retrieve data */
        adios_perform_reads (afile, 1);

        /* print result */
        printf("Results Rank=%d Step=%d\n p[0] = %d t[0,0] = %f\n\n", rank, ii, p[0], t[0]);
    
        /* block until next step is available (30 sec timeout unsupported) */
        adios_advance_step(afile, 0, 30);

    }
    
    /* wait until all readers finish */
    MPI_Barrier (comm);

    /* shutdown ADIOS and MPI */
    adios_read_close(afile);
    adios_read_finalize_method(ADIOS_READ_METHOD_FLEXPATH);
    MPI_Finalize ();

    return 0;
}