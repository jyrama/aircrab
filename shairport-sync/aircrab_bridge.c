#include <stdlib.h>
#include <string.h>

#include "aircrab_bridge.h"
#include "common.h"
#include "utilities/debug.h"

extern int killOption;
extern int display_config_selected;
extern void print_version(void);
extern void usage(char *progname);

void aircrab_set_configfile(const char *s) { config.configfile = strdup(s); }

void aircrab_set_port(int p) {
  if (p < 0 || p > 65535)
    die("Invalid port number \"%d\". It should be between 0 and 65535.", p);
  config.port = p;
}

void aircrab_set_name(const char *s) { config.service_name = strdup(s); }

void aircrab_set_output(const char *s) { config.output_name = strdup(s); }

void aircrab_add_verbose(void) { increase_debug_level(); }

void aircrab_set_display_config(void) { display_config_selected = 1; }

void aircrab_set_kill(void) { killOption = 1; }
