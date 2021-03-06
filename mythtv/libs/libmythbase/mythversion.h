#ifndef MYTHVERSION_H_
#define MYTHVERSION_H_

#include "qglobal.h"
#include "mythbaseexp.h"
#include "version.h"

#if ( QT_VERSION < 0x040600 )
#error You need Qt version >= 4.6.0 to compile MythTV.
#endif

/// Update this whenever the plug-in API changes.
/// Including changes in the libmythbase, libmyth, libmythtv, libmythav* and
/// libmythui class methods used by plug-ins.
#define MYTH_BINARY_VERSION "0.25.20110607-2"

/** \brief Increment this whenever the MythTV network protocol changes.
 *
 *   You must also update this value and any corresponding changes to the
 *   ProgramInfo network protocol layout in the following files:
 *
 *   MythWeb
 *       mythweb/modules/tv/classes/Program.php (layout)
 *
 *   MythTV Perl Bindings
 *       mythtv/bindings/perl/MythTV.pm (version number)
 *       mythtv/bindings/perl/MythTV/Program.pm (layout)
 *
 *   MythTV PHP Bindings
 *       mythtv/bindings/php/MythBackend.php (version number)
 *       mythtv/bindings/php/MythTVProgram.php (layout)
 *       mythtv/bindings/php/MythTVRecording.php (layout)
 *
 *   MythTV Python Bindings
 *       mythtv/bindings/python/MythTV/static.py (version number)
 *       mythtv/bindings/python/MythTV/mythproto.py (layout)
 */
#define MYTH_PROTO_VERSION "66"
#define MYTH_PROTO_TOKEN "0C0FFEE0"

 MBASE_PUBLIC  const char *GetMythSourceVersion();

#endif

