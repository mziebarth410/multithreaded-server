#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "util.h"

#define MAX_THREADS 100
#define MAX_QUEUE_SIZE 100
#define MAX_REQUEST_LENGTH 1024
#define MAX_TYPE_LENGTH 10

//global variables for
int port;
char* path;
int num_dispatcher;
int num_workers;
int queue_length;

// mutex vars
int kill_child = 0;
pthread_mutex_t die_lock;
pthread_mutex_t id_lock;

//Ids for current worker and thread
int cur_worker = 0;
int t_id = -1;

//pthread variables for dispatch and worker
pthread_t* disp_thds;
pthread_t* worker_thds;

//LOG variables
char* file_log = "web_server_log";
FILE *fd_log;
pthread_mutex_t f_lock;

//Given Structure for a single request.
typedef struct request
{
    int m_socket;
    char m_szRequest[MAX_REQUEST_LENGTH];
} request_t;

//request buffer
request_t buf[MAX_QUEUE_SIZE];

//thread vars for buffer
pthread_cond_t free_slot;
pthread_mutex_t buffer_access;
pthread_cond_t buffer_content;
int buf_size = 0;
int current_index = 0;
int buf_cur_out = 0;

//initialize log file
//set up so the file can be used by all threads
void init_log()
{
    //open file
    fd_log = fopen(file_log, "aw+");
    if (fd_log == NULL)
    {
        perror("File could not be created");
        exit(EXIT_FAILURE);
    }
}

// This will write a string to the log file based on the arguments taken in
void log_write(int threadID, int reqNum, int fd, char* req_string, long bytes, char* err_msg)
{
    int a;
    int LOG_BUF_SIZE = (MAX_REQUEST_LENGTH * sizeof(char)) + (10* sizeof(char));
    
    if(fd_log > 0)
    {
        char *message = (char*)malloc(LOG_BUF_SIZE);
        if(bytes == -1)
        {
            sprintf(message, "[%d][%d][%d][%s][%s]\n", threadID, reqNum, fd, req_string, err_msg);
        }
        else
        {
            sprintf(message, "[%d][%d][%d][%s][%li]\n", threadID, reqNum, fd, req_string, bytes);
        }
        pthread_mutex_lock(&f_lock);
        printf("%s", message);
        
        a = fputs(message, fd_log);
        fflush(fd_log);
        
        pthread_mutex_unlock(&f_lock);
        free(message);
        
        if (a < 0)
        {
            perror("Writing to file was unsuccessful");
        }
    }
    else
    {
        fprintf(stderr, "File Not Found\n");
    }
}

//argument is a path to whatever file
//this function decides what content type the file is
char* get_file_type(char* file)
{
    char* extension = strrchr(file, '.');
    char* type = (char*)malloc(MAX_TYPE_LENGTH * sizeof(char));
    
    if(extension!= NULL)
    {
        if (!strncmp(extension+1, "html",4) || !strncmp(extension + 1, "htm",4))
        {
            strcpy(type, "text/html");
        }
        else if(!strncmp(extension+1, "gif", 3))
        {
            strcpy(type,  "img/gif");
        }
        else if(!strncmp(extension+1,"jpg",3))
        {
            strcpy(type, "image/jpeg");
        }
        else
        {
            strcpy(type, "text/plain");
        }
        return type;
    }
    else
    {
        printf("Could not get extension\n");
        return NULL;
    }
}

//calculates the file size
//finds difference between the last point and first point of the file
long get_size(char* file)
{
    FILE* f = fopen(file, "r");
    
    if(f == NULL)
    {
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    return size;
}

//opens file and inserts it into buffer
int load_file(char* file, char* buf, long len)//loading file to buffer
{
    FILE* fp = fopen(file, "r");
    
    if (fp != NULL)
    {
        //close file
        if (fread(buf, len, 1, fp) < 0)
        {
            fclose(fp);
            return -1;
        }
        else
        {
            fclose(fp);
            return 0;
        }
    }
    else
    {
        return -1;
    }
}

//for successful termination, the file must be closed.
//This seems trivial but the error check is important
//and this is set up to work with the sig handler
void handle_termination(int sig)
{
    if (fd_log)
    {
        fclose(fd_log);
    }
    exit(EXIT_SUCCESS);
}

//This sets up a sig handler to take care of sudden termination
//when there is a sudden termination, this signal calls the
//handle termination function closes the file
void sig_handler()
{
    struct sigaction act;
    act.sa_handler = handle_termination;
    sigfillset(&act.sa_mask);
    
    if (sigaction(SIGINT, &act, NULL)< 0)
    {
        perror("Sig Setup unsuccessful\n");
        exit(EXIT_FAILURE);
    }
}


//remove request into global buffer
//basically the opposite of insert_to_buf
//a crucial part of this function is to unlock before and lock after the buffer access
//since this subtracts to the buffer,the current index is decremented
request_t remove_from_buf()
{
    request_t r;
    pthread_mutex_lock(&buffer_access);
    
    while (buf_size == 0)
    {
        pthread_cond_wait(&buffer_content, &buffer_access);
    }
    
    r = buf[buf_cur_out];
    buf_cur_out = (buf_cur_out +1) % MAX_QUEUE_SIZE;
    buf_size--;
    
    pthread_cond_signal(&free_slot);
    pthread_mutex_unlock(&buffer_access);
    
    return r;
}

//insert request into global buffer
//a crucial part of this function is to unlock before and lock after the buffer access
//since this adds to the buffer,the current index is incremented
void insert_to_buf(int socket, char* r)
{
    request_t new_r;
    new_r.m_socket = socket;
    
    strcpy(new_r.m_szRequest, r);
    pthread_mutex_lock(&buffer_access);
    
    while (buf_size == MAX_QUEUE_SIZE)
    {
        pthread_cond_wait(&free_slot, &buffer_access);
    }
    
    buf[current_index] = new_r;
    current_index = (current_index + 1) % MAX_QUEUE_SIZE;
    buf_size++;
    
    pthread_cond_signal(&buffer_content);
    //release lock on buffer access
    pthread_mutex_unlock(&buffer_access);
}


//This accepts connections and gets requests and puts those requests
//in the buffer
//There are also error checks
void * dispatch(void * arg)
{
    int fd_current;
    int result;
    
    while (!kill_child)
    {
        fd_current = accept_connection();
        if (fd_current < 0)
        {
            fprintf(stderr, "Accept_connection() error\n");
        }
        else
        {
            char names[MAX_REQUEST_LENGTH];
            result = get_request(fd_current, names);
            
            if (result < 0)
            {
                fprintf(stderr, "request error\n");
                close(fd_current);
            }
            else
            {
                insert_to_buf(fd_current, names);
            }
        }
    
    }
    
}

//This gets a new worker id using a global counter
//A crucial part of this function is locking and unlocking the
//id threads to
int get_t_id()
{
    pthread_mutex_lock(&id_lock);
    int id = cur_worker;
    cur_worker++;
    
    pthread_mutex_unlock(&id_lock);
    return id;
}


//This function changes the current path to a given file
void get_path(char* whole_path, char* file)
{
    if (file == NULL)
    {
        return;
    }
    
    sprintf(whole_path, "%s%s", path, file);
}


//here are generic error messages to output for various errors
char* FileError = "File Not Found";
char* invalidFileError = "Invalid File Error";
char* returnFileError = "Error: Could not return file";


//This gets a new unique worker id
//loop to remove requests from the buffer as needed, handles errors
//If file was successfully obtained then this function
//gets its content type, puts it into buffer and sends result to request
//handles logging, and error checking before unlocking file path
void * worker(void * arg)
{
    int w_id = get_t_id();
    int num_req = 0;
    
    while (!kill_child)
    {
        request_t r;
        r = remove_from_buf();
        num_req++;
        
        char* r_path = (char*)malloc(strlen(path) + strlen(r.m_szRequest) + 10);
        get_path(r_path, r.m_szRequest);
        long size = get_size(r_path);
        
        if (size == -1)
        {
            if (return_error(r.m_socket, FileError) != 0)
            {
                fprintf(stderr, "Could not send an error to request\n");
            }
            log_write(w_id, num_req, r.m_socket, r.m_szRequest, -1, FileError);
        }
        else
        {
            char* file_type = get_file_type(r.m_szRequest);
            if (file_type != NULL)
            {
                char* to_send = (char*)malloc(size);
                
                if (load_file(r_path,to_send, size) == 0)
                {
                    if(return_result(r.m_socket, file_type, to_send, size)!= 0)
                    {
                        fprintf(stderr, "Error sending result\n");
                    }
                    else
                    {
                        log_write(w_id, num_req, r.m_socket, r.m_szRequest, size, NULL);
                    }
                }
                else
                {
                    perror("Error loading file");
                    
                    if (return_error(r.m_socket, returnFileError) != 0)
                    {
                        fprintf(stderr, "error sending request\n");
                    }
                    log_write(w_id, num_req, r.m_socket, r.m_szRequest, -1, returnFileError);
                }
            }
            else
            {
                if (return_error(r.m_socket, invalidFileError) != 0)
                {
                    fprintf(stderr, "Could not send an error to request\n");
                }
                
                log_write(w_id,num_req,r.m_socket,r.m_szRequest,-1,invalidFileError);
            }
        }
        free(r_path);
    }
    
}


//creates locks for all variables that may need locked/unlocked
//this is to ensure safety when using threads.
void manage_locks()
{
    if ( pthread_mutex_init(&f_lock, NULL) != 0 )
    {
        fprintf(stderr, "Could not obtain a file lock\n");
        exit(EXIT_FAILURE);
    }
    
    if ( pthread_cond_init(&buffer_content, NULL) != 0 )
    {
        fprintf(stderr, "Could not obtain a buffer content cv\n");
        exit(EXIT_FAILURE);
    }
    
    if ( pthread_cond_init(&free_slot, NULL) != 0 )
    {
        fprintf(stderr, "Could not obtain a free slot cv\n");
        exit(EXIT_FAILURE);
    }
    
    if ( pthread_mutex_init(&id_lock, NULL) != 0 )
    {
        fprintf(stderr, "Could not obtain a id lock\n");
        exit(EXIT_FAILURE);
    }
    
    if ( pthread_mutex_init(&die_lock, NULL) != 0 )
    {
        fprintf(stderr, "Could not obtain a child die lock\n");
        exit(EXIT_FAILURE);
    }
    
    if ( pthread_mutex_init(&f_lock, NULL) != 0 )
    {
        fprintf(stderr, "Could not obtain a file lock\n");
        exit(EXIT_FAILURE);
    }
    
    if ( pthread_mutex_init(&buffer_access, NULL) != 0 )
    {
        fprintf(stderr, "Could not obtain a buffer lock\n");
        exit(EXIT_FAILURE);
    }
}


int main(int argc, char **argv)
{
    //Error check first.
    if(argc != 6 && argc != 7)
    {
        printf("Excepting 5 arguments [%d given]\n", argc-1);
        printf("usage: %s port path num_dispatcher num_workers queue_length [cache_size]\n", argv[0]);
        return -1;
    }

	port = atoi(argv[1]);
    
    if (port < 1025 || port > 65535)
    {
        fprintf(stderr, "Invalid port number [%d].\n", port);
        return -1;
    }
    
    path = argv[2];
	num_dispatcher = atoi(argv[3]);
	num_workers = atoi(argv[4]);
	queue_length = atoi(argv[5]);


    if (strlen(path) > 0)
    {
        if (chdir(path) == -1)
        {
            perror("could not chdir\n");
        }
    }
    else
    {
        fprintf(stderr, "invalid path\n");
    }
    
    //output in terminal the
    printf("Starting server on port: %d\n", port);
    printf("path: %s\n", path);
    printf("Num Dispatcher: %d\n", num_dispatcher);
    printf("Num Workers: %d \n", num_workers);
    printf("Queue length: %d \n", queue_length);
    
    //calling initialization functions
    manage_locks();
    init_log();
    init(port);
    
    //create dispatchers
    disp_thds = (pthread_t *)malloc(sizeof(pthread_t)*num_dispatcher);
    
    for (int i = 0; i < num_dispatcher; i++)
    {
        if (pthread_create(&disp_thds[i], NULL, dispatch, NULL) != 0)
        {
            perror("Could not create dispatcher thread\n");
        }
    }
    
    //create workers
    worker_thds = (pthread_t *) malloc( sizeof(pthread_t) * num_workers);
    
    for (int i = 0; i < num_workers; i++)
    {
        if (pthread_create(&worker_thds[i], NULL, worker, NULL) != 0)
        {
            perror("Could not create dispatcher thread\n");
        }
    }
    
    
    //set up signals to handle termination
    sig_handler();
    
    //join dispatcher threads to conclude
    for (int i = 0; i < num_dispatcher; i++)
    {
        pthread_join(disp_thds[i], NULL);
    }
    
    //join worker threads to conclude
    for (int i = 0; i < num_workers; i++)
    {
        pthread_join(worker_thds[i], NULL);
    }
    return 0;
    
}
