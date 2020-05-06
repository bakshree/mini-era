/*
 * Copyright 2019 IBM
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "utils.h"
#include "verbose.h"

#ifdef COMPILE_TO_ESP
#include "contig.h"
#include "mini-era.h"
#endif

#include "scheduler.h"
#include "accelerators.h" // include AFTER scheduler.h -- needs types form scheduler.h

#define total_metadata_pool_blocks  32
task_metadata_block_t master_metadata_pool[total_metadata_pool_blocks];
int free_metadata_pool[total_metadata_pool_blocks];
int free_metadata_blocks = total_metadata_pool_blocks;


void print_metadata_block_contents(task_metadata_block_t* mb)
{
  printf("metadata_block_id = %d\n", mb->metadata.metadata_block_id);
  printf("    status = %d\n",  mb->metadata.status);
  printf("    job_type = %d\n",  mb->metadata.job_type);
  printf("    criticality_level = %d\n",  mb->metadata.criticality_level);
  printf("    data_size  = %d\n",  mb->metadata.data_size);
}

task_metadata_block_t* get_task_metadata_block()
{
  printf("in get_task_metadata_block with %u free_metadata_blocks\n", free_metadata_blocks);
  if (free_metadata_blocks < 1) {
    // Out of metadata blocks -- all in use, cannot enqueue new tasks!
    return NULL;
  }
  int bi = free_metadata_pool[free_metadata_blocks - 1];
  free_metadata_pool[free_metadata_blocks - 1] = -1;
  free_metadata_blocks -= 1;
  // For neatness (not "security") we'll clear the meta-data in the block (not the data data,though)
  master_metadata_pool[bi].metadata.job_type = -1; // unset
  master_metadata_pool[bi].metadata.status = 0;    // allocated
  master_metadata_pool[bi].metadata.criticality_level = 0; // lowest/free?
  master_metadata_pool[bi].metadata.data_size = 0;
  return &(master_metadata_pool[bi]);
}


void free_task_metadata_block(task_metadata_block_t* mb)
{
  int bi = mb->metadata.metadata_block_id;
  if (free_metadata_blocks < total_metadata_pool_blocks) {
    free_metadata_pool[free_metadata_blocks] = bi;
    free_metadata_blocks += 1;
    // For neatness (not "security") we'll clear the meta-data in the block (not the data data,though)
    master_metadata_pool[bi].metadata.job_type = -1; // unset
    master_metadata_pool[bi].metadata.status = -1;   // free
    master_metadata_pool[bi].metadata.criticality_level = 0; // lowest/free?
    master_metadata_pool[bi].metadata.data_size = 0;
  } else {
    printf("ERROR : We are freeing a metadata block when we already have max metadata blocks free...\n");
    printf("   THE FREE Metadata Blocks list:\n");
    for (int ii = 0; ii < free_metadata_blocks; ii++) {
      printf("        free[%2u] = %u\n", ii, free_metadata_pool[ii]);
    }
    printf("    THE Being-Freed Metat-Data Block:\n");
    print_metadata_block_contents(mb);
    exit(-5);
  }
}

int
get_task_status(int task_id) {
  return master_metadata_pool[task_id].metadata.status;
}

static unsigned DMA_WORD_PER_BEAT(unsigned _st)
{
        return (sizeof(void *) / _st);
}

#ifdef HW_VIT
// These are Viterbi Harware Accelerator Variables, etc.
#define  NUM_VIT_ACCEL  4
char*    vitAccelName[NUM_VIT_ACCEL] = {"/dev/vitdodec.0", "/dev/vitdodec.1", "/dev/vitdodec.2", "/dev/vitdodec.3"};

int vitHW_fd[NUM_VIT_ACCEL];
contig_handle_t vitHW_mem[NUM_VIT_ACCEL];
vitHW_token_t *vitHW_lmem[NUM_VIT_ACCEL];   // Pointer to local view of contig memory
vitHW_token_t *vitHW_li_mem[NUM_VIT_ACCEL]; // Pointer to input memory block
vitHW_token_t *vitHW_lo_mem[NUM_VIT_ACCEL]; // Pointer to output memory block
size_t vitHW_in_len[NUM_VIT_ACCEL];
size_t vitHW_out_len[NUM_VIT_ACCEL];
size_t vitHW_in_size[NUM_VIT_ACCEL];
size_t vitHW_out_size[NUM_VIT_ACCEL];
size_t vitHW_out_offset[NUM_VIT_ACCEL];
size_t vitHW_size[NUM_VIT_ACCEL];

struct vitdodec_access vitHW_desc[NUM_VIT_ACCEL];


static void init_vit_parameters(int vn)
{
	size_t vitHW_in_words_adj;
	size_t vitHW_out_words_adj;
	//printf("Doing init_vit_parameters\n");
	if (DMA_WORD_PER_BEAT(sizeof(vitHW_token_t)) == 0) {
		vitHW_in_words_adj  = 24852;
		vitHW_out_words_adj = 18585;
	} else {
		vitHW_in_words_adj  = round_up(24852, DMA_WORD_PER_BEAT(sizeof(vitHW_token_t)));
		vitHW_out_words_adj = round_up(18585, DMA_WORD_PER_BEAT(sizeof(vitHW_token_t)));
	}
	vitHW_in_len[vn] = vitHW_in_words_adj;
	vitHW_out_len[vn] =  vitHW_out_words_adj;
	vitHW_in_size[vn] = vitHW_in_len[vn] * sizeof(vitHW_token_t);
	vitHW_out_size[vn] = vitHW_out_len[vn] * sizeof(vitHW_token_t);
	vitHW_out_offset[vn] = vitHW_in_len[vn];
	vitHW_size[vn] = (vitHW_out_offset[vn] * sizeof(vitHW_token_t)) + vitHW_out_size[vn];
}
#endif // HW_VIT


// Right now default to max of 16k-samples
unsigned fft_logn_samples = 14; // Defaults to 16k samples

#ifdef HW_FFT
// These are FFT Hardware Accelerator Variables, etc.
#define NUM_FFT_ACCEL 4
char* fftAccelName[NUM_FFT_ACCEL] = {"/dev/fft.0", "/dev/fft.1", "/dev/fft.2", "/dev/fft.3"};

int fftHW_fd[NUM_FFT_ACCEL];
contig_handle_t fftHW_mem[NUM_FFT_ACCEL];

fftHW_token_t* fftHW_lmem[NUM_FFT_ACCEL];  // Pointer to local version (mapping) of fftHW_mem
fftHW_token_t* fftHW_li_mem[NUM_FFT_ACCEL]; // Pointer to input memory block
fftHW_token_t* fftHW_lo_mem[NUM_FFT_ACCEL]; // Pointer to output memory block
size_t fftHW_in_len[NUM_FFT_ACCEL];
size_t fftHW_out_len[NUM_FFT_ACCEL];
size_t fftHW_in_size[NUM_FFT_ACCEL];
size_t fftHW_out_size[NUM_FFT_ACCEL];
size_t fftHW_out_offset[NUM_FFT_ACCEL];
size_t fftHW_size[NUM_FFT_ACCEL];
struct fftHW_access fftHW_desc[NUM_FFT_ACCEL];


/* User-defined code */
static void init_fft_parameters(unsigned n)
{
	size_t fftHW_in_words_adj;
	size_t fftHW_out_words_adj;
	int len = 1 << fft_logn_samples;
	if (DMA_WORD_PER_BEAT(sizeof(fftHW_token_t)) == 0) {
		fftHW_in_words_adj  = 2 * len;
		fftHW_out_words_adj = 2 * len;
	} else {
		fftHW_in_words_adj = round_up(2 * len, DMA_WORD_PER_BEAT(sizeof(fftHW_token_t)));
		fftHW_out_words_adj = round_up(2 * len, DMA_WORD_PER_BEAT(sizeof(fftHW_token_t)));
	}
	fftHW_in_len[n] = fftHW_in_words_adj;
	fftHW_out_len[n] =  fftHW_out_words_adj;
	fftHW_in_size[n] = fftHW_in_len[n] * sizeof(fftHW_token_t);
	fftHW_out_size[n] = fftHW_out_len[n] * sizeof(fftHW_token_t);
	fftHW_out_offset[n] = 0;
	fftHW_size[n] = (fftHW_out_offset[n] * sizeof(fftHW_token_t)) + fftHW_out_size[n];
}
#endif // HW_FFT

status_t initialize_scheduler()
{
	DEBUG(printf("In initialize...\n"));
	for (int i = 0; i < total_metadata_pool_blocks; i++) {
	  free_metadata_pool[i] = i;
	}
	
#ifdef HW_FFT
	// This initializes the FFT Accelerator Pool
	for (int fi = 0; fi < NUM_FFT_ACCEL; fi++) {
		DEBUG(printf("Init FFT parameters on acclerator %u\n", fi));
		init_fft_parameters(fi);

		DEBUG(printf(" Acclerator %u opening FFT device %s\n", fi, fftAccelName[fi]));
		fftHW_fd[fi] = open(fftAccelName[fi], O_RDWR, 0);
		if (fftHW_fd[fi] < 0) {
			fprintf(stderr, "Error: cannot open %s", fftAccelName[fi]);
			exit(EXIT_FAILURE);
		}

		printf(" Allocate hardware buffer of size %u\n", fftHW_size[fi]);
		fftHW_lmem[fi] = contig_alloc(fftHW_size[fi], &(fftHW_mem[fi]));
		if (fftHW_lmem[fi] == NULL) {
			fprintf(stderr, "Error: cannot allocate %zu contig bytes", fftHW_size[fi]);
			exit(EXIT_FAILURE);
		}

		fftHW_li_mem[fi] = &(fftHW_lmem[fi][0]);
		fftHW_lo_mem[fi] = &(fftHW_lmem[fi][fftHW_out_offset[fi]]);
		printf(" Set fftHW_li_mem = %p  AND fftHW_lo_mem = %p\n", fftHW_li_mem[fi], fftHW_lo_mem[fi]);

		fftHW_desc[fi].esp.run = true;
		fftHW_desc[fi].esp.coherence = ACC_COH_NONE;
		fftHW_desc[fi].esp.p2p_store = 0;
		fftHW_desc[fi].esp.p2p_nsrcs = 0;
		//fftHW_desc[fi].esp.p2p_srcs = {"", "", "", ""};
		fftHW_desc[fi].esp.contig = contig_to_khandle(fftHW_mem[fi]);

		// Always use BIT-REV in HW for now -- simpler interface, etc.
		fftHW_desc[fi].do_bitrev  = FFTHW_DO_BITREV;

		//fftHW_desc[fi].len      = fftHW_len;
		fftHW_desc[fi].log_len    = fft_logn_samples; 
		fftHW_desc[fi].src_offset = 0;
		fftHW_desc[fi].dst_offset = 0;
	}
#endif

#ifdef HW_VIT
	// This initializes the Viterbi Accelerator Pool
	for (int vi = 0; vi < NUM_VIT_ACCEL; vi++) {
		DEBUG(printf("Init Viterbi parameters on acclerator %u\n", vi));
		init_vit_parameters(vi);

		printf(" Accelerator %u opening Vit-Do-Decode device %s\n", vi, vitAccelName[vi]);
		vitHW_fd[vi] = open(vitAccelName[vi], O_RDWR, 0);
		if(vitHW_fd < 0) {
			fprintf(stderr, "Error: cannot open %s", vitAccelName[vi]);
			exit(EXIT_FAILURE);
		}

		vitHW_lmem[vi] = contig_alloc(vitHW_size[vi], &(vitHW_mem[vi]));
		if (vitHW_lmem[vi] == NULL) {
			fprintf(stderr, "Error: cannot allocate %zu contig bytes", vitHW_size[vi]);
			exit(EXIT_FAILURE);
		}
		vitHW_li_mem[vi] = &(vitHW_lmem[vi][0]);
		vitHW_lo_mem[vi] = &(vitHW_lmem[vi][vitHW_out_offset[vi]]);
		printf(" Set vitHW_li_mem = %p  AND vitHW_lo_mem = %p\n", vitHW_li_mem[vi], vitHW_lo_mem[vi]);

		vitHW_desc[vi].esp.run = true;
		vitHW_desc[vi].esp.coherence = ACC_COH_NONE;
		vitHW_desc[vi].esp.p2p_store = 0;
		vitHW_desc[vi].esp.p2p_nsrcs = 0;
		vitHW_desc[vi].esp.contig = contig_to_khandle(vitHW_mem[vi]);
	}
#endif
	DEBUG(printf("DONE with initialize -- returning success\n"));
	return success;
}


#ifdef COMPILE_TO_ESP
#include "fixed_point.h"
#endif
#include "calc_fmcw_dist.h"

#ifdef HW_FFT
unsigned int fft_rev(unsigned int v)
{
        unsigned int r = v;
        int s = sizeof(v) * CHAR_BIT - 1;

        for (v >>= 1; v; v >>= 1) {
                r <<= 1;
                r |= v & 1;
                s--;
        }
        r <<= s;
        return r;
}

void fft_bit_reverse(float *w, unsigned int n, unsigned int bits)
{
        unsigned int i, s, shift;

        s = sizeof(i) * CHAR_BIT - 1;
        shift = s - bits + 1;

        for (i = 0; i < n; i++) {
                unsigned int r;
                float t_real, t_imag;

                r = fft_rev(i);
                r >>= shift;

                if (i < r) {
                        t_real = w[2 * i];
                        t_imag = w[2 * i + 1];
                        w[2 * i] = w[2 * r];
                        w[2 * i + 1] = w[2 * r + 1];
                        w[2 * r] = t_real;
                        w[2 * r + 1] = t_imag;
                }
        }
}

static void fft_in_hw(int *fd, struct fftHW_access *desc)
{
  if (ioctl(*fd, FFTHW_IOC_ACCESS, *desc)) {
    perror("IOCTL:");
    exit(EXIT_FAILURE);
  }
}
#endif

void
execute_hwr_fft_accelerator(int fn, task_metadata_block_t* task_metadata_block)
{
  DEBUG(printf("In execute_hwr_fft_accelerator: MB %d  CL %d\n", task_metadata_block.metadata.metadata_block_id, task_metadata_block.metadata.criticality_level ));
#ifdef HW_FFT
  float * data = (float*)(task_metadata_block->metadata.data);
  // convert input from float to fixed point
  for (int j = 0; j < 2 * (1 << fft_logn_samples); j++) {
    fftHW_lmem[fn][j] = float2fx(data[j], FX_IL);
  }

  // Call the FFT Accelerator
  //    NOTE: Currently this is blocking-wait for call to complete
  fft_in_hw(&(fftHW_fd[fn]), &(fftHW_desc[fn]));

  // convert output from fixed point to float
  for (int j = 0; j < 2 * (1 << fft_logn_samples); j++) {
    data[j] = (float)fx2float(fftHW_lmem[fn][j], FX_IL);
  }

  task_metadata_block.metadata.status = 3; // done

#else
  printf("ERROR : This executable DOES NOT support Hardware-FFT execution!\n");
  exit(-2);
#endif
}




#ifdef HW_VIT
static void do_decoding_hw(int *fd, struct vitdodec_access *desc)
{
  if (ioctl(*fd, VITDODEC_IOC_ACCESS, *desc)) {
    perror("IOCTL:");
    exit(EXIT_FAILURE);
  }
}
#endif


#ifdef INT_TIME
extern struct timeval dodec_stop, dodec_start;
extern uint64_t dodec_sec;
extern uint64_t dodec_usec;
#endif

void
execute_hwr_viterbi_accelerator(int vn, int n_cbps, int n_traceback, int n_data_bits, uint8_t* inMem, uint8_t* inDat, uint8_t* outMem)
{
	DEBUG(printf("In execute_hwr_viterbi_accelerator\n"));
    #ifdef HW_VIT
	vitHW_desc[vn].cbps = n_cbps;
	vitHW_desc[vn].ntraceback = n_traceback;
	vitHW_desc[vn].data_bits = n_data_bits;

	uint8_t* hwrInMem  = vitHW_li_mem[vn];
	uint8_t* hwrOutMem = vitHW_lo_mem[vn];
	for (int ti = 0; ti < 70; ti ++) {
		hwrInMem[ti] = inMem[ti];
	}
	hwrInMem[70] = 0;
	hwrInMem[71] = 0;
	int imi = 72;
	for (int ti = 0; ti < MAX_ENCODED_BITS; ti ++) {
		hwrInMem[imi++] = inDat[ti];
	}
	for (int ti = 0; ti < (MAX_ENCODED_BITS * 3 / 4); ti ++) {
		outMem[ti] = 0;
	}

      #ifdef INT_TIME
	gettimeofday(&dodec_start, NULL);
      #endif
	do_decoding_hw(&(vitHW_fd[vn]), &(vitHW_desc[vn]));

      #ifdef INT_TIME
	gettimeofday(&dodec_stop, NULL);
	dodec_sec  += dodec_stop.tv_sec  - dodec_start.tv_sec;
	dodec_usec += dodec_stop.tv_usec - dodec_start.tv_usec;
      #endif
#else // HW_VIT
	printf("ERROR : This executable DOES NOT support Viterbi Hardware execution!\n");
	exit(-3);
#endif // HW_VIT
}


void shutdown_scheduler()
{
  #ifdef HW_VIT
  for (int vi = 0; vi < NUM_VIT_ACCEL; vi++) {
    contig_free(vitHW_mem[vi]);
    close(vitHW_fd[vi]);
  }
  #endif

  #ifdef HW_FFT
  for (int fi = 0; fi < NUM_FFT_ACCEL; fi++) {
    contig_free(fftHW_mem[fi]);
    close(fftHW_fd[fi]);
  }
  #endif
}



#ifdef HW_FFT
#define FFT_HW_THRESHOLD 25    // 75% chance of using HWR
#else
#define FFT_HW_THRESHOLD 101   // 0% chance of using HWR
#endif

#ifdef HW_VIT
#define VITERBI_HW_THRESHOLD 25   // 75% chance to use Viterbi Hardware
#else
#define VITERBI_HW_THRESHOLD 101  // 0% chance to use Viterbi Hardware
#endif

/* void */
/* schedule_fft(task_metadata_block_t* task_metadata_block) */
/* { */
/*   // Scheduler should now run this either on CPU or FFT: */
/*   int num = (rand() % (100)); // Return a value from [0,99] */
/*   if (num >= FFT_HW_THRESHOLD) { */
/*     // Execute on hardware */
/*     printf("SCHED: executing FFT on Hardware : %u > %u\n", num, FFT_HW_THRESHOLD); */
/*     execute_hwr_fft_accelerator(0, data);  // only using FFT HW 0 for now -- still blocking, too */
/*   } else { */
/*     // Execute in CPU (softwware) */
/*     printf("SCHED: executing FFT on CPU Software : %u < %u\n", num, FFT_HW_THRESHOLD); */
/*     execute_cpu_fft_accelerator(data); */
/*   } */
/* } */

void
schedule_viterbi(int n_data_bits, int n_cbps, int n_traceback, uint8_t* inMem, uint8_t* inData, uint8_t* outMem)
{
  // Scheduler should now run this either on CPU or VITERBI:
  int num = (rand() % (100)); // Return a value from [0,99]
  if (num >= VITERBI_HW_THRESHOLD) {
    // Execute on hardware
    printf("SCHED: executing VITERBI on Hardware : %u > %u\n", num, VITERBI_HW_THRESHOLD);
    execute_hwr_viterbi_accelerator(0, n_cbps, n_traceback, n_data_bits, inMem, inData, outMem);  // only using VITERBI HW 0 for now -- still blocking, too
  } else {
    // Execute in CPU (softwware)
    printf("SCHED: executing VITERBI on CPU Software : %u < %u\n", num, VITERBI_HW_THRESHOLD);
    execute_cpu_viterbi_accelerator(n_cbps, n_traceback, n_data_bits, inMem, inData, outMem); 
  }
}




void
schedule_task(task_metadata_block_t* task_metadata_block)
{
  task_metadata_block->metadata.status = 1; // queued
  switch(task_metadata_block->metadata.job_type) {
  case fft_task:
    {
      // Scheduler should now run this either on CPU or FFT:
      task_metadata_block->metadata.status = 2; // running
      int num = (rand() % (100)); // Return a value from [0,99]
      if (num >= FFT_HW_THRESHOLD) {
	// Execute on hardware
	printf("SCHED: executing FFT on Hardware : %u > %u\n", num, FFT_HW_THRESHOLD);
	execute_hwr_fft_accelerator(0, task_metadata_block); // data);  // only using FFT HW 0 for now -- still blocking, too
      } else {
	// Execute in CPU (softwware)
	printf("SCHED: executing FFT on CPU Software : %u < %u\n", num, FFT_HW_THRESHOLD);
	execute_cpu_fft_accelerator(task_metadata_block); // data);
      }
      //}
    }
    break;
  case viterbi_task:
    {
      viterbi_data_struct_t* vdata = (viterbi_data_struct_t*)(task_metadata_block->metadata.data);
      int32_t  in_ncbps = vdata->n_cbps;
      int32_t  in_ntraceback = vdata->n_traceback;
      int32_t  in_ndata_bits = vdata->n_data_bits;
      int32_t  inMem_offset = 0;
      int32_t  inData_offset = vdata->inMem_size;
      int32_t  outMem_offset = inData_offset + vdata->inData_size;
      uint8_t* in_Mem  = &(vdata->theData[inMem_offset]);
      uint8_t* in_Data = &(vdata->theData[inData_offset]);
      uint8_t* out_Mem = &(vdata->theData[outMem_offset]);
      //extern void schedule_viterbi(int n_cbps, int n_traceback, int n_data_bits, uint8_t* inMem, uint8_t* inData, uint8_t* outMem);
      schedule_viterbi(in_ncbps, in_ntraceback, in_ndata_bits, in_Mem, in_Data, out_Mem);
    }
    break;
  default:
    printf("ERROR : schedule_task called for unknown task type: %u\n", task_metadata_block->metadata.job_type);
  }


  // For now, since this is blocking, etc. we can return the MetaData Block here
  //   In reality, this should happen when we detect a task has non-blockingly-finished...
  free_task_metadata_block(task_metadata_block);
}