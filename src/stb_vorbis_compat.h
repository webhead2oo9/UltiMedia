#pragma once

#include <stdio.h>

typedef struct
{
    char *alloc_buffer;
    int alloc_buffer_length_in_bytes;
} stb_vorbis_alloc;

typedef struct stb_vorbis stb_vorbis;

typedef struct
{
    unsigned int sample_rate;
    int channels;

    unsigned int setup_memory_required;
    unsigned int setup_temp_memory_required;
    unsigned int temp_memory_required;

    int max_frame_size;
} stb_vorbis_info;

typedef struct
{
    char *vendor;
    int comment_list_length;
    char **comment_list;
} stb_vorbis_comment;

extern stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
extern stb_vorbis_comment stb_vorbis_get_comment(stb_vorbis *f);
extern void stb_vorbis_close(stb_vorbis *f);
extern stb_vorbis *stb_vorbis_open_filename(const char *filename, int *error, const stb_vorbis_alloc *alloc_buffer);
extern stb_vorbis *stb_vorbis_open_file(FILE *file, int close_handle_on_close, int *error, const stb_vorbis_alloc *alloc_buffer);
extern int stb_vorbis_seek(stb_vorbis *f, unsigned int sample_number);
extern int stb_vorbis_seek_start(stb_vorbis *f);
extern unsigned int stb_vorbis_stream_length_in_samples(stb_vorbis *f);
extern int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts);
