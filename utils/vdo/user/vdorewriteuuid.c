/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id: $
 */

#include <err.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include <stdio.h>

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"

#include "constants.h"
#include "types.h"
#include "statusCodes.h"
#include "vdoInternal.h"
#include "volumeGeometry.h"

#include "vdoVolumeUtils.h"

static const char usageString[] = "[--help] vdoBacking";

static const char helpString[] =
  "vdorewriteuuid - generates a new uuid for the vdo volume stored on a backing\n"
  "                store.\n"
  "\n"
  "SYNOPSIS\n"
  "  vdorewriteuuid <vdoBacking>\n"
  "\n"
  "DESCRIPTION\n"
  "  vdorewriteuuid generates a new uuid for a VDO volume, whether or not\n"
  "  the VDO is running.\n"
  "\n";

static struct option options[] = {
  { "help",            no_argument,       NULL, 'h' },
  { NULL,              0,                 NULL,  0  },
};

/**
 * Explain how this command-line tool is used.
 *
 * @param progname           Name of this program
 * @param usageOptionString  Multi-line explanation
 **/
static void usage(const char *progname)
{
  errx(1, "Usage: %s %s\n", progname, usageString);
}

/**
 * Parse the arguments passed; print command usage if arguments are wrong.
 *
 * @param argc  Number of input arguments
 * @param argv  Array of input arguments
 *
 * @return The backing store of the VDO
 **/
static const char *processArgs(int argc, char *argv[])
{
  int   c;
  char *optionString = "h";
  while ((c = getopt_long(argc, argv, optionString, options, NULL)) != -1) {
    switch (c) {
    case 'h':
      printf("%s", helpString);
      exit(0);

    default:
      usage(argv[0]);
      break;
    }
  }

  // Explain usage and exit
  if (optind != (argc - 1)) {
    usage(argv[0]);
  }

  return argv[optind++];
}

/**********************************************************************/
int main(int argc, char *argv[])
{
  STATIC_ASSERT(sizeof(uuid_t) == sizeof(UUID));

  static char errBuf[ERRBUF_SIZE];

  int result = registerStatusCodes();
  if (result != VDO_SUCCESS) {
    errx(1, "Could not register status codes: %s",
	 stringError(result, errBuf, ERRBUF_SIZE));
  }

  const char *vdoBacking = processArgs(argc, argv);

  openLogger();

  VDO *vdo;
  result = makeVDOFromFile(vdoBacking, false, &vdo);
  if (result != VDO_SUCCESS) {
    errx(1, "Could not load VDO from '%s'", vdoBacking);
  }

  VolumeGeometry geometry;
  result = loadVolumeGeometry(vdo->layer, &geometry);
  if (result != VDO_SUCCESS) {
    freeVDOFromFile(&vdo);
    errx(1, "Could not load the geometry from '%s'", vdoBacking);
  }

  // Generate a UUID.
  uuid_t uuid;
  uuid_generate(uuid);

  memcpy(geometry.uuid, uuid, sizeof(UUID));

  result = writeVolumeGeometry(vdo->layer, &geometry);
  if (result != VDO_SUCCESS) {
    freeVDOFromFile(&vdo);
    errx(1, "Could not write the geometry to '%s' %s", vdoBacking,
	 stringError(result, errBuf, ERRBUF_SIZE));
  }

  freeVDOFromFile(&vdo);

  exit(0);
}
