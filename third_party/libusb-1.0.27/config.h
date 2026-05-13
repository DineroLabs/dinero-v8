/* Minimal config.h for libusb 1.0.27 - macOS */
#ifndef LIBUSB_CONFIG_H
#define LIBUSB_CONFIG_H

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* Define to 1 if you have the `gettimeofday' function. */
#define HAVE_GETTIMEOFDAY 1

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if the system has the type `struct timespec'. */
#define HAVE_STRUCT_TIMESPEC 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/time.h> header file. */
#define HAVE_SYS_TIME_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define to the sub-directory where libtool stores uninstalled libraries. */
#define LT_OBJDIR ".libs/"

/* Name of package */
#define PACKAGE "libusb-1.0"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "libusb-devel@lists.sourceforge.net"

/* Define to the full name of this package. */
#define PACKAGE_NAME "libusb-1.0"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "libusb-1.0 1.0.27"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "libusb-1.0"

/* Define to the home page for this package. */
#define PACKAGE_URL "http://libusb.info"

/* Define to the version of this package. */
#define PACKAGE_VERSION "1.0.27"

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Define to 1 to output logging messages to the systemwide log. */
/* #undef USE_SYSTEM_LOGGING_FACILITY */

/* Version number of package */
#define VERSION "1.0.27"

/* Enable GNU extensions. */
#define _GNU_SOURCE 1

/* Define to the oldest supported version of macOS */
#define PLATFORM_OSX 1

/* Define to 1 if compiling for macOS */
#if defined(__APPLE__)
#define OS_DARWIN 1
#endif

/* Use poll() for event handling */
#define POLL_NFDS_TYPE nfds_t

/* Enable thread safety */
#define ENABLE_LOGGING 1
#define ENABLE_DEBUG_LOGGING 1

#endif /* LIBUSB_CONFIG_H */
