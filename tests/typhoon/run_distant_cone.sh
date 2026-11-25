#!/bin/bash
# DistantLight cone vs Typhoon (distant_cone.usda, 12 frames). See run_frames.sh;
# cone_stats.py $OUT splits the sun seen in the mirror from the rest.
exec "$(dirname "$0")/run_frames.sh" "$(dirname "$0")/distant_cone.usda" "${FRAMES:-12}"
