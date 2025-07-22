#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lab_png.h"
#include "crc.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return EXIT_FAILURE;
    }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        fprintf(stderr, "%s: Unable to open file\n", argv[1]);
        return EXIT_FAILURE;
    }

    // Check PNG signature
    U8 sig[PNG_SIG_SIZE];
    if (fread(sig, 1, PNG_SIG_SIZE, fp) != PNG_SIG_SIZE || !is_png(sig, PNG_SIG_SIZE)) {
        printf("%s: Not a PNG file\n", argv[1]);
        fclose(fp);
        return EXIT_SUCCESS;
    }

    // Get IHDR data
    struct data_IHDR ihdr;
    if (!get_png_data_IHDR(&ihdr, fp, 0, SEEK_SET)) {
        fprintf(stderr, "%s: Invalid IHDR chunk\n", argv[1]);
        fclose(fp);
        return EXIT_FAILURE;
    }

    // Get all chunks
    simple_PNG_p png = mallocPNG();
    if (!png || !get_png_chunks(png, fp, 0, SEEK_SET)) {
        fprintf(stderr, "%s: Failed to read PNG chunks\n", argv[1]);
        free_png(png);
        fclose(fp);
        return EXIT_FAILURE;
    }

    // Check IDAT CRC
    U32 expected_crc = get_chunk_crc(png->p_IDAT);
    U32 computed_crc = calculate_chunk_crc(png->p_IDAT);

    if (expected_crc != computed_crc) {
        printf("%s: %u x %u\n", argv[1], ihdr.width, ihdr.height);
        printf("IDAT chunk CRC error: computed %08x, expected %08x\n", computed_crc, expected_crc);
    } else {
        printf("%s: %u x %u\n", argv[1], ihdr.width, ihdr.height);
    }

    free_png(png);
    fclose(fp);
    return EXIT_SUCCESS;
}