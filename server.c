#include <assert.h>
/* FreeBSD */
#define _WITH_GETLINE
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "window.h"
#include "db.h"
#include "words.h"
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>

/* the encapsulation of a client thread, i.e., the thread that handles
 * commands from clients */
typedef struct Client {
	pthread_t thread;
	window_t *win;
    int threadID;
} client_t;

/* Creates, Runs, and Eventually Destroys Client */
void* client_runner(void* c);
/* Interface with a client: get requests, carry them out and report results */
void *client_run(void *);
/* Way to destroy the client */
void client_destroy(client_t *client);
/* Interface to the db routines.  Pass a command, get a result */
int handle_command(char *, char *, int len);
/* Way to spawn more threads and such */
char menu();
/*Mutex to keep track of threads that need to be joined*/
pthread_mutex_t mutex_joinThreads;

/* Mutex and Condition Variable to deal with s and g commands */
pthread_mutex_t mutex_ClientLock;
pthread_cond_t cond_ClientWait;
char lockDownClients; //0 = not lock, 1 = lock

/* Way to show which thread IDs (basic ints) are working/need to be joined */
/* '0' = not in use, '1' = in use, '2' = need to be joined */
char* threadStatus;

/* Way to measure the service time for each thread */
unsigned int* thread_service_times;

//Arrays to hold all the time values
struct timeval* thread_start_times;
struct timeval* thread_end_times;

void* client_runner(void* c)
{   
    client_run((client_t*)c);

    gettimeofday(&(thread_end_times[ ((client_t*)c)->threadID ]), NULL);

    thread_service_times[ ((client_t*)c)->threadID ] = 
        (unsigned int)((thread_end_times[ ((client_t*)c)->threadID ].tv_sec * 1000 + 
            thread_end_times[ ((client_t*)c)->threadID ].tv_usec * 0.001) 
            - (thread_start_times[ ((client_t*)c)->threadID ].tv_sec * 1000 + 
                thread_start_times[ ((client_t*)c)->threadID ].tv_usec * 0.001));

    client_destroy((client_t*)c);
    return  0;
}

/*
 * Create an interactive client - one with its own window.  This routine
 * creates the window (which starts the xterm and a process under it.  The
 * window is labelled with the ID passsed in.  On error, a NULL pointer is
 * returned and no process started.  The client data structure returned must be
 * destroyed using client_destroy()
 */
client_t *client_create(int ID) {
    client_t *new_Client = (client_t *) malloc(sizeof(client_t));
    char title[16];

    if (!new_Client) return NULL;

    new_Client->threadID = ID;

    //Lock Mutex to enter critical section
    pthread_mutex_lock(&mutex_joinThreads);
    //Change status
    threadStatus[new_Client->threadID] = '1';
    //Exit critical section
    pthread_mutex_unlock(&mutex_joinThreads);

    sprintf(title, "Client %d", new_Client->threadID);

    /* Creates a window and set up a communication channel with it */
    if ((new_Client->win = window_create(title))) return new_Client;
    else {
	free(new_Client);
	return NULL;
    }
}

/*
 * Create a client that reads cmmands from a file and writes output to a file.
 * in and out are the filenames.  If out is NULL then /dev/stdout (the main
 * process's standard output) is used.  On error a NULL pointer is returned.
 * The returned client must be disposed of using client_destroy.
 */
client_t *client_create_no_window(char *in, char *out, int ID) {
    //fprintf(stderr, "In Here!\n");

    char *outf = (out) ? out : "/dev/stdout";

    client_t *new_Client = (client_t *) malloc(sizeof(client_t));

    //fprintf(stderr, "%c\n", in[0]);
    //fprintf(stderr, "%s\n", outf);
    new_Client->threadID = ID;

    //Lock Mutex to enter critical section
    pthread_mutex_lock(&mutex_joinThreads);
    //Change status
    threadStatus[new_Client->threadID] = '1';
    //Exit critical section
    pthread_mutex_unlock(&mutex_joinThreads);

    if (!new_Client) return NULL;
    /* Creates a window and set up a communication channel with it */
    if( (new_Client->win = nowindow_create(in, outf))) return new_Client;
    else {
	free(new_Client);
	return NULL;
    }
}

/*
 * Destroy a client created with either client_create or
 * client_create_no_window.  The cient data structure, the underlying window
 * (if any) and process (if any) are all destroyed and freed, and any open
 * files are closed.  Do not access client after calling this function.
 */
void client_destroy(client_t *client) {
	/* Remove the window */

	window_destroy(client->win);

    //Lock Mutex to enter critical section
    pthread_mutex_lock(&mutex_joinThreads);
    //Change status
    //fprintf(stderr, "\nThread %i Terminated, Need to Join\n", client->threadID);
    threadStatus[client->threadID] = '2';
    //Exit critical section
    pthread_mutex_unlock(&mutex_joinThreads);

    pthread_exit(0);
	//free(client);
}

/* Code executed by the client */
void *client_run(void *arg)
{
    pthread_mutex_lock(&mutex_ClientLock);
    if(lockDownClients == '1')
    {
        pthread_cond_wait(&cond_ClientWait,&mutex_ClientLock);
    }
    pthread_mutex_unlock(&mutex_ClientLock);
	client_t *client = (client_t *) arg;

	/* main loop of the client: fetch commands from window, interpret
	 * and handle them, return results to window. */
	char *command = 0;
	size_t clen = 0;
	/* response must be empty for the first call to serve */
	char response[256] = { 0 };

    //Start timing the execution time of the thread
    gettimeofday(&(thread_start_times[ (client)->threadID ]), NULL);

	/* Serve until the other side closes the pipe */
	while (serve(client->win, response, &command, &clen) != -1) {
        //fprintf(stderr, "Thread %i A\n", client->threadID);
        //Lock to check to see if this needs to block
        pthread_mutex_lock(&mutex_ClientLock);
        //fprintf(stderr, "Thread %i C\n", client->threadID);
        if(lockDownClients == '1')
        {
            //fprintf(stderr, "Thread %i D\n", client->threadID);
            pthread_cond_wait(&cond_ClientWait,&mutex_ClientLock);
        }

        pthread_mutex_unlock(&mutex_ClientLock);
        //fprintf(stderr, "Thread %i E\n", client->threadID);
        //fprintf(stderr, "Thread %i F\n", client->threadID);
        handle_command(command, response, sizeof(response));
        //fprintf(stderr, "Thread %i G\n", client->threadID);
	}
	return 0;
}

int handle_command(char *command, char *response, int len) {
    if (command[0] == EOF) {
	strncpy(response, "all done", len - 1);
	return 0;
    }
    interpret_command(command, response, len);
    return 1;
}

char menu()
{
    //Print out the menu and return the command that the user inputs
    fprintf(stderr, "\nList of Commands:\n");
    fprintf(stderr, "e: Create an Interctive Client in a Window\n");
    fprintf(stderr, "E: Create an non-interctive Client\n");
    fprintf(stderr, "s: Stop Processing Command from Clients\n");
    fprintf(stderr, "g: Continue Processing Command from Clients\n");
    fprintf(stderr, "w: Stop Processing Server Commands\n");
    fprintf(stderr, "\nPlease Choose a Command: ");

    int getlineCharsRead;
    size_t bufferSize = 256;
    char* myInput;

    /* These 2 lines are the heart of the program. */
    myInput = (char*) malloc (bufferSize);
    getlineCharsRead = getline (&myInput, &bufferSize, stdin);

    //fprintf(stderr, "%s", myInput );

    /*
    You are probably gonna have to put some parsing code here
    but for now, don't worry about it!
    Also, deal with the error checking here (too lazy to do it now)
    */
    //Returns the first character of the getline
    return myInput[0];
}

int main(int argc, char *argv[]) {
    //fprintf(stderr, "%i\n", getpid());
    int MAX_SIZE = 1000;
    //Allows for a max of 100 threads
    client_t** client_array = malloc(MAX_SIZE * sizeof(client_t*));
    //Array to Signify which threads what to be joined
    threadStatus = malloc(MAX_SIZE * sizeof(char));
    //Array to hold how long a thread takes to run
    thread_service_times = malloc(MAX_SIZE * sizeof(unsigned int));

    thread_start_times = malloc(MAX_SIZE * sizeof(struct timeval));
    thread_end_times = malloc(MAX_SIZE * sizeof(struct timeval));

    //Initialize the Mutex!
    pthread_mutex_init(&mutex_joinThreads,NULL);

    pthread_mutex_init(&mutex_ClientLock,NULL);
    pthread_cond_init(&cond_ClientWait,NULL);

    char* myEfileInput;
    char* myEfileOutput;

    lockDownClients = '0';

    int i;
    for(i = 0; i < MAX_SIZE; i++)
    {
        client_array[i] = 0;
        threadStatus[i] = '0';
    }

    //client_t *c = NULL;	    /* A client to serve */
    int started = 0;	    /* Number of clients started */

    if (argc != 1) {
	fprintf(stderr, "Usage: server\n");
	exit(1);
    }

    //if ((c = client_create(started++)) )  {
	//   client_run(c);
	//   client_destroy(c);
    //}

    char inputCommand = menu();

    //TO PROCESS CTRL-D
    //if(feof(stdin)) //DO SOMETHING

    //This needs to be done with ctrl-d since w continues
    while(!feof(stdin))
    {
        switch(inputCommand)
        {
            //Window Client Create
            case 'e':
                client_array[started] = client_create(started);
                if(client_array[started])
                {
                    int threadCreate = pthread_create(&(client_array[started]->thread), NULL, client_runner, (void*)client_array[started]);
                    if(threadCreate == 0)
                    {
                        fprintf(stderr, "Thread %i Created!\n", started);
                    }
                }
                started++;
            break;

            //Windowless client
            case 'E':
                fprintf(stderr, "\nPlease Enter INPUT Filename: ");

                int getlineCharsReadIn;
                size_t bufferInSize = 256;

                /* These 2 lines are the heart of the program. */
                myEfileInput = (char*) malloc (bufferInSize);
                getlineCharsReadIn = getline (&myEfileInput, &bufferInSize, stdin);


                fprintf(stderr, "Please Enter OUTPUT Filename: ");

                int getlineCharsReadOut;
                size_t bufferOutSize = 256;

                /* These 2 lines are the heart of the program. */
                myEfileOutput = (char*) malloc (bufferOutSize);
                getlineCharsReadOut = getline (&myEfileOutput, &bufferOutSize, stdin);

                //fprintf(stderr, "%i", getlineCharsReadIn);
                //fprintf(stderr, "%i", getlineCharsReadOut);
                
                myEfileInput[getlineCharsReadIn - 1] = '\0';
                myEfileOutput[getlineCharsReadOut - 1] = '\0';

                if(getlineCharsReadOut == 1)
                {
                    myEfileOutput = 0;
                }

                client_array[started] = client_create_no_window(myEfileInput,myEfileOutput,started);
                if(client_array[started])
                {
                    int threadCreate = pthread_create(&(client_array[started]->thread), NULL, client_runner, (void*)client_array[started]);
                    //fprintf(stderr, "%i\n", threadCreate);
                    if(threadCreate == 0)
                    {  
                        fprintf(stderr, "Thread %i Created! (No Window)\n", started);
                    }
                }
                else
                {
                    fprintf(stderr, "Invalid Entry, Please Try Again!\n");
                }
                started++;
            break;

            //Stop all the threads from running
            case 's':
                //Lock the mutex to prevent the clients from continuing to process
                pthread_mutex_lock(&mutex_ClientLock);
                lockDownClients = '1';
                pthread_mutex_unlock(&mutex_ClientLock);
            break;

            //Tell all the threads to start back up again
            case 'g':
                //Boreadcast condition variable to unlock clients to allow them to continue
                pthread_mutex_lock(&mutex_ClientLock);
                lockDownClients = '0';
                pthread_mutex_unlock(&mutex_ClientLock);
                pthread_cond_broadcast(&cond_ClientWait);
            break;

            //Wait for all the threads to terminate and join them
            case 'w':
                //Goes To find any Threads that need to be joined
                //Lock Mutex to enter critical section
                pthread_mutex_lock(&mutex_joinThreads);
                int j;
                for(j = 0; j < MAX_SIZE; j++)
                {
                    if(threadStatus[j] != '0')
                    {
                        //Join the thread
                        pthread_mutex_unlock(&mutex_joinThreads);
                        int endedThreadJoin = pthread_join(client_array[j]->thread,NULL);
                        free(client_array[j]);
                        if(endedThreadJoin == 0)
                        {
                            fprintf(stderr, "Thread %i Terminated and Joined! Service Time: %i milliseconds\n", j, thread_service_times[j]);
                        }
                        pthread_mutex_lock(&mutex_joinThreads);
                        threadStatus[j] = '0';
                    }
                }
                pthread_mutex_unlock(&mutex_joinThreads);
            break;

            default:
                fprintf(stderr, "Invalid Command, Please Try Again.\n");
            break;
        }

        //Lock Mutex to enter critical section
        pthread_mutex_lock(&mutex_joinThreads);

        int k;
        for(k = 0; k < MAX_SIZE; k++)
        {
            if(threadStatus[k] == '2')
            {
                //Join the thread
                fprintf(stderr, "Thread %i Terminated, Need to Join! Service Time: %i milliseconds\n", k, thread_service_times[k]);

                int threadJoin = pthread_join(client_array[k]->thread,NULL);
                free(client_array[k]);
                threadStatus[k] = '0';

                if(threadJoin == 0)
                {
                    fprintf(stderr, "Thread %i Joined!\n", k);
                }
            }
        }
        //Exit critical section
        pthread_mutex_unlock(&mutex_joinThreads);

        inputCommand = menu();
    }

    fprintf(stderr, "\nTerminating.\n");

    //Goes To find any Threads that need to be joined
    //Lock Mutex to enter critical section
    pthread_mutex_lock(&mutex_joinThreads);
    int l;
    for(l = 0; l < MAX_SIZE; l++)
    {
        if(threadStatus[l] != '0')
        {
            //Join the thread
            pthread_mutex_unlock(&mutex_joinThreads);
            int endedThreadJoin = pthread_join(client_array[l]->thread,NULL);
            free(client_array[l]);
            if(endedThreadJoin == 0)
            {
                fprintf(stderr, "Thread %i Terminated and Joined! Service Time: %i milliseconds\n", l, thread_service_times[l]);
            }
            pthread_mutex_lock(&mutex_joinThreads);
            threadStatus[l] = '0';
        }
    }
    pthread_mutex_unlock(&mutex_joinThreads);

    /* Clean up the window data */
    //window_cleanup();

    //Cleanup Routines
    //free(myEfileInput);
    //free(myEfileOutput);
    pthread_mutex_destroy(&mutex_joinThreads);
    pthread_mutex_destroy(&mutex_ClientLock);
    pthread_cond_destroy(&cond_ClientWait);
    free(client_array);
    free(threadStatus);
    free(thread_service_times);
    free(thread_start_times);
    free(thread_end_times);

    return 0;
}
