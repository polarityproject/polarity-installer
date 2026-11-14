#pragma once
// The installers' version (installer.rc reads it too) and the commit they
// were built from (the build scripts pass it; "unknown" otherwise). Both go
// at the top of every log, to match a log with its build's symbols.

#define POLARITY_INSTALLER_VERSION "1.0.0"
#define POLARITY_INSTALLER_VERSION_NUMBERS 1, 0, 0, 0

#ifndef POLARITY_INSTALLER_COMMIT
#define POLARITY_INSTALLER_COMMIT "unknown"
#endif
