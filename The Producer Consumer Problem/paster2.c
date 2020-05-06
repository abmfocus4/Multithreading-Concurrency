
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <curl/curl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <time.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include "zutil.h"
#include "crc.h"
#include "shm_stack.h"

#define IMG_URL "http://ece252-%d.uwaterloo.ca:2530/image?img=%d&part=%d"
#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 10240  /* 1024*10 = 10K */
#define SEM_PROC 1	//used by aarti- CAN'T WE JUST USE 0
#define FORKED_PROD 1
#define FORKED_CON 1


#define PROD_MUTEX 0
#define CON_MUTEX 1
#define SPACES 2
#define ITEMS 3



typedef struct recv_buf_flat {
    char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
                     /* <0 indicates an invalid seq number */
} RECV_BUF;

/* a stack that can hold integers */
/* Note this structure can be used by shared memory,
   since the items field points to the memory right after it.
   Hence the structure and the data items it holds are in one
   continuous chunk of memory.

   The memory layout:
   +===============+
   | size          | 4 bytes
   +---------------+
   | pos           | 4 bytes
   +---------------+
   | items         | 8 bytes
   +---------------+
   | items[0]      | 4 bytes
   +---------------+
   | items[1]      | 4 bytes
   +---------------+
   | ...           | 4 bytes
   +---------------+
   | items[size-1] | 4 bytes
   +===============+
*/
typedef struct int_stack
{
    int size;               /* the max capacity of the stack */
    int pos;                /* position of last item pushed onto the stack */
    int *items;             /* stack of stored integers */
} ISTACK;

int sizeof_shm_recv_buf(size_t nbytes, int B);
void producer(ISTACK *emptyStack, ISTACK *seqStack, ISTACK *prodStack, ISTACK *conStack, RECV_BUF **boundBuf, int img_num, sem_t *sems, U8 *IDATInflatedConcat, int index_size, int buffer_size, U8* ptr);
void consumer(int con_sleep, RECV_BUF **boundBuf, ISTACK *seqStack, ISTACK *emptyStack, ISTACK *conStack, RECV_BUF *buffer, sem_t *sems, U8 *IDATInflatedConcat, int buffer_size, U8* ptr);

typedef unsigned int  U32;

size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);
size_t write_cb_curl(char *p_recv, size_t size, size_t nmemb, void *p_userdata);

int sizeof_shm_recv_buf(size_t nbytes, int B)
{
    return ((sizeof(RECV_BUF) + sizeof(char)*nbytes)*B) ;
}

size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;

    if (realsize > strlen(ECE252_HEADER) &&
	strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0) {

    /* extract img sequence number */
	p->seq = atoi(p_recv + strlen(ECE252_HEADER));


    }
    return realsize;
}

int shm_recv_buf_init(RECV_BUF *ptr, size_t nbytes)
{
    if ( ptr == NULL ) {
        return 1;
    }

    ptr->size = 0;
    ptr->max_size = nbytes;
    ptr->seq = -1; /* valid seq should be non-negative */
    ptr->buf = (char *)ptr + sizeof(RECV_BUF);

    memset(ptr->buf, 0, BUF_SIZE);

    return 0;
}

size_t write_cb_curl(char *p_recv, size_t size, size_t nmemb, void *p_userdata)
{
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;

    if (p->size + realsize + 1 > p->max_size) {/* hope this rarely happens */
        fprintf(stderr, "User buffer is too small, abort...\n");
        abort();
    }

    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}

void producer(ISTACK *emptyStack, ISTACK *seqStack, ISTACK *prodStack, ISTACK *conStack, RECV_BUF **boundBuf, int img_num, sem_t *sems, U8 *IDATInflatedConcat, int index_size, int buffer_size, U8* ptr) {



srand(time(NULL));

    CURL *curl_handle;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    /* init a curl session */
    curl_handle = curl_easy_init();

    if (curl_handle == NULL) {
        fprintf(stderr, "curl_easy_init: returned NULL\n");
        // return 1;
        return;
    }

    int random = (rand() % 3) + 1;

    int index = pop(emptyStack);

    int sequence = pop(seqStack);

    char url[256];
    sprintf(url, "http://ece252-%d.uwaterloo.ca:2530/image?img=%d&part=%d", random, img_num, sequence);

    /* specify URL to get */
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);

    /* register write call back function to process received data */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl);
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)boundBuf[index]);

    /* register header call back function to process received header data */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl);
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)boundBuf[index]);

    /* some servers requires a user-agent field */
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

    /* get it! */
    res = curl_easy_perform(curl_handle);

    if( res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    }
    else
    {
     push(conStack, index);

    }

    /* cleaning up */
    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();

return;
}

void consumer(int con_sleep, RECV_BUF **boundBuf, ISTACK *seqStack, ISTACK *emptyStack, ISTACK *conStack, RECV_BUF *buffer, sem_t *sems, U8 *IDATInflatedConcat, int buffer_size, U8* ptr) {

        int index = pop(conStack);

        memcpy(buffer -> buf, boundBuf[index] -> buf, boundBuf[index] -> size);
        buffer -> seq = boundBuf[index] -> seq;
        buffer -> size = boundBuf[index] -> size;
        buffer -> max_size = boundBuf[index] -> max_size;

        memset(boundBuf[index] -> buf, 0, BUF_SIZE);
        shm_recv_buf_init(boundBuf[index], BUF_SIZE);

        push(emptyStack, index);

        usleep(con_sleep*1000);

        char IDATlength[4];
        memcpy(IDATlength, (buffer -> buf) + 33, 4);

        int IDATLengthNum = ntohl(*((U32 *)IDATlength));

        U8 *IDATData = malloc(sizeof(U8)*BUF_SIZE);
        memcpy(IDATData, (U8* )((buffer -> buf) + 41), IDATLengthNum);

        U64 inflatedLength;

        U8 *IDATStripInflated = malloc(sizeof(U8)*9606);

        int success = mem_inf(IDATStripInflated, &inflatedLength, IDATData, IDATLengthNum);

        if (success < 0) {
            printf("Error with inflation for %d strip\n", buffer -> seq);
            return;
        }

        memcpy((IDATInflatedConcat + ((buffer -> seq) * 9606)), IDATStripInflated, 9606);

        free(IDATStripInflated);

        free(IDATData);

        return;
}

int main(int argc, char **argv){

    //extract argv start

    int buffer_size = atoi(argv[1]);
    int numprod = atoi(argv[2]);
    int numcon = atoi(argv[3]);
    int con_sleep = atoi(argv[4]);
    int img_num = atoi(argv[5]);
    double times[2];
   struct timeval tv;
   int check = 0;
    //int start_flag; //starting the timing parameter to print output

    char *err_str = "incorrect argument recieved- try again!"; //for printing error
	char *img_err_str = "incorrect value for arg N recieved";

	//error checking for args
	if( (buffer_size<=0) || (numprod<=0) || (numcon<=0) || (con_sleep<0))
	{
		fprintf(stderr, "%s\n", err_str);
        return -1;
	}

	if(!((img_num ==1) || (img_num ==2) || (img_num ==3) )){
		fprintf(stderr, "%s\n", img_err_str);
		return -1;
	}

    ISTACK *prodStack;
    ISTACK *conStack;

    ISTACK *emptyStack;//contains empty indexes - buffer_size
    int num_con = FORKED_CON;
    int num_prod = FORKED_PROD;
    ISTACK *seqStack;//contains unrecieved sequences - 50
    int seqStack_size = 50;


    int stack_size1 = sizeof_shm_stack(buffer_size);
    int stack_size2 = sizeof_shm_stack(seqStack_size);

    U8 *IDATInflatedConcat;
    int concat_size = sizeof(U8)*9606*50;
    int factor2 = PROD_MUTEX + ITEMS;
    // RECV_BUF *boundBuf[buffer_size];   //ptrs of bb[0], bb[1]...
    RECV_BUF **boundBuf;
    if((numprod+factor2) > (ITEMS+SPACES))
    check = 1;


    boundBuf = malloc(sizeof(RECV_BUF*)*buffer_size);
    //int completed = CON_MUTEX;
    int not_finished = PROD_MUTEX;
    int boundBuf_size = sizeof_shm_recv_buf(BUF_SIZE, buffer_size);
    int index_size = sizeof_shm_recv_buf(BUF_SIZE, 1);

    U8* ptr;

    sem_t *sems;
    int sems_size = 4*sizeof(sem_t);

    int shmid_prod, shmid_cons, shmid_emptyIndex, shmid_seqStack, shmid_sems, shmid_concat, shmid_ptr;


    shmid_prod = shmget(IPC_PRIVATE, stack_size1, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    if (shmid_prod == -1 ) {
        perror("shmget");
        abort();
    }

    shmid_cons = shmget(IPC_PRIVATE, stack_size1, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    if (shmid_cons == -1 ) {
        perror("shmget");
        abort();
    }

    shmid_emptyIndex = shmget(IPC_PRIVATE, stack_size1, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    if (shmid_emptyIndex == -1 ) {
        perror("shmget");
        abort();
    }

    shmid_seqStack = shmget(IPC_PRIVATE, stack_size2, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    if (shmid_seqStack == -1 ) {
        perror("shmget");
        abort();
    }

    shmid_sems = shmget(IPC_PRIVATE, sems_size, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    if (shmid_sems == -1 ) {
        perror("shmget");
        abort();
    }

    shmid_concat = shmget(IPC_PRIVATE, concat_size, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    if (shmid_concat == -1 ) {
        perror("shmget");
        abort();
    }

    shmid_ptr = shmget(IPC_PRIVATE, boundBuf_size, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    if ( shmid_ptr == -1 ) {
        perror("shmget");
        abort();
    }

    prodStack = shmat(shmid_prod, NULL, 0);
    conStack = shmat(shmid_cons, NULL, 0);
    emptyStack = shmat(shmid_emptyIndex, NULL, 0);
    seqStack = shmat(shmid_seqStack, NULL, 0);
    sems = shmat(shmid_sems, NULL, 0);
    IDATInflatedConcat = shmat(shmid_concat, NULL, 0);
    ptr = shmat(shmid_ptr, NULL, 0);


    for(int i=0; i<buffer_size; i++) {
        boundBuf[i] = (RECV_BUF*)(ptr + (i*index_size));
    }

   int check_1 = init_shm_stack(prodStack, buffer_size);
   int check_2 = init_shm_stack(conStack, buffer_size);
   int check_3 = init_shm_stack(emptyStack, buffer_size);
   int check_4 = init_shm_stack(seqStack, seqStack_size);

   if(check_1 || check_2 || check_3 || check_4) {
        printf("init shm stack has failed\n");
   }


    for (int i = 0; i < buffer_size; i++) {
        int shm_recv_buf_init_check = shm_recv_buf_init(boundBuf[i], BUF_SIZE);

        if(shm_recv_buf_init_check)
        {
            printf("shm_recv_buf_init failed\n");
        }

    }

    for (int i = 0; i < buffer_size; i++) {
        push(emptyStack, i);
    }

    for (int s = 0; s < seqStack_size; s++) {
        push(seqStack, s);
    }

    memset(IDATInflatedConcat, 0, concat_size);


    int sem1 = sem_init(&sems[PROD_MUTEX], 1, 1);
    int sem2 = sem_init(&sems[CON_MUTEX], 1, 1);
    int sem3 = sem_init(&sems[SPACES], 1, buffer_size);
    int sem4 = sem_init(&sems[ITEMS], 1, 0);

	if( sem1 != 0 ) {
        perror("sem_init(sem[1])");
        abort();
    }
    if( sem2 != 0 ) {
        perror("sem_init(sem[1])");
        abort();
    }
    if ( sem3 != 0 ) {
        perror("sem_init(sem[1])");
        abort();
    }
    if ( sem4 != 0 ) {
        perror("sem_init(sem[1])");
        abort();
    }

    //forking start
    //separating child pid and parent pid like aarti for debugging purposes
    pid_t pid=0;
    pid_t prodPid[num_prod];
    pid_t consPid[num_con];
    int prodChildStatus[num_prod];
    int conChildStatus[num_con];
    for (int i = 0; i < num_prod; i++) {
        prodChildStatus[i] = -100;
    }
    for (int j = 0; j < num_con; j++) {
        conChildStatus[j] = -100;
    }
    U64 deflatedLength = 0;
    U8 *IDATFinalDeflated = malloc(sizeof(U8)*BUF_SIZE*50);


    if (gettimeofday(&tv, NULL) != 0) {
    perror("gettimeofday");
    abort();
    }

    if(!check)
    times[1] = (tv.tv_sec) + (tv.tv_usec/1000000.);
    //do actual forks for producers and consumers
        for (int i = 0; i < num_prod; i++) {

        pid = fork();
        if (pid > 0) {
            prodPid[i] = pid;
        }
        else if (pid == 0) {

           while(!is_empty(seqStack)) {

                sem_wait(&sems[SPACES]);
                sem_wait(&sems[PROD_MUTEX]);

                producer(emptyStack, seqStack, prodStack, conStack, boundBuf, img_num, sems, IDATInflatedConcat, index_size, buffer_size, ptr);

                sem_post(&sems[ITEMS]);
                sem_post(&sems[PROD_MUTEX]);

           }

            shmdt(sems);
            shmdt(ptr);
            shmdt(emptyStack);
            shmdt(prodStack);
            shmdt(conStack);
            shmdt(seqStack);
            shmdt(IDATInflatedConcat);

        //for debugging - finding out how much time the producer takes


            break;
        }
        else {
            perror("fork");
            abort();
        }
    }


    for (int i = 0; i < num_con; i++) {

        pid = fork();
        if (pid > 0) {
            consPid[i] = pid;
        }
        else if (pid == 0) {

            prodStack = shmat(shmid_prod, NULL, 0);
            conStack = shmat(shmid_cons, NULL, 0);
            emptyStack = shmat(shmid_emptyIndex, NULL, 0);
            seqStack = shmat(shmid_seqStack, NULL, 0);

            IDATInflatedConcat = shmat(shmid_concat, NULL, 0);
            ptr = shmat(shmid_ptr, NULL, 0);
            sems = shmat(shmid_sems, NULL, 0);

            while(1) {

                if (is_empty(seqStack) && is_empty(conStack)) {
                    break;
                }


                sem_wait(&sems[ITEMS]);

                sem_wait(&sems[CON_MUTEX]);

                RECV_BUF *buffer = malloc(sizeof(RECV_BUF) + BUF_SIZE);

                shm_recv_buf_init(buffer, BUF_SIZE);
                buffer -> buf = malloc(buffer->max_size);

                memset(buffer -> buf, 0, sizeof(char)*BUF_SIZE);

                consumer(con_sleep, boundBuf, seqStack, emptyStack, conStack, buffer, sems, IDATInflatedConcat, buffer_size, ptr);

                free(buffer -> buf);
                buffer -> buf = NULL;
                free(buffer);
                buffer = NULL;


                sem_post(&sems[SPACES]);


                sem_post(&sems[CON_MUTEX]);

            }

            shmdt(sems);
            shmdt(ptr);
            shmdt(emptyStack);
            shmdt(prodStack);
            shmdt(conStack);
            shmdt(seqStack);
            shmdt(IDATInflatedConcat);

            //consumer has completed concatenating images and placed in IDAT data


            break;
        }
        else {
            perror("fork");
            abort();
        }
    }


        if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
        }

        if(!check)
        times[0] = ((tv.tv_sec) + (tv.tv_usec))/1000000.;

    //get parent to wait for children to finish
    if ( pid > 0 ) {
        for (int i = 0; i < num_prod; i++ ) {

            waitpid(prodPid[i], &prodChildStatus[i], 0);

            // if (WIFEXITED(prodChildStatus[i])) {
            //         printf("Child prodpid[%d]=%d terminated with ChildStatus: %d.\n", i, prodPid[i], prodChildStatus[i]);
            // }
        }

        for (int i = 0; i < num_con; i++ ) {

            waitpid(consPid[i], &conChildStatus[i], 0);

            // if (WIFEXITED(conChildStatus[i])) {
            //         printf("Child conspid[%d]=%d terminated with ChildStatus: %d.\n", i, consPid[i], conChildStatus[i]);
            // }
        }



        //deflate the data consumers just processed and put in shm
        int success = mem_def(IDATFinalDeflated, &deflatedLength, IDATInflatedConcat, 9606*50, Z_DEFAULT_COMPRESSION);
        if (success < 0) {
            printf("Error in deflation\n");
            return -1;
        }


        //write deflated data into file
        FILE *finalPNGFile = fopen("all.png", "wb+");
        char header[] = {0x89, 0x50, 0x4E,0x47, 0x0D, 0x0A,0x1A,0x0A};
        fwrite(header, 1, 8, finalPNGFile);

        //write IHDR chunk
        u_int32_t hexVal = 0x0000000d;
        u_int32_t *fwrite1 = malloc(sizeof(int)*4);
        *fwrite1 = htonl(hexVal);

        fwrite(fwrite1, 1, 4, finalPNGFile);

        free(fwrite1);
        fwrite1 = NULL;
        hexVal = 0x49484452;
        u_int32_t *fwrite2 = malloc(sizeof(int)*4);
        *fwrite2 = htonl(hexVal);


        fwrite(fwrite2, 1, 4, finalPNGFile);

        free(fwrite2);
        fwrite2 = NULL;
        hexVal = 400;
        u_int32_t *fwrite3 = malloc(sizeof(int)*4);
        *fwrite3 = htonl(hexVal);

        fwrite(fwrite3, 1, 4, finalPNGFile);

        free(fwrite3);
        fwrite3 = NULL;
        hexVal = 300;
        u_int32_t *fwrite4 = malloc(sizeof(int)*4);
        *fwrite4 = htonl(hexVal);

        fwrite(fwrite4, 1, 4, finalPNGFile);

        free(fwrite4);
        fwrite4 = NULL;
        hexVal = 0x08;
        u_int32_t *fwrite5 = malloc(sizeof(int));
        *fwrite5 = hexVal;

        fwrite(fwrite5, 1, 1, finalPNGFile);//bit depth

        free(fwrite5);
        fwrite5 = NULL;
        hexVal = 0x06;
        u_int32_t *fwrite6 = malloc(sizeof(int));
        *fwrite6 = hexVal;

        fwrite(fwrite6, 1, 1, finalPNGFile);//colour type

        free(fwrite6);
        fwrite6 = NULL;
        hexVal = 0x000000;
        u_int32_t *fwrite7 = malloc(sizeof(int)*3);
        *fwrite7 = htonl(hexVal);

        fwrite(fwrite7, 1, 3, finalPNGFile);//compression, filter, interlace methods

        free(fwrite7);
        fwrite7 = NULL;

        U8 IHDRbuf[17];
        fseek(finalPNGFile, 12, SEEK_SET);
        fread(IHDRbuf, 1, 17, finalPNGFile);
        U32 value = crc(IHDRbuf, 17);
        hexVal = value;
        u_int32_t *fwrite12 = malloc(sizeof(int)*4);
        *fwrite12 = htonl(hexVal);
        fwrite(fwrite12, 1, 4, finalPNGFile);

        //write IDAT chunk
        free(fwrite12);
        fwrite12 = NULL;
        hexVal = deflatedLength;//this was changed from catpng
        u_int32_t *fwrite8 = malloc(sizeof(int)*4);
        *fwrite8 = htonl(hexVal);

        fwrite(fwrite8, 1, 4, finalPNGFile);

        free(fwrite8);
        fwrite8 = NULL;
        hexVal = 0x49444154;
        u_int32_t *fwrite9 = malloc(sizeof(int)*4);
        *fwrite9 = htonl(hexVal);



        fwrite(fwrite9, 1, 4, finalPNGFile);//IDAT

        free(fwrite9);
        fwrite9 = NULL;


        fwrite(IDATFinalDeflated, 1, deflatedLength, finalPNGFile);//this was changed from catpng

        U8 IDATbuf[deflatedLength + 4];//changed from catpng.c
        fseek(finalPNGFile, 37, SEEK_SET);
        fread(IDATbuf, 1, deflatedLength + 4, finalPNGFile);//changed from catpng.c
        value = crc(IDATbuf, deflatedLength + 4);//changed from catpng.c
        hexVal = value;
        u_int32_t *fwrite13 = malloc(sizeof(int)*4);
        *fwrite13 = htonl(hexVal);
        fwrite(fwrite13, 1, 4, finalPNGFile);

        //write IEND chunk
        free(fwrite13);
        fwrite13 = NULL;
        hexVal = 0x00000000;
        u_int32_t *fwrite10 = malloc(sizeof(int)*4);
        *fwrite10 = htonl(hexVal);

        fwrite(fwrite10, 1, 4, finalPNGFile);

        free(fwrite10);
        fwrite10 = NULL;
        hexVal = 0x49454e44;
        u_int32_t *fwrite11 = malloc(sizeof(int)*4);
        *fwrite11 = htonl(hexVal);

        fwrite(fwrite11, 1, 4, finalPNGFile);

        free(fwrite11);
        fwrite11 = NULL;
        U8 IENDbuf[4];
        fseek(finalPNGFile, 49 + deflatedLength, SEEK_SET);//changed from catpng.c
        fread(IENDbuf, 1, 4, finalPNGFile);
        value = crc(IENDbuf, 4);
        hexVal = value;
        u_int32_t *fwrite14 = malloc(sizeof(int)*4);
        *fwrite14 = htonl(hexVal);
        fwrite(fwrite14, 1, 4, finalPNGFile);

        fclose(finalPNGFile);
        free(fwrite14);
        fwrite14 = NULL;


        if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
        }

        if(!check)
        times[0] = (tv.tv_sec) + (tv.tv_usec/1000000.);
        printf("paster2 execution time: %.6lf seconds\n", (times[0] - times[1]));

        //error checking

        if(not_finished){
            printf("Program was terminated before creating all.png\n");
        }
    //forking end

    //free space
    free(IDATFinalDeflated);
    IDATFinalDeflated = NULL;

    //destroy semaphores
    if (sem_destroy(&sems[SPACES]) || sem_destroy(&sems[PROD_MUTEX]) || sem_destroy(&sems[ITEMS]) || sem_destroy(&sems[CON_MUTEX])) {
        perror("Failed sem_destroy function!");
        abort();
    }

    }
    free(boundBuf);
    boundBuf = NULL;
    shmdt(ptr);
    shmdt(emptyStack);
    shmdt(prodStack);
    shmdt(conStack);
    shmdt(seqStack);
    shmdt(sems);
    shmdt(IDATInflatedConcat);

    //delete (?) shared memory
    shmctl(shmid_ptr, IPC_RMID, NULL);
    shmctl(shmid_cons, IPC_RMID, NULL);
    shmctl(shmid_prod, IPC_RMID, NULL);
    shmctl(shmid_emptyIndex, IPC_RMID, NULL);
    shmctl(shmid_seqStack, IPC_RMID, NULL);
    shmctl(shmid_sems, IPC_RMID, NULL);
    shmctl(shmid_concat, IPC_RMID, NULL);



    return 0;
}
