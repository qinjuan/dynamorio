/**************************************************************************************
                            C-DAC Tech Workshop : HeGaPa-2012
                             July 16-20,2012
 Example   : pthread-jacobi.c

 Objective      : Jacobi method to solve AX = b matrix system of linear equations.

 Input          : Class Size
      Number of Threads

 Output         : The solution of  Ax = b or
                  The status of convergence for given bumber of iterations

 Created  : MAY-2012


 E-mail        : hpcfte@cdac.in

*************************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include "annotation/dynamorio_annotations.h"
#include "annotation/bbcount_region_annotations.h"
#include "annotation/memcheck.h"

#define MAX_ITERATIONS 1000
#define MAXTHREADS 8

double Distance(double *X_Old, double *X_New, int matrix_size);
pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;

double   **Matrix_A, *RHS_Vector;
double   *X_New, *X_Old, *Bloc_X, rno,sum;

typedef struct _thread_init_t {
    uint id;
    int inner_iteration_count;
    int outer_iteration_count;
} thread_init_t;

int Number;
void jacobi(thread_init_t *);
static int thread_handling_index;

int main(int argc, char **argv)
{
  double diag_dominant_factor  = 4.0000;
  double tolerance  = 1.0E-5;
  /* .......Variables Initialisation ...... */
  int matrix_size,  NoofRows, NoofCols,CLASS_SIZE,THREADS;
  int NumThreads,ithread;
  double rowsum;
  double  sum;
  int irow, icol, index, Iteration,iteration,ret_count;
  double time_start, time_end,memoryused;
  struct timeval tv;
  struct timezone tz;
  char CLASS;
  FILE *fp;

  pthread_attr_t pta;
  pthread_t *threads;

  memoryused =0.0;

  printf("\n    ---------------------------------------------------------------------------");
  printf("\n     Centre for Development of Advanced Computing (C-DAC)");
  printf("\n     Email : hpcfte@cdac.in");
  printf("\n    ---------------------------------------------------------------------------");
  printf("\n     Objective : To Solve AX=B Linear Equation (Jacobi Method)\n ");
  printf("\n     Performance for solving AX=B Linear Equation using JACOBI METHOD");
  if (DYNAMORIO_ANNOTATE_RUNNING_ON_DYNAMORIO())
    printf("\n     Running on DynamoRIO");
  else
    printf("\n     Running native");
  printf("\n    ..........................................................................\n");

  if( argc != 2 ) {
    printf("     Very Few Arguments\n ");
    printf("     Syntax : exec <Class-Size (Give A/B/C)> <Threads>\n");
    exit(-1);
  } else {
    CLASS = *argv[1];
    THREADS = atoi(argv[1] + 1);
  }
  if (THREADS > MAXTHREADS ) {
    printf("\n Number of Threads must be less than or equal to 8. Aborting ...\n");
    return 1;
  }
  if( CLASS == 'A' ) {
    CLASS_SIZE = 1024;
  }
  if( CLASS == 'B' ) {
    CLASS_SIZE = 2048;
  }
  if( CLASS == 'C' ) {
    CLASS_SIZE = 4096;
  }

  matrix_size = CLASS_SIZE;
  NumThreads = THREADS;
  printf("\n     Matrix Size :  %d",matrix_size);
  printf("\n     Threads     :  %d",NumThreads);

  NoofRows = matrix_size;
  NoofCols = matrix_size;


  /* Allocate The Memory For Matrix_A and RHS_Vector */
  Matrix_A = (double **) malloc(matrix_size * sizeof(double *));
  RHS_Vector = (double *) malloc(matrix_size * sizeof(double));


  /* Populating the Matrix_A and RHS_Vector */
  rowsum = (double) (matrix_size *(matrix_size+1)/2.0);
  for (irow = 0; irow < matrix_size; irow++) {
    Matrix_A[irow] = (double *) malloc(matrix_size * sizeof(double));
    sum = 0.0;
    for (icol = 0; icol < matrix_size; icol++) {
      Matrix_A[irow][icol]= (double) (icol+1);
      if(irow == icol )  Matrix_A[irow][icol] = (double)(rowsum);
      sum = sum + Matrix_A[irow][icol];
    }
    RHS_Vector[irow] = (double)(2*rowsum) - (double)(irow+1);
   }

   memoryused+=(NoofRows * NoofCols * sizeof(double));
   memoryused+=(NoofRows * sizeof(double));

   printf("\n");

  if (NoofRows != NoofCols) {
    printf("Input Matrix Should Be Square Matrix ..... \n");
    exit(-1);
  }

  /* Dynamic Memory Allocation */
  X_New = (double *) malloc(matrix_size * sizeof(double));
  memoryused+=(NoofRows * sizeof(double));
  X_Old = (double *) malloc(matrix_size * sizeof(double));
  memoryused+=(NoofRows * sizeof(double));
  Bloc_X = (double *) malloc(matrix_size * sizeof(double));
  memoryused+=(NoofRows * sizeof(double));

  VALGRIND_MAKE_MEM_DEFINED_IF_ADDRESSABLE(X_New, matrix_size * sizeof(double));

  /* Calculating the time of Operation Start */
  gettimeofday(&tv, &tz);
  time_start= (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;

  /* Initailize X[i] = B[i] */

  for (irow = 0; irow < matrix_size; irow++) {
    Bloc_X[irow] = RHS_Vector[irow];
    X_New[irow] =  RHS_Vector[irow];
  }

  for (ithread=0; ithread<NumThreads; ithread++) {
      char counter_name[32] = {0};
      sprintf(counter_name, "thread #%d", ithread);
      BB_REGION_ANNOTATE_INIT_COUNTER(ithread, counter_name);
  }
  thread_handling_index = NumThreads;
  BB_REGION_ANNOTATE_INIT_COUNTER(thread_handling_index, "thread-handling");

  /* Allocating the memory for user specified number of threads */
  threads = (pthread_t *) malloc(sizeof(pthread_t) * NumThreads);

  /* Initializating the thread attribute */
  pthread_attr_init(&pta);

  Iteration = 0;
  do {
    thread_init_t *thread_inits = malloc(sizeof(thread_init_t) * NumThreads);
    BB_REGION_ANNOTATE_START_COUNTER(thread_handling_index);
    for(index = 0; index < matrix_size; index++)
      X_Old[index] = X_New[index];
    for(ithread=0;ithread<NumThreads;ithread++) {
      thread_inits[ithread].id = ithread;
      thread_inits[ithread].inner_iteration_count = matrix_size/NumThreads;
      thread_inits[ithread].outer_iteration_count = Iteration;
      /* Creating The Threads */
      ret_count = pthread_create(&threads[ithread], &pta, (void *(*) (void *))jacobi,
        (void *) &thread_inits[ithread]);
      if(ret_count) {
        printf("\n ERROR : Return code from pthread_create() is %d ",ret_count);
        exit(-1);
      }
    }

    Iteration++;
    for (ithread=0; ithread<NumThreads; ithread++) {
      ret_count=pthread_join(threads[ithread], NULL);
      if(ret_count) {
        printf("\n ERROR : Return code from pthread_join() is %d ",ret_count);
        exit(-1);
      }
    }
    ret_count=pthread_attr_destroy(&pta);
    if(ret_count) {
      printf("\n ERROR : Return code from pthread_attr_destroy() is %d ",ret_count);
      exit(-1);
    }
    BB_REGION_ANNOTATE_STOP_COUNTER(thread_handling_index);

    if (DYNAMORIO_ANNOTATE_RUNNING_ON_DYNAMORIO()) {
        uint region_count = 0;
        uint bb_count = 0;
        uint thread_region_count, thread_bb_count;
        for (ithread=0; ithread<NumThreads; ithread++) {
            BB_REGION_GET_BASIC_BLOCK_STATS(ithread, &thread_region_count, &thread_bb_count);
            region_count += thread_region_count;
            bb_count += thread_bb_count;
        }
        if (region_count > 0)
            printf("\n     After %d iterations, executed %d basic blocks in %d regions", Iteration,
                bb_count, region_count);
    }
  } while ((Iteration < MAX_ITERATIONS) && (Distance(X_Old, X_New, matrix_size) >= tolerance));

  /* Calculating the time at the end of Operation */
  gettimeofday(&tv, &tz);
  time_end= (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;

  printf("\n");
  printf("\n     The Jacobi Method For AX=B .........DONE");
  printf("\n     Total Number Of Iterations   :  %d",Iteration);
  printf("\n     Memory Utilized              :  %lf MB",(memoryused/(1024*1024)));
  printf("\n    ..........................................................................\n");

  /* Freeing Allocated Memory */
  free(X_New);
  free(X_Old);
  free(Matrix_A);
  free(RHS_Vector);
  free(Bloc_X);
  free(threads);

  return 0;
}

double Distance(double *X_Old, double *X_New, int matrix_size)
{
  int             index;
  double          Sum;

  BB_REGION_ANNOTATE_START_COUNTER(thread_handling_index);
  Sum = 0.0;
  for (index = 0; index < matrix_size; index++)
    Sum += (X_New[index] - X_Old[index]) * (X_New[index] - X_Old[index]);
  BB_REGION_ANNOTATE_STOP_COUNTER(thread_handling_index);
  return (Sum);
}

void jacobi(thread_init_t *init)
{
  int i,j;

  BB_REGION_ANNOTATE_START_COUNTER(init->id);
  for(i = 0; i < init->inner_iteration_count; i++) {
    Bloc_X[i] = RHS_Vector[i];

    for (j = 0;j<i;j++) {
        Bloc_X[i] -= X_Old[j] * Matrix_A[i][j];
    }
    for (j = i+1;j<init->inner_iteration_count;j++) {
      Bloc_X[i] -= X_Old[j] * Matrix_A[i][j];
    }
    Bloc_X[i] = Bloc_X[i] / Matrix_A[i][i];
  }
  for(i = 0; i < init->inner_iteration_count; i++) {
    X_New[i] = Bloc_X[i];
  }
  BB_REGION_ANNOTATE_STOP_COUNTER(init->id);
}


