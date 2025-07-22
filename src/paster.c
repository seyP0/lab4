#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <curl/curl.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <getopt.h>
#include <zlib.h>
#include "lab_png.h"

#define NUM_STRIPS 50
#define NUM_SERVERS 3

const char *servers[NUM_SERVERS] = {
    "http://ece252-1.uwaterloo.ca:2520/image?img=%d",
    "http://ece252-2.uwaterloo.ca:2520/image?img=%d",
    "http://ece252-3.uwaterloo.ca:2520/image?img=%d"
};

// Struct to hold strip data 
typedef struct {
    unsigned char *data[NUM_STRIPS]; // raw PNG bytes per strip
    size_t sizes[NUM_STRIPS]; // size of each buffer
    int received[NUM_STRIPS]; // flags when strip is saved
    pthread_mutex_t lock; // guards all fields below
    int image_num; // 1-3, chosen by user in command-line
    int downloaded; // count of unique stripts fetched
    int current_header_strip;  // current strip number parsed in header_cb

} shared_data_t;

// Header callback to extract strip number from X-Ece252-Fragment <0-49> value
size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t total = size * nitems;
    shared_data_t *shared = (shared_data_t *)userdata;

    if (strncasecmp(buffer, "X-Ece252-Fragment:", 18) == 0) { 
        int strip_no = atoi(buffer + 18);
        shared->current_header_strip = strip_no;  // store the called each header line 
    }

    return total;
}


// Write callback to collect image strip data
// called for each chunk of body data
size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    shared_data_t *shared = (shared_data_t *)userdata;
    size_t total = size * nmemb;

    int strip_no = shared->current_header_strip;

    pthread_mutex_lock(&shared->lock);
    if (!shared->received[strip_no]) {
        shared->data[strip_no] = malloc(total);
        memcpy(shared->data[strip_no], ptr, total); // allocates and copies exactly one buffer per strip
        shared->sizes[strip_no] = total;

        shared->received[strip_no] = 1;
        shared->downloaded++; 
    }
    pthread_mutex_unlock(&shared->lock);
    return total;
}

// continuoutly fetch image strips until all 50 are collected
void *thread_fn(void *arg) {
    shared_data_t *shared = (shared_data_t *)arg;
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to init curl\n");
        pthread_exit(NULL);
    }

    char url[256];
    while (1) {
        pthread_mutex_lock(&shared->lock);
        if (shared->downloaded >= NUM_STRIPS) {
            pthread_mutex_unlock(&shared->lock);
            break;
        }
        pthread_mutex_unlock(&shared->lock);

        // randomly select a server URL to request from
        int server_idx = rand() % NUM_SERVERS;
        snprintf(url, sizeof(url), servers[server_idx], shared->image_num);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, shared);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, shared);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform failed: %s\n", curl_easy_strerror(res));
        }
    }

    curl_easy_cleanup(curl);
    return NULL;
}


int decompress_strip(const unsigned char *png_data, size_t size, unsigned char *out_buffer) {
    simple_PNG_p strip = mallocPNG();
    FILE *fp = fmemopen((void *)png_data, size, "rb");
    if (!fp || !get_png_chunks(strip, fp, 0, SEEK_SET)) {
        fprintf(stderr, "Failed to parse PNG strip.\n");
        return 0;
    }

    uLongf out_len = 400 * 6 + 6; // data size = 6 scamlines + 6 filter bytes
    int res = uncompress(out_buffer, &out_len, strip->p_IDAT->p_data, strip->p_IDAT->length);
    if (res != Z_OK) {
        fprintf(stderr, "uncompressed() failed: %d\n", res);
        return 0;
    }

    fclose(fp);
    free_png(strip);
    return 1;

}



int main(int argc, char *argv[]) {
    int num_threads = 1;
    int image_num = 1;
    int opt;
    char *str = "option requires an argument";

    // Parse command-line arguments
    while ((opt = getopt(argc, argv, "t:n:")) != -1) {

        switch (opt) {
            case 't': // number of threads
                num_threads = strtoul(optarg, NULL, 10);
                printf("option -t specifies a value of %d.\n", num_threads);
                if (num_threads <= 0) {
                        fprintf(stderr, "%s: %s > 0 -- 't'\n", argv[0], str);
                        return EXIT_FAILURE;
                }
                break;
            case 'n': // image number
                image_num = strtoul(optarg, NULL, 10);
                printf("option -n specifies a value of %d.\n", image_num);
                if (image_num <= 0 || image_num > 3) {
                    fprintf(stderr, "%s: %s 1, 2, or 3 -- 'n'\n", argv[0], str);
                    return EXIT_FAILURE;
                }
                break;
            default:
                return EXIT_FAILURE;
        }
    }

    // Initialize shared start for threads
    shared_data_t shared = {0};
    pthread_mutex_init(&shared.lock, NULL);
    shared.image_num = image_num;

    curl_global_init(CURL_GLOBAL_ALL);
    pthread_t threads[num_threads];
    srand(time(NULL));

    // start all threads
    for (int i = 0; i < num_threads; ++i) {
        pthread_create(&threads[i], NULL, thread_fn, &shared);
    }

    // wait for all threads to finish
    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], NULL);
    }

    // save the final assembled image to all.png
    if (save_png_from_memstrips((const unsigned char **)shared.data, shared.sizes, "all.png")) {
        
        fprintf(stderr, "Failed to save all.png\n");
    } else {
        printf("Saved all.png successfully\n");
    }
   
    // cleanup
    curl_global_cleanup();
    pthread_mutex_destroy(&shared.lock);

    printf("Downloaded all 50 strips.\n");
    return 0;
}
