/*
 * Shairport, an Apple Airplay receiver
 * Copyright (c) James Laird 2013
 * All rights reserved.
 * Modifications and additions (c) Mike Brady 2014--2026
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libconfig.h>
#include <libgen.h>
#include <memory.h>
#include <net/if.h>
#include <popt.h>
#include "aircrab_bridge.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


#ifdef CONFIG_FFMPEG
#include <libavutil/log.h>
#endif

#ifdef CONFIG_AIRPLAY_2
#include "ptp-utilities.h"
#include "utilities/generate_device_uuid.h"
#include "utilities/generate_random_uuid.h"
#include <gcrypt.h>
#include <libavcodec/avcodec.h>
#include <sodium.h>
#include <uuid/uuid.h>

#endif

#ifdef CONFIG_MBEDTLS
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR >= 4
#include <psa/crypto.h>
#else
#include <mbedtls/md5.h>
#endif
#endif

#ifdef CONFIG_POLARSSL
#include <polarssl/md5.h>
#endif

#ifdef CONFIG_OPENSSL
#include <openssl/evp.h>
#include <openssl/md5.h>
#endif

#if defined(CONFIG_DBUS_INTERFACE)
#include <glib.h>
#endif

#include "activity_monitor.h"
#include "audio.h"
#include "common.h"
#include "rtp.h"
#include "rtsp.h"
#include "utilities/exit.h"
#include "utilities/string_utilities.h"

#if defined(CONFIG_DACP_CLIENT)
#include "dacp.h"
#endif

#if defined(CONFIG_METADATA)
#include "metadata/core.h"
#endif

#if defined(CONFIG_METADATA_HUB)
#include "metadata/hub.h"
#endif

#ifdef CONFIG_DBUS_INTERFACE
#include "dbus/dbus-service.h"
#endif

#ifdef CONFIG_MQTT
#include "mqtt.h"
#endif

#ifdef CONFIG_MPRIS_INTERFACE
#include "dbus/mpris-service.h"
#endif

#ifdef CONFIG_LIBDAEMON
#include <libdaemon/dexec.h>
#include <libdaemon/dfork.h>
#include <libdaemon/dlog.h>
#include <libdaemon/dpid.h>
#include <libdaemon/dsignal.h>
#else
#include <syslog.h>
#endif

#ifdef CONFIG_SOXR
#include <math.h>
#include <soxr.h>
#endif

#ifdef CONFIG_CONVOLUTION
#include "FFTConvolver/convolver.h"
#include <sndfile.h>
#endif

pid_t pid;
#ifdef CONFIG_LIBDAEMON
int this_is_the_daemon_process = 0;
#endif

#ifndef UUID_STR_LEN
#define UUID_STR_LEN 36
#endif

#define strnull(s) ((s) ? (s) : "(null)")

pthread_t rtsp_listener_thread;

#if defined(CONFIG_DBUS_INTERFACE) || defined(CONFIG_MPRIS_INTERFACE)
GMainLoop *glib_worker_loop = NULL;
#endif

int killOption = 0;
int daemonisewith = 0;
int daemonisewithout = 0;
int log_to_syslog_selected = 0;
#ifdef CONFIG_LIBDAEMON
int log_to_default = 1; // needed if libdaemon used
#endif
int display_config_selected = 0;
int log_to_syslog_select_is_first_command_line_argument = 0;

char configuration_file_path[4096 + 1];
char *config_file_real_path = NULL;

char first_backend_name[256];

void print_version(void) {
  char *version_string = get_version_string();
  if (version_string) {
    printf("%s\n", version_string);
    free(version_string);
  } else {
    debug(1, "Can't print version string!");
  }
}

#ifdef CONFIG_AIRPLAY_2
int has_fltp_capable_aac_decoder(void) {
  // return 1 if the AAC decoder advertises fltp decoding capability, which
  // is needed for decoding Buffered Audio streams
  debug(3, "checking availability of an  fltp-capable aac decoder");
  int has_capability = 0;
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
  if (codec != NULL) {
    const enum AVSampleFormat *formats = NULL;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    // New API (FFmpeg 7.1+) for getting formats
    debug(3, "getting sample formats the new way");
    int format_count = 0;
    if ((avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                      (const void **)&formats, &format_count) < 0) ||
        (format_count == 0))
      formats = NULL; // not clear if the returned pointer is nulled on error or on zero items
#else
    debug(3, "getting sample formats the old way");
    // older API
    formats = codec->sample_fmts;
#endif
    if (formats != NULL) {
      while ((has_capability == 0) && (*formats != AV_SAMPLE_FMT_NONE)) {
        if (*formats == AV_SAMPLE_FMT_FLTP) {
          has_capability = 1;
        }
        formats++;
      }
    }
  } else {
    debug(3, "no AAC codec found.");
  }
  return has_capability;
}
#endif

#ifdef CONFIG_SOXR
// libsoxr keeps a process-wide FFT cache and, the first time it is used,
// registers an atexit() handler that frees it. If that happens after
// exit_function() has been registered, the cache is freed before
// exit_function() (and exit_rtsp_listener()) have stopped the threads that
// use libsoxr, so a thread still inside soxr_oneshot() reads freed memory and
// the program crashes on exit. Do the same conversions that soxr_time_check()
// and the player do here, before registering our own exit handlers, so that
// libsoxr's handler is registered first and therefore runs last. (Only the
// downsampling conversion goes through the FFT stage, but do both anyway.)
static void soxr_register_exit_handler_early(void) {
  const int buffer_length = 352;
  int32_t inbuffer[buffer_length * 2];
  int32_t outbuffer[(buffer_length + 1) * 2];
  memset(inbuffer, 0, sizeof(inbuffer));

  soxr_io_spec_t io_spec = soxr_io_spec(SOXR_INT32_I, SOXR_INT32_I);
  size_t odone;
  for (int stuff = -1; stuff <= 1; stuff += 2)
    soxr_oneshot(buffer_length, buffer_length + stuff, 2, inbuffer, buffer_length, NULL,
                 outbuffer, buffer_length + stuff, &odone, &io_spec, NULL, NULL);
}

pthread_t *soxr_time_check_thread = NULL;
void *soxr_time_check(__attribute__((unused)) void *arg) {
  // this just checks how long it takes to process adding and subtracing a frame
  // from a buffer at 44100
  // #include <syscall.h>
  // debug(1, "soxr_time_check PID %ld", syscall(SYS_gettid));

  const int buffer_length = 352;
  int32_t inbuffer[buffer_length * 2];
  int32_t outbuffer[(buffer_length + 1) * 2];

  // int32_t *outbuffer = (int32_t*)malloc((buffer_length+1)*2*sizeof(int32_t));
  // int32_t *inbuffer = (int32_t*)malloc((buffer_length)*2*sizeof(int32_t));

  // generate a sample signal
  const double frequency = 440; //

  int i;

  int number_of_iterations = 0;
  uint64_t soxr_start_time = get_absolute_time_in_ns();
  uint64_t loop_until_time =
      (uint64_t)1500000000 + soxr_start_time; // loop for a second and a half, max -- no need to be
                                              // able to cancel it, do _don't even try_!
  while (get_absolute_time_in_ns() < loop_until_time) {
    pthread_testcancel();
    number_of_iterations++;
    for (i = 0; i < buffer_length; i++) {
      double w = sin(i * (frequency + number_of_iterations * 2) * 2 * M_PI / 44100);
      int32_t wint = (int32_t)(w * INT32_MAX);
      inbuffer[i * 2] = wint;
      inbuffer[i * 2 + 1] = wint;
    }

    int oldState;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState); // make this un-cancellable

    soxr_io_spec_t io_spec;
    io_spec.itype = SOXR_INT32_I;
    io_spec.otype = SOXR_INT32_I;
    io_spec.scale = 1.0; // this seems to crash if not = 1.0
    io_spec.e = NULL;
    io_spec.flags = 0;

    size_t odone;

    soxr_oneshot(buffer_length, buffer_length + 1, 2,  // Rates and # of chans.
                 inbuffer, buffer_length, NULL,        // Input.
                 outbuffer, buffer_length + 1, &odone, // Output.
                 &io_spec,                             // Input, output and transfer spec.
                 NULL, NULL);                          // Default configuration.

    io_spec.itype = SOXR_INT32_I;
    io_spec.otype = SOXR_INT32_I;
    io_spec.scale = 1.0; // this seems to crash if not = 1.0
    io_spec.e = NULL;
    io_spec.flags = 0;

    soxr_oneshot(buffer_length, buffer_length - 1, 2,  // Rates and # of chans.
                 inbuffer, buffer_length, NULL,        // Input.
                 outbuffer, buffer_length - 1, &odone, // Output.
                 &io_spec,                             // Input, output and transfer spec.
                 NULL, NULL);                          // Default configuration.

    pthread_setcancelstate(oldState, NULL);
  }

  int64_t soxr_execution_time =
      get_absolute_time_in_ns() - soxr_start_time;   // this must be zero or positive
  int soxr_execution_time_int = soxr_execution_time; // must be in or around 1500000000

  // free(outbuffer);
  // free(inbuffer);

  if (number_of_iterations != 0) {
    config.soxr_delay_index = soxr_execution_time_int / number_of_iterations;
  } else {
    debug(1, "No soxr-timing iterations performed, so \"vernier\" iteration will be used.");
    config.soxr_delay_index = 0; // used as a flag
  }
  debug(2, "soxr_delay maximum permissible: %d milliseconds, actual: %f milliseconds.",
        config.soxr_delay_threshold / 1000000, 1E-6 * config.soxr_delay_index);
  if ((config.packet_stuffing == ST_soxr) &&
      (config.soxr_delay_index > config.soxr_delay_threshold))
    inform("Note: this device may be too slow for \"soxr\" interpolation. Consider choosing the "
           "\"auto\", \"vernier\" or \"basic\" interpolation setting.");
  if (config.packet_stuffing == ST_auto)
    debug(
        1, "\"%s\" interpolation has been chosen.",
        ((config.soxr_delay_index != 0) && (config.soxr_delay_index <= config.soxr_delay_threshold))
            ? "soxr"
            : "vernier");
  pthread_exit(NULL);
}
#endif

void usage(char *progname) {

#ifdef CONFIG_AIRPLAY_2
  if (has_fltp_capable_aac_decoder() == 0) {
    printf("\nIMPORTANT NOTE: Shairport Sync can not run on this system.\n");
    printf("A Floating Planar (\"fltp\") AAC decoder is required, ");
    printf("but the system's ffmpeg library does not seem to include one.\n");
    printf("See: "
           "https://github.com/mikebrady/shairport-sync/blob/development/"
           "TROUBLESHOOTING.md#aac-decoder-issues-airplay-2-only\n\n");

  } else {
#endif
    // clang-format off
    printf("Please use the configuration file for settings where possible.\n");
    printf("Many more settings are available in the configuration file.\n");
    printf("\n");
    printf("Usage: %s [options...]\n", progname);
    printf("  or:  %s [options...] -- [audio output-specific options]\n", progname);
    printf("\n");
    printf("Options:\n");
    printf("    -h, --help              Show this help.\n");
    printf("    -V, --version           Show version information -- the version string.\n");
    printf("    --service-type          Select the type of AirPlay service: \"auto\", \"airplay2\" or \"classic\". (You can use \"airplay1\" in place of \"classic\".)\n");
    printf("    -X, --displayConfig     Output OS information, version string, command line, configuration file and active settings to the log.\n");
    printf("    --statistics            Print some interesting statistics. More will be printed if -v / -vv / -vvv are also chosen.\n");
    printf("    -v, --verbose           Print debug information; -v some; -vv more; -vvv lots -- generally too much.\n");
    printf("    -c, --configfile=FILE   Read configuration settings from FILE. Default is %s.\n", configuration_file_path);
    printf("    -a, --name=NAME         Set service name. Default is the hostname with first letter capitalised.\n");
    printf("    --password=PASSWORD     Require PASSWORD to connect. Default is no password.\n");
    printf("    -p, --port=PORT         Set RTSP listening port. Default 5000; 7000 for AirPlay 2.\n");
    printf("    -L, --latency=FRAMES    [Deprecated] Set the latency for audio sent from an unknown device.\n");
    printf("                            The default is to set it automatically.\n");
    printf("    -S, --stuffing=MODE     Set how to adjust current latency to match desired latency, where:\n");
    printf("                            \"vernier\" recodes a packet of frames to a new packet containing more or fewer frames. Recommended for low powered devices;\n");
    printf("                            \"basic\" inserts or deletes audio frames from packet frames with low processor overhead; and\n");
    printf("                            \"soxr\" uses libsoxr to minimally resample packet frames -- moderate floating point processor overhead.\n");
    printf("                            The default \"auto\" setting chooses vernier or soxr depending on processor capability.\n");
    printf("                            The \"soxr\" option is only available if built with soxr support.\n");
    printf("    -B, --on-start=PROGRAM  Run PROGRAM when playback is about to begin.\n");
    printf("    -E, --on-stop=PROGRAM   Run PROGRAM when playback has ended.\n");
    printf("                            For -B and -E options, specify the full path to the program and arguments, e.g. \"/usr/bin/logger\".\n");
    printf("                            Executable scripts work, but the file must be marked executable have the appropriate shebang (#!/bin/sh) on the first line.\n");
    printf("    -w, --wait-cmd          Wait until the -B or -E programs finish before continuing.\n");
    printf("    -o, --output=BACKEND    Select audio backend. They are listed at the end of this text. The first one is the default.\n");
    printf("    -m, --mdns=BACKEND      Use the mDNS backend named BACKEND to advertise the AirPlay service through Bonjour/ZeroConf.\n");
    printf("                            They are listed at the end of this text.\n");
    printf("                            If no mdns backend is specified, they are tried in order until one works.\n");
    printf("    -r, --resync=THRESHOLD  [Deprecated] resync if error exceeds this number of frames. Set to 0 to stop resyncing.\n");
    printf("    -t, --timeout=SECONDS   Go back to idle mode from play mode after a break in communications of this many seconds (default 60). Set to 0 never to exit play mode.\n");
    printf("    --tolerance=TOLERANCE   [Deprecated] Allow a synchronization error of TOLERANCE frames (default 88) before trying to correct it.\n");
    printf("    --logOutputLevel        Log the output level setting -- a debugging option, useful for determining the optimum maximum volume.\n");
#ifdef CONFIG_AIRPLAY_2
    printf("    --nqptp-shared-memory-interface-name=NAME\n");
    printf("                            Use the nqptp shared memory interface called NAME. Default is \"%s\".\n", NQPTP_INTERFACE_NAME);
    printf("                            An advanced setting, needed only when running more than one AirPlay 2 instance on this machine:\n");
    printf("                            give each instance its own name so that they do not share a clock.\n");
#endif

#ifdef CONFIG_LIBDAEMON
    printf("    -d, --daemon            Daemonise.\n");
    printf("    -j, --justDaemoniseNoPIDFile            Daemonise without a PID file.\n");
    printf("    -k, --kill              Kill the existing shairport daemon.\n");
#endif
#ifdef CONFIG_METADATA
    printf("    -M, --metadata-enable   Ask for metadata from the source and process it. Much more flexibility with configuration file settings.\n");
    printf("    --metadata-pipename=PIPE send metadata to PIPE, e.g. --metadata-pipename=/tmp/%s-metadata.\n", config.appName);
    printf("                            The default is /tmp/%s-metadata.\n", config.appName);
    printf("    -g, --get-coverart      Include cover art in the metadata to be gathered and sent.\n");
#endif
#if defined(CONFIG_DBUS_INTERFACE) || defined(CONFIG_MPRIS_INTERFACE)
    printf("    --dbus-default-message-bus=BUS Use the BUS specified for the native D-Bus and MPRIS interfaces.\n");
    printf("                            BUS must be \"system\" (default) or \"session\".\n");
    printf("    --dbus-nametag=TAG      Append TAG to the native D-Bus and MPRIS bus names, e.g. org.gnome.ShairportSync.TAG.\n");
#endif
    printf("    --log-to-syslog         Send debug and statistics information through syslog\n");
    printf("                            If used, this should be the first command line argument.\n");
    printf("    -u, --use-stderr        [Deprecated] This setting is not needed -- stderr is now used by default and syslog is selected using --log-to-syslog.\n");
    printf("\n");
    mdns_ls_backends();
    printf("\n");
    audio_ls_outputs();
    // clang-format on

#ifdef CONFIG_AIRPLAY_2
  }
#endif
}

#ifdef CONFIG_AIRPLAY_2
// The nqptp shared memory interface name is passed to shm_open(3) and is stored
// by nqptp in a 64-byte field, so it must be a "/" followed by between one and
// 62 characters, with no further "/". "source" names where the value came from,
// for the error message.
void check_nqptp_shared_memory_interface_name(const char *name, const char *source) {
  if ((name[0] != '/') || (strlen(name) < 2) || (strlen(name) > 63) ||
      (strchr(name + 1, '/') != NULL))
    die("Invalid %s \"%s\" -- it must begin with a \"/\", contain no other \"/\", and be between "
        "2 and 63 characters long.",
        source, name);
}
#endif

int parse_options(int argc, char **argv) {
  int should_exit = 0;
  int rc = aircrab_parse_options(argc, argv, &should_exit);
  if (should_exit != 0) {
    exit(rc);
  }
  return argc;
}

#if defined(CONFIG_DBUS_INTERFACE) || defined(CONFIG_MPRIS_INTERFACE)

GThread *glib_worker_thread = NULL;

gpointer glib_worker_thread_function(__attribute__((unused)) gpointer data) {

  // use the default global-default main context
  glib_worker_loop = g_main_loop_new(NULL, FALSE);

  // debug(1, "glib worker thread started.");

#ifdef CONFIG_DBUS_INTERFACE
  debug(2, "starting up D-Bus services");
  start_dbus_service();
#endif
#ifdef CONFIG_MPRIS_INTERFACE
  debug(2, "starting up MPRIS services");
  start_mpris_service();
#endif

  // debug(1, "g_main_loop_run start.");

  g_main_loop_run(glib_worker_loop);

  // debug(1, "g_main_loop_run exit.");

#ifdef CONFIG_MPRIS_INTERFACE
  debug(2, "stopping MPRIS service");
  stop_mpris_service();
  // debug(1, "stopped MPRIS service");
#endif

#ifdef CONFIG_DBUS_INTERFACE
  debug(2, "stopping D-Bus service");
  stop_dbus_service();
  // debug(1, "stopped D-Bus service");
#endif

  g_main_loop_unref(glib_worker_loop);
  return NULL;
}

#endif

#ifdef CONFIG_LIBDAEMON
char pid_file_path_string[4096] = "\0";

const char *pid_file_proc(void) {
  snprintf(pid_file_path_string, sizeof(pid_file_path_string), "%s/%s.pid", config.computed_piddir,
           daemon_pid_file_ident ? daemon_pid_file_ident : "unknown");
  debug(1, "PID file: \"%s\".", pid_file_path_string);
  return pid_file_path_string;
}
#endif

void exit_rtsp_listener() {
  debug(3, "exit_rtsp_listener begins");
  pthread_cancel(rtsp_listener_thread);
  pthread_join(rtsp_listener_thread, NULL); // not sure you need this
  debug(2, "exit_rtsp_listener ends");
}

void exit_function() {
  // the following is to ensure that if libdaemon has been included
  // that most of this code will be skipped when the parent process is exiting
  // exec
#ifdef CONFIG_LIBDAEMON
  if ((this_is_the_daemon_process) ||
      (config.daemonise == 0)) { // if this is the daemon process that is exiting or it's not
                                 // actually daemonised at all
#endif
    /*
    Actually, there is no terminate_mqtt() function.
    #ifdef CONFIG_MQTT
            if (config.mqtt_enabled) {
                    terminate_mqtt();
            }
    #endif
    */

    debug(2, "Stopping the activity monitor.");
    activity_monitor_stop();
    debug(2, "Stopping the activity monitor done.");

#ifdef CONFIG_DACP_CLIENT
    debug(2, "Stopping DACP Monitor");
    dacp_monitor_stop();
    debug(2, "Stopping DACP Monitor Done");
#endif

#if defined(CONFIG_DBUS_INTERFACE) || defined(CONFIG_MPRIS_INTERFACE)
    if (glib_worker_loop != NULL) { // may not have been initialised
      g_main_loop_quit(glib_worker_loop);
      debug(3, "GMainLoop stop requested");
    }
    if (glib_worker_thread != NULL) {
      g_thread_join(glib_worker_thread);
      debug(3, "GLib worker thread joined");
    }

#endif

#ifdef CONFIG_METADATA_HUB
    debug(2, "Stopping metadata hub");
    metadata_hub_stop();
    debug(2, "Stopping metadata done");
#endif

#ifdef CONFIG_METADATA
    debug(2, "Stopping metadata");
    metadata_stop(); // close down the metadata pipe
    debug(2, "Stopping metadata done");
#endif

    if ((config.output) && (config.output->deinit)) {
      debug(2, "Deinitialise the audio backend.");
      config.output->deinit();
      debug(2, "Deinitialise the audio backend done.");
    }

#ifdef CONFIG_SOXR
    {
      int oldState;
      pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState); // make this un-cancellable
      if (soxr_time_check_thread != NULL) {
        pthread_cancel(*soxr_time_check_thread);
        pthread_join(*soxr_time_check_thread, NULL);
        free(soxr_time_check_thread);
        soxr_time_check_thread = NULL;
      }
      pthread_setcancelstate(oldState, NULL);
    }

#endif

    if (config.service_name)
      free(config.service_name);

#ifdef CONFIG_MQTT
    if (config.mqtt_empty_payload_substitute)
      free(config.mqtt_empty_payload_substitute);
#endif

#ifdef CONFIG_CONVOLUTION
    if (config.convolution_ir_files) {
      free_ir_filenames(config.convolution_ir_files, config.convolution_ir_file_count);
      config.convolution_ir_files = NULL;
      config.convolution_ir_file_count = 0;
    }
    convolver_pool_closedown();
#endif

    if (config.regtype)
      free(config.regtype);
    if (config.model)
      free(config.model);
    if (config.srcvers)
      free(config.srcvers);
    if (config.osvers)
      free(config.osvers);

#ifdef CONFIG_AIRPLAY_2
    if (config.regtype2)
      free(config.regtype2);
    if (config.nqptp_shared_memory_interface_name)
      free(config.nqptp_shared_memory_interface_name);
    if (config.airplay_device_id)
      free(config.airplay_device_id);
    if (config.airplay_pi)
      free(config.airplay_pi);
    if (config.airplay_pgid)
      free(config.airplay_pgid);
    if (config.airplay_psi)
      free(config.airplay_psi);
    if (config.pk_string)
      free(config.pk_string);
    if (config.firmware_version)
      free(config.firmware_version);
    ptp_shm_interface_close(); // close it if it's open
#endif

#ifdef CONFIG_LIBDAEMON
    if (this_is_the_daemon_process) {
      daemon_retval_send(0);
      daemon_pid_file_remove();
      daemon_signal_done();
      if (config.computed_piddir)
        free(config.computed_piddir);
    }
  }
#endif
  if (config.cfg)
    config_destroy(config.cfg);
  if (config_file_real_path)
    free(config_file_real_path);
  if (config.appName)
    free(config.appName);

  // probably should be freeing malloc'ed memory here, including strdup-created strings...

#ifdef CONFIG_LIBDAEMON
  if (this_is_the_daemon_process) { // this is the daemon that is exiting
    mdns_unregister(); // once the dacp handler is done and all player threrads are done it should
                       // be safe
    debug(1, "libdaemon daemon process exit");
  } else {
    if (config.daemonise)
      debug(1, "libdaemon parent process exit");
    else
      debug(1, "normal exit");
  }
#else
  mdns_unregister(); // once the dacp handler is done and all player threads are done it should
                     // be safe
  debug(2, "normal exit");
#endif
}

// for removing zombie script processes
// see: http://www.microhowto.info/howto/reap_zombie_processes_using_a_sigchld_handler.html
// used with thanks.

void handle_sigchld(__attribute__((unused)) int sig) {
  int saved_errno = errno;
  while (waitpid((pid_t)(-1), 0, WNOHANG) > 0) {
  }
  errno = saved_errno;
}

// for clean exits
void intHandler(__attribute__((unused)) int k) {
  debug(2, "exit on SIGINT");
  exit_request(EXIT_SUCCESS);
}

void termHandler(__attribute__((unused)) int k) {
  debug(2, "exit on SIGTERM");
  exit_request(EXIT_SUCCESS);
}

void _display_config(const char *filename, const int linenumber, __attribute__((unused)) int argc,
                     __attribute__((unused)) char **argv) {

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-zero-length"
#endif

  _inform(filename, linenumber, ">> Display Config Start.");

  // see the man entry on popen
  FILE *fp;
  int status;
  char result[1024];

  fp = popen("uname -a 2>/dev/null", "r");
  if (fp != NULL) {
    if (fgets(result, 1024, fp) != NULL) {
      _inform(filename, linenumber, "");
      _inform(filename, linenumber, "From \"uname -a\":");
      if (result[strlen(result) - 1] <= ' ')
        result[strlen(result) - 1] = '\0'; // remove the last character if it's not printable
      _inform(filename, linenumber, " %s", result);
    }
    status = pclose(fp);
    if (status == -1) {
      debug(1, "Error on pclose");
    }
  }

  fp = popen("(cat /etc/os-release | grep PRETTY_NAME | sed 's/PRETTY_NAME=//' | sed 's/\"//g') "
             "2>/dev/null",
             "r");
  if (fp != NULL) {
    if (fgets(result, 1024, fp) != NULL) {
      _inform(filename, linenumber, "");
      _inform(filename, linenumber, "From /etc/os-release:");
      if (result[strlen(result) - 1] <= ' ')
        result[strlen(result) - 1] = '\0'; // remove the last character if it's not printable
      _inform(filename, linenumber, " %s", result);
    }
    status = pclose(fp);
    if (status == -1) {
      debug(1, "Error on pclose");
    }
  }

  fp = popen("cat /sys/firmware/devicetree/base/model 2>/dev/null", "r");
  if (fp != NULL) {
    if (fgets(result, 1024, fp) != NULL) {
      _inform(filename, linenumber, "");
      _inform(filename, linenumber, "From /sys/firmware/devicetree/base/model:");
      _inform(filename, linenumber, " %s", result);
    }
    status = pclose(fp);
    if (status == -1) {
      debug(1, "Error on pclose");
    }
  }

  char *version_string = get_version_string();
  if (version_string) {
    _inform(filename, linenumber, "");
    _inform(filename, linenumber, "Shairport Sync Version String:");
    _inform(filename, linenumber, " %s", version_string);
    free(version_string);
  } else {
    debug(1, "Can't print version string!\n");
  }

  if (argc != 0) {
    char *obfp = result;
    int i;
    for (i = 0; i < argc - 1; i++) {
      snprintf(obfp, strlen(argv[i]) + 2, "%s ", argv[i]);
      obfp += strlen(argv[i]) + 1;
    }
    snprintf(obfp, strlen(argv[i]) + 1, "%s", argv[i]);
    obfp += strlen(argv[i]);
    *obfp = 0;

    _inform(filename, linenumber, "");
    _inform(filename, linenumber, "Command Line:");
    _inform(filename, linenumber, " %s", result);
  }

  if (config.cfg == NULL)
    _inform(filename, linenumber, "No configuration file.");
  else {
    int configpipe[2];
    if (pipe(configpipe) == 0) {
      FILE *cw;
      cw = fdopen(configpipe[1], "w");
      _inform(filename, linenumber, "");
      _inform(filename, linenumber, "Configuration File:");
      _inform(filename, linenumber, " %s", config_file_real_path);
      _inform(filename, linenumber, "");
      config_write(config.cfg, cw);
      fclose(cw);
      // get back the raw configuration file settings text
      FILE *cr;
      cr = fdopen(configpipe[0], "r");
      int i = 0;
      int ch = 0;
      do {
        ch = fgetc(cr);
        if (ch == EOF) {
          result[i] = '\0';
        } else {
          result[i] = (char)ch;
          i++;
        }
      } while (ch != EOF);
      fclose(cr);
      // debug(1,"result is \"%s\".",result);
      // remove empty stanzas
      char *i0 = str_replace(result, "general : \n{\n};\n", "");
      char *i1 = str_replace(i0, "sessioncontrol : \n{\n};\n", "");
      char *i2 = str_replace(i1, "alsa : \n{\n};\n", "");
      char *i3 = str_replace(i2, "sndio : \n{\n};\n", "");
      char *i4 = str_replace(i3, "pulseaudio : \n{\n};\n", "");
      char *i5 = str_replace(i4, "pipe : \n{\n};\n", "");
      char *i6 = str_replace(i5, "dsp : \n{\n};\n", "");
      char *i7 = str_replace(i6, "metadata : \n{\n};\n", "");
      char *i8 = str_replace(i7, "mqtt : \n{\n};\n", "");
      char *i9 = str_replace(i8, "diagnostics : \n{\n};\n", "");
      char *i10 = str_replace(i9, "pipewire : \n{\n};\n", "");
      char *i11 = str_replace(i10, "stdout : \n{\n};\n", "");
      char *i12 = str_replace(i11, "pipe : \n{\n};\n", "");
      char *i13 = str_replace(i12, "ao : \n{\n};\n", "");
      // debug(1,"i10 is \"%s\".",i10);

      // free intermediate strings
      free(i12);
      free(i11);
      free(i10);
      free(i9);
      free(i8);
      free(i7);
      free(i6);
      free(i5);
      free(i4);
      free(i3);
      free(i2);
      free(i1);
      free(i0);

      // print it out
      if (strlen(i13) == 0)
        _inform(filename, linenumber, "The Configuration file contains no active settings.");
      else {
        _inform(filename, linenumber, "Configuration File Settings:");
        char *p = i13;
        while (*p != '\0') {
          i = 0;
          while ((*p != '\0') && (*p != '\n')) {
            result[i] = *p;
            p++;
            i++;
          }
          if (i != 0) {
            result[i] = '\0';
            _inform(filename, linenumber, " %s", result);
          }
          if (*p == '\n')
            p++;
        }
      }

      free(i13); // free the cleaned-up configuration string

      /*
            while (fgets(result, 1024, cr) != NULL) {
              // replace funny character at the end, if it's there
              if (result[strlen(result) - 1] <= ' ')
                result[strlen(result) - 1] = '\0'; // remove the last character if it's not
         printable _inform(filename, linenumber, " %s", result);
            }
      */
    } else {
      debug(1, "Error making pipe.\n");
    }
  }
  _inform(filename, linenumber, "");
  _inform(filename, linenumber, ">> Display Config End.");

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

#define display_config(argc, argv) _display_config(__FILE__, __LINE__, argc, argv)

#ifdef CONFIG_FFMPEG

/*
typedef struct {           // channel layout names and equates -- see
                           // https://www.ffmpeg.org/doxygen/2.4/group__channel__mask__c.html
  const char *name;        // e.g. "AV_CH_LAYOUT_5POINT1"
  uint64_t channel_layout; // e.g. AV_CH_LAYOUT_5POINT1
} channel_layout_t;
channel_layout_t channel_layouts[] = {
    {"AV_CH_LAYOUT_MONO", AV_CH_LAYOUT_MONO},
    {"AV_CH_LAYOUT_STEREO", AV_CH_LAYOUT_STEREO},
    {"AV_CH_LAYOUT_2POINT1", AV_CH_LAYOUT_2POINT1},
    {"AV_CH_LAYOUT_2_1", AV_CH_LAYOUT_2_1},
    {"AV_CH_LAYOUT_SURROUND", AV_CH_LAYOUT_SURROUND},
    {"AV_CH_LAYOUT_3POINT1", AV_CH_LAYOUT_3POINT1},
    {"AV_CH_LAYOUT_4POINT0", AV_CH_LAYOUT_4POINT0},
    {"AV_CH_LAYOUT_4POINT1", AV_CH_LAYOUT_4POINT1},
    {"AV_CH_LAYOUT_2_2", AV_CH_LAYOUT_2_2},
    {"AV_CH_LAYOUT_QUAD", AV_CH_LAYOUT_QUAD},
    {"AV_CH_LAYOUT_5POINT0", AV_CH_LAYOUT_5POINT0},
    {"AV_CH_LAYOUT_5POINT1", AV_CH_LAYOUT_5POINT1},
    {"AV_CH_LAYOUT_5POINT0_BACK", AV_CH_LAYOUT_5POINT0_BACK},
    {"AV_CH_LAYOUT_5POINT1_BACK", AV_CH_LAYOUT_5POINT1_BACK},
    {"AV_CH_LAYOUT_6POINT0", AV_CH_LAYOUT_6POINT0},
    {"AV_CH_LAYOUT_6POINT0_FRONT", AV_CH_LAYOUT_6POINT0_FRONT},
    {"AV_CH_LAYOUT_HEXAGONAL", AV_CH_LAYOUT_HEXAGONAL},
    {"AV_CH_LAYOUT_6POINT1", AV_CH_LAYOUT_6POINT1},
    {"AV_CH_LAYOUT_6POINT1_BACK", AV_CH_LAYOUT_6POINT1_BACK},
    {"AV_CH_LAYOUT_6POINT1_FRONT", AV_CH_LAYOUT_6POINT1_FRONT},
    {"AV_CH_LAYOUT_7POINT0", AV_CH_LAYOUT_7POINT0},
    {"AV_CH_LAYOUT_7POINT0_FRONT", AV_CH_LAYOUT_7POINT0_FRONT},
    {"AV_CH_LAYOUT_7POINT1", AV_CH_LAYOUT_7POINT1},
    {"AV_CH_LAYOUT_7POINT1_WIDE", AV_CH_LAYOUT_7POINT1_WIDE},
    {"AV_CH_LAYOUT_7POINT1_WIDE_BACK", AV_CH_LAYOUT_7POINT1_WIDE_BACK},
    {"AV_CH_LAYOUT_OCTAGONAL", AV_CH_LAYOUT_OCTAGONAL},
    //        {"AV_CH_LAYOUT_STEREO_DOWNMIX",	AV_CH_LAYOUT_STEREO_DOWNMIX},
};

uint64_t av_channel_layout_lookup(const char *str) {
  unsigned int m;
  uint64_t response = 0;
  for (m = 0; (m < sizeof(channel_layouts) / sizeof(channel_layout_t)) && (response == 0); m++) {
    if (strcasecmp(str, channel_layouts[m].name) == 0) {
      response = channel_layouts[m].channel_layout;
    }
  }
  return response;
}

const char *av_channel_layout_name(uint64_t channel_layout) {
  const char *response = NULL;
  unsigned int m;
  for (m = 0; (m < sizeof(channel_layouts) / sizeof(channel_layout_t)) && (response == NULL); m++) {
    if (channel_layouts[m].channel_layout == channel_layout) {
      response = channel_layouts[m].name;
    }
  }
  return response;
}
*/

#endif

int main(int argc, char **argv) {
  exit_init(); // initialise the exit handler to give us a clean safe exit on request
  // initialise debug messages stuff -- level 0, no elapsed time, relative time, file and line
  // debug_init(int level, int show_elapsed_time, int show_relative_time, int show_file_and_line)
  debug_init(0, 0, 1, 1, exit_request);
  memset(&config, 0, sizeof(config)); // also clears all strings, BTW
#if defined(CONFIG_MBEDTLS) && MBEDTLS_VERSION_MAJOR >= 4
  // Mbed TLS 4 does all hashing, encryption and RSA through PSA
  if (psa_crypto_init() != PSA_SUCCESS)
    die("can not initialise PSA Crypto!");
#endif
  /* Check if we are called with -V or --version parameter */
  if (argc >= 2 && ((strcmp(argv[1], "-V") == 0) || (strcmp(argv[1], "--version") == 0))) {
    print_version();
    exit(EXIT_SUCCESS);
  }

  // this is a bit weird, but necessary -- basename() may modify the argument passed in
  char *basec = strdup(argv[0]);
  char *bname = basename(basec);
  config.appName = strdup(bname);
  if (config.appName == NULL)
    die("can not allocate memory for the app name!");
  free(basec);

  strcpy(configuration_file_path, SYSCONFDIR);
  // strcat(configuration_file_path, "/shairport-sync"); // thinking about adding a special
  // shairport-sync directory
  strcat(configuration_file_path, "/");
  strcat(configuration_file_path, config.appName);
  strcat(configuration_file_path, ".conf");
  config.configfile = configuration_file_path;

#ifdef CONFIG_AIRPLAY_2
  config.get_plist_metadata = 1; // default to using AirPlay-2-type plist information
  config.lossless_mode = 1;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(53, 10, 0)
  avcodec_init();
#endif
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
  avcodec_register_all();
#endif
  if (debug_level() == 0)
    av_log_set_level(AV_LOG_ERROR);
  else
    av_log_set_level(AV_LOG_VERBOSE);
#endif

  /* Check if we are called with -h or --help parameter */
  if (argc >= 2 && ((strcmp(argv[1], "-h") == 0) || (strcmp(argv[1], "--help") == 0))) {
    usage(argv[0]);
    exit(EXIT_SUCCESS);
  }

  /*
    // Check if we are called with -log-to-syslog
    if (argc >= 2 && (strcmp(argv[1], "--log-to-syslog") == 0)) {
      log_to_syslog_select_is_first_command_line_argument = 1;
      log_to_syslog();
    } else {
      log_to_stderr();
    }
  */

  pid = getpid();
  config.log_fd = -1;

#ifdef CONFIG_LIBDAEMON
  daemon_set_verbosity(LOG_DEBUG);
#else
  setlogmask(LOG_UPTO(LOG_DEBUG));
  openlog(NULL, 0, LOG_DAEMON);
#endif
#ifdef CONFIG_SOXR
  soxr_register_exit_handler_early(); // must come before atexit(exit_function)
#endif
  debug(1, "adding the exit function");
  atexit(exit_function);

  config.service_type = APST_auto; // this may be changed by the settings...

  // get a device id -- the first non-local MAC address
  get_device_id((uint8_t *)&config.hw_addr, 6);

  // get the endianness
  union {
    uint32_t u32;
    uint8_t arr[4];
  } xn;

  xn.arr[0] = 0x44; /* Lowest-address byte */
  xn.arr[1] = 0x33;
  xn.arr[2] = 0x22;
  xn.arr[3] = 0x11; /* Highest-address byte */

  if (xn.u32 == 0x11223344)
    config.endianness = SS_LITTLE_ENDIAN;
  else if (xn.u32 == 0x33441122)
    config.endianness = SS_PDP_ENDIAN;
  else if (xn.u32 == 0x44332211)
    config.endianness = SS_BIG_ENDIAN;
  else
    die("Can not recognise the endianness of the processor.");

  // set non-zero / non-NULL default values here
  // but note that audio back ends also have a chance to set defaults

  // get the first output backend in the list and make it the default
  audio_output *first_backend = audio_get_output(NULL);
  if (first_backend == NULL) {
    die("No audio backend found! Check your build of Shairport Sync.");
  } else {
    strncpy(first_backend_name, first_backend->name, sizeof(first_backend_name) - 1);
    config.output_name = first_backend_name;
  }

  // config.statistics_requested = 0; // don't print stats in the log
  // config.userSuppliedLatency = 0; // zero means none supplied

  config.debugger_show_file_and_line =
      1; // by default, log the file and line of the originating message
  config.debugger_show_relative_time =
      1;               // by default, log the  time back to the previous debug message
  config.timeout = 60; // wait this number of seconds to wait for a dropped RTSP connection to come
                       // back before declaring it lost.
  config.buffer_start_fill = 220;

  config.resync_threshold = 0.050; // default
  config.tolerance = 0.002;

#ifdef CONFIG_SOXR
  config.packet_stuffing = ST_auto; // use soxr interpolation by default if support has been
                                    // included and if the CPU is fast enough
#else
  config.packet_stuffing = ST_vernier; // you need to explicitly ask for "basic" (ST_basic)
#endif

  // set_requested_connection_state_to_output(
  //     1); // we expect to be able to connect to the output device
  config.audio_backend_buffer_desired_length = 0.15; // seconds
  config.audio_decoded_buffer_desired_length = 0.75; // seconds
  config.udp_port_base = 6001;
  config.udp_port_range = 10;

  config.current_output_configuration = 0; // no output configuration selected...

  // config.output_format = SPS_FORMAT_S16_LE; // default
  config.output_rate_auto_requested = 1;   // default auto select format
  config.output_format_auto_requested = 1; // default auto select format

#ifdef CONFIG_HAMMERTON
  config.decoders_supported |= 1 << decoder_hammerton; // David Hammerton's decoder (deprecated)
  config.decoder_in_use = 1 << decoder_hammerton;
#endif
#ifdef CONFIG_APPLE_ALAC
  config.decoders_supported |= 1 << decoder_apple_alac; // Apple ALAC decoder (deprecated)
  config.decoder_in_use = 1 << decoder_apple_alac;      // If present, use this in preference
#endif
#ifdef CONFIG_FFMPEG
  config.decoders_supported |= 1 << decoder_ffmpeg_alac;
  config.decoder_in_use = 1 << decoder_ffmpeg_alac; // If present, use this in preference
#endif

  config.output_channel_mapping_enable = 1; // enabled by default
  config.output_channel_map_size = 0;       // use the device's channel map if it has one
  config.mixdown_enable = 1;                // enabled by default
  config.mixdown_channel_layout =
      0; // 0 means pick a mixdown based on the number of output channels

  // initialise random number generator

  r64init(0);

#ifdef CONFIG_LIBDAEMON

  /* Reset signal handlers */
  if (daemon_reset_sigs(-1) < 0) {
    daemon_log(LOG_ERR, "Failed to reset all signal handlers: %s", strerror(errno));
    return 1;
  }

  /* Unblock signals */
  if (daemon_unblock_sigs(-1) < 0) {
    daemon_log(LOG_ERR, "Failed to unblock all signals: %s", strerror(errno));
    return 1;
  }

  /* Set identification string for the daemon for both syslog and PID file */
  daemon_pid_file_ident = daemon_log_ident = daemon_ident_from_argv0(argv[0]);

  daemon_pid_file_proc = pid_file_proc;

#endif
  // parse arguments into config -- needed to locate pid_dir
  int audio_arg = parse_options(argc, argv);

  if (display_config_selected != 0) {
    display_config(argc, argv);
    if (argc == 2) {
      inform(">> Goodbye!");
      exit(EXIT_SUCCESS);
    }
  }

  /* Check if we are called with -k or --kill option */
  if (killOption != 0) {
#ifdef CONFIG_LIBDAEMON
    int ret;

    /* Kill daemon with SIGTERM */
    /* Check if the new function daemon_pid_file_kill_wait() is available, if it is, use it. */
    if ((ret = daemon_pid_file_kill_wait(SIGTERM, 5)) < 0) {
      if (errno == ENOENT)
        warn("Failed to kill the %s daemon. The PID file was not found.", config.appName);
      // daemon_log(LOG_WARNING, "Failed to kill %s daemon: PID file not found.", config.appName);
      else
        warn("Failed to kill the %s daemon. Error: \"%s\", errno %u.", config.appName,
             strerror(errno), errno);
      // daemon_log(LOG_WARNING, "Failed to kill %s daemon: \"%s\", errno %u.", config.appName,
      //            strerror(errno), errno);
    }
    return ret < 0 ? 1 : 0;
#else
    warn("%s was built without libdaemon, so it does not support the -k or --kill option.",
         config.appName);
    return 1;
#endif
  }

#ifdef CONFIG_LIBDAEMON
  /* If we are going to daemonise, check that the daemon is not running already.*/
  if ((config.daemonise) && ((pid = daemon_pid_file_is_running()) >= 0)) {
    warn("The %s daemon is already running with process ID (PID) %u.", config.appName, pid);
    // daemon_log(LOG_ERR, "The %s daemon is already running as PID %u", config.appName, pid);
    return 1;
  }

  /* here, daemonise with libdaemon */

  if (config.daemonise) {
    /* Prepare for return value passing from the initialization procedure of the daemon process */
    if (daemon_retval_init() < 0) {
      die("Failed to create pipe.");
    }

    /* Do the fork */
    if ((pid = daemon_fork()) < 0) {

      /* Exit on error */
      daemon_retval_done();
      return 1;

    } else if (pid) { /* The parent */
      int ret;

      /* Wait for 20 seconds for the return value passed from the daemon process */
      if ((ret = daemon_retval_wait(20)) < 0) {
        die("Could not receive return value from daemon process: %s", strerror(errno));
      }

      switch (ret) {
      case 0:
        break;
      case 1:
        warn("The %s daemon failed to launch: could not close open file descriptors after forking.",
             config.appName);
        break;
      case 2:
        warn("The %s daemon failed to launch: could not create PID file.", config.appName);
        break;
      case 3:
        warn("The %s daemon failed to launch: could not create or access PID directory.",
             config.appName);
        break;
      default:
        warn("The %s daemon failed to launch, error %i.", config.appName, ret);
      }
      return ret;
    } else { /* pid == 0 means we are the daemon */

      this_is_the_daemon_process = 1;
      /*
            if (log_to_default != 0) // if a specific logging mode has not been selected
              log_to_syslog();       // automatically send logs to the daemon_log
      */

      /* Close FDs */
      if (daemon_close_all(-1) < 0) {
        warn("Failed to close all file descriptors while daemonising. Error: %s", strerror(errno));
        /* Send the error condition to the parent process */
        daemon_retval_send(1);
        daemon_signal_done();
        return 0;
      }

      /* Create the PID file if required */
      if (config.daemonise_store_pid) {
        /* Create the PID directory if required -- we don't really care about the result */
        debug(1, "PID directory is \"%s\".", config.computed_piddir);
        int result = mkpath(config.computed_piddir, 0700);
        if ((result != 0) && (result != -EEXIST)) {
          // error creating or accessing the PID file directory
          warn("Failed to create the directory \"%s\" for the PID file. Error: %s.",
               config.computed_piddir, strerror(errno));
          daemon_retval_send(3);
          daemon_signal_done();
          return 0;
        }

        if (daemon_pid_file_create() < 0) {
          // daemon_log(LOG_ERR, "Could not create PID file (%s).", strerror(errno));
          warn("Failed to create the PID file. Error: %s.", strerror(errno));
          daemon_retval_send(2);
          daemon_signal_done();
          return 0;
        }
      }

      /* Send OK to parent process */
      daemon_retval_send(0);
    }
    /* end libdaemon stuff */
  }

#endif

  // Default the RTSP port only if the user did not set one (-p / general.port);
  // config.port is 0 when unset. Honouring an explicit port -- and advertising it,
  // since mdns_register passes config.port -- lets several AirPlay 2 instances share
  // one IP on distinct ports. (AirPlay 2 clients follow the SRV-advertised port,
  // verified on iOS; a port set for AirPlay 2 was previously ignored.)
#ifdef CONFIG_AIRPLAY_2
  if (config.port == 0)
    config.port = (config.service_type == APST_airplay2) ? 7000 : 5000;
#else
  if (config.port == 0)
    config.port = 5000;
#endif

#ifdef CONFIG_AIRPLAY_2
  if (config.service_type == APST_airplay2) {
    if (has_fltp_capable_aac_decoder() == 0) {
      die("Shairport Sync can not run on this system. Run \"shairport-sync -h\" for more "
          "information.");
    }
  }
  uint64_t apf = config.airplay_features;
  uint64_t apfh = config.airplay_features;
  apfh = apfh >> 32;
  uint32_t apf32 = apf;
  uint32_t apfh32 = apfh;
  if (config.service_type == APST_airplay2) {
    debug(1,
          "Startup in AirPlay 2 mode, with features 0x%" PRIx32 ",0x%" PRIx32 " on device \"%s\".",
          apf32, apfh32, config.airplay_device_id);
  } else {
    debug(1, "Startup in Classic AirPlay (aka \"AirPlay 1\") mode. (AirPlay2 build.)");
  }
#else
  debug(1, "Startup in Classic AirPlay (aka \"AirPlay 1\") mode.");
#endif

  // control-c (SIGINT) cleanly
  struct sigaction act;
  memset(&act, 0, sizeof(struct sigaction));
  act.sa_handler = intHandler;
  sigaction(SIGINT, &act, NULL);

  // terminate (SIGTERM)
  struct sigaction act2;
  memset(&act2, 0, sizeof(struct sigaction));
  act2.sa_handler = termHandler;
  sigaction(SIGTERM, &act2, NULL);

  // stop a pipe signal from killing the program
  signal(SIGPIPE, SIG_IGN);

  // install a zombie process reaper
  // see: http://www.microhowto.info/howto/reap_zombie_processes_using_a_sigchld_handler.html
  struct sigaction sa;
  sa.sa_handler = &handle_sigchld;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  if (sigaction(SIGCHLD, &sa, 0) == -1) {
    perror(0);
    exit(1);
  }

  // make sure the program can create files that group and world can read
  umask(S_IWGRP | S_IWOTH);

  /* print out version */

  char *version_dbs = get_version_string();
  if (version_dbs) {
    debug(1, "Version String: \"%s\"", version_dbs);
    free(version_dbs);
  } else {
    debug(1, "Can't print the version information!");
  }

  // print command line

  if (argc != 0) {
    char result[1024];
    char *obfp = result;
    int i;
    for (i = 0; i < argc - 1; i++) {
      snprintf(obfp, strlen(argv[i]) + 2, "%s ", argv[i]);
      obfp += strlen(argv[i]) + 1;
    }
    snprintf(obfp, strlen(argv[i]) + 1, "%s", argv[i]);
    obfp += strlen(argv[i]);
    *obfp = 0;
    debug(1, "Command Line: \"%s\".", result);
  }

#ifdef CONFIG_AIRPLAY_2
  if (sodium_init() < 0) {
    debug(1, "Can't initialise libsodium!");
  } else {
    debug(2, "libsodium initialised.");
  }

  // this code is based on
  // https://www.gnupg.org/documentation/manuals/gcrypt/Initializing-the-library.html

  /* Version check should be the very first call because it
    makes sure that important subsystems are initialized.
    #define NEED_LIBGCRYPT_VERSION to the minimum required version. */

#define NEED_LIBGCRYPT_VERSION "1.5.4"

  if (!gcry_check_version(NEED_LIBGCRYPT_VERSION)) {
    die("libgcrypt is too old (need %s, have %s).", NEED_LIBGCRYPT_VERSION,
        gcry_check_version(NULL));
  }

  /* Disable secure memory.  */
  gcry_control(GCRYCTL_DISABLE_SECMEM, 0);

  /* ... If required, other initialization goes here.  */

  /* Tell Libgcrypt that initialization has completed. */
  gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

  debug(2, "libgcrypt initialised.");

#endif

  debug(2, "Log Verbosity is %d.", debug_level());

  config.output = audio_get_output(config.output_name);
  if (!config.output) {
    die("the audio backend selected: \"%s\" is not supported. Either it is invalid, or support has "
        "not been included in this build of Shairport Sync.",
        config.output_name == NULL ? "<unspecified>" : config.output_name);
  }
  debug(1, "audio backend is \"%s\".", config.output_name);
  config.output->init(argc - audio_arg, argv + audio_arg);

#ifdef CONFIG_FFMPEG
  if (debug_level() <= 1) // keep FFmpeg stuff quiet unless verbosity is 2 or more
    av_log_set_level(AV_LOG_QUIET);

#if LIBAVUTIL_VERSION_MAJOR >= 57

  // default multichannel on
  {
    AVChannelLayout default_layout =
        AV_CHANNEL_LAYOUT_7POINT1; // big fat macro to initialise the default layout
    config.eight_channel_layout = default_layout.u.mask;
  }
  {
    AVChannelLayout default_layout =
        AV_CHANNEL_LAYOUT_5POINT1; // big fat macro to initialise the default layout
    config.six_channel_layout = default_layout.u.mask;
  }

  const char *str;

  if ((config.cfg != NULL) &&
      (config_lookup_string(config.cfg, "general.eight_channel_mode", &str))) {
    if ((strcasecmp(str, "off") == 0) || (strcasecmp(str, "no") == 0)) {
      config.eight_channel_layout = 0; // 0 on initialisation
    } else if ((strcasecmp(str, "on") == 0) || (strcasecmp(str, "yes") == 0)) {
      // AVChannelLayout default_layout =
      //     AV_CHANNEL_LAYOUT_7POINT1; // big fat macro to initialise the default layout
      // config.eight_channel_layout = default_layout.u.mask;
    } else {
      AVChannelLayout channel_layout;
      if (av_channel_layout_from_string(&channel_layout, str) == 0) {
        if (channel_layout.nb_channels == 8) {
          config.eight_channel_layout = channel_layout.u.mask;
        } else {
          warn("the eight_channel_mode setting \"%s\" is a %u-channel layout. If a channel layout "
               "is "
               "given, it must be an 8-channel layout. eight_channel_mode is set to \"off\".",
               str, channel_layout.nb_channels);
        }
        av_channel_layout_uninit(&channel_layout);
      } else {
        warn("the eight_channel_mode setting \"%s\" is not recognised -- it should be \"off\" or "
             "\"on\" or an eight-channel FFmpeg channel layout, e.g. \"7.1\". "
             "eight_channel_mode is set to \"off\".",
             str);
      }
    }
  }

  if ((config.cfg != NULL) &&
      (config_lookup_string(config.cfg, "general.six_channel_mode", &str))) {
    if ((strcasecmp(str, "off") == 0) || (strcasecmp(str, "no") == 0)) {
      config.six_channel_layout = 0; // 0 on initialisation
    } else if ((strcasecmp(str, "on") == 0) || (strcasecmp(str, "yes") == 0)) {
      // AVChannelLayout default_layout =
      //     AV_CHANNEL_LAYOUT_5POINT1; // big fat macro to initialise the default layout
      // config.six_channel_layout = default_layout.u.mask;
    } else {
      AVChannelLayout channel_layout;
      if (av_channel_layout_from_string(&channel_layout, str) == 0) {
        if (channel_layout.nb_channels == 6) {
          config.six_channel_layout = channel_layout.u.mask;
        } else {
          warn("the six_channel_mode setting \"%s\" is a %u-channel layout. If a channel layout is "
               "given, it must be a 6-channel layout. six_channel_mode is set to \"off\".",
               str, channel_layout.nb_channels);
        }
        av_channel_layout_uninit(&channel_layout);
      } else {
        warn("the six_channel_mode setting \"%s\" is not recognised -- it should be \"off\" or "
             "\"on\" or a six-channel FFmpeg channel layout, e.g. \"5.1\". "
             "six_channel_mode is set to \"off\".",
             str);
      }
    }
  }

  if ((config.cfg != NULL) &&
      (config_lookup_non_empty_string(config.cfg, "general.mixdown", &str))) {
    if ((strcasecmp(str, "off") == 0) || (strcasecmp(str, "no") == 0)) {
      config.mixdown_enable = 0; // 0 on initialisation
      debug(1, "mixdown disabled.");
    } else if (strcasecmp(str, "auto") == 0) {
      config.mixdown_enable = 1;
      config.mixdown_channel_layout = 0; // 0 means auto
      debug(1, "mixdown target: auto.");
    } else {
      AVChannelLayout channel_layout;
      if (av_channel_layout_from_string(&channel_layout, str) == 0) {
        config.mixdown_enable = 1;
        config.mixdown_channel_layout = channel_layout.u.mask;
        av_channel_layout_uninit(&channel_layout);
        debug(1, "mixdown target: \"%s\".", str);
      } else {
        warn("the mixdown setting \"%s\" is not recognised -- it should be \"off\" or \"auto\" or "
             "an "
             "FFmpeg channel layout, e.g. \"stereo\". the mixdown is set to \"auto\".",
             str);
        config.mixdown_enable = 1;
        config.mixdown_channel_layout = 0; // 0 means auto
      }
    }
  }
#else

  // default on
  config.eight_channel_layout = AV_CH_LAYOUT_7POINT1;
  config.six_channel_layout = AV_CH_LAYOUT_5POINT1;

  const char *str;

  if ((config.cfg != NULL) &&
      (config_lookup_non_empty_string(config.cfg, "general.eight_channel_mode", &str))) {
    if ((strcasecmp(str, "off") == 0) || (strcasecmp(str, "no") == 0)) {
      config.eight_channel_layout = 0; // 0 on initialisation
    } else if ((strcasecmp(str, "on") == 0) || (strcasecmp(str, "yes") == 0)) {
      // config.eight_channel_layout = AV_CH_LAYOUT_7POINT1;
    } else if (av_get_channel_layout(str) != 0) {
      if (av_get_channel_layout_nb_channels(av_get_channel_layout(str)) == 8) {
        config.eight_channel_layout = av_get_channel_layout(str);
      } else {
        warn("the eight_channel_mode setting \"%s\" is a %u channel layout. If a channel layout is "
             "given, it must be an 8-channel layout. eight_channel_mode is set to \"off\".",
             str, av_get_channel_layout_nb_channels(av_get_channel_layout(str)));
      }
    } else {
      warn("the eight_channel_mode setting \"%s\" is not recognised -- it should be \"off\" or "
           "\"on\" or an 8-channel FFmpeg channel layout, e.g. \"7.1\". "
           "eight_channel_mode is set to \"off\".",
           str);
    }
  }

  if ((config.cfg != NULL) &&
      (config_lookup_non_empty_string(config.cfg, "general.six_channel_mode", &str))) {
    if ((strcasecmp(str, "off") == 0) || (strcasecmp(str, "no") == 0)) {
      config.six_channel_layout = 0; // 0 on initialisation
    } else if ((strcasecmp(str, "on") == 0) || (strcasecmp(str, "yes") == 0)) {
      // config.six_channel_layout = AV_CH_LAYOUT_5POINT1;
    } else if (av_get_channel_layout(str) != 0) {
      if (av_get_channel_layout_nb_channels(av_get_channel_layout(str)) == 6) {
        config.six_channel_layout = av_get_channel_layout(str);
      } else {
        warn("the six_channel_mode setting \"%s\" is a %u channel layout. If a channel layout is "
             "given, it must be a 6-channel layout. six_channel_mode is set to \"off\".",
             str, av_get_channel_layout_nb_channels(av_get_channel_layout(str)));
      }
    } else {
      warn("the six_channel_mode setting \"%s\" is not recognised -- it should be \"off\" or "
           "\"on\" or a 6-channel FFmpeg channel layout, e.g. \"5.1\". "
           "six_channel_mode is set to \"off\".",
           str);
    }
  }

  if ((config.cfg != NULL) &&
      (config_lookup_non_empty_string(config.cfg, "general.mixdown", &str))) {
    if ((strcasecmp(str, "off") == 0) || (strcasecmp(str, "no") == 0)) {
      config.mixdown_enable = 0; // 0 on initialisation
    } else if (strcasecmp(str, "auto") == 0) {
      config.mixdown_enable = 1;
      config.mixdown_channel_layout = 0; // 0 means auto
    } else if (av_get_channel_layout(str) != 0) {
      config.mixdown_enable = 1;
      config.mixdown_channel_layout = av_get_channel_layout(str);
    } else {
      warn("the mixdown setting \"%s\" is not recognised -- it should be \"off\" or \"auto\" or an "
           "FFmpeg channel layout, e.g. \"stereo\". the mixdown is set to \"auto\".",
           str);
      config.mixdown_enable = 1;
      config.mixdown_channel_layout = 0; // 0 means auto
    }
  }
#endif

  if (config.cfg != NULL) {
    config_setting_t *output_channel_mapping_setting =
        config_lookup(config.cfg, "general.output_channel_mapping");
    if (output_channel_mapping_setting != NULL) {
      const char *sstr = config_setting_get_string(output_channel_mapping_setting);
      if (sstr != NULL) { // definitely a string
        if (strcasecmp(sstr, "auto") == 0) {
          config.output_channel_mapping_enable = 1; // this is the default anyway
          config.output_channel_map_size = 0;       // use the device's channel map
          debug(1, "device output channel map chosen");
        } else if ((strcasecmp(sstr, "off") == 0) || (strcasecmp(sstr, "no") == 0)) {
          config.output_channel_mapping_enable = 0; // no mapping
        } else {
          warn("the output_channel_mapping setting \"%s\" is not recognised -- it should be "
               "\"auto\", \"off\" or a "
               "bracketed comma-separated list of short channel names, e.g. (\"FL\", \"FR\", "
               "\"LFE\");",
               sstr);
        }
      } else {
        int i = 0;
        for (i = 0; i < config_setting_length(output_channel_mapping_setting); i++) {
          // is a list or array, so okay
          const char *channel_id =
              config_setting_get_string_elem(output_channel_mapping_setting, i);
          if (channel_id != NULL) { // definitely a string
            int found = 0;
            if (strcmp(channel_id, "--") == 0) {
              found = 1;
            } else {
#if LIBAVUTIL_VERSION_MAJOR >= 57
#define BUFFER_SIZE 32
              char buffer[BUFFER_SIZE];
              enum AVChannel channel_index;
              for (channel_index = AV_CHAN_NONE;
                   ((channel_index < AV_CHAN_BOTTOM_FRONT_RIGHT) && (found == 0));
                   channel_index++) {
                found = av_channel_name(buffer, BUFFER_SIZE, channel_index);
                if (found > 0) {
                  found = ((av_channel_name(buffer, BUFFER_SIZE, channel_index) > 0) &&
                           (strcmp(channel_id, buffer) == 0));
                } else {
                  found = 0;
                }
              }
#else
              uint64_t channel_index;
              for (channel_index = 0; ((channel_index < 64) && (found == 0)); channel_index++) {
                found = ((av_get_channel_name(1 << channel_index) != NULL) &&
                         (strcmp(channel_id, av_get_channel_name(1 << channel_index)) == 0));
              }
#endif
            }
            if (found != 0) {
              config.output_channel_map[i] = strdup(channel_id);
              debug(2, "output channel %d is \"%s\".", i, config.output_channel_map[i]);
            } else {

              warn("during channel mapping, \"%s\" was not recognised as a channel name -- as a "
                   "result, output channel %d will be silent.",
                   channel_id, i);
              config.output_channel_map[i] = strdup("--");
            }
            config.output_channel_map_size++;
          }
        }
        if (config.output_channel_map_size == 0)
          warn("the output_channel_mapping setting was empty. No output channel mapping will be "
               "done.");
        else
          config.output_channel_mapping_enable = 1;
      }
    }
  }

#endif

  switch (config.endianness) {
  case SS_LITTLE_ENDIAN:
    debug(2, "The processor is running little-endian.");
    break;
  case SS_BIG_ENDIAN:
    debug(2, "The processor is running big-endian.");
    break;
  case SS_PDP_ENDIAN:
    debug(2, "The processor is running pdp-endian.");
    break;
  }

  /* Mess around with the latency options */
  // Basically, we expect the source to set the latency and add a fixed offset of 11025 frames to
  // it, which sounds right
  // If this latency is outside the max and min latensies that may be set by the source, clamp it to
  // fit.

  // If they specify a non-standard latency, we suggest the user to use the
  // audio_backend_latency_offset instead.

  if (config.userSuppliedLatency) {
    inform("The fixed latency setting is deprecated, as Shairport Sync gets the correct "
           "latency automatically from the source.");
    inform("Use the audio_backend_latency_offset_in_seconds setting "
           "instead to compensate for timing issues.");
    if ((config.userSuppliedLatency != 0) &&
        ((config.userSuppliedLatency < 4410) ||
         (config.userSuppliedLatency > BUFFER_FRAMES * 352 - 22050)))
      die("An out-of-range fixed latency has been specified. It must be between 4410 and %d (at "
          "44100 frames per second).",
          BUFFER_FRAMES * 352 - 22050);
  }
  const int option_print_level = 1;
  /* Print out options */
  debug(option_print_level, "disable_resend_requests is %s.",
        config.disable_resend_requests ? "on" : "off");
  debug(option_print_level,
        "diagnostic_drop_packet_fraction is %f. A value of 0.0 means no packets will be dropped "
        "deliberately.",
        config.diagnostic_drop_packet_fraction);
  debug(option_print_level, "statistics_requester status is %d.", config.statistics_requested);
#if CONFIG_LIBDAEMON
  debug(option_print_level, "daemon status is %d.", config.daemonise);
  debug(option_print_level, "daemon pid file path is \"%s\".", pid_file_proc());
#endif
  debug(option_print_level, "rtsp listening port is %d.", config.port);
  debug(option_print_level, "udp base port is %d.", config.udp_port_base);
  debug(option_print_level, "udp port range is %d.", config.udp_port_range);
  debug(option_print_level, "player name is \"%s\".", config.service_name);
  debug(option_print_level, "run_this_before_play_begins action is \"%s\".",
        strnull(config.cmd_start));
  debug(option_print_level, "run_this_after_play_ends action is \"%s\".", strnull(config.cmd_stop));
  debug(option_print_level, "wait-cmd status is %d.", config.cmd_blocking);
  debug(option_print_level, "run_this_before_play_begins may return output is %d.",
        config.cmd_start_returns_output);
  debug(option_print_level, "run_this_if_an_unfixable_error_is_detected action is \"%s\".",
        strnull(config.cmd_unfixable));
  debug(option_print_level, "run_this_before_entering_active_state action is  \"%s\".",
        strnull(config.cmd_active_start));
  debug(option_print_level, "run_this_after_exiting_active_state action is  \"%s\".",
        strnull(config.cmd_active_stop));
  debug(option_print_level, "active_state_timeout is  %f seconds.", config.active_state_timeout);
  debug(option_print_level, "mdns backend \"%s\".", strnull(config.mdns_name));
  debug(2, "userSuppliedLatency is %d.", config.userSuppliedLatency);
  debug(option_print_level, "interpolation setting is \"%s\".",
        config.packet_stuffing == ST_basic     ? "basic"
        : config.packet_stuffing == ST_vernier ? "vernier"
        : config.packet_stuffing == ST_soxr    ? "soxr"
                                               : "auto");
  debug(option_print_level, "interpolation soxr_delay_threshold is %d.",
        config.soxr_delay_threshold);
  debug(option_print_level, "resync time is %f seconds.", config.resync_threshold);
  debug(option_print_level, "allow a classic AirPlay session to be interrupted: \"%s\".",
        config.allow_session_interruption == 0 ? "no" : "yes");
  debug(option_print_level, "busy timeout time is %d.", config.timeout);
  debug(option_print_level, "drift tolerance is %f seconds.", config.tolerance);
  debug(option_print_level, "password is \"%s\".", strnull(config.password));
  debug(option_print_level, "default airplay volume is: %.6f.", config.default_airplay_volume);
  debug(option_print_level, "ignore_volume_control is %d.", config.ignore_volume_control);
  if (config.volume_max_db_set)
    debug(option_print_level, "volume_max_db is %d.", config.volume_max_db);
  else
    debug(option_print_level, "volume_max_db is not set");
  debug(option_print_level,
        "volume range in dB (zero means use the range specified by the mixer): %u.",
        config.volume_range_db);
  debug(option_print_level,
        "volume_range_combined_hardware_priority (1 means hardware mixer attenuation is used "
        "first) is %d.",
        config.volume_range_hw_priority);
  debug(option_print_level,
        "playback_mode is %d (0-stereo, 1-mono, 1-reverse_stereo, 2-both_left, 3-both_right).",
        config.playback_mode);
  debug(option_print_level, "disable_synchronization is %d.", config.no_sync);
  debug(option_print_level, "use_mmap_if_available is %d.", config.no_mmap ? 0 : 1);
  debug(option_print_level, "output_format automatic selection is %sabled.",
        config.output_format_auto_requested ? "en" : "dis");
  // if (config.output_format_auto_requested == 0)
  //   debug(option_print_level, "output_format is \"%s\".",
  //         sps_format_description_string(config.current_output_configuration->format));
  debug(option_print_level, "output_rate automatic selection is %sabled.",
        config.output_rate_auto_requested ? "en" : "dis");
  // if (config.output_rate_auto_requested == 0)
  //   debug(option_print_level, "output_rate is %d.", config.current_output_configuration->rate);
  debug(option_print_level, "audio backend desired buffer length is %f seconds.",
        config.audio_backend_buffer_desired_length);
  debug(option_print_level,
        "audio_backend_buffer_interpolation_threshold_in_seconds is %f seconds.",
        config.audio_backend_buffer_interpolation_threshold_in_seconds);
  debug(option_print_level, "audio backend latency offset is %f seconds.",
        config.audio_backend_latency_offset);
  if (config.audio_backend_silent_lead_in_time_auto == 1)
    debug(option_print_level, "audio backend silence lead-in time is \"auto\".");
  else
    debug(option_print_level, "audio backend silence lead-in time is %f seconds.",
          config.audio_backend_silent_lead_in_time);
  debug(option_print_level, "zeroconf regtype is \"%s\".", config.regtype);
  debug(option_print_level,
        "decoders_supported bit field is %d (1 == hammerton, 2 == apple, 4 == ffmpeg).",
        config.decoders_supported);
  debug(option_print_level, "decoder_in_use is %d.", config.decoder_in_use);
  debug(option_print_level, "alsa_use_hardware_mute is %d.", config.alsa_use_hardware_mute);
  if (config.interface)
    debug(option_print_level, "mdns service interface \"%s\" requested.", config.interface);
  else
    debug(option_print_level, "no special mdns service interface was requested.");
  char *realConfigPath = realpath(config.configfile, NULL);
  if (realConfigPath) {
    debug(option_print_level, "configuration file name \"%s\" resolves to \"%s\".",
          config.configfile, realConfigPath);
    free(realConfigPath);
  } else {
    debug(option_print_level, "configuration file name \"%s\" can not be resolved.",
          config.configfile);
  }
#ifdef CONFIG_METADATA
  debug(option_print_level, "metadata enabled is %d.", config.metadata_enabled);
  if (config.metadata_enabled != 0) {
    debug(option_print_level, "metadata pipename is \"%s\".", config.metadata_pipename);
    debug(option_print_level, "metadata socket address is \"%s\" port %d.", config.metadata_sockaddr,
          config.metadata_sockport);
    debug(option_print_level, "metadata socket packet size is \"%zd\".",
          config.metadata_sockmsglength);
    debug(option_print_level, "get-coverart is %d.", config.get_coverart);
  }
#endif
#ifdef CONFIG_MQTT
  debug(option_print_level, "mqtt is %sabled.", config.mqtt_enabled ? "en" : "dis");
  if (config.mqtt_enabled != 0) {  
    debug(option_print_level, "mqtt hostname is %s, port is %d.", config.mqtt_hostname,
          config.mqtt_port);
    debug(option_print_level, "mqtt topic is %s.", config.mqtt_topic);
    debug(option_print_level, "mqtt will%s publish raw metadata.",
          config.mqtt_publish_raw ? "" : " not");
    debug(option_print_level, "mqtt will%s publish parsed metadata.",
          config.mqtt_publish_parsed ? "" : " not");
    debug(option_print_level, "mqtt will%s publish cover Art.",
          config.mqtt_publish_cover ? "" : " not");
    debug(option_print_level, "mqtt will%s set retain flag.",
          config.mqtt_publish_retain ? "" : " not");
    debug(option_print_level, "mqtt remote control is %sabled.",
          config.mqtt_enable_remote ? "en" : "dis");
    debug(option_print_level, "mqtt autodiscovery is %sabled.",
          config.mqtt_enable_autodiscovery ? "en" : "dis");
  }
#endif

#ifdef CONFIG_CONVOLUTION
  debug(option_print_level, "convolution_enabled is %s.",
        config.convolution_enabled != 0 ? "true" : "false");
  if (config.convolution_enabled != 0) {    
    debug(option_print_level, "convolution maximum length is %f seconds.",
          config.convolution_max_length_in_seconds);
    debug(option_print_level, "convolution gain is %f", config.convolution_gain);
    int convolution_ir_files_status = sanity_check_ir_files(
        option_print_level, config.convolution_ir_files, config.convolution_ir_file_count);
    if (convolution_ir_files_status != 0) { // if non zero, it's the index of the errant file + 1
      debug(option_print_level, "convolution impulse response file \"%s\" %s",
            config.convolution_ir_files[convolution_ir_files_status - 1].filename, sf_strerror(NULL));
      warn("Error accessing the convolution impulse response file \"%s\". %s",
           config.convolution_ir_files[convolution_ir_files_status - 1].filename, sf_strerror(NULL));
    }
  }
#endif
  debug(option_print_level, "loudness_enabled is %s.",
        config.loudness_enabled != 0 ? "true" : "false");
  if (config.loudness_enabled != 0) {    
    debug(option_print_level, "loudness reference level is %f", config.loudness_reference_volume_db);
  }

#ifdef CONFIG_SOXR

  {
    int oldState;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState); // make this un-cancellable
    soxr_time_check_thread = malloc(sizeof(pthread_t));
    if (soxr_time_check_thread != NULL) {
      named_pthread_create(soxr_time_check_thread, NULL, &soxr_time_check, NULL, "soxr_checker");
    } else {
      debug(1, "couldn't get memory to start the soxr_checker");
    }
    pthread_setcancelstate(oldState, NULL); // make this un-cancellable
  }

#endif

#ifdef CONFIG_FFMPEG
  debug(2, "LIBAVUTIL_VERSION_MAJOR is %d", LIBAVUTIL_VERSION_MAJOR);

#if LIBAVUTIL_VERSION_MAJOR >= 57
  unsigned int cc;
  for (cc = 1; cc <= 8; cc++) {
    AVChannelLayout output_channel_layout;
    av_channel_layout_default(&output_channel_layout, cc);
    char chLayoutDescription[1024];
    int sts = av_channel_layout_describe(&output_channel_layout, chLayoutDescription,
                                         sizeof(chLayoutDescription));
    if (sts >= 0) {
      int idx;
      char channel_map[1024];
      channel_map[0] = '\0';
      for (idx = 0; idx < output_channel_layout.nb_channels; idx++) {
        enum AVChannel av = av_channel_layout_channel_from_index(&output_channel_layout, idx);
        char chName[64];
        int cts = av_channel_name(chName, sizeof(chName), av);
        if (cts >= 0) {
          if (idx != 0)
            strncat(channel_map, " ", sizeof(channel_map) - 1);
          strncat(channel_map, chName, sizeof(channel_map) - 1 - strlen(channel_map));
          debug(4, "Channel %d: \"%s\"", idx, chName);
        } else {
          debug(1, "Insufficient space for the name of channel %d.", idx);
        }
      }
      if (cc == 1) {
        debug(2, "Default layout for one channel: \"%s\", channel map: \"%s\".",
              chLayoutDescription, channel_map);
      } else {
        debug(2, "Default layout for %u channels: \"%s\", channel map: \"%s\".", cc,
              chLayoutDescription, channel_map);
      }
    } else {
      debug(1, "Insufficient space for the description of the default layout for %u channels.", cc);
    }
    av_channel_layout_uninit(&output_channel_layout);
  }
#endif

#endif

  // In AirPlay 2 mode, the AP1 prefix is the same as the device ID less the colons
  // and has already been calculated.

  // In AirPlay 1 mode, the AP1 prefix is calculated by hashing the service name.

#ifdef CONFIG_AIRPLAY_2
  if (config.service_type != APST_airplay2) {
#endif
    uint8_t ap_md5[16];
    // debug(1, "size of hw_addr is %u.", sizeof(config.hw_addr));
#ifdef CONFIG_OPENSSL
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_md5(), NULL);
    EVP_DigestUpdate(mdctx, config.service_name, strlen(config.service_name));
    EVP_DigestUpdate(mdctx, config.hw_addr, sizeof(config.hw_addr));
    unsigned int md5_digest_len = EVP_MD_size(EVP_md5());
    EVP_DigestFinal_ex(mdctx, ap_md5, &md5_digest_len);
    EVP_MD_CTX_free(mdctx);
#endif

#ifdef CONFIG_MBEDTLS
#if MBEDTLS_VERSION_MAJOR >= 4
    psa_hash_operation_t tctx = PSA_HASH_OPERATION_INIT;
    size_t ap_md5_len;
    psa_hash_setup(&tctx, PSA_ALG_MD5);
    psa_hash_update(&tctx, (const uint8_t *)config.service_name, strlen(config.service_name));
    psa_hash_update(&tctx, (const uint8_t *)config.hw_addr, sizeof(config.hw_addr));
    psa_hash_finish(&tctx, ap_md5, sizeof(ap_md5), &ap_md5_len);
#elif MBEDTLS_VERSION_MINOR >= 7
    mbedtls_md5_context tctx;
    mbedtls_md5_init(&tctx);
    mbedtls_md5_starts_ret(&tctx);
    mbedtls_md5_update_ret(&tctx, (unsigned char *)config.service_name,
                           strlen(config.service_name));
    mbedtls_md5_update_ret(&tctx, (unsigned char *)config.hw_addr, sizeof(config.hw_addr));
    mbedtls_md5_finish_ret(&tctx, ap_md5);
    mbedtls_md5_free(&tctx);
#else
    mbedtls_md5_context tctx;
    mbedtls_md5_init(&tctx);
    mbedtls_md5_starts(&tctx);
    mbedtls_md5_update(&tctx, (unsigned char *)config.service_name, strlen(config.service_name));
    mbedtls_md5_update(&tctx, (unsigned char *)config.hw_addr, sizeof(config.hw_addr));
    mbedtls_md5_finish(&tctx, ap_md5);
    mbedtls_md5_free(&tctx);
#endif
#endif

#ifdef CONFIG_POLARSSL
    md5_context tctx;
    md5_starts(&tctx);
    md5_update(&tctx, (unsigned char *)config.service_name, strlen(config.service_name));
    md5_update(&tctx, (unsigned char *)config.hw_addr, sizeof(config.hw_addr));
    md5_finish(&tctx, ap_md5);
#endif
    memcpy(config.ap1_prefix, ap_md5, sizeof(config.ap1_prefix));

#ifdef CONFIG_AIRPLAY_2
  }
#endif

#ifdef CONFIG_METADATA
  metadata_init(); // create the metadata pipe if necessary
#endif

#ifdef CONFIG_METADATA_HUB
  // debug(1, "Initialising metadata hub");
  metadata_hub_init();
#endif

#ifdef CONFIG_DACP_CLIENT
  // debug(1, "Requesting DACP Monitor");
  dacp_monitor_start();
#endif

#ifdef CONFIG_MQTT
  if (config.mqtt_enabled) {
    initialise_mqtt();
  }
#endif

#if defined(CONFIG_DBUS_INTERFACE) || defined(CONFIG_MPRIS_INTERFACE)
  glib_worker_thread = g_thread_new("glib worker", glib_worker_thread_function, NULL);
#endif

#ifdef CONFIG_METADATA
  send_ssnc_metadata('svna', config.service_name, strlen(config.service_name), 1);
#endif

#ifdef CONFIG_CONVOLUTION
  convolver_pool_init(config.convolution_threads, 8); // 8 channels
#endif
  activity_monitor_start();
  debug(4, "create an RTSP listener");
  // note: the Avahi Threaded Poll thread will be named after whatever name you use here too, so
  // you'll see two threads named "listener" or whatever...
  named_pthread_create(&rtsp_listener_thread, NULL, &rtsp_listen_loop, NULL, "listener");
  atexit(exit_rtsp_listener);

  // wait forever...
  while (1) {
    usleep(1000000);
  }
  return 0;
}
