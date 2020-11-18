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

uint64_t ** times; //2D array countaining counts per period
uint64_t *  count; //updated by worker threads, each has its own slot in the array to increment
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
	if (sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset)){
		printf("Failed to set affinity"); 
	}
	//End CPU pinning code
	while (flag){} //sync threads

	while (control){
		++count[index];
	}
  return NULL;
}

int main(int argc, char** argv) { // #(cpu [Total Run time{ms}] [Period{ms}] [Number of Threads])
	uint64_t total_time =  strtoull(argv[1], NULL, 10);
	uint64_t period =  strtoull(argv[2], NULL, 10);

	int thread_count =  atoi(argv[3]);

	//The thread_count + 1 is for allowing space for the steal time as the last collumn of every period
	count = (uint64_t *) calloc(thread_count + 1, sizeof(uint64_t)); //Initalize count[], giving each thread a value to increment
	times = (uint64_t **) calloc(thread_count + 1, sizeof(uint64_t *));


	for (int i = 0 ; i < thread_count + 1; ++i)
		times[i] = (uint64_t *) calloc(total_time/period, sizeof(uint64_t));

	pthread_t threads[thread_count];
	persistent_indicies = calloc(thread_count, sizeof(int));
	for (int i = 0; i < thread_count; ++i){
		persistent_indicies[i] = i;
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

	while(time_index < stop){
		nanosleep(&t, NULL);
		for (int i = 0; i < thread_count; ++i){
			times[i][time_index] = count[i];
		}

	}
	control = 0;
	for (int i = 0; i < thread_count; ++i)
		pthread_join(threads[i], NULL);

	FILE *csvData = fopen("./speed.csv", "w+");
	if (csvData == NULL){
		perror("Could not open file");
		exit(-1);
	
	}
	//Creates a csv with the period number and then a list of the iterations of each thread during that period
	uint64_t time_marker = 0;
	uint64_t current_time[thread_count]; //Elapsed time at a given period in each thread
	uint64_t temp_period_time = 0; //Temporary value for a given period

	for (int i = 0; i < thread_count + 1; ++i) //Standard array intialiation does not work with uint64_t type arrays
			current_time[i] = 0;

    /* divide the iterations by the period -> print speed not iteration count */

	for (int i = 1; i < total_time/period; ++i){
		time_marker = i;
		fprintf(csvData, "%ld,", time_marker);
		for (int j = 0; j < thread_count; ++j){
			if (i > 1){ //all periods after 1
				temp_period_time = times[j][i - 1] - current_time[j];
			}
			else {
				temp_period_time = times[j][i - 1]; //first period
			}
        	temp_period_time = temp_period_time / period; //Speed = iterations/period -> compensates for the lenght of the period
		fprintf(csvData, "%ld,", temp_period_time);
		current_time[j] = times[j][i - 1];
	}}
	
	fprintf(csvData, "\n");
	
	fclose(csvData);

	//Free memory
	for (int i = 0; i < thread_count; ++i){
		free(times[i]);
	}
	free(times);
	free(count);
	return 0;
}
