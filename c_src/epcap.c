/* Copyright (c) 2009-2010, Michael Santos <michael.santos@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of the author nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <ei.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "epcap.h"

#define NOW_STRING(buffer) fill_buffer_with_current_time(buffer, sizeof(buffer))

int epcap_open(EPCAP_STATE *ep);
void epcap_open_live(EPCAP_STATE *ep, char *errbuf);
int epcap_init(EPCAP_STATE *ep);
void epcap_loop(EPCAP_STATE *ep);
void epcap_ctrl(const char *ctrl_evt);
void epcap_response(struct pcap_pkthdr *hdr, const u_char *pkt, unsigned int datalink);
void epcap_send_free(ei_x_buff *msg);
void epcap_watch();
void init_stats_state(EPCAP_STATE *ep);
void register_stats_signal_handler(EPCAP_STATE *ep);
void set_stats_timer(EPCAP_STATE *ep);
void log_stats(int signo);
char *fill_buffer_with_current_time(char *buffer, int buffer_size);
void block_sig_alarm();
void unblock_sig_alarm();
void usage(EPCAP_STATE *ep);

STATS_STATE ss;

    int
main(int argc, char *argv[])
{
    EPCAP_STATE *ep = NULL;
    pid_t pid = 0;
    int ch = 0;


    IS_NULL(ep = calloc(1, sizeof(EPCAP_STATE)));

    ep->snaplen = SNAPLEN;
    ep->timeout = TIMEOUT;
    ep->buffer_size = DO_NOT_SET_BUFFER_SIZE;
    ep->stats_interval_in_sec = DEFAULT_STATS_INTERVAL_IN_SEC;

    while ( (ch = getopt(argc, argv, "d:f:g:hi:MPs:t:u:vS:NIb:")) != -1) {
      switch (ch) {
            case 'd':   /* chroot directory */
                IS_NULL(ep->chroot = strdup(optarg));
                break;
            case 'f':
                IS_NULL(ep->file = strdup(optarg));
                ep->runasuser = 1;
                break;
            case 'g':
                IS_NULL(ep->group = strdup(optarg));
                break;
            case 'i':
                IS_NULL(ep->dev = strdup(optarg));
                break;
            case 'M':
                ep->rfmon = 1;
                break;
            case 'P':
                ep->promisc = 1;
                break;
            case 's':
                ep->snaplen = (size_t)atoi(optarg);
                break;
            case 't':
                ep->timeout = (u_int32_t)atoi(optarg);
                break;
            case 'u':
                IS_NULL(ep->user = strdup(optarg));
                break;
            case 'v':
                ep->verbose++;
                break;
            case 'S':
                ep->stats_interval_in_sec = atoi(optarg);
                break;
            case 'N':
                ep->no_lookupnet = 1;
                break;
            case 'I':
                ep->filter_in = 1;
                break;
            case 'b':
                ep->buffer_size = atoi(optarg);
                break;
            case 'h':
            default:
                usage(ep);
        }
    }

    argc -= optind;
    argv += optind;

    IS_NULL(ep->filt = strdup( (argc == 1) ? argv[0] : EPCAP_FILTER));

    epcap_priv_issetuid(ep);
    IS_LTZERO(epcap_open(ep));
    if (epcap_priv_drop(ep) < 0)
        exit (1);

    switch (pid = fork()) {
        case -1:
            err(EXIT_FAILURE, "fork");
        case 0:
            (void)close(fileno(stdin));
            IS_LTZERO(epcap_init(ep));
            epcap_loop(ep);
            break;
        default:
            (void)close(fileno(stdout));
            pcap_close(ep->p);
            epcap_watch();
            (void)kill(pid, SIGTERM);

            free(ep->filt);
            free(ep);
            break;
    }

    exit (0);
}


    void
epcap_watch()
{
    int fd = fileno(stdin);
    fd_set rfds;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    (void)select(fd+1, &rfds, NULL, NULL, NULL);

}


    int
epcap_open(EPCAP_STATE *ep)
{
    char errbuf[PCAP_ERRBUF_SIZE];

    if (ep->file) {
        PCAP_ERRBUF(ep->p = pcap_open_offline(ep->file, errbuf));
    } else {
        if (ep->dev == NULL)
            PCAP_ERRBUF(ep->dev = pcap_lookupdev(errbuf));

        epcap_open_live(ep, errbuf);

        /* monitor mode */
        if (pcap_can_set_rfmon(ep->p) == 1)
            (void)pcap_set_rfmon(ep->p, ep->rfmon);
    }

    if (ep->filter_in) {
      pcap_setdirection(ep->p, PCAP_D_IN);
    }

    return (0);
}

    void
epcap_open_live(EPCAP_STATE *ep, char *errbuf)
{
    PCAP_ERRBUF(ep->p = pcap_create(ep->dev, errbuf));
    IS_LTZERO(pcap_set_snaplen(ep->p, ep->snaplen));
    IS_LTZERO(pcap_set_promisc(ep->p, ep->promisc));
    IS_LTZERO(pcap_set_timeout(ep->p, ep->timeout));
    if(ep->buffer_size != DO_NOT_SET_BUFFER_SIZE) {
        IS_LTZERO(pcap_set_buffer_size(ep->p, ep->buffer_size));
        VERBOSE(2, "[%s]: pcap buffer size set to %d bytes\n\r",
                __progname, ep->buffer_size);
    }
    IS_LTZERO(pcap_activate(ep->p));
}


    int
epcap_init(EPCAP_STATE *ep)
{
    struct bpf_program fcode;
    char errbuf[PCAP_ERRBUF_SIZE];

    u_int32_t ipaddr = 0;
    u_int32_t ipmask = 0;


    if (ep->no_lookupnet == 0 &&
        pcap_lookupnet(ep->dev, &ipaddr, &ipmask, errbuf) == -1) {
        VERBOSE(1, "%s\n\r", errbuf);
        return (-1);
    }

    VERBOSE(2, "[%s] Using filter: %s\n\r", __progname, ep->filt);

    if (pcap_compile(ep->p, &fcode, ep->filt, 1 /* optimize == true */, ipmask) != 0) {
        VERBOSE(1, "[%s] Pcap_compile: %s\n\r", __progname, pcap_geterr(ep->p));
        return (-1);
    }

    if (pcap_setfilter(ep->p, &fcode) != 0) {
        VERBOSE(1, "[%s] Pcap_setfilter: %s\n\r", __progname, pcap_geterr(ep->p));
        return (-1);
    }

    return (0);
}


    void
epcap_loop(EPCAP_STATE *ep)
{
    pcap_t *p = ep->p;
    struct pcap_pkthdr *hdr = NULL;
    const u_char *pkt = NULL;

    int read_packet = 1;
    int datalink = pcap_datalink(p);
    char time_buffer[TIME_BUFFER_SIZE];

    if(ep->verbose == 2) {
      init_stats_state(ep);
      register_stats_signal_handler(ep);
      set_stats_timer(ep);
    }

    while (read_packet) {
        switch (pcap_next_ex(p, &hdr, &pkt)) {
            case 0:     /* timeout */
                VERBOSE(1, "[%s][%s][%s]: Timeout reading packet\n\r",
                        __progname, NOW_STRING(time_buffer), ep->dev);
                break;
            case 1:     /* got packet */
                VERBOSE(2, "[%s][%s][%s]: Got packet successfully\n\r",
                        __progname, NOW_STRING(time_buffer), ep->dev);
                block_sig_alarm();
                epcap_response(hdr, pkt, datalink);
                unblock_sig_alarm();
                break;
            case -2:    /* eof */
                VERBOSE(1, "[%s][%s][%s]: End of file\n\r",
                        __progname, NOW_STRING(time_buffer), ep->dev);
                block_sig_alarm();
                epcap_ctrl("eof");
                unblock_sig_alarm();
                read_packet = 0;
                break;
            case -1:    /* error reading packet */
                VERBOSE(1, "[%s][%s][%s]: Error reading packet\n\r",
                        __progname, NOW_STRING(time_buffer), ep->dev);
                /* fall through */
            default:
                read_packet = 0;
        }
    }
}

void epcap_ctrl(const char *ctrl_evt)
{
    ei_x_buff msg;

    IS_FALSE(ei_x_new_with_version(&msg));
    IS_FALSE(ei_x_encode_tuple_header(&msg, 2));
    IS_FALSE(ei_x_encode_atom(&msg, "epcap"));
    IS_FALSE(ei_x_encode_atom(&msg, ctrl_evt));

    epcap_send_free(&msg);
}

    void
epcap_response(struct pcap_pkthdr *hdr, const u_char *pkt, unsigned int datalink)
{
    ei_x_buff msg;


    IS_FALSE(ei_x_new_with_version(&msg));

    /* {packet, DatalinkType, Time, ActualLength, Packet} */
    IS_FALSE(ei_x_encode_tuple_header(&msg, 5));
    IS_FALSE(ei_x_encode_atom(&msg, "packet"));

    /* DataLinkType */
    IS_FALSE(ei_x_encode_long(&msg, datalink));

    /* {MegaSec, Sec, MicroSec} */
    IS_FALSE(ei_x_encode_tuple_header(&msg, 3));
    IS_FALSE(ei_x_encode_long(&msg, abs(hdr->ts.tv_sec / 1000000)));
    IS_FALSE(ei_x_encode_long(&msg, hdr->ts.tv_sec % 1000000));
    IS_FALSE(ei_x_encode_long(&msg, hdr->ts.tv_usec));

    /* ActualLength} */
    IS_FALSE(ei_x_encode_long(&msg, hdr->len));

    /* Packet */
    IS_FALSE(ei_x_encode_binary(&msg, pkt, hdr->caplen));

    /* } */

    epcap_send_free(&msg);
}

void epcap_send_free(ei_x_buff *msg)
{
    u_int16_t len = 0;

    len = htons(msg->index);
    if (write(fileno(stdout), &len, sizeof(len)) != sizeof(len))
        errx(EXIT_FAILURE, "write header failed");

    if (write(fileno(stdout), msg->buff, msg->index) != msg->index)
        errx(EXIT_FAILURE, "write packet failed: %d", msg->index);

    ei_x_free(msg);
}

void init_stats_state(EPCAP_STATE *ep)
{
  ss.p = ep->p;
  ss.dev = ep->dev;
}

void register_stats_signal_handler(EPCAP_STATE *ep)
{
  struct sigaction action;

  action.sa_handler = log_stats;
  sigfillset(&action.sa_mask);
  action.sa_flags = 0;
  sigaction(SIGALRM, &action, 0);
}

void set_stats_timer(EPCAP_STATE *ep)
{
  struct itimerval timer_settings;

  timer_settings.it_interval.tv_sec = ep->stats_interval_in_sec;
  timer_settings.it_interval.tv_usec = 0;
  timer_settings.it_value.tv_sec = ep->stats_interval_in_sec;
  timer_settings.it_value.tv_usec = 0;
  IS_LTZERO(setitimer(ITIMER_REAL, &timer_settings, NULL));
}

void log_stats(int signo)
{
  struct pcap_stat stats;
  char time_buffer[TIME_BUFFER_SIZE];

  if(pcap_stats(ss.p, &stats) != 0) {
      fprintf(stderr, "[%s][%s][%s]: Error reading statistics\n\r", __progname,
              NOW_STRING(time_buffer), ss.dev);
  } else {
      fprintf(stderr, "[%s][%s][%s]: Capture statistics: Received: %u, Dropped:%u\n\r",
              __progname, NOW_STRING(time_buffer), ss.dev,
              stats.ps_recv, stats.ps_drop);
  }
}

char *fill_buffer_with_current_time(char *buffer, int buffer_size)
{
  time_t timer = time(NULL);

  strftime(buffer, buffer_size, "%c", localtime(&timer));
  return buffer;
}

void block_sig_alarm()
{
  sigset_t sigset;

  sigemptyset(&sigset);
  sigaddset(&sigset, SIGALRM);
  sigprocmask(SIG_BLOCK, &sigset, NULL);
}

void unblock_sig_alarm()
{
  sigset_t sigset;

  sigemptyset(&sigset);
  sigaddset(&sigset, SIGALRM);
  sigprocmask(SIG_UNBLOCK, &sigset, NULL);
}

    void
usage(EPCAP_STATE *ep)
{
    (void)fprintf(stderr, "%s, %s\n", __progname, EPCAP_VERSION);
    (void)fprintf(stderr,
            "usage: %s <options>\n"
            "              -d <directory>   chroot directory\n"
            "              -i <interface>   interface to snoop\n"
            "              -f <filename>    read from file instead of live capture\n"
            "              -M               wireless monitor (rfmon) mode\n"
            "              -P               promiscuous mode\n"
            "              -g <group>       unprivileged group\n"
            "              -u <user>        unprivileged user\n"
            "              -s <length>      packet capture length\n"
            "              -t <millisecond> capture timeout\n"
            "              -v               verbose mode\n"
            "              -N               no lookupnet (allow to run on ipv4-less interface)\n"
            "              -I               filter only incoming packets\n",
            __progname
            );

    exit (EXIT_FAILURE);
}
