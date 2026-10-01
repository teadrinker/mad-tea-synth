#  ==============================================================================
#
#  This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.
#
#  See LICENSE.txt for more info.
#
#  ==============================================================================

# Script-mode helper invoked via `cmake -P` from the deploy POST_BUILD step.
# Copies SOURCE_PATH to DEST_PATH, tolerating a destination that's locked by
# another process (e.g. a DAW with the plugin currently loaded) by emitting a
# warning instead of failing the whole build.
#
# Expected -D args: SOURCE_PATH, DEST_DIR, DEST_PATH, IS_BUNDLE

message(STATUS "[iPlug2] Copying plugin: ${DEST_PATH}")

file(MAKE_DIRECTORY "${DEST_DIR}")

if(IS_BUNDLE)
  file(REMOVE_RECURSE "${DEST_PATH}")
  file(COPY "${SOURCE_PATH}/" DESTINATION "${DEST_PATH}")
else()
  file(REMOVE "${DEST_PATH}")
  file(COPY "${SOURCE_PATH}" DESTINATION "${DEST_DIR}")
endif()

if(NOT EXISTS "${DEST_PATH}")
  message(WARNING "[iPlug2] Could not deploy to ${DEST_PATH} (destination may be locked by another process, e.g. a DAW with the plugin loaded). Skipping deploy for this build.")
endif()
