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

#define MAX_URLS 1000
#define BUF_SIZE 1048576  /* 1024*1024 = 1M */
#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_INC  524288   /* 1024*512  = 0.5M */
#define DEFAULT_URL "http://ece252-1.uwaterloo.ca/lab4"
#define LIMIT_URLS 500
#define MAX_PNGS 50

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

typedef struct {
    RECV_BUF buf;
    char *url;
} TEST;

int frontierFirst;  //head of the queue
int frontierTop;    //tail of the queue

int pngUrlsTop;
int visitedUrlsTop;

int connections;

extern char *optarg;

int frontier_is_empty();

htmlDocPtr mem_getdoc(char *buf, int size, const char *url);
xmlXPathObjectPtr getnodeset (xmlDocPtr doc, xmlChar *xpath);
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);
size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata);

int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);
void cleanup(CURL *curl, RECV_BUF *ptr);

static void easy_handle_init(CURLM *cm,RECV_BUF *ptr, const char *url);

int visitedUrlFound(char *url, char **visitedUrls);

int process_data(CURL *curlHandle, char *url, RECV_BUF buf, char **frontier, char **pngUrls, char **visitedUrls);
int process_html(CURL *eh, RECV_BUF *p_recv_buf, char* url, char **frontier, char **visitedUrls);
int process_png(CURL *eh, RECV_BUF *p_recv_buf, char* url, char **pngUrls, char **visitedUrls);
int process_other(CURL *eh, RECV_BUF *p_recv_buf, char* url, char **visitedUrls);
int find_http(char *fname, int size, int follow_relative_links, char *base_url, char **frontier, char **visitedUrls);

int frontier_is_empty(){

    if(frontierFirst > frontierTop)
        return 1;

    return 0;
}

int recv_buf_init(RECV_BUF *ptr, size_t max_size)
{
    void *p = NULL;
    
    if (ptr == NULL) {
        return 1;
    }

    p = malloc(max_size);
    memset(p, 0, max_size);

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

size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;

#ifdef DEBUG1_
    printf("%s", p_recv);
#endif /* DEBUG1_ */
    if (realsize > strlen(ECE252_HEADER) &&
	strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0) {

        /* extract img sequence number */
	p->seq = atoi(p_recv + strlen(ECE252_HEADER));

    }
    return realsize;
}

static void easy_handle_init(CURLM *cm, RECV_BUF *ptr, const char *url)
{

    if ( ptr == NULL || url == NULL) {
        return;
    }
    /* init user defined call back function buffer */
    if ( recv_buf_init(ptr, BUF_SIZE) != 0 ) {
        return;
    }

    /* init a curl session */
    CURL *eh = curl_easy_init();//here

    if (eh == NULL) {
        fprintf(stderr, "curl_easy_init: returned NULL\n");
        return;
    }

    /* specify URL to get */
    curl_easy_setopt(eh, CURLOPT_URL, url);

    /* register write call back function to process received data */
    curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, write_cb_curl3); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(eh, CURLOPT_WRITEDATA, (void *)ptr);

    /* register header call back function to process received header data */
    curl_easy_setopt(eh, CURLOPT_HEADERFUNCTION, header_cb_curl); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(eh, CURLOPT_HEADERDATA, (void *)ptr);

    /* some servers requires a user-agent field */
    curl_easy_setopt(eh, CURLOPT_USERAGENT, "ece252 lab4 crawler");

    /* follow HTTP 3XX redirects */
    curl_easy_setopt(eh, CURLOPT_FOLLOWLOCATION, 1L);
    /* continue to send authentication credentials when following locations */
    curl_easy_setopt(eh, CURLOPT_UNRESTRICTED_AUTH, 1L);
    /* max numbre of redirects to follow sets to 5 */
    curl_easy_setopt(eh, CURLOPT_MAXREDIRS, 5L);
    /* supports all built-in encodings */ 
    curl_easy_setopt(eh, CURLOPT_ACCEPT_ENCODING, "");

    /* Max time in seconds that the connection phase to the server to take */
    curl_easy_setopt(eh, CURLOPT_CONNECTTIMEOUT, 5L);
    /* Max time in seconds that libcurl transfer operation is allowed to take */
    curl_easy_setopt(eh, CURLOPT_TIMEOUT, 10L);
    /* Time out for Expect: 100-continue response in milliseconds */
    curl_easy_setopt(eh, CURLOPT_EXPECT_100_TIMEOUT_MS, 0L);

    /* Enable the cookie engine without reading any initial cookies */
    curl_easy_setopt(eh, CURLOPT_COOKIEFILE, "");
    /* allow whatever auth the proxy speaks */
    curl_easy_setopt(eh, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
    /* allow whatever auth the server speaks */
    curl_easy_setopt(eh, CURLOPT_HTTPAUTH, CURLAUTH_ANY);

    curl_easy_setopt(eh, CURLOPT_PRIVATE, (void *)ptr);

    curl_easy_setopt(eh, CURLOPT_VERBOSE, 0L);

    curl_multi_add_handle(cm, eh);

    return;
}

int visitedUrlFound(char *url, char **visitedUrls) {
    //should never occur if the breaking condition, i.e. the barrier is doing it's job
    if (url == NULL) {
        return 0;
    }

    for (int i = 0; i <= visitedUrlsTop; i++) {
        if((strlen(url) == strlen(visitedUrls[i])) && (!strncmp(visitedUrls[i], url, strlen(url)))){
            return 1;
        }
    }
    return 0;
}

int process_html(CURL *eh, RECV_BUF *p_recv_buf, char* url, char **frontier, char **visitedUrls)
{
    int follow_relative_link = 1;
    char *func_url = NULL; 

    //not sure what the difference between frontier[frontierTop] (base url we're looking at) and this effective url is - double checked on gdb and they're the same
    curl_easy_getinfo(eh, CURLINFO_EFFECTIVE_URL, &func_url);

    find_http(p_recv_buf->buf, p_recv_buf->size, follow_relative_link, func_url, frontier, visitedUrls);

    return 0;
}

int pngFound(char *url, char **pngUrls) {
    for (int i = 0; i < pngUrlsTop; i++) {
        if(!strcmp(pngUrls[i], url)){
            return 1;
        }
    }
    return 0;
}

int process_png(CURL *eh, RECV_BUF *p_recv_buf, char* url, char **pngUrls, char **visitedUrls)
{
    char *eurl = NULL;          /* effective URL */
    int pngNotFound = !visitedUrlFound(url, visitedUrls);
    if (pngNotFound) {

        //again, don't know how effective_url is different from frontier[frontierTop]
        curl_easy_getinfo(eh, CURLINFO_EFFECTIVE_URL, &eurl);
        if ( eurl != NULL) {

            //check if item is png - got from is_png in lab1-pre pnginfo.c
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
                pngUrlsTop++;
                strcpy(pngUrls[pngUrlsTop], eurl);
            }

            //all img/png cases
            int notFound = !visitedUrlFound(eurl, visitedUrls);

            if(notFound) {
                visitedUrlsTop++;
                strcpy(visitedUrls[visitedUrlsTop], eurl);
            }
        }
    }

    return 0;
}

int process_other(CURL *eh, RECV_BUF *p_recv_buf, char* url, char **visitedUrls) {
    
    int notFound = !visitedUrlFound(url, visitedUrls);

    if(notFound) {
        visitedUrlsTop++;
        strcpy(visitedUrls[visitedUrlsTop], url);
    }
    return 0;    
}

int process_data(CURL *curlHandle, char *url, RECV_BUF buf, char **frontier, char **pngUrls, char **visitedUrls)
{
    CURLcode res;
    long response_code;

    //get response code to check if it's an error
    res = curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &response_code);

    if ( response_code >= 400 ) {
        int notFound = !visitedUrlFound(url, visitedUrls);
        if(notFound) {
            visitedUrlsTop++;
            strcpy(visitedUrls[visitedUrlsTop], url);
        }

        return 1;
    }

    char *ct = NULL;
    res = curl_easy_getinfo(curlHandle, CURLINFO_CONTENT_TYPE, &ct);
    if ( res != CURLE_OK || ct == NULL ) {
        printf("failed url: %s\n", url);
        fprintf(stderr, "Failed to obtain Content-Type\n");
        return 2;
    }

    //process data depending on content-type
    if ( strstr(ct, CT_HTML)) {
        return process_html(curlHandle, &buf, url, frontier, visitedUrls);
    } else if ( strstr(ct, CT_PNG) ) {
        return process_png(curlHandle, &buf, url, pngUrls, visitedUrls);
    }
    else {
        return process_other(curlHandle, &buf, url, visitedUrls);
    }
    return 0;
}

//find_http investigates the text/html file - tells you if the text/html link is a redirect or  a plain text/html file
int find_http(char *buf, int size, int follow_relative_links, char *base_url, char **frontier, char **visitedUrls)
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

                href = xmlBuildURI(href, (xmlChar *) base_url);
                if (isTopUrl) {
                    int notFound = !visitedUrlFound((char *)href, visitedUrls);

                    if(notFound) {
                        visitedUrlsTop++;
                        strcpy(visitedUrls[visitedUrlsTop], (char *) href);
                    }
                    xmlFree(old);
                    xmlFree(href);
                    continue;
                }

                //javascript-alert case
                //add old to hash if it isn't already in hash so you never have to deal with any related cURL problems with it in the future (since it's not in http format and is kinda sketch)
                if (href == NULL) {
                    int notFound = !visitedUrlFound((char *)old, visitedUrls);
                    if(notFound) {
                        visitedUrlsTop++;
                        strcpy(visitedUrls[visitedUrlsTop], (char *)old);
                    }
                    xmlFree(href);
                    xmlFree(old);
                    continue;
                }
                xmlFree(old);
            }

            if ( href != NULL && !strncmp((const char *)href, "http", 4) ) {

                int notFound = !visitedUrlFound((char *)href, visitedUrls);
                if (!strcmp((char *)href, "http://ece252-1.uwaterloo.ca/lab4/") || !strcmp((char *)href, "http://ece252-1.uwaterloo.ca/~yqhuang/lab4/")) {
                    printf("stop\n");
                }

                if(notFound) {
                    frontierTop++;
                    strcpy(frontier[frontierTop], (char *)href);
                }
            }

            //mail-to link case
            else {
                int notFound = !visitedUrlFound((char *)href, visitedUrls);

                if(notFound) {
                    visitedUrlsTop++;
                    strcpy(visitedUrls[visitedUrlsTop], (char *)href);
                }
            }
            xmlFree(href);
        }
        xmlXPathFreeObject (result);
    }

    //shift current frontier url into already-seen urls hash
    //plain text/html case is handled here
    //if result returns such that there are no hrefs to follow, just add current text/html link like usual to hash and proceed
    int notFound = !visitedUrlFound(base_url, visitedUrls);

    if(notFound) {
        visitedUrlsTop++;
        strcpy(visitedUrls[visitedUrlsTop], base_url);
    }

    xmlFreeDoc(doc);
    xmlCleanupParser();
    return 0;
}

htmlDocPtr mem_getdoc(char *buf, int size, const char *url)
{
    int opts = HTML_PARSE_NOBLANKS | HTML_PARSE_NOERROR |
               HTML_PARSE_NOWARNING | HTML_PARSE_NONET;
    htmlDocPtr doc = htmlReadMemory(buf, size, url, NULL, opts);
    
    if ( doc == NULL ) {
        fprintf(stderr, "Document not parsed successfully.\n");
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
    int connections = 0;
    int pngs = 0;
    int v_arg = 0;
    char *logFileName;
    char *str = "option requires an argument";
    char *seedUrl;
    xmlInitParser();

    seedUrl = argv[(argc - 1)];

    //get command line args
    while ((c = getopt (argc, argv, "t:m:v:")) != -1) {
        switch (c) {
        case 't':
            connections = strtoul(optarg, NULL, 10);
            if (connections <= 0) {
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

    if (connections < 0) {
        printf("Incorrect connections argument\n");
        return -1;
    }

    if (pngs < 0) {
        printf("Incorrect NUM argument\n");
        return -1;
    }

    if (connections == 0) 
        connections = 1;    //assume that there is a single connection
    if (pngs == 0)
        pngs = 50;      //given in lab manual

    if(pngs > MAX_PNGS)
        pngs = MAX_PNGS;

    char **frontier = malloc(sizeof(char *)*4000);
    for (int i = 0; i < 4000; i++) {
        frontier[i] = malloc(sizeof(char)*500);
        memset(frontier[i], 0, sizeof(char)*500);
    }
    frontierFirst = -1;//front
    frontierTop = -1;//rear

    char **pngUrls = malloc(sizeof(char *)*1000);
    for (int i = 0; i < 1000; i++) {
        pngUrls[i] = malloc(sizeof(char)*500);
        memset(pngUrls[i], 0, sizeof(char)*500);
    }
    pngUrlsTop = -1;

    char **visitedUrls = malloc(sizeof(char *)*500);
    for (int i = 0; i < 500; i++) {
        visitedUrls[i] = malloc(sizeof(char)*500);
        memset(visitedUrls[i], 0, sizeof(char)*500); 
    }
    visitedUrlsTop = -1;

    strcpy(frontier[0], seedUrl);
    frontierFirst++;
    frontierTop++;

    //start timer
    double times[2];
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[0] = (tv.tv_sec) + (tv.tv_usec/1000000.);

    //init cURL stuff
    CURLM *cm = NULL;

    CURL *eh = NULL;
    TEST test[connections];

    CURLMsg *msg = NULL;
    CURLcode res = 0;
    int still_running = 0;
    int msgs_left = 0;
    
    curl_global_init(CURL_GLOBAL_ALL);
    cm = curl_multi_init();

    int counter = 0;
    
    while(!(pngUrlsTop == (pngs - 1)) && !(frontier_is_empty() && !still_running)) {

        counter = 0;


        while(!frontier_is_empty() && (counter < connections)) {

            test[counter].url = malloc(sizeof(char)*500);
            memset(test[counter].url, 0, 500*sizeof(char));
            strcpy(test[counter].url, frontier[frontierFirst]);
            frontierFirst++;

            easy_handle_init(cm, &(test[counter].buf), test[counter].url);
       
            counter++;
            
        }
        
        curl_multi_perform(cm, &still_running);//here
        do {
            curl_multi_perform(cm, &still_running);
        } while(still_running);

        while((msg = curl_multi_info_read(cm, &msgs_left))) {
            if ( msg->msg == CURLMSG_DONE ) {
                eh = msg->easy_handle;
                res = msg->data.result;
                
                char *urlCheck;
                curl_easy_getinfo(eh, CURLINFO_EFFECTIVE_URL, &urlCheck);
                
                //broken link
                if ( res != CURLE_OK ) {

                    int notFound = !visitedUrlFound(urlCheck, visitedUrls);

                    if (notFound) {
                        visitedUrlsTop++;
                        strcpy(visitedUrls[visitedUrlsTop], urlCheck);
                    }

                    curl_multi_remove_handle(cm, eh);
                    curl_easy_cleanup(eh);
                    continue;
                }

                //good link
                else {

                    //find matching buffer
                    int alert = -1;
                    for (int i = 0; i < counter; i++) {
                        if (strlen(urlCheck) != 142 && strlen(test[i].url) != 142) {
                            int effectiveFormatUrlCheck = !strncmp(urlCheck, "http://ece252-1.uwaterloo.ca/~yqhuang/lab4/", 43);
                            int effectiveFormatTestUrl = !strncmp(test[i].url, "http://ece252-1.uwaterloo.ca/~yqhuang/lab4/", 43);

                            //makes both urlcheck and test[i].url the same format
                            if (effectiveFormatUrlCheck && !effectiveFormatTestUrl) {
                                sprintf(urlCheck, "%s%s", "http://ece252-1.uwaterloo.ca/lab4/", urlCheck + 43);
                            }
                            else if (!effectiveFormatUrlCheck && effectiveFormatTestUrl) {
                                sprintf(urlCheck, "%s%s", "http://ece252-1.uwaterloo.ca/~yqhuang/lab4/", urlCheck + 33);
                            }
                        }

                        if (!strcmp(test[i].url, "http://ece252-1.uwaterloo.ca/lab4")) {
                            strcat(test[i].url, "/");
                        }
                        if (!strcmp(urlCheck, test[i].url)) {
                            alert = i;
                            break;
                        }
                    }

                    if (alert < 0 || alert >= counter) {
                        printf("wrong alert value!\n");
                        return -1;
                    }

                    process_data(eh, test[alert].url, test[alert].buf, frontier, pngUrls, visitedUrls);

                    curl_multi_remove_handle(cm, eh);
                    curl_easy_cleanup(eh);
                }
            }
        }
        for (int i = 0; i < counter; i++) {
            free(test[i].url);
            test[i].url = NULL;
            recv_buf_cleanup(&(test[i].buf));
        }
    }


    curl_multi_cleanup(cm);
    curl_global_cleanup();

    
    if(pngUrlsTop > (pngs-1))
        pngUrlsTop = (pngs-1);

    if(visitedUrlsTop > LIMIT_URLS)
        visitedUrlsTop = LIMIT_URLS;

    //write data to png_urls.txt
    FILE *f1 = fopen("png_urls.txt", "wb");
    if (pngUrlsTop != -1) {
        for (int i = 0; i <= pngUrlsTop; i++) {
            fwrite(pngUrls[i], 1, strlen(pngUrls[i]), f1);
            fwrite("\n", sizeof(char), 1, f1);
        }
    }
    fclose(f1);

    //write data to log file if asked for
    if(v_arg) {
        FILE *f2 = fopen(logFileName, "wb");
        if (visitedUrlsTop != -1) {
            for (int i = 0; i <= visitedUrlsTop; i++) {
                fwrite(visitedUrls[i], 1, strlen(visitedUrls[i]), f2);
                fwrite("\n", sizeof(char), 1, f2);
            }
        }
        fclose(f2);
    }  

    //print time to console
    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[1] = (tv.tv_sec) + (tv.tv_usec/1000000.);
      
    printf("findpng2 execution time: %.6lf seconds\n", times[1] - times[0]);

    //clean up
    for (int i = 0; i < 4000; i++) {
        free(frontier[i]);
        frontier[i] = NULL;
    }
    free(frontier);
    frontier = NULL;
    for (int i = 0; i < 1000; i++) {
        free(pngUrls[i]);
        pngUrls[i] = NULL;
    }
    free(pngUrls);
    pngUrls = NULL;
    for (int i = 0; i < 500; i++) {
        free(visitedUrls[i]);
        visitedUrls[i] = NULL;
    }

    free(visitedUrls);
    visitedUrls = NULL;

    return 0;
}