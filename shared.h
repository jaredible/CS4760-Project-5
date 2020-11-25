/*
 * shared.h November 21, 2020
 * Jared Diehl (jmddnb@umsystem.edu)
 */

#ifndef SHARED_H
#define SHARED_H

#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>

#define system _system

#define BUFFER_LENGTH 1024

#define KEY_PATHNAME "."
#define KEY_ID_SYSTEM 0
#define KEY_ID_MESSAGE_QUEUE 1
#define KEY_ID_SEMAPHORE 2
#define PERMS (S_IRUSR | S_IWUSR)

#define PATH_LOG "output.log"
#define TIMEOUT 5
#define PROCESSES_MAX 18
#define PROCESSES_TOTAL 40
#define RESOURCES_MAX 20
#define SHARED_RESOURCES_MIN (int) (RESOURCES_MAX * 0.15)
#define SHARED_RESOURCES_MAX (int) (RESOURCES_MAX * 0.25)

typedef struct {
	unsigned int s;
	unsigned int ns;
} Time;

typedef struct {
	long type;
	pid_t pid;
	int spid;
	bool terminate;
	bool request;
	bool release;
	bool safe;
	char text[BUFFER_LENGTH];
} Message;

typedef struct {
	int resource[RESOURCES_MAX];
	int shared;
} ResourceDescriptor;

typedef struct {
	pid_t pid;
	int spid;
	int maximum[RESOURCES_MAX];
	int allocation[RESOURCES_MAX];
	int request[RESOURCES_MAX];
	int release[RESOURCES_MAX];
} PCB;

typedef struct {
	Time clock;
	PCB ptable[PROCESSES_MAX];
} System;

void printDebug(System *system) {
	int i, j;
	printf("Debug\n");
	for (i = 0; i < PROCESSES_MAX; i++) {
		printf("p%-2d ", i);
		for (j = 0; j < RESOURCES_MAX; j++) {
			printf("%d ", system->ptable[i].allocation[j]);
		}
		printf("\n");
	}
}

#endif
