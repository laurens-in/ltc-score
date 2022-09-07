//  gcc -o jltcdump-simple jltcdump-simple.c `pkg-config --cflags --libs jack ltc`/

/* jack linear time code decoder
 * Copyright (C) 2006, 2012, 2013 Robin Gareus
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
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

#define LTC_QUEUE_LEN (42) // should be >> ( max(jack period size) * max-speedup / (duration of LTC-frame) )

#define _GNU_SOURCE

#ifndef VERSION
#define VERSION "1"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <getopt.h>

#include <jack/jack.h>
#include <ltc.h>
#include <ncurses.h>

#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>

#define NR_OF_PARTS 12
#define NR_OF_MINUTES 110

static int keep_running = 1;

static jack_port_t *input_port = NULL;
static jack_client_t *j_client = NULL;
static uint32_t j_samplerate = 44100;

static LTCDecoder *decoder = NULL;

static pthread_mutex_t ltc_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t data_ready = PTHREAD_COND_INITIALIZER;

static int fps_num = 25;
static int fps_den = 1;

// represent event in score
struct event
{
  int id;
  int start;
  char *sectionName;
  char *notes;
  char *endsWith;
};

struct event *partmap[NR_OF_MINUTES];

static struct event parts[NR_OF_PARTS] = {
    {0, 0, "permutation with overlap", "karplus direct to fft",
     "cont."},
    {1, 8, "lars + candid + olive", "silence", "cont."},
    {2, 11, "around one pitch", "silence", "cont."},
    {3, 15, "michel + candid + drums", "silence", "cont."},
    {4, 23, "lisa + michel", "maybe some texture", "cont."},
    {5, 31, "benoit + morgan", "silence", "BREAK"},
    {6, 36, "benoit + morgan again", "silence", "cont."},
    {7, 40, "free at last", "guitar -> amp -> fft", "cont."},
    {8, 50, "textural midi piano", "come in with texture in the end", "METAL"},
    {9, 58, "ambivalent metal", "chug chug", "cont."},
    {10, 64, "soft intense, michel + morgan", "noise", "cont."},
    {11, 70, "end", "end", "end"}};

/**
 * jack audio process callback
 */
int process(jack_nframes_t nframes, void *arg)
{
  jack_default_audio_sample_t *in;

  in = jack_port_get_buffer(input_port, nframes);

  ltc_decoder_write_float(decoder, in, nframes, 0);

  /* notify reader thread */
  if (pthread_mutex_trylock(&ltc_thread_lock) == 0)
  {
    pthread_cond_signal(&data_ready);
    pthread_mutex_unlock(&ltc_thread_lock);
  }
  return 0;
}

void jack_shutdown(void *arg)
{
  fprintf(stderr, "recv. shutdown request from jackd.\n");
  keep_running = 0;
  pthread_cond_signal(&data_ready);
}

/**
 * open a client connection to the JACK server
 */
static int init_jack(const char *client_name)
{
  jack_status_t status;
  j_client = jack_client_open(client_name, JackNullOption, &status);
  if (j_client == NULL)
  {
    fprintf(stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
    if (status & JackServerFailed)
    {
      fprintf(stderr, "Unable to connect to JACK server\n");
    }
    return (-1);
  }
  if (status & JackServerStarted)
  {
    fprintf(stderr, "JACK server started\n");
  }
  if (status & JackNameNotUnique)
  {
    client_name = jack_get_client_name(j_client);
    fprintf(stderr, "jack-client name: `%s'\n", client_name);
  }

  jack_set_process_callback(j_client, process, 0);

  jack_on_shutdown(j_client, jack_shutdown, NULL);

  j_samplerate = jack_get_sample_rate(j_client);
  return (0);
}

static int jack_portsetup(void)
{
  /* Allocate data structures that depend on the number of ports. */
  decoder = ltc_decoder_create(j_samplerate * fps_den / fps_num, LTC_QUEUE_LEN);
  if (!decoder)
  {
    fprintf(stderr, "cannot create LTC decoder (out of memory).\n");
    return (-1);
  }
  /* create jack port */
  if ((input_port = jack_port_register(j_client, "input_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0)
  {
    fprintf(stderr, "cannot register input port \"input_1\"!\n");
    return (-1);
  }
  return (0);
}

static void jack_port_connect(char **jack_port, int argc)
{
  int i;
  for (i = 0; i < argc; i++)
  {
    if (!jack_port[i])
      continue;
    if (jack_connect(j_client, jack_port[i], jack_port_name(input_port)))
    {
      fprintf(stderr, "cannot connect port %s to %s\n", jack_port[i], jack_port_name(input_port));
    }
  }
}

/**
 *
 */
static void my_decoder_read(LTCDecoder *d)
{
  LTCFrameExt frame;

  while (ltc_decoder_read(d, &frame))
  {

    SMPTETimecode stime;
    ltc_frame_to_time(&stime, &frame.ltc, /* use_date? LTC_USE_DATE : */ 0);

    erase();
    attron(COLOR_PAIR(3));
    mvprintw(0, 0, "%s", "RT60 - TIMECODE");
    attroff(COLOR_PAIR(3));

    mvprintw(1, 0, "%02d:%02d:%02d%c%02d | %.1fdB\n",
             stime.hours,
             stime.mins,
             stime.secs,
             (frame.ltc.dfbit) ? '.' : ':',
             stime.frame,
             frame.volume);
    int currentMin = stime.mins + (stime.hours * 60);
    if (currentMin <= 120 && currentMin >= 0)
    {
      if (partmap[currentMin] != partmap[currentMin + 1])
      {

        if (stime.secs % 2 == 0)
        {
          attron(COLOR_PAIR(1));
        }
        mvprintw(2, 0, "time remaining: %02d ", 60 - stime.secs);
        attroff(COLOR_PAIR(1));
      }
      mvprintw(3, 0, "section: %s", partmap[currentMin]->sectionName);

      mvprintw(4, 0, "notes: %s", partmap[currentMin]->notes);
      attron(COLOR_PAIR(2));
      mvprintw(5, 0, "ends with: %s", partmap[currentMin]->endsWith);
      attroff(COLOR_PAIR(2));
      // if (partmap[currentMin]->id < 11)
      //   mvprintw(6, 0, "upcoming: %s", parts[partmap[currentMin + 1]->id].sectionName);
    }
  }
  refresh();
  fflush(stdout);
}

/**
 *
 */
static void main_loop(void)
{
  // if (pthread_mutex_trylock(&ltc_thread_lock) == 0)
  // {
  //   keep_running = 0;
  // };
  pthread_mutex_lock(&ltc_thread_lock);

  while (keep_running)
  {
    my_decoder_read(decoder);
    pthread_cond_wait(&data_ready, &ltc_thread_lock);
  }

  pthread_mutex_unlock(&ltc_thread_lock);
}

void catchsig(int sig)
{

  signal(SIGHUP, catchsig);

  fprintf(stderr, "caught signal - shutting down.\n");
  keep_running = 0;
  pthread_cond_signal(&data_ready);
}

/**************************
 * main application code
 */

static struct option const long_options[] =
    {
        {"help", no_argument, 0, 'h'},
        {"fps", required_argument, 0, 'f'},
        {"version", no_argument, 0, 'V'},
        {NULL, 0, NULL, 0}};

static void usage(int status)
{
  printf("jltcdump - very simple JACK client to parse linear time code.\n\n");
  printf("Usage: jltcdump [ OPTIONS ] [ JACK-PORTS ]\n\n");
  printf("Options:\n\
  -f, --fps  <num>[/den]     set expected framerate (default 25/1)\n\
  -h, --help                 display this help and exit\n\
  -V, --version              print version information and exit\n\
\n\n");
  printf("Report bugs to Robin Gareus <robin@gareus.org>\n"
         "Website and manual: <https://github.com/x42/ltc-tools>\n");
  exit(status);
}

static int decode_switches(int argc, char **argv)
{
  int c;

  while ((c = getopt_long(argc, argv,
                          "h"  /* help */
                          "f:" /* fps */
                          "V", /* version */
                          long_options, (int *)0)) != EOF)
  {
    switch (c)
    {
    case 'f':
    {
      fps_num = atoi(optarg);
      char *tmp = strchr(optarg, '/');
      if (tmp)
        fps_den = atoi(++tmp);
    }
    break;

    case 'V':
      printf("jltcdump-simple version %s\n\n", VERSION);
      printf("Copyright (C) GPL 2006,2012,2013 Robin Gareus <robin@gareus.org>\n");
      exit(0);

    case 'h':
      usage(0);

    default:
      usage(EXIT_FAILURE);
    }
  }

  return optind;
}

int main(int argc, char **argv)
{
  int i;

  i = decode_switches(argc, argv);

  // -=-=-= INITIALIZE =-=-=-

  initscr();
  curs_set(0);
  start_color();
  use_default_colors();
  init_pair(1, COLOR_WHITE, COLOR_RED);
  init_pair(2, COLOR_YELLOW, -1);
  init_pair(3, COLOR_MAGENTA, -1);

  // -=-=-= INITIALIZE =-=-=-

  for (int i = 0; i < NR_OF_PARTS; i++)
  {
    int ends;
    if (i + 1 == NR_OF_PARTS)
    {
      ends = 120;
    }
    else
    {
      ends = parts[i + 1].start;
    }
    int starts = parts[i].start;
    for (int j = starts; j < ends; j++)
    {
      partmap[j] = &parts[i];
    }
  }

  if (init_jack("timecodeRT60"))
    goto out;

  if (jack_portsetup())
    goto out;

  if (mlockall(MCL_CURRENT | MCL_FUTURE))
  {
    fprintf(stderr, "Warning: Can not lock memory.\n");
  }

  if (jack_activate(j_client))
  {
    fprintf(stderr, "cannot activate client.\n");
    goto out;
  }

  jack_port_connect(&(argv[i]), argc - i);

  signal(SIGHUP, catchsig);
  signal(SIGINT, catchsig);

  printw("ready...\n");

  main_loop();

out:
  if (j_client)
  {
    jack_client_close(j_client);
    j_client = NULL;
  }
  ltc_decoder_free(decoder);
  fprintf(stderr, "bye.\n");

  return (0);
}
/* vi:set ts=8 sts=2 sw=2: */
