/*
 * Copyright (c) 2014,2015 Hayaki Saito
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#if HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif

#if HAVE_UNISTD_H
# include <unistd.h>
#endif

#if HAVE_FCNTL_H
# include <fcntl.h>
#endif

#if HAVE_IO_H
# include <io.h>
#endif

#ifdef HAVE_ERRNO_H
# include <errno.h>
#endif

#ifdef HAVE_LIBCURL
# include <curl/curl.h>
#endif

#if !defined(HAVE_MEMCPY)
# define memcpy(d, s, n) (bcopy ((s), (d), (n)))
#endif

#if !defined(HAVE_MEMMOVE)
# define memmove(d, s, n) (bcopy ((s), (d), (n)))
#endif

#if !defined(O_BINARY) && defined(_O_BINARY)
# define O_BINARY _O_BINARY
#endif  /* !defined(O_BINARY) && !defined(_O_BINARY) */

#include "chunk.h"
#include "allocator.h"

static SIXELSTATUS
sixel_chunk_init(
    sixel_chunk_t * const /* in */ pchunk,
    size_t                /* in */ initial_size)
{
    SIXELSTATUS status = SIXEL_FALSE;

    pchunk->max_size = initial_size;
    pchunk->size = 0;
    pchunk->buffer
        = (unsigned char *)pchunk->allocator->fn_malloc(pchunk->max_size);

    if (pchunk->buffer == NULL) {
        sixel_helper_set_additional_message(
            "sixel_chunk_init: malloc() failed.");
        status = SIXEL_BAD_ALLOCATION;
        goto end;
    }

    status = SIXEL_OK;

end:
    return status;
}


# ifdef HAVE_LIBCURL
static size_t
memory_write(void *ptr,
             size_t size,
             size_t len,
             void *memory)
{
    size_t nbytes;
    sixel_chunk_t *chunk;

    if (ptr == NULL || memory == NULL) {
        return 0;
    }
    nbytes = size * len;
    if (nbytes == 0) {
        return 0;
    }

    chunk = (sixel_chunk_t *)memory;
    if (chunk->buffer == NULL) {
        return 0;
    }

    if (chunk->max_size <= chunk->size + nbytes) {
        do {
            chunk->max_size *= 2;
        } while (chunk->max_size <= chunk->size + nbytes);
        chunk->buffer = (unsigned char*)realloc(chunk->buffer, chunk->max_size);
        if (chunk->buffer == NULL) {
            return 0;
        }
    }

    memcpy(chunk->buffer + chunk->size, ptr, nbytes);
    chunk->size += nbytes;

    return nbytes;
}
# endif


static int
wait_file(int fd, int usec)
{
#if HAVE_SYS_SELECT_H
    fd_set rfds;
    struct timeval tv;
#endif  /* HAVE_SYS_SELECT_H */
    int ret = 1;

#if HAVE_SYS_SELECT_H
    tv.tv_sec = usec / 1000000;
    tv.tv_usec = usec % 1000000;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    ret = select(fd + 1, &rfds, NULL, NULL, &tv);
#else
    (void) fd;
    (void) usec;
#endif  /* HAVE_SYS_SELECT_H */
    if (ret == 0) {
        return (1);
    }
    if (ret < 0) {
        return ret;
    }

    return (0);
}


static SIXELSTATUS
open_binary_file(
    FILE        /* out */   **f,
    char const  /* in */    *filename)
{
    SIXELSTATUS status = SIXEL_FALSE;
    char buffer[1024];
#if HAVE_STAT
    struct stat sb;
#endif  /* HAVE_STAT */

    if (filename == NULL || strcmp(filename, "-") == 0) {
        /* for windows */
#if defined(O_BINARY)
# if HAVE__SETMODE
        _setmode(fileno(stdin), O_BINARY);
# elif HAVE_SETMODE
        setmode(fileno(stdin), O_BINARY);
# endif  /* HAVE_SETMODE */
#endif  /* defined(O_BINARY) */
        *f = stdin;

        status = SIXEL_OK;
        goto end;
    }

#if HAVE_STAT
    if (stat(filename, &sb) != 0) {
        status = (SIXEL_LIBC_ERROR | (errno & 0xff));
        if (sprintf(buffer, "stat('%s') failed.", filename) != EOF) {
            sixel_helper_set_additional_message(buffer);
        }
        goto end;
    }
    if ((sb.st_mode & S_IFMT) == S_IFDIR) {
        status = SIXEL_BAD_INPUT;
        if (sprintf(buffer, "'%s' is directory.", filename) != EOF) {
            sixel_helper_set_additional_message(buffer);
        }
        goto end;
    }
#endif  /* HAVE_STAT */

    *f = fopen(filename, "rb");
    if (!*f) {
        status = (SIXEL_LIBC_ERROR | (errno & 0xff));
        if (sprintf(buffer, "fopen('%s') failed.", filename) != EOF) {
            sixel_helper_set_additional_message(buffer);
        }
        goto end;
    }

    status = SIXEL_OK;
end:
    return status;
}


static SIXELSTATUS
sixel_chunk_from_file(
    char const      /* in */ *filename,
    sixel_chunk_t   /* in */ *pchunk,
    int const       /* in */ *cancel_flag
)
{
    SIXELSTATUS status = SIXEL_FALSE;
    int ret;
    FILE *f;
    int n;

    status = open_binary_file(&f, filename);
    if (SIXEL_FAILED(status)) {
        goto end;
    }

    for (;;) {
        if (pchunk->max_size - pchunk->size < 4096) {
            pchunk->max_size *= 2;
            pchunk->buffer = (unsigned char *)realloc(pchunk->buffer,
                                                      pchunk->max_size);
            if (pchunk->buffer == NULL) {
                sixel_helper_set_additional_message(
                    "sixel_chunk_from_file: realloc() failed.");
                status = SIXEL_BAD_ALLOCATION;
                goto end;
            }
        }

        if (isatty(fileno(f))) {
            for (;;) {
                if (*cancel_flag) {
                    status = SIXEL_INTERRUPTED;
                    goto end;
                }
                ret = wait_file(fileno(f), 10000);
                if (ret < 0) {
                    status = SIXEL_RUNTIME_ERROR;
                    goto end;
                }
                if (ret == 0) {
                    break;
                }
            }
        }
        n = fread(pchunk->buffer + pchunk->size, 1, 4096, f);
        if (n <= 0) {
            break;
        }
        pchunk->size += n;
    }

    if (f != stdin) {
        fclose(f);
    }

    status = SIXEL_OK;

end:
    return status;
}


static SIXELSTATUS
sixel_chunk_from_url(
    char const *url,
    sixel_chunk_t *pchunk,
    int finsecure)
{
    SIXELSTATUS status = SIXEL_FALSE;

# ifdef HAVE_LIBCURL
    CURL *curl;
    CURLcode code;
    char buffer[1024];

    curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (finsecure && strncmp(url, "https://", 8) == 0) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, memory_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)pchunk);
    code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        status = SIXEL_CURL_ERROR & (code & 0xff);
        if (sprintf(buffer, "curl_easy_perform('%s') failed.", url) != EOF) {
            sixel_helper_set_additional_message(buffer);
        }
        curl_easy_cleanup(curl);
        goto end;
    }
    curl_easy_cleanup(curl);

    status = SIXEL_OK;
# else
    (void) url;
    (void) pchunk;
    (void) finsecure;
    sixel_helper_set_additional_message(
        "To specify URI schemes, you have to "
        "configure this program with --with-libcurl "
        "option at compile time.\n");
    status = SIXEL_NOT_IMPLEMENTED;
    goto end;
# endif  /* HAVE_LIBCURL */

end:
    return status;
}


SIXELSTATUS
sixel_chunk_new(
    sixel_chunk_t       /* out */   **ppchunk,
    char const          /* in */    *filename,
    int                 /* in */    finsecure,
    int const           /* in */    *cancel_flag,
    sixel_allocator_t   /* in */    *allocator)
{
    SIXELSTATUS status = SIXEL_FALSE;

    *ppchunk = (sixel_chunk_t *)allocator->fn_malloc(sizeof(sixel_chunk_t));
    if (*ppchunk == NULL) {
        sixel_helper_set_additional_message(
            "sixel_chunk_new: malloc() failed.");
        status = SIXEL_BAD_ALLOCATION;
        goto end;
    }

    /* set allocator to chunk object */
    (*ppchunk)->allocator = allocator;

    status = sixel_chunk_init(*ppchunk, 1024 * 32);
    if (SIXEL_FAILED(status)) {
        goto end;
    }

    if (filename != NULL && strstr(filename, "://")) {
        status = sixel_chunk_from_url(filename, *ppchunk, finsecure);
    } else {
        status = sixel_chunk_from_file(filename, *ppchunk, cancel_flag);
    }
    if (SIXEL_FAILED(status)) {
        goto end;
    }

    status = SIXEL_OK;

end:
    return status;
}


void
sixel_chunk_destroy(
    sixel_chunk_t * const /* in */ pchunk)
{
    if (pchunk) {
        pchunk->allocator->fn_free(pchunk->buffer);
        pchunk->buffer = NULL;
        pchunk->allocator->fn_free(pchunk);
    }
}


#if HAVE_TESTS
static int
test1(void)
{
    int nret = EXIT_FAILURE;
    unsigned char *ptr = malloc(16);

#ifdef HAVE_LIBCURL
    sixel_chunk_t chunk = {0, 0, 0, NULL};
    int nread;

    nread = memory_write(NULL, 1, 1, NULL);
    if (nread != 0) {
        goto error;
    }

    nread = memory_write(ptr, 1, 1, &chunk);
    if (nread != 0) {
        goto error;
    }

    nread = memory_write(ptr, 0, 1, &chunk);
    if (nread != 0) {
        goto error;
    }
#else
    nret = EXIT_SUCCESS;
    goto error;
#endif  /* HAVE_LIBCURL */
    nret = EXIT_SUCCESS;

error:
    free(ptr);
    return nret;
}


int
sixel_chunk_tests_main(void)
{
    int nret = EXIT_FAILURE;
    size_t i;
    typedef int (* testcase)(void);

    static testcase const testcases[] = {
        test1,
    };

    for (i = 0; i < sizeof(testcases) / sizeof(testcase); ++i) {
        nret = testcases[i]();
        if (nret != EXIT_SUCCESS) {
            goto error;
        }
    }

    nret = EXIT_SUCCESS;

error:
    return nret;
}
#endif  /* HAVE_TESTS */


/* emacs, -*- Mode: C; tab-width: 4; indent-tabs-mode: nil -*- */
/* vim: set expandtab ts=4 : */
/* EOF */