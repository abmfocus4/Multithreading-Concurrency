# PROJECT DESCRIPTIONS

## 1. PNG Image Retrieval and Concatenation
- Concepts mastered: Multi-threaded Programming with Blocking I/O

- Context: A set of horizontal strips of a whole PNG image file are stored on the web unsorted. The web server sleeps for a random amount of time and then sends out a random strip of an image in a PNG format. For simplicity assume the PNG image segment consists of one IDAT chunk, one IEND chunk
![](PNG$20format.JPG)
The webserver is built to use HTTP response header that includes the sequence number of the image to tell you which strip is sends

- Objective: The objective is to request all horizontal strips of a picture from the server and
then concatenate these strips to restore the original picture. Because every time the
server sends a random strip, if you use a loop to keep requesting a random strip
from a server, you may receive the same strip multiple times before you receive all
the fifty distinct strips. Due to the randomness, it will take a variable amount of time
to get all the strips you need to restore the original picture

- Requirements: The pthreads library is used to design and implement a threaded program to request
all image segments from a web server by using blocking I/O and concatenate these
segments together to form the whole image

- Things to note:

> Libcurl is thread safe but there are a few exceptions. The man page of libcurlthread (see https://curl.haxx.se/libcurl/c/threadsafe.html) is the ultimate reference.
We re-iterate key points from libcurl manual that are relevant to this lab as
follows:
(1) The same libcurl handle should not be shared in multiple threads.
(2) The libcurl is thread safe but does not have internal thread synchronization
mechanism. You will need to take care of the thread synchronization.

> The pthread API
The pthreads man page gives an overview of POSIX threads. The SEE ALSO section near the bottom of the man page lists functions in the API. The man pages of pthread_create, pthread_join and pthread_exit
provide detailed information of how to create, join and terminate a thread.

> The pthread Memory Leak Bug
There is a known memory leak bug related to pthread_exit(). Please refer to
https://bugzilla.redhat.com/show bug.cgi?id=483821 for details. Using return()
instead of pthread_exit() will avoid the memory leak bug.

---------------------------------------------------------------------------------

## 2. The Producer Consumer Problem
Continuation of the problem faced in project 1 and optimie function by using the producer-consumer Problem as inspiration

- The producers will make requests to the lab web server and together they will
fetch all 50 distinct image segments. Each time an image segment arrives, it gets
placed into a fixed-size buffer of size B, shared with the consumer tasks. When
there are B image segments in the buffer, producers stop producing. When all 50
distinct image segments have been downloaded from the server, all producers will
terminate. That is the buffer can take maximum B items, where each item is an
image segment. The horizontal image strips sent out by the lab servers are all less than 10,000 bytes

- Each consumer reads image segments out of the buffer, one at a time, and then
sleeps for X milliseconds specified by the user in the command line4. Then the consumer will process the received data. The main work is to validate the received
image segment and then inflate the received IDAT data and copy the inflated data into a proper place inside the memory

- Given that the buffer has a fixed size, B, and assuming that B < 50, it is possible for the producers to have produced enough image segments that the buffer is filled before any consumer has read any data. If this happens, the producer is blocked, and must wait till there is at least one free spot in the buffer

- Similarly, it is possible for the consumers to read all of the data from the buffer, and yet more data is expected from the producers. In such a case, the consumer is blocked, and must wait for the producers to deposit one or more additional image segments into the buffer.

- Further, if any given producer or consumer is using the buffer, all other consumers and producers must wait, pending that usage being finished. That is, all
access to the buffer represents a critical section, and must be protected as such

- The program terminates when it finishes outputting the concatenated image segments in the order of the image segment sequence number to a file  named all.png

> Expected Challenge: Multiple producers are
writing to the buffer, thus a mechanism needs to be established to determine whether or not some producer has placed the last image segment into the buffer. Similarly, multiple consumers are reading from the buffer, thus a mechanism needs to be established to determine whether or not some consumer has read out the last image segment from the buffer

---------------------------------------------------------------------

## Multi-threaded Web Crawler
- Inspiration: Sharing memory between threads is alot more easier since they live in the same address space. Hence, we do not need the operating systems's involvement to have a shared memory region between them. In addition, creating/ destroying threads is less expensive than creating/terminating child processes

- New Concepts untilized: semaphore, conditional variable and atomic type facilities to synchronize threads

- Problem: In addition to the previous problem we now have to search some HTTP servers to find the URLs that contain the image strips. We have 50 different URLs each of which links to a unique PNG image segment of a particular image. The mission is to search for these URLs on the servers.

- Idea: Build a simplified web crawler that searches the web by starting from a seed URL. The crawler visits the given URL page and finds two pieces of information.
> The first piece is the URLs that link to valid PNG images. The crawler adds PNG URLs found to a search result table. We want this table to contain unique URLs,hence if the found URL is already in the table, you should not add it to the table.
> The second piece is a set of new URLs to further crawl. The crawler adds this set of new URLs to a URLs pool known as the URLs frontier . Since visiting web pages has costs, we do not want the crawler to visit the same page twice. Hence the crawler needs a mechanism to remember URLs that have been visited already. As the crawler visits the URLs in the URLs frontier, the process of finding the target PNG URLs and new URLs to further explore repeats until it finds no more new PNG URL.

------------------------------------------------------------------

## Single-threaded web crawler
Inspiration: To solve the previous problem, a multi-threaded concurrent web-crawler was created using I/O with cURL in each thread. Now, the plan is to used non-blocking I/O known as asynchronous I/O. The cURL multi interface enables multiple simultaneous transfers in the same thread.
