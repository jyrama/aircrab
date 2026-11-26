#ifndef AIRCRAB_BRIDGE_H
#define AIRCRAB_BRIDGE_H

/* Seam between the C tree and the Rust (clap) CLI parser in rust/, linked in as
   libaircrab_cli.a (see Makefile.am). Returns an exit code and sets *should_exit
   when main() must exit immediately (help/version/parse error). */
int aircrab_parse_options(int argc, char **argv, int *should_exit);

#endif
