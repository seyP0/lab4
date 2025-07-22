#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <curl/curl.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/time.h>
#include <ctype.h>
#include <search.h>  // For hash table functions

// Constants
#define URL_MAX_LEN 2048
#define PNG_SIG_LEN 8
#define INITIAL_LIST_CAPACITY 10000
#define HASH_TABLE_SIZE 100000  // Large hash table for better performance
static const unsigned char PNG_SIGNATURE[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

// Content types
enum ContentType { CONTENT_UNKNOWN, CONTENT_HTML, CONTENT_PNG };

// URL List structure (dynamic array)
typedef struct {
    char **urls;
    unsigned count;
    unsigned capacity;
} url_list_t;

// Global variables
int T = 1;              // Number of threads
int M = 50;             // Max PNGs to find
char *start_url = NULL; // Seed URL
char *log_file = NULL;  // Log file name (optional)
FILE *log_fp = NULL;    // Log file pointer
FILE *png_urls_fp = NULL; // PNG URLs file pointer
volatile int png_count = 0;    // Total PNGs found
volatile int should_exit = 0;  // Flag to signal threads to exit
volatile int active_threads = 0; // Track active threads
pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t frontier_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t visited_mutex = PTHREAD_MUTEX_INITIALIZER;  // Separate mutex for visited URLs
pthread_cond_t frontier_not_empty = PTHREAD_COND_INITIALIZER;

// URL Lists
url_list_t frontier_list = { .urls = NULL, .count = 0, .capacity = 0 };

// Hash table for visited URLs
static int hash_table_initialized = 0;

// Memory buffer for curl
typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} mem_t;

// Initialize URL list
void queue_init(url_list_t *list, int capacity) {
    list->capacity = capacity;
    list->urls = calloc(capacity, sizeof(char *));
    if (!list->urls) {
        perror("calloc url list");
        exit(1);
    }
    list->count = 0;
}

void queue_push(url_list_t *list, char *url) {
    if (!url) return;
    pthread_mutex_lock(&frontier_mutex);
    if (should_exit) {
        pthread_mutex_unlock(&frontier_mutex);
        free(url);
        return;
    }
    if (list->count >= list->capacity) {
        unsigned new_capacity = list->capacity * 2;
        char **new_urls = realloc(list->urls, new_capacity * sizeof(char *));
        if (!new_urls) {
            fprintf(stderr, "queue_push: realloc failed, capacity=%u\n", new_capacity);
            pthread_mutex_unlock(&frontier_mutex);
            free(url);
            return;
        }
        list->urls = new_urls;
        list->capacity = new_capacity;
    }
    list->urls[list->count++] = url;
    pthread_cond_signal(&frontier_not_empty);
    pthread_mutex_unlock(&frontier_mutex);
}

// Fixed queue_pop function
char *queue_pop(url_list_t *list) {
    pthread_mutex_lock(&frontier_mutex);
    while (list->count == 0 && !should_exit) {
        pthread_cond_wait(&frontier_not_empty, &frontier_mutex);
    }
    if (should_exit) {
        pthread_mutex_unlock(&frontier_mutex);
        return NULL;
    }
    if (list->count == 0) {
        pthread_mutex_unlock(&frontier_mutex);
        return NULL;
    }
    char *url = list->urls[--list->count];
    list->urls[list->count] = NULL;
    pthread_mutex_unlock(&frontier_mutex);
    return url;
}

void queue_destroy(url_list_t *list) {
    if (!list->urls) return;
    for (unsigned i = 0; i < list->count; i++) {
        if (list->urls[i]) {
            free(list->urls[i]);
            list->urls[i] = NULL;
        }
    }
    free(list->urls);
    list->urls = NULL;
    list->count = 0;
    list->capacity = 0;
}

// Hash table functions for visited URLs
int init_visited_hash_table() {
    if (hash_table_initialized) return 1;
    if (hcreate(HASH_TABLE_SIZE) == 0) {
        perror("hcreate");
        return 0;
    }
    hash_table_initialized = 1;
    return 1;
}

int is_url_visited(const char *url) {
    if (!hash_table_initialized) return 0;
    
    ENTRY item;
    item.key = (char *)url;
    item.data = NULL;
    
    pthread_mutex_lock(&visited_mutex);
    ENTRY *found = hsearch(item, FIND);
    pthread_mutex_unlock(&visited_mutex);
    
    return found != NULL;
}

int add_to_visited(const char *url) {
    if (!hash_table_initialized) return 0;
    
    // Create a copy of the URL for the hash table
    char *url_copy = malloc(strlen(url) + 1);
    if (!url_copy) {
        fprintf(stderr, "add_to_visited: malloc failed for url copy\n");
        return 0;
    }
    strcpy(url_copy, url);
    
    ENTRY item;
    item.key = url_copy;
    item.data = (void *)1;  // Just a marker
    
    pthread_mutex_lock(&visited_mutex);
    ENTRY *result = hsearch(item, ENTER);
    pthread_mutex_unlock(&visited_mutex);
    
    if (!result) {
        free(url_copy);
        return 0;
    }
    return 1;
}

void cleanup_visited_hash_table() {
    if (hash_table_initialized) {
        hdestroy();
        hash_table_initialized = 0;
    }
}

// Curl callbacks
size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    mem_t *m = userdata;
    if (!m || total == 0) {
        return 0;
    }
    
    // Ensure we have enough capacity
    if (m->len + total + 1 > m->capacity) {
        size_t new_capacity = m->capacity * 2;
        if (new_capacity < m->len + total + 1) {
            new_capacity = m->len + total + 1;
        }
        char *new_data = realloc(m->data, new_capacity);
        if (!new_data) {
            fprintf(stderr, "write_cb: realloc failed, len=%zu, total=%zu\n", m->len, total);
            return 0;
        }
        m->data = new_data;
        m->capacity = new_capacity;
    }
    
    memcpy(m->data + m->len, ptr, total);
    m->len += total;
    m->data[m->len] = '\0';
    return total;
}

size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t realsize = size * nitems;
    int *content_type = (int *)userdata;
    if (!content_type) return realsize;
    
    // Copy buffer to a null-terminated string
    size_t copy_len = realsize < URL_MAX_LEN ? realsize : URL_MAX_LEN - 1;
    char *tmp = malloc(copy_len + 1);
    if (!tmp) {
        fprintf(stderr, "header_cb: malloc failed\n");
        return realsize;
    }
    memcpy(tmp, buffer, copy_len);
    tmp[copy_len] = '\0';
    
    if (strstr(tmp, "Content-Type: text/html")) {
        *content_type = CONTENT_HTML;
    } else if (strstr(tmp, "Content-Type: image/png")) {
        *content_type = CONTENT_PNG;
    }
    
    free(tmp);
    return realsize;
}

// PNG verification
int is_png(mem_t *m) {
    if (!m || !m->data || m->len < PNG_SIG_LEN) {
        return 0;
    }
    return memcmp(m->data, PNG_SIGNATURE, PNG_SIG_LEN) == 0;
}

// URL resolution
char *resolve_url(const char *base_url, const char *relative_url) {
    if (!relative_url || !base_url) {
        return NULL;
    }
    
    // Check for absolute URL
    if (strncmp(relative_url, "http://", 7) == 0 || strncmp(relative_url, "https://", 8) == 0) {
        size_t len = strlen(relative_url);
        if (len >= URL_MAX_LEN) return NULL;
        char *result = malloc(len + 1);
        if (result) {
            memcpy(result, relative_url, len + 1);
        }
        return result;
    }
    
    char *result = malloc(URL_MAX_LEN);
    if (!result) {
        return NULL;
    }
    
    if (relative_url[0] == '/') {
        // Absolute path
        const char *proto_end = strstr(base_url, "://");
        if (proto_end) {
            proto_end += 3;
            const char *host_end = strchr(proto_end, '/');
            size_t host_len = host_end ? (size_t)(host_end - base_url) : strlen(base_url);
            if (host_len + strlen(relative_url) >= URL_MAX_LEN) {
                free(result);
                return NULL;
            }
            snprintf(result, URL_MAX_LEN, "%.*s%s", (int)host_len, base_url, relative_url);
        } else {
            free(result);
            return NULL;
        }
    } else {
        // Relative path
        size_t base_len = strlen(base_url);
        if (base_len >= URL_MAX_LEN) {
            free(result);
            return NULL;
        }
        char *base_copy = malloc(base_len + 1);
        if (!base_copy) {
            free(result);
            return NULL;
        }
        memcpy(base_copy, base_url, base_len + 1);
        
        char *last_slash = strrchr(base_copy, '/');
        if (last_slash && last_slash > base_copy + 7) {
            *last_slash = '\0';
            if (strlen(base_copy) + strlen(relative_url) + 1 >= URL_MAX_LEN) {
                free(base_copy);
                free(result);
                return NULL;
            }
            snprintf(result, URL_MAX_LEN, "%s/%s", base_copy, relative_url);
        } else {
            if (base_len + strlen(relative_url) + 1 >= URL_MAX_LEN) {
                free(base_copy);
                free(result);
                return NULL;
            }
            snprintf(result, URL_MAX_LEN, "%s/%s", base_url, relative_url);
        }
        free(base_copy);
    }
    return result;
}

// Validate URL
int is_valid_url(const char *url) {
    if (!url) return 0;
    return (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

// HTML URL extraction
void extract_urls(const char *html, const char *base_url, url_list_t *list) {
    if (!html || !base_url || !list) {
        return;
    }
    
    size_t html_len = strlen(html);
    if (html_len == 0) {
        return;
    }
    
    const char *pos = html;
    const char *data_end = html + html_len;
    
    while (pos < data_end && (data_end - pos) > 5) {
        const char *attr = NULL;
        const char *href_pos = strstr(pos, "href=");
        const char *src_pos = strstr(pos, "src=");
        
        if (href_pos && (!src_pos || href_pos < src_pos)) {
            attr = href_pos;
            pos = attr + 5;
        } else if (src_pos) {
            attr = src_pos;
            pos = attr + 4;
        } else {
            break;
        }
        
        if (pos >= data_end) break;
        
        // Skip whitespace
        while (pos < data_end && isspace(*pos)) pos++;
        if (pos >= data_end) break;
        
        // Handle quotes
        char quote = 0;
        if (pos < data_end && (*pos == '"' || *pos == '\'')) {
            quote = *pos++;
        }
        if (pos >= data_end) break;
        
        const char *url_start = pos;
        const char *url_end = NULL;
        
        if (quote) {
            url_end = memchr(pos, quote, data_end - pos);
            if (!url_end) url_end = data_end;
        } else {
            url_end = pos;
            while (url_end < data_end && !isspace(*url_end) && *url_end != '>' && *url_end != '\n' && *url_end != '\t') {
                url_end++;
            }
        }
        
        if (!url_end || url_end >= data_end || url_end <= url_start) {
            pos = url_end ? url_end + 1 : data_end;
            continue;
        }
        
        size_t url_len = url_end - url_start;
        if (url_len == 0 || url_len >= URL_MAX_LEN) {
            pos = url_end + (quote && url_end < data_end ? 1 : 0);
            continue;
        }
        
        char *url = malloc(url_len + 1);
        if (!url) {
            pos = url_end + (quote && url_end < data_end ? 1 : 0);
            continue;
        }
        memcpy(url, url_start, url_len);
        url[url_len] = '\0';
        
        char *absolute_url = resolve_url(base_url, url);
        free(url);
        
        if (absolute_url && is_valid_url(absolute_url)) {
            if (!is_url_visited(absolute_url)) {
                char *url_copy = malloc(strlen(absolute_url) + 1);
                if (url_copy) {
                    strcpy(url_copy, absolute_url);
                    queue_push(list, url_copy);
                }
            }
            free(absolute_url);
        } else if (absolute_url) {
            free(absolute_url);
        }
        
        pos = url_end + (quote && url_end < data_end ? 1 : 0);
    }
}

// Fetcher thread
void *fetcher_thread(void *arg) {
    (void)arg;
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Thread %ld: curl_easy_init failed\n", pthread_self());
        return NULL;
    }
    
    mem_t resp = {NULL, 0, 0};
    resp.data = malloc(1024);  // Initial buffer
    resp.capacity = 1024;
    if (!resp.data) {
        curl_easy_cleanup(curl);
        return NULL;
    }
    
    int content_type = CONTENT_UNKNOWN;
    
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &content_type);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "findpng2/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    pthread_mutex_lock(&count_mutex);
    active_threads++;
    pthread_mutex_unlock(&count_mutex);
    
    while (!should_exit) {
        // Check if we've reached the limit
        pthread_mutex_lock(&count_mutex);
        if (png_count >= M) {
            should_exit = 1;
            pthread_mutex_unlock(&count_mutex);
            pthread_mutex_lock(&frontier_mutex);
            pthread_cond_broadcast(&frontier_not_empty);
            pthread_mutex_unlock(&frontier_mutex);
            break;
        }
        pthread_mutex_unlock(&count_mutex);
        
        char *url = queue_pop(&frontier_list);
        if (!url) {
            break;
        }
        
        if (!is_valid_url(url)) {
            free(url);
            continue;
        }
        
        // Check if already visited and add to visited set
        if (is_url_visited(url)) {
            free(url);
            continue;
        }
        
        if (!add_to_visited(url)) {
            free(url);
            continue;
        }
        
        // Log the URL
        if (log_fp) {
            pthread_mutex_lock(&log_mutex);
            fprintf(log_fp, "%s\n", url);
            fflush(log_fp);
            pthread_mutex_unlock(&log_mutex);
        }
        
        // Reset response buffer
        resp.len = 0;
        resp.data[0] = '\0';
        content_type = CONTENT_UNKNOWN;
        
        curl_easy_setopt(curl, CURLOPT_URL, url);
        CURLcode res = curl_easy_perform(curl);
        
        if (res == CURLE_OK) {
            if (content_type == CONTENT_PNG && is_png(&resp)) {
                pthread_mutex_lock(&count_mutex);
                if (png_count < M) {
                    pthread_mutex_lock(&log_mutex);
                    fprintf(png_urls_fp, "%s\n", url);
                    fflush(png_urls_fp);
                    pthread_mutex_unlock(&log_mutex);
                    png_count++;
                    printf("Thread %ld: Found PNG %s (%d/%d)\n", pthread_self(), url, png_count, M);
                }
                pthread_mutex_unlock(&count_mutex);
            } else if (content_type == CONTENT_HTML && resp.data && resp.len > 0) {
                extract_urls(resp.data, url, &frontier_list);
            }
        }
        
        free(url);
    }
    
    curl_easy_cleanup(curl);
    free(resp.data);
    
    pthread_mutex_lock(&count_mutex);
    active_threads--;
    if (active_threads == 0 || png_count >= M) {
        should_exit = 1;
        pthread_mutex_lock(&frontier_mutex);
        pthread_cond_broadcast(&frontier_not_empty);
        pthread_mutex_unlock(&frontier_mutex);
    }
    pthread_mutex_unlock(&count_mutex);
    
    return NULL;
}

void cleanup_resources() {
    pthread_mutex_destroy(&count_mutex);
    pthread_mutex_destroy(&log_mutex);
    pthread_mutex_destroy(&frontier_mutex);
    pthread_mutex_destroy(&visited_mutex);
    pthread_cond_destroy(&frontier_not_empty);
    cleanup_visited_hash_table();
}

void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-t T] [-m M] [-v logfile] URL\n", prog);
    fprintf(stderr, "  -t T        Number of threads (default: 1)\n");
    fprintf(stderr, "  -m M        Max number of PNGs to find (default: 50)\n");
    fprintf(stderr, "  -v logfile  Log visited URLs to file (optional)\n");
    fprintf(stderr, "  URL         Starting URL to crawl\n");
}

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "t:m:v:h")) != -1) {
        switch (opt) {
            case 't':
                T = atoi(optarg);
                if (T <= 0) {
                    fprintf(stderr, "Error: invalid -t <threads>\n");
                    usage(argv[0]);
                    return 1;
                }
                break;
            case 'm':
                M = atoi(optarg);
                if (M <= 0) {
                    fprintf(stderr, "Error: invalid -m <max_pngs>\n");
                    usage(argv[0]);
                    return 1;
                }
                break;
            case 'v':
                log_file = optarg;
                break;
            case 'h':
            default:
                usage(argv[0]);
                return 1;
        }
    }
    
    if (optind >= argc) {
        fprintf(stderr, "Error: missing start URL\n");
        usage(argv[0]);
        return 1;
    }
    
    start_url = argv[optind];
    if (!is_valid_url(start_url)) {
        fprintf(stderr, "Error: Invalid start URL: %s\n", start_url);
        return 1;
    }
    
    curl_global_init(CURL_GLOBAL_ALL);
    
    // Initialize hash table for visited URLs
    if (!init_visited_hash_table()) {
        fprintf(stderr, "Failed to initialize hash table\n");
        curl_global_cleanup();
        return 1;
    }
    
    queue_init(&frontier_list, INITIAL_LIST_CAPACITY);
    
    png_urls_fp = fopen("png_urls.txt", "w");
    if (!png_urls_fp) {
        perror("fopen png_urls.txt");
        cleanup_resources();
        curl_global_cleanup();
        return 1;
    }
    
    if (log_file) {
        log_fp = fopen(log_file, "w");
        if (!log_fp) {
            perror("fopen log file");
            fclose(png_urls_fp);
            cleanup_resources();
            curl_global_cleanup();
            return 1;
        }
    }
    
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    pthread_t *threads = malloc(T * sizeof(pthread_t));
    if (!threads) {
        perror("malloc threads");
        fclose(png_urls_fp);
        if (log_fp) fclose(log_fp);
        cleanup_resources();
        curl_global_cleanup();
        return 1;
    }
    
    // Add seed URL to frontier
    size_t seed_len = strlen(start_url);
    if (seed_len >= URL_MAX_LEN) {
        fprintf(stderr, "Seed URL too long\n");
        free(threads);
        fclose(png_urls_fp);
        if (log_fp) fclose(log_fp);
        cleanup_resources();
        curl_global_cleanup();
        return 1;
    }
    
    char *seed_url = malloc(seed_len + 1);
    if (!seed_url) {
        fprintf(stderr, "malloc failed for seed URL\n");
        free(threads);
        fclose(png_urls_fp);
        if (log_fp) fclose(log_fp);
        cleanup_resources();
        curl_global_cleanup();
        return 1;
    }
    memcpy(seed_url, start_url, seed_len + 1);
    queue_push(&frontier_list, seed_url);
    
    // Create threads
    for (int i = 0; i < T; i++) {
        if (pthread_create(&threads[i], NULL, fetcher_thread, NULL) != 0) {
            perror("pthread_create");
            free(threads);
            fclose(png_urls_fp);
            if (log_fp) fclose(log_fp);
            cleanup_resources();
            curl_global_cleanup();
            return 1;
        }
    }
    
    // Wait for threads to complete
    for (int i = 0; i < T; i++) {
        pthread_join(threads[i], NULL);
    }
    
    free(threads);
    
    gettimeofday(&end, NULL);
    double time_spent = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    
    queue_destroy(&frontier_list);
    cleanup_resources();
    curl_global_cleanup();
    
    fclose(png_urls_fp);
    if (log_fp) fclose(log_fp);
    
    printf("findpng2 execution time: %.6f seconds\n", time_spent);
    return 0;
}
