#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h> // for ntohl, htonl
#include "zutil.h"
#include "lab_png.h"
#include "crc.h"

// Check PNG signature
int is_png(U8 *buf, size_t n) {
    if (n < PNG_SIG_SIZE) return 0;
    U8 expected_signature[PNG_SIG_SIZE] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return memcmp(buf, expected_signature, PNG_SIG_SIZE) == 0;
}

// Read IHDR data
int get_png_data_IHDR(struct data_IHDR *out, FILE *fp, long offset, int whence) {
    if (!out || fseek(fp, offset, whence) != 0) return 0;

    U32 length;
    if (fread(&length, sizeof(U32), 1, fp) != 1) return 0;
    length = ntohl(length);

    U8 type[CHUNK_TYPE_SIZE];
    if (fread(type, 1, CHUNK_TYPE_SIZE, fp) != CHUNK_TYPE_SIZE) return 0;
    if (memcmp(type, "IHDR", CHUNK_TYPE_SIZE) != 0 || length != DATA_IHDR_SIZE) return 0;

    U8 buf[DATA_IHDR_SIZE];
    if (fread(buf, 1, DATA_IHDR_SIZE, fp) != DATA_IHDR_SIZE) return 0;

    out->width      = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    out->height     = (buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
    out->bit_depth  = buf[8];
    out->color_type = buf[9];
    out->compression = buf[10];
    out->filter     = buf[11];
    out->interlace  = buf[12];

    fseek(fp, CHUNK_CRC_SIZE, SEEK_CUR); // Skip CRC
    return 1;
}

// Read a chunk from file
chunk_p get_chunk(FILE *fp) {
    if (!fp) return NULL;

    chunk_p c = malloc(sizeof(struct chunk));
    if (!c) return NULL;

    U32 net_len;
    if (fread(&net_len, 4, 1, fp) != 1) { free(c); return NULL; }
    c->length = ntohl(net_len);

    if (fread(c->type, 1, CHUNK_TYPE_SIZE, fp) != CHUNK_TYPE_SIZE) { free(c); return NULL; }

    c->p_data = malloc(c->length);
    if (!c->p_data || fread(c->p_data, 1, c->length, fp) != c->length) {
        free_chunk(c);
        return NULL;
    }

    U32 net_crc;
    if (fread(&net_crc, 4, 1, fp) != 1) { free_chunk(c); return NULL; }
    c->crc = ntohl(net_crc);

    return c;
}

// Populate a simple_PNG struct from file
int get_png_chunks(simple_PNG_p out, FILE *fp, long offset, int whence) {
    if (!out || fseek(fp, offset, whence) != 0) return 0;

    out->p_IHDR = get_chunk(fp);
    out->p_IDAT = get_chunk(fp);
    out->p_IEND = get_chunk(fp);

    if (!out->p_IHDR || !out->p_IDAT || !out->p_IEND) return 0;
    return 1;
}

// Allocate a PNG container
simple_PNG_p mallocPNG() {
    simple_PNG_p p = malloc(sizeof(struct simple_PNG));
    if (p) p->p_IHDR = p->p_IDAT = p->p_IEND = NULL;
    return p;
}

// Free all memory in a PNG container
void free_png(simple_PNG_p in) {
    if (!in) return;
    free_chunk(in->p_IHDR);
    free_chunk(in->p_IDAT);
    free_chunk(in->p_IEND);
    free(in);
}

// Free a chunk and its data
void free_chunk(chunk_p in) {
    if (!in) return;
    free(in->p_data);
    free(in);
}

// Chunk CRC functions
U32 get_chunk_crc(chunk_p in) {
    return in ? in->crc : 0;
}

U32 calculate_chunk_crc(chunk_p in) {
    if (!in) return 0;
    U8 *buf = malloc(in->length + CHUNK_TYPE_SIZE);
    if (!buf) return 0;
    memcpy(buf, in->type, CHUNK_TYPE_SIZE);
    memcpy(buf + CHUNK_TYPE_SIZE, in->p_data, in->length);
    U32 crc_val = crc(buf, in->length + CHUNK_TYPE_SIZE);
    free(buf);
    return crc_val;
}

// Write a chunk to a file
int write_chunk(FILE* fp, chunk_p in) {
    if (!fp || !in) return 0;

    U32 net_len = htonl(in->length);
    U32 net_crc = htonl(in->crc);

    fwrite(&net_len, sizeof(U32), 1, fp);
    fwrite(in->type, sizeof(U8), CHUNK_TYPE_SIZE, fp);
    fwrite(in->p_data, sizeof(U8), in->length, fp);
    fwrite(&net_crc, sizeof(U32), 1, fp);

    return 1;
}

// Write a PNG to a file
// In src/lab_png.c, replace the write_PNG function (around line 141)
int write_PNG(char* filepath, simple_PNG_p in) {
    if (!filepath || !in || !in->p_IHDR || !in->p_IDAT || !in->p_IEND) return 0;

    FILE *fp = fopen(filepath, "wb");
    if (!fp) return 0;

    // Write PNG signature
    U8 sig[PNG_SIG_SIZE] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (fwrite(sig, 1, PNG_SIG_SIZE, fp) != PNG_SIG_SIZE) {
        fclose(fp);
        return 0;
    }

    // Write IHDR, IDAT, and IEND chunks
    int success = write_chunk(fp, in->p_IHDR) &&
                  write_chunk(fp, in->p_IDAT) &&
                  write_chunk(fp, in->p_IEND);

    fclose(fp);
    return success ? 1 : 0;
}

// save a complete PNG assembled from 50 PNG strip buffers
int save_png_from_memstrips(const unsigned char *data[], const size_t sizes[], const char *filename) { // Reusing from some part of catpng.c
	FILE *out = fopen(filename, "wb");
	if (!out) {
		fprintf(stderr, "Error: Cannot create all.png\n");
		return EXIT_FAILURE;
	}

    // write standard 8-byte PNG signature to file
	U8 sig[PNG_SIG_SIZE] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	fwrite(sig, 1, PNG_SIG_SIZE, out);

	struct data_IHDR ref_hdr; // store IHDR info from the first valid stip
	U8 *concat_data = NULL; // pointer to full uncompressed image buffer
	U64 concat_size = 0; // total size of all uncompressed strips
	U32 total_height = 0; // cumulative height of all strips

	for (int i = 0; i < 50; ++i) {
        // open memory buffer as file stream for strip i
        FILE *fp = fmemopen((void *)data[i], sizes[i], "rb");
		if (!fp) {
			continue;
		}

        // check if strip is a valid PNG and read IHDR info
        U8 sig_buf[PNG_SIG_SIZE];
		if (fread(sig_buf, 1, PNG_SIG_SIZE, fp) != PNG_SIG_SIZE || !is_png(sig_buf, PNG_SIG_SIZE) || !get_png_data_IHDR(&ref_hdr, fp, PNG_SIG_SIZE, SEEK_SET)) {
			//fprintf(stderr, "Warning: %s is not a valid PNG\n");
			fclose(fp);
			continue;
		}


        // parse PNG chunks (IHDR, IDAT, IEND)
		simple_PNG_p png = mallocPNG();
		if (!get_png_chunks(png, fp, PNG_SIG_SIZE, SEEK_SET)) {
            free_png(png);
			fclose(fp);
			continue;
		}

        // estimate raw image size
        U32 bpp = 4; // RGBA
        U64 est_raw_size = ref_hdr.height * (ref_hdr.width * bpp + 1);
        U8 *raw = malloc(est_raw_size);
        U64 raw_len = 0;

        // decompress IDAT data into raw scanlines
        if (!raw || mem_inf(raw, &raw_len, png->p_IDAT->p_data, png->p_IDAT->length) != Z_OK) {
            free(raw);
            free_png(png);
            fclose(fp);
            continue;
        }


        // append raw strip data 
        U8 *new_data = realloc(concat_data, concat_size + raw_len);
        if (!new_data) {
            free(raw);
            free(concat_data);
            free_png(png);
            fclose(fp);
            fclose(out);
            return EXIT_FAILURE;
            
        }

        concat_data = new_data;
        memcpy(concat_data + concat_size, raw, raw_len);
        concat_size += raw_len;
        total_height += ref_hdr.height; // add heigh of current strip

        free(raw);
        free_png(png);
        fclose(fp);

    
	}

    
	struct chunk ihdr = {.length = DATA_IHDR_SIZE};
	memcpy(ihdr.type, "IHDR", 4);
	ihdr.p_data = malloc(DATA_IHDR_SIZE);
	ihdr.p_data[0] = (ref_hdr.width >> 24) & 0xFF;
	ihdr.p_data[1] = (ref_hdr.width >> 16) & 0xFF;
	ihdr.p_data[2] = (ref_hdr.width >> 8) & 0xFF;
	ihdr.p_data[3] = ref_hdr.width & 0xFF;
	ihdr.p_data[4] = (total_height >> 24) & 0xFF;
	ihdr.p_data[5] = (total_height >> 16) & 0xFF;
	ihdr.p_data[6] = (total_height >> 8) & 0xFF;
	ihdr.p_data[7] = total_height & 0xFF;
	ihdr.p_data[8] = ref_hdr.bit_depth;
	ihdr.p_data[9] = ref_hdr.color_type;
	ihdr.p_data[10] = ref_hdr.compression;
	ihdr.p_data[11] = ref_hdr.filter;
	ihdr.p_data[12] = ref_hdr.interlace;
    
    // compute CRC for IHDR and write to file
	ihdr.crc = crc((U8 *)ihdr.type, 4);
	ihdr.crc = crc32(ihdr.crc, ihdr.p_data, ihdr.length);
	write_chunk(out, &ihdr);
	free(ihdr.p_data);



    // compress and write IDAT to produce one IDAT chunk
	U64 zlen = concat_size * 2;
	U8 *zout = malloc(zlen);
	if (!zout || mem_def(zout, &zlen, concat_data, concat_size, Z_DEFAULT_COMPRESSION) != Z_OK) {
		fprintf(stderr, "Error: Compression failed\n");
		free(zout);
		free(concat_data);
		fclose(out);
		return EXIT_FAILURE;
	}

    // construct and write IDAT chunk
	struct chunk idat = {.length = zlen};
	memcpy(idat.type, "IDAT", 4);
	idat.p_data = zout;
    idat.crc = calculate_chunk_crc(&idat);
	write_chunk(out, &idat);
	free(idat.p_data);

    // write final IEND chunk to close PNG
	struct chunk iend = {.length = 0, .p_data = NULL};
	memcpy(iend.type, "IEND", 4);
	iend.crc = calculate_chunk_crc(&iend);
	write_chunk(out, &iend);


    // cleanup
	free(concat_data);
	fclose(out);
	return EXIT_SUCCESS;
}
