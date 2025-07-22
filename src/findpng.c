// In src/findpng.c, replace the search_directory function
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include "lab_png.h"

#define PNG_SIG_SIZE 8
#define PATH_MAX_LEN 4096

void search_directory(const char *base_path, const char *relative_path, int *png_count) {
    char full_path[PATH_MAX_LEN];
    char new_relative_path[PATH_MAX_LEN];
    DIR *dir;
    struct dirent *entry;
    struct stat sb;

    // Build full path
    if (strcmp(relative_path, ".") == 0) {
        snprintf(full_path, sizeof(full_path), "%s", base_path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, relative_path);
    }

    dir = opendir(full_path);
    if (!dir) {
        fprintf(stderr, "Error: Cannot open directory '%s'\n", full_path);
        return;
    }

    while ((entry = readdir(dir))) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // Check path length before constructing paths
        size_t full_path_len = strlen(full_path);
        size_t entry_name_len = strlen(entry->d_name);
        size_t relative_path_len = strlen(relative_path);
        
        // Check if new_full_path would exceed buffer
        if (full_path_len + 1 + entry_name_len >= PATH_MAX_LEN) {
            fprintf(stderr, "Warning: Path too long for '%s/%s'\n", full_path, entry->d_name);
            continue;
        }
        
        // Check if new_relative_path would exceed buffer
        size_t new_relative_len = (strcmp(relative_path, ".") == 0) ? 
            entry_name_len : relative_path_len + 1 + entry_name_len;
        if (new_relative_len >= PATH_MAX_LEN) {
            fprintf(stderr, "Warning: Relative path too long\n");
            continue;
        }

        // Construct new relative path
        if (strcmp(relative_path, ".") == 0) {
            snprintf(new_relative_path, sizeof(new_relative_path), "%s", entry->d_name);
        } else {
            snprintf(new_relative_path, sizeof(new_relative_path), "%s/%s", relative_path, entry->d_name);
        }

        // Construct full path for stat
        char new_full_path[PATH_MAX_LEN];
        snprintf(new_full_path, sizeof(new_full_path), "%s/%s", full_path, entry->d_name);

        if (lstat(new_full_path, &sb) == -1) {
            fprintf(stderr, "Warning: Cannot stat '%s'\n", new_full_path);
            continue;
        }

        if (S_ISLNK(sb.st_mode)) {
            continue; // Skip symbolic links
        } else if (S_ISDIR(sb.st_mode)) {
            search_directory(base_path, new_relative_path, png_count); // Recurse
        } else if (S_ISREG(sb.st_mode)) {
            FILE *fp = fopen(new_full_path, "rb");
            if (!fp) {
                fprintf(stderr, "Warning: Cannot open file '%s'\n", new_full_path);
                continue;
            }

            U8 sig[PNG_SIG_SIZE];
            size_t nread = fread(sig, 1, PNG_SIG_SIZE, fp);
            fclose(fp);

            if (nread == PNG_SIG_SIZE && is_png(sig, PNG_SIG_SIZE)) {
                char absolute_path[PATH_MAX_LEN];
                if (realpath(new_full_path, absolute_path)) {
                    printf("%s\n", absolute_path);
                    (*png_count)++;
                }

                (*png_count)++;
            }
        }
    }

    closedir(dir);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <directory>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Validate input directory
    struct stat sb;
    if (stat(argv[1], &sb) == -1 || !S_ISDIR(sb.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a valid directory\n", argv[1]);
        return EXIT_FAILURE;
    }

    int png_count = 0;
    search_directory(argv[1], ".", &png_count);

    if (png_count == 0) {
        printf("findpng: No PNG file found\n");
    }

    return EXIT_SUCCESS;
}