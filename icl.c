/**
 * SPDX-License-Identifier: MIT
 *
 * It is used to measure inter-core one-way data latency.
 *
 * Build:
 * gcc -O3 -DNDEBUG icl.c -o icl -pthread
 *
 * Plot results using gnuplot:
 * $ ./icl -p | gnuplot
 */

#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <stdatomic.h>
#include <stdalign.h>

/**
 * Structure to hold arguments for the thread function.
 */
typedef struct {
	int cpu;
	int nsamples;
	int use_write;
	int preheat;
	atomic_int *seq1;
	atomic_int *seq2;
} thread_args_t;

/**
 * Pins the current thread to a specific CPU core.
 */
void pinThread(int cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) == -1) {
		perror("sched_setaffinity");
		exit(1);
	}
}

/**
 * The function executed by the spawned thread.
 */
void *thread_func(void *arg)
{
	thread_args_t *args = (thread_args_t *) arg;
	pinThread(args->cpu);

	if (args->preheat) {
		struct timespec start, now;
		clock_gettime(CLOCK_MONOTONIC, &start);
		while (1) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			if ((now.tv_sec - start.tv_sec) * 1e9 +
			    (now.tv_nsec - start.tv_nsec) >= 200000000) {
				break;
			}
		}
	}

	for (int m = 0; m < args->nsamples; ++m) {
		if (!args->use_write) {
			for (int n = 0; n < 100; ++n) {
				while (atomic_load_explicit
				       (args->seq1,
					memory_order_acquire) != n) ;
				atomic_store_explicit(args->seq2, n,
						      memory_order_release);
			}
		} else {
			while (atomic_load_explicit
			       (args->seq2, memory_order_acquire) != 0) ;
			atomic_store_explicit(args->seq2, 1,
					      memory_order_release);
			for (int n = 0; n < 100; ++n) {
				int cmp;
				do {
					cmp = 2 * n;
				} while (!atomic_compare_exchange_strong
					 (args->seq1, &cmp, cmp + 1));
			}
		}
	}
	return NULL;
}

int main(int argc, char *argv[])
{

	int nsamples = 1000;
	int plot = 0;
	int smt = 0;
	int use_write = 0;
	int preheat = 0;
	const char *name = NULL;

	int opt;
	while ((opt = getopt(argc, argv, "Hn:ps:tw")) != -1) {
		switch (opt) {
		case 'H':
			preheat = 1;
			break;
		case 'n':
			name = optarg;
			break;
		case 'p':
			plot = 1;
			break;
		case 's':
			nsamples = atoi(optarg);
			break;
		case 't':
			smt = 1;
			break;
		case 'w':
			use_write = 1;
			break;
		default:
			goto usage;
		}
	}

	if (optind != argc) {
usage:
		fprintf(stderr,
			"usage: icl [-Hptw] [-n name] [-s number_of_samples]\n");
		fprintf(stderr,
			"Use -t to interleave hardware threads with cores.\n");
		fprintf(stderr,
			"The name passed using -n appears in the graph's title.\n");
		fprintf(stderr,
			"Use write cycles instead of read cycles with -w.\n");
		fprintf(stderr,
			"Use -H to preheat each core for 200ms before measuring.\n");
		fprintf(stderr, "\nPlot results using gnuplot:\n");
		fprintf(stderr, "icl -p | gnuplot -p\n");
		exit(1);
	}

	cpu_set_t set;
	CPU_ZERO(&set);
	if (sched_getaffinity(0, sizeof(set), &set) == -1) {
		perror("sched_getaffinity");
		exit(1);
	}

	/* enumerate available CPUs */
	int *cpus = NULL;
	int ncpus = 0;
	for (int i = 0; i < CPU_SETSIZE; ++i) {
		if (CPU_ISSET(i, &set)) {
			ncpus++;
			cpus = realloc(cpus, ncpus * sizeof(int));
			cpus[ncpus - 1] = i;
		}
	}

	long long *data = calloc(ncpus * ncpus, sizeof(long long));

	for (int i = 0; i < ncpus; ++i) {
		for (int j = i + 1; j < ncpus; ++j) {

			alignas(64) atomic_int seq1 = -1;
			alignas(64) atomic_int seq2 = -1;

			pthread_t thread;
			thread_args_t args =
			    { cpus[i], nsamples, use_write, preheat, &seq1,
		   &seq2 };
			pthread_create(&thread, NULL, thread_func, &args);

			struct timespec ts1, ts2;
			long long rtt = -1;

			pinThread(cpus[j]);
			if (preheat) {
				struct timespec start, now;
				clock_gettime(CLOCK_MONOTONIC, &start);
				while (1) {
					clock_gettime(CLOCK_MONOTONIC, &now);
					if ((now.tv_sec - start.tv_sec) * 1e9 +
					    (now.tv_nsec - start.tv_nsec) >=
					    200000000) {
						break;
					}
				}
			}

			for (int m = 0; m < nsamples; ++m) {
				atomic_store(&seq1, -1);
				atomic_store(&seq2, -1);
				if (!use_write) {
					clock_gettime(CLOCK_MONOTONIC, &ts1);
					for (int n = 0; n < 100; ++n) {
						atomic_store_explicit(&seq1, n,
								      memory_order_release);
						while (atomic_load_explicit
						       (&seq2,
							memory_order_acquire) !=
						       n) ;
					}
					clock_gettime(CLOCK_MONOTONIC, &ts2);
					long long current_rtt =
					    (ts2.tv_sec - ts1.tv_sec) * 1e9 +
					    (ts2.tv_nsec - ts1.tv_nsec);
					if (rtt == -1 || current_rtt < rtt) {
						rtt = current_rtt;
					}
				} else {
					/* wait for the other thread to be ready */
					atomic_store_explicit(&seq2, 0,
							      memory_order_release);
					while (atomic_load_explicit
					       (&seq2,
						memory_order_acquire) == 0) ;
					atomic_store_explicit(&seq2, -1,
							      memory_order_release);
					clock_gettime(CLOCK_MONOTONIC, &ts1);
					for (int n = 0; n < 100; ++n) {
						int cmp;
						do {
							cmp = 2 * n - 1;
						} while
						    (!atomic_compare_exchange_strong
						     (&seq1, &cmp, cmp + 1));
					}
					/* wait for the other thread to see the last value */
					while (atomic_load_explicit
					       (&seq1,
						memory_order_acquire) != 199) ;
					clock_gettime(CLOCK_MONOTONIC, &ts2);
					long long current_rtt =
					    (ts2.tv_sec - ts1.tv_sec) * 1e9 +
					    (ts2.tv_nsec - ts1.tv_nsec);
					if (rtt == -1 || current_rtt < rtt) {
						rtt = current_rtt;
					}
				}
			}

			pthread_join(thread, NULL);

			data[i * ncpus + j] = rtt / 2 / 100;
			data[j * ncpus + i] = rtt / 2 / 100;
		}
	}

	if (plot) {
		printf("set terminal pngcairo size 800,600 enhanced font \"Verdana,10\"\n");
		printf
		    ("set title \"%s%sInter-core one-way %s latency between CPU cores\"\n",
		     (name ? name : ""), (name ? " : " : ""),
		     (use_write ? "write" : "data"));
		printf("set xlabel \"CPU\"\n");
		printf("set ylabel \"CPU\"\n");
		printf("set cblabel \"Latency (ns)\"\n");
		printf("set output 'heatmap.png'\n");
		printf("$data << EOD\n");
	}

	printf("%4s", "CPU");
	for (int i = 0; i < ncpus; ++i) {
		int c0 = smt ? (i >> 1) + (i & 1) * ncpus / 2 : i;
		printf(" %4d", cpus[c0]);
	}
	printf("\n");
	for (int i = 0; i < ncpus; ++i) {
		int c0 = smt ? (i >> 1) + (i & 1) * ncpus / 2 : i;
		printf("%4d", cpus[c0]);
		for (int j = 0; j < ncpus; ++j) {
			int c1 = smt ? (j >> 1) + (j & 1) * ncpus / 2 : j;
			int idx0 = -1, idx1 = -1;
			for (int k = 0; k < ncpus; ++k) {
				if (cpus[k] == cpus[c0])
					idx0 = k;
				if (cpus[k] == cpus[c1])
					idx1 = k;
			}
			printf(" %4lld", data[idx0 * ncpus + idx1]);
		}
		printf("\n");
	}

	if (plot) {
		printf("EOD\n");
		printf("set palette defined (0 '#80e0e0', 1 '#54e0eb', "
		       "2 '#34d4f3', 3 '#26baf9', 4 '#40a0ff', 5 '#5888e7', "
		       "6 '#6e72d1', 7 '#845cbb', 8 '#9848a7', 9 '#ac3493', "
		       "10 '#c0207f', 11 '#d20e6d', 12 '#e60059', 13 '#f80047', "
		       "14 '#ff0035', 15 '#ff0625', 16 '#ff2113', 17 '#ff3903', "
		       "18 '#ff5400', 19 '#ff6c00', 20 '#ff8400', 21 '#ff9c00', "
		       "22 '#ffb400', 23 '#ffcc00', 24 '#ffe400', 25 '#fffc00')\n");
		printf("#set tics font \",7\"\n");
		printf
		    ("plot '$data' matrix rowheaders columnheaders using 2:1:3 "
		     "notitle with image, "
		     "'$data' matrix rowheaders columnheaders using "
		     "2:1:(sprintf(\"%%g\",$3)) notitle with labels #font \",5\"\n");
	}

	free(cpus);
	free(data);

	return 0;
}
