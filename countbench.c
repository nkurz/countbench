// cc -g -march=native -std=gnu99 -Wall -Wextra -O3 countbench.c -o countbench
// Source mangled by Nathan Kurz to create a more focussed benchmark than original
// Optimized for Intel Haswell with gcc compiler.  Works on Sandy Bridge but slower.
// ICC works but is slower.  Clang requires '-mavx2' flag (or appropriate equivalent)

/*
  Based on fullbench.c - Demo program to benchmark open-source compression algorithm
  Copyright (C) Yann Collet 2012-2014  GPL v2 License
  - public forum : https://groups.google.com/forum/#!forum/lz4c
  - website : http://fastcompression.blogspot.com/
*/

#define PROGRAM_DESCRIPTION "Byte histogram benchmark"
#define WELCOME_MESSAGE "*** %s %i-bits (%s) ***\n", \
        PROGRAM_DESCRIPTION, (int)(sizeof(void*)*8), __DATE__

// fixed number of iterations instead of time for easier CPU event counting
// #define TIMELOOP   2500
#define ITERATIONS 10000
#define NBLOOPS    6
#define PROBATABLESIZE 2048

#define KB *(1<<10)
#define MB *(1<<20)
#define GB *(1<<30)

#define DEFAULT_BLOCKSIZE (64 KB)
#define DEFAULT_PROBA 20

#include <stdlib.h>    // malloc()
#include <stdio.h>     // fprintf()
#include <string.h>    // strcmp()
#include <sys/timeb.h> // timeb()
#include <stdint.h>    // int/uintX_t types
#include <x86intrin.h> // vector intrinsics (depends on -march/-m flag)
#include <malloc.h>    // memalign()
#include <ctype.h>     // isspace()

typedef uint8_t  BYTE;
typedef uint16_t U16;
typedef uint32_t U32;
typedef  int32_t S32;
typedef uint64_t U64;

#ifdef LIKWID
#include <likwid.h>
// cc -g -march=native -std=gnu99 -Wall -Wextra -O3 countbench.c -o countbench -DLIKWID -llikwid -lpthread -lm
// likwid -m -g UOPS_EXECUTED_PORT_PORT_4:PMC0,UOPS_EXECUTED_PORT_PORT_7:PMC1,UOPS_EXECUTED_PORT_PORT_2:PMC2,UOPS_EXECUTED_PORT_PORT_3:PMC3 -C2 countbench -i2   -P90 -b7
#else
#define likwid_markerInit()
#define likwid_markerThreadInit()
#define likwid_markerStartRegion(name)
#define likwid_markerStopRegion(name)
#define likwid_markerClose()
#endif // LIKWID

#ifdef IACA
// cc -march=native -std=gnu11 -Wall -Wextra -O3 -c countbench.c -DIACA -o iaca.o
// iaca  -mark 0 -64 -arch HSW -analysis THROUGHPUT iaca.o
#include </opt/intel/iaca/include/iacaMarks.h>
#else
#define IACA_START
#define IACA_END
#endif

#ifndef DEBUG
#define DEBUG_PRINT(format, args...)
#define DEBUG_ASSERT(test, args...)
#define DEBUG_ALWAYS(actions...)
#define DEBUG_IS_ACTIVE 0
#define DEBUG_ACTIVE(actions...)
#else // DEBUG
// cc -g -march=native -std=gnu99 -Wall -Wextra -O3 countbench.c -o countbench -DDEBUG
// DEBUG=triv* countbench -P90 -b1 | head -2

/* If the program has been compiled with -DDEBUG, debug printing can be 
   turned on using the environment variable DEBUG.  This can be set from 
   the command line as 'DEBUG=foo_* program'. 

      func_exact              -> function exactly 'func_exact'
      func_wild*              -> function starting with 'func_wild'
      *                       -> all functions

   The wildcard character '*' may only go at the end of an identifier.
   Multiple conditions should be comma separated: 'DEBUG=func1_*,func2_*'
   Whitespace before or after a separating comma is allowed and ignored.
*/

#define COMPILER_POSSIBLY_UNUSED __attribute__((unused))   

#define DEBUG_PRINT_INTERNAL(format, args...)                          \
    do {                                                               \
        if (! isspace(*format)) {                                      \
            fprintf(stderr, "%s:%d %s(): ", __FILE__, __LINE__, __func__);  \
        }                                                              \
        fprintf(stderr, format "\n" , ## args);   \
    } while (0)

// abort on error if test fails
#define DEBUG_ASSERT(test, args...)                                    \
    do {                                                               \
        if (!(test)) {                                                 \
            DEBUG_PRINT_INTERNAL("ASSERT FAILED (" #test ")\n" args);  \
            abort();                                                   \
        }                                                              \
    } while (0)        

// print according to $ENV{DEBUG} matching function name
#define DEBUG_PRINT(format, args...)                                   \
    do {                                                               \
        if (debug_should_print(__func__)) {                            \
                DEBUG_PRINT_INTERNAL(format , ## args);                \
        }                                                              \
    } while (0)

// shorthand for #if DEBUG { actions } #endif  (always executes on -DDEBUG)
#define DEBUG_ALWAYS(actions...)  actions

// boolean to determine if $ENV{DEBUG} includes the current fucntions
#define DEBUG_IS_ACTIVE (debug_should_print(__func__))

#define DEBUG_ACTIVE(actions...) if (DEBUG_IS_ACTIVE) { (actions); } 

// strategy is to allocate nothing so nothing leaks
COMPILER_POSSIBLY_UNUSED static int debug_should_print(const char *func) {
    static char *debug_env; // static variable per compilation unit
    
    if (! debug_env) { // cache result of getenv()
        debug_env = getenv("DEBUG");
        if (! debug_env) debug_env = "";
    }

    char *test = debug_env;
    if (*test == '*') return 1; 
    if (*test == '\0') return 0;

    size_t func_len = strlen(func);

    while (1) {
        while(isspace(*test)) test++; // skip leading whitespace
        if (! *test) return 0;        // 'not found' if end of string

        char *next = strchr(test, ',');
        char *last = next ? next - 1 : test + strlen(test) - 1;
        while (isspace(*last)) last--; // skip trailing whitespace
        size_t len = 1 + last - test;
        
        if (test[len - 1] == '*') {   // match to wildcard if there is one
            if (! memcmp(test, func, len - 1)) return 1;
        } else {                      // otherwise must match whole string
            if (func_len == len && !memcmp(test, func, len)) return 1;
        }
        
        if (next) test = next + 1;    // skip over comma and check next
        else return 0;                // 'not found' if no next exists
    }
}

#endif // DEBUG


#define BMK_DISPLAY(...) fprintf(stderr, __VA_ARGS__)

static U32 BMK_GetMilliStart(void)
{
    struct timeb tb;
    U32 nCount;
    ftime( &tb );
    nCount = (U32) (((tb.time & 0xFFFFF) * 1000) +  tb.millitm);
    return nCount;
}

static U32 BMK_GetMilliSpan(U32 nTimeStart)
{
    U32 nCurrent = BMK_GetMilliStart();
    U32 nSpan = nCurrent - nTimeStart;
    if (nTimeStart > nCurrent)
        nSpan += 0x100000 * 1000;
    return nSpan;
}

#define BMK_PRIME1   2654435761U
#define BMK_PRIME2   2246822519U
static U32 BMK_rand (U32* seed)
{
    *seed =  ( (*seed) * BMK_PRIME1) + BMK_PRIME2;
    return (*seed) >> 11;
}

static void BMK_genData(void* buffer, size_t buffSize, double p)
{
    char table[PROBATABLESIZE];
    int remaining = PROBATABLESIZE;
    unsigned pos = 0;
    unsigned s = 0;
    char* op = (char*) buffer;
    char* oend = op + buffSize;
    unsigned seed = 1;
    static unsigned done = 0;

    if (p<0.01) p = 0.005;
    if (p>1.) p = 1.;
    if (!done)
        {
            done = 1;
            BMK_DISPLAY("\nGenerating %i KB with P=%.2f%%\n", (int)(buffSize >> 10), p*100);
        }

    // Build Table
    while (remaining)
        {
            unsigned n = (unsigned)(remaining * p);
            unsigned end;
            if (!n) n=1;
            end = pos + n;
            while (pos<end) table[pos++]=(char)s;
            s++;
            remaining -= n;
        }

    // Fill buffer
    while (op<oend)
        {
            const unsigned r = BMK_rand(&seed) & (PROBATABLESIZE-1);
            *op++ = table[r];
        }
}

int trivialCount(uint8_t* src, size_t srcSize)
{
    U32 count[256];  // 256 counters starting at zero
    memset(count, 0, sizeof(count));

    // increment counter in bin matching each byte
    uint8_t *end  = src + srcSize;
    while (src < end) {
        DEBUG_ACTIVE(printf("%d ", *src * 4));
        count[*src++]++;
    }

    if (DEBUG_IS_ACTIVE) {
        printf("\n");
        for (int i = 0; i < 256; i++) {
            printf("%d ", count[i]);
        }
        printf("\n");
    }

    return count[0];
}

// overallocate individual count tables to avoid 4K aliasing of 
// same byte within different tables (helps avoid CPU misspeculation)
#define COUNT_SIZE (256 + 8)

#define ASM_INC_OFFSET_BASE_INDEX_SCALE(base, offset, index, scale)     \
    __asm volatile ("incl %c0(%1, %2, %c3)":                            \
                    :    /* no registers written (only memory) */       \
                    "i" (offset), /* constant array offset */           \
                    "r" (base),  /* read only */                        \
                    "r" (index),  /* read only */                       \
                    "i" (scale):  /* constant [1,2,4,8] */              \
                    "memory" /* clobbers */                             \
                    )

// This function is limited by the number of uops in the loop.  Sustained throughput is
// limited to 4 per cycle, and we use about 100.   If you can figure out how to reduce the number
// of uops in the loop (uops, not instructions) this loop should run faster.  

// try using vector shift with extract to utilize port 7

typedef __m128i xmm_t;
int count_vec(uint8_t *src, size_t srcSize)
{
    static U32 count[16][COUNT_SIZE];
    memset(count, 0, sizeof(count));
    size_t remainder = srcSize % 16;
    srcSize = srcSize - remainder;
    xmm_t nextVec = _mm_loadu_si128((xmm_t *)&src[0]);

    /* IACA_START; */
    for (size_t i = 16; i <= srcSize; i += 16) {

        uint64_t byte;

        xmm_t vec = nextVec;
        nextVec = _mm_loadu_si128((xmm_t *)&src[i]);

        byte = _mm_extract_epi8(vec, 0);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 0, byte, 4);

        byte = _mm_extract_epi8(vec, 1);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 1, byte, 4);

        byte = _mm_extract_epi8(vec, 2);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 2, byte, 4);

        byte = _mm_extract_epi8(vec, 3);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 3, byte, 4);

        byte = _mm_extract_epi8(vec, 4);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 4, byte, 4);

        byte = _mm_extract_epi8(vec, 5);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 5, byte, 4);

        byte = _mm_extract_epi8(vec, 6);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 6, byte, 4);

        byte = _mm_extract_epi8(vec, 7);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 7, byte, 4);

        byte = _mm_extract_epi8(vec, 8);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 8, byte, 4);

        byte = _mm_extract_epi8(vec, 9);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 9, byte, 4);

        byte = _mm_extract_epi8(vec, 10);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 10, byte, 4);

        byte = _mm_extract_epi8(vec, 11);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 11, byte, 4);

        byte = _mm_extract_epi8(vec, 12);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 12, byte, 4);

        byte = _mm_extract_epi8(vec, 13);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 13, byte, 4);

        byte = _mm_extract_epi8(vec, 14);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 14, byte, 4);

        byte = _mm_extract_epi8(vec, 15);
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, COUNT_SIZE * 4 * 15, byte, 4);

    }

    src += srcSize;  // skip over the finished part
    for (size_t i = 0; i < remainder; i++) {
        uint64_t byte = src[i];
        ASM_INC_OFFSET_BASE_INDEX_SCALE(count, 0, byte, 4);
    }

    // sum 256 byte counters in 16 separate arrays into count[0][byte]
    for (int i = 0; i < 256; i++) {
        for (int idx=1; idx < 16; idx++) {
            count[0][i] += count[idx][i];
        }
    }

    return count[0][0];
}


// Port 7 friendly increment
#define ASM_INC_OFFSET_BASE_INDEX(offset, base, index)                  \
    __asm volatile (                                                    \
                    "incl %c0+%c1(%2)\n":                               \
                    : /* nothing written except memory */               \
                    "i" (offset),  /* which array */                    \
                    "p" (base),    /* array base */                     \
                    "r" (index):   /* pre-shifted index */              \
                    "memory" /* clobbers */                             \
                                                                        )

#define ASM_LOAD_WORD_FROM_BUFFER(buffer, offset, dest)                 \
    __asm volatile (                                                    \
                    "movzwl %c1(%2), %k0\n":                            \
                    "=r" (dest) :  /* 32-bit write */                   \
                    "i" (offset),  /* byte offset */                    \
                    "r" (buffer)  /* buffer base */                     \
                                                                        )


#define ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(offset, ptr, index, scale, destVec) \
    __asm volatile (                                                    \
                    "vpmovzxbw %c1(%2, %3, %c4), %0\n":                 \
                    "=x" (destVec) :  /* vector write */                \
                    "i" (offset),                                       \
                    "r" (ptr),   /* ptr */                              \
                    "r" (index),  /* offset */                          \
                    "i" (scale)  /* [1,2,4,8] */                        \
                                                                        )

#define ASM_COMPILER_MEM_BARRIER()                                      \
    __asm volatile( "" /* no asm */ :                                   \
                    /* nothing written */ :                             \
                    /* nothing read */ :                                \
                    /* pretend to clobber */ "memory");


#ifdef __AVX2__
typedef __m256i ymm_t;
int port7vec(uint8_t *src, size_t srcSize)
{
    static U32 count[16][COUNT_SIZE];
    memset(count, 0, sizeof(count));

    // 2x32B buffers with 64B alignment
    uint8_t *buffer = memalign(2*32, 64); 
    
    // index == byte * 4 (pre-shifted)
    uint64_t index0, index1, index2, index3;
    uint64_t index4, index5, index6, index7;

    ASM_COMPILER_MEM_BARRIER();

    ymm_t vec0, vec1;
    vec0 = _mm256_cvtepu8_epi16(*(xmm_t *)&src[0]);
    vec0 = _mm256_slli_epi16(vec0, 2);
    _mm256_store_si256((ymm_t *)buffer, vec0);

    ASM_COMPILER_MEM_BARRIER();

    DEBUG_PRINT("Original src %p endSrc %p loaded %p ", src, src + srcSize, &src[0]);
    
    // start with registers loaded
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 0 * 2, index0);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 1 * 2, index1);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 2 * 2, index2);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 3 * 2, index3);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 4 * 2, index4);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 5 * 2, index5);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 6 * 2, index6);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 7 * 2, index7);
    src += 8;
    srcSize -= 8;

    size_t remainder = srcSize % 32;
    remainder += 64;
    srcSize = srcSize - remainder;
    uint8_t *endSrc = src + srcSize;
    int64_t negCount = -srcSize;


    // need to start with both buffers full
    ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(0, endSrc, negCount, 1, vec1);
    vec1 = _mm256_slli_epi16(vec1, 2);
    _mm256_store_si256((ymm_t *)buffer, vec1);

    ASM_COMPILER_MEM_BARRIER();

    ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(16, endSrc, negCount, 1, vec0);
    vec0 = _mm256_slli_epi16(vec0, 2);
    _mm256_store_si256((ymm_t *)(buffer + 32), vec0);

    ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(32, endSrc, negCount, 1, vec0); 

    IACA_START;

    while (negCount != 0) {
        // software prefetch because hardware does not cross 4K pages
        _mm_prefetch((const char *)(endSrc + negCount + 768), 3);

        vec0 = _mm256_slli_epi16(vec0, 2);

        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 0, index0);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 0 * 2, index0);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 1, index1);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 1 * 2, index1);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 2, index2);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 2 * 2, index2);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 3, index3);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 3 * 2, index3);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 4, index4);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 4 * 2, index4);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 5, index5);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 5 * 2, index5);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 6, index6);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 6 * 2, index6);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 7, index7);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 7 * 2, index7);

        ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(48, endSrc, negCount, 1, vec1); 

        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 8, index0);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 8 * 2, index0);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 9, index1);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 9 * 2, index1);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 10, index2);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 10 * 2, index2);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 11, index3);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 11 * 2, index3);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 12, index4);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 12 * 2, index4);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 13, index5);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 13 * 2, index5);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 14, index6);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 14 * 2, index6);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 15, index7);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 15 * 2, index7);


        vec1 = _mm256_slli_epi16(vec1, 2);
        _mm256_store_si256((ymm_t *)buffer, vec0);


        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 0, index0);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 0 * 2, index0);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 1, index1);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 1 * 2, index1);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 2, index2);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 2 * 2, index2);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 3, index3);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 3 * 2, index3);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 4, index4);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 4 * 2, index4);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 5, index5);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 5 * 2, index5);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 6, index6);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 6 * 2, index6);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 7, index7);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 7 * 2, index7);

        //        ASM_COMPILER_MEM_BARRIER();
        ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(64, endSrc, negCount, 1, vec0); 

        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 8, index0);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 8 * 2, index0);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 9, index1);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 9 * 2, index1);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 10, index2);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 10 * 2, index2);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 11, index3);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 11 * 2, index3);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 12, index4);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 12 * 2, index4);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 13, index5);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 13 * 2, index5);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 14, index6);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 14 * 2, index6);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 15, index7);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 15 * 2, index7);

        _mm256_store_si256((ymm_t *)(buffer + 32), vec1);

        negCount += 32;
    }

    IACA_END;

    remainder += 8;  // loaded to registers but unused
    endSrc -= 8;     // portion that needs to be reread

    DEBUG_PRINT("|");
    for (size_t i = 0; i < remainder; i++) {
        uint64_t byte = endSrc[i];
        DEBUG_PRINT("%ld ", byte * 4);
        count[0][byte]++;
    }
    DEBUG_PRINT("\n");

    // sum 256 byte counters in 16 separate arrays into count[0][byte]
    for (int i = 0; i < 256; i++) {
        for (int idx=1; idx < 16; idx++) {
            count[0][i] += count[idx][i];
        }
        DEBUG_PRINT("%d ", count[0][i]);
    }
    DEBUG_PRINT("\n");


    free(buffer);

    return count[0][0];
}
#endif // __AVX2__

int vecavx(uint8_t *src, size_t srcSize)
{
    static U32 count[16][COUNT_SIZE];
    memset(count, 0, sizeof(count));

    // 4x16B buffers with 64B alignment (overcommit for same offsets as AVX2)
    uint8_t *buffer = memalign(4*16, 64); 
    
    // index == byte * 4 (pre-shifted)
    uint64_t index0, index1, index2, index3;
    uint64_t index4, index5, index6, index7;

    xmm_t vec0, vec1, vec2, vec3;
    vec0 = _mm_cvtepu8_epi16(*(xmm_t *)&src[0]);
    vec0 = _mm_slli_epi16(vec0, 2);
    vec1 = _mm_cvtepu8_epi16(*(xmm_t *)&src[16]);
    vec1 = _mm_slli_epi16(vec1, 2);
    _mm_store_si128((xmm_t *)(buffer +  0), vec0);
    _mm_store_si128((xmm_t *)(buffer + 16), vec1);

    ASM_COMPILER_MEM_BARRIER();

    // start with registers loaded
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 0 * 2, index0);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 1 * 2, index1);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 2 * 2, index2);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 3 * 2, index3);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 4 * 2, index4);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 5 * 2, index5);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 6 * 2, index6);
    ASM_LOAD_WORD_FROM_BUFFER(buffer, 7 * 2, index7);
    src += 8;
    srcSize -= 8;


    size_t remainder = srcSize % 32;
    remainder += 64;
    srcSize = srcSize - remainder;
    uint8_t *endSrc = src + srcSize;
    int64_t negCount = -srcSize;

    // need to start with both buffers full
    ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(0, endSrc, negCount, 1, vec2);
    ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(8, endSrc, negCount, 1, vec3);
    vec2 = _mm_slli_epi16(vec2, 2);
    _mm_store_si128((xmm_t *)(buffer + 0), vec2);
    vec3 = _mm_slli_epi16(vec3, 2);
    _mm_store_si128((xmm_t *)(buffer + 16), vec3);


    ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(16, endSrc, negCount, 1, vec0);
    ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(24, endSrc, negCount, 1, vec1);
    vec0 = _mm_slli_epi16(vec0, 2);
    _mm_store_si128((xmm_t *)(buffer + 32), vec0);
    vec1 = _mm_slli_epi16(vec1, 2);
    _mm_store_si128((xmm_t *)(buffer + 48), vec1);

    ASM_COMPILER_MEM_BARRIER();

    ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(32, endSrc, negCount, 1, vec0); 
    ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(40, endSrc, negCount, 1, vec1); 

    IACA_START;

    while (negCount != 0) {
        _mm_prefetch((const char *)(endSrc + negCount + 768), 3);
        vec0 = _mm_slli_epi16(vec0, 2);
        vec1 = _mm_slli_epi16(vec0, 2);

        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 0, index0);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 0 * 2, index0);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 1, index1);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 1 * 2, index1);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 2, index2);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 2 * 2, index2);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 3, index3);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 3 * 2, index3);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 4, index4);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 4 * 2, index4);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 5, index5);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 5 * 2, index5);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 6, index6);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 6 * 2, index6);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 7, index7);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 7 * 2, index7);

        ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(48, endSrc, negCount, 1, vec2); 
        ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(56, endSrc, negCount, 1, vec3); 

        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 8, index0);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 8 * 2, index0);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 9, index1);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 9 * 2, index1);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 10, index2);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 10 * 2, index2);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 11, index3);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 11 * 2, index3);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 12, index4);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 12 * 2, index4);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 13, index5);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 13 * 2, index5);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 14, index6);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 14 * 2, index6);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 15, index7);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 15 * 2, index7);


        vec2 = _mm_slli_epi16(vec2, 2);
        vec3 = _mm_slli_epi16(vec3, 2);
        _mm_store_si128((xmm_t *)(buffer + 0), vec0);
        _mm_store_si128((xmm_t *)(buffer + 16), vec1);

        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 0, index0);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 0 * 2, index0);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 1, index1);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 1 * 2, index1);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 2, index2);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 2 * 2, index2);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 3, index3);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 3 * 2, index3);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 4, index4);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 4 * 2, index4);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 5, index5);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 5 * 2, index5);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 6, index6);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 6 * 2, index6);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 7, index7);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 7 * 2, index7);

        ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(64, endSrc, negCount, 1, vec0); 
        ASM_LOAD_VEC_BYTE_TO_WORD_OFFSET_PTR_INDEX_SCALE(72, endSrc, negCount, 1, vec1); 

        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 8, index0);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 8 * 2, index0);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 9, index1);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 9 * 2, index1);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 10, index2);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 10 * 2, index2);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 11, index3);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 11 * 2, index3);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 12, index4);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 12 * 2, index4);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 13, index5);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 13 * 2, index5);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 14, index6);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 14 * 2, index6);
        ASM_INC_OFFSET_BASE_INDEX(count, COUNT_SIZE * 4 * 15, index7);
        ASM_LOAD_WORD_FROM_BUFFER(buffer, 32 + 15 * 2, index7);

        _mm_store_si128((xmm_t *)(buffer + 32), vec2);
        _mm_store_si128((xmm_t *)(buffer + 48), vec3);


        negCount += 32;
    }

    IACA_END;

    remainder += 8;  // loaded to registers but unused
    endSrc -= 8;

    DEBUG_PRINT("|");
    for (size_t i = 0; i < remainder; i++) {
        uint64_t byte = endSrc[i];
        DEBUG_PRINT("%ld ", byte * 4);
        count[0][byte]++;
    }
    DEBUG_PRINT("\n");

    // sum 256 byte counters in 16 separate arrays into count[0][byte]
    for (int i = 0; i < 256; i++) {
        for (int idx=1; idx < 16; idx++) {
            count[0][i] += count[idx][i];
        }
        DEBUG_PRINT("%d ", count[0][i]);
    }
    DEBUG_PRINT("\n");

    free(buffer);

    return count[0][0];
}



// increment a Haswell Port 7 friendly address [constOffset + byte * 4]
// trying to arrange order to more stores actually happen on Port 7
#define ASM_INC_GLOBAL_OFFSET_MULTIPLE_BYTES(global, byte0, byte1, byte2, byte3, \
                                             tmp32_0, tmp32_1, tmp32_2, tmp32_3, \
                                             offset0, offset1, offset2, offset3) \
    __asm volatile (                                                    \
                    "shl $2, %0\n"      /* byte0 *= 2 */                 \
                    "shl $2, %1\n"      /* byte1 *= 2 */                 \
                    "shl $2, %2\n"      /* byte2 *= 2 */                 \
                    "shl $2, %3\n"      /* byte3 *= 2 */                 \
                    "movl " #global "+%c8(%0), %4\n"                    \
                    "movl " #global "+%c9(%1), %5\n"                   \
                    "movl " #global "+%c10(%2), %6\n"                   \
                    "movl " #global "+%c11(%3), %7\n"                   \
                    "incl %4\n"                                         \
                    "incl %5\n"                                         \
                    "incl %6\n"                                         \
                    "incl %7\n"                                         \
                    "movl %4, " #global "+%c8(%0)\n"                    \
                    "movl %5, " #global "+%c9(%1)\n"                   \
                    "movl %6, " #global "+%c10(%2)\n"                   \
                    "movl %7, " #global "+%c11(%3)\n":                  \
                    "+r" (byte0),  /* read and write */                 \
                    "+r" (byte1),  /* read and write */                 \
                    "+r" (byte2),  /* read and write */                 \
                    "+r" (byte3),  /* read and write */                 \
                    "=r" (tmp32_0),                                     \
                    "=r" (tmp32_1),                                     \
                    "=r" (tmp32_2),                                     \
                    "=r" (tmp32_3):                                     \
                    "i" (offset0),                                      \
                    "i" (offset1),                                      \
                    "i" (offset2),                                      \
                    "i" (offset3):                                      \
                    "memory" /* clobbers */                             \
                                                                        )

static U32 g_count[16][COUNT_SIZE];

int storePort7(uint8_t *src, size_t srcSize)
{
    size_t remainder = srcSize; // initially only
    memset(g_count, 0, sizeof(g_count));
    if (srcSize < 32) {  // or some small number
        goto handle_remainder;
    }

    // preload for first loop iteration
    uint64_t byte0, byte1, byte2, byte3;
    uint64_t byte4 = *(src + 0);
    uint64_t byte5 = *(src + 1);
    uint64_t byte6 = *(src + 2);
    uint64_t byte7 = *(src + 3);
    src += 4;
    srcSize -= 4;

    remainder = srcSize % 16;
    remainder += 16;  // allow for some safe read-ahead
    srcSize = srcSize - remainder;
    uint8_t *endSrc = src + srcSize;

    IACA_START;
    while (src < endSrc) {
        uint32_t tmp32_0, tmp32_1, tmp32_2, tmp32_3;
        
        byte0 = *(src + 0);
        byte1 = *(src + 1);
        byte2 = *(src + 2);
        byte3 = *(src + 3);

        ASM_INC_GLOBAL_OFFSET_MULTIPLE_BYTES(g_count, byte4, byte5, byte6, byte7, 
                                             tmp32_0, tmp32_1, tmp32_2, tmp32_3,
                                             COUNT_SIZE * 4 * 0, COUNT_SIZE * 4 * 1, 
                                             COUNT_SIZE * 4 * 2, COUNT_SIZE * 4 * 3);
        byte4 = *(src + 4);
        byte5 = *(src + 5);
        byte6 = *(src + 6);
        byte7 = *(src + 7);
        ASM_INC_GLOBAL_OFFSET_MULTIPLE_BYTES(g_count, byte0, byte1, byte2, byte3, 
                                             tmp32_0, tmp32_1, tmp32_2, tmp32_3,
                                             COUNT_SIZE * 4 * 4, COUNT_SIZE * 4 * 5, 
                                             COUNT_SIZE * 4 * 6, COUNT_SIZE * 4 * 7);

        byte0 = *(src + 8);
        byte1 = *(src + 9);
        byte2 = *(src + 10);
        byte3 = *(src + 11);
        ASM_INC_GLOBAL_OFFSET_MULTIPLE_BYTES(g_count, byte4, byte5, byte6, byte7, 
                                             tmp32_0, tmp32_1, tmp32_2, tmp32_3,
                                             COUNT_SIZE * 4 * 8, COUNT_SIZE * 4 * 9, 
                                             COUNT_SIZE * 4 * 10, COUNT_SIZE * 4 * 11);
        byte4 = *(src + 12);
        byte5 = *(src + 13);
        byte6 = *(src + 14);
        byte7 = *(src + 15);
        ASM_INC_GLOBAL_OFFSET_MULTIPLE_BYTES(g_count, byte0, byte1, byte2, byte3, 
                                             tmp32_0, tmp32_1, tmp32_2, tmp32_3,
                                             COUNT_SIZE * 4 * 12, COUNT_SIZE * 4 * 13, 
                                             COUNT_SIZE * 4 * 14, COUNT_SIZE * 4 * 15);


        src += 16;
        IACA_END;
    }


    src -= 4; // redo last four that preloaded
    remainder += 4;

 handle_remainder:
    for (size_t i = 0; i < remainder; i++) {
        uint64_t byte = src[i];
        g_count[0][byte]++;
    }

    // sum 256 byte counters in 16 separate arrays into count[0][byte]
    for (int i = 0; i < 256; i++) {
        for (int idx=1; idx < 16; idx++) {
            g_count[0][i] += g_count[idx][i];
        }
    }

    return g_count[0][0];
}

#define ASM_INC_GLOBAL_OFFSET_MULTIPLE_BYTES_RELOAD(global, src, srcOffset, \
                                                    byte0, byte1, byte2, byte3, \
                                                    tmp32_0, tmp32_1, tmp32_2, tmp32_3, \
                                                    offset0, offset1, offset2, offset3) \
    __asm volatile (                                                    \
                    "shl $2, %0\n"      /* byte0 *= 2 */                \
                    "shl $2, %1\n"      /* byte1 *= 2 */                \
                    "shl $2, %2\n"      /* byte2 *= 2 */                \
                    "shl $2, %3\n"      /* byte3 *= 2 */                \
                    "movl " #global "+%c8(%0), %4\n"                    \
                    "movl " #global "+%c9(%1), %5\n"                    \
                    "movl " #global "+%c10(%2), %6\n"                   \
                    "movl " #global "+%c11(%3), %7\n"                   \
                    "incl %4\n"                                         \
                    "incl %5\n"                                         \
                    "incl %6\n"                                         \
                    "incl %7\n"                                         \
                    "movl %4, " #global "+%c8(%0)\n"                    \
                    "movzbl %c13+0(%12), %k0\n"                         \
                    "movl %5, " #global "+%c9(%1)\n"                    \
                    "movzbl %c13+1(%12), %k1\n"                         \
                    "movl %6, " #global "+%c10(%2)\n"                   \
                    "movzbl %c13+2(%12), %k2\n"                         \
                    "movl %7, " #global "+%c11(%3)\n"                   \
                    "movzbl %c13+3(%12), %k3\n" :                       \
                    "+r" (byte0),  /* read and write */                 \
                    "+r" (byte1),  /* read and write */                 \
                    "+r" (byte2),  /* read and write */                 \
                    "+r" (byte3),  /* read and write */                 \
                    "=r" (tmp32_0),                                     \
                    "=r" (tmp32_1),                                     \
                    "=r" (tmp32_2),                                     \
                    "=r" (tmp32_3):                                     \
                    "i" (offset0),                                      \
                    "i" (offset1),                                      \
                    "i" (offset2),                                      \
                    "i" (offset3),                                      \
                    "r" (src),                                          \
                    "i" (srcOffset):                                    \
                    "memory" /* clobbers */                             \
                                                                        )


int reloadPort7(uint8_t *src, size_t srcSize)
{
    size_t remainder = srcSize; // initially only
    memset(g_count, 0, sizeof(g_count));
    if (srcSize < 32) {  // or some small number
        goto handle_remainder;
    }

    // preload for first loop iteration
    uint64_t byte0 = *(src + 0);
    uint64_t byte1 = *(src + 1);
    uint64_t byte2 = *(src + 2);
    uint64_t byte3 = *(src + 3);
    uint64_t byte4 = *(src + 4);
    uint64_t byte5 = *(src + 5);
    uint64_t byte6 = *(src + 6);
    uint64_t byte7 = *(src + 7);
    src += 8;
    srcSize -= 8;

    remainder = srcSize % 16;
    remainder += 16;  // allow for some safe read-ahead
    srcSize = srcSize - remainder;
    uint8_t *endSrc = src + srcSize;

    IACA_START;
    while (src < endSrc) {
        uint32_t tmp32_0, tmp32_1, tmp32_2, tmp32_3;
        
        ASM_INC_GLOBAL_OFFSET_MULTIPLE_BYTES_RELOAD(g_count, src, 0, byte0, byte1, byte2, byte3, 
                                                    tmp32_0, tmp32_1, tmp32_2, tmp32_3,
                                                    COUNT_SIZE * 4 * 0, COUNT_SIZE * 4 * 1, 
                                                    COUNT_SIZE * 4 * 2, COUNT_SIZE * 4 * 3);

        ASM_INC_GLOBAL_OFFSET_MULTIPLE_BYTES_RELOAD(g_count, src, 4, byte4, byte5, byte6, byte7, 
                                                    tmp32_0, tmp32_1, tmp32_2, tmp32_3,
                                                    COUNT_SIZE * 4 * 4, COUNT_SIZE * 4 * 5, 
                                                    COUNT_SIZE * 4 * 6, COUNT_SIZE * 4 * 7);

        ASM_INC_GLOBAL_OFFSET_MULTIPLE_BYTES_RELOAD(g_count, src, 8, byte0, byte1, byte2, byte3, 
                                                    tmp32_0, tmp32_1, tmp32_2, tmp32_3,
                                                    COUNT_SIZE * 4 * 8, COUNT_SIZE * 4 * 9, 
                                                    COUNT_SIZE * 4 * 10, COUNT_SIZE * 4 * 11);
        
        ASM_INC_GLOBAL_OFFSET_MULTIPLE_BYTES_RELOAD(g_count, src, 12, byte4, byte5, byte6, byte7, 
                                                    tmp32_0, tmp32_1, tmp32_2, tmp32_3,
                                                    COUNT_SIZE * 4 * 12, COUNT_SIZE * 4 * 13, 
                                                    COUNT_SIZE * 4 * 14, COUNT_SIZE * 4 * 15);


        src += 16;
        IACA_END;

    }


    src -= 8; // redo last four that preloaded
    remainder += 8;

 handle_remainder:
    for (size_t i = 0; i < remainder; i++) {
        uint64_t byte = src[i];
        g_count[0][byte]++;
    }

    // sum 256 byte counters in 16 separate arrays into count[0][byte]
    for (int i = 0; i < 256; i++) {
        for (int idx=1; idx < 16; idx++) {
            g_count[0][i] += g_count[idx][i];
        }
    }

    return g_count[0][0];
}

#define ASM_INC_OFFSET_BASE_BYTE_MULTIPLE_RELOAD(offset0, offset1, offset2, offset3, \
                                                 base, byte0, byte1, byte2, byte3, \
                                                 src, srcOffset)        \
    __asm volatile (                                                    \
                    "shl $2, %0\n"                                      \
                    "incl %c8+%c4(%0)\n"                        \
                    "movzbl %c10+0(%9), %k0\n"                          \
                    "shl $2, %1\n"                                      \
                    "incl %c8+%c5(%1)\n"                        \
                    "movzbl %c10+1(%9), %k1\n"                         \
                    "shl $2, %2\n"                                      \
                    "incl %c8+%c6(%2)\n"                        \
                    "movzbl %c10+2(%9), %k2\n"                         \
                    "shl $2, %3\n"                                      \
                    "incl %c8+%c7(%3)\n"                        \
                    "movzbl %c10+3(%9), %k3\n":                        \
                    "+&r" (byte0),  /* read and write */                 \
                    "+&r" (byte1),  /* read and write */                 \
                    "+&r" (byte2),  /* read and write */                 \
                    "+&r" (byte3):  /* read and write */                \
                    "i" (offset0),                                      \
                    "i" (offset1),                                      \
                    "i" (offset2),                                      \
                    "i" (offset3),                                      \
                    "p" (base),                                         \
                    "r" (src),                                          \
                    "i" (srcOffset):                                    \
                    "memory" /* clobbers */                             \
                                                                        )

int count8reload(uint8_t *src, size_t srcSize)
{
    static U32 count[16][COUNT_SIZE];
    memset(count, 0, sizeof(count));

    size_t remainder = srcSize; // initially only
    if (srcSize < 32) {  // or some small number
        goto handle_remainder;
    }

    // preload for first loop iteration
    uint64_t byte0 = *(src + 0);
    uint64_t byte1 = *(src + 1);
    uint64_t byte2 = *(src + 2);
    uint64_t byte3 = *(src + 3);
    uint64_t byte4 = *(src + 4);
    uint64_t byte5 = *(src + 5);
    uint64_t byte6 = *(src + 6);
    uint64_t byte7 = *(src + 7);
    src += 8;
    srcSize -= 8;

    remainder = srcSize % 16;
    remainder += 16;  // allow for some safe read-ahead
    srcSize = srcSize - remainder;
    uint8_t *endSrc = src + srcSize;

    IACA_START;
    while (src < endSrc) {
        
        ASM_INC_OFFSET_BASE_BYTE_MULTIPLE_RELOAD(COUNT_SIZE * 4 * 0, COUNT_SIZE * 4 * 1, 
                                                 COUNT_SIZE * 4 * 2, COUNT_SIZE * 4 * 3,
                                                 count, byte0, byte1, byte2, byte3, 
                                                 src, 0);

        ASM_INC_OFFSET_BASE_BYTE_MULTIPLE_RELOAD(COUNT_SIZE * 4 * 4, COUNT_SIZE * 4 * 5, 
                                                 COUNT_SIZE * 4 * 6, COUNT_SIZE * 4 * 7,
                                                 count, byte4, byte5, byte6, byte7, 
                                                 src, 4);

        ASM_INC_OFFSET_BASE_BYTE_MULTIPLE_RELOAD(COUNT_SIZE * 4 * 8, COUNT_SIZE * 4 * 9, 
                                                 COUNT_SIZE * 4 * 10, COUNT_SIZE * 4 * 11,
                                                 count, byte0, byte1, byte2, byte3, 
                                                 src, 8);

        ASM_INC_OFFSET_BASE_BYTE_MULTIPLE_RELOAD(COUNT_SIZE * 4 * 12, COUNT_SIZE * 4 * 13, 
                                                 COUNT_SIZE * 4 * 14, COUNT_SIZE * 4 * 15,
                                                 count, byte4, byte5, byte6, byte7, 
                                                 src, 12);



        src += 16;
        IACA_END;

    }


    src -= 8; // redo last 8 that were preloaded
    remainder += 8;

 handle_remainder:
    for (size_t i = 0; i < remainder; i++) {
        uint64_t byte = src[i];
        count[0][byte]++;
    }

    // sum 256 byte counters in 16 separate arrays into count[0][byte]
    for (int i = 0; i < 256; i++) {
        for (int idx=1; idx < 16; idx++) {
            count[0][i] += count[idx][i];
        }
    }

    return count[0][0];
}

#define ASM_SHIFT_RIGHT(reg, bitsToShift)                                \
    __asm volatile ("shr %1, %0":                                       \
                    "+r" (reg): /* read and written */                  \
                    "i" (bitsToShift) /* constant */                    \
                    )


#define ASM_INC_TABLES(src0, src1, byte0, byte1, offset, size, base, scale) \
    __asm volatile ("movzbl %b2, %k0\n"                /* byte0 = src0 & 0xFF */ \
                    "movzbl %b3, %k1\n"                /* byte1 = src1 & 0xFF */ \
                    "incl (%c4+0)*%c5(%6, %0, %c7)\n"  /* count[i+0][byte0]++ */ \
                    "incl (%c4+1)*%c5(%6, %1, %c7)\n"  /* count[i+1][byte1]++ */ \
                    "movzbl %h2, %k0\n"                /* byte0 = (src0 & 0xFF00) >> 8 */ \
                    "movzbl %h3, %k1\n"                /* byte1 = (src1 & 0xFF00) >> 8 */ \
                    "incl (%c4+2)*%c5(%6, %0, %c7)\n"  /* count[i+2][byte0]++ */ \
                    "incl (%c4+3)*%c5(%6, %1, %c7)\n": /* count[i+3][byte1]++ */ \
                    "=&R" (byte0),  /* write only (R == non REX) */     \
                    "=&R" (byte1):  /* write only (R == non REX) */     \
                    "Q" (src0),  /* read only (Q == must have rH) */    \
                    "Q" (src1),  /* read only (Q == must have rH) */    \
                    "i" (offset), /* constant array offset */           \
                    "i" (size), /* constant array size     */           \
                    "r" (base),  /* read only array address */          \
                    "i" (scale):  /* constant [1,2,4,8] */              \
                    "memory" /* clobbered (forces compiler to compute sum ) */ \
                    )

int count2x64(uint8_t *src, size_t srcSize)
{
    U64 remainder = srcSize;
    if (srcSize < 32) goto handle_remainder;

    U32 count[16][COUNT_SIZE];
    memset(count, 0, sizeof(count));
    
    remainder = srcSize % 16;
    srcSize -= remainder;  
    const BYTE *endSrc = src + srcSize;
    U64 next0 = *(U64 *)(src + 0);
    U64 next1 = *(U64 *)(src + 8);

    IACA_START;

    while (src != endSrc)
    {
        U64 byte0, byte1;
        U64 data0 = next0;
        U64 data1 = next1;

        src += 16;
        next0 = *(U64 *)(src + 0);
        next1 = *(U64 *)(src + 8);

        ASM_INC_TABLES(data0, data1, byte0, byte1, 0, COUNT_SIZE * 4, count, 4);

        ASM_SHIFT_RIGHT(data0, 16);
        ASM_SHIFT_RIGHT(data1, 16);
        ASM_INC_TABLES(data0, data1, byte0, byte1, 4, COUNT_SIZE * 4, count, 4);

        ASM_SHIFT_RIGHT(data0, 16);
        ASM_SHIFT_RIGHT(data1, 16);
        ASM_INC_TABLES(data0, data1, byte0, byte1, 8, COUNT_SIZE * 4, count, 4);

        ASM_SHIFT_RIGHT(data0, 16);
        ASM_SHIFT_RIGHT(data1, 16);
        ASM_INC_TABLES(data0, data1, byte0, byte1, 12, COUNT_SIZE * 4, count, 4);
    }

    IACA_END;

 handle_remainder:
    for (size_t i = 0; i < remainder; i++) {
        uint64_t byte = src[i];
        count[0][byte]++;
    }

    for (int i = 0; i < 256; i++) {
        for (int idx=1; idx < 16; idx++) {
            count[0][i] += count[idx][i];
        }
    }

    return count[0][0];
}


// hist_X_Y functions from https://github.com/powturbo/turbohist

int hist_4_32(uint8_t *in, size_t inlen) { 
#define NU 16
    int i;
    unsigned bin[COUNT_SIZE]={0};
    unsigned c0[COUNT_SIZE]={0},c1[COUNT_SIZE]={0},c2[COUNT_SIZE]={0},c3[COUNT_SIZE]={0}; 
    unsigned char *ip;

    unsigned cp = *(unsigned *)in;
    for(ip = in; ip != in+(inlen&~(NU-1));) {
        unsigned c = cp; ip += 4; cp = *(unsigned *)ip;
        c0[(unsigned char)c      ]++;
        c1[(unsigned char)(c>>8) ]++;
        c2[(unsigned char)(c>>16)]++;
        c3[c>>24                 ]++;

        c = cp; ip += 4; cp = *(unsigned *)ip;
        c0[(unsigned char)c      ]++;
        c1[(unsigned char)(c>>8) ]++;
        c2[(unsigned char)(c>>16)]++;
        c3[c>>24                 ]++;

        c = cp; ip += 4; cp = *(unsigned *)ip;
        c0[(unsigned char)c      ]++;
        c1[(unsigned char)(c>>8) ]++;
        c2[(unsigned char)(c>>16)]++;
        c3[c>>24                 ]++;

        c = cp; ip += 4; cp = *(unsigned *)ip;
        c0[(unsigned char)c      ]++;
        c1[(unsigned char)(c>>8) ]++;
        c2[(unsigned char)(c>>16)]++;
        c3[c>>24                 ]++;
    }
    while(ip < in+inlen) c0[*ip++]++; 
    for(i = 0; i < 256; i++) 
        bin[i] = c0[i]+c1[i]+c2[i]+c3[i];

    return bin[0];
#undef NU
}

int hist_4_64(uint8_t *in, size_t inlen) { 
    int i;
    unsigned bin[COUNT_SIZE]={0};
    unsigned c0[COUNT_SIZE]={0},c1[COUNT_SIZE]={0},c2[COUNT_SIZE]={0},c3[COUNT_SIZE]={0}; 
    unsigned char *ip;

    unsigned long long cp = *(unsigned long long *)in;
    for(ip = in; ip != in+(inlen&~(16-1)); ) {    
        unsigned long long c = cp; ip += 8; cp = *(unsigned long long *)ip; 
        c0[(unsigned char) c     ]++;
        c1[(unsigned char)(c>> 8)]++;
        c2[(unsigned char)(c>>16)]++;
        c3[(unsigned char)(c>>24)]++;
        c0[(unsigned char)(c>>32)]++;
        c1[(unsigned char)(c>>40)]++;
        c2[(unsigned char)(c>>48)]++;
        c3[                c>>56 ]++;

        c = cp; ip += 8; cp = *(unsigned long long *)ip; 
        c0[(unsigned char) c    ]++;
        c1[(unsigned char)(c>> 8)]++;
        c2[(unsigned char)(c>>16)]++;
        c3[(unsigned char)(c>>24)]++;
        c0[(unsigned char)(c>>32)]++;
        c1[(unsigned char)(c>>40)]++;
        c2[(unsigned char)(c>>48)]++;
        c3[                c>>56 ]++;
    }
    while(ip < in+inlen) c0[*ip++]++; 
    for(i = 0; i < 256; i++) 
        bin[i] = c0[i]+c1[i]+c2[i]+c3[i];

    return bin[0];
}

int hist_8_64(uint8_t *in, size_t inlen) { 
    int i;
    unsigned bin[COUNT_SIZE]={0};
    unsigned c0[COUNT_SIZE]={0},c1[COUNT_SIZE]={0},c2[COUNT_SIZE]={0},c3[COUNT_SIZE]={0},c4[COUNT_SIZE]={0},c5[COUNT_SIZE]={0},c6[COUNT_SIZE]={0},c7[COUNT_SIZE]={0}; 
    unsigned char *ip;

    unsigned long long cp = *(unsigned long long *)in;
    for(ip = in; ip != in+(inlen&~(16-1)); ) {    
        unsigned long long c = cp; ip += 8; cp = *(unsigned long long *)ip; 
        c0[(unsigned char) c     ]++;
        c1[(unsigned char)(c>>8) ]++;
        c2[(unsigned char)(c>>16)]++;
        c3[(unsigned char)(c>>24)]++;
        c4[(unsigned char)(c>>32)]++;
        c5[(unsigned char)(c>>40)]++;
        c6[(unsigned char)(c>>48)]++;
        c7[c>>56]++;

        c = cp;  ip += 8; cp = *(unsigned long long *)ip; 
        c0[(unsigned char) c     ]++;
        c1[(unsigned char)(c>>8) ]++;
        c2[(unsigned char)(c>>16)]++;
        c3[(unsigned char)(c>>24)]++;
        c4[(unsigned char)(c>>32)]++;
        c5[(unsigned char)(c>>40)]++;
        c6[(unsigned char)(c>>48)]++;
        c7[                c>>56]++;
    }
    while(ip < in+inlen) c0[*ip++]++; 
    for(i = 0; i < 256; i++) 
        bin[i] = c0[i]+c1[i]+c2[i]+c3[i]+c4[i]+c5[i]+c6[i]+c7[i];

    return bin[0];
}


int hist_4_128(uint8_t *in, size_t inlen) { 
    int i;
    unsigned bin[COUNT_SIZE]={0};
    unsigned c0[COUNT_SIZE]={0},c1[COUNT_SIZE]={0},c2[COUNT_SIZE]={0},c3[COUNT_SIZE]={0}; 
    unsigned char *ip;

    __m128i vcp = _mm_loadu_si128((__m128i*)in);
    for(ip = in; ip != in+(inlen&~(16-1)); ) {
        __m128i vc=vcp; ip += 16; vcp = _mm_loadu_si128((__m128i*)ip);
        c0[_mm_extract_epi8(vc,  0)]++;
        c1[_mm_extract_epi8(vc,  1)]++;
        c2[_mm_extract_epi8(vc,  2)]++;
        c3[_mm_extract_epi8(vc,  3)]++;
        c0[_mm_extract_epi8(vc,  4)]++;
        c1[_mm_extract_epi8(vc,  5)]++;
        c2[_mm_extract_epi8(vc,  6)]++;
        c3[_mm_extract_epi8(vc,  7)]++;
        c0[_mm_extract_epi8(vc,  8)]++;
        c1[_mm_extract_epi8(vc,  9)]++;
        c2[_mm_extract_epi8(vc, 10)]++;
        c3[_mm_extract_epi8(vc, 11)]++;
        c0[_mm_extract_epi8(vc, 12)]++;
        c1[_mm_extract_epi8(vc, 13)]++;
        c2[_mm_extract_epi8(vc, 14)]++;
        c3[_mm_extract_epi8(vc, 15)]++;
    }
    while(ip < in+inlen) 
        c0[*ip++]++; 
    for(i = 0; i < 256; i++) 
        bin[i] = c0[i]+c1[i]+c2[i]+c3[i];

    return bin[0];
}

int hist_8_128(uint8_t *in, size_t inlen) { 
    int i;
    unsigned bin[COUNT_SIZE]={0};
    unsigned c0[COUNT_SIZE]={0},c1[COUNT_SIZE]={0},c2[COUNT_SIZE]={0},c3[COUNT_SIZE]={0},c4[COUNT_SIZE]={0},c5[COUNT_SIZE]={0},c6[COUNT_SIZE]={0},c7[COUNT_SIZE]={0}; 
    unsigned char *ip;

    __m128i vcp = _mm_loadu_si128((__m128i*)in);
    for(ip = in; ip != in+(inlen&~(16-1)); ) {
        __m128i vc=vcp; ip += 16; vcp = _mm_loadu_si128((__m128i*)ip);
        c0[_mm_extract_epi8(vc,  0)]++;
        c1[_mm_extract_epi8(vc,  1)]++;
        c2[_mm_extract_epi8(vc,  2)]++;
        c3[_mm_extract_epi8(vc,  3)]++;
        c4[_mm_extract_epi8(vc,  4)]++;
        c5[_mm_extract_epi8(vc,  5)]++;
        c6[_mm_extract_epi8(vc,  6)]++;
        c7[_mm_extract_epi8(vc,  7)]++;
        c0[_mm_extract_epi8(vc,  8)]++;
        c1[_mm_extract_epi8(vc,  9)]++;
        c2[_mm_extract_epi8(vc, 10)]++;
        c3[_mm_extract_epi8(vc, 11)]++;
        c4[_mm_extract_epi8(vc, 12)]++;
        c5[_mm_extract_epi8(vc, 13)]++;
        c6[_mm_extract_epi8(vc, 14)]++;
        c7[_mm_extract_epi8(vc, 15)]++;
    }
    while(ip < in+inlen) c0[*ip++]++; 
    for(i = 0; i < 256; i++) 
        bin[i] = c0[i]+c1[i]+c2[i]+c3[i]+c4[i]+c5[i]+c6[i]+c7[i];

    return bin[0];
}


int fullSpeedBench(double proba, U32 nbBenchs, U32 algNb)
{
    size_t benchedSize = DEFAULT_BLOCKSIZE;
    void* oBuffer = malloc(benchedSize);
    char* funcName;
    int (*func)(uint8_t *src, size_t srcSize);


    BMK_genData(oBuffer, benchedSize, proba);

    switch (algNb)
        {
        case 1:
            funcName = "trivialCount";
            func = trivialCount;
            break;

        case 2:
            funcName = "count2x64";
            func = count2x64;
            break;

        case 3:
            funcName = "count_vec";
            func = count_vec;
            break;

        case 4:
            funcName = "storePort7";
            func = storePort7;
            break;

        case 5:
            funcName = "reloadPort7";
            func = storePort7;
            break;

        case 6:
            funcName = "count8reload";
            func = count8reload;
            break;

        case 7:
            funcName = "vecavx";
            func = vecavx;
            break;


        case 10:
            funcName = "hist_4_128";
            func = hist_4_128;
            break;

        case 11:
            funcName = "hist_8_128";
            func = hist_8_128;
            break;

        case 12:
            funcName = "hist_4_32";
            func = hist_4_32;
            break;

        case 13:
            funcName = "hist_4_64";
            func = hist_4_64;
            break;

#ifdef __AVX2__
        case 20:
            funcName = "port7vec";
            func = port7vec;
            break;
#endif //__AVX2__

        default:
            BMK_DISPLAY("Unknown algorithm number\n");
            exit(-1);
        }

    // Bench
    BMK_DISPLAY("\r%79s\r", "");
    {
        double bestTime = 999.;
        U32 benchNb=1;
        int errorCode = 0;
        BMK_DISPLAY("%1u-%-22.22s : \r", benchNb, funcName);
        for (benchNb=1; benchNb <= nbBenchs; benchNb++)
            {
                U32 milliTime;
                double averageTime;
                U32 loopNb=0;

                milliTime = BMK_GetMilliStart();
                while(BMK_GetMilliStart() == milliTime);
                milliTime = BMK_GetMilliStart();

                likwid_markerStartRegion(funcName);

                // fixed number of iterations (instead of fixed time in original)
                for (int i = 0; i < ITERATIONS; i++) 
                    {
                        errorCode = func(oBuffer, benchedSize);
                        if (errorCode < 0) exit(-1);
                        loopNb++;
                    }

                likwid_markerStopRegion(funcName);

                milliTime = BMK_GetMilliSpan(milliTime);
                averageTime = (double)milliTime / loopNb;
                if (averageTime < bestTime) bestTime = averageTime;
                BMK_DISPLAY("%1u-%-22.22s : %8.1f MB/s\r", benchNb+1, funcName, (double)benchedSize / bestTime / 1000.);
            }
        BMK_DISPLAY("%4d %-24.24s : %8.1f MB/s   (%i)\n", algNb, funcName, 
                (double)benchedSize / bestTime / 1000., (int)errorCode);
    }

    free(oBuffer);

    return 0;
}


int usage(char* exename)
{
    BMK_DISPLAY( "Usage :\n");
    BMK_DISPLAY( "      %s [arg] \n", exename);
    BMK_DISPLAY( "Arguments :\n");
    BMK_DISPLAY( " -b#    : select function to benchmark (default : 0 ==  all)\n");
    BMK_DISPLAY( " -H/-h  : Help (this text + advanced options)\n");
    return 0;
}

int usage_advanced(char* exename)
{
    usage(exename);
    BMK_DISPLAY( "\nAdvanced options :\n");
    BMK_DISPLAY( " -i#    : iteration loops [1-9] (default : %i)\n", NBLOOPS);
    BMK_DISPLAY( " -P#    : probability curve, in %% (default : %i%%)\n", DEFAULT_PROBA);
    return 0;
}

int badusage(char* exename)
{
    BMK_DISPLAY("Wrong parameters\n");
    usage(exename);
    return 1;
}

int main(int argc, char** argv)
{
    char* exename=argv[0];
    U32 proba = DEFAULT_PROBA;
    U32 nbLoops = NBLOOPS;
    U32 pause = 0;
    U32 algNb = 0;
    int i;
    int result;

    // Welcome message
    BMK_DISPLAY(WELCOME_MESSAGE);
    if (argc<1) return badusage(exename);

    likwid_markerInit();
    likwid_markerThreadInit();

    for(i=1; i<argc; i++)
        {
            char* argument = argv[i];

            if(!argument) continue;   // Protection if argument empty

            // Decode command (note : aggregated commands are allowed)
            if (*argument=='-')
                {
                    argument ++;
                    while (*argument!=0)
                        {

                            switch(*argument)
                                {
                                case '-':   // valid separator
                                    argument++;
                                    break;

                                    // Display help on usage
                                case 'h' :
                                case 'H': return usage_advanced(exename);

                                    // Select Algo nb
                                case 'b':
                                    argument++;
                                    algNb=0;
                                    while ((*argument >='0') && (*argument <='9')) algNb*=10, algNb += *argument++ - '0';
                                    break;

                                    // Modify Nb loops
                                case 'i':
                                    argument++;
                                    nbLoops=0;
                                    while ((*argument >='0') && (*argument <='9')) nbLoops*=10, nbLoops += *argument++ - '0';
                                    break;

                                    // Modify data probability
                                case 'P':
                                    argument++;
                                    proba=0;
                                    while ((*argument >='0') && (*argument <='9')) proba*=10, proba += *argument++ - '0';
                                    break;

                                    // Pause at the end (hidden option)
                                case 'p':
                                    pause=1;
                                    argument++;
                                    break;

                                    // Unknown command
                                default : return badusage(exename);
                                }
                        }
                    continue;
                }

        }

    if (algNb==0)
        {
            result = fullSpeedBench((double)proba / 100, nbLoops, 1);
            result = fullSpeedBench((double)proba / 100, nbLoops, 2);
            result = fullSpeedBench((double)proba / 100, nbLoops, 3);
            result = fullSpeedBench((double)proba / 100, nbLoops, 4);
            result = fullSpeedBench((double)proba / 100, nbLoops, 5);
            result = fullSpeedBench((double)proba / 100, nbLoops, 6);
#ifdef TESTING
            result = fullSpeedBench((double)proba / 100, nbLoops, 7);
#endif
            result = fullSpeedBench((double)proba / 100, nbLoops, 10);
            result = fullSpeedBench((double)proba / 100, nbLoops, 11);
            result = fullSpeedBench((double)proba / 100, nbLoops, 12);
            result = fullSpeedBench((double)proba / 100, nbLoops, 13);

#ifdef __AVX2__
            result = fullSpeedBench((double)proba / 100, nbLoops, 20);
#endif // __AVX2__
        }
    else {
        result = fullSpeedBench((double)proba / 100, nbLoops, algNb);
    }
    if (pause) { BMK_DISPLAY("press enter...\n"); getchar(); }

    likwid_markerClose();
    return result;
}

