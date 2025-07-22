#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lab_png.h"
#include "crc.h"
#include "zutil.h"

#define PNG_SIG_SIZE 8

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s [PNG_FILE]...\n", argv[0]);
		return EXIT_SUCCESS;
	}

	FILE *out = fopen("all.png", "wb");
	if (!out) {
		fprintf(stderr, "Error: Cannot create all.png\n");
		return EXIT_FAILURE;
	}

	U8 sig[PNG_SIG_SIZE] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	fwrite(sig, 1, PNG_SIG_SIZE, out);

	struct data_IHDR ref_hdr;
	U8 *concat_data = NULL;
	U64 concat_size = 0;
	U32 total_height = 0;
	int valid_pngs = 0;

	for (int i = 1; i < argc; ++i) {
		FILE *fp = fopen(argv[i], "rb");
		if (!fp) {
			fprintf(stderr, "Warning: Cannot open %s\n", argv[i]);
			continue;
		}

		U8 sig_buf[PNG_SIG_SIZE];
		if (fread(sig_buf, 1, PNG_SIG_SIZE, fp) != PNG_SIG_SIZE || !is_png(sig_buf, PNG_SIG_SIZE)) {
			fprintf(stderr, "Warning: %s is not a valid PNG\n", argv[i]);
			fclose(fp);
			continue;
		}

		struct data_IHDR hdr;
		if (!get_png_data_IHDR(&hdr, fp, PNG_SIG_SIZE, SEEK_SET)) {
			fprintf(stderr, "Warning: Invalid IHDR in %s\n", argv[i]);
			fclose(fp);
			continue;
		}

		if (valid_pngs == 0) {
			ref_hdr = hdr;
		} else if (hdr.width != ref_hdr.width || hdr.bit_depth != ref_hdr.bit_depth ||
				   hdr.color_type != ref_hdr.color_type || hdr.compression != ref_hdr.compression ||
				   hdr.filter != ref_hdr.filter || hdr.interlace != ref_hdr.interlace) {
			fprintf(stderr, "Warning: %s has incompatible IHDR\n", argv[i]);
			fclose(fp);
			continue;
		}

		simple_PNG_p png = mallocPNG();
		if (!png) {
			fprintf(stderr, "Error: Memory allocation failed for PNG struct\n");
			fclose(fp);
			continue;
		}

		if (!get_png_chunks(png, fp, PNG_SIG_SIZE, SEEK_SET)) {
			fprintf(stderr, "Warning: Failed to read chunks in %s\n", argv[i]);
			free_png(png);
			fclose(fp);
			continue;
		}

		if (!png->p_IDAT || !png->p_IDAT->p_data) {
			fprintf(stderr, "Warning: Missing or invalid IDAT in %s\n", argv[i]);
			free_png(png);
			fclose(fp);
			continue;
		}

		if (get_chunk_crc(png->p_IDAT) != calculate_chunk_crc(png->p_IDAT)) {
			fprintf(stderr, "Warning: IDAT CRC mismatch in %s\n", argv[i]);
			free_png(png);
			fclose(fp);
			continue;
		}

		// decompressed size: height × (bytes per row + 1 for filter byte)
		U32 bpp = 4; // assume 4 bytes per pixel (e.g. RGBA)
		U64 est_raw_size = hdr.height * (hdr.width * bpp + 1);
		U8 *raw = malloc(est_raw_size);
		U64 raw_len = 0;

		if (!raw || mem_inf(raw, &raw_len, png->p_IDAT->p_data, png->p_IDAT->length) != Z_OK) {
			fprintf(stderr, "Warning: Failed to decompress IDAT in %s\n", argv[i]);
			free(raw);
			free_png(png);
			fclose(fp);
			continue;
		}

		U8 *new_data = realloc(concat_data, concat_size + raw_len);
		if (!new_data) {
			fprintf(stderr, "Error: Memory allocation failed during concatenation\n");
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
		total_height += hdr.height;
		valid_pngs++;

		free(raw);
		free_png(png);
		fclose(fp);
	}

	if (valid_pngs == 0) {
		fprintf(stderr, "No valid PNG files found.\n");
		fclose(out);
		return EXIT_SUCCESS;
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
	ihdr.crc = crc((U8 *)ihdr.type, 4);
	ihdr.crc = crc32(ihdr.crc, ihdr.p_data, ihdr.length);
	write_chunk(out, &ihdr);
	free(ihdr.p_data);

	U64 zlen = concat_size * 2;
	U8 *zout = malloc(zlen);
	if (!zout || mem_def(zout, &zlen, concat_data, concat_size, Z_DEFAULT_COMPRESSION) != Z_OK) {
		fprintf(stderr, "Error: Compression failed\n");
		free(zout);
		free(concat_data);
		fclose(out);
		return EXIT_FAILURE;
	}

	struct chunk idat = {.length = zlen};
	memcpy(idat.type, "IDAT", 4);
	idat.p_data = zout;
	idat.crc = crc((U8 *)idat.type, 4);
	idat.crc = crc32(idat.crc, idat.p_data, idat.length);
	write_chunk(out, &idat);
	free(idat.p_data);

	struct chunk iend = {.length = 0, .p_data = NULL};
	memcpy(iend.type, "IEND", 4);
	iend.crc = crc((U8 *)iend.type, 4);
	write_chunk(out, &iend);

	free(concat_data);
	fclose(out);
	return EXIT_SUCCESS;
}
