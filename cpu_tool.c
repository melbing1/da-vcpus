#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE

#include <features.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <inttypes.h>
#include <sched.h>
#include <sys/syscall.h>

int control = 1;
int flag = 1;

uint64_t ** times; // 2D array containing counts per period
uint64_t *  count; // updated by worker threads, each has its own slot in the array to increment
uint64_t time_index = 0;

int * persistent_indicies;

void* worker(void *thread_number){
	int index = *(int *) thread_number;

	/* This program can be configured to run with or without the vCPUs pinned to physical cores (the pinning is sequential) */
	/* To disable core pinning comment out the code between the comments below. */

	//Begin CPU pinning code (Optional)
	cpu_set_t cpuset; 
	CPU_ZERO(&cpuset);
	CPU_SET(index, &cpuset);
	pid_t tid = syscall(SYS_gettid);

	// Pin each core sequentially starting with vCPU 0
	if (sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset)){ printf("Failed to set affinity");}
	//End CPU pinning code (Optional)
	
	while (flag){} // Flag ensures that threads remain in sync by ensuring all workers begin their execution simultaneously
	while (control){ ++count[index]; } //Increment this workers cycle count
  return NULL;
}

/* # cpu [Total Run time{ms}] [Period{ms}] [Number of Threads] */
int main(int argc, char** argv) { 
	uint64_t total_time =  strtoull(argv[1], NULL, 10); // How many periods? (argv 1)
	uint64_t period =  strtoull(argv[2], NULL, 10);     // Length of a single period (argv 2)
	int thread_count =  atoi(argv[3]);									// How many threads should be used? (argv 3)

	// Allocate space storing measurements during execution
	// NOTE: The thread_count + 1 is for allowing space for the steal time as the last collumn of every period
	count = (uint64_t *) calloc(thread_count + 1, sizeof(uint64_t)); //Initalize count[], giving each thread a value to increment
	times = (uint64_t **) calloc(thread_count + 1, sizeof(uint64_t *));

	/* Create the user supplied number of threads and pre-allocate space for each entry */
	for (int i = 0 ; i < thread_count + 1; ++i)
		times[i] = (uint64_t *) calloc(total_time/period, sizeof(uint64_t));

	pthread_t threads[thread_count]; // Array of pthreads -> one per CPU in bounds of 'thread_count'
	persistent_indicies = calloc(thread_count, sizeof(int)); // Temp indices are required to index within the loop 
	for (int i = 0; i < thread_count; ++i){
		persistent_indicies[i] = i;

		// Try to create a start a pthread (it can not actually run until the 'flag' variable is flipped)
		if (pthread_create(&threads[i], NULL, worker, &persistent_indicies[i]))
			printf("Failed to create thread: %d", i);
	}
	flag = 0; //start threads

	int seconds = (int) period / 1000;
	int ms = (int) period % 1000;
	struct timespec t;

	t.tv_sec = seconds;
	t.tv_nsec = ms * 1000000l;

	uint64_t stop = total_time / period;

	uint64_t current_period_steal_time = 0;
	uint64_t last_period_steal_time = 0;
	
	FILE * proc_stat;
	size_t buffer_sz;
	char * buffer;
	
	int space_count = 0;
	char steal_time[50];
	int steal_time_index = 0;
	int store_init_val = 1;

	proc_stat = fopen("/proc/stat", "r");

	while(time_index < stop){
		nanosleep(&t, NULL); // Always begin by sleeping 'period' ms
		for (int i = 0; i < thread_count; ++i){
			times[i][time_index] = count[i]; //Collect cycle counts from each of the CPU arrays
		}
		
		// Ensure that the proc_stat file exists, if not than write -1's into the array slots instead
		if (proc_stat == NULL){
			times[thread_count][time_index] = -1;
		} else { 
		getline(&buffer, &buffer_sz, proc_stat); // Parse steal time from /proc/stat file by space_count
		for (int q = 0; q < buffer_sz; q++){
			if (buffer[q] == ' '){
				if (space_count == 8){
					// Save this as the starting steal since the number is a running total
					// Read this number until the next space
					do {
						steal_time[steal_time_index] = buffer[q + steal_time_index];
						++steal_time_index;
					} while (buffer[q + steal_time_index] != ' '); // Read until a space
						steal_time_index = 0;
						break;
					}
					++space_count;
				}
			}
		}
		// Calculate steal time percentage
		current_period_steal_time = strtoull(steal_time, NULL, 10);
		if (store_init_val){
			 times[thread_count][time_index] = -1; // Set to -1 because we don't have data in the first loop to compare against.
			 store_init_val = 0;
			 last_period_steal_time = current_period_steal_time;
		} else {
			times[thread_count][time_index] = current_period_steal_time - last_period_steal_time;
			last_period_steal_time = current_period_steal_time;
		} ++time_index;

		for (int l = 0; l < 50; l++){
			steal_time[l] = 0;
		}

		// Cleanup
		rewind(proc_stat);
		space_count = 0;
		fflush(proc_stat);
	}
	fclose(proc_stat);
	
	// Stop all threads execution and join back with main thread
	control = 0;
	for (int i = 0; i < thread_count; ++i)
		pthread_join(threads[i], NULL);

	// Dump resulting steal time array into a file
	FILE *csvData = fopen("./speed.csv", "w+");
	if (csvData == NULL){
		perror("Could not open file");
		exit(-1);
	}
	// Creates a CSV with the period number and then a list of the iterations of each thread during that period
	uint64_t time_marker = 0;
	uint64_t current_time[thread_count]; // Elapsed time at a given period in each thread
	uint64_t temp_period_time = 0; // Temporary value for a given period
	uint64_t temp_steal_time = 0;

	for (int i = 0; i < thread_count + 1; ++i) // Standard array initialization does not work with uint64_t type arrays
			current_time[i] = 0;

    /* Divide the iterations by the period -> print speed not iteration count */
    /* Print steal time as percentage */

	for (int i = 1; i <= total_time/period; ++i){
		time_marker = i;
		fprintf(csvData, "%ld,", time_marker);
		for (int j = 0; j < thread_count + 1; ++j){
			if (i > 1){ // All periods after 1
				if (j == thread_count){
					temp_steal_time = times[j][i - 1];
				}
				else {
					temp_period_time = times[j][i - 1] - current_time[j];
				}
			}
			else {
				if (j == thread_count){
					temp_steal_time = times[j][i - 1];
				}
				else {
					temp_period_time = times[j][i - 1]; // First period
				}
			}
			if (j == thread_count) {  // Print steal time
				if (temp_steal_time == -1){}
				else {
        	uint64_t period_in_centiseconds = period * 10; // Steal time is measured in centiseconds
					if (temp_steal_time > 0) temp_steal_time = period_in_centiseconds / temp_steal_time;
					if (temp_steal_time > 0) temp_steal_time = 100 / temp_steal_time; // Convert decimal to a percentage
				}
				fprintf(csvData, "%ld", temp_steal_time);
			}
			else { //print CPU period
        temp_period_time = temp_period_time / period; // Speed = iterations/period -> compensates for the length of the period
				fprintf(csvData, "%ld,", temp_period_time);
				current_time[j] = times[j][i - 1];
			}
		}

		fprintf(csvData, "\n");
	}

	fclose(csvData);

	// Free memory
	for (int i = 0; i < thread_count; ++i){
		free(times[i]);
	}
	free(times);
	free(count);
	return 0;
}
