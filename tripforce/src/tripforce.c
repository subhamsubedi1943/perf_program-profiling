#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <signal.h>
#include <openssl/des.h> /* Requires OpenSSL via libssl or libcrypto */

#ifdef _OPENMP /* If OpenMP is not supported, multithreading is disabled. */
#ifdef __APPLE
	#include <libiomp/omp.h>
#else
	#include <omp.h>
#endif
#else
	/* Single-threaded mode: OMP calls replaced with dummy functions */
	#define omp_init_lock(mutex) ;
	#define omp_destroy_lock(mutex) ;
	#define omp_set_lock(mutex) ;
	#define omp_unset_lock(mutex) ;
	typedef unsigned char omp_lock_t; /* dummy mutex type */
#endif

/*
 * tripforce - tripcode bruteforcer for Futaba-style imageboards
 * Copyright (C) 2016 microsounds <https://github.com/microsounds>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * usage:
 * $ tripforce [OPTION] "SEARCHSTR"
 *
 * building:
 * gcc -O3 -ansi -march=native -o tripforce tripforce.c -fopenmp -lcrypto
 *
 * If your platform doesn't support OpenMP, tripforce defaults to
 * single-threaded mode for backwards compatiblity.
 *
 * note:
 * Tripcodes can only be 10 characters long.
 * Tripcodes can only contain the characters from the range './0-9A-Za-z'.
 * The 10th character of a tripcode can only be from the range '.26AEIMQUYcgkosw'.
 * See README.md for more information.
 */

/*
 * Generating Futaba-style tripcodes:
 *   1. Convert password to Shift-JIS
 *   2. Only first 8 characters of password are significant
 * Generating salt:
 *   3a. Take 2nd and 3rd characters and then append "H." to the end
 * Clamp salt string to DES character range './0-9A-Za-z'
 * 	 3b. Replace any characters outside of range '.' to 'z' with '.'
 * 	 3c. Replace any characters in ":;<=>?@[\]^_`" with "ABCDEFGabcdef"
 * Generating tripcode:
 *   4. Call DES_fcrypt() (fast traditional DES) with password and generated salt
 * 	 5. Return the last 10 characters
 */


/* GLOBAL FUNCTIONALITY */
#define APPLICATION_NAME    "tripforce"
#define APPLICATION_DESC    "tripcode bruteforcer for Futaba-style imageboards"
#define APPLICATION_VER     "0.4.0"
#define APPLICATION_AUTHOR  "Copyright (C) 2016-2022 microsounds <https://github.com/microsounds>"
#define APPLICATION_LICENSE "GNU General Public License v3"

/* enum value is tied to argc index by design */
enum _program_mode {
	HELP_MSG = -1,
	NO_QUERY_MODE = 0, /* no search function */
	CASE_SENSITIVE = 1, /* argv[1] */
	CASE_AGNOSTIC = 2 /* argv[2] */
};
typedef enum _program_mode pmode_t;

/* operating mode for the averaging counter */
enum _avg_stats {
	COUNT_ONLY,
	FETCH_DATA
};

/* ERROR HANDLING */
#define ERROR_NO_QUERY         "You didn't provide a query string.\n"
#define ERROR_QUERY_LENGTH     "Tripcodes cannot be longer than 10 characters.\n"
#define ERROR_QUERY_INVALID    "Tripcodes can only contain the characters ./0-9A-Za-z\n"
#define ERROR_QUERY_TENTH_CHAR "10th character can only be one of these characters: '.26AEIMQUYcgkosw'\n"

/* FUNCTION PROTOTYPES */
void cli_splash(const unsigned int num_cores, pmode_t mode);
void cli_help_msg(void);
int validate_query(const char *query);
void seed_qrand(unsigned seed);
int qrand(void);
unsigned int trip_frequency(enum _avg_stats mode);
void seed_qrand_r(unsigned *seeds, unsigned num);
float trip_rate_condense(const unsigned rate, char *prefix);
void generate_password(char *password, unsigned *seed);
void generate_salt(const char *password, char *salt);
int qrand_r(unsigned *seed);
void strip_outliers(char *salt);
void replace_punctuation(char *salt);
void truncate_tripcode(char *hash);
char *strcasestr(const char *haystack, const char *needle);
void determine_match(pmode_t mode, char *query, char *trip, char *password, omp_lock_t *io_lock);
void sigint_stop(int signo);

/* GLOBALS */
static volatile int run_state;

/* INTERFACE */
void cli_splash(const unsigned int num_cores, pmode_t mode)
{
	unsigned int i;
	fprintf(stdout, "%s %s\n", APPLICATION_NAME, APPLICATION_VER);
	fprintf(stdout, "%s\nReleased under the %s.\n", APPLICATION_AUTHOR, APPLICATION_LICENSE);
	fprintf(stdout, "Utilizing %u thread", num_cores);
	if (num_cores > 1)
		fputc('s', stdout);
	fprintf(stdout, ".%c", '\n');
  if (mode == NO_QUERY_MODE) {
    fprintf(stdout, "Running in benchmark mode, send break to stop.\n");
  } else {
	  for (i = 0; i < 64; i++)
	  	fprintf(stdout, "%c", '-');
	  fprintf(stdout, "%c", '\n');
  }
	fflush(stdout);
}

void cli_help_msg(void)
{
	fprintf(stdout, "usage:\n\t%s [OPTION] \"SEARCHSTR\"\n", APPLICATION_NAME);
	fprintf(stdout, "help:\n");
	fprintf(stdout, "\t(None)\t No query. Program will print random tripcodes to stdout.\n");
	fprintf(stdout, "\t-i\t Case agnostic search.\n");
	fprintf(stdout, "\t-h\t Display this help screen.\n");
}

int validate_query(const char *query)
{
	unsigned int i, len;
	static const unsigned QUERY_MAX_LENGTH = 10;
	static const unsigned TENTH_CHAR_CANDIDATES = 16;
	static const char *tenth_char = ".26AEIMQUYcgkosw";
	if (!query) /* string entered? */
	{
		fprintf(stderr, ERROR_NO_QUERY);
		return 0;
	}
	len = strlen(query);
	if (len > QUERY_MAX_LENGTH) /* valid length? */
	{
		fprintf(stderr, ERROR_QUERY_LENGTH);
		return 0;
	}
	for (i = 0; i < len; i++) /* valid character range? */
	{
		if ( !( (query[i] >= '.' && query[i] <= '9') ||
		        (query[i] >= 'A' && query[i] <= 'Z') ||
		        (query[i] >= 'a' && query[i] <= 'z') ) )
		{
			fprintf(stderr, ERROR_QUERY_INVALID);
			return 0;
		}
	}
	if (len == QUERY_MAX_LENGTH) /* 10th character valid? */
	{
		unsigned match_found = 0;
		for (i = 0; i < TENTH_CHAR_CANDIDATES; i++)
		{
			if (query[len - 1] == tenth_char[i])
				match_found = 1;
		}
		if (!match_found)
		{
			fprintf(stderr, ERROR_QUERY_TENTH_CHAR);
			return 0;
		}
	}
	return 1;
}

/* PRNG */

/* non-reentrant PRNG seed */
static unsigned QRAND_SEED;

void seed_qrand(unsigned seed)
{
	QRAND_SEED = seed;
}

int qrand(void)
{
	/* faster than rand() */
	QRAND_SEED = (214013 * QRAND_SEED + 2531011);
	return (QRAND_SEED >> 16) & 0x7FFF;
}

void seed_qrand_r(unsigned *seeds, unsigned num)
{
	/* populate array of reentrant qrand seeds */
	unsigned int i;
	for (i = 0; i < num; i++)
	{
		int j, random_value;
		random_value = 0;
		j = 0;
		while (!random_value) /* make sure it actually attempts to randomize */
			random_value = qrand();
		while (j++ != random_value)
			qrand(); /* skip qrand() forward by a random amount */
		seeds[i] = qrand();
	}
}

int qrand_r(unsigned *seed)
{
	/* reentrant qrand */
	*seed = (214013 * *seed + 2531011);
	return (*seed >> 16) & 0x7FFF;
}

/* UTILITIES */

unsigned int trip_frequency(enum _avg_stats mode)
{
	/* returns average trip hashing rate in trips/sec */
	static unsigned current_tally = 0;
	static unsigned average = 0;
	static time_t time_at_last_call = 0;
	time_t current_time = time(NULL);

	if (mode == FETCH_DATA)
		return (average) ? average : current_tally;
	else /* COUNT_ONLY */
	{
		if (current_time != time_at_last_call)
		{
			/* floating point divsion runs marginally faster */
			average = (average / 2.0) + (current_tally / 2.0);
			current_tally = 1;
		}
		else
			current_tally++;
		time_at_last_call = current_time;
	}
	return 0;
}

float trip_rate_condense(const unsigned rate, char *prefix)
{
	/* 32-bit unsigned int counter overflows at 4.29 gTrip/s
	   Future versions can take advantage of 64-bit unsigned long long int
	   which goes up to 18.44 eTrips/s at a performance hit of ~2% */
	#define K_TRIP 1000.0f
	#define MAGS 5
	unsigned i;
	static const char trip_prefix[MAGS] = {'\0', 'k', 'm', 'g', 't' };
	static const float trip_magnitude[MAGS] = {
		0.0, /* trip */
		K_TRIP, /* kilotrip */
		K_TRIP * K_TRIP, /* megatrip */
		K_TRIP * K_TRIP * K_TRIP, /* gigatrip */
		K_TRIP * K_TRIP * K_TRIP * K_TRIP, /* teratrip */
	};
	for (i = 0; i < MAGS; i++)
	{
		if (rate < trip_magnitude[i] && i != 0)
		{
			*prefix = trip_prefix[i - 1];
			return (float) rate / trip_magnitude[i - 1];
		}
		else
			continue;
	}
	return (float) rate;
}


/* TRIPCODE ROUTINES */

static const unsigned char PASSWORD_LENGTH = 8;
static const unsigned char SALT_LENGTH = 4;
static const unsigned char DES_FCRYPT_LENGTH = 14;
static const unsigned char TRIPCODE_LENGTH = 10;

void generate_password(char *password, unsigned *seed)
{
	/* Shift-JIS is a legacy 2-byte encoding, and many *chans tend to strip or
	   convert the more exotic characters to UTF-8, leading to unpredictable tripcodes */
	/* Creates a Shift-JIS compatible string from the 1-byte SJIS codepage only */
	static const unsigned char TABLE_SIZE = 92;
	static const char *lookup = " !\"$%&\'()*+,-./0123456789:;<=>?"
	"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_`abcdefghijklmnopqrstuvwxyz{|}";
	/* '#' triggers secure tripcodes on 4chan.org */
	/* '~' and '\' don't have 1-byte Shift-JIS equivalents */
	unsigned char i;
	for (i = 0; i < PASSWORD_LENGTH; i++)
	{
		password[i] = lookup[qrand_r(seed) % TABLE_SIZE];
	}
	password[PASSWORD_LENGTH] = '\0'; /* null terminate */
}


void generate_salt(const char *password, char *salt)
{
	/* this is probably faster than sprintf() */
	salt[0] = password[1];
	salt[1] = password[2];
	salt[2] = 'H';
	salt[3] = '.';
	salt[4] = '\0'; /* null terminate */
}

void strip_outliers(char *salt)
{
	/* clamp character range */
	unsigned char i;
	for (i = 0; i < SALT_LENGTH; i++)
	{
		if (salt[i] < '.' || salt[i] > 'z')
			salt[i] = '.';
	}
}

void replace_punctuation(char *salt)
{
	/* clamp punctuation */
	unsigned char i;
	for (i = 0; i < SALT_LENGTH; i++)
	{
		if (salt[i] >= ':' && salt[i] <= '@') /* if ':;<=>?@' */
			salt[i] += 0x06; /* shift to 'ABCDEFG' */
		else if (salt[i] >= '[' && salt[i] <= '`') /* if '[\]^_`' */
			salt[i] += 0x06; /* shift to 'abcdef' */
	}
}

void truncate_tripcode(char *hash)
{
	/* truncate 14 byte output to 10 bytes by overlap */
	static const unsigned char HASH_OFFSET = 3;
	memmove(hash, hash+HASH_OFFSET, TRIPCODE_LENGTH);
	hash[TRIPCODE_LENGTH] = '\0'; /* null terminate */
}

char *strcasestr(const char *haystack, const char *needle)
{
	/* CASE_AGNOSTIC substring search */
	/*
	 * strcasestr is ~2.1% slower than glibc's strstr
	 * some attempts were made to improve performance
	 * eg. lowercasing the strings in advance to prevent reundant tolower() calls
	 * but this only resulted in slower performance
	 */
	unsigned int len_h, len_n, i, j;
	len_h = strlen(haystack);
	len_n = strlen(needle);
	for (i = 0; i < len_h; i++)
	{
		unsigned matches = 0;
		for (j = 0; j < len_n; j++)
		{
			if (i + len_n <= len_h) /* bounds checking */
			{
        char h, n;
				h = haystack[i + j];
				n = needle[j];
				h += (h >= 'A' && h <= 'Z') ? 0x20 : 0x00;
				n += (h >= 'A' && h <= 'Z') ? 0x20 : 0x00;
				if (h == n)
					matches++;
			}
			else
				break;
		}
		if (matches == len_n)
			return (char *) haystack + i; /* return match location */
	}
	return NULL;
}

void determine_match(pmode_t mode, char *query, char *trip, char *password, omp_lock_t *io_lock)
{
	switch (mode)
	{
		case CASE_AGNOSTIC: if (strcasestr(trip, query)) goto print; break;
		case CASE_SENSITIVE: if (strstr(trip, query)) goto print; break;
		case NO_QUERY_MODE: return; break; /* VERY SLOW (not anymore) */
		default: break;
	}
	return;

	print:
	{
		char prefix; /* get average speed and condense it */
		unsigned int avg_rate;
		float avg_float;

		prefix = '\0'; 
		avg_rate = trip_frequency(FETCH_DATA);
		avg_float = trip_rate_condense(avg_rate, &prefix);

		omp_set_lock(io_lock);
	  	fprintf(stdout, "TRIP: '!%s' -> PASS: '%.8s' ", trip, password);
	  	if (prefix)
  			fprintf(stdout, "@ %.2f %cTrip/s\n", avg_float, prefix);
  		else
   			fprintf(stdout, "@ %u Trip/s\n", avg_rate);
		omp_unset_lock(io_lock);
  }
}

void sigint_stop(int signo) {
  fprintf(stdout, "***Received SIGINT***\n");
  run_state = 0;
}

int main(int argc, char **argv)
{
	omp_lock_t io_lock;
	pmode_t mode;
	unsigned int NUM_CORES, *qrand_seeds;

  #ifdef _OPENMP
  	NUM_CORES = omp_get_num_procs();
  #else
    NUM_CORES = 1;
  #endif

	if (argc == 1)
		mode = NO_QUERY_MODE;
	else if (!strcmp(argv[1], "-h")) /* help screen */
	{
		cli_help_msg();
		return 1;
	}
	else /* determine query mode */
	{
		mode = (!strcmp(argv[1], "-i")) ? CASE_AGNOSTIC : CASE_SENSITIVE;
		if (!validate_query(argv[mode]))
			return 1;
	}

  run_state = 1;
  qrand_seeds = (unsigned int *)malloc(sizeof(unsigned int)*NUM_CORES);
	cli_splash(NUM_CORES, mode);
	omp_init_lock(&io_lock); /* forced blocking I/O */
	seed_qrand(time(NULL)); /* per-thread reentrant PRNG seeds */
	seed_qrand_r(qrand_seeds, NUM_CORES);

  signal(SIGINT, sigint_stop);

#ifdef _OPENMP
	#pragma omp parallel num_threads(NUM_CORES)
#endif
	{
    unsigned int THREAD_ID;
    #ifdef _OPENMP
		  THREAD_ID = omp_get_thread_num();
    #else
		  THREAD_ID = 0;
    #endif
		while (run_state)
		{
			/* Intel Core2 Duo P8600 @ 2.401GHz w/ 2 threads
			   CASE_SENSITIVE: 353.1 kTrips/s
			   CASE_AGNOSTIC:  347.3 kTrips/s */
			char *password;
			char *salt;
			char *trip; /* DES_fcrypt() asks for 14 bytes */
      password = (char *)malloc(sizeof(char)*(PASSWORD_LENGTH+1));
      salt = (char *)malloc(sizeof(char)*(SALT_LENGTH+1));
			trip = (char *)malloc(sizeof(char)*DES_FCRYPT_LENGTH); /* DES_fcrypt() asks for 14 bytes */
			generate_password(password, &qrand_seeds[THREAD_ID]);
			generate_salt(password, salt);
			strip_outliers(salt);
			replace_punctuation(salt);
			DES_fcrypt(password, salt, trip);
			truncate_tripcode(trip);
			determine_match(mode, argv[mode], trip, password, &io_lock);
			trip_frequency(COUNT_ONLY);
      free(password);
      free(salt);
      free(trip);
		}
	}
	{
		char prefix; /* get average speed and condense it */
		unsigned int avg_rate;
		float avg_float;

		prefix = '\0'; 
		avg_rate = trip_frequency(FETCH_DATA);
		avg_float = trip_rate_condense(avg_rate, &prefix);

	  if (prefix)
  		fprintf(stdout, "Final average rate: %.2f %cTrip/s\n", avg_float, prefix);
  	else
   		fprintf(stdout, "Final average rate: %u Trip/s\n", avg_rate);
	}
	omp_destroy_lock(&io_lock);
  free(qrand_seeds);
	return 0;
}
