#!/bin/bash
# Light and shadow linking vs Typhoon (linking.usda, 5 frames). See run_frames.sh.
# Typhoon doesn't register HdsiLightLinkingSceneIndex, so through scene-index
# emulation it gets no collections (empty links match everything); USD's
# legacy UsdImagingDelegate resolves the categories itself.
TY_ENV=${TY_ENV:-USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX=0} \
  exec "$(dirname "$0")/run_frames.sh" "$(dirname "$0")/linking.usda" 5
