/*
 * oss.c November 21, 2020
 * Jared Diehl (jmddnb@umsystem.edu)
 */

#include <libgen.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#include "shared.h"
#include "queue.h"

#define log _log

static char *programName;

static int shmid = -1;
static int msqid = -1;
static int semid = -1;
static System *system = NULL;
static Message message;

static int activeCount = 0;
static int spawnCount = 0;
static int exitCount = 0;

static Queue *queue;
static Time nextSpawn;
static ResourceDescriptor descriptor;

void simulate();
void handleProcesses();
void trySpawnProcess();
void spawnProcess(int);
int findAvailablePID();

void semaLock(int);
void semaRelease(int);
void advanceClock();

void initDescriptor();
void printDescriptor();

void initSystem();
void initPCB(pid_t, int);

void setMatrix(Queue*, int[][RESOURCES_MAX], int[][RESOURCES_MAX], int);
void calculateNeed(int[][RESOURCES_MAX], int[][RESOURCES_MAX], int[][RESOURCES_MAX], int);
void printVector(char*, int[RESOURCES_MAX]);
void printMatrix(char*, Queue*, int[][RESOURCES_MAX], int);
bool safe(Queue*, int);

void init(int, char**);
void usage(int);
void registerSignalHandlers();
void signalHandler(int);
void timer(int);
void initIPC();
void freeIPC();
void finalize();
void error(char*, ...);
void crash(char*);
void log(char*, ...);

static bool verbose = false;
static pid_t pids[PROCESSES_MAX];

int main(int argc, char **argv) {
	init(argc, argv);

	srand(time(NULL) ^ getpid());

	bool ok = true;

	while (true) {
		int c = getopt(argc, argv, "hv");
		if (c == -1) break;
		switch (c) {
			case 'h':
				usage(EXIT_SUCCESS);
			case 'v':
				verbose = true;
				break;
			default:
				ok = false;
		}
	}

	if (optind < argc) {
		char buf[BUFFER_LENGTH];
		snprintf(buf, BUFFER_LENGTH, "found non-option(s): ");
		while (optind < argc) {
			strncat(buf, argv[optind++], BUFFER_LENGTH);
			if (optind < argc) strncat(buf, ", ", BUFFER_LENGTH);
		}
		error(buf);
		ok = false;
	}
	
	if (!ok) usage(EXIT_FAILURE);

	initIPC();

	memset(pids, 0, sizeof(pids));

	system->clock.s = 0;
	system->clock.ns = 0;
	nextSpawn.s = 0;
	nextSpawn.ns = 0;

	FILE *fp;
	if ((fp = fopen(PATH_LOG, "w")) == NULL) crash("fopen");
	if (fclose(fp) == EOF) crash("fclose");

	initSystem();

	queue = queue_create();
	initDescriptor();
	printDescriptor();

	registerSignalHandlers();

	simulate();

	finalize();

	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

void simulate() {
	while (true) {
		trySpawnProcess();
		advanceClock();
		handleProcesses();
		advanceClock();

		int status;
		pid_t pid = waitpid(-1, &status, WNOHANG);
		if (pid > 0) {
			int spid = WEXITSTATUS(status);
			pids[spid] = 0;
			activeCount--;
			exitCount++;
		}

		if (exitCount == PROCESSES_TOTAL) break;
	}
}

void handleProcesses() {
	int i, count = 0;
	QueueNode *next = queue->front;
	
	while (next != NULL) {
		advanceClock();

		int index = next->index;
		message.type = system->ptable[index].pid;
		message.spid = index;
		message.pid = system->ptable[index].pid;
		msgsnd(msqid, &message, sizeof(Message), 0);

		msgrcv(msqid, &message, sizeof(Message), 1, 0);

		advanceClock();

		if (message.terminate == 0) {
			log("%s: [%d.%d] p%d terminating\n", basename(programName), system->clock.s, system->clock.ns, message.spid);

			queue_remove(queue, index);

			next = queue->front;
			for (i = 0; i < count; i++)
				next = (next->next != NULL) ? next->next : NULL;

			continue;
		}

		if (message.request) {
			log("%s: [%d.%d] p%d requesting\n", basename(programName), system->clock.s, system->clock.ns, message.spid);

			message.type = system->ptable[index].pid;
			message.safe = safe(queue, index);
			msgsnd(msqid, &message, sizeof(Message), 0);
		}

		advanceClock();

		if (message.release) log("%s: [%d.%d] p%d releasing\n", basename(programName), system->clock.s, system->clock.ns, message.spid);
		
		count++;

		next = (next->next != NULL) ? next->next : NULL;
	}
}

void trySpawnProcess() {
	if (activeCount < PROCESSES_MAX && spawnCount < PROCESSES_TOTAL && nextSpawn.ns >= (rand() % (500 + 1)) * 1000000) {
		nextSpawn.ns = 0;
		int spid = findAvailablePID();
		if (spid >= 0) spawnProcess(spid);
	}
}

void spawnProcess(int spid) {
	pid_t pid = fork();
	pids[spid] = pid;

	if (pid == -1) crash("fork");
	else if (pid == 0) {
		char arg[BUFFER_LENGTH];
		snprintf(arg, BUFFER_LENGTH, "%d", spid);
		execl("./user", "user", arg, (char*) NULL);
		crash("execl");
	}

	initPCB(pid, spid);
	queue_push(queue, spid);
	activeCount++;
	spawnCount++;
	log("%s: [%d.%d] p%d created\n", basename(programName), system->clock.s, system->clock.ns, spid);
}

void registerSignalHandlers() {
	timer(TIMEOUT);

	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = &signalHandler;
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGALRM, &sa, NULL) == -1) crash("sigaction");

	sigemptyset(&sa.sa_mask);
	sa.sa_handler = &signalHandler;
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGINT, &sa, NULL) == -1) crash("sigaction");

	signal(SIGUSR1, SIG_IGN);
}

void signalHandler(int sig) {
	finalize();
	exit(EXIT_SUCCESS);
}

void finalize() {
	fprintf(stderr, "\n\n");
	fprintf(stderr, "System time: %d.%d\n", system->clock.s, system->clock.ns);
	fprintf(stderr, "Total processes executed: %d\n", spawnCount);

	freeIPC();

	kill(0, SIGUSR1);
	while (waitpid(-1, NULL, WNOHANG) >= 0);
}

void semaLock(int index) {
	struct sembuf sop = { index, -1, 0 };
	semop(semid, &sop, 1);
}

void semaRelease(const int index) {
	struct sembuf sop = { index, 1, 0 };
	semop(semid, &sop, 1);
}

void advanceClock() {
	semaLock(0);

	int rns = rand() % (1 * 1000000) + 1;
	nextSpawn.ns += rns;
	system->clock.ns += rns;

	while (system->clock.ns >= (1000 * 1000000)) {
		system->clock.s++;
		system->clock.ns -= (1000 * 1000000);
	}

	semaRelease(0);
}

void initDescriptor() {
	int i;
	for (i = 0; i < RESOURCES_MAX; i++)
		descriptor.resource[i] = rand() % 10 + 1;

	descriptor.shared = (SHARED_RESOURCES_MAX == 0) ? 0 : rand() % (SHARED_RESOURCES_MAX - (SHARED_RESOURCES_MAX - SHARED_RESOURCES_MIN)) + SHARED_RESOURCES_MIN;
}

void printDescriptor() {
	log("Total Resource\n<");

	int i;
	for (i = 0; i < RESOURCES_MAX; i++) {
		log("%2d", descriptor.resource[i]);
		if (i < RESOURCES_MAX - 1) log(" | ");
	}
	log(">\n\n");

	log("Shareable resources: %d\n", descriptor.shared);
}

void initSystem() {
	int i;
	for (i = 0; i < PROCESSES_MAX; i++) {
		system->ptable[i].spid = -1;
		system->ptable[i].pid = -1;
	}
}

void initPCB(pid_t pid, int spid) {
	PCB *pcb = &system->ptable[spid];

	pcb->pid = pid;
	pcb->spid = spid;

	int i;
	for (i = 0; i < RESOURCES_MAX; i++) {
		pcb->maximum[i] = rand() % (descriptor.resource[i] + 1);
		pcb->allocation[i] = 0;
		pcb->request[i] = 0;
		pcb->release[i] = 0;
	}
}

void setMatrix(Queue *queue, int max[][RESOURCES_MAX], int alloc[][RESOURCES_MAX], int count) {
	QueueNode next;
	next.next = queue->front;

	int i, j;
	int index = next.next->index;
	for (i = 0; i < count; i++) {
		for (j = 0; j < RESOURCES_MAX; j++) {
			max[i][j] = system->ptable[index].maximum[j];
			alloc[i][j] = system->ptable[index].allocation[j];
		}

		if (next.next->next != NULL) {
			next.next = next.next->next;
			index = next.next->index;
		} else next.next = NULL;
	}
}

void calculateNeed(int need[][RESOURCES_MAX], int max[][RESOURCES_MAX], int alloc[][RESOURCES_MAX], int count) {
	int i, j;
	for (i = 0; i < count; i++)
		for (j = 0; j < RESOURCES_MAX; j++)
			need[i][j] = max[i][j] - alloc[i][j];
}

void printVector(char *title, int vector[RESOURCES_MAX]) {
	log("%s Resource\n     ", title);

	int i;
	for (i = 0; i < RESOURCES_MAX; i++) {
		log("%2d", vector[i]);
		if (i < RESOURCES_MAX - 1) log(" | ");
	}
	log("\n");
}

void printMatrix(char *title, Queue *queue, int matrix[][RESOURCES_MAX], int count) {
	QueueNode *next;
	next = queue->front;

	int i, j;
	log("%s Matrix\n", title);

	for (i = 0; i < count; i++) {
		log("P%2d :", next->index);
		for (j = 0; j < RESOURCES_MAX; j++) {
			log("%2d", matrix[i][j]);
			if (j < RESOURCES_MAX - 1) log(" | ");
		}
		log("\n");

		next = (next->next != NULL) ? next->next : NULL;
	}
}

bool safe(Queue *queue, int index) {
	int i, p, j, k;

	QueueNode *next;
	next = queue->front;
	if (next == NULL) return true;

	int count = queue_size(queue);
	int max[count][RESOURCES_MAX];
	int alloc[count][RESOURCES_MAX];
	int req[RESOURCES_MAX];
	int need[count][RESOURCES_MAX];
	int avail[RESOURCES_MAX];

	setMatrix(queue, max, alloc, count);
	calculateNeed(need, max, alloc, count);

	for (i = 0; i < RESOURCES_MAX; i++) {
		avail[i] = descriptor.resource[i];
		req[i] = system->ptable[index].request[i];
	}

	for (i = 0; i < count; i++)
		for (j = 0; j < RESOURCES_MAX; j++)
			avail[j] = avail[j] - alloc[i][j];

	int idx = 0;
	next = queue->front;
	while (next != NULL) {
		if (next->index == index) break;
		idx++;
		next = (next->next != NULL) ? next->next : NULL;
	}

	if (verbose) {
		printMatrix("Maximum", queue, max, count);
		printMatrix("Allocation", queue, alloc, count);
		char buf[BUFFER_LENGTH];
		sprintf(buf, "P%2d Request", index);
		printVector(buf, req);
	}

	bool finish[count];
	int ss[count];
	memset(finish, 0, count * sizeof(finish[0]));

	int work[RESOURCES_MAX];
	for (i = 0; i < RESOURCES_MAX; i++)
		work[i] = avail[i];

	for (j = 0; j < RESOURCES_MAX; j++) {
		if (need[idx][j] < req[j] && j < descriptor.shared) {
			log("\tAsked for more than initial max request\n");

			if (verbose) {
				printVector("Available", avail);
				printMatrix("Need", queue, need, count);
			}

			return false;
		}

		if (req[j] <= avail[j] && j < descriptor.shared) {
			avail[j] -= req[j];
			alloc[idx][j] += req[j];
			need[idx][j] -= req[j];
		} else {
			log("\tNot enough available resources\n");

			if (verbose) {
				printVector("Available", avail);
				printMatrix("Need", queue, need, count);
			}

			return false;
		}
	}

	i = 0;
	while (index < count) {
		bool found = false;
		for (p = 0; p < count; p++) {
			if (finish[p] == 0) {
				for (j = 0; j < RESOURCES_MAX; j++)
					if (need[p][j] > work[j] && descriptor.shared) break;

				if (j == RESOURCES_MAX) {
					for (k = 0; k < RESOURCES_MAX; k++)
						work[k] += alloc[p][k];

					ss[i++] = p;
					finish[p] = 1;
					found = true;
				}
			}
		}

		if (found == false) {
			log("System is in UNSAFE (not safe) state\n");
			return false;
		}
	}

	if (verbose) {
		printVector("Available", avail);
		printMatrix("Need", queue, need, count);
	}

	int sequence[count];
	int seq_index = 0;
	next = queue->front;
	while (next != NULL) {
		sequence[seq_index++] = next->index;
		next = (next->next != NULL) ? next->next : NULL;
	}

	log("System is in SAFE state. Safe sequence is: ");
	for (i = 0; i < count; i++)
		log("%2d ", sequence[ss[i]]);
	log("\n\n");

	return true;
}

void initIPC() {
	key_t key;

	if ((key = ftok(".", 0)) == -1) crash("ftok");
	if ((shmid = shmget(key, sizeof(System), IPC_EXCL | IPC_CREAT | PERMS)) == -1) crash("shmget");
	if ((system = (System*) shmat(shmid, NULL, 0)) == (void*) -1) crash("shmat");

	if ((key = ftok(".", 1)) == -1) crash("ftok");
	if ((msqid = msgget(key, IPC_EXCL | IPC_CREAT | PERMS)) == -1) crash("msgget");

	if ((key = ftok(".", 2)) == -1) crash("ftok");
	if ((semid = semget(key, 1, IPC_EXCL | IPC_CREAT | PERMS)) == -1) crash("semget");
	if (semctl(semid, 0, SETVAL, 1) == -1) crash("semctl");
}

void freeIPC() {
	if (system != NULL && shmdt(system) == -1) crash("shmdt");
	if (shmid > 0 && shmctl(shmid, IPC_RMID, NULL) == -1) crash("shmdt");

	if (msqid > 0 && msgctl(msqid, IPC_RMID, NULL) == -1) crash("msgctl");

	if (semid > 0 && semctl(semid, 0, IPC_RMID) == -1) crash("semctl");
}

void crash(char *msg) {
	char buf[BUFFER_LENGTH];
	snprintf(buf, BUFFER_LENGTH, "%s: %s", programName, msg);
	perror(buf);
	
	freeIPC();
	
	exit(EXIT_FAILURE);
}

void error(char *fmt, ...) {
	char buf[BUFFER_LENGTH];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, BUFFER_LENGTH, fmt, args);
	va_end(args);
	
	fprintf(stderr, "%s: %s\n", programName, buf);
	
	freeIPC();
}

void usage(int status) {
	if (status != EXIT_SUCCESS) fprintf(stderr, "Try '%s -h' for more information\n", programName);
	else {
		printf("Usage: %s [-v]\n", programName);
		printf("    -v : verbose on\n");
	}
	exit(status);
}

void init(int argc, char **argv) {
	programName = argv[0];

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
}

void log(char *fmt, ...) {
	FILE *fp = fopen(PATH_LOG, "a+");
	if(fp == NULL) crash("fopen");

	char buf[BUFFER_LENGTH];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, BUFFER_LENGTH, fmt, args);
	va_end(args);

	fprintf(stderr, buf);
	fprintf(fp, buf);

	if (fclose(fp) == EOF) crash("fclose");
}

int findAvailablePID() {
	int i;
	for (i = 0; i < PROCESSES_MAX; i++)
		if (pids[i] == 0) return i;
	return -1;
}

void timer(int duration) {
	struct itimerval val;
	val.it_value.tv_sec = duration;
	val.it_value.tv_usec = 0;
	val.it_interval.tv_sec = 0;
	val.it_interval.tv_usec = 0;
	if (setitimer(ITIMER_REAL, &val, NULL) == -1) crash("setitimer");
}