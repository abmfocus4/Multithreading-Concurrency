#include <stdlib.h>
#include <stdio.h>

#include <string.h>
#include <unistd.h>

#include <pthread.h>

#include <curl/curl.h>
#include <sys/types.h>

#include "crc.h"        //for crc()
#include "zutil.h"	    //for mem_inf()
#include <math.h>       //for power()

#include <time.h>
#include <arpa/inet.h>

//need to add catpng and findpng

#define max(a, b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

//Globals

struct thread_args
{
    char arg_URL_1[256];
    char arg_URL_2[256];
    char arg_URL_3[256];

    int thread_num;
    
};

struct thread_ret
{
    //tells whether the thread successfully got a strip or was given duplicate strip by server
    int ret_val;
    char *buf;       /* memory to hold a copy of received data */
    int seq;         /* seq num of the buf recieved */
    size_t size;
};

#define FINAL_IMAGE_SIZE 500000 //GIVEN MAX SIZE IS 8*50*1024 KB
#define STRIP_WIDTH 400
#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 1048576  /* 1024*1024 = 1M */
#define BUF_INC  524288   /* 1024*512  = 0.5M */
#define pngFileSetSize 65536 //(256*256)
#define maxSizePNGMacro 16777216 //(4096*4096)

typedef struct recv_buf2 {
    char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
                     /* <0 indicates an invalid seq number */
} RECV_BUF;

typedef unsigned char U8;
typedef unsigned int U32;

//global variables always init to 0 as per c std
int fragment_recved[50] = {0}; //slot is 0 if not recieved and 1 if recieved

int allImagesGotten=0;

extern char *optarg;

//Prototypes
void *do_work(void *arg); //routine that can run as a thread by threads

int recv_buf_init(RECV_BUF *ptr, size_t max_size);

size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata);

size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);

int write_file(const char *path, const void *in, size_t len);

int catpng(char **pngFiles, int pngFilesLength);

char** findpng(char *fileName, char **pngFileSet);

int recv_buf_cleanup(RECV_BUF *ptr);

////////////////////////////////////////////////////////////////////////////////

int recv_buf_cleanup(RECV_BUF *ptr)
{
    if (ptr == NULL) {
	return 1;
    }
    
    free(ptr->buf);
    ptr->size = 0;
    ptr->max_size = 0;
    return 0;
}

int write_file(const char *path, const void *in, size_t len)
{
    FILE *fp = NULL;

    if (path == NULL) {
        fprintf(stderr, "write_file: file name is null!\n");
        return -1;
    }

    if (in == NULL) {
        fprintf(stderr, "write_file: input data is null!\n");
        return -1;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        perror("fopen");
        return -2;
    }

    if (fwrite(in, 1, len, fp) != len) {
        fprintf(stderr, "write_file: imcomplete write!\n");
        return -3; 
    }
    return fclose(fp);

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



size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata)
{
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;
 
    if (p->size + realsize + 1 > p->max_size) {/* hope this rarely happens */ 
        /* received data is not 0 terminated, add one byte for terminating 0 */
        size_t new_size = p->max_size + max(BUF_INC, realsize + 1);   
        char *q = realloc(p->buf, new_size);
        if (q == NULL) {
            perror("realloc"); /* out of memory */
            return -1;
        }
        p->buf = q;
        p->max_size = new_size;
    }

    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}


int recv_buf_init(RECV_BUF *ptr, size_t max_size)
{
    void *p = NULL;
    
    if (ptr == NULL) {
        return 1;
    }

    p = malloc(max_size);
    if (p == NULL) {
	return 2;
    }
    
    ptr->buf = p;
    ptr->size = 0;
    ptr->max_size = max_size;
    ptr->seq = -1;              /* valid seq should be non-negative */
    return 0;
}

void *do_work(void *arg){

struct thread_args *p_in = arg;
struct thread_ret *p_out = malloc(sizeof(struct thread_ret)); /* the root thread will need to free the memory */
p_out->buf = malloc(sizeof(char)*BUF_SIZE);

//do actual work
CURL *curl_handle;
CURLcode res;
RECV_BUF recv_buf;

recv_buf_init(&recv_buf, BUF_SIZE);

curl_global_init(CURL_GLOBAL_DEFAULT);

/* init a curl session */
curl_handle = curl_easy_init();

if (curl_handle == NULL) {
        fprintf(stderr, "curl_easy_init: returned NULL\n");
        return NULL;
    }

/* specify URL to get */

//thread_num contains the thread num
//divding work between each machine
if( (((p_in->thread_num)%3)+1) == 1)
{
    curl_easy_setopt(curl_handle, CURLOPT_URL, p_in->arg_URL_1);//giving url from input argument to thread

}

else if( (((p_in->thread_num)%3)+1) == 2){
curl_easy_setopt(curl_handle, CURLOPT_URL, p_in->arg_URL_2);//giving url from input argument to thread
}

else
{
    curl_easy_setopt(curl_handle, CURLOPT_URL, p_in->arg_URL_3);//giving url from input argument to thread
}


/* register write call back function to process received data */
curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl3); 

/* user defined data structure passed to the call back function */
curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&recv_buf);

/* register header call back function to process received header data */
curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl); 

/* user defined data structure passed to the call back function */
curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)&recv_buf);

/* some servers requires a user-agent field */
curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

/* get it! */
res = curl_easy_perform(curl_handle);
if( res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    }

//variables for keeping track of loop and sequence strips recieved
fragment_recved[recv_buf.seq] = 1; //changing buffer value
p_out->ret_val = 1; // thread successfully got a strip
p_out->seq = recv_buf.seq;
p_out -> size = recv_buf.size;
//copying the return value of p_out to be recv_buf.buf
memcpy(p_out->buf, recv_buf.buf, (recv_buf.size)+1);


/* cleaning up */
curl_easy_cleanup(curl_handle);
curl_global_cleanup();
recv_buf_cleanup(&recv_buf);

return ((void *)p_out);

}

/////////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv){

//////////////////////////////////////////////////////////////////////////////

//trying to get num_of_threads(t) and image_num(N)

    int c; //case for switch statement
    int t = 1; //default
    int n = 1; //default
    char *str = "option requires an argument"; //for printing error
    
    while ((c = getopt (argc, argv, "t:n:")) != -1) {
        switch (c) {

        //num_threads
        case 't':
	    t = strtoul(optarg, NULL, 10);
	    
        //t contains the num_threads

        //error checking for t value
	    if (t <= 0) {
                fprintf(stderr, "%s: %s > 0 -- 't'\n", argv[0], str);
                return -1;
            }
            break;

        //image_num
        case 'n':
            n = strtoul(optarg, NULL, 10);
	    
            //n contains the image_num

            //error checking for n value
            if (n <= 0 || n > 3) {
                fprintf(stderr, "%s: %s 1, 2, or 3 -- 'n'\n", argv[0], str);
                return -1;
            }
            break;
        default:
            return -1;
        }
    }

//creating URL to work on

//url for different machines
char url_1[256]; 
char url_2[256];
char url_3[256];

memset(url_1, 0, 256);
memset(url_2, 0, 256);
memset(url_3, 0, 256);

char machine_1[] ="http://ece252-1.uwaterloo.ca:2520/image?img=";

char machine_2[]="http://ece252-2.uwaterloo.ca:2520/image?img=";

char machine_3[]="http://ece252-3.uwaterloo.ca:2520/image?img=";

//url contains the machine specific urls
strcpy(url_1,machine_1);
strcpy(url_2,machine_2);
strcpy(url_3,machine_3);

//appending image_num to urls
url_1[44] = n + '0';
url_2[44] = n + '0';
url_3[44] = n + '0';

//urls for different machines are now ready
//creating n threads
int num_threads = t;
struct thread_args in_params[num_threads]; //don't know what input parameters to send
struct thread_ret *p_results[num_threads]; //don't know what results to get

U8 **IDATData = malloc(sizeof(char)*50*BUF_SIZE);

for (int i = 0; i< 50; i++) {
    IDATData[i] = malloc(sizeof(char)*BUF_SIZE);
}

U8 **IDATInflated = malloc(sizeof(char)*50*BUF_SIZE);

for (int i = 0; i< 50; i++) {
    IDATInflated[i] = malloc(sizeof(char)*BUF_SIZE);
}

int IDAT_lengths[50] = {0};


while(!allImagesGotten) {
    
    pthread_t *p_tids = malloc(sizeof(pthread_t) * num_threads);

    for(int i=0; i<num_threads;i++)
    {
        //initializing input params
        in_params[i].thread_num = i;

        strcpy(in_params[i].arg_URL_1,url_1);
        strcpy(in_params[i].arg_URL_2,url_2);
        strcpy(in_params[i].arg_URL_3,url_3);

        pthread_create(p_tids + i, NULL, do_work, in_params + i);
    }

    //threads have done their work now, time to join
    char IDATLengthArray[4];
    for (int i = 0; i< num_threads; i++) {

        //Thread ID %lu joined
        pthread_join(p_tids[i], (void **)&(p_results[i]));
                
                memcpy(IDATLengthArray, p_results[i]->buf + 33, 4);
                IDAT_lengths[p_results[i] -> seq] = ntohl(*((U32 *)IDATLengthArray));
                memcpy(IDATData[p_results[i] -> seq], (p_results[i] -> buf) + 41, IDAT_lengths[p_results[i] -> seq]);
          
    }

    /* cleaning up */

    free(p_tids);
    
    /* the memory was allocated in the do_work thread for return values */
    /* we need to free the memory here */

    //deallocate all the parameters and arguments
    for (int i=0; i<num_threads; i++) {
        free(p_results[i]);
        p_results[i] = NULL;
    }

    for (int i = 0; i < 50; i++) {
                if(!fragment_recved[i]) {
                    allImagesGotten = 0;
                    break;
                }
                allImagesGotten = 1;
            }

}

U64 IDATInflatedLength = 0;
U64 inflatedLengthTotal = 0;
int flag = 0;
U8 *IDATFinal = malloc(sizeof(char)*BUF_SIZE*50);
for (int i = 0; i < 50; i++) {
    int success = mem_inf(IDATInflated[i], &IDATInflatedLength, IDATData[i], IDAT_lengths[i]);
    if (success < 0) {
        printf("Error with inflation for %d strip\n", i);
        return -1;
    }
    inflatedLengthTotal += IDATInflatedLength;
    int temp = flag;
    for (int j = temp; j < inflatedLengthTotal; j++) {
        IDATFinal[j] = IDATInflated[i][j - temp];
        flag++;
    }
}

U8 *IDATFinalDeflated = malloc(sizeof(char)*BUF_SIZE*50);
U64 deflatedLength = 0;
int success = mem_def(IDATFinalDeflated, &deflatedLength, IDATFinal, inflatedLengthTotal, Z_DEFAULT_COMPRESSION);
if (success < 0) {
    printf("Error in deflation\n");
    return -1;
}

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
        hexVal = deflatedLength;
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

        fwrite(IDATFinalDeflated, 1, deflatedLength, finalPNGFile);

        U8 IDATbuf[deflatedLength + 4];
        fseek(finalPNGFile, 37, SEEK_SET);
        fread(IDATbuf, 1, deflatedLength + 4, finalPNGFile);
        value = crc(IDATbuf, deflatedLength + 4);
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
        fseek(finalPNGFile, 49 + deflatedLength, SEEK_SET);
        fread(IENDbuf, 1, 4, finalPNGFile);
        value = crc(IENDbuf, 4);
        hexVal = value;
        u_int32_t *fwrite14 = malloc(sizeof(int)*4);
        *fwrite14 = htonl(hexVal);
        fwrite(fwrite14, 1, 4, finalPNGFile);

        fclose(finalPNGFile);
        free(fwrite14);
        fwrite14 = NULL;
        free(IDATFinalDeflated);
        IDATFinalDeflated = NULL;


for(int i = 0; i < 50; i++) {
    free(IDATData[i]);
    IDATData[i] = NULL;
}

free(IDATData);
IDATData = NULL;
free(IDATFinal);
IDATFinal = NULL;
free(IDATFinalDeflated);
IDATFinalDeflated = NULL;


return 0;

}
