#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>
#include <search.h>                 //for hsearch
#include <pthread.h>


#define MAX_URLS 1000
#define BUF_SIZE 1048576  /* 1024*1024 = 1M */
#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_INC  524288   /* 1024*512  = 0.5M */
#define DEFAULT_URL "http://ece252-1.uwaterloo.ca/lab4"
#define MULTI_THREAD 2


#define CT_PNG  "image/png"
#define CT_HTML "text/html"
#define CT_JPG "image/jpeg"
#define CT_PNG_LEN  9
#define CT_HTML_LEN 9

#define max(a, b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

typedef struct recv_buf2 {
    char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
                     /* <0 indicates an invalid seq number */
} RECV_BUF;

//frontier is a list of Urls to be processed
//it is implemented as a queue
char **frontier;
int frontierFirst;  //head of the queue
int frontierTop;    //tail of the queue

char **pngUrls;
int pngUrlsTop;

char **visitedUrls;
int visitedUrlsTop;

int threads_waiting;
int threads;

extern char *optarg;

pthread_mutex_t png_mutex;
pthread_mutex_t frontier_mutex;
pthread_mutex_t visited_mutex;
pthread_cond_t threshold_cv;

htmlDocPtr mem_getdoc(char *buf, int size, const char *url);
xmlXPathObjectPtr getnodeset (xmlDocPtr doc, xmlChar *xpath);

size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);
size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata);
int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);
void cleanup(CURL *curl, RECV_BUF *ptr);
CURL *easy_handle_init(RECV_BUF *ptr, const char *url);

int process_data(CURL *curl_handle, RECV_BUF *p_recv_buf);
int process_html(CURL *curl_handle, RECV_BUF *p_recv_buf);
int find_http(char *fname, int size, int follow_relative_links, const char *base_url);
int process_other(CURL *curl_handle, RECV_BUF *p_recv_buf);
int process_png(CURL *curl_handle, RECV_BUF *p_recv_buf);

void *do_work(void *arg);

struct thread_args {
    int thread_num;
    int pngs;

};

int visitedUrlFound(char *url) {

    //should never occur if the breaking condition, i.e. the barrier is doing it's job
    if (url == NULL) {
        return 0;
    }

    pthread_mutex_lock(&visited_mutex);
    for (int i = 0; i <= visitedUrlsTop; i++) {
        if(!strncmp(visitedUrls[i], url, strlen(url))){
            pthread_mutex_unlock(&visited_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&visited_mutex);
    return 0;
    
}

int process_other(CURL *curl_handle, RECV_BUF *p_recv_buf){


        pthread_mutex_lock(&frontier_mutex);
        int notFound = !visitedUrlFound(frontier[frontierFirst]);
        pthread_mutex_unlock(&frontier_mutex);

        if(notFound) {

            pthread_mutex_lock(&visited_mutex);

            __sync_add_and_fetch(&visitedUrlsTop, 1);
            visitedUrls[visitedUrlsTop] = malloc(sizeof(char)*500);
            memset(visitedUrls[visitedUrlsTop], 0, sizeof(char)*500);

            pthread_mutex_lock(&frontier_mutex);
            if (frontier[frontierFirst] == NULL) {
                pthread_mutex_unlock(&frontier_mutex);
                pthread_mutex_unlock(&visited_mutex);
                return 0;
            }
            strcpy(visitedUrls[visitedUrlsTop], frontier[frontierFirst]);
            pthread_mutex_unlock(&frontier_mutex);
            pthread_mutex_unlock(&visited_mutex);
        }

        __sync_add_and_fetch( &frontierFirst, 1 );
        return 0;
        
}

void *do_work(void *arg) {
    struct thread_args *p_in = arg;
    int num_pngs = p_in->pngs;


    //init cURL stuff
    CURL *curl_handle;
    CURLcode res;
    RECV_BUF recv_buf;

    while(1) {
        //Breaking conditons:
        pthread_mutex_lock(&png_mutex);
        if (pngUrlsTop == (num_pngs - 1)) {
            pthread_mutex_unlock(&png_mutex);
            break;
        }
        pthread_mutex_unlock(&png_mutex);
    
    //Barrier code begins:
            pthread_mutex_lock(&frontier_mutex);

            if((frontierFirst > frontierTop)){
            threads_waiting++;
            if (threads_waiting == threads) {
                pthread_cond_broadcast(&threshold_cv);
                pthread_mutex_unlock(&frontier_mutex);
                break;
            }

            pthread_cond_wait(&threshold_cv, &frontier_mutex);

            if (threads_waiting == threads) {
                pthread_mutex_unlock(&frontier_mutex);
                break;
            }

            threads_waiting--;
            pthread_mutex_unlock( &frontier_mutex );
            continue;
        }


        int notFound = !visitedUrlFound(frontier[frontierFirst]);
        pthread_mutex_unlock(&frontier_mutex);
        

        //Barrier code Ends:

        if(notFound) {

        //if the hash doesn't already contain the url, proceed

            //initialize cURL functions
            pthread_mutex_lock(&frontier_mutex);
            curl_handle = easy_handle_init(&recv_buf, frontier[frontierFirst]);
            pthread_mutex_unlock(&frontier_mutex);

            if ( curl_handle == NULL ) {
                fprintf(stderr, "Curl initialization failed. Exiting...\n");
                cleanup(curl_handle, &recv_buf);
                curl_global_cleanup();
                abort();
            }

            else {
                //connect to url using cURL and collect file data inside recv_buf
                res = curl_easy_perform(curl_handle);

                if (res != CURLE_OK) {

                    pthread_mutex_lock(&visited_mutex);
                    __sync_add_and_fetch(&visitedUrlsTop, 1);
                    visitedUrls[visitedUrlsTop] = malloc(sizeof(char)*500);
                    memset(visitedUrls[visitedUrlsTop], 0, sizeof(char)*500);
                    pthread_mutex_lock(&frontier_mutex);
                    if (frontier[frontierFirst] == NULL) {
                            pthread_mutex_unlock(&frontier_mutex);
                            pthread_mutex_unlock(&visited_mutex);
                            break;
                    }
                    strcpy(visitedUrls[visitedUrlsTop], frontier[frontierFirst]);
                    pthread_mutex_unlock(&frontier_mutex);
                    pthread_mutex_unlock(&visited_mutex);

                    __sync_add_and_fetch( &frontierFirst, 1 );

                    cleanup(curl_handle, &recv_buf);
                }
                else {
                    process_data(curl_handle, &recv_buf);
                    /* cleaning up */
                    cleanup(curl_handle, &recv_buf);
                }
            }
        }
        else {
            //if the url is found in the hash, we just want to look at the next element and proceed
            pthread_mutex_lock(&frontier_mutex);
            if (frontier[frontierFirst] == NULL) {
                pthread_mutex_unlock(&frontier_mutex);
                break;
            }
            __sync_add_and_fetch( &frontierFirst, 1 );
            pthread_mutex_unlock(&frontier_mutex);
        }
    }

    return NULL;   
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
    ptr->seq = -1;              /* valid seq should be positive */
    return 0;
}

int recv_buf_cleanup(RECV_BUF *ptr)
{
    if (ptr == NULL) {
	return 1;
    }
    
    free(ptr->buf);
    ptr -> buf = NULL;
    ptr->size = 0;
    ptr->max_size = 0;
    return 0;
}

void cleanup(CURL *curl, RECV_BUF *ptr)
{
    curl_easy_cleanup(curl);
    recv_buf_cleanup(ptr);
}

/**
 * @brief write callback function to save a copy of received data in RAM.
 *        The received libcurl data are pointed by p_recv, 
 *        which is provided by libcurl and is not user allocated memory.
 *        The user allocated memory is at p_userdata. One needs to
 *        cast it to the proper struct to make good use of it.
 *        This function maybe invoked more than once by one invokation of
 *        curl_easy_perform().
 */

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

/**
 * @brief  cURL header call back function to extract image sequence number from 
 *         http header data. An example header for image part n (assume n = 2) is:
 *         X-Ece252-Fragment: 2
 * @param  char *p_recv: header data delivered by cURL
 * @param  size_t size size of each memb
 * @param  size_t nmemb number of memb
 * @param  void *userdata user defined data structurea
 * @return size of header data received.
 * @details this routine will be invoked multiple times by the libcurl until the full
 * header data are received.  we are only interested in the ECE252_HEADER line 
 * received so that we can extract the image sequence number from it. This
 * explains the if block in the code.
 */
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;

#ifdef DEBUG1_
    
#endif /* DEBUG1_ */
    if (realsize > strlen(ECE252_HEADER) &&
	strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0) {

        /* extract img sequence number */
	p->seq = atoi(p_recv + strlen(ECE252_HEADER));

    }
    return realsize;
}

/**
 * @brief create a curl easy handle and set the options.
 * @param RECV_BUF *ptr points to user data needed by the curl write call back function
 * @param const char *url is the target url to fetch resoruce
 * @return a valid CURL * handle upon sucess; NULL otherwise
 * Note: the caller is responsbile for cleaning the returned curl handle
 */

CURL *easy_handle_init(RECV_BUF *ptr, const char *url)
{
    CURL *curl_handle = NULL;

    if ( ptr == NULL || url == NULL) {
        return NULL;
    }

    /* init user defined call back function buffer */
    if ( recv_buf_init(ptr, BUF_SIZE) != 0 ) {
        return NULL;
    }
    /* init a curl session */
    curl_handle = curl_easy_init();

    if (curl_handle == NULL) {
        fprintf(stderr, "curl_easy_init: returned NULL\n");
        return NULL;
    }

    /* specify URL to get */
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);

    /* register write call back function to process received data */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl3); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)ptr);

    /* register header call back function to process received header data */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)ptr);

    /* some servers requires a user-agent field */
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "ece252 lab4 crawler");

    /* follow HTTP 3XX redirects */
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    /* continue to send authentication credentials when following locations */
    curl_easy_setopt(curl_handle, CURLOPT_UNRESTRICTED_AUTH, 1L);
    /* max numbre of redirects to follow sets to 5 */
    curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 5L);
    /* supports all built-in encodings */ 
    curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "");

    /* Max time in seconds that the connection phase to the server to take */
    curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 5L);
    /* Max time in seconds that libcurl transfer operation is allowed to take */
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
    /* Time out for Expect: 100-continue response in milliseconds */
    curl_easy_setopt(curl_handle, CURLOPT_EXPECT_100_TIMEOUT_MS, 0L);

    /* Enable the cookie engine without reading any initial cookies */
    curl_easy_setopt(curl_handle, CURLOPT_COOKIEFILE, "");
    /* allow whatever auth the proxy speaks */
    curl_easy_setopt(curl_handle, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
    /* allow whatever auth the server speaks */
    curl_easy_setopt(curl_handle, CURLOPT_HTTPAUTH, CURLAUTH_ANY);

    return curl_handle;
}

int process_html(CURL *curl_handle, RECV_BUF *p_recv_buf)
{

    int follow_relative_link = 1;
    char *url = NULL; 


    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &url);

    find_http(p_recv_buf->buf, p_recv_buf->size, follow_relative_link, url);

    return 0;
}

int process_png(CURL *curl_handle, RECV_BUF *p_recv_buf)
{
    pthread_mutex_lock(&frontier_mutex);
    char *eurl = NULL;          /* effective URL */

    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &eurl);

    if ( eurl != NULL) {

        int notFound = !visitedUrlFound(frontier[frontierFirst]);

        if(notFound) {
                pthread_mutex_lock(&visited_mutex);
                __sync_add_and_fetch(&visitedUrlsTop, 1);
                visitedUrls[visitedUrlsTop] = malloc(sizeof(char)*500);
                memset(visitedUrls[visitedUrlsTop], 0, sizeof(char)*500);
                if (frontier[frontierFirst] == NULL) {
                    pthread_mutex_unlock(&frontier_mutex);
                    pthread_mutex_unlock(&visited_mutex);
                    return 1;
                }
                strcpy(visitedUrls[visitedUrlsTop], frontier[frontierFirst]);
                pthread_mutex_unlock(&visited_mutex);
            }

 
        __sync_add_and_fetch( &frontierFirst, 1 );

        int isPng = 1;
        char pngSig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        for(int i = 0; i < 8; i++) {
            if((char)(p_recv_buf -> buf[i]) != pngSig[i]) {
                isPng = 0;
                break;
            }
        }

        //png file case
        if (isPng) {
            
            pthread_mutex_lock(&png_mutex);
            __sync_add_and_fetch(&pngUrlsTop, 1);
            pngUrls[pngUrlsTop] = malloc(strlen(eurl)*sizeof(char));
            memset(pngUrls[pngUrlsTop], 0, strlen(eurl)*sizeof(char));
            strcpy(pngUrls[pngUrlsTop], eurl);
            pthread_mutex_unlock(&png_mutex);
          
        }
    }
    pthread_mutex_unlock(&frontier_mutex);
    return 0;
}
/**
 * @brief process teh download data by curl
 * @param CURL *curl_handle is the curl handler
 * @param RECV_BUF p_recv_buf contains the received data. 
 * @return 0 on success; non-zero otherwise
 */

int process_data(CURL *curl_handle, RECV_BUF *p_recv_buf)
{

    CURLcode res;

    long response_code;

    //get response code to check if it's an error
    res = curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
    if ( res != CURLE_OK ) {
        printf("response code get error\n");
        return 1;
    }

    //response code: 400s or 500s => url with error
    if ( response_code >= 400 ) { 

        pthread_mutex_lock(&frontier_mutex);
        int notFound = !visitedUrlFound(frontier[frontierFirst]);
        pthread_mutex_unlock(&frontier_mutex);

        if(notFound) {
                pthread_mutex_lock(&visited_mutex);
                __sync_add_and_fetch(&visitedUrlsTop, 1);
                visitedUrls[visitedUrlsTop] = malloc(sizeof(char)*500);
                memset(visitedUrls[visitedUrlsTop], 0, (sizeof(char)*500));
                pthread_mutex_lock(&frontier_mutex);
                strcpy(visitedUrls[visitedUrlsTop], frontier[frontierFirst]);
                pthread_mutex_unlock(&frontier_mutex);
                pthread_mutex_unlock(&visited_mutex);
                __sync_add_and_fetch( &frontierFirst, 1 );
                
           
        }
        else
        __sync_add_and_fetch( &frontierFirst, 1 );

        return 1;
    }

    //get content type to check what to do with that info next
    char *ct = NULL;
    res = curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_TYPE, &ct);
    if ( !(res == CURLE_OK && ct != NULL) ) {
        fprintf(stderr, "Failed to obtain Content-Type\n");
        return 2;
    }

    //process data depending on content-type
    if ( strstr(ct, CT_HTML)) {
        return process_html(curl_handle, p_recv_buf);
    } else if ( strstr(ct, CT_PNG) ) {
        return process_png(curl_handle, p_recv_buf);
    } else {
        return process_other(curl_handle, p_recv_buf);
    }

    return 0;
}

//find_http investigates the text/html file - tells you if the text/html link is a redirect or  a plain text/html file
int find_http(char *buf, int size, int follow_relative_links, const char *base_url)
{
    
    int i;
    htmlDocPtr doc;
    xmlChar *xpath = (xmlChar*) "//a/@href";
    xmlNodeSetPtr nodeset;
    xmlXPathObjectPtr result;
    xmlChar *href;
		
    if (buf == NULL) {
        return 1;
    }

    doc = mem_getdoc(buf, size, base_url);

    //result tells you if the text/html is a redirect or plain html file
    result = getnodeset (doc, xpath);

        pthread_mutex_lock(&frontier_mutex);
        int notFound = !visitedUrlFound(frontier[frontierFirst]);
        pthread_mutex_unlock(&frontier_mutex);

        if(notFound) {

                pthread_mutex_lock(&frontier_mutex);
                pthread_mutex_lock(&visited_mutex);
                __sync_add_and_fetch(&visitedUrlsTop, 1);
                visitedUrls[visitedUrlsTop] = malloc(sizeof(char)*500);
                memset(visitedUrls[visitedUrlsTop], 0, sizeof(char)*500);

                if (frontier[frontierFirst] == NULL) {
                    pthread_mutex_unlock(&visited_mutex);
                    pthread_mutex_unlock(&frontier_mutex);
                    return 1;
                }
                strcpy(visitedUrls[visitedUrlsTop], frontier[frontierFirst]);
                pthread_mutex_unlock(&visited_mutex);
                pthread_mutex_unlock(&frontier_mutex);
        }

    __sync_add_and_fetch( &frontierFirst, 1 );

    //redirect text/html case
    if (result) {
        nodeset = result->nodesetval;

        //goes in and finds all the redirect's child links
        for (i=0; i < nodeset->nodeNr; i++) {
            //normal href - can be #top rather than http://ece252-1.uwaterloo.ca/~yqhuang/lab4/#top
            href = xmlNodeListGetString(doc, nodeset->nodeTab[i]->xmlChildrenNode, 1);

            //if it is #top, that means it's just the same page as the base_url, so we can just discard it - may have to add to hash table before adding it rather than just discarding it
            int isTopUrl = 0;
            if (!strcmp((char *)href, "#top")) {
                isTopUrl = 1;
            }

            //this is set in the beginning of this fn
            if ( follow_relative_links ) {
                xmlChar *old = href;

                //sometimes, before this step, you'll have something like #top in href; after this step, that gets appended to base_url since that's where you found it (I think) and becomes http://ece252-1.uwaterloo.ca/~yqhuang/lab4/#top
                href = xmlBuildURI(href, (xmlChar *) base_url);
                if (isTopUrl) {


                    int notFound = !visitedUrlFound((char *)href);
        

                    if(notFound) {

                            pthread_mutex_lock(&visited_mutex);
                            __sync_add_and_fetch(&visitedUrlsTop, 1);
                            visitedUrls[visitedUrlsTop] = malloc(sizeof(char)*500);
                            memset(visitedUrls[visitedUrlsTop], 0, sizeof(char)*500);
                            strcpy(visitedUrls[visitedUrlsTop], (char *) href);
                
                            pthread_mutex_unlock(&visited_mutex);

                    }
                    xmlFree(old);
                    xmlFree(href);
                    continue;
                }

                //javascript-alert case
                if (href == NULL) {
                
                    int notFound = !visitedUrlFound((char *)old);
              

                        if(notFound) {
                                pthread_mutex_lock(&visited_mutex);
                                __sync_add_and_fetch(&visitedUrlsTop, 1);
                                visitedUrls[visitedUrlsTop] = malloc(sizeof(char)*500);
                                memset(visitedUrls[visitedUrlsTop], 0, sizeof(char)*500);
                                strcpy(visitedUrls[visitedUrlsTop], (char *)old);                               
                                pthread_mutex_unlock(&visited_mutex);
                     
                        }
                        
                    xmlFree(href);
                    xmlFree(old);
                    continue;
                }
                xmlFree(old);
            }

            if ( href != NULL && !strncmp((const char *)href, "http", 4) ) {

                int notFound = !visitedUrlFound((char *)href);
             

                if(notFound) {
                    pthread_mutex_lock(&frontier_mutex);
                    __sync_add_and_fetch( &frontierTop, 1 );
                    frontier[frontierTop] = malloc(sizeof(char)*500);
                    memset(frontier[frontierTop], 0, sizeof(char)*500);
                    strcpy(frontier[frontierTop], (char *)href);
                    fflush(stdout);
                    pthread_mutex_unlock(&frontier_mutex);
                }
            }

            //mail-to link case
            else {
          
                int notFound = !visitedUrlFound((char *)href);


                if(notFound) {
             
                        pthread_mutex_lock(&visited_mutex);
                        __sync_add_and_fetch(&visitedUrlsTop, 1);
                        visitedUrls[visitedUrlsTop] = malloc(sizeof(char)*500);
                        memset(visitedUrls[visitedUrlsTop], 0, sizeof(char)*500);
                        strcpy(visitedUrls[visitedUrlsTop], (char *)href);
                
                        pthread_mutex_unlock(&visited_mutex);
                 
                }
            }
            xmlFree(href);
        }
        xmlXPathFreeObject (result);
    }

    xmlFreeDoc(doc);
    return 0;
}

htmlDocPtr mem_getdoc(char *buf, int size, const char *url)
{
    int opts = HTML_PARSE_NOBLANKS | HTML_PARSE_NOERROR | \
               HTML_PARSE_NOWARNING | HTML_PARSE_NONET;
    htmlDocPtr doc = htmlReadMemory(buf, size, url, NULL, opts);
    
    if ( doc == NULL ) {
      
        return NULL;
    }
    return doc;
}

xmlXPathObjectPtr getnodeset (xmlDocPtr doc, xmlChar *xpath)
{
	
    xmlXPathContextPtr context;
    xmlXPathObjectPtr result;

    context = xmlXPathNewContext(doc);
    if (context == NULL) {
        printf("Error in xmlXPathNewContext\n");
        return NULL;
    }
    result = xmlXPathEvalExpression(xpath, context);
    xmlXPathFreeContext(context);
    if (result == NULL) {
        printf("Error in xmlXPathEvalExpression\n");
        return NULL;
    }

    //returns NULL if there base_url is a plain text/html file (not redirect)
    if(xmlXPathNodeSetIsEmpty(result->nodesetval)){
        xmlXPathFreeObject(result);
   
        return NULL;
    }
    return result;
}


int main(int argc, char **argv) {

    //init variables for argument extractions
    int c;
    threads = 0;
    int pngs = 0;
    xmlInitParser();
   
    char *logFileName;
    
    int v_arg = 0;
    char *str = "option requires an argument";
    char *seedUrl;

    //get command line args
    while ((c = getopt (argc, argv, "t:m:v:")) != -1) {
        switch (c) {
        case 't':
	    threads = strtoul(optarg, NULL, 10);
	    if (threads <= 0) {
                fprintf(stderr, "%s: %s > 0 -- 't'\n", argv[0], str);
                return -1;
            }
            break;
        case 'm':
            pngs = strtoul(optarg, NULL, 10);
            if (pngs <= 0) {
                fprintf(stderr, "%s: %s greater than 0 -- 'n'\n", argv[0], str);
                return -1;
            }
            break;

        case 'v':
            v_arg = 1;
            logFileName = optarg;
            break;

        default:
            return -1;
        }
    }

        seedUrl = argv[(argc - 1)];


    if (threads < 0) {
        printf("Incorrect threads argument\n");
        return -1;
    }

    if (pngs < 0) {
        printf("Incorrect NUM argument\n");
        return -1;
    }

    frontier = malloc(sizeof(char *)*MAX_URLS);
    frontierFirst = -1;//front
    frontierTop = -1;//rear
    pngUrls = malloc(sizeof(char *)*MAX_URLS);
    if(threads > 2) threads = MULTI_THREAD;
    pngUrlsTop = -1;
    visitedUrls = malloc(sizeof(char *)*2000);
    visitedUrlsTop = -1;
    __sync_add_and_fetch( &frontierFirst, 1 );
    __sync_add_and_fetch( &frontierTop, 1 );

    frontier[0] = malloc(500*sizeof(char));
    memset(frontier[0], 0, 500*sizeof(char));
    memcpy(frontier[0], seedUrl, strlen(seedUrl));

    //start timer
    double times[2];
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[0] = (tv.tv_sec) + (tv.tv_usec/1000000.);

    struct thread_args in_params[threads]; //don't know what input parameters to send
    pthread_t *p_tids = malloc(sizeof(pthread_t) * threads);
    if(p_tids == NULL)
        return -1;

    pthread_mutex_init(&png_mutex, NULL);
    pthread_cond_init (&threshold_cv, NULL );
    pthread_mutex_init(&frontier_mutex, NULL);
    pthread_mutex_init(&visited_mutex, NULL);

    //creating threads
    for(int i=0; i<threads;i++)
    {
        //initializing input params
        in_params[i].thread_num = i;
        in_params[i].pngs = pngs;
        
       pthread_create((p_tids + i), NULL, do_work, in_params + i);
    }

    //joining threads
    
    for (int i = 0; i < threads; i++) {
        //Thread ID %lu joined
        pthread_join(p_tids[i], NULL);
    }

    free(p_tids);
    p_tids = NULL;

    // write data to png_urls.txt
    FILE *f1 = fopen("png_urls.txt", "wb");
    if (pngUrlsTop != -1) {
    
        for(int i = 0; i <= pngUrlsTop; i++) {
               
                fwrite(pngUrls[i], 1, strlen(pngUrls[i]), f1);
                fwrite("\n", sizeof(char), 1, f1);
            
        }   
    }

    fclose(f1);

    if(v_arg) {
        FILE *f2 = fopen(logFileName, "wb");
        if (visitedUrlsTop != -1) {
 
            for (int i = 0; i <= visitedUrlsTop; i++) {

                    fwrite(visitedUrls[i], 1, strlen(visitedUrls[i]), f2);
                    fwrite("\n", sizeof(char), 1, f2);
            }


        }
        //check if all png urls are unique
        fclose(f2);
    }  

    //print time to console
    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[1] = (tv.tv_sec) + (tv.tv_usec/1000000.);
      
    printf("findpng2 execution time: %.6lf seconds\n", times[1] - times[0]);

    for (int i = 0; i <= frontierTop; i++) {
        free(frontier[i]);
        frontier[i] = NULL;
    }

     for (int i = 0; i <= visitedUrlsTop; i++) {
        free(visitedUrls[i]);
        visitedUrls[i] = NULL;
    }

     for (int i = 0; i <= pngUrlsTop; i++) {
        free(pngUrls[i]);
        pngUrls[i] = NULL;
    }

    pthread_mutex_destroy(&png_mutex);
    pthread_cond_destroy(&threshold_cv);
    pthread_mutex_destroy(&frontier_mutex);
    pthread_mutex_destroy(&visited_mutex);
    xmlCleanupParser();
    
    return 0;
}